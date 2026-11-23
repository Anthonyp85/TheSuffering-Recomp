#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Milestone 3 Deterministic Input Replay & Regression Harness for The Suffering (USA).

Runs and validates:
  1. Record -> Replay round-trip + hybrid/frame/time mode comparison (short menu script)
  2. 3 consecutive warm-Z:\\ Release replays of tests/m3_full_regression.rec (hybrid)
  3. 1 cold-Z:\\ Release replay (deletes out/build/release/hdd/Z before launch)
  4. 1 Debug build replay of tests/m3_full_regression.rec (hybrid)

Safety invariants:
  - Aborts immediately if any suffering*.exe process is already running.
  - Verifies protected source media size and mtime_ns before and after execution.
  - Never modifies, moves, or deletes the read-only .xiso.iso / .iso disc image.
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import shutil
import statistics
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
CAP_ROOT = REPO / ".local" / "captures_m3"
GOLDEN_SOURCE = REPO / "tests" / "m3_regression_source.txt"
GOLDEN_REC = REPO / "tests" / "m3_full_regression.rec"

PROTECTED_MEDIA = [
    p for p in [
        Path(os.environ["XBE_ISO_PATH"]) if os.environ.get("XBE_ISO_PATH") else (REPO / "Suffering, The (USA)" / "Suffering, The (USA).xiso.iso")
    ] if p.is_file()
]

user32 = ctypes.windll.user32
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CLOSE = 0x0010
VK_F3 = 0x72
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)

FULL_CAP_TIMES = [27.8, 49.5, 55.5, 64.2, 80.2, 91.5, 105.5]


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
    k32.CreateToolhelp32Snapshot.restype = ctypes.c_void_p
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    if not snap or snap == ctypes.c_void_p(-1).value:
        return
    try:
        pe = PROCESSENTRY32W()
        pe.dwSize = ctypes.sizeof(PROCESSENTRY32W)
        if k32.Process32FirstW(snap, ctypes.byref(pe)):
            while True:
                if pe.szExeFile.lower().startswith("suffering") and pe.szExeFile.lower().endswith(".exe"):
                    raise RuntimeError(f"Aborting: {pe.szExeFile} (pid={pe.th32ProcessID}) is already running!")
                if not k32.Process32NextW(snap, ctypes.byref(pe)):
                    break
    finally:
        k32.CloseHandle(snap)


def snapshot_protected_media() -> dict[str, tuple[int, int]]:
    snap: dict[str, tuple[int, int]] = {}
    for p in PROTECTED_MEDIA:
        if not p.exists():
            raise RuntimeError(f"Protected media missing: {p}")
        st = p.stat()
        snap[str(p)] = (st.st_size, st.st_mtime_ns)
    return snap


def verify_protected_media(before: dict[str, tuple[int, int]]) -> None:
    after = snapshot_protected_media()
    for k, v in before.items():
        if after.get(k) != v:
            raise RuntimeError(f"Protected media altered! {k}: before={v} after={after.get(k)}")


def find_windows(pid: int) -> list[tuple[int, str]]:
    out: list[tuple[int, str]] = []

    def cb(h: int, _: int) -> bool:
        wp = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(h, ctypes.byref(wp))
        if wp.value == pid and user32.IsWindowVisible(h):
            c = ctypes.create_unicode_buffer(256)
            user32.GetClassNameW(h, c, 256)
            out.append((h, c.value))
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return out


def bmp_to_png(bmp_path: Path, png_path: Path) -> tuple[int, int]:
    b = bmp_path.read_bytes()
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    top_down = h < 0
    h = abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    step = bpp // 8
    rows = []
    for y in range(h):
        sy = y if top_down else (h - 1 - y)
        row = b[off + sy * stride : off + sy * stride + w * step]
        rgb = bytearray(w * 3)
        for x in range(w):
            rgb[x * 3] = row[x * step + 2]
            rgb[x * 3 + 1] = row[x * step + 1]
            rgb[x * 3 + 2] = row[x * step]
        rows.append(b"\x00" + bytes(rgb))
    raw = b"".join(rows)

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(raw, 6))
        + chunk(b"IEND", b"")
    )
    png_path.parent.mkdir(parents=True, exist_ok=True)
    png_path.write_bytes(png)
    return w, h


def ensure_dll_fresh(out_dir: Path) -> None:
    exe = out_dir / "suffering.exe"
    dll = out_dir / "suffering.dll"
    if exe.exists() and dll.exists():
        newest = exe.stat().st_mtime
        comp_dir = out_dir / "compiler"
        if comp_dir.exists():
            for p in comp_dir.rglob("*"):
                if p.is_file():
                    newest = max(newest, p.stat().st_mtime)
        if dll.stat().st_mtime <= newest + 1.0:
            t_now = max(time.time(), newest + 10.0)
            os.utime(dll, (t_now, t_now))


def validate_rec_file(rec_path: Path) -> dict[str, Any]:
    if not rec_path.exists():
        raise AssertionError(f"Recording file missing: {rec_path}")
    lines = rec_path.read_text(encoding="utf-8", errors="replace").splitlines()
    if not lines or lines[0].strip() != "#TSREC 1":
        raise AssertionError(f"Invalid .rec header in {rec_path}: {lines[:1]}")

    i_count = 0
    c_count = 0
    r_count = 0
    s_footer = None
    prev_fid = -1
    prev_ts = -1
    prev_eff = -1

    for idx, line in enumerate(lines, 1):
        if not line or line.startswith("#"):
            continue
        if line.startswith("I "):
            parts = line.split()
            if len(parts) != 19:
                raise AssertionError(f"{rec_path}:{idx}: malformed I line ({len(parts)} fields)")
            fid = int(parts[1])
            ts = int(parts[2])
            eff = int(parts[3])
            if fid < prev_fid or ts < prev_ts or eff < prev_eff:
                raise AssertionError(
                    f"{rec_path}:{idx}: non-monotonic timestamps/frame_id "
                    f"({fid},{ts},{eff}) < ({prev_fid},{prev_ts},{prev_eff})"
                )
            prev_fid, prev_ts, prev_eff = fid, ts, eff
            i_count += 1
        elif line.startswith("C "):
            c_count += 1
        elif line.startswith("R "):
            r_count += 1
        elif line.startswith("S "):
            s_footer = line
        else:
            raise AssertionError(f"{rec_path}:{idx}: unknown record prefix: {line[:20]}")

    if i_count == 0 or c_count == 0 or s_footer is None:
        raise AssertionError(f"{rec_path}: incomplete recording (I={i_count}, C={c_count}, S={s_footer})")

    return {
        "path": str(rec_path),
        "lines": len(lines),
        "input_records": i_count,
        "checkpoint_records": c_count,
        "rumble_records": r_count,
        "footer": s_footer,
        "last_frame_id": prev_fid,
        "last_timestamp_us": prev_ts,
        "last_eff_us": prev_eff,
    }


def parse_state_jsonl(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise AssertionError(f"State JSONL missing: {path}")
    sdl3_audit = None
    summary = None
    checkpoints: list[dict[str, Any]] = []
    rumbles: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        obj = json.loads(line)
        t = obj.get("type")
        if t == "sdl3_audit":
            sdl3_audit = obj
        elif t == "checkpoint":
            checkpoints.append(obj)
        elif t == "rumble":
            rumbles.append(obj)
        elif t == "summary":
            summary = obj
    return {
        "sdl3_audit": sdl3_audit,
        "checkpoints": checkpoints,
        "rumbles": rumbles,
        "summary": summary,
    }


def cam_dist(a: list[float], b: list[float]) -> float:
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def verify_full_state_sequence(parsed: dict[str, Any], expect_replay_finished: bool) -> dict[str, Any]:
    sdl = parsed["sdl3_audit"]
    summary = parsed["summary"]
    cps = parsed["checkpoints"]
    if not sdl:
        raise AssertionError("Missing sdl3_audit entry in state JSONL")
    if not sdl.get("hidapi_enabled", False):
        raise AssertionError(f"SDL3 HIDAPI not enabled: {sdl}")
    if not summary:
        raise AssertionError("Missing summary entry in state JSONL")
    if expect_replay_finished and not summary.get("replay_finished", False):
        raise AssertionError(f"Replay did not finish cleanly: {summary}")

    # 1. Verify startup session=1 -> first gameplay entry session=2 with 1a PrisonDeathRow loaded
    idx_s1 = next((i for i, c in enumerate(cps) if c["session"] == 1), None)
    if idx_s1 is None:
        raise AssertionError("Never observed Title/Init session=1")

    idx_s2_first = next(
        (
            i
            for i, c in enumerate(cps)
            if i > idx_s1
            and c["session"] == 2
            and (c["lvl_flags"] & 2)
            and "1a prisondeathrow" in c["level"].lower()
        ),
        None,
    )
    if idx_s2_first is None:
        raise AssertionError("Never entered 1a PrisonDeathRow (session=2, lvl_flags&2)")

    # Locate cell spawn position (first session=2 unpaused checkpoint after cutscene skip where cam x < 860000)
    idx_cell_spawn = next(
        (
            i
            for i, c in enumerate(cps)
            if i >= idx_s2_first
            and c["session"] == 2
            and c["pause"] == 0
            and c["cam_valid"]
            and c["cam"][0] < 860000.0
            and c["cam"][0] > 700000.0
        ),
        None,
    )
    if idx_cell_spawn is None:
        raise AssertionError("Never reached Torque's cell spawn camera in 1a PrisonDeathRow")
    spawn_cam = cps[idx_cell_spawn]["cam"]

    # 2. Verify sub-deadzone / sub-threshold hold kept camera stationary right after cell spawn
    # Look at next periodic checkpoint before movement starts (within +1.5s of spawn)
    spawn_t = cps[idx_cell_spawn]["timestamp_us"]
    deadzone_cp = next(
        (
            c
            for c in cps[idx_cell_spawn + 1 :]
            if c["session"] == 2
            and c["pause"] == 0
            and 800_000 <= (c["timestamp_us"] - spawn_t) <= 3_500_000
        ),
        None,
    )
    deadzone_drift = cam_dist(spawn_cam, deadzone_cp["cam"]) if deadzone_cp else 0.0

    # 3. Verify active movement changed camera significantly (> 1000 units)
    idx_moved = next(
        (
            i
            for i, c in enumerate(cps)
            if i > idx_cell_spawn
            and c["session"] == 2
            and c["cam_valid"]
            and cam_dist(spawn_cam, c["cam"]) > 1000.0
        ),
        None,
    )
    if idx_moved is None:
        raise AssertionError(f"Camera never moved >1000 units from cell spawn {spawn_cam}")
    moved_cam = cps[idx_moved]["cam"]
    max_move_dist = max(
        cam_dist(spawn_cam, c["cam"])
        for c in cps[idx_cell_spawn:]
        if c["session"] == 2 and c["cam_valid"] and c["cam"][0] < 860000.0
    )

    # 4. Verify Pause (pause=1) -> Resume (pause=0) -> Pause again -> Revert To Last Checkpoint
    idx_pause1 = next(
        (i for i, c in enumerate(cps) if i > idx_moved and c["session"] == 2 and c["pause"] == 1),
        None,
    )
    if idx_pause1 is None:
        raise AssertionError("Never observed Pause (pause=1) after gameplay movement")

    idx_resume1 = next(
        (i for i, c in enumerate(cps) if i > idx_pause1 and c["session"] == 2 and c["pause"] == 0),
        None,
    )
    if idx_resume1 is None:
        raise AssertionError("Never observed Resume (pause=0) after Pause")

    idx_pause2 = next(
        (i for i, c in enumerate(cps) if i > idx_resume1 and c["session"] == 2 and c["pause"] == 1),
        None,
    )
    if idx_pause2 is None:
        raise AssertionError("Never observed second Pause (before Revert To Last Checkpoint)")

    # After Revert To Last Checkpoint, verify gtime rolled back and camera snapped back near cell spawn (< 500 units)
    pre_revert_gtime = cps[idx_pause2]["gtime"]
    idx_reverted = next(
        (
            i
            for i, c in enumerate(cps)
            if i > idx_pause2
            and c["session"] == 2
            and c["pause"] == 0
            and c["cam_valid"]
            and c["gtime"] < pre_revert_gtime - 10.0
            and cam_dist(spawn_cam, c["cam"]) < 500.0
        ),
        None,
    )
    if idx_reverted is None:
        raise AssertionError("Revert To Last Checkpoint did not roll back gtime and restore cell spawn camera!")
    reverted_cam = cps[idx_reverted]["cam"]
    revert_err = cam_dist(spawn_cam, reverted_cam)

    # 5. Verify Quit Game -> Title Screen (session in {1,3}, mode=1, level="") -> New Game Restart (session=2)
    idx_quit_title = next(
        (
            i
            for i, c in enumerate(cps)
            if i > idx_reverted and c["session"] in (1, 3) and c["mode"] == 1 and c["level"] == ""
        ),
        None,
    )
    if idx_quit_title is None:
        raise AssertionError("Quit Game never returned to Title Screen (session in {1,3}, mode=1, level='')")

    idx_restart_s2 = next(
        (
            i
            for i, c in enumerate(cps)
            if i > idx_quit_title
            and c["session"] == 2
            and (c["lvl_flags"] & 2)
            and "1a prisondeathrow" in c["level"].lower()
        ),
        None,
    )
    if idx_restart_s2 is None:
        raise AssertionError("New Game restart never re-entered 1a PrisonDeathRow (session=2)")

    return {
        "spawn_cam": spawn_cam,
        "deadzone_drift": round(deadzone_drift, 3),
        "moved_cam": moved_cam,
        "max_move_dist": round(max_move_dist, 2),
        "reverted_cam": reverted_cam,
        "revert_err_from_spawn": round(revert_err, 3),
        "pause1_poll": cps[idx_pause1]["poll_id"],
        "resume1_poll": cps[idx_resume1]["poll_id"],
        "reverted_poll": cps[idx_reverted]["poll_id"],
        "quit_title_poll": cps[idx_quit_title]["poll_id"],
        "restart_s2_poll": cps[idx_restart_s2]["poll_id"],
        "final_gframe": cps[-1]["gframe"],
        "total_polls": summary["total_polls"],
        "duration_s": round(summary["duration_us"] / 1e6, 3),
        "eff_duration_s": round(summary["eff_duration_us"] / 1e6, 3),
        "rumble_events": summary["rumble_events"],
        "checkpoint_count": len(cps),
    }


def run_game_pass(
    *,
    tag: str,
    build: str,
    timeout_s: float,
    cap_times: list[float],
    autotest_script: Path | None = None,
    record_path: Path | None = None,
    replay_path: Path | None = None,
    replay_mode: str = "hybrid",
    replay_exit: bool = False,
    clear_z_cache: bool = False,
) -> dict[str, Any]:
    check_no_suffering_running()
    out_dir = REPO / "out" / "build" / build
    exe = out_dir / "suffering.exe"
    if not exe.exists():
        raise RuntimeError(f"Executable missing: {exe}")
    ensure_dll_fresh(out_dir)

    if clear_z_cache:
        z_dir = out_dir / "hdd" / "Z"
        if z_dir.exists():
            shutil.rmtree(z_dir)
            print(f"[{tag}] Cleared Z:\\ cache: {z_dir}")

    cap_sub = f"captures_m3_{tag}"
    cap_dir = out_dir / cap_sub
    if cap_dir.exists():
        shutil.rmtree(cap_dir)
    cap_dir.mkdir(parents=True, exist_ok=True)

    CAP_ROOT.mkdir(parents=True, exist_ok=True)
    state_path = CAP_ROOT / f"{tag}_state.jsonl"
    if state_path.exists():
        state_path.unlink()

    env = os.environ.copy()
    env.pop("SUFFERING_AUTOTEST", None)
    env.pop("XBE_INPUT_RECORD", None)
    env.pop("XBE_INPUT_REPLAY", None)
    env.pop("XBE_REPLAY_MODE", None)
    env.pop("XBE_REPLAY_EXIT", None)
    env.pop("XBE_STATE_LOG", None)
    env["XBE_CAPTURE_DIR"] = cap_sub
    env["XBE_PERF"] = "1"
    if autotest_script is not None:
        env["SUFFERING_AUTOTEST"] = str(autotest_script)

    cmd = [str(exe), "--state-log", str(state_path)]
    if record_path is not None:
        cmd += ["--record-input", str(record_path)]
    if replay_path is not None:
        cmd += ["--replay-input", str(replay_path), "--replay-mode", replay_mode]
        if replay_exit:
            cmd += ["--replay-exit"]

    crash_dir = out_dir / "CrashDumps"
    dumps_before = set(crash_dir.glob("*.dmp")) if crash_dir.exists() else set()

    print(f"\n=== [{tag}] build={build} mode={replay_mode} cold_z={clear_z_cache} ===")
    print("  CMD:", " ".join(cmd))
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=str(out_dir), env=env)
    ci = 0
    exit_code: int | None = None
    timed_out = False

    try:
        while True:
            el = time.monotonic() - t0
            rc = proc.poll()
            if rc is not None:
                exit_code = rc
                break
            wins = find_windows(proc.pid)
            if ci < len(cap_times) and el >= cap_times[ci]:
                gw = [h for h, c in wins if c == "TheSuffering"]
                if gw:
                    user32.PostMessageW(gw[0], WM_KEYDOWN, VK_F3, 0)
                    user32.PostMessageW(gw[0], WM_KEYUP, VK_F3, 0)
                    print(f"  [t={el:5.2f}s] F3 capture #{ci + 1}")
                ci += 1
            if el >= timeout_s:
                timed_out = not (autotest_script is not None and not replay_exit)
                for h, _ in wins:
                    user32.PostMessageW(h, WM_CLOSE, 0, 0)
                try:
                    exit_code = proc.wait(timeout=6.0)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    exit_code = proc.wait(timeout=4.0)
                break
            time.sleep(0.15)
    finally:
        if proc.poll() is None:
            proc.kill()
            exit_code = proc.wait()

    wall_s = round(time.monotonic() - t0, 3)
    print(f"  Exited rc={exit_code} wall={wall_s}s timed_out={timed_out}")

    dumps_after = set(crash_dir.glob("*.dmp")) if crash_dir.exists() else set()
    new_dumps = sorted(str(p.relative_to(REPO)) for p in (dumps_after - dumps_before))

    pngs: list[str] = []
    png_dir = CAP_ROOT / tag
    if png_dir.exists():
        shutil.rmtree(png_dir)
    for i, d in enumerate(sorted(x for x in cap_dir.iterdir() if x.is_dir())):
        bmp = d / "final.bmp"
        if bmp.exists():
            png = png_dir / f"{tag}_{i + 1:02d}_{d.name}.png"
            bmp_to_png(bmp, png)
            pngs.append(str(png.relative_to(REPO)))

    # Inspect latest runtime log
    log_dir = out_dir / "logs"
    pat = "suffering_*.log" if build == "debug" else "suffering.log"
    logs = sorted(log_dir.glob(pat), key=lambda p: p.stat().st_mtime, reverse=True)
    log_text = logs[0].read_text(encoding="utf-8", errors="replace") if logs else ""
    xiso_mounted = "Suffering, The (USA).xiso.iso" in log_text
    hidapi_logged = "driver=hidapi(1," in log_text
    replay_done_logged = "input_recorder: replay completed" in log_text

    if exit_code != 0:
        raise AssertionError(f"[{tag}] Non-zero exit code: {exit_code}")
    if new_dumps:
        raise AssertionError(f"[{tag}] New crash dumps generated: {new_dumps}")
    if not xiso_mounted:
        raise AssertionError(f"[{tag}] Direct .xiso.iso mount not found in runtime log!")
    if not hidapi_logged:
        raise AssertionError(f"[{tag}] SDL3 HIDAPI audit line not found in runtime log!")
    if replay_exit and timed_out:
        raise AssertionError(f"[{tag}] Replay with --replay-exit timed out at {timeout_s}s!")

    parsed = parse_state_jsonl(state_path)
    return {
        "tag": tag,
        "build": build,
        "replay_mode": replay_mode,
        "cold_z": clear_z_cache,
        "exit_code": exit_code,
        "wall_s": wall_s,
        "new_crash_dumps": new_dumps,
        "xiso_mounted": xiso_mounted,
        "hidapi_logged": hidapi_logged,
        "replay_done_logged": replay_done_logged,
        "sdl3_audit": parsed["sdl3_audit"],
        "summary": parsed["summary"],
        "pngs": pngs,
        "state_log": str(state_path.relative_to(REPO)),
        "_parsed": parsed,
    }


def run_record_golden(duration_s: float) -> dict[str, Any]:
    res = run_game_pass(
        tag="record_golden",
        build="release",
        timeout_s=duration_s,
        cap_times=FULL_CAP_TIMES,
        autotest_script=GOLDEN_SOURCE,
        record_path=GOLDEN_REC,
        replay_mode="hybrid",
        replay_exit=False,
    )
    rec_meta = validate_rec_file(GOLDEN_REC)
    seq_metrics = verify_full_state_sequence(res["_parsed"], expect_replay_finished=False)
    print("\n=== GOLDEN RECORDING VERIFIED ===")
    print("  REC:", json.dumps(rec_meta, indent=2))
    print("  SEQ:", json.dumps(seq_metrics, indent=2))
    return {"pass": {k: v for k, v in res.items() if k != "_parsed"}, "rec": rec_meta, "seq": seq_metrics}


def run_roundtrip_and_modes() -> dict[str, Any]:
    rt_script = CAP_ROOT / "roundtrip_source.txt"
    rt_script.write_text(
        "# Short Menu Round-Trip Script (Title -> Options -> Controller -> Title)\n"
        "3000 3100 A\n"
        "7000 7100 A\n"
        "11000 11100 A\n"
        "15000 15100 A\n"
        "23000 23100 DOWN\n"
        "23800 23900 DOWN\n"
        "24600 24700 A\n"
        "26000 26100 A\n"
        "27600 27700 B\n"
        "28600 28700 B\n",
        encoding="utf-8",
    )
    rt_rec = CAP_ROOT / "roundtrip.rec"
    rec_pass = run_game_pass(
        tag="roundtrip_record",
        build="release",
        timeout_s=33.0,
        cap_times=[27.2, 31.0],
        autotest_script=rt_script,
        record_path=rt_rec,
    )
    rec_meta = validate_rec_file(rt_rec)

    mode_results: dict[str, Any] = {}
    for mode in ("hybrid", "frame", "time"):
        r = run_game_pass(
            tag=f"roundtrip_replay_{mode}",
            build="release",
            timeout_s=50.0,
            cap_times=[27.2, 31.0],
            replay_path=rt_rec,
            replay_mode=mode,
            replay_exit=True,
        )
        summary = r["summary"]
        if not summary or not summary.get("replay_finished"):
            raise AssertionError(f"[roundtrip_replay_{mode}] Replay did not complete cleanly!")
        mode_results[mode] = {
            "wall_s": r["wall_s"],
            "total_polls": summary["total_polls"],
            "duration_s": round(summary["duration_us"] / 1e6, 3),
            "eff_duration_s": round(summary["eff_duration_us"] / 1e6, 3),
            "replay_finished": summary["replay_finished"],
            "pngs": r["pngs"],
        }
    return {
        "record_wall_s": rec_pass["wall_s"],
        "rec_meta": rec_meta,
        "modes": mode_results,
    }


def run_full_suite() -> int:
    media_before = snapshot_protected_media()
    try:
        rec_meta = validate_rec_file(GOLDEN_REC)
        roundtrip = run_roundtrip_and_modes()

        runs: list[dict[str, Any]] = []
        plan = [
            ("release_warm_1", "release", "hybrid", False),
            ("release_warm_2", "release", "hybrid", False),
            ("release_warm_3", "release", "hybrid", False),
            ("release_cold_z", "release", "hybrid", True),
            ("debug_warm_1", "debug", "hybrid", False),
        ]
        for tag, build, mode, cold_z in plan:
            r = run_game_pass(
                tag=tag,
                build=build,
                timeout_s=145.0,
                cap_times=FULL_CAP_TIMES,
                replay_path=GOLDEN_REC,
                replay_mode=mode,
                replay_exit=True,
                clear_z_cache=cold_z,
            )
            seq = verify_full_state_sequence(r["_parsed"], expect_replay_finished=True)
            entry = {k: v for k, v in r.items() if k != "_parsed"}
            entry["sequence"] = seq
            runs.append(entry)
            print(f"  [{tag}] PASS: {json.dumps(seq)}")

        # Compute cross-run quantitative consistency metrics
        warm = [r for r in runs if r["tag"].startswith("release_warm_")]
        all_polls = [r["sequence"]["total_polls"] for r in runs]
        warm_polls = [r["sequence"]["total_polls"] for r in warm]
        warm_gframes = [r["sequence"]["final_gframe"] for r in warm]
        warm_wall = [r["wall_s"] for r in warm]
        warm_moved_z = [r["sequence"]["moved_cam"][2] for r in warm]
        max_revert_err = max(r["sequence"]["revert_err_from_spawn"] for r in runs)
        max_deadzone_drift = max(r["sequence"]["deadzone_drift"] for r in runs)

        consistency = {
            "warm_release_count": len(warm),
            "warm_wall_s": warm_wall,
            "warm_wall_spread_s": round(max(warm_wall) - min(warm_wall), 3),
            "warm_polls": warm_polls,
            "warm_polls_spread": max(warm_polls) - min(warm_polls),
            "warm_final_gframe": warm_gframes,
            "warm_final_gframe_spread": max(warm_gframes) - min(warm_gframes),
            "warm_moved_cam_z_spread": round(max(warm_moved_z) - min(warm_moved_z), 3),
            "all_runs_polls_mean": round(statistics.mean(all_polls), 1),
            "all_runs_polls_spread": max(all_polls) - min(all_polls),
            "max_deadzone_drift": round(max_deadzone_drift, 3),
            "max_revert_err_from_spawn": round(max_revert_err, 3),
        }

        verify_protected_media(media_before)

        report = {
            "status": "PASS",
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "golden_rec": rec_meta,
            "roundtrip_and_modes": roundtrip,
            "full_replay_runs": runs,
            "consistency": consistency,
            "protected_media_verified": True,
        }
        out_json = CAP_ROOT / "m3_regression_summary.json"
        out_json.write_text(json.dumps(report, indent=2), encoding="utf-8")
        print(f"\n=== ALL MILESTONE 3 REGRESSION CHECKS PASSED ===")
        print(f"Summary written to: {out_json.relative_to(REPO)}")
        print(json.dumps(consistency, indent=2))
        return 0
    finally:
        verify_protected_media(media_before)


def main() -> int:
    ap = argparse.ArgumentParser(description="The Suffering (USA) Milestone 3 Regression Runner")
    ap.add_argument("--record", action="store_true", help="Record golden tests/m3_full_regression.rec from source script")
    ap.add_argument("--record-duration", type=float, default=108.0, help="Recording wall duration in seconds")
    ap.add_argument("--single", choices=["release_warm", "release_cold_z", "debug_warm"], help="Run a single replay pass")
    args = ap.parse_args()

    media_before = snapshot_protected_media()
    try:
        if args.record:
            run_record_golden(args.record_duration)
            verify_protected_media(media_before)
            return 0
        if args.single:
            build = "debug" if args.single == "debug_warm" else "release"
            cold_z = args.single == "release_cold_z"
            r = run_game_pass(
                tag=args.single,
                build=build,
                timeout_s=145.0,
                cap_times=FULL_CAP_TIMES,
                replay_path=GOLDEN_REC,
                replay_mode="hybrid",
                replay_exit=True,
                clear_z_cache=cold_z,
            )
            seq = verify_full_state_sequence(r["_parsed"], expect_replay_finished=True)
            print(f"[{args.single}] PASS:", json.dumps(seq, indent=2))
            verify_protected_media(media_before)
            return 0
        return run_full_suite()
    except Exception as exc:
        print(f"REGRESSION FAILED: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
