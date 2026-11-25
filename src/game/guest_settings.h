// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

namespace suffering::guest_settings {

// Complete host-persisted mirror of The Suffering (USA, Title ID 0x4D570015) in-game user preferences
// stored in CUserSettings at guest VA 0x002DB218 (size 0x54; user preference fields span +0x00..+0x40,
// while progression bitfields +0x44/+0x48/+0x50 are intentionally left under guest save-file ownership).
//
// Polarity & factory-default notes (verified from sub_00016CA0, sub_000505D0, CControllerGui & CGameplayGui):
//  - invert_pitch (+0x30, 0x002DB248):
//      true  (1) = "Invert Pitch Axis: Yes" (pushing Right Stick UP pitches camera DOWN — Xbox factory default)
//      false (0) = "Invert Pitch Axis: No"  (pushing Right Stick UP pitches camera UP   — modern standard)
//  - stereo (+0x18) / surround (+0x1C) ("Speaker Setup"):
//      Mono = (false, false), Stereo = (true, false), Surround = (true, true) [Xbox factory default]
//  - checkpoints (+0x40, 0x002DB258):
//      0 = Off, 1 = On, 2 = Prompt [Xbox factory default]
//
// All persistence and live guest overrides are automatically bypassed when app::IsReplayOrAutotestActive() is true.
struct GuestUserSettings {
    // True once populated from [game_options] in settings.toml, modified via Launcher / F1 UI,
    // or committed by the player in an in-game Options screen.
    bool has_saved_values = false;

    // Controller & Aiming (CControllerGui 0x0029AA38 & CGameplayGui 0x0029AE70)
    uint32_t control_scheme = 0u;        // +0x00 (0x002DB218): 0..4 controller preset (default 0)
    bool invert_pitch = true;            // +0x30 (0x002DB248): true = Yes/Inverted (default 1), false = No/Standard (0)
    bool vibration = true;               // +0x34 (0x002DB24C): true = On (default 1), false = Off (0)
    float turn_sensitivity = 1.0f;       // +0x24 (0x002DB23C): [0.25..4.0], default 1.0
    float pitch_sensitivity = 1.0f;      // +0x28 (0x002DB240): [0.25..4.0], default 1.0
    bool auto_targeting = true;          // +0x2C (0x002DB244): true = On (default 1), false = Off (0)
    bool auto_pitch_correction = true;   // +0x3C (0x002DB254): true = On (default 1), false = Off (0)

    // Gameplay & Display (CGameplayGui 0x0029AE70 & CScreenAdjustGui 0x0029B768)
    uint32_t checkpoints = 2u;           // +0x40 (0x002DB258): 0 = Off, 1 = On, 2 = Prompt (default 2)
    bool help_text = true;               // +0x38 (0x002DB250): true = On (default 1), false = Off (0)
    int32_t screen_offset_x = 0;         // +0x04 (0x002DB21C): Screen Adjust X offset in pixels (default 0)
    int32_t screen_offset_y = 0;         // +0x08 (0x002DB220): Screen Adjust Y offset in pixels (default 0)

    // Audio & Subtitles (CAudioGui 0x0029A958)
    float effects_volume = 1.0f;         // +0x0C (0x002DB224): [0.0..1.0], default 1.0
    float dialog_volume = 1.0f;          // +0x10 (0x002DB228): [0.0..1.0], default 1.0
    float music_volume = 1.0f;           // +0x14 (0x002DB22C): [0.0..1.0], default 1.0
    bool stereo = true;                  // +0x18 (0x002DB230): Mono=(false,false), Stereo=(true,false), Surround=(true,true)
    bool surround = true;                // +0x1C (0x002DB234): default (true,true) = Surround
    bool subtitles = false;              // +0x20 (0x002DB238): false = Off (default 0), true = On (1)
};

// Returns factory-default settings (matching sub_00016CA0, with has_saved_values = false).
GuestUserSettings MakeDefaultSettings();

// Clamps all numeric fields to safe valid guest ranges (e.g. control_scheme in 0..4, checkpoints in 0..2,
// sensitivities in [0.25, 4.0], volumes in [0.0, 1.0], screen offsets in [-64, 64]).
GuestUserSettings SanitizeSettings(const GuestUserSettings& s);

// Thread-safe snapshot of current settings (for TOML serialization in MkbSaveSettings and UI display).
GuestUserSettings GetSettings();

// Called during host config load (MkbLoadSettings) to install settings parsed from [game_options] in settings.toml.
// If s.has_saved_values is true, queues an immediate sync into guest 0x002DB218 on boot (or next guest frame).
void SetSettingsFromConfig(const GuestUserSettings& s);

// Resets all user options to Xbox factory defaults; when push_to_guest is true, marks has_saved_values = true
// and queues immediate application into guest 0x002DB218 on the next guest frame.
void ResetToDefaults(bool push_to_guest = true);

// Returns true (and clears the flag) if an in-game Options edit occurred while the F1 ImGui overlay was open
// and deferred its app::Save() call to the host UI thread. (When F1 is closed, in-game commits call app::Save()
// directly on the guest main thread.)
bool ConsumePendingHostSaveRequest();

// Live thread-safe getters & setters for Launcher and F1 Controller / Audio UI.
// Every setter marks has_saved_values = true and schedules an immediate guest RAM + CUserSettings::Apply(1)
// sync on the very next guest frame (caller on the host UI thread should call app::Save() to persist to disk).
bool GetControllerInvertY();
void SetControllerInvertY(bool inverted);

bool GetControllerVibration();
void SetControllerVibration(bool enabled);

uint32_t GetControllerScheme();
void SetControllerScheme(uint32_t scheme);

float GetControllerTurnSensitivity();
void SetControllerTurnSensitivity(float sens);

float GetControllerPitchSensitivity();
void SetControllerPitchSensitivity(float sens);

bool GetSubtitles();
void SetSubtitles(bool enabled);

}  // namespace suffering::guest_settings
