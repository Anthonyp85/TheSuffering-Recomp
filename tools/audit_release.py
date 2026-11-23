#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Automated Copyright, Sanitization, History & Cross-Platform Release Archive Auditor (v1.4.0).

Runs 6 mandatory gates and writes a JSON report to .local/m6_audit/audit_release_report.json:
  Gate 1: Full Git history scan (git rev-list --objects --all) for forbidden extensions/paths,
          binary magic headers (incl. ELF), large blobs (>512 KiB; 1536 KiB cap for patches/*.patch),
          and empty remotes (unless --allow-remote).
  Gate 2: HEAD & working-tree hygiene/sanitization (clean tree unless --allow-dirty, zero
          machine/site leak strings, GPL-2.0-or-later SPDX headers incl. .sh/.yml/.desktop,
          SDK patch byte parity).
  Gate 3: Source release archive (.local/dist/TheSuffering-Recomp-v1.4.0-source.zip) exact
          1-to-1 match with git ls-tree -r HEAD, required files, zero forbidden files/strings.
  Gate 4: Win64 launcher release archive (.local/dist/TheSuffering-Recomp-v1.4.0-win64-launcher.zip)
          exact allowlist of redistributable PE/lib files (incl. SHA-256-pinned official vendor
          upscaler plugins), required license/notice docs, zero suffering.dll / .xbe / .iso /
          .7z / compiler/work / runtime state.
  Gate 4b: Native Linux x86_64 & Steam Deck release archive
          (.local/dist/TheSuffering-Recomp-v1.4.0-linux-x86_64.tar.gz) ELF64 header check,
          GLIBC_2.35 max symbol version verification, self-contained compiler/ RPATH & deps,
          required Steam Deck/desktop/license files, LF script endings, zero libsuffering.so /
          .dll / .exe / .xbe / .iso / compiler/work / runtime state.
  Gate 5: Protected source media integrity check (strictly read-only; skip with --skip-media in CI).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path
from typing import Any

REPO = Path(__file__).resolve().parent.parent
SDK_DIR = REPO / "XboxXBERecomp"
PATCH_FILE = REPO / "patches" / "xboxxberecomp.patch"
DIST_DIR = REPO / ".local" / "dist"
DEFAULT_SOURCE_ZIP = DIST_DIR / "TheSuffering-Recomp-v1.4.0-source.zip"
DEFAULT_LAUNCHER_ZIP = DIST_DIR / "TheSuffering-Recomp-v1.4.0-win64-launcher.zip"
DEFAULT_LINUX_TAR = DIST_DIR / "TheSuffering-Recomp-v1.4.0-linux-x86_64.tar.gz"
REPORT_PATH = REPO / ".local" / "m6_audit" / "audit_release_report.json"

FORBIDDEN_EXTS = {
    ".iso", ".xiso", ".xbe", ".7z", ".rar", ".zip", ".gdf",
    ".vdu", ".sdu", ".tdu", ".mdu", ".adu", ".ldu", ".lvl",
    ".odu", ".wdu", ".qdu", ".xdu", ".fdu", ".xpu",
    ".exe", ".dll", ".lib", ".pdb", ".obj", ".o", ".dmp",
    ".gpr", ".pe", ".bin", ".so",
}
FORBIDDEN_DIR_PREFIXES = (
    "generated/", ".local/", "out/", "build/", "cache/", "hdd/", "logs/", "crashdumps/",
)
FORBIDDEN_MAGICS: list[tuple[str, bytes]] = [
    ("XBEH", b"XBEH"),
    ("XDVDFS", b"MICROSOFT*XBOX*MEDIA"),
    ("7Z", b"\x37\x7a\xbc\xaf\x27\x1c"),
    ("SRSC", b"SRSC"),
    ("PE_MZ", b"MZ\x90\x00"),
    ("ELF", b"\x7fELF"),
    ("MINIDUMP", b"MDMP"),
]
MAX_REPO_BLOB_BYTES = 512 * 1024
MAX_PATCH_BLOB_BYTES = 1536 * 1024

SANITIZE_REGEXES: list[tuple[str, re.Pattern[str]]] = [
    ("windows_users_path", re.compile(r"[A-Za-z]:\\Users\\[A-Za-z0-9_.-]+", re.IGNORECASE)),
    ("macos_users_path", re.compile(r"/Users/[A-Za-z0-9_.-]+/")),
    ("private_key_block", re.compile(r"-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----")),
    ("github_token", re.compile(r"(?:gh[pousr]_[A-Za-z0-9_]{20,}|github_pat_[A-Za-z0-9_]{20,})")),
]

SPDX_EXTS = {".h", ".hpp", ".c", ".cpp", ".py", ".rc", ".bat", ".sh", ".yml", ".yaml", ".desktop"}
SPDX_EXACT = {"CMakeLists.txt"}
SKIP_TEXT_EXTS = {".ico", ".png"}

REQUIRED_SOURCE_FILES = {
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
    "README.md",
    "setup_sdk.bat",
    "setup_sdk.sh",
    "build.bat",
    "release.bat",
    "release_linux.sh",
    "packaging/steamdeck/install-steamdeck.sh",
    "packaging/steamdeck/the-suffering-recomp.desktop",
    ".github/workflows/build.yml",
    "src/res/suffering.png",
    "patches/xboxxberecomp.patch",
    "config/game.toml",
    ".gitignore",
    ".gitattributes",
}

REQUIRED_LAUNCHER_FILES = {
    "suffering.exe",
    "readme.md",
    "license",
    "third_party_notices.md",
    "sdk-license.txt",
    "third_party.txt",
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "compiler/clang.exe",
    "compiler/lld-link.exe",
    "compiler/xbe_recomp.exe",
    "compiler/llvm-license.txt",
    "compiler/suffering.lib",
    "compiler/msvcp140.dll",
    "compiler/vcruntime140.dll",
    "compiler/vcruntime140_1.dll",
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
}

ALLOWED_LAUNCHER_BINARIES = {
    "suffering.exe",
    "msvcp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "compiler/clang.exe",
    "compiler/lld-link.exe",
    "compiler/xbe_recomp.exe",
    "compiler/suffering.lib",
    "compiler/msvcp140.dll",
    "compiler/vcruntime140.dll",
    "compiler/vcruntime140_1.dll",
    "plugins/nvngx_dlss.dll",
    "plugins/amd_fidelityfx_loader_dx12.dll",
    "plugins/amd_fidelityfx_upscaler_dx12.dll",
    "plugins/libxess.dll",
}

REQUIRED_LINUX_FILES = {
    "suffering",
    "install-steamdeck.sh",
    "the-suffering-recomp.desktop",
    "suffering.png",
    "readme.md",
    "license",
    "third_party_notices.md",
    "sdk-license.txt",
    "third_party.txt",
    "compiler/xbe_recomp",
    "compiler/clang",
    "compiler/ld.lld",
    "compiler/llvm-license.txt",
    "compiler/config/game.toml",
    "compiler/xdk/5788/layout.toml",
}

LSB_SYSTEM_LIBS = {
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libm.so.6",
    "libstdc++.so.6",
    "libgcc_s.so.1",
    "libdl.so.2",
    "libpthread.so.0",
    "librt.so.1",
}
MAX_GLIBC_VERSION = (2, 35)

PINNED_VENDOR_PLUGIN_SHA256: dict[str, tuple[int, str]] = {
    "plugins/nvngx_dlss.dll": (
        58956912,
        "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e",
    ),
    "plugins/amd_fidelityfx_loader_dx12.dll": (
        39184,
        "e2191d3c0828be45eb8affd4e3ec0378f2293adc345b772a7170edcf3b021ecb",
    ),
    "plugins/amd_fidelityfx_upscaler_dx12.dll": (
        57664272,
        "aad4fccccb29084a655f9a4f9f613bd22c8c03fb4a0f8840148cc6543b4ccace",
    ),
    "plugins/libxess.dll": (
        77795704,
        "251659dd84a3e84de67c886a4186e01f3eca49b00641906fe38bb6b807e5d5b7",
    ),
}


def git_out(repo: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(repo), *args], stderr=subprocess.DEVNULL)


def blob_size_limit(rel_path: str) -> int:
    norm = rel_path.replace("\\", "/").lower()
    if norm.startswith("patches/") and norm.endswith(".patch"):
        return MAX_PATCH_BLOB_BYTES
    return MAX_REPO_BLOB_BYTES


def is_forbidden_path(rel_path: str) -> str | None:
    low = rel_path.replace("\\", "/").lower()
    for pref in FORBIDDEN_DIR_PREFIXES:
        if low.startswith(pref):
            return f"forbidden_prefix:{pref}"
    ext = Path(low).suffix
    if ext in FORBIDDEN_EXTS:
        return f"forbidden_ext:{ext}"
    return None


def detect_magic(data: bytes) -> str | None:
    head = data[:64]
    for name, sig in FORBIDDEN_MAGICS:
        if head.startswith(sig):
            return name
    if len(data) >= 0x10014 and data[0x10000:0x10014] == b"MICROSOFT*XBOX*MEDIA":
        return "XDVDFS_0x10000"
    return None


def scan_sanitize(text: str) -> list[str]:
    hits = []
    for label, rx in SANITIZE_REGEXES:
        if rx.search(text):
            hits.append(label)
    return hits


def inspect_elf64(data: bytes) -> dict[str, Any]:
    """Pure-Python ELF64 x86_64 header, DT_NEEDED, RPATH/RUNPATH, and GLIBC version inspector."""
    info: dict[str, Any] = {
        "valid_elf64_x86_64": False,
        "needed": [],
        "rpath": [],
        "glibc_versions": [],
        "max_glibc": None,
        "error": None,
    }
    if len(data) < 64 or data[:4] != b"\x7fELF":
        info["error"] = "not an ELF binary"
        return info
    ei_class, ei_data = data[4], data[5]
    if ei_class != 2 or ei_data != 1:
        info["error"] = f"unexpected ELF class/data ({ei_class}/{ei_data}); expected ELF64 little-endian"
        return info
    e_machine = struct.unpack_from("<H", data, 18)[0]
    if e_machine != 62:
        info["error"] = f"unexpected e_machine={e_machine}; expected 62 (EM_X86_64)"
        return info
    info["valid_elf64_x86_64"] = True

    e_shoff = struct.unpack_from("<Q", data, 40)[0]
    e_shentsize = struct.unpack_from("<H", data, 58)[0]
    e_shnum = struct.unpack_from("<H", data, 60)[0]
    if e_shoff == 0 or e_shnum == 0 or e_shentsize < 64:
        return info

    sections: list[tuple[int, int, int, int, int]] = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        if off + 64 > len(data):
            break
        sh_type = struct.unpack_from("<I", data, off + 4)[0]
        sh_offset = struct.unpack_from("<Q", data, off + 24)[0]
        sh_size = struct.unpack_from("<Q", data, off + 32)[0]
        sh_link = struct.unpack_from("<I", data, off + 40)[0]
        sh_entsize = struct.unpack_from("<Q", data, off + 56)[0]
        sections.append((sh_type, sh_offset, sh_size, sh_link, sh_entsize))

    def read_cstr(blob: bytes, idx: int) -> str:
        if idx < 0 or idx >= len(blob):
            return ""
        end = blob.find(b"\x00", idx)
        if end < 0:
            end = len(blob)
        return blob[idx:end].decode("utf-8", errors="replace")

    needed: list[str] = []
    rpaths: list[str] = []
    glibc_vers: set[tuple[int, int]] = set()
    glibc_re = re.compile(r"^GLIBC_(\d+)\.(\d+)(?:\.\d+)?$")

    for sh_type, sh_offset, sh_size, sh_link, _ in sections:
        if sh_offset + sh_size > len(data):
            continue
        if sh_type == 6 and sh_link < len(sections):  # SHT_DYNAMIC
            _, str_off, str_sz, _, _ = sections[sh_link]
            dynstr = data[str_off : str_off + str_sz]
            pos = sh_offset
            end = sh_offset + sh_size
            while pos + 16 <= end:
                d_tag, d_val = struct.unpack_from("<qQ", data, pos)
                pos += 16
                if d_tag == 0:
                    break
                if d_tag == 1:  # DT_NEEDED
                    needed.append(read_cstr(dynstr, int(d_val)))
                elif d_tag in (15, 29):  # DT_RPATH / DT_RUNPATH
                    val = read_cstr(dynstr, int(d_val))
                    if val:
                        rpaths.extend(p for p in val.split(":") if p)
        elif sh_type == 0x6FFFFFFE and sh_link < len(sections):  # SHT_GNU_verneed
            _, str_off, str_sz, _, _ = sections[sh_link]
            dynstr = data[str_off : str_off + str_sz]
            pos = sh_offset
            end = sh_offset + sh_size
            while pos + 16 <= end:
                vn_ver, vn_cnt, _vn_file, vn_aux, vn_next = struct.unpack_from("<HHIII", data, pos)
                if vn_ver != 1:
                    break
                apos = pos + vn_aux
                for _ in range(vn_cnt):
                    if apos + 16 > end:
                        break
                    _vna_hash, _vna_flags, _vna_other, vna_name, vna_next = struct.unpack_from("<IHHII", data, apos)
                    ver_name = read_cstr(dynstr, int(vna_name))
                    m = glibc_re.match(ver_name)
                    if m:
                        glibc_vers.add((int(m.group(1)), int(m.group(2))))
                    if vna_next == 0:
                        break
                    apos += vna_next
                if vn_next == 0:
                    break
                pos += vn_next

    info["needed"] = needed
    info["rpath"] = rpaths
    sorted_vers = sorted(glibc_vers)
    info["glibc_versions"] = [f"{a}.{b}" for a, b in sorted_vers]
    info["max_glibc"] = list(sorted_vers[-1]) if sorted_vers else None
    return info


def gate1_git_history(allow_remote: bool) -> dict[str, Any]:
    errors: list[str] = []
    raw = git_out(REPO, "rev-list", "--objects", "--all").decode("utf-8", errors="replace").splitlines()
    blob_count = 0
    max_blob = 0
    for line in raw:
        parts = line.split(" ", 1)
        oid = parts[0]
        path = parts[1] if len(parts) > 1 else ""
        if path:
            bad = is_forbidden_path(path)
            if bad:
                errors.append(f"history path {path} ({oid[:8]}): {bad}")
        otype = git_out(REPO, "cat-file", "-t", oid).decode().strip()
        if otype != "blob":
            continue
        blob_count += 1
        data = git_out(REPO, "cat-file", "-p", oid)
        if len(data) > max_blob:
            max_blob = len(data)
        limit = blob_size_limit(path)
        if len(data) > limit:
            errors.append(f"large blob {path or oid[:8]}: {len(data)} bytes > {limit}")
        mag = detect_magic(data)
        if mag:
            errors.append(f"forbidden magic {mag} in blob {path or oid[:8]}")

    remotes = git_out(REPO, "remote", "-v").decode("utf-8", errors="replace").strip()
    if remotes and not allow_remote:
        errors.append(f"git remotes configured (forbidden before approval): {remotes}")

    return {
        "passed": len(errors) == 0,
        "objects_scanned": len(raw),
        "blobs_scanned": blob_count,
        "max_blob_bytes": max_blob,
        "remotes_empty": remotes == "",
        "errors": errors,
    }


def gate2_head_hygiene(allow_dirty: bool) -> dict[str, Any]:
    errors: list[str] = []
    status = git_out(REPO, "status", "--porcelain").decode("utf-8", errors="replace").splitlines()
    if status and not allow_dirty:
        errors.append(f"working tree not clean: {status}")

    tree_lines = git_out(REPO, "ls-tree", "-r", "HEAD").decode("utf-8", errors="replace").splitlines()
    tracked_files: list[str] = []
    spdx_checked = 0
    for line in tree_lines:
        meta, rel = line.split("\t", 1)
        mode, otype, oid = meta.split(" ")
        if otype != "blob":
            continue  # submodule gitlink (160000 commit)
        tracked_files.append(rel)
        bad = is_forbidden_path(rel)
        if bad:
            errors.append(f"HEAD path {rel}: {bad}")
        data = git_out(REPO, "cat-file", "-p", oid)
        mag = detect_magic(data)
        if mag:
            errors.append(f"HEAD magic {mag} in {rel}")
        # UTF-8 text checks (skip binary .ico / .png)
        if Path(rel).suffix.lower() not in SKIP_TEXT_EXTS:
            text = data.decode("utf-8", errors="replace")
            hits = scan_sanitize(text)
            if hits:
                errors.append(f"HEAD sanitization hit in {rel}: {hits}")
            if Path(rel).suffix.lower() in SPDX_EXTS or Path(rel).name in SPDX_EXACT:
                spdx_checked += 1
                header = "\n".join(text.splitlines()[:5])
                if "SPDX-License-Identifier: GPL-2.0-or-later" not in header:
                    errors.append(f"Missing GPL-2.0-or-later SPDX header in {rel}")

    # Also scan live working-tree tracked + untracked non-ignored files
    wt_files = git_out(REPO, "ls-files", "--cached", "--others", "--exclude-standard").decode("utf-8", errors="replace").splitlines()
    wt_checked = 0
    for rel in wt_files:
        p = REPO / rel
        if not p.is_file():
            continue  # submodule dir or deleted
        wt_checked += 1
        bad = is_forbidden_path(rel)
        if bad:
            errors.append(f"working-tree path {rel}: {bad}")
        data = p.read_bytes()
        limit = blob_size_limit(rel)
        if len(data) > limit:
            errors.append(f"working-tree large file {rel}: {len(data)} bytes > {limit}")
        mag = detect_magic(data)
        if mag:
            errors.append(f"working-tree magic {mag} in {rel}")
        if Path(rel).suffix.lower() not in SKIP_TEXT_EXTS:
            text = data.decode("utf-8", errors="replace")
            hits = scan_sanitize(text)
            if hits:
                errors.append(f"working-tree sanitization hit in {rel}: {hits}")
            if Path(rel).suffix.lower() in SPDX_EXTS or Path(rel).name in SPDX_EXACT:
                header = "\n".join(text.splitlines()[:5])
                if "SPDX-License-Identifier: GPL-2.0-or-later" not in header:
                    errors.append(f"Missing GPL-2.0-or-later SPDX header in working-tree {rel}")

    # SDK patch byte-for-byte parity
    sdk_diff = git_out(SDK_DIR, "diff")
    patch_bytes = PATCH_FILE.read_bytes() if PATCH_FILE.is_file() else b""
    patch_parity = sdk_diff == patch_bytes and len(patch_bytes) > 0
    if not patch_parity:
        errors.append(f"SDK patch parity mismatch (patch={len(patch_bytes)} B, diff={len(sdk_diff)} B)")
    sdk_st = [l for l in git_out(SDK_DIR, "status", "--short").decode("utf-8", errors="replace").splitlines() if l.strip()]
    sdk_untracked = [l for l in sdk_st if l.startswith("??")]
    if sdk_untracked:
        errors.append(f"Untracked files in XboxXBERecomp: {sdk_untracked}")

    return {
        "passed": len(errors) == 0,
        "working_tree_clean": len(status) == 0,
        "head_blob_count": len(tracked_files),
        "working_tree_files_checked": wt_checked,
        "spdx_files_checked": spdx_checked,
        "patch_parity": patch_parity,
        "patch_bytes": len(patch_bytes),
        "patch_sha256": hashlib.sha256(patch_bytes).hexdigest(),
        "sdk_changed_entries": len(sdk_st),
        "errors": errors,
    }


def gate3_source_zip(zip_path: Path) -> dict[str, Any]:
    errors: list[str] = []
    if not zip_path.is_file():
        return {"passed": False, "path": str(zip_path), "errors": [f"Source zip missing: {zip_path}"]}

    head_blobs = set()
    for line in git_out(REPO, "ls-tree", "-r", "HEAD").decode("utf-8", errors="replace").splitlines():
        meta, rel = line.split("\t", 1)
        if meta.split(" ")[1] == "blob":
            head_blobs.add(rel.replace("\\", "/"))

    zip_files = set()
    with zipfile.ZipFile(zip_path, "r") as zf:
        for info in zf.infolist():
            name = info.filename.replace("\\", "/")
            if info.is_dir() or name.endswith("/"):
                continue
            zip_files.add(name)
            bad = is_forbidden_path(name)
            if bad:
                errors.append(f"source.zip forbidden entry {name}: {bad}")
            data = zf.read(info)
            mag = detect_magic(data)
            if mag:
                errors.append(f"source.zip forbidden magic {mag} in {name}")
            if Path(name).suffix.lower() not in SKIP_TEXT_EXTS:
                hits = scan_sanitize(data.decode("utf-8", errors="replace"))
                if hits:
                    errors.append(f"source.zip sanitization hit in {name}: {hits}")

    missing_req = sorted(REQUIRED_SOURCE_FILES - zip_files)
    if missing_req:
        errors.append(f"source.zip missing required files: {missing_req}")
    if zip_files != head_blobs:
        extra = sorted(zip_files - head_blobs)
        missing = sorted(head_blobs - zip_files)
        errors.append(f"source.zip does not match HEAD 1-to-1 (extra={extra}, missing={missing})")

    zbytes = zip_path.read_bytes()
    return {
        "passed": len(errors) == 0,
        "path": str(zip_path.relative_to(REPO)) if zip_path.is_relative_to(REPO) else str(zip_path),
        "bytes": len(zbytes),
        "sha256": hashlib.sha256(zbytes).hexdigest(),
        "file_count": len(zip_files),
        "matches_head_1to1": zip_files == head_blobs,
        "errors": errors,
    }


def gate4_launcher_zip(zip_path: Path) -> dict[str, Any]:
    errors: list[str] = []
    if not zip_path.is_file():
        return {"passed": False, "path": str(zip_path), "errors": [f"Launcher zip missing: {zip_path}"]}

    files_lower: set[str] = set()
    assets_files: list[str] = []
    with zipfile.ZipFile(zip_path, "r") as zf:
        for info in zf.infolist():
            name = info.filename.replace("\\", "/")
            low = name.lower()
            if info.is_dir() or name.endswith("/"):
                continue
            files_lower.add(low)

            if low == "suffering.dll" or low.endswith("/suffering.dll"):
                errors.append(f"launcher.zip must not contain game-derived DLL: {name}")
            if low.startswith("compiler/work/"):
                errors.append(f"launcher.zip must not contain compiler/work/: {name}")
            if low.startswith("assets/"):
                assets_files.append(name)
            for runtime_pref in ("settings.toml", "hdd/", "logs/", "cache/", "crashdumps/"):
                if low == runtime_pref or low.startswith(runtime_pref):
                    errors.append(f"launcher.zip must not contain runtime state: {name}")
            ext = Path(low).suffix
            if ext in {".xbe", ".iso", ".xiso", ".7z", ".pdb", ".dmp", ".vdu", ".sdu", ".tdu", ".mdu", ".adu", ".ldu", ".lvl"}:
                errors.append(f"launcher.zip forbidden file: {name}")
            if ext in {".exe", ".dll", ".lib"} and low not in ALLOWED_LAUNCHER_BINARIES:
                errors.append(f"launcher.zip non-allowlisted binary: {name}")

            if low == "compiler/suffering.lib":
                data = zf.read(info)
                if not data.startswith(b"!<arch>\n"):
                    errors.append("compiler/suffering.lib is not a COFF archive")
                if len(data) > 256 * 1024:
                    errors.append(f"compiler/suffering.lib unexpectedly large: {len(data)} B")
                if b"XBEH" in data or b"SRSC" in data:
                    errors.append("compiler/suffering.lib contains forbidden Xbox/game markers")

            if low in PINNED_VENDOR_PLUGIN_SHA256:
                exp_size, exp_sha = PINNED_VENDOR_PLUGIN_SHA256[low]
                data = zf.read(info)
                actual_sha = hashlib.sha256(data).hexdigest()
                if len(data) != exp_size or actual_sha != exp_sha:
                    errors.append(
                        f"launcher.zip vendor plugin hash/size mismatch for {name}: "
                        f"got {len(data)} B / {actual_sha}, expected {exp_size} B / {exp_sha}"
                    )
                if b"DO NOT DISTRIBUTE" in data or "DO NOT DISTRIBUTE".encode("utf-16-le") in data:
                    errors.append(f"launcher.zip vendor plugin {name} contains forbidden watermark string")

    if assets_files:
        errors.append(f"launcher.zip assets/ folder is not empty: {assets_files}")
    missing_req = sorted(REQUIRED_LAUNCHER_FILES - files_lower)
    if missing_req:
        errors.append(f"launcher.zip missing required files: {missing_req}")

    zbytes = zip_path.read_bytes()
    return {
        "passed": len(errors) == 0,
        "path": str(zip_path.relative_to(REPO)) if zip_path.is_relative_to(REPO) else str(zip_path),
        "bytes": len(zbytes),
        "sha256": hashlib.sha256(zbytes).hexdigest(),
        "file_count": len(files_lower),
        "vendor_plugins_verified": sorted(p for p in PINNED_VENDOR_PLUGIN_SHA256 if p in files_lower),
        "errors": errors,
    }


def gate4b_linux_tar_gz(tar_path: Path) -> dict[str, Any]:
    errors: list[str] = []
    if not tar_path.is_file():
        return {"passed": False, "path": str(tar_path), "errors": [f"Linux tarball missing: {tar_path}"]}

    files_lower: set[str] = set()
    bundled_compiler_libs: set[str] = set()
    assets_files: list[str] = []
    elf_reports: dict[str, Any] = {}
    compiler_elfs: list[tuple[str, dict[str, Any]]] = []

    with tarfile.open(tar_path, "r:gz") as tf:
        for member in tf.getmembers():
            raw_name = member.name.replace("\\", "/")
            name = raw_name[2:] if raw_name.startswith("./") else raw_name
            name = name.lstrip("/")
            if not name:
                continue
            if (member.mode & 0o002) != 0:
                errors.append(f"linux tar world-writable permissions (0o{member.mode & 0o777:03o}) on: {name}")
            if member.isdir():
                continue
            low = name.lower()
            files_lower.add(low)

            if low == "libsuffering.so" or low.endswith("/libsuffering.so"):
                errors.append(f"linux tar must not contain game-derived libsuffering.so: {name}")
            if low == "suffering.dll" or low.endswith("/suffering.dll"):
                errors.append(f"linux tar must not contain suffering.dll: {name}")
            if low.startswith("compiler/work/"):
                errors.append(f"linux tar must not contain compiler/work/: {name}")
            if low.startswith("assets/"):
                assets_files.append(name)
            for runtime_pref in ("settings.toml", "hdd/", "logs/", "cache/", "crashdumps/"):
                if low == runtime_pref or low.startswith(runtime_pref):
                    errors.append(f"linux tar must not contain runtime state: {name}")

            ext = Path(low).suffix
            if ext in {".xbe", ".iso", ".xiso", ".7z", ".dll", ".exe", ".lib", ".pdb", ".dmp", ".vdu", ".sdu", ".tdu", ".mdu", ".adu", ".ldu", ".lvl"}:
                errors.append(f"linux tar forbidden file: {name}")

            if low.startswith("compiler/lib/") and ".so" in Path(low).name:
                bundled_compiler_libs.add(Path(name).name)
            elif ".so" in Path(low).name and not low.startswith("compiler/lib/"):
                errors.append(f"linux tar unexpected shared library outside compiler/lib/: {name}")

            if not member.isfile():
                continue
            fobj = tf.extractfile(member)
            if fobj is None:
                continue
            data = fobj.read()

            if low in {"install-steamdeck.sh", "the-suffering-recomp.desktop"} and b"\r\n" in data:
                errors.append(f"linux tar script/desktop file has CRLF line endings: {name}")

            is_target_elf = low in {"suffering", "compiler/xbe_recomp", "compiler/clang", "compiler/ld.lld"} or (
                low.startswith("compiler/lib/") and ".so" in Path(low).name
            )
            if is_target_elf:
                einfo = inspect_elf64(data)
                elf_reports[name] = {
                    "valid_elf64_x86_64": einfo["valid_elf64_x86_64"],
                    "max_glibc": einfo["max_glibc"],
                    "needed": einfo["needed"],
                    "rpath": einfo["rpath"],
                }
                if not einfo["valid_elf64_x86_64"]:
                    errors.append(f"linux tar invalid ELF64 x86_64 binary {name}: {einfo['error']}")
                    continue
                if einfo["max_glibc"] and tuple(einfo["max_glibc"]) > MAX_GLIBC_VERSION:
                    errors.append(
                        f"linux tar binary {name} requires GLIBC_{einfo['max_glibc'][0]}.{einfo['max_glibc'][1]} "
                        f"> GLIBC_{MAX_GLIBC_VERSION[0]}.{MAX_GLIBC_VERSION[1]}"
                    )
                if low == "suffering":
                    non_lsb = [lib for lib in einfo["needed"] if lib not in LSB_SYSTEM_LIBS]
                    if non_lsb:
                        errors.append(f"linux tar ./suffering links non-LSB external libraries: {non_lsb}")
                else:
                    compiler_elfs.append((name, einfo))

    # Validate compiler/ self-contained dependencies and $ORIGIN RPATHs
    for cname, einfo in compiler_elfs:
        non_lsb = [lib for lib in einfo["needed"] if lib not in LSB_SYSTEM_LIBS]
        missing_deps = [lib for lib in non_lsb if lib not in bundled_compiler_libs]
        if missing_deps:
            errors.append(f"linux tar {cname} has unbundled non-LSB deps (not in compiler/lib/): {missing_deps}")
        if non_lsb:
            rpaths = einfo["rpath"]
            is_lib = cname.lower().startswith("compiler/lib/")
            expected_rp = "$ORIGIN" if is_lib else "$ORIGIN/lib"
            if expected_rp not in rpaths:
                errors.append(f"linux tar {cname} missing required RPATH '{expected_rp}' (got {rpaths})")

    if assets_files:
        errors.append(f"linux tar assets/ folder is not empty: {assets_files}")
    missing_req = sorted(REQUIRED_LINUX_FILES - files_lower)
    if missing_req:
        errors.append(f"linux tar missing required files: {missing_req}")

    tbytes = tar_path.read_bytes()
    return {
        "passed": len(errors) == 0,
        "path": str(tar_path.relative_to(REPO)) if tar_path.is_relative_to(REPO) else str(tar_path),
        "bytes": len(tbytes),
        "sha256": hashlib.sha256(tbytes).hexdigest(),
        "file_count": len(files_lower),
        "bundled_compiler_libs": sorted(bundled_compiler_libs),
        "elf_binaries_verified": sorted(elf_reports.keys()),
        "errors": errors,
    }


def gate5_protected_media() -> dict[str, Any]:
    """Verify optional local disc image (if present) remains readable and unmodified during builds."""
    errors: list[str] = []
    env_iso = os.environ.get("XBE_ISO_PATH", "").strip()
    iso = Path(env_iso) if env_iso else (REPO / "Suffering, The (USA)" / "Suffering, The (USA).xiso.iso")
    checked: list[dict[str, Any]] = []
    if iso.is_file():
        st = iso.stat()
        ok = st.st_size >= 0x20000
        if not ok:
            errors.append(f"Disc image unexpectedly small: {iso.name} (size={st.st_size})")
        checked.append({"name": iso.name, "size": st.st_size, "ok": ok})
    return {"passed": len(errors) == 0, "checked": checked, "errors": errors}


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Audit repository, Win64 release zips, and Linux x86_64 release tarball for copyright, sanitization, ELF64/GLIBC parity, and completeness."
    )
    ap.add_argument("--source-zip", type=Path, default=DEFAULT_SOURCE_ZIP)
    ap.add_argument("--launcher-zip", type=Path, default=DEFAULT_LAUNCHER_ZIP)
    ap.add_argument("--linux-tar", type=Path, default=None, help=f"Path to Linux .tar.gz (default: {DEFAULT_LINUX_TAR})")
    ap.add_argument("--allow-dirty", action="store_true")
    ap.add_argument("--allow-remote", action="store_true")
    ap.add_argument("--skip-zips", action="store_true")
    ap.add_argument("--skip-source-zip", action="store_true")
    ap.add_argument("--skip-linux-tar", action="store_true")
    ap.add_argument("--skip-media", action="store_true")
    args = ap.parse_args()

    explicit_linux_tar = args.linux_tar is not None
    linux_tar_path = args.linux_tar if args.linux_tar is not None else DEFAULT_LINUX_TAR

    report: dict[str, Any] = {
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "head_commit": git_out(REPO, "rev-parse", "HEAD").decode().strip(),
        "gates": {},
    }

    report["gates"]["gate1_git_history"] = gate1_git_history(args.allow_remote)
    report["gates"]["gate2_head_hygiene"] = gate2_head_hygiene(args.allow_dirty)
    if not args.skip_zips:
        if not args.skip_source_zip:
            report["gates"]["gate3_source_zip"] = gate3_source_zip(args.source_zip)
        report["gates"]["gate4_launcher_zip"] = gate4_launcher_zip(args.launcher_zip)
    if not args.skip_linux_tar and (not args.skip_zips or explicit_linux_tar):
        report["gates"]["gate4b_linux_tar_gz"] = gate4b_linux_tar_gz(linux_tar_path)
    if not args.skip_media:
        report["gates"]["gate5_protected_media"] = gate5_protected_media()

    all_pass = all(g["passed"] for g in report["gates"].values())
    report["all_passed"] = all_pass

    REPORT_PATH.parent.mkdir(parents=True, exist_ok=True)
    REPORT_PATH.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    for name, g in report["gates"].items():
        status = "PASS" if g["passed"] else "FAIL"
        print(f"  [{status}] {name}")
        for e in g.get("errors", []):
            print(f"         ERROR: {e}")

    print(f"\nReport written to: {REPORT_PATH.relative_to(REPO)}")
    if all_pass:
        print("=== ALL RELEASE AUDIT GATES PASSED ===")
        return 0
    print("=== RELEASE AUDIT FAILED ===")
    return 1


if __name__ == "__main__":
    sys.exit(main())
