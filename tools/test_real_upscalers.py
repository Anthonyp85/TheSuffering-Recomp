#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Live Hardware GPU Verification Suite for Bundled Official Vendor Upscalers & NGX Driver Fallback.

Tests all 3 real vendor temporal upscalers in live 3D Level 1 gameplay (m3_full_regression.rec):
  1. NVIDIA DLSS 4 (v310.9.16, Preset K, plugins\\nvngx_dlss.dll)
  2. AMD FidelityFX FSR 3.1 (SDK v2.4.0 / provider 3.1.6, plugins\\amd_fidelityfx_loader_dx12.dll + upscaler)
  3. Intel XeSS (SDK v3.0.2 / XeSS 2.0.2 DP4a, plugins\\libxess.dll)
  4. NVIDIA NGX Driver-Cache Auto-Fallback (%ProgramData%\\NVIDIA\\NGX -> cache\\ngx\\nvngx_dlss.dll)
  5. Pre-Game Launcher Tab 1 & Tab 5 visual captures confirming out-of-the-box [Ready] badges

Outputs:
  - .local/captures_v11/upscale_real_{dlss4,fsr31,xess}_quality.png
  - .local/captures_v11/upscale_ngx_driver_fallback_quality.png
  - .local/captures_v11/launcher_bundled_tab{1_upscaling,5_tools}.png
  - .local/v11_test/real_upscalers_summary.json
"""

from __future__ import annotations

import ctypes
import json
import os
import re
import shutil
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
REPLAY_REC = REPO / "tests" / "m3_full_regression.rec"
SETTINGS_FILE = EXE_DIR / "settings.toml"
LOGS_DIR = EXE_DIR / "logs"
CRASH_DIR = EXE_DIR / "CrashDumps"

CAP_DIR = REPO / ".local" / "captures_v11"
WORK_DIR = REPO / ".local" / "v11_test"
SUMMARY_JSON = WORK_DIR / "real_upscalers_summary.json"

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


def bmp32_to_png_and_stats(bmp_path: Path, png_path: Path) -> dict[str, Any]:
    data = bmp_path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise RuntimeError(f"Invalid BMP header: {bmp_path}")
    pix_off = struct.unpack_from("<I", data, 10)[0]
    w, h_signed = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 32 or w <= 0 or h_signed == 0:
        raise RuntimeError(f"Unsupported BMP ({w}x{h_signed} {bpp}bpp): {bmp_path}")
    top_down = h_signed < 0
    h = abs(h_signed)
    stride = w * 4
    rows_rgb = bytearray()
    non_black = 0
    luma_sum = 0.0
    luma_sq = 0.0
    total_px = w * h
    for y in range(h):
        src_y = y if top_down else (h - 1 - y)
        off = pix_off + src_y * stride
        row = data[off : off + stride]
        rows_rgb.append(0)
        for x in range(0, stride, 4):
            b, g, r = row[x], row[x + 1], row[x + 2]
            rows_rgb.extend((r, g, b))
            if (r | g | b) != 0:
                non_black += 1
            lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
            luma_sum += lum
            luma_sq += lum * lum

    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    idat = zlib.compress(bytes(rows_rgb), level=6)
    png_bytes = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    png_path.parent.mkdir(parents=True, exist_ok=True)
    png_path.write_bytes(png_bytes)

    mean_luma = luma_sum / max(total_px, 1)
    var = max(0.0, (luma_sq / max(total_px, 1)) - mean_luma * mean_luma)
    return {
        "png": str(png_path.relative_to(REPO)),
        "width": w,
        "height": h,
        "non_black_pct": round(100.0 * non_black / max(total_px, 1), 2),
        "mean_luma": round(mean_luma, 2),
        "luma_stddev": round(var ** 0.5, 2),
        "png_bytes": len(png_bytes),
    }


def latest_log_after(since_mtime: float) -> str:
    if not LOGS_DIR.is_dir():
        return ""
    cands = sorted(LOGS_DIR.glob("suffering*.log"), key=lambda p: p.stat().st_mtime, reverse=True)
    for p in cands:
        if p.stat().st_mtime >= since_mtime - 1.0:
            return p.read_text(encoding="utf-8", errors="replace")
    return cands[0].read_text(encoding="utf-8", errors="replace") if cands else ""


def parse_perf_gpu_ms(perf_json: Path) -> dict[str, Any]:
    if not perf_json.is_file():
        return {"samples": 0}
    try:
        data = json.loads(perf_json.read_text(encoding="utf-8", errors="replace"))
    except Exception:
        return {"samples": 0}
    phases = data.get("phases", {})
    ov = phases.get("overall", {})
    gp = phases.get("gameplay_active", {})
    target = gp if int(gp.get("frames", 0)) >= 20 else ov
    return {
        "phase": "gameplay_active" if target is gp else "overall",
        "samples": int(target.get("frames", 0)),
        "gpu_ms_avg": round(float(target.get("gpu_ms_avg", 0.0)), 3),
        "gpu_ms_p99": round(float(target.get("gpu_ms_p99", 0.0)), 3),
        "render_host_ms_avg": round(float(target.get("render_host_ms_avg", 0.0)), 3),
        "avg_fps": round(float(target.get("avg_fps", 0.0)), 2),
    }


def parse_render_line(log_txt: str) -> dict[str, Any]:
    m = re.search(r"d3d12:.*render=(\d+)x(\d+)\s*->\s*vp=(\d+)x(\d+)\s*\(bb=(\d+)x(\d+)", log_txt)
    if not m:
        return {}
    return {
        "render_w": int(m.group(1)),
        "render_h": int(m.group(2)),
        "vp_w": int(m.group(3)),
        "vp_h": int(m.group(4)),
        "bb_w": int(m.group(5)),
        "bb_h": int(m.group(6)),
    }


def run_exe(args: list[str], extra_env: dict[str, str] | None = None, timeout: int = 150) -> tuple[int, str]:
    check_no_suffering_running()
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    t0 = time.time()
    proc = subprocess.run([str(EXE), *args], cwd=str(EXE_DIR), env=env, timeout=timeout, capture_output=True)
    log_txt = latest_log_after(t0)
    return proc.returncode, log_txt


def main() -> int:
    if not EXE.is_file():
        print(f"ERROR: {EXE} not found. Run build.bat first.")
        return 1
    check_no_suffering_running()
    CAP_DIR.mkdir(parents=True, exist_ok=True)
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    orig_settings = SETTINGS_FILE.read_bytes() if SETTINGS_FILE.is_file() else None
    before_dumps = set(CRASH_DIR.glob("*.dmp")) if CRASH_DIR.is_dir() else set()

    summary: dict[str, Any] = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "real_upscalers": {},
        "ngx_driver_fallback": {},
        "launcher_bundled_captures": {},
        "errors": [],
    }

    try:
        iso_line = ""
        if orig_settings:
            for ln in orig_settings.decode("utf-8", errors="replace").splitlines():
                if ln.strip().startswith("iso_path"):
                    iso_line = ln
                    break
        test_toml = (
            'display_mode = "windowed"\n'
            "width = 1280\n"
            "height = 720\n"
            "fps_cap = 60\n"
            'present_mode = "auto"\n'
            "vsync = true\n"
            "aspect_correct_render = true\n"
            "skip_launcher = false\n"
            + (iso_line + "\n" if iso_line else "")
        )
        SETTINGS_FILE.write_text(test_toml, encoding="utf-8")

        cases = [
            (
                "dlss4_quality",
                ["--upscaler=dlss", "--upscale-quality=quality", "--dlss-preset=k", "--sharpness=25"],
                "upscale_real_dlss4_quality.png",
                ["upscale/dlss: initialized successfully", "upscale/dlss: first EvaluateFeature succeeded"],
                r"upscale/dlss: shutdown \(evaluated (\d+) frames\)",
            ),
            (
                "fsr31_quality",
                ["--upscaler=fsr3", "--upscale-quality=quality", "--sharpness=25"],
                "upscale_real_fsr31_quality.png",
                ["upscale/fsr3: context ready", "provider=3.1.6", "upscale/fsr3: first ffxDispatch succeeded"],
                r"upscale/fsr3: shutdown \(dispatched (\d+) frames\)",
            ),
            (
                "xess_quality",
                ["--upscaler=xess", "--upscale-quality=quality", "--sharpness=25"],
                "upscale_real_xess_quality.png",
                ["Ready (XeSS 2.0.2)", "upscale/xess: first xessD3D12Execute succeeded"],
                r"upscale/xess: shutdown \(executed (\d+) frames\)",
            ),
        ]

        print("[1/3] Running live 3D Level 1 gameplay verification for ALL 3 real vendor upscalers...")
        for name, uargs, png_name, req_tokens, frames_re in cases:
            bmp = WORK_DIR / f"real_{name}.bmp"
            png = CAP_DIR / png_name
            perf_path = WORK_DIR / f"perf_real_{name}.json"
            for p in (bmp, perf_path):
                if p.exists():
                    p.unlink()
            cmd_args = [
                *uargs,
                f"--replay-input={REPLAY_REC}",
                "--replay-mode=hybrid",
                "--capture-at-gframe=4560",
                f"--capture-bmp={bmp}",
                f"--perf-log={perf_path}",
                "--capture-exit",
                "--no-launcher",
            ]
            rc, log_txt = run_exe(cmd_args, timeout=160)
            missing_tokens = [tok for tok in req_tokens if tok not in log_txt]
            had_fallback = ("Vendor Evaluate fallback" in log_txt) or ("falling back cleanly" in log_txt)
            m_frames = re.search(frames_re, log_txt)
            shutdown_frames = int(m_frames.group(1)) if m_frames else 0
            rinfo = parse_render_line(log_txt)
            perf_m = parse_perf_gpu_ms(perf_path)
            gameplay_frames = int(perf_m.get("samples", 0)) if perf_m.get("phase") == "gameplay_active" else 0
            entry: dict[str, Any] = {
                "rc": rc,
                "render_info": rinfo,
                "perf": perf_m,
                "gameplay_frames": gameplay_frames,
                "shutdown_eval_frames": shutdown_frames,
                "missing_log_tokens": missing_tokens,
                "had_fallback": had_fallback,
            }
            if rc != 0 or not bmp.is_file():
                summary["errors"].append(f"Real {name} run failed (rc={rc}, bmp={bmp.is_file()})")
            else:
                entry["image"] = bmp32_to_png_and_stats(bmp, png)
                if entry["image"]["non_black_pct"] < 15.0:
                    summary["errors"].append(f"Real {name} image looks blank: {entry['image']}")
            if missing_tokens:
                summary["errors"].append(f"Real {name} missing log tokens: {missing_tokens}")
            if had_fallback:
                summary["errors"].append(f"Real {name} unexpectedly fell back to FSR 1.0")
            if gameplay_frames < 100:
                summary["errors"].append(f"Real {name} recorded too few gameplay frames: {gameplay_frames}")
            if rinfo.get("render_w") != 852 or rinfo.get("render_h") != 480:
                summary["errors"].append(f"Real {name} render mismatch: {rinfo}")

            summary["real_upscalers"][name] = entry
            print(
                f"      {name:16s}: rc={rc} gameplay_frames={gameplay_frames} "
                f"{rinfo.get('render_w')}x{rinfo.get('render_h')}->{rinfo.get('vp_w')}x{rinfo.get('vp_h')} "
                f"gpu_ms={perf_m.get('gpu_ms_avg', 'n/a')} fps={perf_m.get('avg_fps', 'n/a')} "
                f"fallback={had_fallback} missing={missing_tokens}"
            )

        print("[2/3] Testing live NVIDIA NGX driver-cache auto-fallback (empty plugins\\ + XBE_NGX_DRIVER_FALLBACK=1)...")
        empty_dir = WORK_DIR / "empty_plugins_ngx_fallback"
        if empty_dir.exists():
            shutil.rmtree(empty_dir)
        empty_dir.mkdir(parents=True, exist_ok=True)
        ngx_cache_dir = EXE_DIR / "cache" / "ngx"
        if ngx_cache_dir.exists():
            shutil.rmtree(ngx_cache_dir)

        fb_bmp = WORK_DIR / "real_ngx_driver_fallback.bmp"
        fb_png = CAP_DIR / "upscale_ngx_driver_fallback_quality.png"
        fb_perf = WORK_DIR / "perf_ngx_driver_fallback.json"
        for p in (fb_bmp, fb_perf):
            if p.exists():
                p.unlink()
        rc_fb, log_fb = run_exe(
            [
                "--upscaler=dlss",
                "--upscale-quality=quality",
                "--dlss-preset=k",
                f"--replay-input={REPLAY_REC}",
                "--replay-mode=hybrid",
                "--capture-at-gframe=4560",
                f"--capture-bmp={fb_bmp}",
                f"--perf-log={fb_perf}",
                "--capture-exit",
                "--no-launcher",
            ],
            extra_env={
                "XBE_UPSCALER_PLUGINS_DIR": str(empty_dir),
                "XBE_NGX_DRIVER_FALLBACK": "1",
            },
            timeout=160,
        )
        staged_dll = ngx_cache_dir / "nvngx_dlss.dll"
        staged_stamp = ngx_cache_dir / "source.txt"
        fb_req = [
            "upscale/dlss: auto-staged signed NGX driver model",
            "Ready (NGX driver model)",
            "upscale/dlss: first EvaluateFeature succeeded",
        ]
        fb_missing = [tok for tok in fb_req if tok not in log_fb]
        m_fb = re.search(r"upscale/dlss: shutdown \(evaluated (\d+) frames\)", log_fb)
        fb_shutdown_frames = int(m_fb.group(1)) if m_fb else 0
        fb_perf_m = parse_perf_gpu_ms(fb_perf)
        fb_gameplay_frames = int(fb_perf_m.get("samples", 0)) if fb_perf_m.get("phase") == "gameplay_active" else 0
        fb_entry: dict[str, Any] = {
            "rc": rc_fb,
            "staged_dll_exists": staged_dll.is_file(),
            "staged_dll_bytes": staged_dll.stat().st_size if staged_dll.is_file() else 0,
            "staged_stamp_exists": staged_stamp.is_file(),
            "gameplay_frames": fb_gameplay_frames,
            "shutdown_eval_frames": fb_shutdown_frames,
            "missing_log_tokens": fb_missing,
            "perf": fb_perf_m,
        }
        if rc_fb == 0 and fb_bmp.is_file():
            fb_entry["image"] = bmp32_to_png_and_stats(fb_bmp, fb_png)
        else:
            summary["errors"].append(f"NGX driver fallback failed (rc={rc_fb}, bmp={fb_bmp.is_file()})")
        if not staged_dll.is_file() or not staged_stamp.is_file():
            summary["errors"].append("NGX driver fallback did not stage cache/ngx/nvngx_dlss.dll + source.txt")
        if fb_missing:
            summary["errors"].append(f"NGX driver fallback missing log tokens: {fb_missing}")
        if fb_gameplay_frames < 100:
            summary["errors"].append(f"NGX driver fallback recorded too few gameplay frames: {fb_gameplay_frames}")
        summary["ngx_driver_fallback"] = fb_entry
        print(
            f"      ngx_fallback    : rc={rc_fb} staged={fb_entry['staged_dll_bytes']} B "
            f"gameplay_frames={fb_gameplay_frames} missing={fb_missing}"
        )

        print("[3/3] Capturing Pre-Game Launcher Tab 1 (Upscaling) & Tab 5 (Tools) with bundled plugins...")
        for tab_idx, stem, extra_args in [
            (1, "launcher_bundled_tab1_upscaling", ["--upscaler=dlss"]),
            (5, "launcher_bundled_tab5_tools", []),
        ]:
            bmp = WORK_DIR / f"{stem}.bmp"
            png = CAP_DIR / f"{stem}.png"
            if bmp.exists():
                bmp.unlink()
            rc_l, log_l = run_exe(
                [
                    "--launcher",
                    f"--launcher-tab={tab_idx}",
                    *extra_args,
                    f"--capture-launcher-bmp={bmp}",
                    "--launcher-exit",
                ],
                timeout=30,
            )
            if rc_l != 0 or not bmp.is_file():
                summary["errors"].append(f"Bundled launcher tab {tab_idx} capture failed (rc={rc_l})")
                continue
            st = bmp32_to_png_and_stats(bmp, png)
            st["rc"] = rc_l
            st["clean_exit"] = "launcher: closed (launch_game=false)" in log_l
            summary["launcher_bundled_captures"][stem] = st
            print(f"      {stem}.png: {st['width']}x{st['height']} non_black={st['non_black_pct']}%")

        after_dumps = set(CRASH_DIR.glob("*.dmp")) if CRASH_DIR.is_dir() else set()
        new_dumps = sorted(p.name for p in (after_dumps - before_dumps))
        summary["new_crash_dumps"] = new_dumps
        if new_dumps:
            summary["errors"].append(f"New crash dumps created: {new_dumps}")
    finally:
        if orig_settings is not None:
            SETTINGS_FILE.write_bytes(orig_settings)
        elif SETTINGS_FILE.exists():
            SETTINGS_FILE.unlink()

    summary["all_passed"] = len(summary["errors"]) == 0
    SUMMARY_JSON.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"\nSummary written to: {SUMMARY_JSON.relative_to(REPO)}")
    if summary["all_passed"]:
        print("=== ALL 3 REAL VENDOR UPSCALERS + NGX FALLBACK VERIFIED ON Hardware GPU ===")
        return 0
    print("=== REAL UPSCALER VERIFICATION FAILED ===")
    for e in summary["errors"]:
        print("  ERROR:", e)
    return 1


if __name__ == "__main__":
    sys.exit(main())
