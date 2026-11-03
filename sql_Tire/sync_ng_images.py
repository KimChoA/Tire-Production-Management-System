import sqlite3
import urllib.request
import urllib.error
import getpass
import re
import os
from pathlib import Path

DB = Path(r"C:\sqlite\production.db")
PI = "http://192.168.0.209:5001"

if not DB.exists():
    raise SystemExit("SQLite DB 파일이 없습니다.")

token = os.environ.get("PI_CONTROL_TOKEN", "").strip()
if not token:
    raise SystemExit("토큰이 비어 있습니다.")

con = sqlite3.connect(str(DB), timeout=10)

try:
    columns = {
        row[1]
        for row in con.execute(
            "PRAGMA table_info(inspection)"
        )
    }

    for col in ("yolo_image_path", "opencv_image_path"):
        if col not in columns:
            con.execute(
                f"ALTER TABLE inspection ADD COLUMN {col} TEXT"
            )

    con.commit()

    records = con.execute("""
        SELECT id, tire_id
        FROM inspection
        WHERE result = 'NG'
          AND data_source = 'HARDWARE'
        ORDER BY id DESC
        LIMIT 200
    """).fetchall()

    saved = 0

    for row_id, tire_id in records:
        if not re.fullmatch(
            r"TIRE-[A-Za-z0-9_-]{1,64}", tire_id
        ):
            continue

        # 타이어 ID에서 Pi의 로컬 검사 날짜 추출
        match = re.match(
            r"TIRE-(\d{4})(\d{2})(\d{2})-", tire_id
        )
        if not match:
            continue

        day = "-".join(match.groups())

        for camera in ("yolo", "opencv"):
            column = camera + "_image_path"

            old = con.execute(
                f"SELECT {column} FROM inspection WHERE id=?",
                (row_id,)
            ).fetchone()[0]

            if old:
                existing = DB.parent / Path(old)
                if existing.is_file():
                    continue

            url = (
                f"{PI}/api/ng-image/"
                f"{tire_id}/{camera}"
            )

            req = urllib.request.Request(
                url,
                headers={"X-Line-Token": token}
            )

            try:
                with urllib.request.urlopen(
                    req, timeout=5
                ) as response:
                    if "image/jpeg" not in response.headers.get(
                        "Content-Type", ""
                    ):
                        continue
                    data = response.read(
                        12 * 1024 * 1024 + 1
                    )

            except urllib.error.HTTPError as e:
                if e.code == 404:
                    continue
                print(
                    f"[HTTP {e.code}] {tire_id} {camera}"
                )
                continue

            except Exception as e:
                print(
                    f"[연결 실패] {tire_id} {camera}: {e}"
                )
                continue

            if (
                not data.startswith(b"\xff\xd8\xff")
                or len(data) > 12 * 1024 * 1024
            ):
                print("[JPEG 검증 실패]", tire_id, camera)
                continue

            relative = (
                Path("inspection_images")
                / day
                / tire_id
                / (camera + ".jpg")
            )

            destination = DB.parent / relative
            destination.parent.mkdir(
                parents=True, exist_ok=True
            )

            temporary = destination.with_suffix(
                ".jpg.tmp"
            )
            temporary.write_bytes(data)
            os.replace(temporary, destination)

            con.execute(
                f"UPDATE inspection SET {column}=? WHERE id=?",
                (relative.as_posix(), row_id)
            )
            con.commit()

            saved += 1
            print("[저장 완료]", destination)

    print()
    print("검사한 NG 기록:", len(records), "건")
    print("새로 동기화한 사진:", saved, "장")
    print("저장 폴더:", DB.parent / "inspection_images")

finally:
    con.close()

