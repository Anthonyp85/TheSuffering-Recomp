// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace input {
struct Pad;
}

namespace suffering {

// Host-boundary SDL3 controller audit, opt-in input recording (--record-input / XBE_INPUT_RECORD),
// deterministic replay (--replay-input / XBE_INPUT_REPLAY, --replay-mode=hybrid|frame|time, --replay-exit),
// state checkpoint logging (--state-log / XBE_STATE_LOG), and scripted autotest (SUFFERING_AUTOTEST).
// All hooks are zero-overhead no-ops during normal gameplay when no flags/env vars are set.
void OnInit();
void OnWindow(void* hwnd);
bool OnMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void OnPad(int port, input::Pad& pad);
void OnRumble(int port, uint16_t& left, uint16_t& right);

// TSREC 1 serializes Pad only. Successful recording sessions disable native M&K
// input and guest preference overrides without changing saved controls settings.
bool IsInputRecordingActive();
// Also true during legacy replay/autotest; use at native-input hook boundaries.
bool IsPadOnlyInputSession();

}  // namespace suffering
