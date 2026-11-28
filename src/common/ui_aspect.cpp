// SPDX-License-Identifier: GPL-2.0-or-later
#include "common/ui_aspect.h"

#include <atomic>
#include <cctype>
#include <string>

#include <toml++/toml.hpp>

namespace suffering::ui_aspect {

namespace {

std::atomic<uint32_t> g_saved_mode{static_cast<uint32_t>(Mode::AspectCorrect)};
std::atomic<uint32_t> g_cli_mode{static_cast<uint32_t>(Mode::Count)};  // Count == unset
std::atomic<bool> g_menu_pillarboxed_43{false};

Mode ClampMode(uint32_t raw) {
    if (raw >= static_cast<uint32_t>(Mode::Count)) return Mode::AspectCorrect;
    return static_cast<Mode>(raw);
}

std::string ToLowerAscii(const char* s) {
    std::string out;
    if (!s) return out;
    while (*s) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*s++))));
    }
    return out;
}

}  // namespace

Mode GetSavedMode() {
    return ClampMode(g_saved_mode.load(std::memory_order_acquire));
}

Mode GetMode() {
    uint32_t cli = g_cli_mode.load(std::memory_order_acquire);
    if (cli < static_cast<uint32_t>(Mode::Count)) {
        return static_cast<Mode>(cli);
    }
    return GetSavedMode();
}

void SetMode(Mode mode) {
    Mode clamped = ClampMode(static_cast<uint32_t>(mode));
    g_cli_mode.store(static_cast<uint32_t>(Mode::Count), std::memory_order_release);
    g_saved_mode.store(static_cast<uint32_t>(clamped), std::memory_order_release);
}

void SetTransientCliMode(Mode mode) {
    Mode clamped = ClampMode(static_cast<uint32_t>(mode));
    g_cli_mode.store(static_cast<uint32_t>(clamped), std::memory_order_release);
}

void ClearTransientCliMode() {
    g_cli_mode.store(static_cast<uint32_t>(Mode::Count), std::memory_order_release);
}

bool HasTransientCliMode() {
    return g_cli_mode.load(std::memory_order_acquire) < static_cast<uint32_t>(Mode::Count);
}

const char* ModeToken(Mode mode) {
    switch (ClampMode(static_cast<uint32_t>(mode))) {
        case Mode::AspectCorrect:
            return "aspect_correct";
        case Mode::MenusAndCrosshair:
            return "menus_and_crosshair";
        case Mode::Stretched16x9:
            return "stretched_16x9";
        default:
            return "aspect_correct";
    }
}

bool ParseModeToken(const char* token, Mode& out) {
    std::string s = ToLowerAscii(token);
    if (s == "aspect_correct" || s == "aspect" || s == "4:3" || s == "43" || s == "0") {
        out = Mode::AspectCorrect;
        return true;
    }
    if (s == "menus_and_crosshair" || s == "menus_crosshair" || s == "hud16x9" || s == "edge_hud" || s == "1") {
        out = Mode::MenusAndCrosshair;
        return true;
    }
    if (s == "stretched_16x9" || s == "stretched" || s == "stretch" || s == "16:9" || s == "original" || s == "2") {
        out = Mode::Stretched16x9;
        return true;
    }
    return false;
}

const char* ModeLabel(Mode mode) {
    switch (ClampMode(static_cast<uint32_t>(mode))) {
        case Mode::AspectCorrect:
            return "Aspect-Correct 4:3 UI + 1:1 Crosshair (Recommended)";
        case Mode::MenusAndCrosshair:
            return "4:3 Menus + 1:1 Crosshair (16:9 Edge HUD)";
        case Mode::Stretched16x9:
            return "Original Xbox 16:9 Stretch";
        default:
            return "Aspect-Correct 4:3 UI + 1:1 Crosshair (Recommended)";
    }
}

const char* ModeShortLabel(Mode mode) {
    switch (ClampMode(static_cast<uint32_t>(mode))) {
        case Mode::AspectCorrect:
            return "4:3 UI + 1:1 Crosshair";
        case Mode::MenusAndCrosshair:
            return "4:3 Menus + 1:1 Crosshair";
        case Mode::Stretched16x9:
            return "16:9 Stretch";
        default:
            return "4:3 UI + 1:1 Crosshair";
    }
}

const char* ModeDescription(Mode mode) {
    switch (ClampMode(static_cast<uint32_t>(mode))) {
        case Mode::AspectCorrect:
            return "Menus, FMVs, HUD, and 3D HUD items preserve 4:3 proportions on widescreen; crosshair is 1:1 "
                   "unstretched and aligned with 16:9 aim.";
        case Mode::MenusAndCrosshair:
            return "Menus and FMVs are centered at 4:3 and crosshair is 1:1 unstretched, while gameplay HUD stays "
                   "anchored to the 16:9 edges.";
        case Mode::Stretched16x9:
            return "Original Xbox widescreen behavior: 2D menus, FMVs, HUD, and crosshair stretch horizontally "
                   "across 16:9.";
        default:
            return "";
    }
}

void LoadSettings(const void* toml_table_ptr) {
    Mode loaded = Mode::AspectCorrect;
    if (toml_table_ptr) {
        const auto& root = *static_cast<const toml::table*>(toml_table_ptr);
        if (const auto* tbl = root["ui_aspect"].as_table()) {
            if (auto sv = (*tbl)["mode"].value<std::string>()) {
                Mode parsed = Mode::AspectCorrect;
                if (ParseModeToken(sv->c_str(), parsed)) {
                    loaded = parsed;
                }
            } else if (auto iv = (*tbl)["mode"].value<int64_t>()) {
                if (*iv >= 0 && *iv < static_cast<int64_t>(Mode::Count)) {
                    loaded = static_cast<Mode>(*iv);
                }
            }
        }
    }
    g_saved_mode.store(static_cast<uint32_t>(loaded), std::memory_order_release);
}

void SaveSettings(void* toml_table_ptr) {
    if (!toml_table_ptr) return;
    auto& root = *static_cast<toml::table*>(toml_table_ptr);
    const Mode saved = GetSavedMode();
    if (saved == Mode::AspectCorrect) {
        root.erase("ui_aspect");
        return;
    }
    toml::table tbl;
    tbl.insert("mode", std::string(ModeToken(saved)));
    root.insert_or_assign("ui_aspect", std::move(tbl));
}

void SetMenuPillarboxed43Active(bool active) {
    g_menu_pillarboxed_43.store(active, std::memory_order_release);
}

bool IsCurrentMenuPillarboxed43() {
    return g_menu_pillarboxed_43.load(std::memory_order_acquire);
}

}  // namespace suffering::ui_aspect
