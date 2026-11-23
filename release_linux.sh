#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Stages and packages TheSuffering-Recomp-v1.4.0-linux-x86_64.tar.gz for Native Linux x86_64 & Steam Deck.
# Bundles stripped non-LSB shared library dependencies for compiler/{clang,ld.lld,xbe_recomp} inside
# compiler/lib/ with $ORIGIN:$ORIGIN/lib RPATH so stock read-only SteamOS 3.x requires ZERO system LLVM packages.
# Strictly excludes libsuffering.so, *.dll, *.exe, *.xbe, *.iso, compiler/work/, and runtime state.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="1.4.0"
BUILD_DIR="$REPO_ROOT/out/build/linux-release"
OUT_TAR=""

usage() {
    cat <<'EOF'
Usage: ./release_linux.sh [options]

Options:
  --build-dir <dir>   CMake Linux build output directory (default: out/build/linux-release)
  --out-tar <path>    Output .tar.gz path (default: .local/dist/TheSuffering-Recomp-v1.4.0-linux-x86_64.tar.gz)
  --version <ver>     Release version tag without 'v' (default: 1.4.0)
  -h, --help          Show this help text
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        --build-dir=*)
            BUILD_DIR="${1#*=}"
            shift
            ;;
        --out-tar)
            OUT_TAR="$2"
            shift 2
            ;;
        --out-tar=*)
            OUT_TAR="${1#*=}"
            shift
            ;;
        --version)
            VERSION="$2"
            shift 2
            ;;
        --version=*)
            VERSION="${1#*=}"
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

if [[ -z "$OUT_TAR" ]]; then
    OUT_TAR="$REPO_ROOT/.local/dist/TheSuffering-Recomp-v${VERSION}-linux-x86_64.tar.gz"
fi

mkdir -p "$(dirname "$OUT_TAR")"

for req in \
    "$BUILD_DIR/suffering" \
    "$BUILD_DIR/compiler/xbe_recomp" \
    "$BUILD_DIR/compiler/clang" \
    "$BUILD_DIR/compiler/ld.lld" \
    "$BUILD_DIR/compiler/LLVM-LICENSE.txt" \
    "$BUILD_DIR/compiler/config/game.toml" \
    "$BUILD_DIR/compiler/xdk/5788/layout.toml" \
    "$REPO_ROOT/src/res/suffering.png" \
    "$REPO_ROOT/packaging/steamdeck/install-steamdeck.sh" \
    "$REPO_ROOT/packaging/steamdeck/the-suffering-recomp.desktop" \
    "$REPO_ROOT/XboxXBERecomp/LICENSE" \
    "$REPO_ROOT/XboxXBERecomp/THIRD_PARTY.txt"; do
    if [[ ! -e "$req" ]]; then
        echo "ERROR: Required build/packaging input missing: $req" >&2
        exit 1
    fi
done

STAGE="$(mktemp -d "${TMPDIR:-/tmp}/suffering-linux-stage.XXXXXX")"
trap 'rm -rf "$STAGE"' EXIT

mkdir -p "$STAGE/assets" "$STAGE/compiler/lib"

# 1. Host executable & metadata
cp -L "$BUILD_DIR/suffering" "$STAGE/suffering"
chmod 0755 "$STAGE/suffering"

cp "$REPO_ROOT/packaging/steamdeck/install-steamdeck.sh" "$STAGE/install-steamdeck.sh"
chmod 0755 "$STAGE/install-steamdeck.sh"
cp "$REPO_ROOT/packaging/steamdeck/the-suffering-recomp.desktop" "$STAGE/the-suffering-recomp.desktop"
chmod 0644 "$STAGE/the-suffering-recomp.desktop"
cp "$REPO_ROOT/src/res/suffering.png" "$STAGE/suffering.png"
chmod 0644 "$STAGE/suffering.png"

cp "$REPO_ROOT/README.md" "$STAGE/README.md"
cp "$REPO_ROOT/LICENSE" "$STAGE/LICENSE"
cp "$REPO_ROOT/THIRD_PARTY_NOTICES.md" "$STAGE/THIRD_PARTY_NOTICES.md"
cp "$REPO_ROOT/XboxXBERecomp/LICENSE" "$STAGE/SDK-LICENSE.txt"
cp "$REPO_ROOT/XboxXBERecomp/THIRD_PARTY.txt" "$STAGE/THIRD_PARTY.txt"

# 2. First-launch compiler toolchain (zero compiler/work/)
cp -L "$BUILD_DIR/compiler/xbe_recomp" "$STAGE/compiler/xbe_recomp"
cp -L "$BUILD_DIR/compiler/clang" "$STAGE/compiler/clang"
cp -L "$BUILD_DIR/compiler/ld.lld" "$STAGE/compiler/ld.lld"
chmod 0755 "$STAGE/compiler/xbe_recomp" "$STAGE/compiler/clang" "$STAGE/compiler/ld.lld"

cp -a "$BUILD_DIR/compiler/include" "$STAGE/compiler/include"
cp -a "$BUILD_DIR/compiler/config" "$STAGE/compiler/config"
cp -a "$BUILD_DIR/compiler/xdk" "$STAGE/compiler/xdk"
cp "$BUILD_DIR/compiler/LLVM-LICENSE.txt" "$STAGE/compiler/LLVM-LICENSE.txt"

# 3. Bundle non-LSB shared libraries for compiler/{xbe_recomp,clang,ld.lld} into compiler/lib/
is_lsb_system_lib() {
    case "$1" in
        linux-vdso.so*|ld-linux-x86-64.so*|libc.so.6|libm.so.6|libstdc++.so.6|libgcc_s.so.1|libdl.so.2|libpthread.so.0|librt.so.1)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

collect_deps() {
    local bin="$1"
    local line soname resolved cand
    while IFS= read -r line; do
        if [[ "$line" =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]*=\>[[:space:]]*([^[:space:]]+) ]]; then
            soname="${BASH_REMATCH[1]}"
            resolved="${BASH_REMATCH[2]}"
            if is_lsb_system_lib "$soname"; then
                continue
            fi
            if [[ "$resolved" == "not" || ! -f "$resolved" ]]; then
                resolved=""
                for cand in /usr/lib/llvm-*/lib/"$soname" /usr/lib/x86_64-linux-gnu/"$soname" /usr/local/lib/"$soname"; do
                    if [[ -f "$cand" ]]; then
                        resolved="$cand"
                        break
                    fi
                done
                if [[ -z "$resolved" ]]; then
                    echo "ERROR: Unresolved shared library dependency for $bin: $soname" >&2
                    exit 1
                fi
            fi
            if [[ ! -f "$STAGE/compiler/lib/$soname" ]]; then
                cp -L "$resolved" "$STAGE/compiler/lib/$soname"
                chmod 0755 "$STAGE/compiler/lib/$soname"
                collect_deps "$STAGE/compiler/lib/$soname"
            fi
        fi
    done < <(ldd "$bin" 2>/dev/null || true)
}

collect_deps "$STAGE/compiler/xbe_recomp"
collect_deps "$STAGE/compiler/clang"
collect_deps "$STAGE/compiler/ld.lld"

# Strip debug info from bundled compiler libs and set $ORIGIN RPATHs
if command -v strip >/dev/null 2>&1; then
    for so in "$STAGE/compiler/lib"/*.so*; do
        [[ -f "$so" ]] || continue
        strip --strip-unneeded "$so" 2>/dev/null || true
    done
fi

if command -v patchelf >/dev/null 2>&1; then
    for bin in "$STAGE/compiler/xbe_recomp" "$STAGE/compiler/clang" "$STAGE/compiler/ld.lld"; do
        patchelf --force-rpath --set-rpath '$ORIGIN:$ORIGIN/lib' "$bin"
    done
    for so in "$STAGE/compiler/lib"/*.so*; do
        [[ -f "$so" ]] || continue
        patchelf --force-rpath --set-rpath '$ORIGIN' "$so"
    done
else
    if compgen -G "$STAGE/compiler/lib/*.so*" >/dev/null; then
        echo "ERROR: patchelf is required to set RPATH on bundled compiler/lib/*.so*" >&2
        exit 1
    fi
fi

# Collect verbatim /usr/share/doc/<pkg>/copyright notices for every bundled compiler/lib/*.so
if compgen -G "$STAGE/compiler/lib/*.so*" >/dev/null && command -v dpkg >/dev/null 2>&1; then
    LIB_NOTICES="$STAGE/compiler/lib/THIRD_PARTY_LIBS.txt"
    {
        echo "Third-Party Copyright & License Notices for Bundled compiler/lib/*.so"
        echo "======================================================================"
    } >"$LIB_NOTICES"
    declare -A seen_pkgs=()
    for so in "$STAGE/compiler/lib"/*.so*; do
        [[ -f "$so" ]] || continue
        soname="$(basename "$so")"
        pkg="$(dpkg -S "/usr/lib/x86_64-linux-gnu/$soname" "/lib/x86_64-linux-gnu/$soname" /usr/lib/llvm-*/lib/"$soname" 2>/dev/null | head -n1 | cut -d: -f1 || true)"
        if [[ -n "$pkg" && -z "${seen_pkgs[$pkg]:-}" && -f "/usr/share/doc/$pkg/copyright" ]]; then
            seen_pkgs["$pkg"]=1
            printf "\n----------------------------------------------------------------------\nPackage: %s (%s)\n----------------------------------------------------------------------\n" "$pkg" "$soname" >>"$LIB_NOTICES"
            cat "/usr/share/doc/$pkg/copyright" >>"$LIB_NOTICES"
        fi
    done
    chmod 0644 "$LIB_NOTICES"
fi

# Remove compiler/lib if empty (e.g. statically linked clang/lld)
rmdir "$STAGE/compiler/lib" 2>/dev/null || true

# 4. Hygiene assertion: zero game-derived libs, Windows binaries, copyrighted media, or work dirs
if find "$STAGE" \( -name 'libsuffering.so' -o -name '*.dll' -o -name '*.exe' -o -name '*.xbe' -o -name '*.iso' -o -name '*.xiso' -o -name 'work' \) | grep -q .; then
    echo "ERROR: Forbidden artifact detected in staging directory!" >&2
    find "$STAGE" \( -name 'libsuffering.so' -o -name '*.dll' -o -name '*.exe' -o -name '*.xbe' -o -name '*.iso' -o -name '*.xiso' -o -name 'work' \) >&2
    exit 1
fi

# 5. Normalize file permissions (avoid inheriting 0777 from NTFS/WSL2 mounts)
find "$STAGE" -type d -exec chmod 0755 {} +
find "$STAGE" -type f -exec chmod 0644 {} +
chmod 0755 "$STAGE/suffering" "$STAGE/install-steamdeck.sh" \
           "$STAGE/compiler/xbe_recomp" "$STAGE/compiler/clang" "$STAGE/compiler/ld.lld"
if [[ -d "$STAGE/compiler/lib" ]]; then
    find "$STAGE/compiler/lib" -type f -name '*.so*' -exec chmod 0755 {} +
fi

# 6. Create reproducible gzipped tarball
rm -f "$OUT_TAR"
tar -C "$STAGE" -czf "$OUT_TAR" .

SIZE_BYTES="$(stat -c%s "$OUT_TAR")"
SHA256="$(sha256sum "$OUT_TAR" | awk '{print $1}')"
echo "Packaged: $OUT_TAR"
echo "  Size:   $SIZE_BYTES bytes"
echo "  SHA256: $SHA256"
