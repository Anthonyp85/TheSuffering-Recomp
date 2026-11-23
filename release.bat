@echo off
rem SPDX-License-Identifier: GPL-2.0-or-later
setlocal
set "GAME_EXE=suffering"
cd /d "%~dp0"
set "OUT=out\build\release"
set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%PF86%\Microsoft Visual Studio\2022\BuildTools\VC\Redist\MSVC" set "VSDIR=%PF86%\Microsoft Visual Studio\2022\BuildTools"
if not defined VSDIR for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
set "VCRUNTIME="
for /d %%v in ("%VSDIR%\VC\Redist\MSVC\1*") do for /d %%c in ("%%v\x64\Microsoft.VC*.CRT") do set "VCRUNTIME=%%c"
if not exist "%VCRUNTIME%\vcruntime140.dll" echo Visual C++ runtime not found: install Visual Studio with the C++ workload && exit /b 1

set "SEVENZ="
for /f "delims=" %%z in ('where 7z 2^>nul') do if not defined SEVENZ set "SEVENZ=%%z"
if not defined SEVENZ if exist "%ProgramFiles%\7-Zip\7z.exe" set "SEVENZ=%ProgramFiles%\7-Zip\7z.exe"
if not defined SEVENZ echo 7-Zip (7z.exe) not found in PATH or "%ProgramFiles%\7-Zip\7z.exe" && exit /b 1

set "PY="
for /f "delims=" %%p in ('where python 2^>nul') do if not defined PY set "PY=%%p"
if not defined PY echo Python 3 (python.exe) not found in PATH && exit /b 1

if not exist "%OUT%\%GAME_EXE%.exe" call build.bat --no-prompt || exit /b 1
if not exist "%OUT%\sdk_dir.txt" echo %OUT%\sdk_dir.txt was not written, run build.bat first. && exit /b 1
set /p SDK=<out\build\release\sdk_dir.txt
del /q "%GAME_EXE%.zip" 2>nul

rem Empty assets folder beside the exe where the player places their own disc image (.iso / .xiso.iso)
rd /s /q "%OUT%\release" 2>nul
mkdir "%OUT%\release\assets"
mkdir "%OUT%\release\plugins"
"%PY%" "%~dp0tools\fetch_upscalers.py" --cache-dir "%~dp0.local\vendor_sdks" --stage-dir "%OUT%\release\plugins" || exit /b 1
copy /y "%SDK%\LICENSE" "%OUT%\release\SDK-LICENSE.txt" >nul

rem Copy VC runtime DLLs needed by xbe_recomp.exe inside compiler\
del /q "%OUT%\compiler\*.dll" 2>nul
for %%f in (msvcp140 vcruntime140 vcruntime140_1) do if exist "%VCRUNTIME%\%%f.dll" copy /y "%VCRUNTIME%\%%f.dll" "%OUT%\compiler\" >nul

rem Package ONLY clean redistributable binaries/docs (zero copyrighted assets, XBE, ISO, or game-derived code)
"%SEVENZ%" a "%GAME_EXE%.zip" ".\%OUT%\%GAME_EXE%.exe" ".\%OUT%\release\assets" ".\%OUT%\release\plugins" ".\%OUT%\compiler" .\README.md .\LICENSE .\THIRD_PARTY_NOTICES.md ".\%OUT%\release\SDK-LICENSE.txt" "%SDK%\THIRD_PARTY.txt" "%VCRUNTIME%\msvcp140.dll" "%VCRUNTIME%\vcruntime140.dll" "%VCRUNTIME%\vcruntime140_1.dll" -xr!work
