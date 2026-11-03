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
        self._shutdown_event = threading.Event()
        self._conveyor_running = False
        self._stop_alarm_event = threading.Event()
        self._buzzer_thread = None
        self._is_alarm_active = False
        self._servo_lock = threading.Lock()
        self.estop = False
        self.recheck = False
        self.quality_result = None
        self._serial_status_handler = None
        self._last_serial_event = None
        self._eject_thread = None
        self._closed = False

        # Opening the UNO USB port usually resets its sketch.
        # If this port differs, check `ls /dev/ttyACM* /dev/ttyUSB*`.
        self.arduino = serial.Serial(
            arduino_port, baudrate=9600, timeout=0.7, write_timeout=1
        )
        sleep(2.0)
        self._send_arduino("STOP")

        try:
            # RGB 1: conveyor state (R22 G27 B17)
            self.conveyor_led = RGBLED(red=22, green=27, blue=17)
            # RGB 2: inspection state (R23 G24 B25)
            self.inspection_led = RGBLED(red=23, green=24, blue=25)
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

    def set_recheck(self, enabled):
        self.recheck = bool(enabled)
        if self.estop: return
        if enabled:
            self.conveyor_led.blink(on_time=0.3, off_time=0.3,
                on_color=(1, 0, 0), off_color=(0, 0, 0), background=True)
        else:
            if self.quality_result == "NG":
                self.conveyor_led.color = (1, 0, 0)
            elif self._conveyor_running or self.quality_result == "OK":
                self.conveyor_led.color = (0, 1, 0)
            else:
                self.conveyor_led.off()

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
                        self.conveyor_led.off()
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
        """Explicit RUN request: buzzer off, green LED, Arduino conveyor on."""
        self._conveyor_running = False
        if self.estop or self.recheck:
            raise RuntimeError("RUN 차단: 비상정지 또는 재확인 상태")
        self._send_arduino("RUN", expect_reply=True)
        self._conveyor_running = True
        self.stop_alarm()
        self.conveyor_led.color = (1, 0, 0) if self.quality_result == "NG" else (0, 1, 0)
        print("[Conveyor] RUN requested (check D7 stop switch)")

    def set_conveyor_error(self):
        """Stop conveyor, blink red LED, and sound alarm."""
        self._conveyor_running = False
        try:
            self._send_arduino("STOP")
        except (serial.SerialException, OSError, RuntimeError) as exc:
            print(f"[Arduino STOP error] {exc}")
        self.conveyor_led.blink(
            on_time=0.3, off_time=0.3,
            on_color=(1, 0, 0), off_color=(0, 0, 0),
            background=True,
        )
        self.start_alarm()

    def set_conveyor_stop(self):
        """Stop actual conveyor, silence alarm, and turn conveyor LED off."""
        self._conveyor_running = False
        try:
            self._send_arduino("STOP")
        finally:
            self.stop_alarm()
            self.conveyor_led.off()

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
        if not self.estop and not self.recheck:
            self.conveyor_led.color = (0, 1, 0)

    def set_inspection_ng(self, auto_eject=True):
        self.quality_result = "NG"
        if not self.estop and not self.recheck:
            self.conveyor_led.color = (1, 0, 0)
        if auto_eject:
            # Avoid triggering multiple overlapping ejection movements.
            if self._eject_thread is None or not self._eject_thread.is_alive():
                self._eject_thread = threading.Thread(target=self.eject_tire, daemon=True)
                self._eject_thread.start()

    def eject_tire(self):
        with self._servo_lock:
            try:
                if self.estop: return
                self.servo.angle = self.HOME_ANGLE
                sleep(0.1)
                if self.estop: return
                self.servo.angle = self.EJECT_ANGLE
                waited = 0.0
                while waited < self.EJECT_HOLD_TIME and not self.estop:
                    sleep(0.05)
                    waited += 0.05
                if self.estop: return
                self.servo.angle = self.HOME_ANGLE
                sleep(self.RETURN_WAIT_TIME)
                self.servo.detach()
            except Exception as exc:
                print(f"[Servo error] {exc}")

    def return_servo(self):
        with self._servo_lock:
            self.servo.angle = self.HOME_ANGLE
            sleep(0.45)
            self.servo.detach()

    def all_off(self):
        """Lights off + buzzer off + actual conveyor STOP."""
        self._conveyor_running = False
        try:
            self._send_arduino("STOP")
        except (serial.SerialException, OSError, RuntimeError) as exc:
            print(f"[Arduino STOP error] {exc}")
        self.stop_alarm()
        self.conveyor_led.off()
        self.inspection_led.off()

    def close(self):
        if self._closed:
            return
        self._closed = True
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
    hw = TireLineHardware(arduino_port="/dev/ttyACM0")
    print("=" * 56)
    print("Tire production line - Pi + Arduino hardware test")
    print("1: RUN conveyor + green LED / alarm off")
    print("2: STOP conveyor + flashing red LED / buzzer alarm")
    print("3: Inspection OK / green inspection LED")
    print("4: Inspection NG / red inspection LED + servo ejection")
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
