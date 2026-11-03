"""Integrated tire-line controller.

Side OpenCV (belt moving) -> top YOLO (stop at center) -> final decision/eject
-> restart.  No product proximity sensor.  RECHECK holds belt and flashes RGB1 red.
Requires actual trained best_v3.pt and the side inspector reference images.
Use with dual_camera_server.py; ONLY this service owns the two USB cameras.
"""
from __future__ import annotations
import collections
import os
import threading
import time
import uuid
from pathlib import Path

import cv2
import numpy as np
from tire_line_hardware import TireLineHardware
from inspect_side_integrated import MultiInspector, resize_frame

BASE = Path(__file__).resolve().parent
MODEL = Path(os.environ.get('YOLO_MODEL', str(BASE / 'models/best_v3.pt')))
CENTER_MIN = float(os.environ.get('TOP_CENTER_MIN', '0.42'))
CENTER_MAX = float(os.environ.get('TOP_CENTER_MAX', '0.58'))
CONF = float(os.environ.get('YOLO_CONF', '0.70'))
SIDE_TTL = float(os.environ.get('SIDE_TO_TOP_MAX_SECONDS', '18'))
AUTO_EJECT_ENABLED = os.environ.get('AUTO_EJECT_ENABLED', '0') == '1' # enable only after physical alignment test

class TireLineController:
    def __init__(self, cameras, notify, arduino_port=None):
        self.cameras = cameras
        self.notify = notify
        self.lock = threading.RLock()
        self.shutdown = threading.Event()
        self.state = 'STOPPED'
        self.message = '시작 대기'
        self.last_result = 'NONE'
        self.product_id = ''
        self.mode = 'AUTO'
        self.queued_side = collections.deque(maxlen=6)
        self.side_active = False
        self.side_samples = []
        self.side_absent = 0
        self.top_occupied = False
        self.top_absent = 0
        self._last_status = None
        self._last_inspect = 0.0
        self.hardware = TireLineHardware(arduino_port or os.environ.get('ARDUINO_PORT', '/dev/ttyACM0'))
        self.hardware.set_status_handler(self._on_serial)
        self.model = None
        self.side_inspector = None
        self.model_error = None
        self.side_error = None
        self._load_models()
        # Separate loops: heavy CPU YOLO inference must not delay side-frame sampling.
        threading.Thread(target=self._side_loop, daemon=True).start()
        threading.Thread(target=self._top_loop, daemon=True).start()

    def _load_models(self):
        try:
            if not MODEL.is_file(): raise FileNotFoundError(str(MODEL))
            from ultralytics import YOLO
            self.model = YOLO(str(MODEL))
        except Exception as exc:
            self.model_error = str(exc)
            print('[YOLO NOT READY]', exc, flush=True)
        try:
            self.side_inspector = MultiInspector()
        except Exception as exc:
            self.side_error = str(exc)
            print('[SIDE NOT READY]', exc, flush=True)

    def _event(self, equipment, status, message):
        self.notify('EQUIPMENT', {'equipment': equipment, 'status': status, 'message': message})

    def _set_state(self, state, message):
        with self.lock:
            if self.state == state and self.message == message: return
            self.state, self.message = state, message
        self._event('CONVEYOR', 'RUNNING' if state=='RUNNING' else
                    ('RECHECK' if state=='RECHECK' else 'ERROR' if state=='ESTOP' else 'STOPPED'), message)

    def _on_serial(self, reply):
        if reply in ('ESTOP_ACTIVE', 'BLOCKED:ESTOP'):
            self.hardware.estop = True
            self._set_state('ESTOP', '비상정지 버튼 작동: 모터 정지, RGB2·부저 경고')
            self._event('LED','ESTOP','RGB2 빨간색 점멸')
            self._event('BUZZER','ESTOP','부저 경고')
            self._event('ALERT', 'ESTOP', '비상정지 버튼이 눌렸습니다. 현장 상태를 확인하세요.')
        elif reply == 'ESTOP_RELEASED':
            self._set_state('STOPPED', '비상정지 해제: 작업자 START 버튼으로 재가동')
            self._event('ALERT', 'CLEAR', '비상정지가 해제되었습니다. START 대기')
        elif reply == 'WATCHDOG_STOP':
            self._set_state('STOPPED', '통신 워치독 정지, START 재요청 필요')
            self._event('ALERT', 'WARN', '컨베이어 통신 워치독으로 정지')

    def status(self):
        with self.lock:
            return {'state': self.state, 'message': self.message, 'estop': self.hardware.estop,
                    'recheck': self.hardware.recheck, 'last_result': self.last_result,
                    'product_id': self.product_id, 'mode': self.mode,
                    'yolo_ready': self.model is not None,
                    'opencv_ready': self.side_inspector is not None,
                    'yolo_error': self.model_error, 'opencv_error': self.side_error,
                    'side_queue': len(self.queued_side),
                    'rgb1': 'RED_BLINK' if self.hardware.recheck else
                            ('GREEN' if self.last_result=='OK' else 'RED' if self.last_result=='NG' else 'OFF'),
                    'rgb2': 'RED_BLINK' if self.hardware.estop else 'OFF',
                    'buzzer': 'ON' if self.hardware.estop else 'OFF',
                    'servo': 'HOME'}

    def control(self, action):
        action=action.upper()
        with self.lock:
            if action=='STOP':
                self.hardware.set_conveyor_stop()
                self._set_state('STOPPED','현장 작업자가 컨베이어를 정지했습니다.')
                return self.status()
            if action=='ACK_RECHECK':
                if self.hardware.estop: raise ValueError('비상정지 상태에서는 재확인을 해제할 수 없습니다.')
                if self.state != 'RECHECK': raise ValueError('현재 재확인 상태가 아닙니다.')
                self.hardware.set_recheck(False)
                self.queued_side.clear(); self.side_samples.clear(); self.side_active=False
                self.top_occupied=True # require product to leave the center before next cycle
                self._set_state('STOPPED', '재확인 경고 해제; 검사 대상 수동 확인 후 START')
                self._event('ALERT','CLEAR','재확인 경고를 해제했습니다. START 대기')
                return self.status()
            if action=='START':
                if self.hardware.estop: raise ValueError('비상정지 버튼 해제 후 시작할 수 있습니다.')
                if self.hardware.recheck: raise ValueError('재확인 경고 해제 후 시작하세요.')
                if self.mode=='AUTO' and (self.model is None or self.side_inspector is None):
                    raise ValueError('YOLO 모델 또는 측면 기준 사진이 없어 AUTO 검사를 시작할 수 없습니다.')
                self.hardware.set_conveyor_normal()
                self._set_state('RUNNING','컨베이어 가동 / 측면 OpenCV 검사 중')
                return self.status()
            if action=='SERVO_HOME':
                if self.hardware.estop or self.state=='RUNNING':
                    raise ValueError('비상정지 또는 컨베이어 가동 중에는 서보 복귀를 할 수 없습니다.')
                self.hardware.return_servo()
                return self.status()
            if action=='SERVO_TEST':
                if self.hardware.estop or self.state=='RUNNING':
                    raise ValueError('비상정지 또는 컨베이어 가동 중에는 서보 테스트를 할 수 없습니다.')
                self.hardware.eject_tire()
                self._event('SERVO', 'OK', '작업자 서보 테스트 완료')
                return self.status()
            if action in ('AUTO','MANUAL'):
                if self.state == 'RUNNING': raise ValueError('운전 중 모드 변경 불가')
                self.mode=action
                self._event('MODE','OK',f'{action} 모드')
                return self.status()
            raise ValueError('지원하지 않는 제어 명령: '+action)

    def _recheck(self, why):
        with self.lock:
            if self.hardware.estop: return
            self.hardware.set_conveyor_stop()
            self.hardware.set_recheck(True)  # RGB1 only, no buzzer
            self.last_result='RECHECK'
            self._set_state('RECHECK', why)
            self._event('LED', 'RECHECK', 'RGB1 빨간색 점멸')
            self._event('ALERT','RECHECK',f'재확인 필요: {why}')

    def _side_loop(self):
        while not self.shutdown.wait(0.45):
            if self.hardware.estop or self.state != 'RUNNING': continue
            self._side_once()

    def _top_loop(self):
        while not self.shutdown.wait(0.08):
            if self.hardware.estop or self.state != 'RUNNING': continue
            if self.model is not None: self._top_once()

    def _side_once(self):
        raw=self.cameras['side'].get_frame()
        if raw is None:return
        if self.side_inspector is None:return
        try:
            image=resize_frame(raw)
            data,_=self.side_inspector.inspect(image)
            verdict=data.get('side_result','RECHECK_UNCLASSIFIED')
            detected=data.get('location')=='FOUND'
        except Exception as exc:
            self.side_error=str(exc)
            self._recheck(f'측면 검사 오류: {exc}')
            return
        with self.lock:
            if self.state != 'RUNNING': return
            if detected:
                self.side_absent=0
                self.side_active=True
                self.side_samples.append(verdict)
                self.side_samples=self.side_samples[-10:]
            elif self.side_active and verdict=='NO_PRODUCT':
                self.side_absent+=1
                if self.side_absent>=3:
                    samples=self.side_samples
                    # Only a stable single inspection session is promoted.
                    if len(samples)<3:
                        side_result='RECHECK'
                    elif any(s.startswith('RECHECK') for s in samples):
                        side_result='RECHECK'
                    elif samples.count('NG_DAMAGE')>=2:
                        side_result='NG'
                    elif samples.count('SIDE_PASS')>=2:
                        side_result='PASS'
                    else:
                        side_result='RECHECK'
                    self.queued_side.append({'result':side_result,'time':time.monotonic(),
                        'reason':samples[-1] if samples else 'NO_SAMPLES'})
                    self._event('CAMERA','OK',f'측면 OpenCV 검사: {side_result}')
                    self.side_active=False;self.side_samples=[];self.side_absent=0
            # Unexpected multiple products / tracking ambiguity needs operator review.
            elif verdict.startswith('RECHECK_MULTIPLE'):
                self._recheck('측면 카메라에 여러 제품이 감지됐습니다.')
    def _top_once(self):
        frame=self.cameras['top'].get_frame()
        if frame is None:return
        try:
            result=self.model.predict(source=frame,conf=CONF,imgsz=640,
                device='cpu',verbose=False,max_det=4,agnostic_nms=True,iou=0.45)[0]
            boxes=result.boxes
            if len(boxes)==0:
                self.top_absent+=1
                if self.top_absent>=3:self.top_occupied=False
                return
            self.top_absent=0
            if self.top_occupied:return
            if len(boxes)!=1:
                self._recheck('상단 YOLO에 여러 제품 또는 여러 판정이 감지됐습니다.')
                return
            b=boxes[0]
            xyxy=b.xyxy[0].tolist()
            center=(xyxy[0]+xyxy[2])*0.5/frame.shape[1]
            if not (CENTER_MIN <= center <= CENTER_MAX):return
            self.top_occupied=True
            # STOP happens BEFORE final top classification and ejection.
            self.hardware.set_conveyor_stop()
            self._set_state('INSPECTING','상단 YOLO 중앙 위치 도착: 컨베이어 정지')
            # Confirm classification AGAIN after motor stops; never use only a moving frame.
            time.sleep(0.25)
            stopped_frame = self.cameras['top'].get_frame()
            if stopped_frame is None:
                self._recheck('상단 정지 후 프레임을 확보하지 못했습니다.')
                return
            stopped = self.model.predict(source=stopped_frame, conf=CONF, imgsz=640,
                device='cpu', verbose=False, max_det=4, agnostic_nms=True, iou=0.45)[0]
            if len(stopped.boxes) != 1:
                self._recheck('컨베이어 정지 후 YOLO 단일 판정이 확인되지 않습니다.')
                return
            confirmed = stopped.boxes[0]
            cxy=confirmed.xyxy[0].tolist()
            cc=(cxy[0]+cxy[2])*0.5/stopped_frame.shape[1]
            if not (CENTER_MIN <= cc <= CENTER_MAX):
                self._recheck('정지 후 타이어가 YOLO 검사 중앙에서 벗어났습니다.')
                return
            name=str(self.model.names[int(confirmed.cls.item())]).upper().strip()
            self._finish_top(name)
        except Exception as exc:
            self._recheck(f'상단 YOLO 오류: {exc}')

    def _finish_top(self,name):
        with self.lock:
            if not self.queued_side:
                self._recheck('측면 OpenCV 결과가 없어 동일 제품을 확인할 수 없습니다.')
                return
            if len(self.queued_side)!=1:
                self._recheck('측면 검출 대기 제품이 여러 개여서 순서가 불확실합니다.')
                return
            side=self.queued_side.popleft()
            if time.monotonic()-side['time']>SIDE_TTL:
                self._recheck('측면과 상단 검사 시간 차이가 커서 제품 매칭이 불확실합니다.')
                return
            top={'NORMAL':'PASS','OK':'PASS','WHEEL_DAMAGE':'WHEEL_DAMAGE',
                 'TIRE_DAMAGE':'TIRE_DAMAGE','BOTH_DAMAGE':'BOTH_DAMAGE'}.get(name)
            if side['result']=='RECHECK' or top is None:
                self._recheck('검사 결과 재확인 필요: 측면='+side['result']+' 상단='+name)
                return
            side_bad=(side['result']=='NG')
            wheel_bad=top in ('WHEEL_DAMAGE','BOTH_DAMAGE')
            tire_bad=side_bad or top in ('TIRE_DAMAGE','BOTH_DAMAGE')
            if wheel_bad and tire_bad: defect='BOTH_DAMAGE'
            elif wheel_bad: defect='WHEEL_DAMAGE'
            elif tire_bad: defect='TIRE_DAMAGE'
            else: defect=''
            result='NG' if defect else 'OK'
            self.product_id='TIRE-'+time.strftime('%Y%m%d-%H%M%S')+'-'+uuid.uuid4().hex[:6]
            # Persist the completed verdict BEFORE servo action or recheck halt.
            # The outbox synchronously commits to the Pi SQLite database, then
            # retries on connectivity failures using this immutable event id.
            self.notify('INSPECTION', {'tire_id':self.product_id, 'result':result,
                'event_id':str(uuid.uuid4()), 'defect_type':defect or None,
                'defect_detail':f'SIDE={side["reason"]}; TOP={name}',
                'image_path':'', 'is_demo':False})
            self.last_result=result
            if result=='NG':
                self.hardware.set_inspection_ng(auto_eject=False)
                if not AUTO_EJECT_ENABLED:
                    self._recheck('불량 판정 저장 완료. 서보 위치 미검증으로 자동 배출 중지: 위치 보정 후 AUTO_EJECT_ENABLED=1 설정 필요')
                    return
                self.hardware.eject_tire()
            else:
                self.hardware.set_inspection_ok()
            self._event('CAMERA','OK',f'검사 완료: {result} {defect}')
            self._event('LED','OK' if result=='OK' else 'NG',
                        'RGB1 녹색' if result=='OK' else 'RGB1 빨간색')
            self._event('SERVO','OK','불량 배출 완료' if result=='NG' else '서보 대기')
            if not self.hardware.estop and not self.hardware.recheck:
                self.hardware.set_conveyor_normal()
                self._set_state('RUNNING','검사·배출 완료 / 컨베이어 재가동')

    def close(self):
        self.shutdown.set()
        self.hardware.close()
