// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "app/app.h"

namespace input {
struct Pad;
}

namespace suffering {

// Persistent game-specific settings bound into settings.toml via app::kGame.options.
std::vector<app::Option> UiOptions();

void UiInit();
void UiOnWindow(void* hwnd);
bool UiOnMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);
void OnUi(double fps);
void UiFilterPad(int port, input::Pad& pad, bool scripted);
void UiFilterRumble(int port, uint16_t& left, uint16_t& right);
bool UiIsOverlayOpen();
void UiSetOverlayOpen(bool open);
void UiRequestRefreshCursor();

}  // namespace suffering
