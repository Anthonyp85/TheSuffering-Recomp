#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Read-only XDVDFS (Xbox ISO) extractor for default.xbe.

Extracts ONLY the requested XBE (default: default.xbe, matched case-insensitively
in the root directory) from an Original Xbox XISO or Redump full-disc image into a
private build-time location (e.g. .local/xbe/default.xbe). Never modifies the
source disc image and never extracts game media/assets.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys
from pathlib import Path

SECTOR_SIZE = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
PARTITION_OFFSETS = (0, 0x18300000)


def find_xdvdfs_header(f) -> tuple[int, int, int]:
    """Return (partition_base, root_dir_sector, root_dir_size)."""
    f.seek(0, os.SEEK_END)
    file_size = f.tell()

    for base in PARTITION_OFFSETS:
        vd_off = base + 32 * SECTOR_SIZE
        if vd_off + SECTOR_SIZE > file_size:
            continue
        f.seek(vd_off)
        sector = f.read(SECTOR_SIZE)
        if len(sector) != SECTOR_SIZE:
            continue
        if sector[0:20] == MAGIC and sector[0x7EC:0x800] == MAGIC:
            root_sector, root_size = struct.unpack_from("<II", sector, 20)
            return base, root_sector, root_size

    raise RuntimeError(
        "Valid XDVDFS volume descriptor ('MICROSOFT*XBOX*MEDIA') not found at "
        "standard XISO (0x10000) or Redump (0x18310000) offsets."
    )


def find_root_entry(
    f, partition_base: int, root_sector: int, root_size: int, target_name: str
) -> tuple[str, int, int]:
    """Traverse the XDVDFS root directory B-tree case-insensitively."""
    dir_off = partition_base + root_sector * SECTOR_SIZE
    f.seek(dir_off)
    table = f.read(root_size)
    if len(table) != root_size:
        raise RuntimeError(
            f"Truncated XDVDFS root directory at 0x{dir_off:X} ({len(table)}/{root_size} bytes)"
        )

    target_lower = target_name.lower()
    stack = [0]
    visited: set[int] = set()

    while stack:
        off = stack.pop()
        if off in visited or off + 14 > len(table):
            continue
        visited.add(off)

        left_dw, right_dw, sector, size, attr, name_len = struct.unpack_from(
            "<HHIIBB", table, off
        )
        name_end = off + 14 + name_len
        if name_end > len(table):
            raise RuntimeError(f"Corrupt XDVDFS entry at root offset 0x{off:X}")

        name = table[off + 14 : name_end].decode("ascii", errors="replace")
        is_dir = bool(attr & 0x10)
        if not is_dir and name.lower() == target_lower:
            return name, partition_base + sector * SECTOR_SIZE, size

        if right_dw != 0 and right_dw != 0xFFFF:
            stack.append(right_dw * 4)
        if left_dw != 0 and left_dw != 0xFFFF:
            stack.append(left_dw * 4)

    raise FileNotFoundError(
        f"'{target_name}' not found in XDVDFS root directory (sector {root_sector})."
    )


def sha1_file(path: Path) -> tuple[int, str]:
    h = hashlib.sha1()
    total = 0
    with path.open("rb") as fh:
        while True:
            chunk = fh.read(1 << 20)
            if not chunk:
                break
            total += len(chunk)
            h.update(chunk)
    return total, h.hexdigest()


def extract_xbe(iso_path: Path, out_path: Path, xbe_name: str = "default.xbe") -> None:
    if not iso_path.is_file():
        raise FileNotFoundError(f"Disc image not found: {iso_path}")

    with iso_path.open("rb") as f:
        part_base, root_sector, root_size = find_xdvdfs_header(f)
        matched_name, xbe_off, xbe_size = find_root_entry(
            f, part_base, root_sector, root_size, xbe_name
        )
        f.seek(xbe_off)
        data = f.read(xbe_size)

    if len(data) != xbe_size:
        raise RuntimeError(
            f"Truncated read for /{matched_name}: got {len(data)} of {xbe_size} bytes"
        )
    if data[:4] != b"XBEH":
        raise RuntimeError(
            f"/{matched_name} does not start with 'XBEH' magic (got {data[:4]!r})"
        )

    expected_sha1 = hashlib.sha1(data).hexdigest()

    if out_path.is_file():
        cur_size, cur_sha1 = sha1_file(out_path)
        if cur_size == xbe_size and cur_sha1 == expected_sha1:
            print(
                f"extract_xbe: {out_path} up to date "
                f"(/{matched_name}, {xbe_size} bytes, sha1 {expected_sha1})"
            )
            return

    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp_path = out_path.with_name(out_path.name + ".tmp")
    try:
        with tmp_path.open("wb") as out_f:
            out_f.write(data)
        os.replace(tmp_path, out_path)
    finally:
        if tmp_path.exists():
            try:
                tmp_path.unlink()
            except OSError:
                pass

    print(
        f"extract_xbe: extracted /{matched_name} -> {out_path} "
        f"({xbe_size} bytes, sha1 {expected_sha1})"
    )


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="Read-only extractor for default.xbe from an Original Xbox XISO."
    )
    parser.add_argument("--iso", required=True, type=Path, help="Path to .iso / .xiso.iso image")
    parser.add_argument("--out", required=True, type=Path, help="Destination path for extracted XBE")
    parser.add_argument(
        "--name",
        default="default.xbe",
        help="Root XBE filename to extract (case-insensitive, default: default.xbe)",
    )
    args = parser.parse_args(argv)

    try:
        extract_xbe(args.iso, args.out, args.name)
    except Exception as exc:
        print(f"extract_xbe: error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
