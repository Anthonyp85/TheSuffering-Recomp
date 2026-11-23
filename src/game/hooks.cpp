// SPDX-License-Identifier: GPL-2.0-or-later
// Game-specific hooks for The Suffering (USA) default.xbe (Title ID 0x4D570015).
// Implements native-PC Mouse & Keyboard look injection (post-deadzone, framerate-independent,
// zero-coasting yaw slew bypass, decoupled PC pitch polarity, and Scheme 1 dedicated R3 flashlight).

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "app/app.h"
#include "common/mkb_input.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

X86_EXTERN_FUNC(__imp__sub_0014E1F0);
X86_EXTERN_FUNC(__imp__sub_001B09A0);
X86_EXTERN_FUNC(__imp__sub_000576A0);
X86_EXTERN_FUNC(__imp__sub_000505D0);
X86_EXTERN_FUNC(__imp__sub_000465F0);
X86_EXTERN_FUNC(__imp__sub_00265C7B);

// 1. InputMgr::SetControlTable — upgrade Default Scheme 0 (0x002BE260, R3 unbound) to Scheme 1
// (0x002BE8E0, 100% identical to Scheme 0 plus R3 -> action 1054 Flashlight Toggle) so the F key
// toggles the flashlight natively without touching D-Pad or affecting existing controller bindings.
X86_FUNC(sub_0014E1F0) {
    if (!app::IsReplayOrAutotestActive() &&
        suffering::MkbGetConfig().input_mode != suffering::InputMode::ControllerOnly) {
        if (X86_MEM32(ctx.esp + 0x04u) == 0x002BE260u) {
            X86_MEM32(ctx.esp + 0x04u) = 0x002BE8E0u;
            suffering::MkbNotifyScheme1HookApplied();
        }
    }
    __imp__sub_0014E1F0(ctx, base);
}

// 2. CXboxInputDevice::Poll (__thiscall) — post-deadzone framerate-independent linear mouse injection.
X86_FUNC(sub_001B09A0) {
    if (app::IsReplayOrAutotestActive() ||
        suffering::MkbGetConfig().input_mode == suffering::InputMode::ControllerOnly) {
        __imp__sub_001B09A0(ctx, base);
        return;
    }

    const uint32_t dev = ctx.ecx;
    __imp__sub_001B09A0(ctx, base);

    if (ctx.eax != 0u && dev >= 0x10000u && X86_MEM32(dev + 0x0Cu) == 0u && suffering::MkbIsInActive3DGameplay()) {
        uint32_t dt_ptr = X86_MEM32(0x002D0D94u);
        float dt = (dt_ptr >= 0x10000u && mem::IsCommitted(dt_ptr)) ? X86_MEMF32(dt_ptr) : X86_MEMF32(0x002E01D0u);
        float sens_x = X86_MEMF32(0x002DB23Cu);
        float sens_y = X86_MEMF32(0x002DB240u);

        float look_x = 0.0f;
        float look_y = 0.0f;
        if (suffering::MkbConsumeMouseLook(dt, sens_x, sens_y, look_x, look_y)) {
            X86_MEMF32(dev + 0x58u) = (look_x < 0.0f) ? -look_x : 0.0f;  // ch 8: RS_LEFT  (0x404)
            X86_MEMF32(dev + 0x5Cu) = (look_x > 0.0f) ? look_x : 0.0f;   // ch 9: RS_RIGHT (0x405)
            X86_MEMF32(dev + 0x50u) = (look_y > 0.0f) ? look_y : 0.0f;   // ch 6: RS_UP    (0x415)
            X86_MEMF32(dev + 0x54u) = (look_y < 0.0f) ? -look_y : 0.0f;  // ch 7: RS_DOWN  (0x416)
            X86_MEMF32(dev + 0x9Cu) = std::max(std::fabs(look_x), std::fabs(look_y));  // ch 25: RS_ANY
        }
    }
}

// 3. Player Yaw Behavior (shared across 3rd-Person, 1st-Person, and Creature modes):
// Bypass yaw acceleration slew so mouse movement snaps in 1 frame with zero stop-coasting drift,
// and optionally bypass controller sticky slowdown ([ent + 0x664]) when mouse_raw_aim is enabled.
X86_FUNC(sub_000576A0) {
    if (app::IsReplayOrAutotestActive() || !suffering::MkbIsMouseLookActive()) {
        __imp__sub_000576A0(ctx, base);
        return;
    }

    const uint32_t ent = ctx.ecx;
    X86_MEMF32(ent + 0x182u) = 1.0e9f;
    const bool raw_aim = suffering::MkbGetConfig().mouse_raw_aim;
    uint16_t saved_target = 0;
    if (raw_aim) {
        saved_target = X86_MEM16(ent + 0x664u);
        X86_MEM16(ent + 0x664u) = 0u;
    }
    __imp__sub_000576A0(ctx, base);
    if (raw_aim) {
        X86_MEM16(ent + 0x664u) = saved_target;
    }
}

// 4. Player Pitch Behavior (shared across 3rd-Person, 1st-Person, and Creature modes):
// Decouple PC mouse Y polarity from Xbox controller Invert Pitch ([0x002DB248]) and optionally
// bypass controller sticky slowdown ([ent + 0x664]).
X86_FUNC(sub_000505D0) {
    if (app::IsReplayOrAutotestActive() || !suffering::MkbIsMouseLookActive()) {
        __imp__sub_000505D0(ctx, base);
        return;
    }

    const uint32_t ent = ctx.ecx;
    const uint32_t saved_inv = X86_MEM32(0x002DB248u);
    X86_MEM32(0x002DB248u) = 0u;  // Canonical non-inverted PC sign: RS_UP (+0x50) pitches camera UP
    const bool raw_aim = suffering::MkbGetConfig().mouse_raw_aim;
    uint16_t saved_target = 0;
    if (raw_aim) {
        saved_target = X86_MEM16(ent + 0x664u);
        X86_MEM16(ent + 0x664u) = 0u;
    }
    __imp__sub_000505D0(ctx, base);
    if (raw_aim) {
        X86_MEM16(ent + 0x664u) = saved_target;
    }
    X86_MEM32(0x002DB248u) = saved_inv;
}

// 5. Mounted Turret / Searchlight Aim:
// Apply identical yaw slew bypass and decoupled PC pitch polarity during mouse look.
X86_FUNC(sub_000465F0) {
    if (app::IsReplayOrAutotestActive() || !suffering::MkbIsMouseLookActive()) {
        __imp__sub_000465F0(ctx, base);
        return;
    }

    const uint32_t ent = ctx.ecx;
    const float saved_accel = X86_MEMF32(ent + 0x182u);
    const uint32_t saved_inv = X86_MEM32(0x002DB248u);
    X86_MEMF32(ent + 0x182u) = 1.0e9f;
    X86_MEM32(0x002DB248u) = 0u;
    __imp__sub_000465F0(ctx, base);
    X86_MEMF32(ent + 0x182u) = saved_accel;
    X86_MEM32(0x002DB248u) = saved_inv;
}

// 6. XMVDecoder_GetNextFrame (__stdcall, ret 0x10):
// RiotXmvPlayer polls GetNextFrame in a zero-sleep busy loop while *pResult == XMV_NOFRAME (0),
// pegging the pinned guest core at 100% CPU and starving the SRSC streaming worker thread.
// Sleep on the 60 Hz VBlank condition variable whenever XMV_NOFRAME is returned.
X86_FUNC(sub_00265C7B) {
    const uint32_t p_result = X86_MEM32(ctx.esp + 0x0Cu);
    const uint32_t vb_before = d3d::VBlankCount();
    __imp__sub_00265C7B(ctx, base);
    if (ctx.eax == 0u && p_result >= 0x10000u && mem::IsCommitted(p_result) && mem::IsCommitted(p_result + 3u) &&
        X86_MEM32(p_result) == 0u) {
        d3d::WaitNextVBlank(vb_before, 2000u);
    }
}
