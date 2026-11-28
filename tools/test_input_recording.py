#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the production recorder state, parser, and OnPad against host-boundary doubles.

No game assets, window, SDL device, or SDK libraries are needed. The SDK input::Pad
header is used directly. Source sections are compiled, not checked with regexes:
assertions exercise their behavior with physical, native, and replay input.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parent.parent


def section(source: str, start: str, end: str) -> str:
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=os.environ.get("CXX"))
    parser.add_argument("--check-regression", action="store_true", help="also verify the pre-fix mode selection fails")
    args = parser.parse_args()
    compiler = args.compiler or shutil.which("clang++") or shutil.which("g++")
    if not compiler and os.name == "nt":
        candidate = Path(os.environ.get("ProgramFiles", "C:/Program Files")) / "LLVM/bin/clang++.exe"
        if candidate.is_file():
            compiler = str(candidate)
    if not compiler:
        parser.error("C++ compiler missing; set CXX or pass --compiler")

    source = (ROOT / "src/common/input_recorder.cpp").read_text(encoding="utf-8")
    prefix = r'''
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include "core/input.h"
#include "common/mkb_input.h"
#include "common/input_recorder.h"
#include "game/cheats.h"
#define LOGE(...) ((void)0)
#define LOGR(...) ((void)0)
namespace app {
bool virtual_session = false;
bool IsReplayOrAutotestActive() { return virtual_session; }
void* Window() { return nullptr; }
}
namespace kernel {
bool guest_thread = true;
void* Current() { return guest_thread ? reinterpret_cast<void*>(1) : nullptr; }
}
#ifdef _WIN32
using HWND = void*;
constexpr int WM_CLOSE = 16;
void PostMessageW(HWND, int, int, int) {}
#else
struct SDL_Event { int type; };
constexpr int SDL_EVENT_QUIT = 1;
void SDL_PushEvent(SDL_Event*) {}
#endif
namespace suffering {
'''
    globals_code = section(source, "std::atomic<bool> g_recording_active", "// Verified read-only guest")
    state = section(source, "constexpr uint64_t kMaxEffStepUs", "#ifdef _WIN32\nstd::string WideToUtf8")
    pad_equal = section(source, "bool PadsEqual(", "bool SafeReadU32(")
    parser_code = section(source, "void ApplyToken(", "void WriteStateCheckpoint(")
    queries = section(source, "bool IsInputRecordingActive()", "void OnInit()")
    lifecycle = section(source, "void OpenRecFile(", "perf::PhaseInfo QueryGuestPerfPhase()")
    on_pad = section(source, "void OnPad(", "void OnRumble(")
    cheat_source = (ROOT / "src/game/cheats.cpp").read_text(encoding="utf-8")
    cheats = r'''
namespace cheats {
std::array<std::atomic<bool>, kCheatToggleCount> g_toggles{};
std::mutex g_state_mutex;
std::vector<CheatAction> g_pending_actions;
'''
    cheats += section(cheat_source, "void SetToggle(", "bool ToggleCheat(")
    cheats += section(cheat_source, "void EnqueueAction(", "void ClearActionFeedback(")
    cheats += "}\n"
    # Compile each actual entry guard with valid guest arguments; the instrumentation
    # after the guard stands for entering the enhancement body. No guest assets needed.
    stair_source = (ROOT / "src/game/stairs_fix.cpp").read_text(encoding="utf-8")
    stair_names = ["sub_00179D60", "sub_001709C0", "sub_00174340", "sub_00174530", "sub_0003CE90", "sub_0005BF60"]
    stairs = r'''
int stair_original_calls = 0, stair_enhancement_calls = 0;
struct TestGuestContext { uint32_t ecx = 1, esp = 1; };
constexpr uint32_t g_active_torque_va = 1, g_active_collider_va = 1, kVtTorquePrimary = 1;
bool ValidGuest(uint32_t, uint32_t) { return true; }
#define X86_MEM32(va) kVtTorquePrimary
#define X86_FUNC(name) void name(TestGuestContext& ctx, uint8_t* base)
'''
    for name in stair_names:
        stairs += f"X86_FUNC(__imp__{name}) {{ ++stair_original_calls; }}\n"
        stairs += section(stair_source, f"X86_FUNC({name}) {{", "\n\n")
        stairs += "\n    ++stair_enhancement_calls;\n}\n"
    stairs += "void RunStairGuards() { TestGuestContext ctx;\n"
    stairs += "\n".join(f"{name}(ctx, nullptr);" for name in stair_names)
    stairs += "\n}\n#undef X86_FUNC\n#undef X86_MEM32\n"
    doubles = r'''
MkbConfig config;
int merge_calls = 0;
MkbConfig& MkbGetConfig() { return config; }
void MkbNotifyGamepadRightStickActive() {}
void UiFilterPad(int, input::Pad&, bool) {}
bool MkbIsSyntheticTestActive() { return false; }
void MkbAdvanceSyntheticEvents() {}
bool UiIsOverlayOpen() { return false; }
void MkbMergeGameplayPad(input::Pad& pad) { ++merge_calls; pad.analog[7] = 255; }
void MkbLogSyntheticTelemetry(uint64_t, const input::Pad&, bool) {}
GuestState ReadGuestState() { return {}; }
void WriteStateCheckpoint(RecorderState&, uint64_t, uint64_t, uint64_t, const GuestState&, const char*) {}
'''
    tests = r'''
} // namespace suffering
int failures = 0;
void Check(bool ok, const char* description) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", description); ++failures; }
}
int main(int argc, char** argv) {
    using namespace suffering;
    Check(argc == 3, "fixture and output paths supplied");
    if (argc != 3) return 2;
    auto& st = State();
    Check(!IsInputRecordingActive() && !IsPadOnlyInputSession(), "ordinary session has no recording constraint or notice");
    st.record_path = argv[2]; // An existing directory cannot be opened as a file.
    OpenRecFile(st);
    Check(!st.rec_file && !IsInputRecordingActive() && !IsPadOnlyInputSession(), "failed recording does not disable native input or show a recording notice");
    const std::string written_rec = (std::filesystem::path(argv[2]) / "roundtrip.rec").string();
    st.record_path = written_rec;
    OpenRecFile(st);
    Check(st.rec_file != nullptr, "temporary recording file opened");
    if (!st.rec_file) return 2;
    Check(IsInputRecordingActive() && IsPadOnlyInputSession(), "record session announces controller-only contract");
    cheats::SetToggle(cheats::CheatToggle::GodMode, true);
    Check(!cheats::IsToggleEffective(cheats::CheatToggle::GodMode), "recording suppresses armed cheats absent from the Pad stream");
    cheats::EnqueueAction(cheats::CheatAction::FullHealAndXombium);
    Check(cheats::g_pending_actions.empty(), "recording refuses unrecorded one-shot cheat actions");
    RunStairGuards();
    Check(stair_original_calls == 6 && stair_enhancement_calls == 0,
          "recording delegates all six stair entry points to original guest behavior");
    for (InputMode mode : {InputMode::Auto, InputMode::MouseKeyboardOnly, InputMode::ControllerOnly}) {
        config.input_mode = mode;
        input::Pad physical{};
        physical.connected = true;
        physical.analog[7] = 99;
        physical.rx = 1234;
        input::Pad observed = physical;
        OnPad(0, observed);
        Check(PadsEqual(observed, physical) && observed.connected, "recording retains physical controller input");
        Check(merge_calls == 0, "recording never accepts unrecorded native gameplay input");
        Check(config.input_mode == mode, "recording leaves saved input-mode preference untouched");
    }
    kernel::guest_thread = false;
    input::Pad disconnected{};
    const uint64_t before_poll = st.poll_id;
    OnPad(0, disconnected);
    Check(!disconnected.connected && st.poll_id == before_poll, "recording host poll does not fabricate an MKB device or advance stream");
    kernel::guest_thread = true;
    FinalizeLocked(st);
    Check(!IsInputRecordingActive() && IsPadOnlyInputSession(), "closing recording keeps native input blocked until shutdown");
    RecorderState recorded;
    Check(LoadRecFile(written_rec, recorded), "new controller-only recording retains TSREC1 parser compatibility");
    Check(recorded.rec_total_polls == 3 && recorded.replay_samples.size() == 1 &&
              recorded.replay_samples.front().pad.analog[7] == 99 && recorded.replay_samples.front().pad.rx == 1234,
          "recorded stream contains exactly the physical pad values");

    // Existing TSREC1 files still load with unchanged Pad fields and frame-mode timing.
    Check(LoadRecFile(argv[1], st), "existing TSREC1 golden fixture loads");
    Check(!st.replay_samples.empty(), "existing recording contains controller samples");
    if (!st.replay_samples.empty()) {
        st.replay_enabled = true;
        st.replay_mode = ReplayMode::Frame;
        app::virtual_session = true;
        g_pad_only_recording_session = false;
        Check(IsPadOnlyInputSession(), "legacy replay also blocks native actions");
        RunStairGuards();
        Check(stair_original_calls == 12 && stair_enhancement_calls == 0, "legacy replay retains original stair behavior");
        for (size_t i = 0; i < st.replay_samples.size(); ++i) {
            const auto expected = st.replay_samples[i];
            st.anchor_set = true;
            st.poll_id = expected.frame_id;
            st.last_poll_tp = st.t0 = std::chrono::steady_clock::now();
            input::Pad observed{};
            OnPad(0, observed);
            Check(PadsEqual(observed, expected.pad), "legacy replay reproduces controller sample at its original poll");
        }
        st.poll_id = st.rec_total_polls;
        input::Pad released{};
        OnPad(0, released);
        Check(st.replay_finished && PadsEqual(released, input::Pad{}) && released.connected,
              "legacy replay releases controls at stream end");
    }
    st.replay_enabled = false;
    app::virtual_session = false;
    config.input_mode = InputMode::Auto;
    Check(cheats::IsToggleEffective(cheats::CheatToggle::GodMode), "normal gameplay retains armed cheats");
    cheats::EnqueueAction(cheats::CheatAction::FullHealAndXombium);
    Check(cheats::g_pending_actions.size() == 1, "normal gameplay still accepts cheat actions");
    RunStairGuards();
    Check(stair_enhancement_calls == 6, "normal gameplay retains all six stair enhancements");
    input::Pad normal{};
    OnPad(0, normal);
    Check(merge_calls > 0 && normal.connected && normal.analog[7] == 255,
          "ordinary non-recording session still merges native gameplay input");
    std::printf("input recording regression: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
'''
    output = ROOT / ".local/input_recording_test"
    output.mkdir(parents=True, exist_ok=True)

    def run(body: str, stem: str) -> subprocess.CompletedProcess:
        cpp = output / f"{stem}.cpp"
        exe = output / (f"{stem}.exe" if os.name == "nt" else stem)
        cpp.write_text(prefix + globals_code + state + pad_equal + parser_code + lifecycle + queries + doubles + body + cheats + stairs + tests,
                       encoding="utf-8")
        subprocess.run([compiler, "-std=c++17", "-D_CRT_SECURE_NO_WARNINGS", "-Isrc", "-IXboxXBERecomp/runtime", str(cpp), "-o", str(exe)],
                       cwd=ROOT, check=True)
        return subprocess.run([str(exe), str(ROOT / "tests/m3_full_regression.rec"), str(output)], cwd=ROOT)

    if args.check_regression:
        old_selection = on_pad.replace(
            "IsPadOnlyInputSession() ? InputMode::ControllerOnly : MkbGetConfig().input_mode",
            "MkbGetConfig().input_mode",
        )
        if old_selection == on_pad:
            raise AssertionError("could not restore the reviewed mode-selection bug")
        if run(old_selection, "input_recording_before").returncode == 0:
            raise AssertionError("regression test failed to detect pre-fix input loss")
    run(on_pad, "input_recording_after").check_returncode()


if __name__ == "__main__":
    main()
