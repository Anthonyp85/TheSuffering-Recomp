// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstdint>

#include "game/guest_settings.h"

namespace suffering::gameplay_input {

// One scheduling decision per guest frame, shared by every cloth object.
struct ClothAccumulator {
    uint32_t last_frame_id = 0;
    bool has_frame = false;
    double accum = 0.0;
    float step_dt = 0.0f;

    float StepForFrame(uint32_t frame_id, float dt) {
        if (dt >= 0.014f || !(dt > 0.0f && std::isfinite(dt))) {
            has_frame = false;
            accum = 0.0;
            return dt;
        }
        if (!has_frame || frame_id != last_frame_id) {
            has_frame = true;
            last_frame_id = frame_id;
            accum += double(dt);
            constexpr float kStep60 = 1.0f / 60.0f;
            if (accum >= kStep60) {
                // dt < 0.014 and accum < kStep60 on entry, so at most one
                // fixed step is due. Only retain time not passed to the guest.
                step_dt = kStep60;
                accum -= double(step_dt);
            } else {
                step_dt = 0.0f;
            }
        }
        return step_dt;
    }
};

inline bool SettingsValuesEqual(const guest_settings::GuestUserSettings& a,
                                const guest_settings::GuestUserSettings& b) {
    auto feq = [](float x, float y) { return std::fabs(x - y) <= 1.0e-5f; };
    return a.control_scheme == b.control_scheme && a.invert_pitch == b.invert_pitch && a.vibration == b.vibration &&
           feq(a.turn_sensitivity, b.turn_sensitivity) && feq(a.pitch_sensitivity, b.pitch_sensitivity) &&
           a.auto_targeting == b.auto_targeting && a.auto_pitch_correction == b.auto_pitch_correction &&
           a.checkpoints == b.checkpoints && a.help_text == b.help_text &&
           a.screen_offset_x == b.screen_offset_x && a.screen_offset_y == b.screen_offset_y &&
           feq(a.effects_volume, b.effects_volume) && feq(a.dialog_volume, b.dialog_volume) &&
           feq(a.music_volume, b.music_volume) && a.stereo == b.stereo && a.surround == b.surround &&
           a.subtitles == b.subtitles;
}

// Return whether the caller must persist now. The caller holds the settings lock.
inline bool CommitMenuSettings(const guest_settings::GuestUserSettings& snap,
                               guest_settings::GuestUserSettings& current,
                               guest_settings::GuestUserSettings& last_saved, bool dragging, bool& pending) {
    // Always mirror the latest guest value, including a drag back to the saved
    // baseline. Equality with disk decides persistence, not snapshot freshness.
    current = snap;
    const bool values_changed = !SettingsValuesEqual(snap, last_saved);
    pending = dragging && values_changed;
    if (!values_changed || dragging) return false;
    last_saved = snap;
    return true;
}

inline bool FlushMenuSettings(guest_settings::GuestUserSettings& current,
                              guest_settings::GuestUserSettings& last_saved) {
    if (SettingsValuesEqual(current, last_saved)) return false;
    current.has_saved_values = true;
    last_saved = current;
    return true;
}

}  // namespace suffering::gameplay_input
