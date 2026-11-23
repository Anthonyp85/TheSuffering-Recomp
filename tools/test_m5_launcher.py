#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Milestone 5 End-to-End Automated Verification Suite for The Suffering (USA).

Validates:
  A. Deep read-only XISO/XBE validator matrix (--validate-iso):
     - Valid retail .xiso.iso (0xEB) -> OK
     - Missing path -> ERR_NOT_FOUND
     - Compressed .7z archive (read-only) -> ERR_COMPRESSED_ARCHIVE
     - Non-Xbox / zeroed .iso -> ERR_NOT_XISO
     - Synthetic wrong-Title-ID XISO (0xDEADBEEF) -> ERR_WRONG_GAME
     - Synthetic Redump 0x7D XISO (SHA-1 55c902f6...) -> OK
  B. Windows Integration:
     - PE .rsrc verification on suffering.exe (RT_GROUP_ICON #1 + VS_VERSION_INFO)
     - Non-interactive --create-shortcut into .local/m5_test/shortcut/ (never touches user Desktop)
  C. Full unpacked release.bat end-user simulation in .local/m5_release_test/:
     - Runs release.bat, verifies suffering.zip contents (incl. vcruntime140_1.dll, zero copyrighted/DLL/XBE files)
     - Unpacks to .local/m5_release_test/ and deletes temporary root suffering.zip
     - Launch #1 (no suffering.dll, no settings.toml): compiles all 64 partitions + links suffering.dll + boots to Title Screen
     - Launch #2 (via settings.toml iso_path, no --iso): verifies suffering.dll mtime unchanged (instant reuse) + boots cleanly
  D. Dear ImGui Settings Overlay Visual Capture & Settings Persistence:
     - Captures composited backbuffer (.local/captures_m5/settings_overlay.bmp + .png) with Settings overlay & FPS HUD open
     - Verifies non-persistent CLI override (--fps-cap 30 does not overwrite settings.toml fps_cap = 60)
"""

from __future__ import annotations

import ctypes
import json
import os
import shutil
import struct
import subprocess
import sys
import time
import zipfile
import zlib
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
RELEASE_EXE = REPO / "out" / "build" / "release" / "suffering.exe"
RETAIL_ISO = Path(os.environ["XBE_ISO_PATH"]) if os.environ.get("XBE_ISO_PATH") else (REPO / "Suffering, The (USA)" / "Suffering, The (USA).xiso.iso")
PRIVATE_XBE = REPO / ".local" / "xbe" / "default.xbe"
ROUNDTRIP_REC = REPO / ".local" / "captures_m3" / "roundtrip.rec"

TEST_DIR = REPO / ".local" / "m5_test"
RELEASE_TEST_DIR = REPO / ".local" / "m5_release_test"
CAP_DIR = REPO / ".local" / "captures_m5"
SUMMARY_JSON = TEST_DIR / "m5_verification_summary.json"

PROTECTED_MEDIA = [p for p in [RETAIL_ISO] if p.is_file()]

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
                name = pe.szExeFile.lower()
                if name.startswith("suffering") and name.endswith(".exe"):
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


def build_synthetic_xiso(out_path: Path, xbe_bytes: bytes) -> None:
    """Create a minimal valid single-file XISO containing /Default.xbe."""
    sector_size = 2048
    root_sector = 34
    xbe_sector = 35
    xbe_sectors = (len(xbe_bytes) + sector_size - 1) // sector_size
    total_sectors = xbe_sector + xbe_sectors + 2
    img = bytearray(total_sectors * sector_size)

    # XDVDFS Volume Descriptor at sector 32 (0x10000)
    vd_off = 32 * sector_size
    magic = b"MICROSOFT*XBOX*MEDIA"
    img[vd_off : vd_off + 20] = magic
    struct.pack_into("<II", img, vd_off + 20, root_sector, 2048)
    img[vd_off + 0x7EC : vd_off + 0x7EC + 20] = magic

    # Root directory entry at sector 34: single file "Default.xbe"
    fname = b"Default.xbe"
    dir_off = root_sector * sector_size
    struct.pack_into("<HHIIBB", img, dir_off, 0, 0, xbe_sector, len(xbe_bytes), 0x20, len(fname))
    img[dir_off + 14 : dir_off + 14 + len(fname)] = fname

    # XBE payload at sector 35
    xbe_off = xbe_sector * sector_size
    img[xbe_off : xbe_off + len(xbe_bytes)] = xbe_bytes
    out_path.write_bytes(bytes(img))


def build_mock_wrong_game_xbe() -> bytes:
    """Create a minimal valid XBEH header with Title ID 0xDEADBEEF and Title Name 'Mock Other Game'."""
    buf = bytearray(0x400)
    buf[0:4] = b"XBEH"
    base = 0x00010000
    cert_va = base + 0x184
    struct.pack_into("<I", buf, 0x104, base)          # base_va
    struct.pack_into("<I", buf, 0x108, 0x400)         # header_size
    struct.pack_into("<I", buf, 0x10C, 0x10000)       # image_size
    struct.pack_into("<I", buf, 0x118, cert_va)       # cert_va
    struct.pack_into("<I", buf, 0x11C, 0)             # nsec
    struct.pack_into("<I", buf, 0x120, base + 0x380)  # sec_addr
    struct.pack_into("<I", buf, 0x128, (base + 0x1000) ^ 0xA8FC57AB)  # retail entry
    struct.pack_into("<I", buf, 0x158, (base + 0x2000) ^ 0x5B6D40B6)  # retail kernel thunk
    # Certificate at 0x184
    struct.pack_into("<I", buf, 0x184 + 0x00, 0x1EC)
    struct.pack_into("<I", buf, 0x184 + 0x08, 0xDEADBEEF)  # Title ID
    name_u16 = "Mock Other Game".encode("utf-16-le")
    buf[0x184 + 0x0C : 0x184 + 0x0C + len(name_u16)] = name_u16
    return bytes(buf)


def run_validate_iso(target: Path) -> tuple[int, dict[str, Any]]:
    proc = subprocess.run(
        [str(RELEASE_EXE), "--validate-iso", str(target)],
        cwd=str(RELEASE_EXE.parent),
        capture_output=True,
        text=True,
        timeout=30,
    )
    line = proc.stdout.strip().splitlines()[-1] if proc.stdout.strip() else "{}"
    return proc.returncode, json.loads(line)


def check_pe_resources(exe_path: Path) -> dict[str, Any]:
    """Verify RT_GROUP_ICON (#1) and VS_VERSION_INFO ('The Suffering') via Win32 API."""
    k32 = ctypes.windll.kernel32
    ver = ctypes.windll.version
    LOAD_LIBRARY_AS_DATAFILE = 0x00000002
    RT_GROUP_ICON = 14
    RT_VERSION = 16

    k32.LoadLibraryExW.restype = ctypes.c_void_p
    k32.FindResourceW.restype = ctypes.c_void_p
    k32.FindResourceW.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    k32.FreeLibrary.argtypes = [ctypes.c_void_p]

    hmod = k32.LoadLibraryExW(str(exe_path), None, LOAD_LIBRARY_AS_DATAFILE)
    if not hmod:
        raise RuntimeError(f"LoadLibraryExW failed on {exe_path}")
    try:
        has_icon = bool(k32.FindResourceW(hmod, ctypes.c_void_p(1), ctypes.c_void_p(RT_GROUP_ICON)))
        has_version = bool(k32.FindResourceW(hmod, ctypes.c_void_p(1), ctypes.c_void_p(RT_VERSION)))
    finally:
        k32.FreeLibrary(hmod)

    dummy = ctypes.c_ulong(0)
    sz = ver.GetFileVersionInfoSizeW(str(exe_path), ctypes.byref(dummy))
    strings: dict[str, str] = {}
    if sz > 0:
        buf = ctypes.create_string_buffer(sz)
        if ver.GetFileVersionInfoW(str(exe_path), 0, sz, buf):
            for key in ("ProductName", "FileDescription", "OriginalFilename", "LegalCopyright"):
                lp = ctypes.c_void_p()
                ln = ctypes.c_uint(0)
                if ver.VerQueryValueW(buf, f"\\StringFileInfo\\040904b0\\{key}", ctypes.byref(lp), ctypes.byref(ln)):
                    if lp.value and ln.value:
                        strings[key] = ctypes.wstring_at(lp.value, ln.value - 1)
    return {
        "has_group_icon_1": has_icon,
        "has_version_info_1": has_version,
        "version_strings": strings,
    }


def bmp32_to_png(bmp_path: Path, png_path: Path) -> dict[str, Any]:
    """Convert top-down 32-bit BGRA BMP to PNG and compute basic visual stats."""
    data = bmp_path.read_bytes()
    if len(data) < 54 or data[0:2] != b"BM":
        raise RuntimeError(f"Invalid BMP header: {bmp_path}")
    px_off = struct.unpack_from("<I", data, 10)[0]
    w, h_signed = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 32:
        raise RuntimeError(f"Expected 32bpp BMP, got {bpp}")
    top_down = h_signed < 0
    h = abs(h_signed)
    stride = w * 4
    raw = bytearray()
    non_black = 0
    r_sum = g_sum = b_sum = 0
    total_px = w * h
    for y in range(h):
        src_y = y if top_down else (h - 1 - y)
        row_off = px_off + src_y * stride
        row = data[row_off : row_off + stride]
        raw.append(0)
        out_row = bytearray(stride)
        for x in range(w):
            i = x * 4
            b, g, r = row[i], row[i + 1], row[i + 2]
            out_row[i] = r
            out_row[i + 1] = g
            out_row[i + 2] = b
            out_row[i + 3] = 255
            if r > 8 or g > 8 or b > 8:
                non_black += 1
            r_sum += r
            g_sum += g
            b_sum += b
        raw.extend(out_row)

    def chunk(tag: bytes, payload: bytes) -> bytes:
        crc = zlib.crc32(tag + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", crc)

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), level=6)
    png_bytes = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")
    png_path.write_bytes(png_bytes)
    return {
        "width": w,
        "height": h,
        "non_black_pct": round(100.0 * non_black / max(1, total_px), 2),
        "mean_rgb": [round(r_sum / total_px, 2), round(g_sum / total_px, 2), round(b_sum / total_px, 2)],
        "png_bytes": len(png_bytes),
    }


def parse_state_jsonl(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {"exists": False}
    checkpoints = []
    summary = None
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            continue
        obj = json.loads(line)
        if obj.get("type") == "checkpoint":
            checkpoints.append(obj)
        elif obj.get("type") == "summary":
            summary = obj
    max_gframe = max((c.get("gframe", 0) for c in checkpoints), default=0)
    return {
        "exists": True,
        "checkpoints": len(checkpoints),
        "max_gframe": max_gframe,
        "summary": summary,
    }


def main() -> int:
    check_no_suffering_running()
    media_before = snapshot_protected_media()

    TEST_DIR.mkdir(parents=True, exist_ok=True)
    CAP_DIR.mkdir(parents=True, exist_ok=True)

    results: dict[str, Any] = {"timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")}

    # === TEST A: Validator Matrix ===
    print("=== [Test A] Deep Read-Only XISO/XBE Validator Matrix ===")
    val_res: dict[str, Any] = {}

    rc, obj = run_validate_iso(RETAIL_ISO)
    val_res["valid_retail_0xeb"] = {"rc": rc, **obj}
    assert rc == 0 and obj["valid"] is True and obj["code"] == "OK" and obj["title_id"] == "0x4D570015", val_res["valid_retail_0xeb"]
    print(f"  [PASS] valid_retail_0xeb: {obj['revision']} sha1={obj['sha1'][:12]} xdk={obj['xdk_build']}")

    rc, obj = run_validate_iso(TEST_DIR / "does_not_exist.xiso.iso")
    val_res["missing_file"] = {"rc": rc, **obj}
    assert rc == 1 and obj["valid"] is False and obj["code"] == "ERR_NOT_FOUND", val_res["missing_file"]
    print(f"  [PASS] missing_file: {obj['code']}")

    mock_7z = TEST_DIR / "mock_archive.7z"
    mock_7z.write_bytes(b"\x37\x7a\xbc\xaf\x27\x1c" + b"\x00" * 58)
    rc, obj = run_validate_iso(mock_7z)
    val_res["compressed_7z"] = {"rc": rc, **obj}
    assert rc == 1 and obj["valid"] is False and obj["code"] == "ERR_COMPRESSED_ARCHIVE", val_res["compressed_7z"]
    print(f"  [PASS] compressed_7z: {obj['code']}")

    bad_iso = TEST_DIR / "bad_not_xiso.iso"
    bad_iso.write_bytes(b"\x00" * (256 * 1024))
    rc, obj = run_validate_iso(bad_iso)
    val_res["not_xiso"] = {"rc": rc, **obj}
    assert rc == 1 and obj["valid"] is False and obj["code"] == "ERR_NOT_XISO", val_res["not_xiso"]
    print(f"  [PASS] not_xiso: {obj['code']}")

    wrong_iso = TEST_DIR / "mock_wrong_game.xiso.iso"
    build_synthetic_xiso(wrong_iso, build_mock_wrong_game_xbe())
    rc, obj = run_validate_iso(wrong_iso)
    val_res["wrong_game"] = {"rc": rc, **obj}
    assert rc == 1 and obj["valid"] is False and obj["code"] == "ERR_WRONG_GAME" and obj["title_id"] == "0xDEADBEEF", val_res["wrong_game"]
    print(f"  [PASS] wrong_game: {obj['code']} detected='{obj['title_name']}' ({obj['title_id']})")

    redump_iso = TEST_DIR / "mock_redump_0x7d.xiso.iso"
    try:
        xbe_mut = bytearray(PRIVATE_XBE.read_bytes())
        assert xbe_mut[0x1CCFCF] == 0xEB, f"Unexpected byte at 0x1CCFCF: {xbe_mut[0x1CCFCF]:#x}"
        xbe_mut[0x1CCFCF] = 0x7D
        build_synthetic_xiso(redump_iso, bytes(xbe_mut))
        rc, obj = run_validate_iso(redump_iso)
        val_res["redump_0x7d"] = {"rc": rc, **obj}
        assert rc == 0 and obj["valid"] is True and obj["sha1"] == "55c902f622047484bc6c7ea4b1b1d714da2096fc", val_res["redump_0x7d"]
        print(f"  [PASS] redump_0x7d: {obj['revision']} sha1={obj['sha1'][:12]}")
    finally:
        if redump_iso.exists():
            redump_iso.unlink()

    results["validator_matrix"] = val_res

    # === TEST B: Windows Integration (PE Resources + Shortcut) ===
    print("=== [Test B] Windows Integration (PE Resources & .lnk Shortcut) ===")
    pe_info = check_pe_resources(RELEASE_EXE)
    assert pe_info["has_group_icon_1"] and pe_info["has_version_info_1"], pe_info
    assert pe_info["version_strings"].get("ProductName") == "The Suffering", pe_info
    assert pe_info["version_strings"].get("OriginalFilename") == "suffering.exe", pe_info
    print(f"  [PASS] PE resources: icon=#1 version={pe_info['version_strings']['ProductName']}")

    sc_dir = TEST_DIR / "shortcut"
    if sc_dir.exists():
        shutil.rmtree(sc_dir)
    proc = subprocess.run(
        [str(RELEASE_EXE), "--create-shortcut", str(sc_dir)],
        cwd=str(RELEASE_EXE.parent),
        capture_output=True,
        text=True,
        timeout=30,
    )
    sc_obj = json.loads(proc.stdout.strip().splitlines()[-1])
    lnk_path = Path(sc_obj["path"])
    assert proc.returncode == 0 and sc_obj["ok"] is True and lnk_path.is_file(), sc_obj
    lnk_bytes = lnk_path.read_bytes()
    assert lnk_bytes[:8] == b"\x4c\x00\x00\x00\x01\x14\x02\x00", "Invalid ShellLink header"
    assert b"suffering.exe" in lnk_bytes.lower() or "suffering.exe".encode("utf-16-le") in lnk_bytes.lower()
    results["windows_integration"] = {
        "pe_resources": pe_info,
        "shortcut": {"ok": True, "path": str(lnk_path), "bytes": len(lnk_bytes)},
    }
    print(f"  [PASS] Shortcut created: {lnk_path} ({len(lnk_bytes)} B)")

    # === TEST C: Full Unpacked release.bat End-User Simulation ===
    print("=== [Test C] Full Unpacked release.bat End-User Simulation ===")
    rel_proc = subprocess.run(["cmd", "/c", str(REPO / "release.bat")], cwd=str(REPO), capture_output=True, text=True, timeout=300)
    assert rel_proc.returncode == 0, f"release.bat failed:\n{rel_proc.stdout}\n{rel_proc.stderr}"
    zip_path = REPO / "suffering.zip"
    assert zip_path.is_file(), "suffering.zip not created by release.bat"

    zip_entries: list[str] = []
    with zipfile.ZipFile(zip_path, "r") as zf:
        zip_entries = zf.namelist()
        lower_entries = [e.replace("\\", "/").lower() for e in zip_entries]
        for req in (
            "suffering.exe",
            "compiler/vcruntime140_1.dll",
            "compiler/vcruntime140.dll",
            "compiler/msvcp140.dll",
            "compiler/clang.exe",
            "compiler/lld-link.exe",
            "compiler/xbe_recomp.exe",
            "compiler/suffering.lib",
            "compiler/config/game.toml",
            "compiler/xdk/5788/layout.toml",
            "plugins/readme.txt",
            "plugins/nvngx_dlss.dll",
            "plugins/amd_fidelityfx_loader_dx12.dll",
            "plugins/amd_fidelityfx_upscaler_dx12.dll",
            "plugins/libxess.dll",
            "plugins/nvngx-dlss-license.txt",
            "plugins/amd-fidelityfx-license.md",
            "plugins/intel-xess-license.txt",
            "plugins/intel-xess-third-party.txt",
            "readme.md",
            "license",
            "sdk-license.txt",
            "third_party.txt",
        ):
            assert req in lower_entries, f"Missing required release.zip entry: {req}"
        for bad_ext in (".xbe", ".iso", ".7z"):
            bad_hits = [e for e in lower_entries if e.endswith(bad_ext)]
            assert not bad_hits, f"Forbidden copyrighted file in release.zip: {bad_hits}"
        assert "suffering.dll" not in lower_entries, "suffering.dll must NOT be shipped in release.zip"
        assert not any(e.startswith("compiler/work/") for e in lower_entries), "compiler/work must NOT be shipped"

        if RELEASE_TEST_DIR.exists():
            shutil.rmtree(RELEASE_TEST_DIR)
        RELEASE_TEST_DIR.mkdir(parents=True, exist_ok=True)
        zf.extractall(RELEASE_TEST_DIR)

    zip_size = zip_path.stat().st_size
    zip_path.unlink()  # keep repo root clean
    print(f"  [PASS] release.bat zip verified ({zip_size / (1024*1024):.1f} MiB, {len(zip_entries)} entries) & unpacked to .local/m5_release_test/")

    unpacked_exe = RELEASE_TEST_DIR / "suffering.exe"
    unpacked_dll = RELEASE_TEST_DIR / "suffering.dll"
    unpacked_settings = RELEASE_TEST_DIR / "settings.toml"
    assert not unpacked_dll.exists() and not unpacked_settings.exists()

    # Launch #1: Cold end-user first run -> PrepareGameCode compiles 64 partitions + links suffering.dll + boots to Title Screen
    check_no_suffering_running()
    l1_state = TEST_DIR / "release_launch1_state.jsonl"
    if l1_state.exists():
        l1_state.unlink()
    t0 = time.monotonic()
    p1 = subprocess.run(
        [
            str(unpacked_exe),
            "--iso", str(RETAIL_ISO),
            "--auto-start",
            "--state-log", str(l1_state),
            "--replay-input", str(ROUNDTRIP_REC),
            "--replay-exit",
        ],
        cwd=str(RELEASE_TEST_DIR),
        timeout=300,
    )
    l1_wall = round(time.monotonic() - t0, 3)
    assert p1.returncode == 0, f"Release Launch #1 failed rc={p1.returncode}"
    assert unpacked_dll.is_file(), "suffering.dll was not compiled on Launch #1!"
    built_txt = RELEASE_TEST_DIR / "compiler" / "work" / "suffering" / "built.txt"
    assert built_txt.is_file(), "built.txt stamp missing after Launch #1!"
    dll_mtime_1 = unpacked_dll.stat().st_mtime_ns
    dll_size_1 = unpacked_dll.stat().st_size
    l1_dumps = list((RELEASE_TEST_DIR / "CrashDumps").glob("*.dmp")) if (RELEASE_TEST_DIR / "CrashDumps").exists() else []
    assert not l1_dumps, f"Crash dumps on Launch #1: {l1_dumps}"
    l1_st = parse_state_jsonl(l1_state)
    assert l1_st.get("exists") and l1_st.get("max_gframe", 0) > 0, l1_st
    print(f"  [PASS] Release Launch #1 (cold local compile -> Title Screen): wall={l1_wall}s dll={dll_size_1}B gframe={l1_st['max_gframe']}")

    # Launch #2: Persist iso_path in settings.toml, run WITHOUT --iso -> instant suffering.dll reuse!
    check_no_suffering_running()
    iso_toml = str(RETAIL_ISO).replace("\\", "\\\\")
    unpacked_settings.write_text(
        f'display_mode = "windowed"\n'
        f'width = 1280\n'
        f'height = 720\n'
        f'render_percent = 125\n'
        f'fps_cap = 60\n'
        f'vsync = true\n'
        f'master_volume = 75\n'
        f'audio_mute = false\n'
        f'mute_unfocused = true\n'
        f'rumble_enabled = true\n'
        f'rumble_strength = 80\n'
        f'touchpad_as_back = true\n'
        f'show_fps_hud = true\n'
        f'iso_path = "{iso_toml}"\n',
        encoding="utf-8",
    )
    l2_state = TEST_DIR / "release_launch2_state.jsonl"
    if l2_state.exists():
        l2_state.unlink()
    t0 = time.monotonic()
    p2 = subprocess.run(
        [
            str(unpacked_exe),
            "--state-log", str(l2_state),
            "--replay-input", str(ROUNDTRIP_REC),
            "--replay-exit",
        ],
        cwd=str(RELEASE_TEST_DIR),
        timeout=120,
    )
    l2_wall = round(time.monotonic() - t0, 3)
    assert p2.returncode == 0, f"Release Launch #2 failed rc={p2.returncode}"
    dll_mtime_2 = unpacked_dll.stat().st_mtime_ns
    assert dll_mtime_2 == dll_mtime_1, f"suffering.dll was rebuilt on Launch #2! ({dll_mtime_1} -> {dll_mtime_2})"
    l2_dumps = list((RELEASE_TEST_DIR / "CrashDumps").glob("*.dmp")) if (RELEASE_TEST_DIR / "CrashDumps").exists() else []
    assert not l2_dumps, f"Crash dumps on Launch #2: {l2_dumps}"
    l2_st = parse_state_jsonl(l2_state)
    assert l2_st.get("exists") and l2_st.get("max_gframe", 0) > 0, l2_st
    print(f"  [PASS] Release Launch #2 (settings.toml iso_path + instant DLL reuse): wall={l2_wall}s gframe={l2_st['max_gframe']}")

    results["end_user_release_simulation"] = {
        "zip_bytes": zip_size,
        "zip_entry_count": len(zip_entries),
        "launch1_cold_compile": {
            "rc": p1.returncode,
            "wall_s": l1_wall,
            "dll_bytes": dll_size_1,
            "built_txt_exists": True,
            "crash_dumps": 0,
            "state": l1_st,
        },
        "launch2_instant_reuse": {
            "rc": p2.returncode,
            "wall_s": l2_wall,
            "dll_mtime_unchanged": True,
            "crash_dumps": 0,
            "state": l2_st,
        },
    }

    # === TEST D: ImGui Settings Overlay Visual Capture & Settings Persistence ===
    print("=== [Test D] Dear ImGui Settings Overlay Visual Capture & Settings Persistence ===")
    check_no_suffering_running()
    ui_bmp = CAP_DIR / "settings_overlay.bmp"
    ui_png = CAP_DIR / "settings_overlay.png"
    for p in (ui_bmp, ui_png):
        if p.exists():
            p.unlink()

    t0 = time.monotonic()
    p3 = subprocess.run(
        [
            str(unpacked_exe),
            "--open-settings",
            "--show-fps-hud",
            "--fps-cap", "30",  # verify temporary CLI override does NOT pollute settings.toml
            "--capture-ui-bmp", str(ui_bmp),
            "--capture-ui-exit",
        ],
        cwd=str(RELEASE_TEST_DIR),
        timeout=90,
    )
    ui_wall = round(time.monotonic() - t0, 3)
    assert p3.returncode == 0, f"ImGui capture run failed rc={p3.returncode}"
    assert ui_bmp.is_file(), f"Composited UI capture missing: {ui_bmp}"
    img_stats = bmp32_to_png(ui_bmp, ui_png)
    assert img_stats["width"] == 1280 and img_stats["height"] == 720, img_stats
    assert img_stats["non_black_pct"] > 5.0, f"Captured overlay looks blank: {img_stats}"

    # Verify settings.toml still has fps_cap = 60 and persisted fields intact
    st_text = unpacked_settings.read_text(encoding="utf-8")
    assert "fps_cap = 60" in st_text, f"Temporary --fps-cap 30 polluted settings.toml!\n{st_text}"
    assert "master_volume = 75" in st_text and "mute_unfocused = true" in st_text and "rumble_strength = 80" in st_text
    # Check latest log in .local/m5_release_test/logs/ confirmed Settings load
    logs = sorted((RELEASE_TEST_DIR / "logs").glob("suffering*.log"), key=lambda p: p.stat().st_mtime_ns)
    assert logs, "No log file found in .local/m5_release_test/logs/"
    last_log = logs[-1].read_text(encoding="utf-8", errors="replace")
    assert "master_volume = 75" in last_log and "render_percent = 125" in last_log and "mute_unfocused = true" in last_log

    results["imgui_overlay_and_persistence"] = {
        "rc": p3.returncode,
        "wall_s": ui_wall,
        "bmp_path": str(ui_bmp.relative_to(REPO)),
        "png_path": str(ui_png.relative_to(REPO)),
        "image_stats": img_stats,
        "cli_override_non_persistent_ok": True,
        "settings_loaded_in_log_ok": True,
    }
    print(f"  [PASS] ImGui overlay captured: {ui_png} ({img_stats['width']}x{img_stats['height']}, non_black={img_stats['non_black_pct']}%)")

    verify_protected_media(media_before)
    results["protected_media_ok"] = True
    SUMMARY_JSON.write_text(json.dumps(results, indent=2), encoding="utf-8")
    print(f"\n=== ALL MILESTONE 5 VERIFICATION TESTS PASSED -> {SUMMARY_JSON.relative_to(REPO)} ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
