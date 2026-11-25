// SPDX-License-Identifier: GPL-2.0-or-later
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "common/input_recorder.h"
#include "common/launcher_ui.h"
#include "common/mkb_input.h"
#include "common/settings_ui.h"
#include "common/ui_common.h"
#include "common/version.h"
#include "game/guest_settings.h"

static_assert(SUFFERING_VERSION_MAJOR == 1 && SUFFERING_VERSION_MINOR == 6 && SUFFERING_VERSION_PATCH == 1,
              "Keep game_info.cpp and suffering.rc synchronized with version.h (v1.6.1)");

namespace {

double Round3(float v) {
    if (!std::isfinite(v)) return 1.0;
    return std::round(static_cast<double>(v) * 1000.0) / 1000.0;
}

void LoadAllGameSettings(const void* table_ptr) {
    suffering::MkbLoadSettings(table_ptr);
    suffering::SharedUiOptions& ui = suffering::GetSharedUiOptions();
    ui.dismissed_update_tag.clear();

    suffering::guest_settings::GuestUserSettings gs = suffering::guest_settings::MakeDefaultSettings();
    if (!table_ptr) {
        suffering::guest_settings::SetSettingsFromConfig(gs);
        return;
    }

    const auto& root = *static_cast<const toml::table*>(table_ptr);
    if (auto tag = root["dismissed_update_tag"].value<std::string>()) {
        ui.dismissed_update_tag = *tag;
    }

    if (const auto* go = root["game_options"].as_table()) {
        if (auto v = (*go)["control_scheme"].value<int64_t>()) {
            gs.control_scheme = static_cast<uint32_t>(std::clamp<int64_t>(*v, 0, 4));
        }
        if (auto v = (*go)["invert_pitch"].value<bool>()) {
            gs.invert_pitch = *v;
        } else if (auto v2 = (*go)["controller_invert_y"].value<bool>()) {
            gs.invert_pitch = *v2;
        }
        if (auto v = (*go)["vibration"].value<bool>()) gs.vibration = *v;
        if (auto v = (*go)["turn_sensitivity"].value<double>(); v && std::isfinite(*v)) {
            gs.turn_sensitivity = static_cast<float>(*v);
        }
        if (auto v = (*go)["pitch_sensitivity"].value<double>(); v && std::isfinite(*v)) {
            gs.pitch_sensitivity = static_cast<float>(*v);
        }
        if (auto v = (*go)["auto_targeting"].value<bool>()) gs.auto_targeting = *v;
        if (auto v = (*go)["auto_pitch_correction"].value<bool>()) gs.auto_pitch_correction = *v;
        if (auto v = (*go)["checkpoints"].value<int64_t>()) {
            gs.checkpoints = static_cast<uint32_t>(std::clamp<int64_t>(*v, 0, 2));
        }
        if (auto v = (*go)["help_text"].value<bool>()) gs.help_text = *v;
        if (auto v = (*go)["screen_offset_x"].value<int64_t>()) {
            gs.screen_offset_x = static_cast<int32_t>(std::clamp<int64_t>(*v, -64, 64));
        }
        if (auto v = (*go)["screen_offset_y"].value<int64_t>()) {
            gs.screen_offset_y = static_cast<int32_t>(std::clamp<int64_t>(*v, -64, 64));
        }
        if (auto v = (*go)["effects_volume"].value<double>(); v && std::isfinite(*v)) {
            gs.effects_volume = static_cast<float>(*v);
        }
        if (auto v = (*go)["dialog_volume"].value<double>(); v && std::isfinite(*v)) {
            gs.dialog_volume = static_cast<float>(*v);
        }
        if (auto v = (*go)["music_volume"].value<double>(); v && std::isfinite(*v)) {
            gs.music_volume = static_cast<float>(*v);
        }
        if (auto v = (*go)["stereo"].value<bool>()) gs.stereo = *v;
        if (auto v = (*go)["surround"].value<bool>()) gs.surround = *v;
        if (auto v = (*go)["subtitles"].value<bool>()) gs.subtitles = *v;

        gs = suffering::guest_settings::SanitizeSettings(gs);
        gs.has_saved_values = true;
    }

    suffering::guest_settings::SetSettingsFromConfig(gs);
}

void SaveAllGameSettings(void* table_ptr) {
    if (!table_ptr) return;
    suffering::MkbSaveSettings(table_ptr);

    auto& root = *static_cast<toml::table*>(table_ptr);
    root.insert_or_assign("dismissed_update_tag", suffering::GetSharedUiOptions().dismissed_update_tag);

    const suffering::guest_settings::GuestUserSettings gs =
        suffering::guest_settings::SanitizeSettings(suffering::guest_settings::GetSettings());
    if (gs.has_saved_values) {
        toml::table go;
        go.insert("control_scheme", static_cast<int64_t>(gs.control_scheme));
        go.insert("invert_pitch", gs.invert_pitch);
        go.insert("vibration", gs.vibration);
        go.insert("turn_sensitivity", Round3(gs.turn_sensitivity));
        go.insert("pitch_sensitivity", Round3(gs.pitch_sensitivity));
        go.insert("auto_targeting", gs.auto_targeting);
        go.insert("auto_pitch_correction", gs.auto_pitch_correction);
        go.insert("checkpoints", static_cast<int64_t>(gs.checkpoints));
        go.insert("help_text", gs.help_text);
        go.insert("screen_offset_x", static_cast<int64_t>(gs.screen_offset_x));
        go.insert("screen_offset_y", static_cast<int64_t>(gs.screen_offset_y));
        go.insert("effects_volume", Round3(gs.effects_volume));
        go.insert("dialog_volume", Round3(gs.dialog_volume));
        go.insert("music_volume", Round3(gs.music_volume));
        go.insert("stereo", gs.stereo);
        go.insert("surround", gs.surround);
        go.insert("subtitles", gs.subtitles);
        root.insert_or_assign("game_options", std::move(go));
    }
}

}  // namespace

#ifdef _WIN32
// Hint dual-GPU laptops (NVIDIA Optimus / AMD PowerXpress) to prefer the discrete GPU by default.
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001u;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// Host application metadata for The Suffering (USA, Original Xbox, Title ID 0x4D570015, Recomp v1.6.1).
const app::GameInfo app::kGame = {
    .title = L"The Suffering",
    .short_title = L"The Suffering",
    .name = L"TheSuffering",
    .log = "suffering",
    .settings = "settings.toml",
    .splash = L"splash",
    .missing = L"Provide The Suffering (USA) Xbox disc image (.iso / .xiso.iso) via the file picker, --iso <path>, "
               L"XBE_ISO_PATH, settings.toml (iso_path), or the assets folder.",
    .images = {{"default.xbe", "suffering"}},
    .options = suffering::UiOptions(),
    .load = LoadAllGameSettings,
    .save = SaveAllGameSettings,
    .init = suffering::OnInit,
    .loaded = nullptr,
    .window = suffering::OnWindow,
    .message = suffering::OnMessage,
    .ui = suffering::OnUi,
    .pad = suffering::OnPad,
    .rumble = suffering::OnRumble,
    // Support both extract-xiso (0xEB, kImageSha1) and unpatched Redump (0x7D) USA Retail XBEs
    .releases = {{"default.xbe", "55c902f622047484bc6c7ea4b1b1d714da2096fc"}},
    .expected_title_id = 0x4D570015u,
    // Break Riot Engine single-core Xbox AB-BA deadlock (Main: 0x2E1444 -> 0x2E1A90 vs Worker: 0x2E1A90 -> 0x2E1444)
    .cs_aliases = {{0x002E1444u, 0x002E1A90u}},
    // Pin guest threads to one P-core with real guest priorities (prevents multi-core bitfield race at 0x00176712)
    .single_core_guest = true,
    // Pre-Game Graphical Launcher (bypassed automatically in headless/replay/autotest/skip_launcher modes)
    .launcher = suffering::RunLauncher,
};
