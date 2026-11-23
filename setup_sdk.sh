#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

SDK_COMMIT="cc9edda3a821a4204dc28783f429b88a27ade4f9"
PATCH_FILE="${SCRIPT_DIR}/patches/xboxxberecomp.patch"
XBE_SDK_GIT_URL="${XBE_SDK_GIT_URL:-https://github.com/TGP482/XboxXBERecomp.git}"

DEFAULT_SDK="${SCRIPT_DIR}/XboxXBERecomp"
SDK_DIR="${XBE_SDK_DIR:-$DEFAULT_SDK}"

if ! command -v git >/dev/null 2>&1; then
    echo "      ERROR: git is required to initialize and patch XboxXBERecomp." >&2
    exit 1
fi

if [[ ! -f "${SDK_DIR}/cmake/XbeRecomp.cmake" ]]; then
    echo "      Initializing XboxXBERecomp SDK at ${SDK_COMMIT} ..."
    if [[ -d "${SCRIPT_DIR}/.git" && "${SDK_DIR}" == "${DEFAULT_SDK}" && "${XBE_SDK_GIT_URL}" == "https://github.com/TGP482/XboxXBERecomp.git" ]]; then
        git submodule update --init XboxXBERecomp >/dev/null 2>&1 || true
    fi
    if [[ ! -f "${SDK_DIR}/cmake/XbeRecomp.cmake" ]]; then
        rm -rf "${SDK_DIR}"
        git clone "${XBE_SDK_GIT_URL}" "${SDK_DIR}"
        git -C "${SDK_DIR}" checkout "${SDK_COMMIT}"
    fi
fi

if [[ ! -f "${PATCH_FILE}" ]]; then
    echo "      ERROR: SDK patch file not found: \"${PATCH_FILE}\"" >&2
    exit 1
fi

if git -C "${SDK_DIR}" apply --reverse --check --whitespace=nowarn "${PATCH_FILE}" >/dev/null 2>&1; then
    echo "      XboxXBERecomp patch already applied."
    exit 0
fi

if git -C "${SDK_DIR}" apply --check --whitespace=nowarn "${PATCH_FILE}" >/dev/null 2>&1; then
    git -C "${SDK_DIR}" apply --whitespace=nowarn "${PATCH_FILE}"
    if [[ -e "${SDK_DIR}/.git" ]]; then
        git -C "${SDK_DIR}" add -N \
            xdk/5788/layout.toml \
            xdk/5788/signatures.toml \
            runtime/core/perf.h \
            runtime/core/perf.cpp \
            runtime/audio/sdl_audio.cpp \
            runtime/gpu/dxgi_compat.h \
            runtime/gpu/vulkan.cpp \
            runtime/app/main_linux.cpp >/dev/null 2>&1 || true
    fi
    echo "      Applied patches/xboxxberecomp.patch cleanly."
    exit 0
fi

echo "      ERROR: patches/xboxxberecomp.patch could not be applied cleanly to \"${SDK_DIR}\"." >&2
echo "      Reset XboxXBERecomp to pinned commit ${SDK_COMMIT} and re-run ./setup_sdk.sh:" >&2
echo "        git -C \"${SDK_DIR}\" reset --hard ${SDK_COMMIT}" >&2
echo "        git -C \"${SDK_DIR}\" clean -fd" >&2
exit 1
