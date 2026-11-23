#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Automated Native-PC Mouse & Keyboard (M&K) + Menu Mouse Verification Suite for The Suffering (USA).

Validates:
  1. Visual UI captures of the Controls -> Mouse & Keyboard tab in both Pre-Game Launcher
     (out/test_mkb/launcher_controls_mkb.png) and In-Game F1 Overlay (out/test_mkb/ingame_f1_controls_mkb.png).
  2. End-to-end zero-controller gameplay (--no-launcher --input-mode mkb) using internal synthetic M&K events
     (--test-mkb + --mkb-telemetry, never touching global OS SendInput):
       - Virtual Port 0 auto-connect + Scheme 1 R3 flashlight hook active
       - Title Screen -> New Game -> Difficulty -> Cutscene Skip -> 1a PrisonDeathRow using Enter/Esc only
       - Cardinal (W, D) & diagonal-normalized (W+D) WASD movement
       - 1px micro-mouse (dx=1, dy=0) and (dx=0, dy=-1) immediate response (0% deadzone)
       - 300px horizontal flick proportionality + zero yaw coasting drift after stop (slew bypass)
       - Vertical pitch polarity: dy < 0 pitches UP when mouse_invert_y=false, DOWN when --mouse-invert-y 1
       - F -> R3 (Flashlight), WheelUp/WheelDown -> D-Pad Left/Right pulses, Tab -> Archives & Esc -> Close,
         Esc -> Pause & second Esc -> Resume
       - ClipCursor capture transitions (true in focused 3D gameplay; false on F1 open, Pause, Focus loss)
  3. Native Menu Mouse & Click-Through Suppression Suite:
       - Exact physical -> 640x480 guest UI coordinate transform self-test across 16:9, 4:3 pillarbox,
         16:10 Steam Deck (+40px letterbox), 21:9 ultrawide (+440px pillarbox), 1024x768 4:3, and Integer scaling
       - LMB advances Opening Movies & "Press Start" Title Screen
       - Dead-space LMB click on Main Menu & Pause Menu does NOT misfire any button
       - Mouse hover updates focused menu widget ONLY when cursor moves (stationary mouse never fights keyboard)
       - Mouse wheel scrolls menu focus cleanly
       - Held-LMB across Pause -> 3D gameplay transition suppresses Fire (pad_rt == 0, suppressed_mask & 1 != 0)
         and re-enables normal Fire (pad_rt == 255) after physical release
       - Held-RMB unpauses Pause Menu & closes Map/Archives cleanly while suppressing ThrowGrenade (pad_lt == 0)
  4. Zero new crash dumps, zero modification of out/build/release/settings.toml (verified by SHA-256 + mtime_ns).
"""

from __future__ import annotations

import ctypes
import hashlib
import json
import math
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
EXE = REPO / "out" / "build" / "release" / "suffering.exe"
EXE_DIR = EXE.parent
SETTINGS_FILE = EXE_DIR / "settings.toml"
LOGS_DIR = EXE_DIR / "logs"
CRASH_DIR = EXE_DIR / "CrashDumps"
OUT_DIR = REPO / "out" / "test_mkb"
WORK_DIR = REPO / ".local" / "mkb_test"
SUMMARY_JSON = WORK_DIR / "mkb_verification_summary.json"

TRANSIENT_DISPLAY_FLAGS = [
    "--display-mode=windowed",
    "--width=1280",
    "--height=720",
    "--fps-cap=60",
    "--vsync=0",
    "--upscaler=fsr1",
    "--upscale-quality=native_aa",
]

VT_MAIN_MENU = 0x0029B4F8
VT_OPTIONS = 0x0029B5F8
VT_PAUSE = 0x0029B690

TH32CS_SNAPPROCESS = 0x00000002


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", ctypes.c_uint32),
        ("cntUsage", ctypes.c_uint32),
        ("th32ProcessID", ctypes.c_uint32),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", ctypes.c_uint32),
        ("cntThreads", ctypes.c_uint32),
        ("th32ParentProcessID", ctypes.c_uint32),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", ctypes.c_uint32),
        ("szExeFile", ctypes.c_wchar * 260),
    ]


def check_no_suffering_running() -> None:
    k32 = ctypes.windll.kernel32
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if snap == ctypes.c_void_p(-1).value:
        return
    try:
        pe = PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(pe)
        if k32.Process32FirstW(snap, ctypes.byref(pe)):
            while True:
                if pe.szExeFile.lower().startswith("suffering"):
                    raise RuntimeError(f"Refusing to run while {pe.szExeFile} (PID {pe.th32ProcessID}) is active")
                if not k32.Process32NextW(snap, ctypes.byref(pe)):
                    break
    finally:
        k32.CloseHandle(snap)


def file_fingerprint(path: Path) -> tuple[bool, str, int]:
    if not path.is_file():
        return (False, "", 0)
    data = path.read_bytes()
    st = path.stat()
    return (True, hashlib.sha256(data).hexdigest(), st.st_mtime_ns)


def bmp_to_png_and_stats(bmp_path: Path, png_path: Path) -> dict[str, Any]:
    data = bmp_path.read_bytes()
    off, = struct.unpack_from("<I", data, 10)
    w, h_raw = struct.unpack_from("<ii", data, 18)
    bpp, = struct.unpack_from("<H", data, 28)
    top_down = h_raw < 0
    h = abs(h_raw)
    stride = ((w * (bpp // 8) + 3) // 4) * 4
    rows = bytearray()
    non_black = 0
    luma_sum = 0.0
    luma_sq = 0.0
    for y in range(h):
        sy = y if top_down else (h - 1 - y)
        ro = off + sy * stride
        rows.append(0)
        for x in range(w):
            px = ro + x * (bpp // 8)
            b, g, r = data[px], data[px + 1], data[px + 2]
            rows.extend((r, g, b))
            if (r | g | b) != 0:
                non_black += 1
            lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
            luma_sum += lum
            luma_sq += lum * lum

    def chunk(tag: bytes, payload: bytes) -> bytes:
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    png_bytes = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(bytes(rows), 6)) + chunk(b"IEND", b"")
    png_path.parent.mkdir(parents=True, exist_ok=True)
    png_path.write_bytes(png_bytes)
    n = max(w * h, 1)
    mean = luma_sum / n
    std = max(0.0, (luma_sq / n) - mean * mean) ** 0.5
    return {"png": str(png_path.relative_to(REPO)), "width": w, "height": h, "non_black_pct": round(100.0 * non_black / n, 2), "luma_stddev": round(std, 2)}


def angle_deg_xz(f1: list[float], f2: list[float]) -> float:
    a1 = math.atan2(f1[0], f1[2])
    a2 = math.atan2(f2[0], f2[2])
    d = math.degrees(a2 - a1)
    while d > 180.0:
        d -= 360.0
    while d < -180.0:
        d += 360.0
    return d


def angle_deg_3d(f1: list[float], f2: list[float]) -> float:
    n1 = math.sqrt(sum(c * c for c in f1))
    n2 = math.sqrt(sum(c * c for c in f2))
    if n1 < 1e-6 or n2 < 1e-6:
        return 0.0
    dot = max(-1.0, min(1.0, sum(a * b for a, b in zip(f1, f2)) / (n1 * n2)))
    return math.degrees(math.acos(dot))


def pitch_deg_from_fwd(f: list[float]) -> float:
    horiz = math.sqrt(f[0] * f[0] + f[2] * f[2])
    return math.degrees(math.atan2(f[1], max(horiz, 1e-6)))


def dist_3d(p1: list[float], p2: list[float]) -> float:
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(p1, p2)))


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return rows


def find_at_ms(rows: list[dict[str, Any]], ms: int) -> dict[str, Any]:
    for r in rows:
        if r.get("ms", 0) >= ms:
            return r
    return rows[-1]


def latest_log_contains(substr: str) -> bool:
    if not LOGS_DIR.is_dir():
        return False
    logs = sorted(LOGS_DIR.glob("*.log"), key=lambda p: p.stat().st_mtime_ns, reverse=True)
    for lg in logs[:3]:
        if substr in lg.read_text(encoding="utf-8", errors="replace"):
            return True
    return False


def build_main_gameplay_script(path: Path) -> None:
    lines = [
        "# Skip intro FMVs & enter New Game -> Normal using Keyboard only",
        "4500 keydown Enter",
        "4700 keyup Enter",
        "8500 keydown Enter",
        "8700 keyup Enter",
        "12500 keydown Enter",
        "12700 keyup Enter",
        "16500 keydown Enter",
        "16700 keyup Enter",
        "20500 keydown Enter",
        "20700 keyup Enter",
        "24500 keydown Enter",
        "24700 keyup Enter",
        "28500 keydown Enter",
        "28700 keyup Enter",
        "32500 keydown Enter",
        "32700 keyup Enter",
        "36000 keydown Esc",
        "36200 keyup Esc",
        "39000 keydown Esc",
        "39200 keyup Esc",
        "42000 keydown Esc",
        "42200 keyup Esc",
        "# By t=45000ms we are standing idle in active 3D gameplay (1a PrisonDeathRow)",
        "# Test 1: 1px micro-mouse horizontal & vertical (zero deadzone proof)",
        "46000 mouse 1 0",
        "46800 mouse 0 -1",
        "# Test 2: 300px horizontal flick right (proportionality + 0 coasting drift after stop)",
        "48000 mouse 300 0",
        "# Test 3: 200px vertical mouse UP (dy=-200) -> verify pitch UP (non-inverted)",
        "49500 mouse 0 -200",
        "51000 mouse 0 200",
        "# Test 4: Cardinal WASD (W, then D) and diagonal (W+D) movement",
        "52500 keydown W",
        "53600 keyup W",
        "54000 keydown D",
        "54900 keyup D",
        "55300 keydown W",
        "55300 keydown D",
        "56300 keyup W",
        "56300 keyup D",
        "# Test 5: F -> R3 (Flashlight), WheelUp/WheelDown -> D-Pad Left/Right",
        "57200 keydown F",
        "57400 keyup F",
        "58000 wheel 1",
        "58700 wheel -1",
        "# Test 6: Tab -> Archives (ui_stack > 0) & Esc -> Close Archives",
        "59600 keydown Tab",
        "59800 keyup Tab",
        "61200 keydown Esc",
        "61400 keyup Esc",
        "# Test 7: Esc -> Pause & second Esc -> Resume",
        "62600 keydown Esc",
        "62800 keyup Esc",
        "64400 keydown Esc",
        "64600 keyup Esc",
        "# Test 8: F1 Overlay open/close & Focus loss/regain cursor capture transitions",
        "66000 overlay 1",
        "67200 overlay 0",
        "68400 focus 0",
        "69600 focus 1",
        "71000 exit",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_invert_y_script(path: Path) -> None:
    lines = [
        "4500 keydown Enter",
        "4700 keyup Enter",
        "8500 keydown Enter",
        "8700 keyup Enter",
        "12500 keydown Enter",
        "12700 keyup Enter",
        "16500 keydown Enter",
        "16700 keyup Enter",
        "20500 keydown Enter",
        "20700 keyup Enter",
        "24500 keydown Enter",
        "24700 keyup Enter",
        "28500 keydown Enter",
        "28700 keyup Enter",
        "32500 keydown Enter",
        "32700 keyup Enter",
        "36000 keydown Esc",
        "36200 keyup Esc",
        "39000 keydown Esc",
        "39200 keyup Esc",
        "42000 keydown Esc",
        "42200 keyup Esc",
        "# With --mouse-invert-y 1, mouse UP (dy=-200) must pitch DOWN",
        "46000 mouse 0 -200",
        "48000 exit",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_menu_mouse_script(path: Path) -> None:
    lines = [
        "# Stage 4: Native Menu Mouse + Click-Through Suppression Suite (1280x720 windowed)",
        "# 1. Advance Opening Movies & 'Press Start' Title Screen using LMB only",
        "4500 menu_cursor 640 360",
        "4600 menu_lmb_down",
        "4750 menu_lmb_up",
        "8500 menu_lmb_down",
        "8650 menu_lmb_up",
        "12500 menu_lmb_down",
        "12650 menu_lmb_up",
        "16500 menu_lmb_down",
        "16650 menu_lmb_up",
        "19500 menu_lmb_down",
        "19650 menu_lmb_up",
        "# 2. Main Menu dead-space click at top-left (20,20) must be a strict no-op",
        "21500 menu_cursor 20 20",
        "21700 menu_lmb_down",
        "21850 menu_lmb_up",
        "# 3. Hover sweep down Main Menu buttons -> updates focus_idx on mouse move",
        "22400 menu_cursor 640 240",
        "22600 menu_cursor 640 280",
        "22800 menu_cursor 640 320",
        "23000 menu_cursor 640 360",
        "23200 menu_cursor 640 400",
        "23400 menu_cursor 640 440",
        "23600 menu_cursor 640 480",
        "23800 menu_cursor 640 520",
        "24000 menu_cursor 640 560",
        "24200 menu_cursor 640 600",
        "# 4. Stationary mouse must NOT fight keyboard ArrowDown / wheel scroll",
        "24700 keydown ArrowDown",
        "24850 keyup ArrowDown",
        "25300 menu_wheel -1",
        "25800 menu_wheel 1",
        "# 5. LMB click Options at (640,420) (fidx=10, zero HDD stall) -> RMB Back to Main Menu!",
        "26200 menu_cursor 640 420",
        "26400 menu_lmb_down",
        "26550 menu_lmb_up",
        "27500 menu_rmb_down",
        "27650 menu_rmb_up",
        "# 6. Wheel up twice from Options (fidx=10 -> 8 -> 4 New Game), start New Game -> Normal, skip cutscene via LMB",
        "28200 menu_wheel 2",
        "28700 keydown Enter",
        "28900 keyup Enter",
        "30000 keydown Enter",
        "30200 keyup Enter",
        "33500 menu_cursor 640 360",
        "33700 menu_lmb_down",
        "33850 menu_lmb_up",
        "37000 menu_lmb_down",
        "37150 menu_lmb_up",
        "40000 menu_lmb_down",
        "40150 menu_lmb_up",
        "# 7. Now in active 3D gameplay -> open Pause Menu -> test dead-space, hover, wheel, held-RMB unpause",
        "46500 keydown Esc",
        "46700 keyup Esc",
        "47500 menu_cursor 20 20",
        "47650 menu_lmb_down",
        "47800 menu_lmb_up",
        "48100 menu_cursor 640 240",
        "48300 menu_cursor 640 300",
        "48500 menu_cursor 640 360",
        "48700 menu_cursor 640 420",
        "48900 menu_wheel -1",
        "49200 menu_wheel 1",
        "# Hold RMB for 600ms across Pause -> 3D transition: must unpause AND keep pad_lt == 0!",
        "49600 menu_rmb_down",
        "50200 menu_rmb_up",
        "# 8. Re-open Pause -> hold LMB across Pause -> 3D transition: must keep pad_rt == 0!",
        "51000 keydown Esc",
        "51200 keyup Esc",
        "51900 menu_cursor 20 20",
        "52000 menu_lmb_down",
        "52150 keydown Enter",
        "52300 keyup Enter",
        "52800 menu_lmb_up",
        "# 9. Fresh LMB press in 3D gameplay after release must fire normally (pad_rt == 255)",
        "53300 mbtn_down MouseLeft",
        "53500 mbtn_up MouseLeft",
        "# 10. Open Map/Archives (Tab) -> wheel -> RMB closes cleanly with pad_lt == 0",
        "54200 keydown Tab",
        "54400 keyup Tab",
        "55300 menu_wheel 1",
        "55600 menu_wheel -1",
        "56000 menu_rmb_down",
        "56500 menu_rmb_up",
        "57400 exit",
    ]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main() -> int:
    check_no_suffering_running()
    if not EXE.is_file():
        print(f"ERROR: Release executable not found: {EXE}")
        return 2

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    WORK_DIR.mkdir(parents=True, exist_ok=True)
    settings_before = file_fingerprint(SETTINGS_FILE)
    dumps_before = set(CRASH_DIR.glob("*.dmp")) if CRASH_DIR.exists() else set()

    summary: dict[str, Any] = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "ui_captures": {},
        "checks": {},
        "menu_mouse_checks": {},
        "errors": [],
    }

    # ------------------------------------------------------------------
    # 1. Visual Captures of Controls -> Mouse & Keyboard Tab
    # ------------------------------------------------------------------
    print("[1/4] Capturing Controls -> Mouse & Keyboard UI (Launcher & In-Game F1)...")
    l_bmp = WORK_DIR / "launcher_controls_mkb.bmp"
    l_png = OUT_DIR / "launcher_controls_mkb.png"
    if l_bmp.exists():
        l_bmp.unlink()
    p = subprocess.run(
        [str(EXE), *TRANSIENT_DISPLAY_FLAGS, "--launcher", "--launcher-tab=4", f"--capture-launcher-bmp={l_bmp}", "--launcher-exit"],
        cwd=str(EXE_DIR),
        timeout=30,
        check=False,
    )
    if p.returncode != 0 or not l_bmp.is_file():
        summary["errors"].append(f"Launcher Controls capture failed (rc={p.returncode})")
    else:
        st = bmp_to_png_and_stats(l_bmp, l_png)
        summary["ui_captures"]["launcher_controls_mkb"] = st
        print(f"      {st['png']}: {st['width']}x{st['height']} non_black={st['non_black_pct']}% std={st['luma_stddev']}")

    f1_bmp = WORK_DIR / "ingame_f1_controls_mkb.bmp"
    f1_png = OUT_DIR / "ingame_f1_controls_mkb.png"
    if f1_bmp.exists():
        f1_bmp.unlink()
    p = subprocess.run(
        [str(EXE), *TRANSIENT_DISPLAY_FLAGS, "--no-launcher", "--open-settings", "--settings-tab=2", f"--capture-ui-bmp={f1_bmp}", "--capture-ui-exit"],
        cwd=str(EXE_DIR),
        timeout=45,
        check=False,
    )
    if p.returncode != 0 or not f1_bmp.is_file():
        summary["errors"].append(f"In-Game F1 Controls capture failed (rc={p.returncode})")
    else:
        st = bmp_to_png_and_stats(f1_bmp, f1_png)
        summary["ui_captures"]["ingame_f1_controls_mkb"] = st
        print(f"      {st['png']}: {st['width']}x{st['height']} non_black={st['non_black_pct']}% std={st['luma_stddev']}")

    # ------------------------------------------------------------------
    # 2. End-to-End Zero-Controller M&K Gameplay Proof (--input-mode mkb)
    # ------------------------------------------------------------------
    print("[2/4] Running zero-controller M&K gameplay test (--input-mode mkb, invert_y=0)...")
    script_main = WORK_DIR / "mkb_main.txt"
    telem_main = WORK_DIR / "mkb_main_telemetry.jsonl"
    if telem_main.exists():
        telem_main.unlink()
    build_main_gameplay_script(script_main)
    check_no_suffering_running()
    p = subprocess.run(
        [
            str(EXE),
            *TRANSIENT_DISPLAY_FLAGS,
            "--no-launcher",
            "--input-mode=mkb",
            "--mouse-sens=1.0",
            "--mouse-invert-y=0",
            f"--test-mkb={script_main}",
            f"--mkb-telemetry={telem_main}",
        ],
        cwd=str(EXE_DIR),
        timeout=110,
        check=False,
    )
    if p.returncode != 0 or not telem_main.is_file():
        summary["errors"].append(f"Main M&K gameplay test failed (rc={p.returncode})")
    else:
        rows = load_jsonl(telem_main)
        in3d_rows = [r for r in rows if r.get("in_3d")]
        summary["checks"]["total_telemetry_rows"] = len(rows)
        summary["checks"]["in_3d_rows"] = len(in3d_rows)
        if not in3d_rows:
            summary["errors"].append("Never entered active 3D gameplay via keyboard-only menu navigation!")
        else:
            pad_conn_all = all(r.get("pad_conn") for r in rows)
            scheme1_ok = any(r.get("scheme1") for r in rows)
            summary["checks"]["port0_auto_connect"] = pad_conn_all
            summary["checks"]["scheme1_flashlight_hook"] = scheme1_ok
            if not pad_conn_all:
                summary["errors"].append("Port 0 did not stay connected in M&K-only mode")
            if not scheme1_ok:
                summary["errors"].append("Scheme 1 R3 flashlight hook was not triggered")

            r_pre_1px_x = find_at_ms(rows, 45800)
            r_post_1px_x = find_at_ms(rows, 46300)
            dx_1px_deg = abs(angle_deg_3d(r_pre_1px_x["cam_fwd"], r_post_1px_x["cam_fwd"]))

            r_pre_1px_y = find_at_ms(rows, 46600)
            r_post_1px_y = find_at_ms(rows, 47100)
            dy_1px_deg = abs(pitch_deg_from_fwd(r_post_1px_y["cam_fwd"]) - pitch_deg_from_fwd(r_pre_1px_y["cam_fwd"]))
            dy_1px_pitch = abs(r_post_1px_y.get("pitch", 0.0) - r_pre_1px_y.get("pitch", 0.0))

            summary["checks"]["micro_1px_yaw_deg"] = round(dx_1px_deg, 5)
            summary["checks"]["micro_1px_pitch_deg"] = round(max(dy_1px_deg, dy_1px_pitch), 5)
            if dx_1px_deg < 1e-4:
                summary["errors"].append(f"1px horizontal mouse produced no rotation ({dx_1px_deg} deg)")
            if max(dy_1px_deg, dy_1px_pitch) < 1e-4:
                summary["errors"].append(f"1px vertical mouse produced no rotation ({dy_1px_deg} / {dy_1px_pitch})")

            r_pre_flick = find_at_ms(rows, 47800)
            r_post_flick_1 = find_at_ms(rows, 48200)
            r_post_flick_2 = find_at_ms(rows, 49100)
            flick_yaw_deg = abs(angle_deg_xz(r_pre_flick["cam_fwd"], r_post_flick_1["cam_fwd"]))
            coast_drift_deg = abs(angle_deg_xz(r_post_flick_1["cam_fwd"], r_post_flick_2["cam_fwd"]))
            summary["checks"]["flick_300px_yaw_deg"] = round(flick_yaw_deg, 3)
            summary["checks"]["flick_yaw_per_count_deg"] = round(flick_yaw_deg / 300.0, 5)
            summary["checks"]["yaw_coast_drift_deg"] = round(coast_drift_deg, 5)
            if flick_yaw_deg < 5.0:
                summary["errors"].append(f"300px horizontal flick too small: {flick_yaw_deg:.3f} deg")
            if coast_drift_deg > 0.25:
                summary["errors"].append(f"Yaw coasting drift detected after mouse stop: {coast_drift_deg:.4f} deg")

            r_pre_up = find_at_ms(rows, 49300)
            r_post_up = find_at_ms(rows, 50200)
            up_pitch_delta = pitch_deg_from_fwd(r_post_up["cam_fwd"]) - pitch_deg_from_fwd(r_pre_up["cam_fwd"])
            up_fwd_y_delta = r_post_up["cam_fwd"][1] - r_pre_up["cam_fwd"][1]
            summary["checks"]["mouse_up_non_inverted_pitch_deg"] = round(up_pitch_delta, 3)
            summary["checks"]["mouse_up_non_inverted_fwd_y_delta"] = round(up_fwd_y_delta, 5)
            summary["checks"]["pitch_per_count_deg"] = round(abs(up_pitch_delta) / 200.0, 5)
            if up_pitch_delta <= 2.0:
                summary["errors"].append(f"Mouse UP (dy=-200, invert_y=0) did not pitch UP: delta={up_pitch_delta:.3f} deg (fwd_y_delta={up_fwd_y_delta:.5f})")

            r_pre_wasd = find_at_ms(rows, 52300)
            r_post_w = find_at_ms(rows, 53800)
            r_post_d = find_at_ms(rows, 55100)
            r_post_wd = find_at_ms(rows, 56600)
            move_w = dist_3d(r_pre_wasd["cam_pos"], r_post_w["cam_pos"])
            move_d = dist_3d(r_post_w["cam_pos"], r_post_d["cam_pos"])
            move_wd = dist_3d(r_post_d["cam_pos"], r_post_wd["cam_pos"])
            diag_rows = [r for r in rows if 55450 <= r.get("ms", 0) <= 56150 and (r.get("pad_lx", 0) != 0 and r.get("pad_ly", 0) != 0)]
            diag_mag_ratio = (
                max(math.hypot(r["pad_lx"], r["pad_ly"]) / 32767.0 for r in diag_rows) if diag_rows else 0.0
            )
            summary["checks"]["wasd_move_w_units"] = round(move_w, 2)
            summary["checks"]["wasd_move_d_units"] = round(move_d, 2)
            summary["checks"]["wasd_move_wd_units"] = round(move_wd, 2)
            summary["checks"]["wasd_diag_stick_norm"] = round(diag_mag_ratio, 4)
            if max(move_w, move_d, move_wd) < 80.0:
                summary["errors"].append(f"WASD movement too small: W={move_w:.1f}, D={move_d:.1f}, WD={move_wd:.1f}")
            if not diag_rows or diag_mag_ratio > 1.02:
                summary["errors"].append(f"WASD diagonal stick normalization failed: ratio={diag_mag_ratio:.4f}")

            r3_seen = any((r.get("pad_btns", 0) & 0x80) != 0 for r in rows if 57100 <= r.get("ms", 0) <= 57600)
            wheel_up_seen = any((r.get("pad_btns", 0) & 0x04) != 0 for r in rows if 57900 <= r.get("ms", 0) <= 58500)
            wheel_dn_seen = any((r.get("pad_btns", 0) & 0x08) != 0 for r in rows if 58600 <= r.get("ms", 0) <= 59300)
            archives_opened = any(
                (not r.get("in_3d")) and (r.get("pause", 0) & 1) != 0 and (r.get("mode", 0) & 4) == 0
                for r in rows if 59900 <= r.get("ms", 0) <= 61100
            )
            archives_closed = any(
                r.get("in_3d") and (r.get("pause", 0) & 1) == 0 and (r.get("mode", 0) & 4) != 0
                for r in rows if 61500 <= r.get("ms", 0) <= 62500
            )
            pause_opened = any((r.get("pause", 0) & 1) != 0 for r in rows if 62900 <= r.get("ms", 0) <= 64300)
            pause_resumed = any((r.get("pause", 0) & 1) == 0 and r.get("in_3d") for r in rows if 64800 <= r.get("ms", 0) <= 65900)

            summary["checks"]["f_r3_flashlight_pulse"] = r3_seen
            summary["checks"]["wheel_up_dpad_left"] = wheel_up_seen
            summary["checks"]["wheel_down_dpad_right"] = wheel_dn_seen
            summary["checks"]["tab_archives_open"] = archives_opened
            summary["checks"]["esc_archives_close"] = archives_closed
            summary["checks"]["esc_pause_open"] = pause_opened
            summary["checks"]["esc_pause_resume"] = pause_resumed

            for key, val in [
                ("f_r3_flashlight_pulse", r3_seen),
                ("wheel_up_dpad_left", wheel_up_seen),
                ("wheel_down_dpad_right", wheel_dn_seen),
                ("tab_archives_open", archives_opened),
                ("esc_archives_close", archives_closed),
                ("esc_pause_open", pause_opened),
                ("esc_pause_resume", pause_resumed),
            ]:
                if not val:
                    summary["errors"].append(f"Action check failed: {key}")

            cap_in_3d = any(r.get("captured") for r in rows if 45000 <= r.get("ms", 0) <= 52000 and r.get("in_3d"))
            cap_released_pause = any((not r.get("captured")) for r in rows if 63100 <= r.get("ms", 0) <= 64200 and (r.get("pause", 0) & 1) != 0)
            cap_released_f1 = any((not r.get("captured")) for r in rows if 66300 <= r.get("ms", 0) <= 67100 and r.get("overlay"))
            cap_released_focus = any((not r.get("captured")) for r in rows if 68700 <= r.get("ms", 0) <= 69500 and not r.get("focused"))
            summary["checks"]["cursor_captured_in_3d"] = cap_in_3d
            summary["checks"]["cursor_released_on_pause"] = cap_released_pause
            summary["checks"]["cursor_released_on_f1"] = cap_released_f1
            summary["checks"]["cursor_released_on_focus_loss"] = cap_released_focus
            for key, val in [
                ("cursor_captured_in_3d", cap_in_3d),
                ("cursor_released_on_pause", cap_released_pause),
                ("cursor_released_on_f1", cap_released_f1),
                ("cursor_released_on_focus_loss", cap_released_focus),
            ]:
                if not val:
                    summary["errors"].append(f"Cursor capture check failed: {key}")

            print(
                f"      1px: yaw={dx_1px_deg:.4f} deg, pitch={max(dy_1px_deg, dy_1px_pitch):.4f} deg | "
                f"300px flick: {flick_yaw_deg:.2f} deg (coast={coast_drift_deg:.4f} deg) | "
                f"UP(dy=-200): {up_pitch_delta:+.2f} deg"
            )
            print(
                f"      WASD: W={move_w:.1f} D={move_d:.1f} WD={move_wd:.1f} diag_norm={diag_mag_ratio:.4f} | "
                f"F={r3_seen} Wheel={wheel_up_seen}/{wheel_dn_seen} Tab/Esc={archives_opened}/{archives_closed} Pause={pause_opened}/{pause_resumed}"
            )
            print(
                f"      Cursor capture: 3D={cap_in_3d} PauseRel={cap_released_pause} F1Rel={cap_released_f1} FocusRel={cap_released_focus}"
            )

    # ------------------------------------------------------------------
    # 3. Inverted-Y Polarity Verification (--mouse-invert-y 1)
    # ------------------------------------------------------------------
    print("[3/4] Verifying --mouse-invert-y 1 polarity (dy=-200 must pitch DOWN)...")
    script_inv = WORK_DIR / "mkb_inv_y.txt"
    telem_inv = WORK_DIR / "mkb_inv_y_telemetry.jsonl"
    if telem_inv.exists():
        telem_inv.unlink()
    build_invert_y_script(script_inv)
    check_no_suffering_running()
    p = subprocess.run(
        [
            str(EXE),
            *TRANSIENT_DISPLAY_FLAGS,
            "--no-launcher",
            "--input-mode=mkb",
            "--mouse-sens=1.0",
            "--mouse-invert-y=1",
            f"--test-mkb={script_inv}",
            f"--mkb-telemetry={telem_inv}",
        ],
        cwd=str(EXE_DIR),
        timeout=90,
        check=False,
    )
    if p.returncode != 0 or not telem_inv.is_file():
        summary["errors"].append(f"Invert-Y M&K test failed (rc={p.returncode})")
    else:
        rows_inv = load_jsonl(telem_inv)
        r_pre = find_at_ms(rows_inv, 45800)
        r_post = find_at_ms(rows_inv, 47000)
        inv_pitch_delta = pitch_deg_from_fwd(r_post["cam_fwd"]) - pitch_deg_from_fwd(r_pre["cam_fwd"])
        summary["checks"]["mouse_up_inverted_pitch_deg"] = round(inv_pitch_delta, 3)
        print(f"      UP(dy=-200, invert_y=1): {inv_pitch_delta:+.2f} deg")
        if inv_pitch_delta >= -2.0:
            summary["errors"].append(f"Mouse UP with --mouse-invert-y 1 did not pitch DOWN: delta={inv_pitch_delta:.3f} deg")

    # ------------------------------------------------------------------
    # 4. Native Menu Mouse & Click-Through Suppression Suite
    # ------------------------------------------------------------------
    print("[4/4] Running Native Menu Mouse & Click-Through Suppression Suite...")
    script_menu = WORK_DIR / "mkb_menu_mouse.txt"
    telem_menu = WORK_DIR / "mkb_menu_mouse_telemetry.jsonl"
    if telem_menu.exists():
        telem_menu.unlink()
    build_menu_mouse_script(script_menu)
    check_no_suffering_running()
    p = subprocess.run(
        [
            str(EXE),
            *TRANSIENT_DISPLAY_FLAGS,
            "--no-launcher",
            "--input-mode=mkb",
            "--mouse-sens=1.0",
            "--mouse-invert-y=0",
            f"--test-mkb={script_menu}",
            f"--mkb-telemetry={telem_menu}",
        ],
        cwd=str(EXE_DIR),
        timeout=95,
        check=False,
    )
    if p.returncode != 0 or not telem_menu.is_file():
        summary["errors"].append(f"Menu Mouse test failed (rc={p.returncode})")
    else:
        mrows = load_jsonl(telem_menu)
        coord_selftest_ok = latest_log_contains("mkb: menu_coord_selftest=PASS")

        # 4a. Reached Main Menu via LMB on movies & Press Start
        main_menu_seen = any(r.get("top_vt") == VT_MAIN_MENU for r in mrows if 18000 <= r.get("ms", 0) <= 26500)
        # 4b. Dead-space click at (20,20) did NOT activate Main Menu button
        r_after_dead = find_at_ms(mrows, 22200)
        main_dead_noop = (r_after_dead.get("top_vt") == VT_MAIN_MENU) and (r_after_dead.get("session") == 1)
        # 4c. Hover sweep across Main Menu updated focus_idx across multiple buttons
        sweep_focus = {r.get("focus_idx") for r in mrows if 22400 <= r.get("ms", 0) <= 24500 and r.get("top_vt") == VT_MAIN_MENU and r.get("focus_idx", -1) >= 0}
        hover_changed = len(sweep_focus) >= 2
        # 4d. Stationary mouse did not override keyboard ArrowDown
        r_pre_kb = find_at_ms(mrows, 24600)
        r_post_kb = find_at_ms(mrows, 25100)
        stationary_kb_ok = (r_post_kb.get("top_vt") == VT_MAIN_MENU) and (r_post_kb.get("focus_idx") != r_pre_kb.get("focus_idx"))
        # 4d2. LMB clicked Options at (640,420) -> VT_OPTIONS and RMB returned to Main Menu
        lmb_opened_submenu = any(r.get("top_vt") == VT_OPTIONS for r in mrows if 26500 <= r.get("ms", 0) <= 27500)
        rmb_returned_main = any(r.get("top_vt") == VT_MAIN_MENU for r in mrows if 27700 <= r.get("ms", 0) <= 29200)
        # 4e. Entered 3D gameplay after cutscene LMB skip
        entered_3d_ok = any(r.get("in_3d") for r in mrows if 40000 <= r.get("ms", 0) <= 46400)
        # 4f. Pause Menu opened, cursor uncaptured, dead-space click was a no-op
        pause_seen = any(r.get("top_vt") == VT_PAUSE and (not r.get("captured")) for r in mrows if 46900 <= r.get("ms", 0) <= 48000)
        r_after_pause_dead = find_at_ms(mrows, 47950)
        pause_dead_noop = (not r_after_pause_dead.get("in_3d")) and (r_after_pause_dead.get("top_vt") == VT_PAUSE)
        # 4g. Held-RMB unpaused Pause Menu cleanly with pad_lt == 0 throughout hold
        rmb_hold_3d_rows = [r for r in mrows if 49700 <= r.get("ms", 0) <= 50150 and r.get("in_3d")]
        rmb_unpause_ok = len(rmb_hold_3d_rows) > 0
        rmb_no_grenade_bleed = rmb_unpause_ok and all(r.get("pad_lt", 0) == 0 for r in rmb_hold_3d_rows)
        # 4h. Held-LMB across Pause -> 3D suppressed Fire (pad_rt == 0), then fresh LMB fired (pad_rt == 255)
        lmb_hold_3d_rows = [r for r in mrows if 52250 <= r.get("ms", 0) <= 52750 and r.get("in_3d")]
        lmb_no_fire_bleed = len(lmb_hold_3d_rows) > 0 and all(r.get("pad_rt", 0) == 0 for r in lmb_hold_3d_rows)
        lmb_fresh_fire_ok = any(r.get("in_3d") and r.get("pad_rt", 0) == 255 for r in mrows if 53300 <= r.get("ms", 0) <= 53650)
        # 4i. RMB closed Map/Archives cleanly back to 3D with pad_lt == 0
        map_rmb_rows = [r for r in mrows if 56100 <= r.get("ms", 0) <= 57100 and r.get("in_3d")]
        map_rmb_close_ok = len(map_rmb_rows) > 0 and all(r.get("pad_lt", 0) == 0 for r in map_rmb_rows)

        mm = summary["menu_mouse_checks"]
        mm["coord_transform_selftest"] = coord_selftest_ok
        mm["lmb_press_start_to_main_menu"] = main_menu_seen
        mm["main_menu_dead_space_noop"] = main_dead_noop
        mm["main_menu_hover_focus_indices"] = sorted(sweep_focus)
        mm["main_menu_hover_selection"] = hover_changed
        mm["stationary_mouse_respects_keyboard"] = stationary_kb_ok
        mm["lmb_button_opens_submenu"] = lmb_opened_submenu
        mm["rmb_cancels_submenu_to_main"] = rmb_returned_main
        mm["cutscene_lmb_skip_to_3d"] = entered_3d_ok
        mm["pause_cursor_visible_uncaptured"] = pause_seen
        mm["pause_dead_space_noop"] = pause_dead_noop
        mm["rmb_unpause_zero_grenade_bleed"] = rmb_no_grenade_bleed
        mm["lmb_resume_zero_fire_bleed"] = lmb_no_fire_bleed
        mm["lmb_fresh_press_fires_after_release"] = lmb_fresh_fire_ok
        mm["rmb_close_map_archives_zero_bleed"] = map_rmb_close_ok

        print(
            f"      CoordSelfTest={coord_selftest_ok} MainMenuLMB={main_menu_seen} DeadSpaceNoop={main_dead_noop}/{pause_dead_noop} "
            f"Hover={sorted(sweep_focus)} StationaryKB={stationary_kb_ok} SubmenuLMB/RMB={lmb_opened_submenu}/{rmb_returned_main}"
        )
        print(
            f"      ClickThrough: HoldLMBZeroFire={lmb_no_fire_bleed} FreshLMBFire={lmb_fresh_fire_ok} "
            f"HoldRMBUnpauseZeroLT={rmb_no_grenade_bleed} MapRMBClose={map_rmb_close_ok}"
        )
        for key, val in [
            ("coord_transform_selftest", coord_selftest_ok),
            ("lmb_press_start_to_main_menu", main_menu_seen),
            ("main_menu_dead_space_noop", main_dead_noop),
            ("main_menu_hover_selection", hover_changed),
            ("stationary_mouse_respects_keyboard", stationary_kb_ok),
            ("lmb_button_opens_submenu", lmb_opened_submenu),
            ("rmb_cancels_submenu_to_main", rmb_returned_main),
            ("cutscene_lmb_skip_to_3d", entered_3d_ok),
            ("pause_cursor_visible_uncaptured", pause_seen),
            ("pause_dead_space_noop", pause_dead_noop),
            ("rmb_unpause_zero_grenade_bleed", rmb_no_grenade_bleed),
            ("lmb_resume_zero_fire_bleed", lmb_no_fire_bleed),
            ("lmb_fresh_press_fires_after_release", lmb_fresh_fire_ok),
            ("rmb_close_map_archives_zero_bleed", map_rmb_close_ok),
        ]:
            if not val:
                summary["errors"].append(f"Menu mouse check failed: {key}")

    # ------------------------------------------------------------------
    # Zero-touch settings.toml & zero crash dump assertions
    # ------------------------------------------------------------------
    settings_after = file_fingerprint(SETTINGS_FILE)
    settings_untouched = settings_before == settings_after
    summary["checks"]["settings_toml_untouched"] = settings_untouched
    if not settings_untouched:
        summary["errors"].append(f"out/build/release/settings.toml was modified during test run! before={settings_before} after={settings_after}")

    dumps_after = set(CRASH_DIR.glob("*.dmp")) if CRASH_DIR.exists() else set()
    new_dumps = sorted(x.name for x in (dumps_after - dumps_before))
    summary["new_crash_dumps"] = new_dumps
    if new_dumps:
        summary["errors"].append(f"New crash dumps detected: {new_dumps}")

    summary["all_passed"] = len(summary["errors"]) == 0
    SUMMARY_JSON.parent.mkdir(parents=True, exist_ok=True)
    SUMMARY_JSON.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\nSummary written to: {SUMMARY_JSON.relative_to(REPO)}")
    if summary["errors"]:
        print("FAILURES:")
        for e in summary["errors"]:
            print("  -", e)
        return 1
    print("=== ALL NATIVE-PC M&K & MENU MOUSE TESTS PASSED ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
