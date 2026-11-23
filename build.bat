@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
setlocal
set "GAME_TITLE=The Suffering"
set "GAME_XBE=default.xbe"
set "GAME_EXE=suffering"
set "NO_PROMPT="
if defined CI set "NO_PROMPT=1"
if /i "%~1"=="--no-prompt" set "NO_PROMPT=1"
if /i "%~2"=="--no-prompt" set "NO_PROMPT=1"

title %GAME_TITLE% - Build
cd /d "%~dp0"
echo.
echo   %GAME_TITLE% - build
echo   ================================================
echo.

set "PF86=%ProgramFiles(x86)%"
set "VSINST=%PF86%\Microsoft Visual Studio\Installer"
set "VSWHERE=%VSINST%\vswhere.exe"
set "OUT=%CD%\out\build\release"
set "DEFAULT_ISO=%CD%\Suffering, The (USA)\Suffering, The (USA).xiso.iso"
if not defined XBE_ISO_PATH set "XBE_ISO_PATH=%DEFAULT_ISO%"
set "XBE_LOCAL_PATH=%CD%\.local\xbe\%GAME_XBE%"

if exist "%XBE_LOCAL_PATH%" goto :xbe_ready
if not exist "%XBE_ISO_PATH%" goto :nogame
echo [0/4] Extracting %GAME_XBE% from disc image (read-only)
where python >nul 2>nul || goto :nopython
python "%CD%\tools\extract_xbe.py" --iso "%XBE_ISO_PATH%" --out "%XBE_LOCAL_PATH%" || goto :fail
:xbe_ready

rem toolchain
echo [1/4] Toolchain
set "VSDIR="
if exist "%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%PF86%\Microsoft Visual Studio\2022\BuildTools"
if not defined VSDIR if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto :novs
call :setuptools
if not defined TOOLS_OK goto :missing_tools
for /f "tokens=*" %%v in ('clang++ --version ^| findstr /i version') do echo       %%v

rem sdk bootstrap & idempotent patch
:sdk_setup
echo [2/4] XboxXBERecomp SDK setup
call "%CD%\setup_sdk.bat" || goto :fail

rem symbols
:symbols
echo [3/4] XDK symbols
cmake --preset release %XBE_CMAKE_ARGS% || goto :fail
findstr /b /l /c:"[symbols]" config\game.toml >nul 2>nul && echo       config\game.toml already written && goto :game
cmake --build out\build\release --target xbe_symbols || goto :fail
if not exist out\build\release\sdk_dir.txt echo       out\build\release\sdk_dir.txt was not written, update the SDK. && goto :fail
set /p SDK=<out\build\release\sdk_dir.txt
if not exist config mkdir config
"%OUT%\xbe_symbols.exe" "%XBE_LOCAL_PATH%" "%SDK%\xdk" --library %GAME_EXE%.dll --out config\game.toml || goto :fail

rem game
:game
echo [4/4] Game code
echo       first build recompiles %GAME_XBE%, expect several minutes ...
cmake --build out\build\release || goto :fail

if exist "%CD%\.local\vendor_sdks\dlss\nvngx_dlss.dll" (
    python "%CD%\tools\fetch_upscalers.py" --offline --cache-dir "%CD%\.local\vendor_sdks" --stage-dir "%OUT%\plugins" || goto :fail
)

if exist "%OUT%\settings.toml" goto :settings_done
if not exist "%XBE_ISO_PATH%" goto :settings_done
if /i "%XBE_ISO_PATH%"=="%DEFAULT_ISO%" goto :write_default_settings
python -c "import json, os, pathlib; p=pathlib.Path(os.environ['OUT'])/'settings.toml'; p.write_text('iso_path = ' + json.dumps(os.environ['XBE_ISO_PATH']) + '\n', encoding='utf-8')" || goto :fail
goto :settings_done
:write_default_settings
>"%OUT%\settings.toml" echo iso_path = '..\..\..\Suffering, The (USA)\Suffering, The (USA).xiso.iso'
:settings_done

echo.
echo   %GAME_TITLE% built.
echo   out\build\release\%GAME_EXE%.exe
echo.
if defined NO_PROMPT exit /b 0
choice /c YN /n /m "  Play now? [Y/N] "
if errorlevel 2 goto :pause_done
start "" /d "%OUT%" "%OUT%\%GAME_EXE%.exe"
exit /b 0
:pause_done
pause
exit /b 0

rem helpers
:setuptools
set TOOLS_OK=
set MISSING=
call "%VSDIR%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
set "VSCMAKE=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%PATH%"
if exist "%VSCMAKE%\Ninja\ninja.exe" set "PATH=%VSCMAKE%\Ninja;%PATH%"
if exist "%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang++.exe" set "PATH=%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%VSDIR%\VC\Tools\Llvm\x64\bin\clang++.exe" set "PATH=%VSDIR%\VC\Tools\Llvm\x64\bin;%PATH%"
if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "PATH=%ProgramFiles%\LLVM\bin;%PATH%"
where clang    >nul 2>nul || set "MISSING=%MISSING% clang"
where clang++  >nul 2>nul || set "MISSING=%MISSING% clang++"
where lld-link >nul 2>nul || set "MISSING=%MISSING% lld-link"
where rc       >nul 2>nul || set "MISSING=%MISSING% rc"
where cmake    >nul 2>nul || set "MISSING=%MISSING% cmake"
where ninja    >nul 2>nul || set "MISSING=%MISSING% ninja"
where git      >nul 2>nul || set "MISSING=%MISSING% git"
where python   >nul 2>nul || set "MISSING=%MISSING% python"
if defined MISSING echo       missing:%MISSING% && exit /b 0
set TOOLS_OK=1
exit /b 0

:novs
echo       Visual Studio with the C++ workload was not found.
echo       Install Visual Studio Community or Build Tools with "Desktop development with C++".
goto :fail

:missing_tools
if defined NO_PROMPT goto :byhand
choice /c YN /n /m "      Let Visual Studio install the missing Clang/CMake components? [Y/N] "
if errorlevel 2 goto :byhand
echo       Close Visual Studio if it is open, it will be shut down anyway.
echo       Installing. Accept the admin prompt, then wait for the installer.
powershell -NoProfile -Command "Start-Process -Verb RunAs -Wait -FilePath '%VSINST%\setup.exe' -ArgumentList 'modify','--installPath','%VSDIR%','--add','Microsoft.VisualStudio.Component.VC.Llvm.Clang','--add','Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset','--add','Microsoft.VisualStudio.Component.VC.CMake.Project','--passive','--norestart','--force'"
echo       Press a key once the Visual Studio Installer window has closed.
pause >nul
call :setuptools
if not defined TOOLS_OK goto :byhand
goto :after_install
:after_install
for /f "tokens=*" %%v in ('clang++ --version ^| findstr /i version') do echo       %%v
goto :sdk_setup

:byhand
echo.
echo       Install missing tools, then run this again:
echo         clang/clang++/lld-link - LLVM for Windows (C:\Program Files\LLVM\bin) or VS "C++ Clang tools for Windows"
echo         rc                     - Windows 10/11 SDK (Visual Studio C++ workload)
echo         cmake / ninja          - VS "C++ CMake tools for Windows"
echo         git                    - https://git-scm.com/download/win
echo         python                 - https://www.python.org/downloads/
goto :fail

:nopython
echo   Python 3 is required to extract %GAME_XBE% from the Xbox disc image.
goto :fail

:nogame
echo   Original Xbox disc image not found:
echo     "%XBE_ISO_PATH%"
echo   Provide The Suffering (USA) .iso/.xiso.iso via XBE_ISO_PATH or place default.xbe at:
echo     "%XBE_LOCAL_PATH%"
goto :fail

:fail
echo.
echo   BUILD FAILED. Read the last message above.
echo.
if not defined NO_PROMPT pause
exit /b 1
