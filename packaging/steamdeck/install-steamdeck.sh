#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Idempotent Steam Deck (SteamOS 3.x) & Linux Desktop installer for The Suffering Native Recomp.
# Installs the 256x256 PNG icon, XDG .desktop entry, and native Linux Non-Steam shortcut in Steam's
# binary shortcuts.vdf (zero Proton override needed). Pure bash + stdlib python3.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_DIR="$SCRIPT_DIR"
STEAM_DIR=""
REGISTER_STEAM=1
UNINSTALL=0
APP_NAME="The Suffering (Native Recomp)"
DESKTOP_ID="the-suffering-recomp"

usage() {
    cat <<'EOF'
Usage: ./install-steamdeck.sh [options]

Options:
  --install-dir <dir>   Folder containing ./suffering and ./suffering.png (default: script directory)
  --steam-dir <dir>     Explicit Steam root containing userdata/ (default: auto-detected)
  --no-steam            Install .desktop + icon only; skip Steam shortcuts.vdf registration
  --uninstall           Remove installed .desktop entry, icon, and Steam Non-Steam shortcut
  -h, --help            Show this help text
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --install-dir)
            INSTALL_DIR="$2"
            shift 2
            ;;
        --install-dir=*)
            INSTALL_DIR="${1#*=}"
            shift
            ;;
        --steam-dir)
            STEAM_DIR="$2"
            shift 2
            ;;
        --steam-dir=*)
            STEAM_DIR="${1#*=}"
            shift
            ;;
        --no-steam)
            REGISTER_STEAM=0
            shift
            ;;
        --uninstall)
            UNINSTALL=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

INSTALL_DIR="$(cd "$INSTALL_DIR" && pwd)"
EXE_PATH="$INSTALL_DIR/suffering"
ICON_SRC="$INSTALL_DIR/suffering.png"
if [[ ! -f "$ICON_SRC" && -f "$INSTALL_DIR/src/res/suffering.png" ]]; then
    ICON_SRC="$INSTALL_DIR/src/res/suffering.png"
fi

APPS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
ICONS_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps"
ICON_DST="$ICONS_DIR/${DESKTOP_ID}.png"
DESKTOP_DST="$APPS_DIR/${DESKTOP_ID}.desktop"

# Locate Desktop directory if present
DESKTOP_DIR=""
if [[ -f "${XDG_CONFIG_HOME:-$HOME/.config}/user-dirs.dirs" ]]; then
    # shellcheck disable=SC1090
    source "${XDG_CONFIG_HOME:-$HOME/.config}/user-dirs.dirs" || true
    DESKTOP_DIR="${XDG_DESKTOP_DIR:-}"
fi
if [[ -z "$DESKTOP_DIR" && -d "$HOME/Desktop" ]]; then
    DESKTOP_DIR="$HOME/Desktop"
fi
DESKTOP_LINK="${DESKTOP_DIR:+$DESKTOP_DIR/The Suffering.desktop}"

# Auto-detect Steam installation root
if [[ -z "$STEAM_DIR" ]]; then
    for cand in \
        "$HOME/.local/share/Steam" \
        "$HOME/.steam/steam" \
        "$HOME/.steam/root" \
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        if [[ -d "$cand/userdata" ]]; then
            STEAM_DIR="$cand"
            break
        fi
    done
fi

run_vdf_helper() {
    local mode="$1"
    python3 - "$mode" "$STEAM_DIR" "$APP_NAME" "$EXE_PATH" "$INSTALL_DIR" "$ICON_DST" <<'PYEOF'
import os
import shutil
import struct
import sys
import zlib
from pathlib import Path

mode, steam_dir_s, app_name, exe_path, start_dir, icon_path = sys.argv[1:7]

def read_cstr(buf: bytes, pos: int) -> tuple[str, int]:
    end = buf.find(b"\x00", pos)
    if end < 0:
        raise ValueError(f"Unterminated VDF string at offset {pos}")
    return buf[pos:end].decode("utf-8", errors="surrogateescape"), end + 1

def parse_vdf_map(buf: bytes, pos: int) -> tuple[dict, int]:
    out = {}
    n = len(buf)
    while pos < n:
        tag = buf[pos]
        pos += 1
        if tag == 0x08:
            return out, pos
        key, pos = read_cstr(buf, pos)
        if tag == 0x00:
            val, pos = parse_vdf_map(buf, pos)
        elif tag == 0x01:
            val, pos = read_cstr(buf, pos)
        elif tag == 0x02:
            if pos + 4 > n:
                raise ValueError(f"Truncated uint32 for key {key}")
            val = struct.unpack_from("<I", buf, pos)[0]
            pos += 4
        else:
            raise ValueError(f"Unsupported binary VDF type 0x{tag:02x} for key {key}")
        out[key] = val
    return out, pos

def encode_vdf_map(d: dict) -> bytes:
    out = bytearray()
    for k, v in d.items():
        kb = str(k).encode("utf-8", errors="surrogateescape") + b"\x00"
        if isinstance(v, dict):
            out.append(0x00)
            out.extend(kb)
            out.extend(encode_vdf_map(v))
        elif isinstance(v, int):
            out.append(0x02)
            out.extend(kb)
            out.extend(struct.pack("<I", v & 0xFFFFFFFF))
        else:
            vb = str(v).encode("utf-8", errors="surrogateescape") + b"\x00"
            out.append(0x01)
            out.extend(kb)
            out.extend(vb)
    out.append(0x08)
    return bytes(out)

def canonical_appid(exe_quoted: str, name: str) -> int:
    crc = zlib.crc32((exe_quoted + name).encode("utf-8")) & 0xFFFFFFFF
    return (crc | 0x80000000) & 0xFFFFFFFF

def load_shortcuts(vdf_path: Path) -> list[dict]:
    if not vdf_path.is_file() or vdf_path.stat().st_size == 0:
        return []
    raw = vdf_path.read_bytes()
    root, _ = parse_vdf_map(raw, 0)
    sc = root.get("shortcuts", {})
    if not isinstance(sc, dict):
        return []
    items = []
    for k in sorted(sc.keys(), key=lambda x: int(x) if str(x).isdigit() else 999999):
        if isinstance(sc[k], dict):
            items.append(sc[k])
    return items

def save_shortcuts(vdf_path: Path, entries: list[dict]) -> None:
    vdf_path.parent.mkdir(parents=True, exist_ok=True)
    if vdf_path.is_file():
        bak = vdf_path.with_suffix(".vdf.bak")
        if not bak.exists():
            shutil.copy2(vdf_path, bak)
    sc_map = {str(i): entry for i, entry in enumerate(entries)}
    blob = encode_vdf_map({"shortcuts": sc_map})
    tmp = vdf_path.with_suffix(".vdf.tmp")
    tmp.write_bytes(blob)
    os.replace(tmp, vdf_path)

steam_root = Path(steam_dir_s)
userdata = steam_root / "userdata"
if not userdata.is_dir():
    print(f"Steam userdata not found under {steam_root}; skipping Steam shortcut registration.")
    sys.exit(0)

user_dirs = sorted(
    p for p in userdata.iterdir()
    if p.is_dir() and p.name.isdigit() and p.name != "0"
)
if not user_dirs:
    print(f"No Steam user profiles found in {userdata}; skipping Steam shortcut registration.")
    sys.exit(0)

exe_quoted = f'"{exe_path}"'
start_quoted = f'"{start_dir.rstrip("/")}/"'
target_entry = {
    "appid": canonical_appid(exe_quoted, app_name),
    "AppName": app_name,
    "Exe": exe_quoted,
    "StartDir": start_quoted,
    "icon": icon_path if Path(icon_path).is_file() else "",
    "ShortcutPath": "",
    "LaunchOptions": "",
    "IsHidden": 0,
    "AllowDesktopConfig": 1,
    "AllowOverlay": 1,
    "OpenVR": 0,
    "Devkit": 0,
    "DevkitGameID": "",
    "DevkitOverrideAppID": 0,
    "LastPlayTime": 0,
    "FlatpakAppID": "",
    "tags": {"0": "Recomp"},
}

def matches_ours(entry: dict) -> bool:
    name = str(entry.get("AppName", entry.get("appname", "")))
    exe = str(entry.get("Exe", entry.get("exe", ""))).strip('"')
    return name == app_name or exe == exe_path

updated = 0
for udir in user_dirs:
    vdf_path = udir / "config" / "shortcuts.vdf"
    entries = load_shortcuts(vdf_path)
    if mode == "uninstall":
        new_entries = [e for e in entries if not matches_ours(e)]
        if len(new_entries) != len(entries):
            save_shortcuts(vdf_path, new_entries)
            print(f"Removed Steam shortcut from {vdf_path}")
            updated += 1
    else:
        replaced = False
        for i, e in enumerate(entries):
            if matches_ours(e):
                # Preserve existing LastPlayTime if already present
                if isinstance(e.get("LastPlayTime"), int):
                    target_entry["LastPlayTime"] = e["LastPlayTime"]
                entries[i] = dict(target_entry)
                replaced = True
                break
        if not replaced:
            entries.append(dict(target_entry))
        save_shortcuts(vdf_path, entries)
        print(f"{'Updated' if replaced else 'Added'} Steam shortcut in {vdf_path} (appid=0x{target_entry['appid']:08X})")
        updated += 1

print(f"Steam shortcuts.vdf profiles processed: {updated}")
PYEOF
}

if [[ "$UNINSTALL" -eq 1 ]]; then
    rm -f "$DESKTOP_DST" "$ICON_DST"
    if [[ -n "$DESKTOP_LINK" ]]; then
        rm -f "$DESKTOP_LINK"
    fi
    if [[ "$REGISTER_STEAM" -eq 1 && -n "$STEAM_DIR" ]]; then
        run_vdf_helper uninstall
    fi
    echo "Uninstalled $APP_NAME desktop entries and Steam shortcut."
    exit 0
fi

if [[ ! -f "$EXE_PATH" ]]; then
    echo "ERROR: $EXE_PATH not found. Run this script from the unpacked release directory or pass --install-dir." >&2
    exit 1
fi

chmod +x "$EXE_PATH"
for tool in "$INSTALL_DIR/compiler/xbe_recomp" "$INSTALL_DIR/compiler/clang" "$INSTALL_DIR/compiler/ld.lld"; do
    if [[ -f "$tool" ]]; then
        chmod +x "$tool"
    fi
done

mkdir -p "$APPS_DIR" "$ICONS_DIR"
if [[ -f "$ICON_SRC" ]]; then
    cp -f "$ICON_SRC" "$ICON_DST"
    chmod 0644 "$ICON_DST"
fi

write_desktop_file() {
    local dst="$1"
    cat >"$dst" <<EOF
# SPDX-License-Identifier: GPL-2.0-or-later
[Desktop Entry]
Type=Application
Name=${APP_NAME}
GenericName=Action Horror Game
Comment=The Suffering (USA) Native Linux Static Recompilation (Vulkan 1.2 + SDL3)
Exec="${EXE_PATH}"
Path=${INSTALL_DIR}
Icon=${ICON_DST}
Terminal=false
Categories=Game;ActionGame;
StartupNotify=true
EOF
    chmod 0755 "$dst"
}

write_desktop_file "$DESKTOP_DST"
echo "Installed desktop entry: $DESKTOP_DST"

if [[ -n "$DESKTOP_LINK" ]]; then
    write_desktop_file "$DESKTOP_LINK"
    echo "Installed Desktop shortcut: $DESKTOP_LINK"
fi

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
fi

if [[ "$REGISTER_STEAM" -eq 1 ]]; then
    if [[ -n "$STEAM_DIR" ]]; then
        run_vdf_helper install
        echo ""
        echo "Steam Deck / SteamOS Note:"
        echo "  - Restart Steam (or switch back to Gaming Mode) to see '${APP_NAME}' in Non-Steam Games."
        echo "  - Do NOT force a Proton compatibility tool: this build runs NATIVELY on Linux (Vulkan 1.2 + SDL3)."
    else
        echo "Steam directory not detected; skipped Steam shortcuts.vdf (pass --steam-dir <path> to register)."
    fi
fi
