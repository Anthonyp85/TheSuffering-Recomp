#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run the asset-free regressions for the eight repository review fixes.

Requires a C++20 compiler and the initialized/patched XboxXBERecomp SDK.
Windows additionally executes the updater against synthetic application folders.
These checks do not replace live game replay or Vulkan validation-layer testing.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CXX"))
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("clang++") or shutil.which("g++")
    if not compiler and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang++.exe"
        if candidate.is_file():
            compiler = str(candidate)
    if not compiler:
        parser.error("C++20 compiler not found; pass --compiler or set CXX")
    if not (ROOT / "XboxXBERecomp/runtime/core/input.h").is_file():
        parser.error("initialize the SDK with setup_sdk.bat or bash setup_sdk.sh first")

    def run(command: list[str]) -> None:
        subprocess.run(command, cwd=ROOT, check=True)

    print("Checking cloth timing, slider persistence, and reserved keys...", flush=True)
    with tempfile.TemporaryDirectory(prefix="suffering-review-tests-") as temp:
        exe = Path(temp) / ("gameplay_input.exe" if os.name == "nt" else "gameplay_input")
        run([compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror", "-Isrc",
             "tests/gameplay_input_regression.cpp", "-o", str(exe)])
        run([str(exe)])

    print("Checking controller-only recording and legacy replay...", flush=True)
    run([sys.executable, "tools/test_input_recording.py", "--compiler", compiler])
    print("Checking renderer lifecycle, Vulkan dependencies, and CLI persistence...", flush=True)
    run([sys.executable, "tools/test_renderer_sdk_regressions.py", "--compiler", compiler])
    if os.name == "nt":
        print("Checking Windows update scripts with synthetic application folders...", flush=True)
        run([sys.executable, "tools/test_update_paths.py", "--cxx", compiler])
    else:
        print("SKIP: Windows updater execution requires Windows.", flush=True)
    print("All applicable review-fix regressions passed.", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
