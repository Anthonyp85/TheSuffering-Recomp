#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Milestone 4 Performance & Frame-Pacing Benchmark Runner for The Suffering (USA).

Runs zero-F3 deterministic Release replays of tests/m3_full_regression.rec with
--perf-log and --state-log to collect all 11 mandatory metrics across:
  --phase baseline : cold-Z (with hang tracking), 3x warm-Z, 30 FPS vs 60 FPS speed check
  --phase postopt  : 5x cold-Z (run 1 also cold-shader-cache), 3x warm-Z+warm-cache, 30 vs 60 FPS
"""

from __future__ import annotations

import argparse
import ctypes
import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
PERF_ROOT = REPO / ".local" / "m4_perf"
GOLDEN_REC = REPO / "tests" / "m3_full_regression.rec"

PROTECTED_MEDIA = [
    p for p in [
        Path(os.environ["XBE_ISO_PATH"]) if os.environ.get("XBE_ISO_PATH") else (REPO / "Suffering, The (USA)" / "Suffering, The (USA).xiso.iso")
    ] if p.is_file()
]

TH32CS_SNAPPROCESS = 0x00000002
WM_CLOSE = 0x0010
user32 = ctypes.windll.user32
EnumWindowsProc = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)


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


def find_windows(pid: int) -> list[int]:
    out: list[int] = []

    def cb(h: int, _: int) -> bool:
        wp = ctypes.c_ulong()
        user32.GetWindowThreadProcessId(h, ctypes.byref(wp))
        if wp.value == pid and user32.IsWindowVisible(h):
            out.append(h)
        return True

    user32.EnumWindows(EnumWindowsProc(cb), 0)
    return out


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


def cam_dist(a: list[float], b: list[float]) -> float:
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def analyze_state_jsonl(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"present": False}
    cps = []
    summary = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        obj = json.loads(line)
        if obj.get("type") == "checkpoint":
            cps.append(obj)
        elif obj.get("type") == "summary":
            summary = obj

    # Measure active gameplay segment (first cell spawn -> first Pause transition after spawn)
    s0 = next(
        (
            i
            for i, c in enumerate(cps)
            if c["session"] == 2 and c["pause"] == 0 and c["cam_valid"] and 700000.0 < c["cam"][0] < 860000.0
        ),
        None,
    )
    spawn_cam = cps[s0]["cam"] if s0 is not None else None
    max_move_dist = 0.0
    path_len = 0.0
    gameplay_wall_s = 0.0
    gameplay_gtime_s = 0.0
    end_cam = None

    if s0 is not None:
        t_spawn = cps[s0]["timestamp_us"]
        p1 = next(
            (
                i
                for i in range(s0 + 1, len(cps))
                if cps[i]["session"] == 2 and cps[i]["pause"] == 1 and cps[i]["timestamp_us"] > t_spawn + 5_000_000
            ),
            len(cps),
        )
        seg = [c for c in cps[s0:p1] if c["cam_valid"] and c["pause"] == 0]
        if len(seg) >= 2:
            for i in range(1, len(seg)):
                path_len += cam_dist(seg[i - 1]["cam"], seg[i]["cam"])
            max_move_dist = max(cam_dist(spawn_cam, c["cam"]) for c in seg)
            gameplay_wall_s = (seg[-1]["timestamp_us"] - seg[0]["timestamp_us"]) / 1e6
            gameplay_gtime_s = seg[-1]["gtime"] - seg[0]["gtime"]
            end_cam = seg[-1]["cam"]

    return {
        "present": True,
        "summary": summary,
        "checkpoints": len(cps),
        "spawn_cam": spawn_cam,
        "end_cam_before_pause": end_cam,
        "max_move_dist": round(max_move_dist, 2),
        "gameplay_seg_path_len": round(path_len, 2),
        "gameplay_seg_wall_s": round(gameplay_wall_s, 3),
        "gameplay_seg_gtime_s": round(gameplay_gtime_s, 3),
        "gameplay_speed_units_per_s": round(path_len / gameplay_wall_s, 2) if gameplay_wall_s > 0.5 else 0.0,
        "gtime_to_wall_ratio": round(gameplay_gtime_s / gameplay_wall_s, 4) if gameplay_wall_s > 0.5 else 0.0,
    }


def run_single(
    *,
    out_root: Path,
    tag: str,
    build: str = "release",
    fps_cap: int | None = None,
    vsync: int | None = None,
    clear_z: bool = False,
    clear_shader_cache: bool = False,
    no_shader_cache: bool = False,
    timeout_s: float = 155.0,
    allow_timeout: bool = False,
) -> dict[str, Any]:
    check_no_suffering_running()
    out_dir = REPO / "out" / "build" / build
    exe = out_dir / "suffering.exe"
    ensure_dll_fresh(out_dir)

    if clear_z:
        z_dir = out_dir / "hdd" / "Z"
        if z_dir.exists():
            shutil.rmtree(z_dir)
            print(f"[{tag}] Cleared Z:\\ cache")
    if clear_shader_cache:
        c_dir = out_dir / "cache"
        if c_dir.exists():
            shutil.rmtree(c_dir)
            print(f"[{tag}] Cleared shader/PSO cache")

    out_root.mkdir(parents=True, exist_ok=True)
    perf_path = out_root / f"{tag}.perf.json"
    state_path = out_root / f"{tag}.state.jsonl"
    for p in (perf_path, state_path, perf_path.with_suffix(".frames.csv")):
        if p.exists():
            p.unlink()

    env = os.environ.copy()
    for k in (
        "SUFFERING_AUTOTEST",
        "XBE_INPUT_RECORD",
        "XBE_INPUT_REPLAY",
        "XBE_REPLAY_MODE",
        "XBE_REPLAY_EXIT",
        "XBE_STATE_LOG",
        "XBE_PERF_LOG",
        "XBE_PERF",
        "XBE_CAPTURE_DIR",
        "XBE_FPS_CAP",
        "XBE_VSYNC",
        "XBE_NO_SHADER_CACHE",
    ):
        env.pop(k, None)

    cmd = [
        str(exe),
        "--replay-input",
        str(GOLDEN_REC),
        "--replay-mode",
        "hybrid",
        "--replay-exit",
        "--perf-log",
        str(perf_path),
        "--state-log",
        str(state_path),
    ]
    if fps_cap is not None:
        cmd += ["--fps-cap", str(fps_cap)]
    if vsync is not None:
        cmd += ["--vsync", str(vsync)]
    if no_shader_cache:
        cmd += ["--no-shader-cache"]

    crash_dir = out_dir / "CrashDumps"
    dumps_before = set(crash_dir.glob("*.dmp")) if crash_dir.exists() else set()

    print(f"\n=== [{tag}] build={build} fps_cap={fps_cap} vsync={vsync} cold_z={clear_z} cold_cache={clear_shader_cache} ===")
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd, cwd=str(out_dir), env=env)
    timed_out = False
    exit_code: int | None = None

    try:
        while True:
            el = time.monotonic() - t0
            rc = proc.poll()
            if rc is not None:
                exit_code = rc
                break
            if el >= timeout_s:
                timed_out = True
                for h in find_windows(proc.pid):
                    user32.PostMessageW(h, WM_CLOSE, 0, 0)
                try:
                    exit_code = proc.wait(timeout=6.0)
                except subprocess.TimeoutExpired:
                    proc.terminate()
                    exit_code = proc.wait(timeout=4.0)
                break
            time.sleep(0.2)
    finally:
        if proc.poll() is None:
            proc.kill()
            exit_code = proc.wait()

    wall_s = round(time.monotonic() - t0, 3)
    dumps_after = set(crash_dir.glob("*.dmp")) if crash_dir.exists() else set()
    new_dumps = sorted(str(p.relative_to(REPO)) for p in (dumps_after - dumps_before))

    perf_data = json.loads(perf_path.read_text(encoding="utf-8")) if perf_path.exists() else None
    state_info = analyze_state_jsonl(state_path)
    replay_ok = bool(state_info.get("summary") and state_info["summary"].get("replay_finished"))

    print(f"  rc={exit_code} wall={wall_s}s timed_out={timed_out} replay_finished={replay_ok} dumps={len(new_dumps)}")
    if perf_data:
        gp = perf_data["phases"].get("gameplay_active", {})
        ov = perf_data["phases"].get("overall", {})
        sh = perf_data.get("shaders", {})
        st = perf_data.get("stalls", {})
        print(
            f"  OVERALL : fps={ov.get('avg_fps')} p50={ov.get('p50_ms')}ms p99={ov.get('p99_ms')}ms "
            f"1%low={ov.get('fps_1pct_low_mean')} max={ov.get('max_ms')}ms"
        )
        print(
            f"  GAMEPLAY: frames={gp.get('frames')} fps={gp.get('avg_fps')} p50={gp.get('p50_ms')}ms "
            f"p99={gp.get('p99_ms')}ms 1%low={gp.get('fps_1pct_low_mean')} max={gp.get('max_ms')}ms "
            f"gpu={gp.get('gpu_ms_avg')}ms g_dt_p50={gp.get('g_dt_ms_p50')}ms"
        )
        print(
            f"  SHADERS : compile={sh.get('compile_count')} ({sh.get('compile_ms_total')}ms, hits={sh.get('shader_cache_hits')}) "
            f"pso={sh.get('pso_create_count')} ({sh.get('pso_create_ms_total')}ms, hits={sh.get('pso_cache_hits')}) "
            f"glock_bg_max={st.get('d3d_glock_bg_wait_ms_max')}ms"
        )

    if new_dumps:
        raise AssertionError(f"[{tag}] New crash dumps: {new_dumps}")
    if not allow_timeout and (timed_out or exit_code != 0 or not replay_ok):
        raise AssertionError(f"[{tag}] Failed: rc={exit_code} timed_out={timed_out} replay_ok={replay_ok}")

    return {
        "tag": tag,
        "build": build,
        "fps_cap_override": fps_cap,
        "vsync_override": vsync,
        "cold_z": clear_z,
        "cold_cache": clear_shader_cache,
        "exit_code": exit_code,
        "wall_s": wall_s,
        "timed_out": timed_out,
        "replay_finished": replay_ok,
        "new_crash_dumps": new_dumps,
        "perf_json": str(perf_path.relative_to(REPO)),
        "state_jsonl": str(state_path.relative_to(REPO)),
        "perf": perf_data,
        "state": state_info,
    }


def aggregate_warm(runs: list[dict[str, Any]]) -> dict[str, Any]:
    def vals(path_fn):
        return [path_fn(r) for r in runs if r.get("perf")]

    gp_fps = vals(lambda r: r["perf"]["phases"]["gameplay_active"].get("avg_fps", 0.0))
    gp_p50 = vals(lambda r: r["perf"]["phases"]["gameplay_active"].get("p50_ms", 0.0))
    gp_p99 = vals(lambda r: r["perf"]["phases"]["gameplay_active"].get("p99_ms", 0.0))
    gp_1pct = vals(lambda r: r["perf"]["phases"]["gameplay_active"].get("fps_1pct_low_mean", 0.0))
    gp_max = vals(lambda r: r["perf"]["phases"]["gameplay_active"].get("max_ms", 0.0))
    ov_max = vals(lambda r: r["perf"]["phases"]["overall"].get("max_ms", 0.0))
    sh_ms = vals(lambda r: r["perf"]["shaders"].get("compile_ms_total", 0.0))
    pso_ms = vals(lambda r: r["perf"]["shaders"].get("pso_create_ms_total", 0.0))
    glock_max = vals(lambda r: r["perf"]["stalls"].get("d3d_glock_bg_wait_ms_max", 0.0))
    return {
        "count": len(runs),
        "gameplay_avg_fps_mean": round(statistics.mean(gp_fps), 2) if gp_fps else 0.0,
        "gameplay_p50_ms_mean": round(statistics.mean(gp_p50), 3) if gp_p50 else 0.0,
        "gameplay_p99_ms_mean": round(statistics.mean(gp_p99), 3) if gp_p99 else 0.0,
        "gameplay_1pct_low_mean": round(statistics.mean(gp_1pct), 2) if gp_1pct else 0.0,
        "gameplay_max_ms_mean": round(statistics.mean(gp_max), 3) if gp_max else 0.0,
        "overall_max_ms_mean": round(statistics.mean(ov_max), 3) if ov_max else 0.0,
        "shader_compile_ms_mean": round(statistics.mean(sh_ms), 2) if sh_ms else 0.0,
        "pso_create_ms_mean": round(statistics.mean(pso_ms), 2) if pso_ms else 0.0,
        "glock_bg_max_ms_mean": round(statistics.mean(glock_max), 3) if glock_max else 0.0,
    }


def run_baseline() -> int:
    out_root = PERF_ROOT / "baseline"
    cold_attempts = []
    cold_pass = None
    for attempt in range(1, 4):
        tag = f"baseline_cold_try{attempt}"
        r = run_single(
            out_root=out_root,
            tag=tag,
            clear_z=True,
            clear_shader_cache=True,
            timeout_s=155.0,
            allow_timeout=True,
        )
        cold_attempts.append(r)
        if not r["timed_out"] and r["exit_code"] == 0 and r["replay_finished"]:
            cold_pass = r
            break
    if cold_pass is None:
        raise RuntimeError("Baseline cold-Z failed all 3 attempts!")

    warm_runs = [
        run_single(out_root=out_root, tag=f"baseline_warm_{i}", clear_z=False, timeout_s=155.0)
        for i in (1, 2, 3)
    ]
    fps30 = run_single(out_root=out_root, tag="baseline_fps30", fps_cap=30, timeout_s=175.0)
    fps60 = run_single(out_root=out_root, tag="baseline_fps60", fps_cap=60, timeout_s=155.0)

    summary = {
        "phase": "baseline",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "cold_attempts": len(cold_attempts),
        "cold_hangs": sum(1 for r in cold_attempts if r["timed_out"]),
        "cold_pass": cold_pass,
        "warm_runs": warm_runs,
        "warm_aggregate": aggregate_warm(warm_runs),
        "speed_fidelity": {
            "fps30": fps30,
            "fps60": fps60,
        },
    }
    (out_root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\n=== BASELINE COMPLETE -> {(out_root / 'summary.json').relative_to(REPO)} ===")
    print(json.dumps(summary["warm_aggregate"], indent=2))
    return 0


def run_postopt() -> int:
    out_root = PERF_ROOT / "postopt"
    cold_runs = []
    for i in range(1, 6):
        r = run_single(
            out_root=out_root,
            tag=f"postopt_cold_{i}",
            clear_z=True,
            clear_shader_cache=(i == 1),
            timeout_s=155.0,
            allow_timeout=False,
        )
        cold_runs.append(r)

    warm_runs = [
        run_single(out_root=out_root, tag=f"postopt_warm_{i}", clear_z=False, timeout_s=155.0)
        for i in (1, 2, 3)
    ]
    fps30 = run_single(out_root=out_root, tag="postopt_fps30", fps_cap=30, timeout_s=175.0)
    fps60 = run_single(out_root=out_root, tag="postopt_fps60", fps_cap=60, timeout_s=155.0)

    summary = {
        "phase": "postopt",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "cold_z_pass_count": len(cold_runs),
        "cold_z_total": 5,
        "cold_runs": cold_runs,
        "warm_runs": warm_runs,
        "warm_aggregate": aggregate_warm(warm_runs),
        "speed_fidelity": {
            "fps30": fps30,
            "fps60": fps60,
        },
    }
    (out_root / "summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"\n=== POST-OPT COMPLETE -> {(out_root / 'summary.json').relative_to(REPO)} ===")
    print(json.dumps(summary["warm_aggregate"], indent=2))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="The Suffering (USA) Milestone 4 Performance Benchmark")
    ap.add_argument("--phase", choices=["baseline", "postopt", "smoke"], required=True)
    args = ap.parse_args()

    media_before = snapshot_protected_media()
    try:
        if args.phase == "smoke":
            run_single(out_root=PERF_ROOT / "smoke", tag="smoke_warm", timeout_s=155.0)
            verify_protected_media(media_before)
            return 0
        rc = run_baseline() if args.phase == "baseline" else run_postopt()
        verify_protected_media(media_before)
        return rc
    finally:
        verify_protected_media(media_before)


if __name__ == "__main__":
    sys.exit(main())
