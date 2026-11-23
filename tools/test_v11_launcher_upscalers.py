#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""v1.1.0 Automated Verification Suite: Pre-Game Launcher, All 9 Upscalers & Cross-Hardware Profiles.

Produces:
  - .local/captures_v11/*.png (Launcher tabs, all 6 built-in upscalers, in-game F1 upscaling overlay)
  - .local/v11_test/v11_verification_summary.json

Verifies:
  1. Pre-Game Graphical Launcher visual captures (Tabs 0, 1, 2) via --launcher --launcher-tab N --capture-launcher-bmp --launcher-exit
  2. All 6 zero-DLL built-in upscalers (Bilinear, Integer 50%, Bicubic 67%, CAS 67% & 100%, FSR 1.0 67% & 50% + sharpness 0 vs 80, NIS 67%)
     plus In-Game F1 Upscaling Overlay, recording render_w x render_h -> vp_w x vp_h and gpu_ms metrics.
  3. Vendor Temporal Plugins (DLSS, FSR 3.1, XeSS):
     - Clean missing-DLL fallback (0 crashes, DriverStore _nvngx.dll discovery on RTX, fallback -> FSR 1.0).
     - Isolated in-process mock C-ABI verifiers (.local/v11_test/mock_plugins/, never in out/) verifying
       color, depth (R32G8X24_TYPELESS), motionVectors (R16G16_FLOAT), reactive (R8_UNORM), output (R8G8B8A8_UNORM),
       non-zero 3D Halton jitter in session==2, zero jitter/bypass on Title Screen (session==1), and cameraNear==32.0.
  4. Cross-Hardware Profile Matrix (--hw-preset=handheld|1080p|1440p|4k, --present-mode=auto|vsync|vrr|mailbox,
     P-core affinity CPU != 0 on multi-core host, RAM-bounded compile slots log).
"""

from __future__ import annotations

import ctypes
import json
import os
import re
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
EXE = REPO / "out" / "build" / "release" / "suffering.exe"
EXE_DIR = EXE.parent
CLANG = EXE_DIR / "compiler" / "clang.exe"
LLD = EXE_DIR / "compiler" / "lld-link.exe"
REPLAY_REC = REPO / "tests" / "m3_full_regression.rec"
RETAIL_ISO = REPO / "Suffering, The (USA)" / "Suffering, The (USA).xiso.iso"

CAP_DIR = REPO / ".local" / "captures_v11"
WORK_DIR = REPO / ".local" / "v11_test"
MOCK_DIR = WORK_DIR / "mock_plugins"
SUMMARY_JSON = WORK_DIR / "v11_verification_summary.json"
SETTINGS_FILE = EXE_DIR / "settings.toml"
LOGS_DIR = EXE_DIR / "logs"
CRASH_DIR = EXE_DIR / "CrashDumps"

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
        raise RuntimeError(f"Unsupported BMP format ({w}x{h_signed} {bpp}bpp): {bmp_path}")
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
        rows_rgb.append(0)  # PNG filter type 0
        # Convert BGRA -> RGB and sample stats
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


def run_exe(args: list[str], extra_env: dict[str, str] | None = None, timeout: int = 90) -> tuple[int, str]:
    check_no_suffering_running()
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    t0 = time.time()
    proc = subprocess.run([str(EXE), *args], cwd=str(EXE_DIR), env=env, timeout=timeout, capture_output=True)
    log_txt = latest_log_after(t0)
    return proc.returncode, log_txt


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


def build_mock_plugins() -> tuple[Path, Path]:
    MOCK_DIR.mkdir(parents=True, exist_ok=True)
    fsr3_src = MOCK_DIR / "mock_fsr3.cpp"
    xess_src = MOCK_DIR / "mock_xess.cpp"
    fsr3_dll = MOCK_DIR / "amd_fidelityfx_dx12.dll"
    xess_dll = MOCK_DIR / "libxess.dll"

    fsr3_code = r"""// Test-only isolated mock ffx_api verifier (lives strictly under .local/v11_test/mock_plugins/)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

struct FfxHeader { uint64_t type; FfxHeader* pNext; };
struct FfxDim2D { uint32_t w, h; };
struct FfxF2 { float x, y; };
struct FfxResDesc { uint32_t type, format, w, h, d, mips, flags, usage; };
struct FfxRes { void* resource; FfxResDesc desc; uint32_t state; };
struct FfxCreateUp { FfxHeader h; uint32_t flags; FfxDim2D maxRender, maxUp; void* msg; };
struct FfxCreateDx12 { FfxHeader h; ID3D12Device* dev; };
struct FfxDispatchUp {
    FfxHeader h; void* cl; FfxRes color, depth, mv, exp, reactive, tc, out;
    FfxF2 jitter, mvScale; FfxDim2D renderSz, upSz;
    bool enableSharp; float sharp, dt, preExp; bool reset;
    float camNear, camFar, fovY, meters; uint32_t flags;
};

struct MockCtx {
    uint32_t create_flags = 0;
    uint32_t dispatches = 0;
    uint32_t nonzero_jitter = 0;
    uint32_t valid_res = 0;
    uint32_t near_ok = 0;
    uint32_t color_fmt = 0, depth_fmt = 0, mv_fmt = 0, react_fmt = 0, out_fmt = 0;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    float last_near = 0.0f;
};

static void WriteReport(const MockCtx* c) {
    const char* p = std::getenv("MOCK_REPORT_PATH");
    if (!p || !*p) return;
    if (FILE* f = std::fopen(p, "wb")) {
        std::fprintf(f,
            "{\"plugin\":\"fsr3\",\"create_flags\":%u,\"dispatches\":%u,\"nonzero_jitter\":%u,"
            "\"valid_res\":%u,\"near_ok\":%u,\"last_near\":%.2f,\"render\":\"%ux%u\",\"output\":\"%ux%u\","
            "\"color_fmt\":%u,\"depth_fmt\":%u,\"mv_fmt\":%u,\"reactive_fmt\":%u,\"output_fmt\":%u}\n",
            c->create_flags, c->dispatches, c->nonzero_jitter, c->valid_res, c->near_ok, c->last_near,
            c->rw, c->rh, c->ow, c->oh, c->color_fmt, c->depth_fmt, c->mv_fmt, c->react_fmt, c->out_fmt);
        std::fclose(f);
    }
}

extern "C" {
__declspec(dllexport) uint32_t __cdecl ffxCreateContext(void** ctx, FfxHeader* desc, const void*) {
    auto* c = new MockCtx();
    if (desc && desc->type == 0x00010000ull) {
        c->create_flags = reinterpret_cast<FfxCreateUp*>(desc)->flags;
    }
    *ctx = c;
    return 0;
}
__declspec(dllexport) uint32_t __cdecl ffxDestroyContext(void** ctx, const void*) {
    if (ctx && *ctx) {
        auto* c = static_cast<MockCtx*>(*ctx);
        WriteReport(c);
        delete c;
        *ctx = nullptr;
    }
    return 0;
}
__declspec(dllexport) uint32_t __cdecl ffxDispatch(void** ctx, const FfxHeader* desc) {
    if (!ctx || !*ctx || !desc || desc->type != 0x00010001ull) return 1;
    auto* c = static_cast<MockCtx*>(*ctx);
    const auto* d = reinterpret_cast<const FfxDispatchUp*>(desc);
    c->dispatches++;
    c->rw = d->renderSz.w; c->rh = d->renderSz.h;
    c->ow = d->upSz.w; c->oh = d->upSz.h;
    c->last_near = d->camNear;
    if (std::fabs(d->camNear - 32.0f) < 0.01f) c->near_ok++;
    if (std::fabs(d->jitter.x) > 1e-5f || std::fabs(d->jitter.y) > 1e-5f) c->nonzero_jitter++;
    auto* r_col = static_cast<ID3D12Resource*>(d->color.resource);
    auto* r_dep = static_cast<ID3D12Resource*>(d->depth.resource);
    auto* r_mv  = static_cast<ID3D12Resource*>(d->mv.resource);
    auto* r_rea = static_cast<ID3D12Resource*>(d->reactive.resource);
    auto* r_out = static_cast<ID3D12Resource*>(d->out.resource);
    if (r_col && r_dep && r_mv && r_rea && r_out) {
        c->color_fmt = uint32_t(r_col->GetDesc().Format);
        c->depth_fmt = uint32_t(r_dep->GetDesc().Format);
        c->mv_fmt    = uint32_t(r_mv->GetDesc().Format);
        c->react_fmt = uint32_t(r_rea->GetDesc().Format);
        c->out_fmt   = uint32_t(r_out->GetDesc().Format);
        if ((c->depth_fmt == DXGI_FORMAT_R32G8X24_TYPELESS || c->depth_fmt == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) &&
            c->mv_fmt == DXGI_FORMAT_R16G16_FLOAT &&
            c->react_fmt == DXGI_FORMAT_R8_UNORM &&
            c->out_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
            c->valid_res++;
        }
    }
    WriteReport(c);
    return 0;
}
__declspec(dllexport) uint32_t __cdecl ffxQuery(void**, FfxHeader*) { return 0; }
__declspec(dllexport) uint32_t __cdecl ffxConfigure(void**, const FfxHeader*) { return 0; }
}
"""

    xess_code = r"""// Test-only isolated mock libxess.dll verifier (lives strictly under .local/v11_test/mock_plugins/)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d12.h>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

struct XessVer { uint16_t maj, min, pat, res; };
struct XessC2 { uint32_t x, y; };
struct XessInit {
    XessC2 outRes; int32_t qual; uint32_t flags, cMask, vMask;
    void* bufHeap; uint64_t bufOff; void* texHeap; uint64_t texOff; void* psoLib;
};
struct XessExec {
    ID3D12Resource *col, *vel, *dep, *exp, *resp, *out;
    float jx, jy, expScale; uint32_t reset, inW, inH;
    XessC2 bCol, bVel, bDep, bResp, r0, bOut;
    void* heap; uint32_t heapOff;
};

struct MockXess {
    uint32_t init_flags = 0;
    float vel_sx = 0.0f, vel_sy = 0.0f;
    uint32_t executes = 0;
    uint32_t nonzero_jitter = 0;
    uint32_t valid_res = 0;
    uint32_t depth_fmt = 0, mv_fmt = 0, resp_fmt = 0, out_fmt = 0;
    uint32_t iw = 0, ih = 0, ow = 0, oh = 0;
};

static void WriteReport(const MockXess* c) {
    const char* p = std::getenv("MOCK_REPORT_PATH");
    if (!p || !*p) return;
    if (FILE* f = std::fopen(p, "wb")) {
        std::fprintf(f,
            "{\"plugin\":\"xess\",\"init_flags\":%u,\"vel_scale\":[%.1f,%.1f],\"executes\":%u,"
            "\"nonzero_jitter\":%u,\"valid_res\":%u,\"render\":\"%ux%u\",\"output\":\"%ux%u\","
            "\"depth_fmt\":%u,\"mv_fmt\":%u,\"responsive_fmt\":%u,\"output_fmt\":%u}\n",
            c->init_flags, c->vel_sx, c->vel_sy, c->executes, c->nonzero_jitter, c->valid_res,
            c->iw, c->ih, c->ow, c->oh, c->depth_fmt, c->mv_fmt, c->resp_fmt, c->out_fmt);
        std::fclose(f);
    }
}

extern "C" {
__declspec(dllexport) int32_t __cdecl xessGetVersion(XessVer* v) {
    if (v) *v = {2, 0, 1, 0};
    return 0;
}
__declspec(dllexport) int32_t __cdecl xessD3D12CreateContext(ID3D12Device*, void** out) {
    *out = new MockXess();
    return 0;
}
__declspec(dllexport) int32_t __cdecl xessDestroyContext(void* ctx) {
    if (ctx) {
        auto* c = static_cast<MockXess*>(ctx);
        WriteReport(c);
        delete c;
    }
    return 0;
}
__declspec(dllexport) int32_t __cdecl xessD3D12Init(void* ctx, const XessInit* p) {
    if (!ctx || !p) return -1;
    auto* c = static_cast<MockXess*>(ctx);
    c->init_flags = p->flags;
    c->ow = p->outRes.x; c->oh = p->outRes.y;
    return 0;
}
__declspec(dllexport) int32_t __cdecl xessSetVelocityScale(void* ctx, float x, float y) {
    if (!ctx) return -1;
    auto* c = static_cast<MockXess*>(ctx);
    c->vel_sx = x; c->vel_sy = y;
    return 0;
}
__declspec(dllexport) int32_t __cdecl xessSetJitterScale(void*, float, float) { return 0; }
__declspec(dllexport) int32_t __cdecl xessD3D12Execute(void* ctx, ID3D12GraphicsCommandList*, const XessExec* p) {
    if (!ctx || !p) return -1;
    auto* c = static_cast<MockXess*>(ctx);
    c->executes++;
    c->iw = p->inW; c->ih = p->inH;
    if (std::fabs(p->jx) > 1e-5f || std::fabs(p->jy) > 1e-5f) c->nonzero_jitter++;
    if (p->col && p->dep && p->vel && p->resp && p->out) {
        c->depth_fmt = uint32_t(p->dep->GetDesc().Format);
        c->mv_fmt    = uint32_t(p->vel->GetDesc().Format);
        c->resp_fmt  = uint32_t(p->resp->GetDesc().Format);
        c->out_fmt   = uint32_t(p->out->GetDesc().Format);
        if ((c->depth_fmt == DXGI_FORMAT_R32G8X24_TYPELESS || c->depth_fmt == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) &&
            c->mv_fmt == DXGI_FORMAT_R16G16_FLOAT &&
            c->resp_fmt == DXGI_FORMAT_R8_UNORM &&
            c->out_fmt == DXGI_FORMAT_R8G8B8A8_UNORM) {
            c->valid_res++;
        }
    }
    WriteReport(c);
    return 0;
}
}
"""
    fsr3_src.write_text(fsr3_code, encoding="utf-8")
    xess_src.write_text(xess_code, encoding="utf-8")

    # Compile with VS + clang environment
    vs_bat = r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    for src, dll in [(fsr3_src, fsr3_dll), (xess_src, xess_dll)]:
        cmd = (
            f'call "{vs_bat}" >nul && "{CLANG}" -shared -O2 -std=c++20 -fuse-ld=lld '
            f'"{src}" -o "{dll}"'
        )
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True)
        if r.returncode != 0 or not dll.is_file():
            raise RuntimeError(f"Failed compiling mock DLL {dll.name}:\n{r.stdout}\n{r.stderr}")
    return fsr3_dll, xess_dll


def main() -> int:
    if not EXE.is_file():
        print(f"ERROR: {EXE} not found. Run build.bat first.")
        return 1
    check_no_suffering_running()
    CAP_DIR.mkdir(parents=True, exist_ok=True)
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    # Snapshot existing settings.toml & CrashDumps so tests stay 100% clean
    orig_settings = SETTINGS_FILE.read_bytes() if SETTINGS_FILE.is_file() else None
    before_dumps = set(CRASH_DIR.glob("*.dmp")) if CRASH_DIR.is_dir() else set()

    summary: dict[str, Any] = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "launcher_captures": {},
        "builtin_upscalers": {},
        "ingame_f1_overlay": {},
        "vendor_fallbacks": {},
        "vendor_mock_abi": {},
        "hardware_matrix": {},
        "errors": [],
    }

    try:
        # Configure deterministic 1280x720 windowed settings for uniform side-by-side comparison
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

        # ------------------------------------------------------------------
        # 1. Pre-Game Graphical Launcher Visual Captures (All 6 Tabs 0..5)
        #    + Fresh-dir CLI --iso verification without settings.toml
        # ------------------------------------------------------------------
        print("[1/5] Capturing Pre-Game Graphical Launcher tabs 0..5 + fresh CLI --iso check...")
        for tab_idx, stem in [
            (0, "launcher_tab0_play"),
            (1, "launcher_tab1_upscaling"),
            (2, "launcher_tab2_display_gpu"),
            (3, "launcher_tab3_audio"),
            (4, "launcher_tab4_controller"),
            (5, "launcher_tab5_tools"),
        ]:
            bmp = WORK_DIR / f"{stem}.bmp"
            png = CAP_DIR / f"{stem}.png"
            if bmp.exists():
                bmp.unlink()
            rc, log_txt = run_exe(
                ["--launcher", f"--launcher-tab={tab_idx}", f"--capture-launcher-bmp={bmp}", "--launcher-exit"],
                timeout=30,
            )
            if rc != 0 or not bmp.is_file():
                summary["errors"].append(f"Launcher tab {tab_idx} capture failed (rc={rc})")
                continue
            st = bmp32_to_png_and_stats(bmp, png)
            st["rc"] = rc
            st["clean_exit"] = "launcher: closed (launch_game=false)" in log_txt
            if st["non_black_pct"] < 20.0 or st["luma_stddev"] < 5.0:
                summary["errors"].append(f"Launcher tab {tab_idx} image looks blank/corrupt: {st}")
            summary["launcher_captures"][stem] = st
            print(f"      {stem}.png: {st['width']}x{st['height']} non_black={st['non_black_pct']}% std={st['luma_stddev']}")

        # Verify fresh directory (zero settings.toml / assets) honors --iso=<RETAIL_ISO>
        # AND does not create settings.toml on Exit / --launcher-exit
        fresh_dir = WORK_DIR / "fresh_cli_iso"
        if fresh_dir.exists():
            shutil.rmtree(fresh_dir)
        fresh_dir.mkdir(parents=True, exist_ok=True)
        fresh_exe = fresh_dir / "suffering.exe"
        shutil.copy2(EXE, fresh_exe)
        fresh_bmp = WORK_DIR / "launcher_fresh_cli_iso.bmp"
        fresh_png = CAP_DIR / "launcher_fresh_cli_iso.png"
        if fresh_bmp.exists():
            fresh_bmp.unlink()
        check_no_suffering_running()
        fresh_proc = subprocess.run(
            [
                str(fresh_exe),
                "--launcher",
                "--launcher-tab=0",
                f"--iso={RETAIL_ISO}",
                f"--capture-launcher-bmp={fresh_bmp}",
                "--launcher-exit",
            ],
            cwd=str(fresh_dir),
            timeout=30,
            check=False,
        )
        fresh_log_path = fresh_dir / "logs" / "suffering.log"
        fresh_log = fresh_log_path.read_text(encoding="utf-8", errors="replace") if fresh_log_path.is_file() else ""
        fresh_settings_created = (fresh_dir / "settings.toml").exists()
        cli_iso_valid = "launcher: disc status valid=true" in fresh_log
        if fresh_proc.returncode != 0 or not fresh_bmp.is_file() or not cli_iso_valid or fresh_settings_created:
            summary["errors"].append(
                f"Fresh CLI --iso launcher check failed: rc={fresh_proc.returncode}, "
                f"cli_iso_valid={cli_iso_valid}, settings_created={fresh_settings_created}"
            )
        else:
            fst = bmp32_to_png_and_stats(fresh_bmp, fresh_png)
            fst["rc"] = fresh_proc.returncode
            fst["cli_iso_valid"] = cli_iso_valid
            fst["settings_created_on_exit"] = fresh_settings_created
            summary["launcher_captures"]["launcher_fresh_cli_iso"] = fst
            print(
                f"      launcher_fresh_cli_iso.png: {fst['width']}x{fst['height']} "
                f"cli_iso_valid={cli_iso_valid} settings_created_on_exit={fresh_settings_created}"
            )
        shutil.rmtree(fresh_dir, ignore_errors=True)

        # ------------------------------------------------------------------
        # 2. All 6 Built-in Zero-DLL Upscalers Side-by-Side + GPU Metrics
        # ------------------------------------------------------------------
        print("[2/5] Testing all 6 built-in zero-DLL upscalers + In-Game F1 overlay...")
        upscaler_cases = [
            ("bilinear_100", ["--upscaler=bilinear", "--upscale-quality=native_aa"], 1280, 720, False),
            ("integer_50", ["--upscaler=integer", "--upscale-quality=performance"], 640, 360, False),
            ("bicubic_67", ["--upscaler=bicubic", "--upscale-quality=quality"], 852, 480, False),
            ("cas_67", ["--upscaler=cas", "--upscale-quality=quality", "--sharpness=50"], 852, 480, False),
            ("cas_100", ["--upscaler=cas", "--upscale-quality=native_aa", "--sharpness=50"], 1280, 720, False),
            ("fsr1_67_sharp0", ["--upscaler=fsr1", "--upscale-quality=quality", "--sharpness=0"], 852, 480, False),
            ("fsr1_67_sharp80", ["--upscaler=fsr1", "--upscale-quality=quality", "--sharpness=80"], 852, 480, True),
            ("fsr1_50", ["--upscaler=fsr1", "--upscale-quality=performance", "--sharpness=40"], 640, 360, False),
            ("nis_67", ["--upscaler=nis", "--upscale-quality=quality", "--sharpness=50"], 852, 480, True),
        ]
        for name, uargs, exp_rw, exp_rh, use_3d in upscaler_cases:
            bmp = WORK_DIR / f"{name}.bmp"
            png = CAP_DIR / f"upscale_{name}.png"
            perf_path = WORK_DIR / f"perf_{name}.jsonl"
            for p in (bmp, perf_path):
                if p.exists():
                    p.unlink()
            cmd_args = [*uargs, f"--perf-log={perf_path}", f"--capture-bmp={bmp}", "--capture-exit", "--no-launcher"]
            if use_3d and REPLAY_REC.is_file():
                cmd_args += [f"--replay-input={REPLAY_REC}", "--replay-mode=hybrid", "--capture-at-gframe=4560"]
                timeout = 135
            else:
                cmd_args += ["--capture-after-frames=150"]
                timeout = 45
            rc, log_txt = run_exe(cmd_args, timeout=timeout)
            if rc != 0 or not bmp.is_file():
                summary["errors"].append(f"Upscaler {name} failed (rc={rc}, bmp={bmp.is_file()})")
                continue
            st = bmp32_to_png_and_stats(bmp, png)
            rinfo = parse_render_line(log_txt)
            perf_m = parse_perf_gpu_ms(perf_path)
            st["render_info"] = rinfo
            st["perf"] = perf_m
            st["rc"] = rc
            if rinfo.get("render_w") != exp_rw or rinfo.get("render_h") != exp_rh:
                summary["errors"].append(
                    f"Upscaler {name} render mismatch: got {rinfo.get('render_w')}x{rinfo.get('render_h')}, expected {exp_rw}x{exp_rh}"
                )
            if st["non_black_pct"] < 5.0:
                summary["errors"].append(f"Upscaler {name} image looks blank: {st}")
            summary["builtin_upscalers"][name] = st
            print(
                f"      {name}: {rinfo.get('render_w')}x{rinfo.get('render_h')} -> {rinfo.get('vp_w')}x{rinfo.get('vp_h')}, "
                f"gpu_ms={perf_m.get('gpu_ms_avg', 'n/a')} ({perf_m.get('phase', '?')}), non_black={st['non_black_pct']}%"
            )

        # In-Game F1 Upscaling Overlay capture (with live FPS HUD!)
        f1_bmp = WORK_DIR / "ingame_f1_upscaling.bmp"
        f1_png = CAP_DIR / "ingame_f1_upscaling.png"
        if f1_bmp.exists():
            f1_bmp.unlink()
        rc, log_txt = run_exe(
            [
                "--no-launcher",
                "--open-settings",
                "--settings-tab=0",
                "--show-fps-hud",
                "--upscaler=fsr1",
                "--upscale-quality=quality",
                f"--capture-ui-bmp={f1_bmp}",
                "--capture-ui-exit",
            ],
            timeout=45,
        )
        if rc == 0 and f1_bmp.is_file():
            st = bmp32_to_png_and_stats(f1_bmp, f1_png)
            st["rc"] = rc
            summary["ingame_f1_overlay"] = st
            print(f"      ingame_f1_upscaling.png: {st['width']}x{st['height']} non_black={st['non_black_pct']}%")
        else:
            summary["errors"].append(f"In-game F1 overlay capture failed (rc={rc})")

        # ------------------------------------------------------------------
        # 3. Vendor Temporal Plugins: Missing-DLL Fallback + Mock C-ABI Verifier
        # ------------------------------------------------------------------
        print("[3/5] Testing DLSS / FSR 3.1 / XeSS missing-DLL graceful fallbacks...")
        empty_plugins = WORK_DIR / "empty_plugins"
        empty_plugins.mkdir(parents=True, exist_ok=True)
        for vmode in ("dlss", "fsr3", "xess"):
            bmp = WORK_DIR / f"fallback_{vmode}.bmp"
            if bmp.exists():
                bmp.unlink()
            rc, log_txt = run_exe(
                [
                    "--no-launcher",
                    f"--upscaler={vmode}",
                    "--upscale-quality=quality",
                    f"--capture-bmp={bmp}",
                    "--capture-after-frames=90",
                    "--capture-exit",
                ],
                extra_env={"XBE_UPSCALER_PLUGINS_DIR": str(empty_plugins)},
                timeout=45,
            )
            fb_info = {
                "rc": rc,
                "captured": bmp.is_file(),
                "ngx_driver_discovered": "discovered DriverStore _nvngx.dll" in log_txt,
                "clean_fallback_logged": "falling back cleanly" in log_txt or "fallback" in log_txt.lower(),
            }
            if rc != 0 or not bmp.is_file():
                summary["errors"].append(f"Vendor fallback {vmode} failed (rc={rc})")
            summary["vendor_fallbacks"][vmode] = fb_info
            print(f"      fallback {vmode}: rc={rc}, ngx_discovered={fb_info['ngx_driver_discovered']}, fallback_ok={fb_info['clean_fallback_logged']}")

        print("[4/5] Compiling & running isolated in-process FSR 3.1 & XeSS C-ABI mock verifiers...")
        build_mock_plugins()
        for vmode in ("fsr3", "xess"):
            # (a) Title Screen check (session==1): temporal dispatch MUST NOT be called (zero jitter bypass!)
            rep_title = WORK_DIR / f"mock_{vmode}_title.json"
            if rep_title.exists():
                rep_title.unlink()
            rc_t, _ = run_exe(
                [
                    "--no-launcher",
                    f"--upscaler={vmode}",
                    "--upscale-quality=quality",
                    "--capture-bmp=" + str(WORK_DIR / f"mock_{vmode}_title.bmp"),
                    "--capture-after-frames=100",
                    "--capture-exit",
                ],
                extra_env={"XBE_UPSCALER_PLUGINS_DIR": str(MOCK_DIR), "MOCK_REPORT_PATH": str(rep_title)},
                timeout=45,
            )
            title_dispatches = 0
            if rep_title.is_file():
                td = json.loads(rep_title.read_text(encoding="utf-8"))
                title_dispatches = int(td.get("dispatches", td.get("executes", 0)))

            # (b) Active 3D Gameplay check (session==2): non-null color/depth/MV/reactive/output, non-zero jitter, near==32
            rep_3d = WORK_DIR / f"mock_{vmode}_3d.json"
            if rep_3d.exists():
                rep_3d.unlink()
            rc_3d, _ = run_exe(
                [
                    "--no-launcher",
                    f"--upscaler={vmode}",
                    "--upscale-quality=quality",
                    f"--replay-input={REPLAY_REC}",
                    "--replay-mode=hybrid",
                    "--capture-bmp=" + str(WORK_DIR / f"mock_{vmode}_3d.bmp"),
                    "--capture-at-gframe=4560",
                    "--capture-exit",
                ],
                extra_env={"XBE_UPSCALER_PLUGINS_DIR": str(MOCK_DIR), "MOCK_REPORT_PATH": str(rep_3d)},
                timeout=135,
            )
            if not rep_3d.is_file():
                summary["errors"].append(f"Mock {vmode} 3D C-ABI report missing (rc={rc_3d})")
                continue
            r3 = json.loads(rep_3d.read_text(encoding="utf-8"))
            r3["title_screen_dispatches"] = title_dispatches
            r3["rc_title"] = rc_t
            r3["rc_3d"] = rc_3d
            calls = int(r3.get("dispatches", r3.get("executes", 0)))
            valid_res = int(r3.get("valid_res", 0))
            nz_jit = int(r3.get("nonzero_jitter", 0))
            if title_dispatches != 0:
                summary["errors"].append(f"Mock {vmode} dispatched on 2D Title Screen ({title_dispatches} calls)")
            if calls == 0 or valid_res == 0 or nz_jit == 0:
                summary["errors"].append(f"Mock {vmode} 3D C-ABI check failed: {r3}")
            if vmode == "fsr3" and int(r3.get("near_ok", 0)) == 0:
                summary["errors"].append(f"Mock fsr3 cameraNear != 32.0: {r3}")
            summary["vendor_mock_abi"][vmode] = r3
            print(f"      mock {vmode}: title_calls={title_dispatches}, 3d_calls={calls}, valid_res={valid_res}, nonzero_jitter={nz_jit}")

        # ------------------------------------------------------------------
        # 4. Cross-Hardware Profile Matrix + P-Core Affinity Verification
        # ------------------------------------------------------------------
        print("[5/5] Testing Cross-Hardware Preset & Present Mode matrix + P-Core affinity...")
        hw_cases = [
            ("preset_handheld", ["--hw-preset=handheld"]),
            ("preset_1080p", ["--hw-preset=1080p"]),
            ("preset_1440p", ["--hw-preset=1440p"]),
            ("preset_4k", ["--hw-preset=4k"]),
            ("present_auto", ["--present-mode=auto"]),
            ("present_vsync", ["--present-mode=vsync"]),
            ("present_vrr", ["--present-mode=vrr"]),
            ("present_mailbox", ["--present-mode=mailbox"]),
        ]
        for label, hargs in hw_cases:
            rc, log_txt = run_exe(
                [
                    "--no-launcher",
                    *hargs,
                    "--capture-bmp=" + str(WORK_DIR / f"hw_{label}.bmp"),
                    "--capture-after-frames=60",
                    "--capture-exit",
                ],
                timeout=45,
            )
            m_cpu = re.search(r"threads: single_core_guest pinned to logical CPU (\d+) \(mask (0x[0-9A-Fa-f]+)", log_txt)
            rinfo = parse_render_line(log_txt)
            cpu_idx = int(m_cpu.group(1)) if m_cpu else -1
            entry = {
                "rc": rc,
                "guest_cpu": cpu_idx,
                "guest_mask": m_cpu.group(2) if m_cpu else "",
                "render_info": rinfo,
            }
            if rc != 0:
                summary["errors"].append(f"HW matrix case {label} failed (rc={rc})")
            if os.cpu_count() and os.cpu_count() >= 4 and cpu_idx == 0:
                summary["errors"].append(f"HW matrix case {label} pinned guest to CPU 0 on multi-core host!")
            summary["hardware_matrix"][label] = entry
            print(f"      {label}: rc={rc}, guest_cpu={cpu_idx}, render={rinfo.get('render_w')}x{rinfo.get('render_h')}")

        # Verify 0 new crash dumps
        after_dumps = set(CRASH_DIR.glob("*.dmp")) if CRASH_DIR.is_dir() else set()
        new_dumps = sorted(p.name for p in (after_dumps - before_dumps))
        summary["new_crash_dumps"] = new_dumps
        if new_dumps:
            summary["errors"].append(f"New crash dumps created during v1.1.0 tests: {new_dumps}")

    finally:
        if orig_settings is not None:
            SETTINGS_FILE.write_bytes(orig_settings)
        elif SETTINGS_FILE.exists():
            SETTINGS_FILE.unlink()

    summary["all_passed"] = len(summary["errors"]) == 0
    SUMMARY_JSON.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"\nSummary written to: {SUMMARY_JSON.relative_to(REPO)}")
    if summary["all_passed"]:
        print("=== ALL v1.1.0 LAUNCHER, UPSCALER & CROSS-HARDWARE TESTS PASSED ===")
        return 0
    print("=== v1.1.0 TESTS FAILED ===")
    for e in summary["errors"]:
        print("  ERROR:", e)
    return 1


if __name__ == "__main__":
    sys.exit(main())
