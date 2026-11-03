"""Two-camera Raspberry Pi preview + paired image capture.

Top camera is reserved for YOLO and side camera for OpenCV inspection.
This module does NOT invent OK/NG results: integration points are marked below.
"""
import os
import json
import queue
import socket
import re
import threading
import time
from datetime import datetime
from pathlib import Path

import cv2
from flask import Flask, Response, jsonify, request, send_file
from tire_line_controller import TireLineController
from inspection_outbox import InspectionOutbox

TOP_DEVICE = os.getenv('TOP_CAMERA', '/dev/video0')
SIDE_DEVICE = os.getenv('SIDE_CAMERA', '/dev/video2')
HOST = os.getenv('CAMERA_HOST', '0.0.0.0')
PORT = int(os.getenv('CAMERA_PORT', '5001'))
OUTPUT = Path(os.getenv('INSPECTION_DIR', './inspection_images')).resolve()

app = Flask(__name__)
CONTROL_TOKEN = os.getenv('PI_CONTROL_TOKEN', '').strip()
CENTRAL_HOST = os.getenv('CENTRAL_HOST', '192.168.0.104')
CENTRAL_PORT = int(os.getenv('CENTRAL_PORT', '5000'))
events = queue.Queue(maxsize=200)
controller = None
outbox = None
control_busy = threading.Lock()

def publish_event(kind, payload):
    if kind == 'INSPECTION':
        if outbox is None:
            raise RuntimeError('실검사 전송 보관함이 초기화되지 않았습니다.')
        return outbox.enqueue(payload)
    packet={'type':kind, **payload}
    try: events.put_nowait(packet)
    except queue.Full: print('[SERVER EVENT QUEUE FULL]', packet, flush=True)

def publish_worker():
    # EQUIPMENT broadcasts are received by both MFC and WinForms through TCP server.
    while True:
        packet=events.get()
        try:
            with socket.create_connection((CENTRAL_HOST,CENTRAL_PORT),timeout=2) as conn:
                conn.sendall((json.dumps(packet, ensure_ascii=False)+'\n').encode('utf-8'))
        except OSError as exc:
            print('[EVENT SEND ERROR]',str(exc),flush=True)

threading.Thread(target=publish_worker,daemon=True).start()

def heartbeat_worker():
    cycles=0
    while True:
        publish_event('HEARTBEAT', {'equipment':'RASPBERRY_PI','status':'ALIVE'})
        cycles+=1
        if cycles%5==0 and controller is not None:
            st=controller.status()
            if st['state'] in ('RECHECK','ESTOP'):
                publish_event('EQUIPMENT', {
                    'equipment':'ALERT', 'status':st['state'],
                    'message':st['message']})
        time.sleep(2)

threading.Thread(target=heartbeat_worker,daemon=True).start()


class Camera:
    def __init__(self, device, width=640, height=480):
        self.width = width
        self.height = height
        self.device = device
        self.lock = threading.Lock()
        self.frame = None
        self.error = '카메라 연결 대기'
        self.running = True
        threading.Thread(target=self.run, daemon=True).start()

    def run(self):
        while self.running:
            cap = cv2.VideoCapture(int(self.device) if str(self.device).isdigit() else self.device, cv2.CAP_V4L2)
            if not cap.isOpened():
                with self.lock: self.error = f'카메라 열기 실패: {self.device}'
                cap.release()
                time.sleep(2)
                continue
            cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'MJPG'))
            cap.set(cv2.CAP_PROP_FRAME_WIDTH, self.width)
            cap.set(cv2.CAP_PROP_FRAME_HEIGHT, self.height)
            while self.running:
                ok, frame = cap.read()
                if not ok:
                    with self.lock: self.error = '카메라 프레임 수신 실패'
                    break
                with self.lock:
                    self.frame = frame.copy()
                    self.error = None
                time.sleep(0.025)
            cap.release()
            time.sleep(1)

    def get_frame(self):
        with self.lock:
            return None if self.frame is None else self.frame.copy()

cameras = {'top': Camera(TOP_DEVICE, 640, 480), 'side': Camera(SIDE_DEVICE, 1920, 1080)}

@app.get('/api/state')
def api_state():
    if controller is None: return jsonify(error='hardware initialization failed'), 503
    result = controller.status()
    result['inspection_pending_uploads'] = outbox.pending_count() if outbox else -1
    return jsonify(result)

@app.post('/api/control')
def api_control():
    if not CONTROL_TOKEN or request.headers.get('X-Line-Token') != CONTROL_TOKEN:
        return jsonify(error='제어 토큰이 없거나 일치하지 않습니다.'), 403
    if controller is None:return jsonify(error='hardware not ready'), 503
    body = request.get_json(silent=True) or {}
    action = str(body.get('action', '')).strip().upper()

    allowed = {
        'START', 'STOP', 'ACK_RECHECK',
        'SERVO_HOME', 'SERVO_TEST', 'AUTO', 'MANUAL'
    }
    if action not in allowed:
        return jsonify(error='지원하지 않는 제어 명령'), 400

    # 일반 제어 명령은 중복 실행하지 않습니다.
    # STOP은 별도 접수하지만 물리적 비상정지 수단은 아닙니다.
    acquired = False
    if action != 'STOP':
        acquired = control_busy.acquire(blocking=False)
        if not acquired:
            return jsonify(error='다른 장비 제어 명령이 실행 중입니다.'), 409

    try:
        return jsonify(controller.control(action))
    except (ValueError, RuntimeError, OSError) as exc:
        return jsonify(error=str(exc)), 409
    finally:
        if acquired:
            control_busy.release()

@app.get('/health')
def health():
    return jsonify({name: {'ready': cam.get_frame() is not None, 'error': cam.error}
                    for name, cam in cameras.items()})

def draw_top_yolo(frame):
    if controller is None:
        return frame

    st = controller.status()
    boxes = st.get("yolo_boxes", [])

    # 오래된 박스는 표시하지 않음
    if st.get("yolo_boxes_age", 999) > 2.5:
        return frame

    h, w = frame.shape[:2]

    for b in boxes:
        label = str(b.get("label", "UNKNOWN"))
        conf = float(b.get("conf", 0.0))

        x1 = max(0, min(w - 1, int(b["x1"])))
        y1 = max(0, min(h - 1, int(b["y1"])))
        x2 = max(0, min(w - 1, int(b["x2"])))
        y2 = max(0, min(h - 1, int(b["y2"])))

        if x2 <= x1 or y2 <= y1:
            continue

        color = (
            (0, 210, 0) if label == "NORMAL"
            else (0, 0, 255)
        )

        cv2.rectangle(
            frame, (x1, y1), (x2, y2), color, 3
        )

        text = f"{label} {conf:.2f}"
        font = cv2.FONT_HERSHEY_SIMPLEX
        scale = 0.55
        thickness = 2

        (tw, th), baseline = cv2.getTextSize(
            text, font, scale, thickness
        )

        tx = min(x1, max(0, w - tw - 10))
        ty = max(th + 10, y1 - 8)

        cv2.rectangle(
            frame,
            (tx, ty - th - 7),
            (min(w - 1, tx + tw + 8), ty + baseline + 3),
            color, -1
        )
        cv2.putText(
            frame, text, (tx + 4, ty),
            font, scale, (255, 255, 255),
            thickness, cv2.LINE_AA
        )

    return frame


@app.get('/snapshot/<name>')
def snapshot(name):
    cam = cameras.get(name)
    if cam is None:
        return jsonify(error='unknown camera'), 404
    frame = cam.get_frame()
    if frame is None:
        return jsonify(error=cam.error or 'camera not ready'), 503
    if name == 'top':
        frame = draw_top_yolo(frame)
    # Resize HTTP preview only, not the full-resolution inspection frames.
    if frame.shape[1] > 960:
        frame=cv2.resize(frame,(960,540),interpolation=cv2.INTER_AREA)
    ok, jpeg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 78])
    if not ok:
        return jsonify(error='jpeg encoding failed'), 500
    return Response(jpeg.tobytes(), mimetype='image/jpeg', headers={'Cache-Control': 'no-store'})

@app.get('/stream/<name>')
def stream(name):
    if name not in cameras:
        return jsonify(error='unknown camera'), 404
    def frames():
        while True:
            frame = cameras[name].get_frame()
            if frame is not None:
                if name == 'top':
                    frame = draw_top_yolo(frame)
                ok, jpg = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, 75])
                if ok:
                    yield b'--frame\r\nContent-Type: image/jpeg\r\n\r\n' + jpg.tobytes() + b'\r\n'
            time.sleep(0.15)
    return Response(frames(), mimetype='multipart/x-mixed-replace; boundary=frame')


@app.get('/api/ng-image/<tire_id>/<camera>')
def get_ng_image(tire_id, camera):
    # 기존 Pi 제어 토큰으로 이미지 조회 보호
    if (
        not CONTROL_TOKEN
        or request.headers.get("X-Line-Token") != CONTROL_TOKEN
    ):
        return jsonify(error="unauthorized"), 403

    if not re.fullmatch(r"TIRE-[A-Za-z0-9_-]{1,64}", tire_id):
        return jsonify(error="invalid tire_id"), 400

    if camera not in ("yolo", "opencv"):
        return jsonify(error="invalid camera"), 400

    root = (
        Path(__file__).resolve().parent
        / "inspection_images"
    )

    if not root.is_dir():
        return jsonify(error="no inspection images"), 404

    # 날짜별로 저장된 해당 검사 ID의 사진만 검색
    matches = sorted(
        root.glob(f"????-??-??/{tire_id}/{camera}.jpg")
    )

    if not matches:
        return jsonify(error="image not found"), 404

    path = matches[-1].resolve()

    # 저장 루트 밖 파일 접근 차단
    if not path.is_relative_to(root.resolve()):
        return jsonify(error="invalid path"), 403

    return send_file(
        path,
        mimetype="image/jpeg",
        as_attachment=False,
        conditional=True
    )


@app.post('/capture')
def capture():
    payload = request.get_json(silent=True) or {}
    inspection_id = str(payload.get('inspection_id', '')).strip()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,64}', inspection_id):
        return jsonify(error='inspection_id required: letters, digits, - or _'), 400
    top = cameras['top'].get_frame()
    side = cameras['side'].get_frame()
    if top is None or side is None:
        return jsonify(error='both cameras must be ready'), 503
    today = datetime.now().strftime('%Y-%m-%d')
    folder = OUTPUT / today / inspection_id
    if folder.exists():
        return jsonify(error='inspection_id already exists'), 409
    folder.mkdir(parents=True, exist_ok=False)
    top_path = folder / 'top.jpg'
    side_path = folder / 'side.jpg'
    if not cv2.imwrite(str(top_path), top) or not cv2.imwrite(str(side_path), side):
        return jsonify(error='failed to save image'), 500
    return jsonify(inspection_id=inspection_id,
                   captured_at=datetime.now().isoformat(timespec='seconds'),
                   top_image=str(top_path), side_image=str(side_path),
                   top_result='PENDING', side_result='PENDING', final_result='PENDING')

if __name__ == '__main__':
    if not CONTROL_TOKEN:
        raise SystemExit('환경 변수 PI_CONTROL_TOKEN 설정 필요 (원격 장비 제어 보호)')
    try:
        outbox = InspectionOutbox(
            os.getenv('INSPECTION_OUTBOX_DB', str(Path(__file__).resolve().parent / 'inspection_outbox.sqlite3')),
            CENTRAL_HOST, CENTRAL_PORT, os.getenv('INSPECTION_INGEST_TOKEN', ''))
        outbox.start()
        controller = TireLineController(cameras, publish_event)
    except Exception as exc:
        raise SystemExit(f'하드웨어 초기화 실패: {exc}') from exc
    print(f'TOP_CAMERA={TOP_DEVICE}, SIDE_CAMERA={SIDE_DEVICE}, port={PORT}')
    try:
        app.run(host=HOST, port=PORT, threaded=True, debug=False)
    finally:
        if controller is not None: controller.close()
        if outbox is not None: outbox.close()
