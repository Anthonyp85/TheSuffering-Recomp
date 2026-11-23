// SPDX-License-Identifier: GPL-2.0-or-later
#include "app/app.h"
#include "common/input_recorder.h"
#include "common/launcher_ui.h"
#include "common/mkb_input.h"
#include "common/settings_ui.h"

#ifdef _WIN32
// Hint dual-GPU laptops (NVIDIA Optimus / AMD PowerXpress) to prefer the discrete GPU by default.
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001u;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}
#endif

// Host application metadata for The Suffering (USA, Original Xbox, Title ID 0x4D570015).
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
    .load = suffering::MkbLoadSettings,
    .save = suffering::MkbSaveSettings,
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
