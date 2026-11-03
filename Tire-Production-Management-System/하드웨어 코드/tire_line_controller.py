
"""Integrated tire-line controller.

Side OpenCV -> Top YOLO -> final decision -> inspection outbox.
One final inspection event per matched tire.
Hardware remains stopped on NG pending operator review.
"""

from __future__ import annotations

import collections
import cv2
import os
import threading
import time
import uuid
from pathlib import Path
from datetime import datetime

from tire_line_hardware import TireLineHardware
from inspect_side_integrated import MultiInspector, resize_frame


BASE = Path(__file__).resolve().parent
NG_IMAGE_ROOT = BASE / "inspection_images"

MODEL = Path(
    os.environ.get(
        "YOLO_MODEL",
        str(BASE / "models/best_v3.pt")
    )
)

CENTER_MIN = float(os.getenv("TOP_CENTER_MIN", "0.30"))
CENTER_MAX = float(os.getenv("TOP_CENTER_MAX", "0.70"))
CONF = float(os.getenv("YOLO_CONF", "0.70"))
SIDE_TTL = float(os.getenv("SIDE_TO_TOP_MAX_SECONDS", "40"))

# Physical servo alignment and stop circuit must be validated first.
AUTO_EJECT_ENABLED = os.getenv("AUTO_EJECT_ENABLED", "0") == "1"


class TireLineController:

    def __init__(self, cameras, notify, arduino_port=None):
        self.cameras = cameras
        self.notify = notify

        self.lock = threading.RLock()
        self.shutdown = threading.Event()

        self.state = "STOPPED"
        self.message = "시작 대기"
        self.mode = "AUTO"

        self.last_result = "NONE"
        self.last_yolo = "NONE"
        self.last_opencv = "NONE"
        self.last_defect = "NONE"
        self.last_inspection_time = ""
        self.last_event_id = ""
        self.last_confidence = 0.0
        self.yolo_boxes = []
        self.yolo_boxes_updated_at = 0.0

        self.product_id = ""
        self.servo_state = "HOME"

        self.queued_side = collections.deque(maxlen=6)
        self.side_active = False
        self.side_samples = []
        self.side_last_frame = None
        self.side_absent = 0

        self.top_occupied = False
        self.top_absent = 0

        self._last_status = None
        self._last_inspect = 0.0

        port = arduino_port or os.getenv(
            "ARDUINO_PORT", "/dev/ttyUSB0"
        )

        self.hardware = TireLineHardware(arduino_port=port)
        self.hardware.set_status_handler(self._on_serial)

        self.model = None
        self.side_inspector = None
        self.model_error = None
        self.side_error = None

        self._load_models()

        self.side_thread = threading.Thread(
            target=self._side_loop,
            daemon=True
        )

        self.top_thread = threading.Thread(
            target=self._top_loop,
            daemon=True
        )

        self.side_thread.start()
        self.top_thread.start()

    def _load_models(self):
        try:
            if not MODEL.is_file():
                raise FileNotFoundError(str(MODEL))

            from ultralytics import YOLO
            self.model = YOLO(str(MODEL))

        except Exception as exc:
            self.model_error = str(exc)
            print("[YOLO NOT READY]", exc, flush=True)

        try:
            self.side_inspector = MultiInspector()

        except Exception as exc:
            self.side_error = str(exc)
            print("[SIDE NOT READY]", exc, flush=True)

    def _event(self, equipment, status, message):
        self.notify(
            "EQUIPMENT",
            {
                "equipment": equipment,
                "status": status,
                "message": message
            }
        )

    def _set_state(self, state, message):
        with self.lock:
            if self.state == state and self.message == message:
                return

            self.state = state
            self.message = message

        equipment_status = (
            "RUNNING" if state == "RUNNING"
            else "RECHECK" if state == "RECHECK"
            else "ERROR" if state == "ESTOP"
            else "STOPPED"
        )

        self._event(
            "CONVEYOR",
            equipment_status,
            message
        )

    def _on_serial(self, reply):
        if reply in ("ESTOP_ACTIVE", "BLOCKED:ESTOP"):
            self.hardware.estop = True

            self._set_state(
                "ESTOP",
                "비상정지 버튼 작동: 모터 정지"
            )

            self._event(
                "LED",
                "ESTOP",
                "RGB2 빨간색 점멸"
            )

            self._event(
                "BUZZER",
                "ESTOP",
                "부저 경고"
            )

            self._event(
                "ALERT",
                "ESTOP",
                "비상정지 버튼 작동. 현장 확인 필요"
            )

        elif reply == "ESTOP_RELEASED":
            self._set_state(
                "STOPPED",
                "비상정지 해제: 작업자 START 대기"
            )

            self._event(
                "ALERT",
                "CLEAR",
                "비상정지 해제. 자동 재가동하지 않습니다."
            )

        elif reply == "WATCHDOG_STOP":
            self._set_state(
                "STOPPED",
                "통신 워치독 정지"
            )

            self._event(
                "ALERT",
                "WARN",
                "Arduino 통신 워치독 정지"
            )

    def status(self):
        with self.lock:
            return {
                "state": self.state,
                "message": self.message,
                "estop": self.hardware.estop,
                "recheck": self.hardware.recheck,
                "mode": self.mode,

                "product_id": self.product_id,
                "last_result": self.last_result,

                "yolo_result": self.last_yolo,
                "opencv_result": self.last_opencv,
                "defect_type": self.last_defect,
                "inspection_time": self.last_inspection_time,
                "event_id": self.last_event_id,
                "yolo_confidence": self.last_confidence,
                "yolo_boxes": [dict(b) for b in self.yolo_boxes],
                "yolo_boxes_age": (
                    time.monotonic() - self.yolo_boxes_updated_at
                ),

                "yolo_ready": self.model is not None,
                "opencv_ready": self.side_inspector is not None,

                "yolo_error": self.model_error,
                "opencv_error": self.side_error,

                "side_queue": len(self.queued_side),

                "rgb1": self.hardware.rgb1_status(),

                "rgb2": (
                    "RED_BLINK" if self.hardware.estop
                    else "OFF"
                ),

                "buzzer": (
                    "ON" if self.hardware.estop
                    else "OFF"
                ),

                "servo": self.servo_state,
                "auto_eject_enabled": AUTO_EJECT_ENABLED
            }

    def control(self, action):
        action = str(action).upper()

        with self.lock:

            
            if action == "STOP":
                self.hardware.set_conveyor_stop()

                if self.hardware.recheck:
                    self._set_state(
                        "RECHECK",
                        "컨베이어 정지 / 재확인 필요"
                    )
                else:
                    self._set_state(
                        "STOPPED",
                        "작업자가 컨베이어를 정지했습니다."
                    )

                return self.status()

            if action == "ACK_RECHECK":
                if self.hardware.estop:
                    raise ValueError(
                        "비상정지 상태에서는 재확인을 해제할 수 없습니다."
                    )

                if not self.hardware.recheck:
                    raise ValueError(
                        "현재 재확인 경고가 없습니다."
                    )

                self.hardware.set_conveyor_stop()
                self.hardware.set_recheck(False)

                self.queued_side.clear()
                self.side_samples.clear()
                self.side_active = False
                self.side_absent = 0

                self.top_occupied = True

                self._set_state(
                    "STOPPED",
                    "재확인 해제 / 제품 확인 후 START 대기"
                )

                self._event(
                    "ALERT",
                    "CLEAR",
                    "재확인 경고 해제"
                )

                return self.status()


            if action == "START":
                if self.hardware.estop:
                    raise ValueError(
                        "비상정지 해제 후 시작할 수 있습니다."
                    )

                if self.hardware.recheck:
                    raise ValueError(
                        "재확인 경고를 먼저 해제하세요."
                    )

                if self.state not in ("STOPPED", "RUNNING"):
                    raise ValueError(
                        "현재 상태에서는 컨베이어를 시작할 수 없습니다."
                    )

                if self.state == "RUNNING":
                    return self.status()

                if self.mode == "AUTO":
                    if self.model is None or self.side_inspector is None:
                        raise ValueError(
                            "YOLO 또는 OpenCV 검사 준비가 완료되지 않았습니다."
                        )

                self.hardware.set_conveyor_normal()

                self._set_state(
                    "RUNNING",
                    "컨베이어 가동 / 측면 OpenCV 검사 중"
                )

                return self.status()

            if action == "SERVO_HOME":
                if (
                    self.hardware.estop
                    or self.hardware.recheck
                    or self.state != "STOPPED"
                ):
                    raise ValueError(
                        "컨베이어 정지 및 경고 해제 상태에서만 "
                        "서보를 복귀할 수 있습니다."
                    )

                self.servo_state = "RETURNING"

                try:
                    self.hardware.return_servo()
                    self.servo_state = "HOME"

                    self._event(
                        "SERVO",
                        "OK",
                        "서보 원점 복귀 명령 완료"
                    )

                except Exception:
                    self.servo_state = "UNKNOWN"
                    raise

                return self.status()

            if action == "SERVO_TEST":
                if (
                    self.hardware.estop
                    or self.hardware.recheck
                    or self.state != "STOPPED"
                ):
                    raise ValueError(
                        "컨베이어 정지 및 경고 해제 상태에서만 "
                        "서보 테스트가 가능합니다."
                    )

                self.servo_state = "EJECTING"

                try:
                    self.hardware.eject_tire()
                    self.servo_state = "HOME"

                    self._event(
                        "SERVO",
                        "OK",
                        "서보 테스트 명령 완료"
                    )

                except Exception:
                    self.servo_state = "UNKNOWN"
                    raise

                return self.status()

            if action in ("AUTO", "MANUAL"):
                if self.state not in ("STOPPED",):
                    raise ValueError(
                        "정지 상태에서만 운전 모드를 변경할 수 있습니다."
                    )

                self.mode = action

                self._event(
                    "MODE",
                    "OK",
                    f"{action} 모드"
                )

                return self.status()

            raise ValueError(
                "지원하지 않는 제어 명령: " + action
            )

    def _recheck(self, why):
        with self.lock:
            if self.hardware.estop:
                return

            if self.state not in ("RUNNING", "INSPECTING"):
                return

            self.hardware.set_conveyor_stop()
            self.hardware.set_recheck(True)

            self._set_state(
                "RECHECK",
                why
            )

            self._event(
                "LED",
                "RECHECK",
                "RGB1 빨간색 점멸"
            )

            self._event(
                "ALERT",
                "RECHECK",
                f"재확인 필요: {why}"
            )

    def _side_loop(self):
        while not self.shutdown.wait(0.45):
            if self.hardware.estop:
                continue

            with self.lock:
                can_inspect = (
                    self.state == "RUNNING"
                    or (
                        self.state == "INSPECTING"
                        and self.side_active
                    )
                )

            if not can_inspect:
                continue

            self._side_once()

    def _top_loop(self):
        while not self.shutdown.wait(0.08):
            if self.hardware.estop:
                continue

            if self.state != "RUNNING":
                continue

            if self.model is not None:
                self._top_once()

    def _side_once(self):
        raw = self.cameras["side"].get_frame()

        if raw is None or self.side_inspector is None:
            return

        try:
            image = resize_frame(raw)
            data, _ = self.side_inspector.inspect(image)

            verdict = str(
                data.get("side_result", "RECHECK_UNCLASSIFIED")
            )

            detected = data.get("location") == "FOUND"

        except Exception as exc:
            self.side_error = str(exc)
            self._recheck(f"측면 검사 오류: {exc}")
            return

        with self.lock:
            if not (
                self.state == "RUNNING"
                or (
                    self.state == "INSPECTING"
                    and self.side_active
                )
            ):
                return

            if verdict.startswith("RECHECK_MULTIPLE"):
                self._recheck(
                    "측면 카메라에 여러 제품이 감지됐습니다."
                )
                return

            if detected:
                self.side_last_frame = raw.copy()
                self.side_absent = 0
                self.side_active = True

                self.side_samples.append(verdict)
                self.side_samples = self.side_samples[-10:]

            elif self.side_active and verdict == "NO_PRODUCT":
                self.side_absent += 1

                if self.side_absent < 3:
                    return

                samples = list(self.side_samples)

                # 측면 OpenCV 다중 프레임 판정 집계
                pass_count = samples.count("SIDE_PASS")
                damage_count = samples.count("NG_DAMAGE")
                wrong_count = samples.count("NG_WRONG_TIRE")

                has_multiple = any(
                    s.startswith("RECHECK_MULTIPLE")
                    for s in samples
                )

                # 일시적인 미분류 결과는 유효 판정과 분리
                # 서로 다른 확정 판정이 섞이면 재확인
                valid_groups = sum([
                    pass_count > 0,
                    damage_count > 0,
                    wrong_count > 0
                ])

                if len(samples) < 3 or has_multiple:
                    side_result = "RECHECK"
                    side_reason = "INSUFFICIENT_OR_MULTIPLE"

                elif valid_groups > 1:
                    side_result = "RECHECK"
                    side_reason = "CONFLICTING_SIDE_RESULTS"

                elif damage_count >= 2:
                    side_result = "NG"
                    side_reason = "NG_DAMAGE"

                elif wrong_count >= 2:
                    side_result = "NG"
                    side_reason = "NG_WRONG_TIRE"

                elif pass_count >= 2:
                    side_result = "PASS"
                    side_reason = "SIDE_PASS"

                else:
                    side_result = "RECHECK"
                    side_reason = "INSUFFICIENT_VALID_SAMPLES"

                print(
                    f"[SIDE VOTE] "
                    f"PASS={pass_count} "
                    f"DAMAGE={damage_count} "
                    f"WRONG={wrong_count} "
                    f"TOTAL={len(samples)} "
                    f"RESULT={side_result} "
                    f"REASON={side_reason}",
                    flush=True
                )

                self.queued_side.append({
                    "result": side_result,
                    "time": time.monotonic(),
                    "reason": side_reason,
                    "image_frame": (
                        self.side_last_frame.copy()
                        if self.side_last_frame is not None
                        else None
                    )
                })
                print(
                    f"[INSPECTION TIMING] SIDE completed "
                    f"result={side_result} reason={side_reason} "
                    f"queue={len(self.queued_side)}",
                    flush=True
                )

                self._event(
                    "CAMERA",
                    "OK",
                    f"측면 OpenCV 검사: {side_result}"
                )

                self.side_active = False
                self.side_samples.clear()
                self.side_absent = 0

    def _store_yolo_boxes(self, result):
        boxes = []
        for b in result.boxes:
            x1, y1, x2, y2 = map(
                int, b.xyxy[0].tolist()
            )
            cls_id = int(b.cls.item())
            label = str(self.model.names[cls_id]).upper()
            boxes.append({
                "x1": x1,
                "y1": y1,
                "x2": x2,
                "y2": y2,
                "label": label,
                "conf": float(b.conf.item())
            })
        with self.lock:
            self.yolo_boxes = boxes
            self.yolo_boxes_updated_at = time.monotonic()

    def _top_once(self):
        frame = self.cameras["top"].get_frame()

        if frame is None:
            return

        try:
            moving = self.model.predict(
                source=frame,
                conf=CONF,
                imgsz=640,
                device="cpu",
                verbose=False,
                max_det=4,
                agnostic_nms=True,
                iou=0.45
            )[0]

            self._store_yolo_boxes(moving)
            boxes = moving.boxes

            if len(boxes) == 0:
                self.top_absent += 1

                if self.top_absent >= 3:
                    self.top_occupied = False

                return

            self.top_absent = 0

            if self.top_occupied:
                return

            if len(boxes) != 1:
                self._recheck(
                    "상단 YOLO에 여러 판정이 감지됐습니다."
                )
                return

            box = boxes[0]
            xyxy = box.xyxy[0].tolist()

            center = (
                (xyxy[0] + xyxy[2]) * 0.5
                / frame.shape[1]
            )

            if not CENTER_MIN <= center <= CENTER_MAX:
                return

            with self.lock:
                if self.state != "RUNNING" or self.hardware.estop:
                    return

                self.top_occupied = True
                self.hardware.set_conveyor_stop()

                self._set_state(
                    "INSPECTING",
                    "YOLO 중앙 위치 도착: 컨베이어 정지"
                )

            time.sleep(0.25)

            stopped_frame = self.cameras["top"].get_frame()

            if stopped_frame is None:
                self._recheck(
                    "컨베이어 정지 후 카메라 프레임 확보 실패"
                )
                return

            # 정지 후 YOLO 최대 3회 재검사
            confirmed_result = None
            last_error = "YOLO 판정 실패"

            for attempt in range(3):
                with self.lock:
                    if (
                        self.state != "INSPECTING"
                        or self.hardware.estop
                        or self.hardware.recheck
                    ):
                        return

                if attempt > 0:
                    time.sleep(0.3)

                stopped_frame = self.cameras["top"].get_frame()

                if stopped_frame is None:
                    last_error = "카메라 프레임 확보 실패"
                    continue

                stopped = self.model.predict(
                    source=stopped_frame,
                    conf=CONF,
                    imgsz=640,
                    device="cpu",
                    verbose=False,
                    max_det=4,
                    agnostic_nms=True,
                    iou=0.45
                )[0]

                self._store_yolo_boxes(stopped)

                if len(stopped.boxes) != 1:
                    last_error = (
                        f"YOLO 검출 개수 {len(stopped.boxes)}개"
                    )
                    print(
                        f"[YOLO RETRY] {attempt + 1}/3 "
                        f"{last_error}",
                        flush=True
                    )
                    continue

                box = stopped.boxes[0]
                xyxy = box.xyxy[0].tolist()

                center = (
                    (xyxy[0] + xyxy[2]) * 0.5
                    / stopped_frame.shape[1]
                )

                if not CENTER_MIN <= center <= CENTER_MAX:
                    last_error = (
                        f"검사 위치 이탈 ({center:.3f})"
                    )
                    print(
                        f"[YOLO RETRY] {attempt + 1}/3 "
                        f"{last_error}",
                        flush=True
                    )
                    continue

                name = str(
                    self.model.names[int(box.cls.item())]
                ).upper().strip()

                confidence = float(box.conf.item())
                confirmed_result = (name, confidence)

                print(
                    f"[YOLO CONFIRMED] "
                    f"{name} confidence={confidence:.3f} "
                    f"attempt={attempt + 1}",
                    flush=True
                )
                break

            if confirmed_result is None:
                self._recheck(
                    f"정지 후 YOLO 3회 확인 실패: {last_error}"
                )
                return

            with self.lock:
                if (
                    self.state != "INSPECTING"
                    or self.hardware.estop
                    or self.hardware.recheck
                ):
                    return

            name, confidence = confirmed_result
            self._finish_top(name, confidence, stopped_frame)

        except Exception as exc:
            self._recheck(f"상단 YOLO 오류: {exc}")

    def _finish_top(self, name, confidence=0.0, top_frame=None):
        wait_seconds = max(
            0.0,
            float(os.getenv("SIDE_FINALIZE_WAIT_SECONDS", "3.0"))
        )
        deadline = time.monotonic() + wait_seconds

        while time.monotonic() < deadline:
            with self.lock:
                if (
                    self.state != "INSPECTING"
                    or self.hardware.estop
                    or self.hardware.recheck
                ):
                    return

                if len(self.queued_side) >= 1:
                    break

                if not self.side_active:
                    break

            time.sleep(0.1)

        with self.lock:

            if self.state != "INSPECTING" or self.hardware.estop:
                return

            if len(self.queued_side) != 1:
                queue_count = len(self.queued_side)

                print(
                    f"[SIDE QUEUE ERROR] "
                    f"count={queue_count} "
                    f"state={self.state}",
                    flush=True
                )

                self._recheck(
                    f"측면 검사 결과 매칭 실패 "
                    f"(대기열 {queue_count}건)"
                )
                return

            side = self.queued_side[0]

            elapsed = time.monotonic() - side["time"]
            print(
                f"[INSPECTION TIMING] SIDE->TOP={elapsed:.2f}s "
                f"LIMIT={SIDE_TTL:.2f}s "
                f"queue={len(self.queued_side)}",
                flush=True
            )



            if elapsed > SIDE_TTL:
                self._recheck(
                    f"측면 검사 결과 유효시간 초과 "
                    f"({elapsed:.1f}초)"
                )
                return

            top = {
                "NORMAL": "PASS",
                "OK": "PASS",
                "WHEEL_DAMAGE": "WHEEL_DAMAGE",
                "TIRE_DAMAGE": "TIRE_DAMAGE",
                "BOTH_DAMAGE": "BOTH_DAMAGE"
            }.get(name)

            side_result = side["result"]
            side_reason = str(
                side.get("reason", "")
            ).upper().strip()

            if side_result == "RECHECK" or top is None:
                self._recheck(
                    f"검사 재확인 필요: SIDE={side_result}, TOP={name}"
                )
                return

            if side_result not in ("PASS", "NG"):
                self._recheck(
                    "측면 검사 결과가 올바르지 않습니다."
                )
                return

            if (
                side_result == "NG"
                and side_reason not in (
                    "NG_WRONG_TIRE",
                    "NG_DAMAGE"
                )
            ):
                self._recheck(
                    "측면 불량 원인 확인 실패"
                )
                return

            wrong_tire = (
                side_result == "NG"
                and side_reason == "NG_WRONG_TIRE"
            )

            side_damage = (
                side_result == "NG"
                and side_reason == "NG_DAMAGE"
            )

            wheel_bad = top in (
                "WHEEL_DAMAGE",
                "BOTH_DAMAGE"
            )

            tire_bad = (
                side_damage
                or top in (
                    "TIRE_DAMAGE",
                    "BOTH_DAMAGE"
                )
            )

            if wrong_tire:
                defect = "WRONG_TIRE"

            elif wheel_bad and tire_bad:
                defect = "BOTH_DAMAGE"

            elif wheel_bad:
                defect = "WHEEL_DAMAGE"

            elif tire_bad:
                defect = "TIRE_DAMAGE"

            else:
                defect = ""

            final_result = "NG" if defect else "OK"

            product_id = (
                "TIRE-"
                + time.strftime("%Y%m%d-%H%M%S")
                + "-"
                + uuid.uuid4().hex[:6]
            )

            event_id = str(uuid.uuid4())

            inspection_time = time.strftime(
                "%Y-%m-%d %H:%M:%S"
            )

            # Store NG evidence in separate JPEG files.
            yolo_image_path = ""
            opencv_image_path = ""

            if final_result == "NG":
                try:
                    day = datetime.now().strftime("%Y-%m-%d")
                    folder = NG_IMAGE_ROOT / day / product_id
                    folder.mkdir(parents=True, exist_ok=True)

                    # Top YOLO: capture the frame used for verdict.
                    if (
                        name in (
                            "WHEEL_DAMAGE",
                            "TIRE_DAMAGE",
                            "BOTH_DAMAGE"
                        )
                        and top_frame is not None
                    ):
                        dest = folder / "yolo.jpg"
                        if cv2.imwrite(
                            str(dest), top_frame,
                            [cv2.IMWRITE_JPEG_QUALITY, 80]
                        ):
                            yolo_image_path = str(
                                dest.relative_to(BASE)
                            )

                    # Side OpenCV: use the cached detection frame.
                    side_frame = side.get("image_frame")
                    if (
                        side_result == "NG"
                        and side_frame is not None
                    ):
                        dest = folder / "opencv.jpg"
                        if cv2.imwrite(
                            str(dest), side_frame,
                            [cv2.IMWRITE_JPEG_QUALITY, 75]
                        ):
                            opencv_image_path = str(
                                dest.relative_to(BASE)
                            )

                    print(
                        "[NG IMAGE] "
                        f"tire={product_id} "
                        f"yolo={yolo_image_path or '-'} "
                        f"opencv={opencv_image_path or '-'}",
                        flush=True
                    )
                except Exception as exc:
                    print(
                        f"[NG IMAGE ERROR] {exc}",
                        flush=True
                    )

            inspection_data = {
                "tire_id": product_id,
                "event_id": event_id,
                "result": final_result,

                "defect_type": defect or None,
                "defect_detail": (
                    f"SIDE={side_reason}; TOP={name}"
                ),

                "yolo_result": name,
                "opencv_result": side_reason,
                "yolo_confidence": confidence,

                "inspection_time": inspection_time,
                "image_path": "",
                "pi_yolo_image_path": yolo_image_path,
                "pi_opencv_image_path": opencv_image_path,
                "data_source": "HARDWARE",
                "is_demo": False
            }

            # Persist to the Pi outbox before changing equipment state.
            # If this raises, the same verdict must not be sent again
            # as an unrelated product.
            try:
                self.notify("INSPECTION", inspection_data)

            except Exception as exc:
                self._recheck(
                    f"검사 결과 저장 실패: {exc}"
                )
                return

            self.queued_side.popleft()

            self.product_id = product_id
            self.last_result = final_result
            self.last_yolo = name
            self.last_opencv = side_reason
            self.last_defect = defect or "NONE"
            self.last_inspection_time = inspection_time
            self.last_event_id = event_id
            self.last_confidence = confidence

            self._event(
                "CAMERA",
                "OK",
                (
                    f"검사 완료: {final_result} "
                    f"YOLO={name}, OpenCV={side_reason}"
                )
            )

            if final_result == "NG":
                self.hardware.set_inspection_ng(
                    auto_eject=False
                )

                self._event(
                    "LED",
                    "NG",
                    "RGB1 불량 판정"
                )

                if not AUTO_EJECT_ENABLED:
                    self.servo_state = "WAIT"
                    self._event(
                        "SERVO",
                        "WAIT",
                        "NG 배출 대기"
                    )

                    # Preserve the existing NG approval/recheck flow,
                    # but let RGB1 show solid red for 2 seconds first.
                    def delayed_ng_recheck():
                        with self.lock:
                            if (
                                not self.shutdown.is_set()
                                and not self.hardware.estop
                                and not self.hardware.recheck
                                and self.state == "INSPECTING"
                                and self.last_event_id == event_id
                            ):
                                self._recheck(
                                    "NG 검사 결과 저장 완료 / "
                                    "자동 배출 승인 대기"
                                )

                    timer = threading.Timer(2.0, delayed_ng_recheck)
                    timer.daemon = True
                    timer.start()
                    return

                # The conveyor has already been commanded STOP.
                # Do not start a servo action during an emergency.
                if self.hardware.estop or self.hardware.recheck:
                    self._recheck("NG 배출 차단: 안전 경고")
                    return

                self.servo_state = "EJECTING"
                self._event(
                    "SERVO",
                    "RUNNING",
                    "NG 서보 배출 명령"
                )

                try:
                    completed = self.hardware.eject_tire()
                except Exception as exc:
                    self.servo_state = "UNKNOWN"
                    self._recheck(
                        f"서보 배출 오류: {exc}"
                    )
                    self._event(
                        "SERVO",
                        "ERROR",
                        str(exc)
                    )
                    return

                if not completed:
                    self.servo_state = "UNKNOWN"
                    self._recheck(
                        "서보 배출 취소 또는 원점 복귀 미완료"
                    )
                    self._event(
                        "SERVO",
                        "ERROR",
                        "배출 완료 확인 실패"
                    )
                    return

                self.servo_state = "HOME"
                self._event(
                    "SERVO",
                    "OK",
                    "배출 및 HOME 복귀 명령 완료"
                )

                # Cancel or emergency stop prevents automatic restart.
                if self.state != "INSPECTING" or self.hardware.estop:
                    return

                try:
                    self.hardware.set_conveyor_normal()
                    self._set_state(
                        "RUNNING",
                        "NG 배출 후 다음 타이어 이송"
                    )
                except Exception as exc:
                    self._set_state(
                        "STOPPED",
                        f"배출 후 재가동 실패: {exc}"
                    )
                return

            self.hardware.set_inspection_ok()

            self._event(
                "LED",
                "OK",
                "RGB1 녹색"
            )

            if (
                not self.hardware.estop
                and not self.hardware.recheck
                and self.state == "INSPECTING"
            ):
                try:
                    self.hardware.set_conveyor_normal()

                    self._set_state(
                        "RUNNING",
                        "정상 검사 완료 / 컨베이어 재가동"
                    )

                except Exception as exc:
                    self._set_state(
                        "STOPPED",
                        f"컨베이어 재가동 실패: {exc}"
                    )

                    self._event(
                        "ALERT",
                        "WARN",
                        f"재가동 실패: {exc}"
                    )

    def close(self):
        self.shutdown.set()

        try:
            self.hardware.close()
        finally:
            for worker in (self.side_thread, self.top_thread):
                if worker.is_alive():
                    worker.join(timeout=2)
