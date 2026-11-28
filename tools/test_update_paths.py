#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production Windows updater script against disposable files only.

Compiles the complete update_checker.cpp with narrow host-service stubs, calls
BuildUpdateApplyScript, then runs its output with Windows PowerShell. The staged
suffering.exe is this test's harmless marker writer, never the game executable.
No game media, installed application, network, or SDK checkout is needed.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time


REPO = Path(__file__).resolve().parent.parent

HOST_STUB = r"""
#pragma once
#include <filesystem>
#include <stdexcept>
namespace app {
inline std::filesystem::path ExeDir() { throw std::runtime_error("Unexpected application access"); }
inline bool IsReplayOrAutotestActive() { return true; }
}
"""

UI_STUB = r"""
#pragma once
#include <windows.h>
#include <string>
#include <vector>
namespace suffering {
inline std::vector<std::string> GetProcessArgsUtf8() { return {}; }
inline std::wstring Utf8ToWideStr(const std::string& value) {
    int n = MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0);
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), int(value.size()), result.data(), n);
    return result;
}
inline std::string WideToUtf8Str(const std::wstring& value) {
    int n = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), result.data(), n, nullptr, nullptr);
    return result;
}
}
"""

FIXTURE = r"""
#include "common/update_checker.h"
#include <filesystem>
#include <fstream>
#include <iostream>
int wmain(int argc, wchar_t** argv) {
    if (argc == 1) {
        // Only the disposable copy named suffering.exe takes this branch.
        std::ofstream("relaunch-marker.txt", std::ios::binary) << "stub relaunched";
        return 0;
    }
    if (argc == 3 && std::wstring(argv[1]) == L"--command") {
        std::cout << suffering::BuildUpdateApplyCommand(argv[2]);
        return 0;
    }
    if (argc != 5) return 2;
    std::ofstream out(std::filesystem::path(argv[4]), std::ios::binary);
    out << suffering::BuildUpdateApplyScript(argv[1], argv[2], argv[3], 0);
    return out.good() ? 0 : 3;
}
"""


def run_checked(args: list[str | Path], **kwargs: object) -> subprocess.CompletedProcess[str]:
    result = subprocess.run([str(arg) for arg in args], capture_output=True, text=True, **kwargs)
    if result.returncode:
        raise RuntimeError(f"Command failed ({result.returncode}): {args[0]}\n{result.stdout}\n{result.stderr}")
    return result


def compile_fixture(work: Path, compiler: str) -> Path:
    stubs = work / "stubs"
    for name, contents in {
        "app/app.h": HOST_STUB,
        "common/ui_common.h": UI_STUB,
        "core/log.h": "#pragma once\n#define LOGR(...) ((void)0)\n#define LOGW(...) ((void)0)\n#define LOGE(...) ((void)0)\n",
    }.items():
        path = stubs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding="utf-8")
    fixture = work / "fixture.cpp"
    fixture.write_text(FIXTURE, encoding="utf-8")
    executable = work / "update_script_fixture.exe"
    run_checked([
        compiler, "-std=c++20", "-O0", "-fuse-ld=lld", f"-I{stubs}", f"-I{REPO / 'src'}",
        REPO / "src/common/update_checker.cpp", fixture, "-o", executable,
        "-lwinhttp", "-lshell32",
    ], timeout=120)
    return executable


def put(root: Path, relative: str, data: bytes) -> None:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def exercise_paths(work: Path, executable: Path, powershell: Path, suffix: str) -> None:
    target = work / ("install " + suffix)
    stage = target / "stage [!%]"
    staged_root = stage / "payload '[]!"
    staged_root.mkdir(parents=True)
    # All recursive deletion performed by the generated script is confined here.
    assert stage.resolve().is_relative_to(work.resolve())
    preserved = [
        "settings.toml", "hdd/save.dat", "assets/texture.dat", "cache/cache.dat",
        "logs/run.log", "CrashDumps/test.dat", "textures/user.dat", "update_staging/user.dat",
        "test.iso", "test.xiso", "test.xbe", "test.7z", "compiler/work/suffering/keep.dat",
    ]
    for relative in preserved:
        put(target, relative, b"preserve original")
        put(staged_root, relative, b"must not replace user data")
    put(target, "compiler/work/suffering/built.txt", b"stale compilation marker")
    put(staged_root, "compiler/work/suffering/built.txt", b"must not restore compilation marker")
    put(target, "runtime.dat", b"old runtime")
    put(staged_root, "runtime.dat", b"updated runtime")
    put(staged_root, "compiler/tool.dat", b"updated tool")
    shutil.copy2(executable, staged_root / "suffering.exe")
    script = stage / "update_apply.ps1"
    run_checked([executable, target, staged_root, stage, script], timeout=10)
    assert script.read_bytes().isascii(), "Generated Windows script must not depend on a code page"
    env = os.environ.copy()
    env["UPD_TEST"] = "must-not-expand"
    command = run_checked([executable, "--command", script], timeout=10).stdout
    assert command.isascii(), "Helper startup must not depend on a code page"
    run_checked([
        powershell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command,
    ], cwd=target, env=env, timeout=45)
    marker = target / "relaunch-marker.txt"
    deadline = time.monotonic() + 5
    while not marker.exists() and time.monotonic() < deadline:
        time.sleep(0.02)
    assert marker.read_bytes() == b"stub relaunched", "Replacement stub did not relaunch in the install directory"
    assert (target / "runtime.dat").read_bytes() == b"updated runtime"
    assert (target / "compiler/tool.dat").read_bytes() == b"updated tool"
    for relative in preserved:
        assert (target / relative).read_bytes() == b"preserve original", relative
    assert not (target / "compiler/work/suffering/built.txt").exists()
    assert not stage.exists(), "Completed update must remove its staging directory"


def exercise_copy_failure(work: Path, executable: Path, powershell: Path) -> None:
    target = work / "copy_failure"
    stage = target / "stage"
    stage.mkdir(parents=True)
    assert stage.resolve().is_relative_to(work.resolve())
    script = stage / "update_apply.ps1"
    put(target, "runtime.dat", b"original runtime")
    shutil.copy2(executable, target / "suffering.exe")
    run_checked([executable, target, stage / "missing_payload", stage, script], timeout=10)
    command = run_checked([executable, "--command", script], timeout=10).stdout
    result = subprocess.run([
        str(powershell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command,
    ], cwd=target, capture_output=True, timeout=45)
    assert result.returncode != 0, "A failed copy must fail the helper"
    assert script.exists(), "Failed update must retain staging for diagnosis"
    assert (target / "runtime.dat").read_bytes() == b"original runtime"
    assert not (target / "relaunch-marker.txt").exists(), "Failed update must not relaunch"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("clang++") or str(
        Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang++.exe"))
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("This regression exercises Windows PowerShell and robocopy; run it on Windows.")
    powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    run_checked([
        powershell, "-NoProfile", "-NonInteractive", "-Command",
        "if (Get-Process -Name 'suffering*' -ErrorAction SilentlyContinue) { exit 1 }",
    ], timeout=10)
    local = REPO / ".local/update_path_tests"
    local.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=local))
    fixture = compile_fixture(work, args.cxx)
    # Non-ASCII + cmd expansions + PowerShell quote/wildcard/interpolation syntax.
    exercise_paths(work, fixture, powershell, "Jos\u00e9 \u65e5\u672c \U0001f680")
    exercise_paths(work, fixture, powershell, "%UPD_TEST%!UPD_TEST! &()^[]$'`\u2019")
    exercise_copy_failure(work, fixture, powershell)
    print("PASS: production updater preserves Unicode/metacharacter paths, user data, relaunch, cleanup, and copy failures")
    print(f"Disposable fixtures: {work.relative_to(REPO)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
