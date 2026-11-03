"""측면 이동 검사 단일 파일 통합본.

실행: python inspect_side_integrated.py
의존 패키지: OpenCV, NumPy. 기존 측면 Python 모듈은 필요하지 않는다.
동일 폴더의 camera_captures 아래 배경/기준 사진 5장은 그대로 필요하다.
상부 YOLO 및 두 카메라 사이 실제 제품 ID 매칭은 포함하지 않는다.
import 시 카메라를 열지 않으며 main() 호출 시 기존 측면 검사를 실행한다.
기존 검사 조건, 화면, 저장 경로 및 추적 정책을 유지한 파일 통합본이다.
"""

# ===== 결과 전달 및 최종 판정 함수 =====

"""같은 라파의 1차 결과 파일과 2차 최종 판정 인터페이스.

2차는 실제로 검사 중인 제품의 product_id를 받아 read_stage1()을 호출한다.
이 모듈은 파일 순서/영상 프레임 순서로 물리 제품을 자동 매칭하지 않는다.
"""
from datetime import datetime
from pathlib import Path
import json
import os
import re
import tempfile

RESULT_DIR = Path(__file__).resolve().parent / "stage1_results"
VALID_RESULTS = {"PASS", "NG", "RECHECK"}


def _validate_id(product_id):
    if not isinstance(product_id, str) or not re.fullmatch(r"[A-Za-z0-9_-]{1,96}", product_id):
        raise ValueError("제품 ID는 영문·숫자·밑줄·하이픈으로 구성해야 합니다.")
    return product_id


def normalize_side_result(reason):
    if reason == "SIDE_PASS":
        return "PASS"
    if reason in {"NG_DAMAGE", "NG_WRONG_TIRE"}:
        return "NG"
    if isinstance(reason, str) and reason.startswith("RECHECK_"):
        return "RECHECK"
    raise ValueError(f"전달할 수 없는 1차 결과: {reason}")


def build_stage1_record(result, product_id):
    _validate_id(product_id)
    verdict = normalize_side_result(result.get("side_result"))
    return {
        "schema_version": 1,
        "product_id": product_id,
        "stage": 1,
        "source": "side_opencv",
        "result": verdict,
        "is_ng": True if verdict == "NG" else False if verdict == "PASS" else None,
        "reason": result["side_result"],
        "pattern": result.get("pattern", "-"),
        "damage": result.get("damage", "-"),
        "created_at": datetime.now().astimezone().isoformat(timespec="milliseconds"),
        "capture_id": product_id,
        "original_image": f"live_results_bg/{product_id}.jpg",
        "result_image": f"live_results_bg/{product_id}_result.jpg",
    }


def write_stage1(result, product_id, directory=None):
    """완성된 JSON만 공개한다. 기존 제품 ID의 다른 결과는 덮어쓰지 않는다."""
    record = build_stage1_record(result, product_id)
    folder = Path(directory) if directory is not None else RESULT_DIR
    folder.mkdir(parents=True, exist_ok=True)
    destination = folder / f"{product_id}.json"
    if destination.exists():
        previous = json.loads(destination.read_text(encoding="utf-8"))
        before = {k: v for k, v in previous.items() if k != "created_at"}
        after = {k: v for k, v in record.items() if k != "created_at"}
        if before != after:
            raise ValueError(f"동일 제품 ID의 결과 충돌: {product_id}")
        return destination
    temporary = None
    try:
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", dir=folder,
                                         prefix=".stage1_", suffix=".tmp", delete=False) as file:
            temporary = Path(file.name)
            json.dump(record, file, ensure_ascii=False, indent=2, allow_nan=False)
            file.write("\n")
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, destination)
        return destination
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def read_stage1(product_id, directory=None):
    """제품 ID로 읽는다. 결과가 아직 없으면 None을 반환한다."""
    _validate_id(product_id)
    folder = Path(directory) if directory is not None else RESULT_DIR
    path = folder / f"{product_id}.json"
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return None
    if (record.get("schema_version") != 1 or record.get("stage") != 1
            or record.get("product_id") != product_id
            or record.get("result") not in VALID_RESULTS):
        raise ValueError(f"잘못된 1차 결과 파일: {path}")
    expected = True if record["result"] == "NG" else False if record["result"] == "PASS" else None
    if record.get("is_ng") is not expected:
        raise ValueError(f"1차 result/is_ng 값 불일치: {path}")
    return record


def combine_results(product_id, stage1_record, stage2_result):
    """2차 담당자가 사용: 불량 우선 OR, 둘 다 통과일 때만 최종 통과."""
    _validate_id(product_id)
    if stage2_result is not None and stage2_result not in VALID_RESULTS:
        raise ValueError("2차 결과는 PASS/NG/RECHECK 또는 None이어야 합니다.")
    if stage1_record is None:
        first = None
    else:
        if stage1_record.get("product_id") != product_id or stage1_record.get("stage") != 1:
            return {"product_id": product_id, "result": "RECHECK", "reason": "PRODUCT_ID_MISMATCH"}
        first = stage1_record.get("result")
        if first not in VALID_RESULTS:
            raise ValueError("1차 결과 형식이 잘못됐습니다.")
    if first == "NG" or stage2_result == "NG":
        verdict, reason = "NG", "DEFECT_IN_STAGE1_OR_STAGE2"
    elif first == "PASS" and stage2_result == "PASS":
        verdict, reason = "PASS", "BOTH_STAGES_PASS"
    else:
        verdict, reason = "RECHECK", "RESULT_MISSING_OR_RECHECK"
    return {"product_id": product_id, "result": verdict, "reason": reason,
            "stage1_result": first, "stage2_result": stage2_result}

# ===== 측면 타이어 파손 검사 =====

import cv2
import numpy as np

def check_damage(crop):
    """현재 샘플의 타이어 몸통에서 내부 노란 휠 노출만 검사한다."""
    h, w = crop.shape[:2]
    if min(h, w) < 20:
        return "RECHECK_SMALL_REGION", []

    # 위쪽에 원래 보이는 휠과 제품 양쪽 여백은 검사 영역에서 제외한다.
    x1, x2 = int(w * 0.12), int(w * 0.88)
    y1, y2 = int(h * 0.25), int(h * 0.85)
    region = crop[y1:y2, x1:x2]
    gray = cv2.cvtColor(region, cv2.COLOR_BGR2GRAY)
    if (gray <= 3).mean() > 0.10 or (gray >= 252).mean() > 0.25:
        return "RECHECK_EXPOSURE", []
    if cv2.Laplacian(gray, cv2.CV_64F).var() < 15:
        return "RECHECK_BLUR", []

    hsv = cv2.cvtColor(region, cv2.COLOR_BGR2HSV)
    yellow = cv2.inRange(
        hsv,
        np.array([12, 100, 65], dtype=np.uint8),
        np.array([38, 255, 255], dtype=np.uint8),
    )
    yellow = cv2.morphologyEx(
        yellow, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8)
    )
    _, _, stats, _ = cv2.connectedComponentsWithStats(yellow)
    rh, rw = region.shape[:2]
    marks = []
    for x, y, bw, bh, area in stats[1:]:
        if (
            bh >= rh * 0.55
            and bw >= max(3, rw * 0.04)
            and area >= rw * rh * 0.015
            and y + bh >= rh * 0.70
        ):
            marks.append((x1 + int(x), y1 + int(y), int(bw), int(bh)))

    if marks:
        return "FOUND", marks
    return "NOT_DETECTED", []

# ===== 배경 차분, 무늬 검사, 화면 및 사진 저장 =====

from pathlib import Path
from datetime import datetime
import argparse
import json

import cv2
import numpy as np


BASE_DIR = Path(__file__).resolve().parent
BACKGROUND_PATH = BASE_DIR / "camera_captures/capture_20261017_124509_763351.png"
REFERENCE_PATHS = {
    "A": BASE_DIR / "live_results/20261017_124138_429391.jpg",
    "B": BASE_DIR / "camera_captures/capture_20261017_124606_761117.png",
}
SAVE_DIR = BASE_DIR / "live_results_bg"
WINDOW = "Live inspection"
RESULT_WINDOW = "Captured inspection result"
ROI = (0.12, 0.12, 0.92, 0.82)
# 현재 원본의 밝기 보정 후 표본: 타이어 S 95백분위 약 132,
# 녹색 벨트 S 5백분위 약 185. 그 사이 값으로 녹색 그림자 복원을 제한한다.
DARK_TIRE_MAX_SATURATION = 140
# 저장 마스크의 휠 단독 높이/폭 약 .20, 현재 조합 기준은 .5 이상.
# 얇은 휠만 검출된 상태를 무늬/파손 검사에 넘기지 않는 보수적 하한이다.
MIN_PRODUCT_HEIGHT_WIDTH_RATIO = 0.35


def resize_frame(image):
    h, w = image.shape[:2]
    if abs(h / w - 9 / 16) > 0.01:
        raise ValueError("카메라 해상도를 1920x1080으로 설정하세요.")
    return cv2.resize(image, (960, 540), interpolation=cv2.INTER_AREA)


def read_image(path):
    if not path.is_file():
        raise FileNotFoundError(f"필요한 사진이 없습니다: {path}")
    image = cv2.imdecode(np.fromfile(str(path), np.uint8), cv2.IMREAD_COLOR)
    if image is None:
        raise ValueError(f"사진을 읽을 수 없습니다: {path}")
    return resize_frame(image)


def desc(p):
    g = cv2.cvtColor(p, cv2.COLOR_BGR2GRAY)
    g = cv2.createCLAHE(2.0, (4, 4)).apply(g)
    g = cv2.resize(g, (128, 64))
    gx = cv2.Sobel(g, cv2.CV_32F, 1, 0, ksize=3)
    gy = cv2.Sobel(g, cv2.CV_32F, 0, 1, ksize=3)
    mag, ang = cv2.cartToPolar(gx, gy, angleInDegrees=True)
    bins = np.clip(((ang % 180) / 20).astype(np.int32), 0, 8)
    fs = []
    for y in range(0, 64, 16):
        for x in range(0, 128, 32):
            hist = np.bincount(
                bins[y : y + 16, x : x + 32].ravel(),
                weights=mag[y : y + 16, x : x + 32].ravel(),
                minlength=9,
            )
            fs.extend(hist / (np.linalg.norm(hist) + 1e-8))
    f = np.array(fs, np.float32)
    return f / (np.linalg.norm(f) + 1e-8)


def variants(crop):
    # 무늬 영역만 반전한다. 파손 검사에는 원본을 사용한다.
    h, w = crop.shape[:2]
    p = crop[int(h * 0.30) : int(h * 0.85), int(w * 0.15) : int(w * 0.85)]
    return np.stack(
        [desc(p), desc(cv2.flip(p, 0)), desc(cv2.flip(p, 1)), desc(cv2.flip(p, -1))]
    )


class Inspector:
    def _locate_original(self, image):
        h, w = image.shape[:2]
        left, top, right, bottom = (
            int(w * ROI[0]),
            int(h * ROI[1]),
            int(w * ROI[2]),
            int(h * ROI[3]),
        )
        # 제품이 들어오지 않는 왼쪽 벽에서 밝기 차이를 구한다.
        shifts = []
        for y1, y2 in [(0.06, 0.14), (0.14, 0.22), (0.22, 0.30)]:
            region = np.s_[int(h * y1) : int(h * y2), int(w * 0.02) : int(w * 0.10)]
            diff = image[region].astype(np.float32) - self.background[region].astype(
                np.float32
            )
            sh = np.median(diff.reshape(-1, 3), axis=0)
            shifts.append(sh)
        shift = np.median(shifts, axis=0)
        if np.max(np.abs(shift)) > 70 or np.max(np.ptp(shifts, axis=0)) > 12:
            return "RECHECK_LIGHT", None, None, shift
        aligned = np.clip(image.astype(np.float32) - shift, 0, 255).astype(np.uint8)
        H, S, V = cv2.split(cv2.cvtColor(aligned, cv2.COLOR_BGR2HSV))
        green = (H > 35) & (H < 105) & (S > 25)
        belt = (H > 35) & (H < 105) & (S > 45) & (V > 25)
        fraction = belt[:, int(w * 0.08) : int(w * 0.92)].mean(1)
        valid = (
            (fraction > 0.35) & (np.arange(h) > h * 0.35) & (np.arange(h) < h * 0.80)
        )
        # 가장 길게 이어진 녹색 구간의 끝을 벨트 앞 경계로 사용한다.
        edges = np.diff(np.r_[False, valid, False].astype(np.int8))
        starts = np.where(edges == 1)[0]
        ends = np.where(edges == -1)[0]
        if not len(starts):
            return "RECHECK_BELT", None, None, shift
        i = int(np.argmax(ends - starts))
        if ends[i] - starts[i] < 15:
            return "RECHECK_BELT", None, None, shift
        bottom = min(bottom, int(ends[i]) - 3)
        diff = cv2.absdiff(
            cv2.GaussianBlur(aligned, (5, 5), 0),
            cv2.GaussianBlur(self.background, (5, 5), 0),
        ).max(2)
        # 녹색 반사를 띤 검은 타이어도 배경보다 충분히 어두우면 유지한다.
        # 기존 국소 복원의 70% 밝기 기준을 최초 마스크에도 적용한다.
        # 벨트 위의 본체도 복원하되 고채도 녹색 그림자는 추가하지 않는다.
        # 휠만 FOUND로 잡히면 국소 복원이 실행되지 않던 경로를 보완한다.
        reference_value = cv2.cvtColor(self.background, cv2.COLOR_BGR2HSV)[:, :, 2]
        dark_foreground = (
            (V.astype(np.float32) < reference_value.astype(np.float32) * 0.70)
            & (S < DARK_TIRE_MAX_SATURATION)
        )
        fg = (diff > 18) & (~green | dark_foreground)
        scope = np.zeros((h, w), bool)
        scope[top:bottom, left:right] = True
        fg &= scope
        if fg[scope].mean() > 0.6:
            return "RECHECK_BACKGROUND", None, np.uint8(fg) * 255, shift
        mask = cv2.morphologyEx(
            np.uint8(fg) * 255, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8)
        )
        mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((7, 25), np.uint8))
        mask[~scope] = 0
        cs, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        cs = [
            c
            for c in cs
            if cv2.contourArea(c) > w * h * 0.0015
            and cv2.boundingRect(c)[2] > w * 0.06
            and cv2.boundingRect(c)[3] > h * 0.015
        ]
        if not cs:
            return "NO_PRODUCT", None, mask, shift
        cs.sort(key=cv2.contourArea, reverse=True)
        points = cs[0]
        remaining = cs[1:]
        while remaining:
            merged = False
            others = []
            for c in remaining:
                x, y, bw, bh = cv2.boundingRect(points)
                xx, yy, ww, hh = cv2.boundingRect(c)
                vo = max(0, min(y + bh, yy + hh) - max(y, yy))
                ho = max(0, min(x + bw, xx + ww) - max(x, xx))
                hg = max(0, x - (xx + ww), xx - (x + bw))
                vg = max(0, y - (yy + hh), yy - (y + bh))
                if (vo > min(bh, hh) * 0.5 and hg < w * 0.12) or (
                    ho > min(bw, ww) * 0.5 and vg < h * 0.1
                ):
                    points = np.concatenate([points, c])
                    merged = True
                else:
                    others.append(c)
            remaining = others
            if not merged:
                break
        x, y, bw, bh = cv2.boundingRect(points)
        px = int(bw * 0.15)
        py = int(bh * 0.12)
        x1, y1 = max(left, x - px), max(top, y - py)
        x2, y2 = min(right, x + bw + px), min(bottom, y + bh + py)
        box = (x1, y1, x2 - x1, y2 - y1)
        status = "FOUND"
        if remaining:
            status = "RECHECK_MULTIPLE"
        elif (bw < w * 0.12 or bh < h * 0.06
              or bh < bw * MIN_PRODUCT_HEIGHT_WIDTH_RATIO):
            status = "RECHECK_OBJECT"
        elif (
            x <= left + 2 or x + bw >= right - 2 or y <= top + 2 or y + bh >= bottom - 2
        ):
            status = "RECHECK_CROP"
        return status, box, mask, shift

    def locate(self, image):
        state, box, mask, shift = self._locate_original(image)
        if state != "RECHECK_CROP" or box is None or mask is None:
            return state, box, mask, shift

        height, width = image.shape[:2]
        left, top, right, bottom = (
            int(width * ROI[0]), int(height * ROI[1]),
            int(width * ROI[2]), int(height * ROI[3]),
        )
        contours, _ = cv2.findContours(
            mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE
        )
        contours = [c for c in contours if cv2.contourArea(c) > width * height * 0.0015]
        if not contours:
            return state, box, mask, shift

        contour = max(contours, key=cv2.contourArea)
        x, y, w, h = cv2.boundingRect(contour)
        # 좌우 또는 위쪽 경계에 닿은 후보는 보정하지 않고 보류한다.
        if x <= left + 2 or x + w >= right - 2 or y <= top + 2:
            return state, box, mask, shift

        shape = np.zeros_like(mask)
        cv2.drawContours(shape, [contour], -1, 255, -1)
        profile = np.count_nonzero(shape[y:y + h, x:x + w], axis=1)
        core_rows = np.flatnonzero(profile >= profile.max() * 0.60)
        if not len(core_rows):
            return state, box, mask, shift
        core_end = y + int(core_rows[-1]) + 1
        # 넓은 본체가 아래끝까지 이어지면 잘라내지 않는다.
        if y + h - core_end < max(10, round(h * 0.06)):
            return state, box, mask, shift

        repair_bottom = min(y + h, core_end + max(8, round(h * 0.04)))
        repair_left = max(left, x - round(w * 0.30))
        repair_right = min(right, x + w + round(w * 0.30))
        repair_top = max(top, y - round(h * 0.05))

        aligned = np.clip(image.astype(np.float32) - shift, 0, 255).astype(np.uint8)
        value = cv2.cvtColor(aligned, cv2.COLOR_BGR2HSV)[:, :, 2]
        reference_value = cv2.cvtColor(self.background, cv2.COLOR_BGR2HSV)[:, :, 2]
        difference = cv2.absdiff(
            cv2.GaussianBlur(aligned, (5, 5), 0),
            cv2.GaussianBlur(self.background, (5, 5), 0),
        ).max(2)
        # 밝은 배경 앞에서 제외된 어두운 제품을 주변에서만 복원한다.
        repair = (
            (reference_value > 115) & (difference > 18)
            & (value.astype(np.float32) < reference_value.astype(np.float32) * 0.70)
        )
        scope = np.zeros_like(mask)
        scope[repair_top:repair_bottom, repair_left:repair_right] = 255
        refined = np.where(
            scope > 0, np.maximum(mask, np.uint8(repair) * 255), 0
        ).astype(np.uint8)
        refined = cv2.morphologyEx(refined, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
        refined = cv2.morphologyEx(refined, cv2.MORPH_CLOSE, np.ones((3, 7), np.uint8))
        contours, _ = cv2.findContours(refined, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        contours = [
            c for c in contours
            if cv2.contourArea(c) > width * height * 0.0015
            and cv2.boundingRect(c)[2] > w * 0.08
        ]
        if not contours:
            return "RECHECK_OBJECT", box, refined, shift
        # 서로 다른 큰 영역이 남으면 재확인한다.
        contours.sort(key=cv2.contourArea, reverse=True)
        if len(contours) > 1 and cv2.contourArea(contours[1]) > cv2.contourArea(contours[0]) * 0.20:
            return "RECHECK_MULTIPLE", box, refined, shift
        xx, yy, ww, hh = cv2.boundingRect(contours[0])
        if hh < ww * MIN_PRODUCT_HEIGHT_WIDTH_RATIO:
            return "RECHECK_OBJECT", box, refined, shift
        px, py = int(ww * 0.15), int(hh * 0.12)
        x1, y1 = max(left, xx - px), max(top, yy - py)
        x2, y2 = min(right, xx + ww + px), min(bottom, yy + hh + py)
        refined_box = (x1, y1, x2 - x1, y2 - y1)
        if xx <= left + 2 or xx + ww >= right - 2 or yy <= top + 2 or yy + hh >= bottom - 2:
            return "RECHECK_CROP", refined_box, refined, shift
        return "FOUND", refined_box, refined, shift

    def __init__(self):
        self.background = read_image(BACKGROUND_PATH)
        self.references = {}
        for label, path in REFERENCE_PATHS.items():
            image = read_image(path)
            state, box, _, _ = self.locate(image)
            if state != "FOUND":
                raise ValueError(f"{label}형 기준 사진 검출 실패: {state}")
            x, y, w, h = box
            self.references[label] = variants(image[y : y + h, x : x + w])
        print("현재 카메라의 기준 사진을 불러왔습니다.")
        print("무늬는 원본·상하 반전·좌우 반전·180도 반전을 비교합니다.")

    def inspect(self, image):
        state, box, mask, shift = self.locate(image)
        if mask is None:
            mask = np.zeros(image.shape[:2], np.uint8)
        result = {
            "side_result": state,
            "damage": "-",
            "pattern": "-",
            "distance_a": None,
            "distance_b": None,
            "box_960": box,
            "marks": [],
            "location": state,
            "light_shift_bgr": [float(v) for v in shift],
            "pattern_flip_invariant": True,
        }
        if state != "FOUND":
            return result, mask
        x, y, w, h = box
        crop = image[y : y + h, x : x + w]
        gray = cv2.cvtColor(crop, cv2.COLOR_BGR2GRAY)
        if cv2.Laplacian(gray, cv2.CV_64F).var() < 15:
            result["side_result"] = "RECHECK_BLUR"
            return result, mask
        damage, marks = check_damage(crop)
        query = variants(crop)
        distances = {}
        for label, reference in self.references.items():
            pairwise = np.linalg.norm(query[:, None, :] - reference[None, :, :], axis=2)
            distances[label] = float(pairwise.min())
        a, b = distances["A"], distances["B"]
        if min(a, b) > 0.55 or abs(a - b) < 0.06:
            pattern = "RECHECK"
        else:
            pattern = "A" if a < b else "B"
        if damage in {"FOUND", "FOUND_DARK"}:
            verdict = "NG_DAMAGE"
        elif damage != "NOT_DETECTED":
            verdict = "RECHECK_DAMAGE"
        elif pattern == "RECHECK":
            verdict = "RECHECK_PATTERN"
        elif pattern == "B":
            verdict = "NG_WRONG_TIRE"
        else:
            verdict = "SIDE_PASS"
        result.update(
            side_result=verdict,
            damage=damage,
            pattern=pattern,
            distance_a=a,
            distance_b=b,
            marks=marks,
        )
        return result, mask


def panel(image, title):
    canvas = np.zeros((320, 400, 3), np.uint8)
    if image is not None and image.size:
        if image.ndim == 2:
            image = cv2.cvtColor(image, cv2.COLOR_GRAY2BGR)
        h, w = image.shape[:2]
        scale = min(400 / w, 290 / h)
        nw, nh = max(1, round(w * scale)), max(1, round(h * scale))
        x, y = (400 - nw) // 2, 30 + (290 - nh) // 2
        canvas[y : y + nh, x : x + nw] = cv2.resize(image, (nw, nh))
    cv2.putText(
        canvas, title, (8, 22), cv2.FONT_HERSHEY_SIMPLEX, 0.5, (255, 255, 255), 1
    )
    return canvas


def make_preview(image, result, mask):
    verdict = result["side_result"]
    color = (
        (0, 255, 0)
        if verdict == "SIDE_PASS"
        else ((0, 0, 255) if verdict.startswith("NG_") else (0, 255, 255))
    )
    overview, crop = image.copy(), None
    if result["box_960"] is not None:
        x, y, w, h = result["box_960"]
        cv2.rectangle(overview, (x, y), (x + w, y + h), color, 2)
        crop = image[y : y + h, x : x + w].copy()
        for bx, by, bw, bh in result["marks"]:
            cv2.rectangle(crop, (bx, by), (bx + bw, by + bh), (0, 0, 255), 2)
    body = np.hstack(
        [
            panel(overview, "Detected product"),
            panel(crop, "Inspection crop"),
            panel(mask, "Foreground - NOT damage mask"),
        ]
    )
    banner = np.zeros((110, 1200, 3), np.uint8)
    a, b = result["distance_a"], result["distance_b"]
    distance = "-" if a is None else f"A={a:.3f} B={b:.3f}"
    lines = [
        verdict,
        f"Damage: {result['damage']} | Pattern: {result['pattern']} | {distance}",
        "4 flip directions | Snapshot only | Use I / S / Q in LIVE window",
    ]
    for i, line in enumerate(lines):
        cv2.putText(
            banner,
            line,
            (10, 25 + i * 32),
            cv2.FONT_HERSHEY_SIMPLEX,
            0.55,
            color if i == 0 else (255, 255, 255),
            1,
        )
    return np.vstack([banner, body])


def save_snapshot(snapshot):
    if snapshot is None:
        print("먼저 I 또는 Space로 검사하세요.")
        return
    capture_id, raw, preview, result = snapshot
    SAVE_DIR.mkdir(parents=True, exist_ok=True)
    original_path = SAVE_DIR / f"{capture_id}.jpg"
    result_path = SAVE_DIR / f"{capture_id}_result.jpg"
    options = [cv2.IMWRITE_JPEG_QUALITY, 95]
    if not cv2.imwrite(str(original_path), raw, options):
        raise OSError("원본 사진 저장 실패")
    if not cv2.imwrite(str(result_path), preview, options):
        raise OSError("결과 사진 저장 실패")
    record = dict(
        result,
        capture_id=capture_id,
        inspection_scope="visible_side_only",
        original_image=original_path.name,
        result_image=result_path.name,
    )
    (SAVE_DIR / f"{capture_id}.json").write_text(
        json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    print(f"저장 완료: {SAVE_DIR}")

# ===== 새 배경/양방향 기준 및 내부 휠 노출 검사 =====

from datetime import datetime
from pathlib import Path
import argparse
import json

import cv2
import numpy as np


BASE_DIR = Path(__file__).resolve().parent
ROI = (0.12, 0.12, 0.92, 0.82)

BACKGROUND = (
    BASE_DIR
    / "camera_captures/empty_belt_20261020_162834_953664.png"
)

REFERENCES = {
    "A": [
        BASE_DIR
        / "camera_captures/empty_belt_20261020_163022_751513.png",
        BASE_DIR
        / "camera_captures/empty_belt_20261020_163031_684657.png",
    ],
    "B": [
        BASE_DIR
        / "camera_captures/empty_belt_20261020_163330_734108.png",
        BASE_DIR
        / "camera_captures/empty_belt_20261020_163049_151216.png",
    ],
}


def check_exposed_wheel(crop):
    h, w = crop.shape[:2]
    if min(h, w) < 20:
        return 'RECHECK_SMALL_REGION', None, []
    x1, x2, y1, y2 = (int(w * 0.12), int(w * 0.88), int(h * 0.25), int(h * 0.85))
    region = crop[y1:y2, x1:x2]
    rh, rw = region.shape[:2]
    yellow = cv2.inRange(cv2.cvtColor(region, cv2.COLOR_BGR2HSV), np.array([12, 100, 65], np.uint8), np.array([38, 255, 255], np.uint8))
    ratio = cv2.countNonZero(yellow) / yellow.size
    clean = cv2.morphologyEx(yellow, cv2.MORPH_OPEN, np.ones((3, 3), np.uint8))
    _, _, stats, _ = cv2.connectedComponentsWithStats(clean)
    marks = [(x1 + int(x), y1 + int(y), int(bw), int(bh)) for x, y, bw, bh, area in stats[1:] if bh >= rh * 0.55 and bw >= max(3, rw * 0.04) and (area >= rw * rh * 0.015) and (y + bh >= rh * 0.7)]
    gray = cv2.cvtColor(region, cv2.COLOR_BGR2GRAY)
    if (gray <= 3).mean() > 0.1 or (gray >= 252).mean() > 0.25:
        status = 'RECHECK_EXPOSURE'
    elif cv2.Laplacian(gray, cv2.CV_64F).var() < 15:
        status = 'RECHECK_BLUR'
    else:
        status = 'EXPOSED_WHEEL_CANDIDATE' if marks else 'NO_EXPOSURE_CANDIDATE'
    return (status, ratio, marks)


class MultiInspector(Inspector):
    def inspect(self, image):
        result, mask = super().inspect(image)
        result["exposure_trial"] = "NOT_RUN"
        result["yellow_ratio"] = None
        result["damage_policy"] = "visible_exposed_wheel_damage_v1"
        if result["location"] != "FOUND" or result["side_result"] == "RECHECK_BLUR":
            return result, mask

        x, y, w, h = result["box_960"]
        status, ratio, marks = check_exposed_wheel(image[y:y+h, x:x+w])
        original = result["damage"]
        result["damage_original"] = original
        result["exposure_trial"] = status
        result["yellow_ratio"] = ratio

        # 화질 불량과 어두운 틈은 재확인으로 유지한다.
        if status.startswith("RECHECK_"):
            result["side_result"] = "RECHECK_DAMAGE"
            result["damage"] = status
            result["marks"] = []
        elif status == "EXPOSED_WHEEL_CANDIDATE":
            # 타이어 파손으로 내부 휠이 노출된 특징이다. 휠 파손 판정은 아니다.
            result["side_result"] = "NG_DAMAGE"
            result["damage"] = "FOUND_EXPOSED_WHEEL"
            result["marks"] = marks
        elif original in {"FOUND_DARK", "SUSPECT"}:
            result["side_result"] = "RECHECK_DAMAGE"
            result["damage"] = "RECHECK_DARK"
        return result, mask

    def __init__(self):
        # 이전 기준 대신 새 환경에서 촬영한 사진을 불러온다.
        self.background = read_image(BACKGROUND)
        self.references = {}

        for label, paths in REFERENCES.items():
            features = []

            for path in paths:
                image = read_image(path)
                state, box, _, _ = self.locate(image)

                if state != "FOUND":
                    raise ValueError(
                        f"{label}형 기준 사진 검출 실패: "
                        f"{state} / {path.name}"
                    )

                x, y, w, h = box
                features.append(
                    variants(image[y:y + h, x:x + w])
                )

            self.references[label] = np.concatenate(
                features, axis=0
            )

        print("새 배경: 2026-10-20 16:28 / 기준: 16:30~16:33 / ROI 아래 설정: 82%")
        print("A/B 모두 실제 양쪽 방향 기준을 불러왔습니다.")
        print("기준 불러오기 성공은 이동 검사 정확도 검증이 아닙니다.")

# ===== 측면 제품 ID 추적 =====

"""측면의 단일 제품 ID 수명 관리. 상부 카메라 매칭은 수행하지 않는다.

오른쪽 진입 2회, 왼쪽 통과 2회로 제품을 구분한다. 단일 상자 검출기를
사용하므로 동시에 여러 제품이 보이거나 추적이 끊기면 세션을 보류한다.
보류 상태는 자동 해제하지 않는다. 벨트를 비우고 프로그램을 다시 시작한다.
"""
from datetime import datetime
from pathlib import Path
import json
import os
import tempfile
import uuid



ENTRY_CENTER_X = 624
EXIT_CENTER_X = 384
EXIT_COUNT = 2
EMPTY_COUNT = 4
MAX_OBSERVATION_GAP = 3.0
# 현재 960x540 검출 ROI의 위쪽 경계: int(540 * .12) = 64.
# 위쪽에 잘린 영역을 진입 확인의 기준 모양으로 사용하지 않는다.
ROI_TOP_FRACTION = 0.12
ROI_RIGHT_FRACTION = 0.92
# 이번 기록에서 부분 진입부터 경계 통과까지 약 6초가 걸렸다.
# 고정 지연이 아니라, 잘린 상태가 계속될 때만 적용하는 대기 상한이다.
MAX_ENTRY_VIEW_WAIT = 15.0


def atomic_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=path.parent,
                                         prefix='.tracking_', suffix='.tmp', delete=False) as file:
            temporary = Path(file.name)
            json.dump(data, file, ensure_ascii=False, indent=2, allow_nan=False)
            file.write('\n')
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


class SideProductTracker:
    def __init__(self, log, save_snapshot, directory=None, result_directory=None):
        self.log = log
        self.save_snapshot = save_snapshot
        self.folder = Path(directory) if directory is not None else Path(__file__).resolve().parent / 'stage1_tracking'
        self.result_directory = result_directory
        self.session_id = datetime.now().strftime('%Y%m%d_%H%M%S_') + uuid.uuid4().hex[:8]
        self.state = 'WAIT_EMPTY'
        self.reason = 'START_WITH_EMPTY_BELT'
        self.product = None
        self.candidate = None
        self.empty_count = 0
        self.exit_count = 0
        self.ordinal = 0
        self.last_box = None
        self.last_seen = None
        self.last_observed = None
        self.last_snapshot = None
        self.confirmed_snapshot = None
        self.unknown_since = None
        self.last_signal = None
        self.checkpoint()
        print('ID 준비: 빈 벨트를 4회 검사한 뒤 READY가 됩니다.')

    @property
    def product_id(self):
        return None if self.product is None else self.product['product_id']

    def checkpoint(self):
        data = {'schema_version': 1, 'session_id': self.session_id,
                'state': self.state, 'reason': self.reason,
                'product_id': self.product_id, 'updated_at': datetime.now().astimezone().isoformat(),
                'matching_enabled': False}
        atomic_json(self.folder / (self.session_id + '_session.json'), data)
        # 상부에서 읽을 수 있는 현재 세션 상태. 대기열로 자동 소비하지 않는다.
        atomic_json(self.folder / 'current_session.json', data)

    def product_checkpoint(self):
        atomic_json(self.folder / 'products' / (self.product_id + '.json'), self.product)

    @staticmethod
    def box(result):
        box = result.get('box_960')
        if result.get('location') not in {'FOUND', 'RECHECK_CROP'} or box is None:
            return None
        if len(box) != 4 or box[2] <= 0 or box[3] <= 0:
            return None
        return tuple(float(v) for v in box)

    @staticmethod
    def center(box):
        return box[0] + box[2] / 2

    @staticmethod
    def continuous(before, after):
        # 튜닝 전 보수적 범위. 이것만으로 제품 동일성이 보장되는 것은 아니다.
        dx = SideProductTracker.center(after) - SideProductTracker.center(before)
        dy = abs((after[1] + after[3]/2) - (before[1] + before[3]/2))
        return (-max(180, before[2]*0.75) <= dx <= max(60, before[2]*0.2)
                and dy <= max(60, before[3]*0.4)
                and 0.5 <= after[2]/before[2] <= 2.0)

    @staticmethod
    def tracking_signal(result, mask=None):
        """검사 판정을 바꾸지 않고 ID용 위치만 추출한다.

        저장 마스크에서 확인한 얇은 벨트 가장자리(h=13px)를 제외한다.
        위치 검출의 MULTIPLE을 무조건 여러 제품으로 해석하지 않는다.
        위아래로 분리된 성분을 묶은 뒤 큰 그룹이 두 개면 보류한다.
        픽셀 기준은 960x540 기준이다.
        """
        if mask is None:
            return {'kind': 'MULTIPLE' if result.get('location') == 'RECHECK_MULTIPLE'
                    else 'EMPTY' if result.get('side_result') == 'NO_PRODUCT'
                    else 'BODY' if SideProductTracker.box(result) is not None else 'UNKNOWN',
                    'box': SideProductTracker.box(result), 'source': 'result_box'}
        import cv2
        if mask.ndim != 2:
            raise ValueError('ID 추적 마스크는 단일 채널이어야 합니다.')
        height, width = mask.shape
        contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        bodies = []
        for contour in contours:
            x, y, w, h = cv2.boundingRect(contour)
            area = cv2.contourArea(contour)
            if h >= max(40, height * 0.08) and area >= width * height * 0.0015:
                bodies.append((area, (x, y, w, h)))
        bodies.sort(reverse=True)
        # v12: 휠/타이어의 위아래 분리를 제품 수로 세지 않는다.
        # 기존 검사기의 세로 병합 조건과 동일하게 가로 겹침 > 50%,
        # 세로 간격 < 영상 높이 10%인 영역만 묶는다.
        # 벨트 진행 방향인 좌우 간격으로는 병합하지 않는다.
        components = [list(box) for _, box in bodies]
        groups = []
        for area, box in bodies:
            groups.append([area, box, 1])
        changed = True
        while changed:
            changed = False
            for i in range(len(groups)):
                if changed:
                    break
                for j in range(i + 1, len(groups)):
                    area, (x, y, w, h), count = groups[i]
                    other_area, (xx, yy, ww, hh), other_count = groups[j]
                    overlap = max(0, min(x+w, xx+ww) - max(x, xx))
                    gap = max(0, y-(yy+hh), yy-(y+h))
                    if overlap > min(w, ww)*0.5 and gap < height*0.1:
                        left, top = min(x, xx), min(y, yy)
                        right, bottom = max(x+w, xx+ww), max(y+h, yy+hh)
                        groups[i] = [area+other_area,
                                     (left, top, right-left, bottom-top),
                                     count+other_count]
                        del groups[j]
                        changed = True
                        break
        groups.sort(key=lambda item: item[0], reverse=True)
        if groups:
            multiple = len(groups) > 1 and groups[1][0] >= groups[0][0]*0.20
            box = groups[0][1]
            return {'kind': 'MULTIPLE' if multiple else 'BODY',
                    'box': box, 'body_count': len(groups),
                    'component_count': len(bodies), 'component_boxes': components,
                    'merged_component_count': groups[0][2],
                    'source': 'mask_vertical_groups_v12',
                    'top_clipped': box[1] <= int(height*ROI_TOP_FRACTION)+2,
                    'right_clipped': box[0]+box[2] >= int(width*ROI_RIGHT_FRACTION)-2}
        # NO_PRODUCT만 빈 벨트 확인 횟수에 포함한다.
        if result.get('side_result') == 'NO_PRODUCT':
            kind = 'EMPTY'
        elif contours and all(cv2.boundingRect(c)[3] < max(40, height*0.08) for c in contours):
            kind = 'THIN_ARTIFACT'
        else:
            kind = 'UNKNOWN'
        return {'kind': kind, 'box': None, 'body_count': 0, 'source': 'mask'}

    def observe(self, result, capture_id, now, mask=None):
        """새 검사 결과마다 1회 호출. 제품이 종료되면 True를 반환한다."""
        if self.state == 'BLOCKED':
            return False
        if self.last_observed is not None and now-self.last_observed > MAX_OBSERVATION_GAP:
            self.block('RECHECK_FRAME_GAP')
            return True
        self.last_observed = now
        signal = self.tracking_signal(result, mask)
        self.last_signal = signal
        empty = signal['kind'] == 'EMPTY'
        self.empty_count = self.empty_count + 1 if empty else 0
        if self.state == 'WAIT_EMPTY':
            if self.empty_count >= EMPTY_COUNT:
                self.state, self.reason = 'READY', 'EMPTY_CONFIRMED'
                self.checkpoint()
                print('ID READY: 제품을 오른쪽에서 투입하세요.')
            return False
        if signal['kind'] == 'MULTIPLE':
            self.block('RECHECK_MULTIPLE_PRODUCTS')
            return True
        box = signal['box']
        if self.product is not None:
            if empty:
                self.exit_count = 0
                if self.empty_count >= EMPTY_COUNT:
                    self.block('RECHECK_TRACK_LOST')
                    return True
                return False
            if box is None:
                self.exit_count = 0
                if now-self.last_seen > MAX_OBSERVATION_GAP:
                    self.block('RECHECK_TRACK_LOST')
                    return True
                return False
            if not self.continuous(self.last_box, box):
                self.block('RECHECK_IDENTITY_JUMP')
                return True
            self.last_box, self.last_seen = box, now
            self.exit_count = (self.exit_count + 1 if signal['kind'] == 'BODY'
                               and self.center(box) < EXIT_CENTER_X else 0)
            if self.exit_count >= EXIT_COUNT:
                self.finish('EXIT_LEFT')
                self.state, self.reason = 'TAIL', 'PREVIOUS_PRODUCT_EXITED'
                self.checkpoint()
                return True
            return False
        if empty:
            self.candidate = None
            self.unknown_since = None
            if self.state == 'TAIL' and self.empty_count >= EMPTY_COUNT:
                self.state, self.reason = 'READY', 'EMPTY_CONFIRMED'
                self.checkpoint()
            return False
        if signal.get('top_clipped') or signal.get('right_clipped'):
            # 부분 진입 중의 크기 증가는 제품 동일성 기준으로 쓰지 않는다.
            # ID 생성 전의 경계 잘림은 위치가 안정될 때까지 제한 시간만 대기한다.
            # ACTIVE 제품의 추적 기준은 바꾸지 않는다.
            self.candidate = None
            signal['entry_action'] = ('WAIT_TOP_CLIP' if signal.get('top_clipped')
                                      else 'WAIT_RIGHT_CLIP')
            if self.unknown_since is None:
                self.unknown_since = now
            if now-self.unknown_since > MAX_ENTRY_VIEW_WAIT:
                self.block('RECHECK_ENTRY_OCCLUDED')
                return True
            return False
        if box is None:
            # 진입 전 얇은 잡음은 ID를 만들지도, 빈 벨트로 세지도 않는다.
            # 불명확한 위치도 한 프레임으로 세션을 막지 않고 제한 시간만 기다린다.
            if signal['kind'] == 'THIN_ARTIFACT' and self.candidate is None:
                self.unknown_since = None
                return False
            if self.unknown_since is None:
                self.unknown_since = now
            if now-self.unknown_since > MAX_OBSERVATION_GAP:
                self.block('RECHECK_UNLOCATED_PRODUCT')
                return True
            return False
        self.unknown_since = None
        center = self.center(box)
        if self.state == 'TAIL' and center < EXIT_CENTER_X:
            return False  # 이전 제품의 왼쪽 잔여 영상
        if center < ENTRY_CENTER_X:
            self.block('RECHECK_MISSED_ENTRY')
            return True
        if self.candidate is None:
            self.candidate = (capture_id, box, now, datetime.now().astimezone().isoformat())
            self.log.write('entry_candidate', capture_id=capture_id, box=box,
                           observed_monotonic_s=now)
            return False
        first_id, first_box, first_seen, entered_at = self.candidate
        if now-first_seen > MAX_OBSERVATION_GAP or not self.continuous(first_box, box):
            self.log.write('entry_candidate_mismatch', first_capture_id=first_id,
                           first_box=first_box, current_capture_id=capture_id,
                           current_box=box, gap_s=now-first_seen)
            self.block('RECHECK_ENTRY_AMBIGUOUS')
            return True
        self.ordinal += 1
        self.product = {'schema_version': 1, 'session_id': self.session_id,
                        'product_id': first_id, 'entry_sequence': self.ordinal,
                        'entered_at': entered_at, 'entry_monotonic_s': first_seen,
                        'status': 'PENDING', 'matching_enabled': False}
        self.state, self.reason = 'ACTIVE', 'ENTRY_CONFIRMED'
        self.last_box, self.last_seen = box, now
        self.last_snapshot = self.confirmed_snapshot = None
        self.exit_count = 0
        self.candidate = None
        self.product_checkpoint()
        self.checkpoint()
        self.log.write('product_enter', **self.product)
        print(f'제품 진입 ID: {self.product_id}')
        return False

    def remember(self, snapshot):
        if self.product is not None:
            self.last_snapshot = (snapshot[0], snapshot[1], snapshot[2], dict(snapshot[3]))

    def confirm(self, snapshot):
        if self.product is None:
            raise ValueError('제품 ID 없이 판정을 확정할 수 없습니다.')
        self.confirmed_snapshot = (snapshot[0], snapshot[1], snapshot[2], dict(snapshot[3]))

    def finish(self, exit_reason, force_recheck=False):
        if self.product is None:
            return
        selected = self.confirmed_snapshot or self.last_snapshot
        if selected is None:
            raise ValueError('활성 제품의 저장 영상이 없습니다.')
        pid = self.product_id
        result = dict(selected[3])
        if force_recheck or self.confirmed_snapshot is None:
            result['side_result_original'] = result.get('side_result')
            result['side_result'] = exit_reason if exit_reason.startswith('RECHECK_') else 'RECHECK_UNCONFIRMED'
        result['product_id'] = pid
        result['stage1_result'] = normalize_side_result(result['side_result'])
        result['session_id'] = self.session_id
        result['entry_sequence'] = self.product['entry_sequence']
        # 기존 이미지 저장 규칙과 전달 JSON의 이미지 경로를 맞춘다.
        if force_recheck or self.confirmed_snapshot is None:
            self.save_snapshot((pid, selected[1], selected[2], result))
        path = write_stage1(result, pid, self.result_directory)
        self.product.update(status='FINAL', result=result['stage1_result'],
                            reason=result['side_result'], exit_reason=exit_reason,
                            exited_at=datetime.now().astimezone().isoformat(),
                            exit_monotonic_s=self.last_observed,
                            result_path=str(path), identity_valid=not force_recheck)
        self.product_checkpoint()
        self.log.write('stage1_export', product_id=pid, result=result['stage1_result'],
                       reason=result['side_result'], exit_reason=exit_reason, path=str(path))
        print(f"1차 전달 파일: {path} / {result['stage1_result']} / {result['side_result']}")
        self.product = None
        self.last_snapshot = self.confirmed_snapshot = None
        self.candidate = None
        self.exit_count = 0

    def block(self, reason):
        if self.state == 'BLOCKED':
            return
        # 먼저 보류 상태를 공개해 결과 파일 쓰기 중에도 상부가 소비하지 못하게 한다.
        self.state, self.reason = 'BLOCKED', reason
        self.checkpoint()
        self.finish(reason, force_recheck=True)
        self.checkpoint()
        self.log.write('tracking_blocked', reason=reason, session_id=self.session_id)
        print(f'ID BLOCKED: {reason} / 자동 연결 금지. 벨트를 비우고 재시작하세요.')

    def close(self, reason='RECHECK_INTERRUPTED'):
        if self.product is not None:
            self.block(reason)
        elif self.state != 'BLOCKED':
            self.state, self.reason = 'CLOSED', 'SESSION_ENDED'
            self.checkpoint()

# ===== 실시간 카메라, 연속 판정, 로그 및 실행 시작점 =====

from datetime import datetime
import time
import threading
import json
import zipfile
from pathlib import Path

import cv2



# 검사 완료 후 다음 검사까지 기다리는 시간이다.
INSPECT_INTERVAL = 0.20

# 같은 판정이 연속으로 나와야 하는 횟수이다.
CONFIRM_COUNT = 3

# 다음 제품을 받기 전에 빈 벨트를 확인하는 횟수이다.
EMPTY_COUNT = 4


CASES = {
    "1": "A_HUB_UP",
    "2": "A_HUB_DOWN",
    "3": "B_HUB_UP",
    "4": "B_HUB_DOWN",
    "5": "BROKEN_TIRE_NORMAL_WHEEL",
    "6": "BROKEN_TIRE_BROKEN_WHEEL",
}


class InspectionLog:
    def __init__(self):
        folder = Path(__file__).resolve().parent / "motion_test_logs"
        folder.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
        self.path = folder / f"motion_{stamp}.jsonl"
        self.file = self.path.open("x", encoding="utf-8")
        self.started = time.monotonic()
        self.case = "UNLABELLED"
        self.block = 0
        self.write(
            "session_start", version="motion_log_v11_entry_boundary_guard",
            direction="RIGHT_TO_LEFT",
            station_center_x=[432, 624], exit_center_x=384,
            exit_confirm_count=2,
            inspect_interval=INSPECT_INTERVAL,
            confirm_count=CONFIRM_COUNT, empty_count=EMPTY_COUNT,
            scope="visible_side_only",
        )
        print(f"시험 로그: {self.path}")

    def write(self, event, **data):
        record = dict(
            time=datetime.now().isoformat(timespec="milliseconds"),
            elapsed_s=round(time.monotonic() - self.started, 6),
            event=event, case=self.case, block=self.block,
            **data,
        )
        self.file.write(json.dumps(record, ensure_ascii=False) + "\n")
        self.file.flush()

    def select(self, key):
        self.case = CASES[key]
        self.block += 1
        self.write("case_start", expected_passes=3)
        print(f"시험 조건: {self.case} / 3회 통과시켜 주세요.")

    def close(self):
        try:
            self.write("session_end")
        finally:
            self.file.close()
        print(f"로그 저장 완료: {self.path}")


class LatestFrame:
    """검사 중에도 카메라를 읽어 최신 영상을 유지한다."""

    def __init__(self, camera):
        self.camera = camera
        self.lock = threading.Lock()
        self.frame = None
        self.sequence = 0
        self.failed = False
        self.stop_event = threading.Event()

        self.thread = threading.Thread(
            target=self.run,
            daemon=True,
        )
        self.thread.start()

    def run(self):
        while not self.stop_event.is_set():
            ok, frame = self.camera.read()

            with self.lock:
                if not ok or frame is None:
                    self.failed = True
                    return

                self.frame = frame
                self.sequence += 1

    def get(self):
        with self.lock:
            if self.failed:
                raise OSError("영상 읽기 실패")

            frame = (
                None
                if self.frame is None
                else self.frame.copy()
            )

            return frame, self.sequence

    def stop(self):
        self.stop_event.set()
        self.thread.join(timeout=2)


class ResultGate:
    """제품이 나간 뒤 다음 제품의 자동 저장을 허용한다."""

    def __init__(self):
        self.saved = False
        self.last = None
        self.count = 0
        self.empty_count = 0
        self.exit_count = 0
        self.reset_reason = None

    def update(self, result):
        self.reset_reason = None
        verdict = result["side_result"]

        if verdict == "NO_PRODUCT":
            self.empty_count += 1
            self.last = None
            self.count = 0

            if self.empty_count >= EMPTY_COUNT:
                if self.saved:
                    self.reset_reason = "EMPTY_CONFIRMED"
                self.saved = False
                self.exit_count = 0

            return False

        self.empty_count = 0

        box = result.get("box_960")
        full_box = result.get("location") == "FOUND" and box is not None
        center = None if not full_box else box[0] + box[2] / 2

        if self.saved:
            if center is not None and center < 384:
                self.exit_count += 1
            else:
                self.exit_count = 0
            if self.exit_count >= 2:
                self.saved = False
                self.last = None
                self.count = 0
                self.exit_count = 0
                self.reset_reason = "PRODUCT_PASSED_STATION_LEFT"
            return False

        if center is None or not (432 <= center <= 624):
            self.last = None
            self.count = 0
            return False

        accepted = verdict in {
            "SIDE_PASS",
            "NG_WRONG_TIRE",
            "NG_DAMAGE",
        }

        accepted = accepted or (
            verdict == "RECHECK_DAMAGE"
            and result["damage"] in {"RECHECK_DARK", "RECHECK_EXPOSED_WHEEL"}
        )

        if not accepted:
            self.last = None
            self.count = 0
            return False

        key = (
            verdict,
            result["damage"],
            "-" if result["damage"] in {"RECHECK_EXPOSED_WHEEL", "FOUND_EXPOSED_WHEEL"} else result["pattern"],
        )

        if key == self.last:
            self.count += 1
        else:
            self.last = key
            self.count = 1

        return self.count >= CONFIRM_COUNT

    def mark_saved(self):
        self.saved = True
        self.last = None
        self.count = 0


class CropEvidence:
    """조건 키와 무관하게 중앙 구간의 잘림·다중 영역 보류를 수집한다."""

    def __init__(self, log):
        self.log = log
        self.folder = log.path.parent / (log.path.stem + "_evidence")
        self.last_time = -float("inf")
        self.empty_count = 0
        self.pass_count = 0
        self.total_count = 0
        self.case = log.case

    def update(self, image, mask, result, capture_id, now):
        if self.case != self.log.case:
            self.case = self.log.case
            self.empty_count = 0
            self.pass_count = 0
            self.last_time = -float("inf")
        if result["side_result"] == "NO_PRODUCT":
            self.empty_count += 1
            if self.empty_count >= EMPTY_COUNT:
                self.pass_count = 0
            return
        self.empty_count = 0
        box = result.get("box_960")
        if result.get("location") not in {"RECHECK_CROP", "RECHECK_MULTIPLE"} or box is None:
            return
        center = box[0] + box[2] / 2
        if not (432 <= center <= 624):
            return
        if self.pass_count >= 8 or self.total_count >= 64 or now-self.last_time < 0.75:
            return
        self.folder.mkdir(parents=True, exist_ok=True)
        image_path = self.folder / f"{capture_id}_image.jpg"
        mask_path = self.folder / f"{capture_id}_mask.png"
        if not cv2.imwrite(str(image_path), image, [cv2.IMWRITE_JPEG_QUALITY, 92]):
            raise OSError("진단 원본 저장 실패")
        if not cv2.imwrite(str(mask_path), mask):
            raise OSError("진단 마스크 저장 실패")
        self.pass_count += 1
        self.total_count += 1
        self.last_time = now
        self.log.write("crop_evidence", capture_id=capture_id,
                       image=image_path.name, mask=mask_path.name,
                       diagnostic_only=True, frame_size=[960, 540],
                       location=result.get("location"))

    def archive(self):
        archive_path = self.log.path.with_name(self.log.path.stem + "_diagnostic.zip")
        with zipfile.ZipFile(archive_path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.write(self.log.path, self.log.path.name)
            if self.folder.exists():
                for path in sorted(self.folder.iterdir()):
                    archive.write(path, "evidence/" + path.name)
        print(f"보낼 진단 ZIP: {archive_path}")
        print(f"잘림·다중 영역 보류 진단 사진: {self.total_count}장 (검사 결과 자동 저장과 별도)")

    def save_tracking(self, image, mask, capture_id, tracker, phase):
        """진입 후보/최초 보류 프레임을 별도 저장한다. 판정에는 사용하지 않는다."""
        if self.total_count >= 64:
            return
        self.folder.mkdir(parents=True, exist_ok=True)
        image_path = self.folder / f"{capture_id}_tracking_image.jpg"
        mask_path = self.folder / f"{capture_id}_tracking_mask.png"
        if not cv2.imwrite(str(image_path), image, [cv2.IMWRITE_JPEG_QUALITY, 92]):
            raise OSError('ID 진단 원본 저장 실패')
        if not cv2.imwrite(str(mask_path), mask):
            raise OSError('ID 진단 마스크 저장 실패')
        self.total_count += 1
        self.log.write('tracking_evidence', capture_id=capture_id, phase=phase,
                       image=image_path.name, mask=mask_path.name,
                       state=tracker.state, reason=tracker.reason,
                       signal=tracker.last_signal, diagnostic_only=True)


def save_tracking_snapshot(snapshot):
    # 판정 보류로 내보내는 증거 영상에 최종 전달 상태를 명시한다.
    capture_id, raw, preview, result = snapshot
    preview = preview.copy()
    cv2.rectangle(preview, (0, 0), (preview.shape[1], 58), (0, 0, 0), -1)
    cv2.putText(preview, "STAGE1: " + result["side_result"], (10, 23),
                cv2.FONT_HERSHEY_SIMPLEX, 0.55, (0, 255, 255), 1)
    cv2.putText(preview, "ID: " + capture_id, (10, 48),
                cv2.FONT_HERSHEY_SIMPLEX, 0.5, (0, 255, 255), 1)
    save_snapshot((capture_id, raw, preview, result))


def main():
    inspector = MultiInspector()
    gate = ResultGate()

    camera = cv2.VideoCapture(
        "/dev/video0",
        cv2.CAP_V4L2,
    )

    stream = None
    log = None
    evidence = None
    tracker = None

    try:
        if not camera.isOpened():
            raise OSError(
                "카메라를 열지 못했습니다. "
                "다른 카메라 프로그램을 종료하세요."
            )

        camera.set(
            cv2.CAP_PROP_FOURCC,
            cv2.VideoWriter_fourcc(*"MJPG"),
        )
        camera.set(cv2.CAP_PROP_FRAME_WIDTH, 1920)
        camera.set(cv2.CAP_PROP_FRAME_HEIGHT, 1080)
        camera.set(cv2.CAP_PROP_FPS, 15)
        camera.set(cv2.CAP_PROP_BUFFERSIZE, 1)

        log = InspectionLog()
        evidence = CropEvidence(log)
        tracker = SideProductTracker(log, save_tracking_snapshot)

        next_check = 0.0
        snapshot = None
        result = None
        active = True
        processing_ms = 0.0

        stream = LatestFrame(camera)
        last_sequence = -1

        print("이동 중 자동 검사를 시작합니다.")
        print("숫자 선택 없이도 중앙 구간의 잘림·다중 영역 보류를 수집하고 Q 종료 시 ZIP으로 묶습니다.")
        print("같은 판정이 3번 연속 나오면 한 번 자동 저장합니다.")
        print("오른쪽에서 왼쪽으로 이동하는 벨트 시험용입니다.")
        print("오른쪽 진입 시 ID 생성, 중앙 3회 판정 시 영상 저장, 왼쪽 통과 시 1차 결과 전달")
        print("시작 전 빈 벨트를 확인하세요. ID BLOCKED이면 벨트를 비우고 프로그램을 재시작하세요.")
        print("내부 휠 노출 파손은 NG_DAMAGE, 어두운 틈은 RECHECK_DAMAGE로 저장합니다.")
        print("P: 검사 일시정지/재개 | S: 수동 저장 | Q: 종료")

        print("조건 선택: 1 A기둥위 | 2 A기둥아래 | 3 B기둥위 | 4 B기둥아래")
        print("5 파손타이어+정상휠 | 6 파손타이어+파손휠")
        print("빈 벨트에서 조건 키를 누르고 각 조건을 3회씩 통과시키세요.")

        while True:
            raw, sequence = stream.get()

            if raw is None:
                key = cv2.waitKey(10) & 255

                if key in [27, ord("q"), ord("Q")]:
                    break

                continue

            image = resize_frame(raw)
            now = time.monotonic()

            if (
                active
                and now >= next_check
                and sequence != last_sequence
            ):
                last_sequence = sequence
                started = time.monotonic()

                result, mask = inspector.inspect(image)
                # 어두운 틈만으로 파손을 확정하지 않는다.
                if result["damage"] in {"FOUND_DARK", "SUSPECT"}:
                    result["damage_original"] = result["damage"]
                    result["damage"] = "RECHECK_DARK"
                    result["side_result"] = "RECHECK_DAMAGE"
                preview = make_preview(image, result, mask)

                processing_ms = (
                    time.monotonic() - started
                ) * 1000

                next_check = (
                    time.monotonic() + INSPECT_INTERVAL
                )

                capture_id = datetime.now().strftime(
                    "%Y%m%d_%H%M%S_%f"
                )

                previous_state = tracker.state
                had_candidate = tracker.candidate is not None
                ended = tracker.observe(result, capture_id, time.monotonic(), mask=mask)
                if tracker.state == 'BLOCKED' and previous_state != 'BLOCKED':
                    evidence.save_tracking(image, mask, capture_id, tracker, 'BLOCKED')
                elif not had_candidate and tracker.candidate is not None:
                    evidence.save_tracking(image, mask, capture_id, tracker, 'CANDIDATE')
                if ended:
                    gate = ResultGate()
                if tracker.product_id is not None:
                    result["product_id"] = tracker.product_id
                    result["session_id"] = tracker.session_id

                snapshot = (
                    capture_id,
                    raw.copy(),
                    preview,
                    result,
                )

                tracker.remember(snapshot)
                cv2.imshow(RESULT_WINDOW, preview)

                saved_before = gate.saved
                should_save = (gate.update(result)
                               if tracker.product_id is not None and not ended else False)
                log.write(
                    "inspection", capture_id=capture_id,
                    sequence=sequence,
                    processing_ms=round(processing_ms, 3),
                    result=result,
                    stable_count=gate.count,
                    empty_count=gate.empty_count,
                    saved_before=saved_before,
                    saved_after_update=gate.saved,
                    auto_save_due=should_save,
                    gate_reset_reason=gate.reset_reason,
                    id_tracking_state=tracker.state,
                    id_tracking_signal=tracker.last_signal,
                )

                evidence.update(image, mask, result, capture_id, time.monotonic())

                if should_save:
                    log.write("auto_save_attempt", capture_id=capture_id)
                    result["product_id"] = tracker.product_id
                    result["stage1_result"] = normalize_side_result(result["side_result"])
                    auto_snapshot = (tracker.product_id, snapshot[1], preview, result)
                    save_snapshot(auto_snapshot)
                    tracker.confirm(auto_snapshot)
                    print(f"영상 저장 ID: {tracker.product_id} / 결과 전달은 왼쪽 통과 때 실행")
                    gate.mark_saved()
                    log.write(
                        "auto_saved", capture_id=capture_id,
                        product_id=tracker.product_id,
                        side_result=result["side_result"],
                        pattern=result["pattern"],
                    )

                    print(
                        "자동 저장:",
                        result["side_result"],
                        result["pattern"],
                    )

            display = image.copy()
            shade = display.copy()

            cv2.rectangle(
                shade,
                (0, 0),
                (960, 105),
                (0, 0, 0),
                -1,
            )

            display = cv2.addWeighted(
                shade, 0.7,
                display, 0.3,
                0,
            )

            status = (
                "WAIT"
                if result is None
                else result["side_result"]
            )

            mode = "AUTO" if active else "PAUSED"

            lines = [
                f"{mode} | {log.case} | Last result: {status}",
                (
                    f"Stable: {gate.count}/{CONFIRM_COUNT}"
                    f" | Saved: {gate.saved}"
                    f" | Process: {processing_ms:.0f}ms"
                    f" | ID: {tracker.state}"
                ),
                "P pause/resume | S save current inspection | Q quit",
            ]

            for row, line in enumerate(lines):
                cv2.putText(
                    display,
                    line,
                    (10, 25 + row * 30),
                    cv2.FONT_HERSHEY_SIMPLEX,
                    0.55,
                    (0, 255, 255),
                    1,
                )

            cv2.imshow(WINDOW, display)
            key = cv2.waitKey(1) & 255

            if key in [27, ord("q"), ord("Q")]:
                break

            if cv2.getWindowProperty(
                WINDOW,
                cv2.WND_PROP_VISIBLE,
            ) < 1:
                break

            if chr(key) in CASES:
                log.select(chr(key))

            elif key in [ord("p"), ord("P")]:
                active = not active
                log.write("mode_change", active=active)
                if not active:
                    tracker.block("RECHECK_PAUSED")

                gate.last = None
                gate.count = 0

                print(
                    "검사 재개"
                    if active
                    else "검사 일시정지"
                )

            elif key in [ord("s"), ord("S")]:
                if snapshot is not None:
                    log.write("manual_save_attempt", capture_id=snapshot[0])
                save_snapshot(snapshot)
                if snapshot is not None:
                    log.write("manual_saved", capture_id=snapshot[0])

    except Exception as error:
        if log is not None:
            log.write("error", message=str(error), error_type=type(error).__name__)
        raise
    finally:
        try:
            if tracker is not None:
                tracker.close()
        finally:
            if stream is not None:
                stream.stop()
            camera.release()
            cv2.destroyAllWindows()
            if log is not None:
                log.close()
                if evidence is not None:
                    evidence.archive()


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, cv2.error) as error:
        print(f"실행 오류: {error}")
