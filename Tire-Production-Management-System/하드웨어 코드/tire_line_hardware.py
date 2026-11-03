import os
import threading
from time import sleep

import serial
from gpiozero import RGBLED, AngularServo, TonalBuzzer
from gpiozero.tones import Tone


class TireLineHardware:
    """Pi GPIO hardware + Arduino UNO conveyor controller over USB serial."""

    def __init__(self, arduino_port="/dev/ttyACM0"):
        # Raspberry Pi GPIO pin mapping (unchanged)
        self.SERVO_PIN = 18
        self.BUZZER_PIN = 13
        self.HOME_ANGLE = -35
        self.EJECT_ANGLE = 90
        self.EJECT_HOLD_TIME = 0.35
        self.RETURN_WAIT_TIME = 0.45

        self._serial_lock = threading.Lock()
        self._run_lock = threading.Lock()
        self._run_ack = threading.Event()
        self._run_pending = False
        self._run_response = None
        self._shutdown_event = threading.Event()
        self._conveyor_running = False
        self._stop_alarm_event = threading.Event()
        self._buzzer_thread = None
        self._is_alarm_active = False
        self._servo_lock = threading.Lock()
        self._servo_cancel = threading.Event()
        self.estop = False
        self.recheck = False
        self.quality_result = None
        self._serial_status_handler = None
        self._last_serial_event = None
        self._eject_thread = None
        self._closed = False
        self._rgb1_lock = threading.RLock()
        self._rgb1_timer = None
        self._rgb1_generation = 0
        self._rgb1_state = "OFF"

        # Opening the UNO USB port usually resets its sketch.
        # If this port differs, check `ls /dev/ttyACM* /dev/ttyUSB*`.
        self.arduino = serial.Serial(
            arduino_port, baudrate=9600, timeout=0.7, write_timeout=1
        )
        sleep(2.0)
        self._send_arduino("STOP")

        try:
            # RGB1: OK / NG / RECHECK
            self.conveyor_led = RGBLED(red=23, green=24, blue=25)
            # RGB2: ESTOP
            self.inspection_led = RGBLED(red=22, green=27, blue=17)
            self.servo = AngularServo(
                self.SERVO_PIN,
                initial_angle=None,
                min_angle=-90,
                max_angle=90,
                min_pulse_width=1.0 / 1000,
                max_pulse_width=2.0 / 1000,
            )
            self.buzzer = TonalBuzzer(self.BUZZER_PIN)
        except Exception:
            self.arduino.close()
            raise

        self._init_servo()
        self.all_off()

        self._keepalive_thread = threading.Thread(
            target=self._conveyor_keepalive_worker, daemon=True
        )
        self._keepalive_thread.start()
        self._serial_reader_thread = threading.Thread(target=self._arduino_reader, daemon=True)
        self._serial_reader_thread.start()
        self._send_arduino("STATUS")

    def _send_arduino(self, command, expect_reply=False):
        with self._serial_lock:
            if not self.arduino.is_open:
                raise RuntimeError("Arduino serial port is closed")
            # Reader runs only in one worker; keep replies in a queue for RUN.
            # Serial reader must not clear input while status events are expected.
            self.arduino.write((command + "\n").encode("ascii"))
            self.arduino.flush()
            # RUN acknowledgement is processed by _arduino_reader.
            # The physical D7 stop switch blocks RUN independently on Arduino.

    def _arduino_reader(self):
        while not self._shutdown_event.is_set():
            try:
                reply = self.arduino.readline().decode("ascii", errors="replace").strip()
                if not reply: continue
                self._last_serial_event = reply

                if reply in ("RUN_OK", "BLOCKED:ESTOP",
                             "ESTOP_ACTIVE", "WATCHDOG_STOP"):
                    with self._run_lock:
                        if self._run_pending:
                            self._run_response = reply
                            self._run_ack.set()

                if reply == "ESTOP_ACTIVE" or reply == "BLOCKED:ESTOP":
                    self.estop = True
                    self._conveyor_running = False
                    self.inspection_led.blink(on_time=0.3, off_time=0.3,
                        on_color=(1, 0, 0), off_color=(0, 0, 0), background=True)
                    self.start_alarm()
                elif reply == "ESTOP_RELEASED":
                    self.estop = False
                    self.stop_alarm()
                    self.inspection_led.off()
                elif reply in ("WATCHDOG_STOP", "STOP_PIN_OPEN"):
                    self._conveyor_running = False
                if self._serial_status_handler:
                    self._serial_status_handler(reply)
            except (serial.SerialException, OSError, RuntimeError):
                self._conveyor_running = False
                break

    def set_status_handler(self, handler):
        self._serial_status_handler = handler

    def _rgb1_cancel_timer(self):
        """Cancel pending RGB1 OFF callback. Caller may already hold RLock."""
        with self._rgb1_lock:
            self._rgb1_generation += 1
            if self._rgb1_timer is not None:
                self._rgb1_timer.cancel()
                self._rgb1_timer = None

    def _rgb1_off(self):
        with self._rgb1_lock:
            self._rgb1_cancel_timer()
            self._rgb1_state = "OFF"
            self.conveyor_led.off()

    def _rgb1_pulse(self, color, state):
        """Show a final inspection result for exactly 2 seconds."""
        with self._rgb1_lock:
            self._rgb1_cancel_timer()
            if self._closed or self.estop or self.recheck:
                self._rgb1_state = "OFF"
                self.conveyor_led.off()
                return

            generation = self._rgb1_generation
            self._rgb1_state = state
            self.conveyor_led.color = color

            def turn_off():
                with self._rgb1_lock:
                    if generation != self._rgb1_generation or self._closed:
                        return
                    self.conveyor_led.off()
                    self._rgb1_state = "OFF"
                    self._rgb1_timer = None

            timer = threading.Timer(2.0, turn_off)
            timer.daemon = True
            self._rgb1_timer = timer
            timer.start()

    def set_recheck(self, enabled):
        with self._rgb1_lock:
            self.recheck = bool(enabled)
            self._rgb1_cancel_timer()
            if self.estop or not enabled:
                self._rgb1_state = "OFF"
                self.conveyor_led.off()
            else:
                self._rgb1_state = "RED_BLINK"
                self.conveyor_led.blink(
                    on_time=0.3, off_time=0.3,
                    on_color=(1, 0, 0), off_color=(0, 0, 0),
                    background=True,
                )

    def rgb1_status(self):
        with self._rgb1_lock:
            return self._rgb1_state

    def _conveyor_keepalive_worker(self):
        """Only PING while running; no auto-RUN after physical stop or watchdog."""
        while not self._shutdown_event.wait(0.4):
            if self._conveyor_running:
                try:
                    self._send_arduino("PING")
                except (serial.SerialException, OSError, RuntimeError) as exc:
                    self._conveyor_running = False
                    print(f"[Arduino communication error] {exc}")
                    try:
                        self._rgb1_off()
                    except Exception:
                        pass

    def _init_servo(self):
        try:
            self.servo.angle = self.HOME_ANGLE
            sleep(0.4)
            self.servo.detach()
        except Exception as exc:
            print(f"[Servo init error] {exc}")

    # Conveyor methods now control the *actual* Arduino motor as well as LEDs.
    def set_conveyor_normal(self):
        """Start only when no servo operation is in progress."""
        if not self._servo_lock.acquire(blocking=False):
            raise RuntimeError("서보 동작 중 컨베이어 START 금지")
        try:
            return self._set_conveyor_normal_locked()
        finally:
            self._servo_lock.release()

    def _set_conveyor_normal_locked(self):
        """Explicit RUN request: buzzer off, Arduino conveyor on."""
        self._conveyor_running = False
        if self.estop or self.recheck:
            raise RuntimeError("RUN 차단: 비상정지 또는 재확인 상태")
        with self._run_lock:
            if self._run_pending:
                raise RuntimeError("RUN 승인 대기 중입니다.")
            self._run_pending = True
            self._run_response = None
            self._run_ack.clear()

        try:
            self._send_arduino("RUN")
            received = self._run_ack.wait(timeout=1.0)

            with self._run_lock:
                response = self._run_response
                pending = self._run_pending
                self._run_pending = False

                approved = (
                    received and pending and response == "RUN_OK"
                    and not self.estop and not self.recheck
                    and not self._shutdown_event.is_set()
                )
                self._conveyor_running = bool(approved)

            if not approved:
                self._conveyor_running = False
                self._send_arduino("STOP")
                raise RuntimeError(
                    "Arduino RUN 승인 실패: " +
                    str(response or "시간 초과 또는 취소")
                )

            self.stop_alarm()
            self.quality_result = None
            print("[Conveyor] RUN_OK received")

        except Exception:
            with self._run_lock:
                self._run_pending = False
                self._conveyor_running = False
            try:
                self._send_arduino("STOP")
            except Exception:
                pass
            raise

    def set_conveyor_error(self):
        """Stop conveyor, blink red LED, and sound alarm."""
        self._conveyor_running = False
        try:
            self._send_arduino("STOP")
        except (serial.SerialException, OSError, RuntimeError) as exc:
            print(f"[Arduino STOP error] {exc}")
        # Keep the existing alarm/STOP behavior; RGB1 is reserved for inspection results.
        self.start_alarm()

    def set_conveyor_stop(self):
        """Stop actual conveyor and request servo cancellation."""
        self._servo_cancel.set()
        with self._run_lock:
            self._run_pending = False
            self._run_ack.set()
            self._conveyor_running = False
        try:
            self._send_arduino("STOP")
        finally:
            self.stop_alarm()
            self._rgb1_off()

    def start_alarm(self):
        if self._is_alarm_active:
            return
        self._is_alarm_active = True
        self._stop_alarm_event.clear()

        def alarm_worker():
            tone = Tone("A5")
            while not self._stop_alarm_event.is_set():
                try:
                    self.buzzer.play(tone)
                except Exception as exc:
                    print(f"[Buzzer error] {exc}")
                if self._stop_alarm_event.wait(0.25):
                    break
                try:
                    self.buzzer.stop()
                except Exception:
                    pass
                if self._stop_alarm_event.wait(0.15):
                    break
            try:
                self.buzzer.stop()
            except Exception:
                pass

        self._buzzer_thread = threading.Thread(target=alarm_worker, daemon=True)
        self._buzzer_thread.start()

    def stop_alarm(self):
        self._stop_alarm_event.set()
        if self._buzzer_thread and self._buzzer_thread.is_alive():
            self._buzzer_thread.join(timeout=0.7)
        try:
            self.buzzer.stop()
        except Exception:
            pass
        self._is_alarm_active = False

    def set_inspection_ok(self):
        self.quality_result = "OK"
        self._rgb1_pulse((0, 1, 0), "GREEN")

    def set_inspection_ng(self, auto_eject=False):
        self.quality_result = "NG"
        self._rgb1_pulse((1, 0, 0), "RED")
        if auto_eject:
            # Avoid triggering multiple overlapping ejection movements.
            if self._eject_thread is None or not self._eject_thread.is_alive():
                self._eject_thread = threading.Thread(target=self.eject_tire, daemon=True)
                self._eject_thread.start()

    def eject_tire(self):
        """Return True only after the commanded HOME return completes."""
        if not self._servo_lock.acquire(blocking=False):
            raise RuntimeError("다른 서보 동작이 진행 중입니다.")

        completed = False
        try:
            if self.estop or self.recheck or self._conveyor_running:
                raise RuntimeError(
                    "서보 배출 차단: 정지 및 경고 상태 확인 필요"
                )

            self._servo_cancel.clear()

            self.servo.angle = self.HOME_ANGLE
            if self._servo_cancel.wait(0.1) or self.estop:
                return False

            self.servo.angle = self.EJECT_ANGLE

            if self._servo_cancel.wait(self.EJECT_HOLD_TIME) or self.estop:
                return False

            self.servo.angle = self.HOME_ANGLE

            if self._servo_cancel.wait(self.RETURN_WAIT_TIME) or self.estop:
                return False

            completed = True
            return True

        finally:
            try:
                self.servo.detach()
            finally:
                self._servo_lock.release()

    def return_servo(self):
        if not self._servo_lock.acquire(blocking=False):
            raise RuntimeError("다른 서보/START 명령이 실행 중입니다.")

        try:
            if self.estop or self.recheck or self._conveyor_running:
                raise RuntimeError(
                    "서보 복귀 차단: 컨베이어 정지 및 경고 상태를 확인하세요."
                )
            self._servo_cancel.clear()
            self.servo.angle = self.HOME_ANGLE
            self._servo_cancel.wait(0.45)
        finally:
            try:
                self.servo.detach()
            finally:
                self._servo_lock.release()

    def all_off(self):
        """Lights off + buzzer off + actual conveyor STOP."""
        self._conveyor_running = False
        try:
            self._send_arduino("STOP")
        except (serial.SerialException, OSError, RuntimeError) as exc:
            print(f"[Arduino STOP error] {exc}")
        self.stop_alarm()
        self._rgb1_off()
        self.inspection_led.off()

    def close(self):
        if self._closed:
            return
        self._closed = True
        self._rgb1_off()
        self._servo_cancel.set()
        self._conveyor_running = False
        self._shutdown_event.set()
        try:
            self._send_arduino("STOP")
        except Exception as exc:
            print(f"[Arduino STOP on close] {exc}")
        if self._keepalive_thread.is_alive():
            self._keepalive_thread.join(timeout=1)
        if self._serial_reader_thread.is_alive():
            self._serial_reader_thread.join(timeout=1)
        self.stop_alarm()
        if self._eject_thread and self._eject_thread.is_alive():
            self._eject_thread.join(timeout=2)
        try:
            self.servo.angle = self.HOME_ANGLE
            sleep(0.5)
            self.servo.detach()
        except Exception:
            pass
        for hardware in (self.servo, self.buzzer, self.conveyor_led, self.inspection_led):
            try:
                hardware.close()
            except Exception:
                pass
        try:
            self.arduino.close()
        except Exception:
            pass
        print("[Hardware cleanup complete]")


def hardware_test_function():
    # Change to /dev/ttyUSB0 if your Arduino uses that device path.
    hw = TireLineHardware(arduino_port=os.getenv("ARDUINO_PORT", "/dev/ttyUSB0"))
    print("=" * 56)
    print("Tire production line - Pi + Arduino hardware test")
    print("1: RUN conveyor / RGB1 remains OFF")
    print("2: STOP conveyor + buzzer alarm")
    print("3: Inspection OK / green RGB1 for 2 seconds")
    print("4: Inspection NG / red RGB1 for 2 seconds + servo ejection")
    print("5: Servo standalone ejection")
    print("6: Conveyor STOP + all LEDs / alarm off")
    print("Q: Quit safely")
    print("=" * 56)

    try:
        while True:
            cmd = input("Choose 1-6 / Q: ").strip().upper()
            if cmd == "1":
                hw.set_conveyor_normal()
            elif cmd == "2":
                hw.set_conveyor_error()
            elif cmd == "3":
                hw.set_inspection_ok()
            elif cmd == "4":
                hw.set_inspection_ng(auto_eject=True)
            elif cmd == "5":
                hw.eject_tire()
            elif cmd == "6":
                hw.all_off()
            elif cmd == "Q":
                break
            else:
                print("Choose 1-6 or Q")
    except KeyboardInterrupt:
        print("Keyboard interrupt")
    finally:
        hw.close()


if __name__ == "__main__":
    hardware_test_function()