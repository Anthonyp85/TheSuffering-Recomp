#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate clean, copyright-free icons (.ico and 256x256 .png) for The Suffering Recomp.

Outputs:
  - src/res/suffering.ico containing 16x16, 32x32, 48x48, and 256x256 32-bit RGBA PNG-encoded ICO frames
  - src/res/suffering.png containing the 256x256 32-bit RGBA PNG icon for Linux / Steam Deck / SDL3
(pure Python, zero external dependencies).
"""

from __future__ import annotations

import math
import struct
import zlib
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
OUT_ICO = REPO / "src" / "res" / "suffering.ico"
OUT_PNG = REPO / "src" / "res" / "suffering.png"
SIZES = (16, 32, 48, 256)


def _clamp_u8(v: float) -> int:
    return 0 if v <= 0.0 else (255 if v >= 255.0 else int(v + 0.5))


def _render_rgba(size: int) -> bytes:
    """Procedural dark industrial iron-bar & crimson geometric emblem."""
    out = bytearray(size * size * 4)
    inv = 1.0 / float(size)
    # Rounded square mask radius & border thickness in normalized [0, 1] coords
    corner = 0.17
    border = max(0.055, 1.2 * inv)
    bar_half_w = 0.045
    bar_xs = (0.31, 0.50, 0.69)

    for y in range(size):
        ny = (y + 0.5) * inv
        for x in range(size):
            nx = (x + 0.5) * inv

            # Signed distance to rounded square [0.04, 0.96]
            dx = abs(nx - 0.5) - (0.46 - corner)
            dy = abs(ny - 0.5) - (0.46 - corner)
            sd = math.hypot(max(dx, 0.0), max(dy, 0.0)) + min(max(dx, dy), 0.0) - corner

            # Anti-aliased alpha edge
            aa = max(0.75 * inv, 0.004)
            alpha = 1.0 - min(max((sd + aa) / (2.0 * aa), 0.0), 1.0)
            if alpha <= 0.0:
                continue

            # Deep charcoal radial backdrop with subtle crimson glow at lower center
            gx = (nx - 0.50) * 1.45
            gy = (ny - 0.58) * 1.35
            glow = math.exp(-(gx * gx + gy * gy) * 3.6)
            r = 16.0 + 135.0 * glow
            g = 15.0 + 22.0 * glow
            b = 18.0 + 20.0 * glow

            # Crimson geometric diamond crest behind bars
            diam = abs(nx - 0.50) * 1.15 + abs(ny - 0.50) * 0.92
            if diam < 0.34:
                edge = min(max((0.34 - diam) / (2.0 * aa), 0.0), 1.0)
                inner_band = 1.0 if diam > 0.25 else 0.55
                cr = 185.0 * inner_band + 28.0 * (1.0 - ny)
                cg = 26.0 * inner_band
                cb = 26.0 * inner_band
                r = r * (1.0 - edge) + cr * edge
                g = g * (1.0 - edge) + cg * edge
                b = b * (1.0 - edge) + cb * edge

            # Three vertical brushed-steel bars + horizontal cross-rail
            in_bar = 0.0
            bar_shade = 0.0
            if 0.14 <= ny <= 0.86:
                for bx in bar_xs:
                    bd = abs(nx - bx)
                    if bd < bar_half_w + aa:
                        cov = 1.0 - min(max((bd - bar_half_w + aa) / (2.0 * aa), 0.0), 1.0)
                        if cov > in_bar:
                            in_bar = cov
                            # Specular highlight offset slightly left of bar center
                            bar_shade = max(0.0, 1.0 - abs((nx - (bx - 0.014)) / bar_half_w))
            # Cross-rail at upper-middle and lower-middle
            for ry in (0.27, 0.73):
                rd = abs(ny - ry)
                if 0.14 <= nx <= 0.86 and rd < 0.032 + aa:
                    cov = 1.0 - min(max((rd - 0.032 + aa) / (2.0 * aa), 0.0), 1.0)
                    if cov > in_bar:
                        in_bar = cov
                        bar_shade = 0.55

            if in_bar > 0.0:
                sr = 78.0 + 92.0 * bar_shade
                sg = 82.0 + 94.0 * bar_shade
                sb = 88.0 + 98.0 * bar_shade
                r = r * (1.0 - in_bar) + sr * in_bar
                g = g * (1.0 - in_bar) + sg * in_bar
                b = b * (1.0 - in_bar) + sb * in_bar

            # Outer beveled iron frame
            if sd > -border:
                f_t = min(max((sd + border) / border, 0.0), 1.0)
                bevel = 0.85 if (nx + ny) < 1.0 else 0.45
                fr = 95.0 * bevel + 30.0 * (1.0 - f_t)
                fg = 96.0 * bevel + 24.0 * (1.0 - f_t)
                fb = 104.0 * bevel + 26.0 * (1.0 - f_t)
                r, g, b = fr, fg, fb

            idx = (y * size + x) * 4
            out[idx + 0] = _clamp_u8(r)
            out[idx + 1] = _clamp_u8(g)
            out[idx + 2] = _clamp_u8(b)
            out[idx + 3] = _clamp_u8(alpha * 255.0)
    return bytes(out)


def _png_chunk(tag: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(tag + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", crc)


def _encode_png(size: int, rgba: bytes) -> bytes:
    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    stride = size * 4
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type None
        raw.extend(rgba[y * stride : (y + 1) * stride])
    idat = zlib.compress(bytes(raw), level=9)
    return (
        b"\x89PNG\r\n\x1a\n"
        + _png_chunk(b"IHDR", ihdr)
        + _png_chunk(b"IDAT", idat)
        + _png_chunk(b"IEND", b"")
    )


def build_icons(out_ico: Path, out_png: Path) -> None:
    pngs = [(sz, _encode_png(sz, _render_rgba(sz))) for sz in SIZES]
    header = struct.pack("<HHH", 0, 1, len(pngs))
    dir_size = 6 + 16 * len(pngs)
    entries = bytearray()
    payload = bytearray()
    offset = dir_size
    png_256 = b""
    for sz, blob in pngs:
        if sz == 256:
            png_256 = blob
        w = 0 if sz >= 256 else sz
        h = 0 if sz >= 256 else sz
        entries.extend(struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(blob), offset))
        payload.extend(blob)
        offset += len(blob)
    out_ico.parent.mkdir(parents=True, exist_ok=True)
    out_ico.write_bytes(header + bytes(entries) + bytes(payload))
    if png_256:
        out_png.parent.mkdir(parents=True, exist_ok=True)
        out_png.write_bytes(png_256)


if __name__ == "__main__":
    build_icons(OUT_ICO, OUT_PNG)
    print(f"Generated {OUT_ICO} ({OUT_ICO.stat().st_size} bytes, sizes={SIZES})")
    print(f"Generated {OUT_PNG} ({OUT_PNG.stat().st_size} bytes, 256x256 RGBA PNG)")
