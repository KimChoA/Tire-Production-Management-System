# test_pi.py (가상 라즈베리파이 하트비트 송신기)
import socket
import time

SERVER_IP = "127.0.0.1"  # 초아님 서버 IP (로컬이면 127.0.0.1)
SERVER_PORT = 5000

print(f"[라즈베리파이 시뮬레이터] 서버({SERVER_IP}:{SERVER_PORT}) 연결 시도...")
client = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
client.connect((SERVER_IP, SERVER_PORT))
print("[연결 성공] 2초마다 하트비트 신호를 전송합니다. (종료하려면 Ctrl+C)")

try:
    while True:
        # ⭐️ 2초마다 하트비트 패킷 전송 (끝에 \n 필수!)
        packet = '{"type":"HEARTBEAT","equipment":"RASPBERRY_PI","status":"ALIVE"}\n'
        client.sendall(packet.encode('utf-8'))
        print(">> [송신] HEARTBEAT: ALIVE")
        time.sleep(2)
except KeyboardInterrupt:
    print("\n[테스트] 라즈베리파이 전원이 꺼졌습니다! (송신 중단)")
    client.close()