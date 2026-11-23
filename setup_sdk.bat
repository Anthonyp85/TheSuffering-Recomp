@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
setlocal
cd /d "%~dp0"

set "SDK_COMMIT=cc9edda3a821a4204dc28783f429b88a27ade4f9"
set "PATCH_FILE=%CD%\patches\xboxxberecomp.patch"
if not defined XBE_SDK_GIT_URL set "XBE_SDK_GIT_URL=https://github.com/TGP482/XboxXBERecomp.git"

set "DEFAULT_SDK=%CD%\XboxXBERecomp"
if defined XBE_SDK_DIR (
    set "SDK_DIR=%XBE_SDK_DIR%"
) else (
    set "SDK_DIR=%DEFAULT_SDK%"
)

where git >nul 2>nul
if errorlevel 1 (
    echo       ERROR: git is required to initialize and patch XboxXBERecomp.
    exit /b 1
)

if exist "%SDK_DIR%\cmake\XbeRecomp.cmake" goto :check_patch

echo       Initializing XboxXBERecomp SDK at %SDK_COMMIT% ...
if exist "%CD%\.git" if /i "%SDK_DIR%"=="%DEFAULT_SDK%" if /i "%XBE_SDK_GIT_URL%"=="https://github.com/TGP482/XboxXBERecomp.git" (
    git submodule update --init XboxXBERecomp >nul 2>nul
)

if not exist "%SDK_DIR%\cmake\XbeRecomp.cmake" (
    if exist "%SDK_DIR%" rd /s /q "%SDK_DIR%" 2>nul
    git clone "%XBE_SDK_GIT_URL%" "%SDK_DIR%" || exit /b 1
    git -C "%SDK_DIR%" checkout %SDK_COMMIT% || exit /b 1
)

:check_patch
if not exist "%PATCH_FILE%" (
    echo       ERROR: SDK patch file not found: "%PATCH_FILE%"
    exit /b 1
)

git -C "%SDK_DIR%" apply --reverse --check --whitespace=nowarn "%PATCH_FILE%" >nul 2>nul
if not errorlevel 1 (
    echo       XboxXBERecomp patch already applied.
    exit /b 0
)

git -C "%SDK_DIR%" apply --check --whitespace=nowarn "%PATCH_FILE%" >nul 2>nul
if not errorlevel 1 (
    git -C "%SDK_DIR%" apply --whitespace=nowarn "%PATCH_FILE%" || exit /b 1
    if exist "%SDK_DIR%\.git" (
        git -C "%SDK_DIR%" add -N xdk/5788/layout.toml xdk/5788/signatures.toml runtime/core/perf.h runtime/core/perf.cpp runtime/audio/sdl_audio.cpp runtime/gpu/dxgi_compat.h runtime/gpu/vulkan.cpp runtime/app/main_linux.cpp >nul 2>nul
    )
    echo       Applied patches\xboxxberecomp.patch cleanly.
    exit /b 0
)

echo       ERROR: patches\xboxxberecomp.patch could not be applied cleanly to "%SDK_DIR%".
echo       Reset XboxXBERecomp to pinned commit %SDK_COMMIT% and re-run setup_sdk.bat:
echo         git -C "%SDK_DIR%" reset --hard %SDK_COMMIT%
echo         git -C "%SDK_DIR%" clean -fd
exit /b 1
