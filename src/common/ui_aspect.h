// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace suffering::ui_aspect {

enum class Mode : uint32_t {
    AspectCorrect = 0,      // 4:3 menus/FMVs/HUD/3D-HUD + 1:1 unstretched crosshair
    MenusAndCrosshair = 1,  // 4:3 menus/FMVs + 1:1 unstretched crosshair, 16:9 edge HUD
    Stretched16x9 = 2,      // Original Xbox stretched 16:9 2D UI & crosshair
    Count = 3,
};

// Exact guest 640x480 -> 4:3 pillarbox geometry inside a 16:9 frame:
// (4/3) / (16/9) = 3/4 = 0.75, centered offset = 640 * (1 - 0.75) / 2 = 80.0.
constexpr float kGuestWidth = 640.0f;
constexpr float kGuestHeight = 480.0f;
constexpr float kPillarboxScaleX = 0.75f;
constexpr float kPillarboxOffsetX = 80.0f;
constexpr float kPillarboxRightX = kGuestWidth - kPillarboxOffsetX;  // 560.0f

inline constexpr float MapGuestXToPillarbox43(float x) {
    return kPillarboxOffsetX + x * kPillarboxScaleX;
}

inline constexpr float MapQuadXLocalScale075(float x, float center_x) {
    return center_x + (x - center_x) * kPillarboxScaleX;
}

// Persistent user setting (saved under [ui_aspect] in settings.toml; omitted at default).
Mode GetSavedMode();
// Effective runtime mode (returns transient CLI override if set, otherwise saved mode).
Mode GetMode();
void SetMode(Mode mode);

// Optional transient CLI override (--ui-aspect=...) for testing without mutating settings.toml.
void SetTransientCliMode(Mode mode);
void ClearTransientCliMode();
bool HasTransientCliMode();

const char* ModeToken(Mode mode);
bool ParseModeToken(const char* token, Mode& out);
const char* ModeLabel(Mode mode);
const char* ModeShortLabel(Mode mode);
const char* ModeDescription(Mode mode);

// TOML persistence hooks (called from src/common/game_info.cpp).
void LoadSettings(const void* toml_table_ptr);
void SaveSettings(void* toml_table_ptr);

// Published on the guest thread each UI frame so PC menu mouse hit-testing
// maps physical cursor coordinates into the active 4:3 centered menu sub-box.
void SetMenuPillarboxed43Active(bool active);
bool IsCurrentMenuPillarboxed43();

// True when a non-stretched mode is selected (still gated by active 16:9 output at render time).
inline bool ShouldPillarboxMenus(Mode m) {
    return m == Mode::AspectCorrect || m == Mode::MenusAndCrosshair;
}
inline bool ShouldPillarboxHud(Mode m) {
    return m == Mode::AspectCorrect;
}
inline bool ShouldCorrectCrosshair(Mode m) {
    return m == Mode::AspectCorrect || m == Mode::MenusAndCrosshair;
}

}  // namespace suffering::ui_aspect
