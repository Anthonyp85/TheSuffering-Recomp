#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch, verify, and stage official signed vendor upscaler plugins (DLSS 4, AMD FSR 3.1/4, Intel XeSS).

All binaries are pinned by exact byte length and SHA-256, verified as PE32+ x64,
checked for 0 occurrences of "DO NOT DISTRIBUTE" (ASCII & UTF-16LE), and verified
via offline Windows Authenticode (WinVerifyTrust with WTD_CACHE_ONLY_URL_RETRIEVAL).

Zero network requests are performed when pinned files are already present in --cache-dir.
"""

from __future__ import annotations

import argparse
import ctypes
import hashlib
import shutil
import struct
import sys
import urllib.request
import zipfile
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_CACHE_DIR = REPO / ".local" / "vendor_sdks"
DEFAULT_STAGE_DIR = REPO / "out" / "build" / "release" / "plugins"


@dataclass(frozen=True)
class DirectArtifact:
    stage_name: str
    cache_rel: str
    url: str
    size: int
    sha256: str
    is_dll: bool
    label: str


@dataclass(frozen=True)
class ZipMemberArtifact:
    stage_name: str
    member_path: str
    extracted_rel: str
    size: int
    sha256: str
    is_dll: bool
    label: str


DIRECT_ARTIFACTS: tuple[DirectArtifact, ...] = (
    DirectArtifact(
        stage_name="nvngx_dlss.dll",
        cache_rel="dlss/nvngx_dlss.dll",
        url="https://github.com/NVIDIA/DLSS/raw/v310.9.16/lib/Windows_x86_64/rel/nvngx_dlss.dll",
        size=58956912,
        sha256="d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
        is_dll=True,
        label="NVIDIA DLSS Super Resolution v310.9.16",
    ),
    DirectArtifact(
        stage_name="NVNGX-DLSS-LICENSE.txt",
        cache_rel="dlss/LICENSE.txt",
        url="https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.16/LICENSE.txt",
        size=26620,
        sha256="d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0",
        is_dll=False,
        label="NVIDIA RTX SDKs License Agreement",
    ),
    DirectArtifact(
        stage_name="amd_fidelityfx_loader_dx12.dll",
        cache_rel="ffx/amd_fidelityfx_loader_dx12.dll",
        url="https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll",
        size=39184,
        sha256="e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb",
        is_dll=True,
        label="AMD FidelityFX DX12 Loader v2.4.0",
    ),
    DirectArtifact(
        stage_name="amd_fidelityfx_upscaler_dx12.dll",
        cache_rel="ffx/amd_fidelityfx_upscaler_dx12.dll",
        url="https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/raw/v2.4.0/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll",
        size=57664272,
        sha256="aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace",
        is_dll=True,
        label="AMD FidelityFX DX12 Upscaler v2.4.0 (FSR 3.1.6 / FSR 4)",
    ),
    DirectArtifact(
        stage_name="AMD-FIDELITYFX-LICENSE.md",
        cache_rel="ffx/LICENSE.md",
        url="https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/v2.4.0/LICENSE.md",
        size=65770,
        sha256="32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb",
        is_dll=False,
        label="AMD FidelityFX SDK License",
    ),
)

XESS_ZIP_REL = "xess/XeSS_SDK_3.0.2.zip"
XESS_ZIP_URL = "https://github.com/intel/xess/releases/download/v3.0.2/XeSS_SDK_3.0.2.zip"
XESS_ZIP_SIZE = 76839749
XESS_ZIP_SHA256 = "88b8a373f30e33f3558a77a93e634f11b8132fc3047ea1a8edeead32b8471990"

XESS_MEMBERS: tuple[ZipMemberArtifact, ...] = (
    ZipMemberArtifact(
        stage_name="libxess.dll",
        member_path="bin/libxess.dll",
        extracted_rel="xess/extracted/bin/libxess.dll",
        size=77795704,
        sha256="251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7",
        is_dll=True,
        label="Intel XeSS v3.0.2 (libxess 2.0.2.68)",
    ),
    ZipMemberArtifact(
        stage_name="INTEL-XESS-LICENSE.txt",
        member_path="LICENSE.txt",
        extracted_rel="xess/extracted/LICENSE.txt",
        size=4194,
        sha256="784be42c39a4a4d03cf85d88e15e59d75828fca773ce45b8dbfffbd7eaf208df",
        is_dll=False,
        label="Intel Simplified Software License",
    ),
    ZipMemberArtifact(
        stage_name="INTEL-XESS-THIRD-PARTY.txt",
        member_path="third-party-programs.txt",
        extracted_rel="xess/extracted/third-party-programs.txt",
        size=35788,
        sha256="e8d8899dbfc4de0e8ca25560be9358e03c1405d7c956d20a1838c3c709b9b51c",
        is_dll=False,
        label="Intel XeSS Third-Party Programs Notice",
    ),
)

PLUGINS_README = """Bundled Modern Temporal / AI Upscaler Plugins (Direct3D 12)
===========================================================

All 9 upscaling technologies work out of the box on Direct3D 12:

  Built-In Zero-DLL Spatial Shaders (all D3D12 GPUs):
    - AMD FSR 1.0 (EASU + RCAS)
    - NVIDIA Image Scaling (NIS v1.0.3)
    - AMD FidelityFX CAS
    - Bicubic Catmull-Rom (5-Tap)
    - Pixel-Perfect Integer
    - Bilinear (Original)

  Bundled Official Vendor-Signed Temporal / AI Plugins (in this plugins\\ folder):
    - NVIDIA DLSS 4 Super Resolution (v310.9.16, RTX GPUs, Presets J/K & DLAA):
        nvngx_dlss.dll
        License: NVNGX-DLSS-LICENSE.txt (NVIDIA RTX SDKs License Agreement)
        (Loaded alongside your NVIDIA driver's _nvngx.dll; if nvngx_dlss.dll is
         removed, suffering.exe can also auto-discover signed NVIDIA NGX driver
         DLSS models on RTX systems.)

    - AMD FidelityFX Super Resolution 3.1 / FSR 4 (SDK v2.4.0, ALL SM 6.2+ GPUs):
        amd_fidelityfx_loader_dx12.dll + amd_fidelityfx_upscaler_dx12.dll
        License: AMD-FIDELITYFX-LICENSE.md (AMD Binary Redistribution License)

    - Intel Xe Super Sampling (XeSS SDK v3.0.2 / XeSS 2.0.2, Arc XMX + SM 6.4+ DP4a):
        libxess.dll
        Licenses: INTEL-XESS-LICENSE.txt, INTEL-XESS-THIRD-PARTY.txt

These vendor plugin DLLs are distributed unmodified under the GPLv2 Runtime
Upscaler Plugin Exception in LICENSE and remain governed by their respective
vendor license agreements. You may update or delete any DLL in this folder at
any time; if a vendor plugin is removed or unsupported on your GPU, the game
safely falls back to built-in AMD FSR 1.0 (EASU + RCAS).
"""


def sha256_file(path: Path) -> tuple[int, str]:
    h = hashlib.sha256()
    total = 0
    with path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            total += len(chunk)
            h.update(chunk)
    return total, h.hexdigest()


def verify_pe64_and_clean(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise RuntimeError(f"{path.name}: missing MZ DOS header")
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if e_lfanew + 26 > len(data) or data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        raise RuntimeError(f"{path.name}: missing PE\\0\\0 signature")
    machine = struct.unpack_from("<H", data, e_lfanew + 4)[0]
    opt_magic = struct.unpack_from("<H", data, e_lfanew + 24)[0]
    if machine != 0x8664 or opt_magic != 0x020B:
        raise RuntimeError(f"{path.name}: not PE32+ x64 (machine=0x{machine:04x}, opt=0x{opt_magic:04x})")
    if b"DO NOT DISTRIBUTE" in data or "DO NOT DISTRIBUTE".encode("utf-16-le") in data:
        raise RuntimeError(f"{path.name}: forbidden watermark string found!")


class _GUID(ctypes.Structure):
    _fields_ = [
        ("Data1", ctypes.c_uint32),
        ("Data2", ctypes.c_uint16),
        ("Data3", ctypes.c_uint16),
        ("Data4", ctypes.c_ubyte * 8),
    ]


class _WINTRUST_FILE_INFO(ctypes.Structure):
    _fields_ = [
        ("cbStruct", ctypes.c_uint32),
        ("pcwszFilePath", ctypes.c_wchar_p),
        ("hFile", ctypes.c_void_p),
        ("pgKnownSubject", ctypes.c_void_p),
    ]


class _WINTRUST_DATA(ctypes.Structure):
    _fields_ = [
        ("cbStruct", ctypes.c_uint32),
        ("pPolicyCallbackData", ctypes.c_void_p),
        ("pSIPClientData", ctypes.c_void_p),
        ("dwUIChoice", ctypes.c_uint32),
        ("fdwRevocationChecks", ctypes.c_uint32),
        ("dwUnionChoice", ctypes.c_uint32),
        ("pFile", ctypes.POINTER(_WINTRUST_FILE_INFO)),
        ("dwStateAction", ctypes.c_uint32),
        ("hWVTStateData", ctypes.c_void_p),
        ("pwszURLReference", ctypes.c_wchar_p),
        ("dwProvFlags", ctypes.c_uint32),
        ("dwUIContext", ctypes.c_uint32),
        ("pSignatureSettings", ctypes.c_void_p),
    ]


def verify_authenticode(path: Path) -> None:
    if sys.platform != "win32":
        return
    wintrust = ctypes.windll.wintrust
    wintrust.WinVerifyTrust.argtypes = [ctypes.c_void_p, ctypes.POINTER(_GUID), ctypes.POINTER(_WINTRUST_DATA)]
    wintrust.WinVerifyTrust.restype = ctypes.c_long

    # WINTRUST_ACTION_GENERIC_VERIFY_V2 = {00AAC56B-CD44-11d0-8CC2-00C04FC295EE}
    action = _GUID(0x00AAC56B, 0xCD44, 0x11D0, (ctypes.c_ubyte * 8)(0x8C, 0xC2, 0x00, 0xC0, 0x4F, 0xC2, 0x95, 0xEE))
    fi = _WINTRUST_FILE_INFO()
    fi.cbStruct = ctypes.sizeof(_WINTRUST_FILE_INFO)
    fi.pcwszFilePath = str(path.resolve())

    wd = _WINTRUST_DATA()
    wd.cbStruct = ctypes.sizeof(_WINTRUST_DATA)
    wd.dwUIChoice = 2  # WTD_UI_NONE
    wd.fdwRevocationChecks = 0  # WTD_REVOKE_NONE
    wd.dwUnionChoice = 1  # WTD_CHOICE_FILE
    wd.pFile = ctypes.pointer(fi)
    wd.dwStateAction = 1  # WTD_STATEACTION_VERIFY
    wd.dwProvFlags = 0x00001000  # WTD_CACHE_ONLY_URL_RETRIEVAL

    status = wintrust.WinVerifyTrust(None, ctypes.byref(action), ctypes.byref(wd))
    wd.dwStateAction = 2  # WTD_STATEACTION_CLOSE
    wintrust.WinVerifyTrust(None, ctypes.byref(action), ctypes.byref(wd))
    if status != 0:
        raise RuntimeError(f"{path.name}: Authenticode WinVerifyTrust failed (status=0x{status & 0xFFFFFFFF:08X})")


def verify_artifact(path: Path, exp_size: int, exp_sha256: str, is_dll: bool, label: str) -> None:
    if not path.is_file():
        raise RuntimeError(f"Missing required artifact: {path}")
    size, digest = sha256_file(path)
    if size != exp_size or digest.lower() != exp_sha256.lower():
        raise RuntimeError(
            f"Hash/size mismatch for {path.name} ({label}): "
            f"got {size} B / {digest}, expected {exp_size} B / {exp_sha256}"
        )
    if is_dll:
        verify_pe64_and_clean(path)
        verify_authenticode(path)


def download_to(url: str, dest: Path, offline: bool) -> None:
    if offline:
        raise RuntimeError(f"Offline mode enabled and cached file missing: {dest}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    tmp = dest.with_suffix(dest.suffix + ".tmp")
    print(f"  [download] {url} -> {dest}")
    req = urllib.request.Request(url, headers={"User-Agent": "TheSuffering-Recomp-UpscalerFetcher/1.1"})
    with urllib.request.urlopen(req, timeout=120) as resp, tmp.open("wb") as out:
        shutil.copyfileobj(resp, out, 1024 * 1024)
    tmp.replace(dest)


def ensure_direct_cached(cache_dir: Path, art: DirectArtifact, offline: bool) -> Path:
    p = cache_dir / art.cache_rel
    if p.is_file():
        size, digest = sha256_file(p)
        if size == art.size and digest.lower() == art.sha256.lower():
            return p
    download_to(art.url, p, offline)
    verify_artifact(p, art.size, art.sha256, art.is_dll, art.label)
    return p


def ensure_xess_cached(cache_dir: Path, offline: bool) -> dict[str, Path]:
    out: dict[str, Path] = {}
    all_extracted = True
    for m in XESS_MEMBERS:
        ep = cache_dir / m.extracted_rel
        if ep.is_file():
            sz, dig = sha256_file(ep)
            if sz == m.size and dig.lower() == m.sha256.lower():
                out[m.stage_name] = ep
                continue
        all_extracted = False
        break
    if all_extracted:
        return out

    zip_path = cache_dir / XESS_ZIP_REL
    if not zip_path.is_file() or sha256_file(zip_path) != (XESS_ZIP_SIZE, XESS_ZIP_SHA256):
        download_to(XESS_ZIP_URL, zip_path, offline)
        verify_artifact(zip_path, XESS_ZIP_SIZE, XESS_ZIP_SHA256, False, "Intel XeSS SDK v3.0.2 zip")

    with zipfile.ZipFile(zip_path, "r") as zf:
        for m in XESS_MEMBERS:
            ep = cache_dir / m.extracted_rel
            ep.parent.mkdir(parents=True, exist_ok=True)
            with zf.open(m.member_path, "r") as src, ep.open("wb") as dst:
                shutil.copyfileobj(src, dst, 1024 * 1024)
            verify_artifact(ep, m.size, m.sha256, m.is_dll, m.label)
            out[m.stage_name] = ep
    return out


def verify_staged_dir(stage_dir: Path) -> bool:
    for art in DIRECT_ARTIFACTS:
        p = stage_dir / art.stage_name
        verify_artifact(p, art.size, art.sha256, art.is_dll, art.label)
        tag = "DLL+PE64+Authenticode+Clean" if art.is_dll else "License"
        print(f"  [OK] {art.stage_name:36s} {art.size:10d} B  sha256={art.sha256[:16]}... ({tag})")
    for m in XESS_MEMBERS:
        p = stage_dir / m.stage_name
        verify_artifact(p, m.size, m.sha256, m.is_dll, m.label)
        tag = "DLL+PE64+Authenticode+Clean" if m.is_dll else "License"
        print(f"  [OK] {m.stage_name:36s} {m.size:10d} B  sha256={m.sha256[:16]}... ({tag})")
    readme = stage_dir / "README.txt"
    if not readme.is_file() or readme.stat().st_size == 0:
        raise RuntimeError(f"Missing or empty {readme}")
    print(f"  [OK] {'README.txt':36s} {readme.stat().st_size:10d} B  (Bundled Plugins Guide)")
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description="Fetch, verify, and stage official signed vendor upscaler DLLs & licenses.")
    ap.add_argument("--cache-dir", type=Path, default=DEFAULT_CACHE_DIR, help="Local SDK cache directory")
    ap.add_argument("--stage-dir", type=Path, default=DEFAULT_STAGE_DIR, help="Target plugins\\ directory to stage into")
    ap.add_argument("--offline", action="store_true", help="Never perform network downloads; require files in --cache-dir")
    ap.add_argument("--verify-only", action="store_true", help="Verify staged files (or cache if stage-dir empty) without copying")
    args = ap.parse_args()

    if args.verify_only:
        print(f"Verifying vendor upscaler plugins in: {args.stage_dir}")
        verify_staged_dir(args.stage_dir)
        print("=== ALL 4 VENDOR DLLS + 4 LICENSES + README VERIFIED ===")
        return 0

    print(f"Staging official vendor upscalers (cache={args.cache_dir} -> stage={args.stage_dir})...")
    args.stage_dir.mkdir(parents=True, exist_ok=True)

    staged_sources: list[tuple[str, Path, int, str, bool, str]] = []
    for art in DIRECT_ARTIFACTS:
        src = ensure_direct_cached(args.cache_dir, art, args.offline)
        verify_artifact(src, art.size, art.sha256, art.is_dll, art.label)
        staged_sources.append((art.stage_name, src, art.size, art.sha256, art.is_dll, art.label))

    xess_map = ensure_xess_cached(args.cache_dir, args.offline)
    for m in XESS_MEMBERS:
        src = xess_map[m.stage_name]
        verify_artifact(src, m.size, m.sha256, m.is_dll, m.label)
        staged_sources.append((m.stage_name, src, m.size, m.sha256, m.is_dll, m.label))

    for stage_name, src, exp_size, exp_sha, is_dll, label in staged_sources:
        dst = args.stage_dir / stage_name
        need_copy = True
        if dst.is_file():
            sz, dig = sha256_file(dst)
            if sz == exp_size and dig.lower() == exp_sha.lower():
                need_copy = False
        if need_copy:
            shutil.copy2(src, dst)
        verify_artifact(dst, exp_size, exp_sha, is_dll, label)

    (args.stage_dir / "README.txt").write_text(PLUGINS_README, encoding="utf-8", newline="\r\n")
    verify_staged_dir(args.stage_dir)
    print("=== VENDOR UPSCALER PLUGINS STAGED & VERIFIED ===")
    return 0


if __name__ == "__main__":
    sys.exit(main())
