// SPDX-License-Identifier: GPL-2.0-or-later
// Game-specific hooks for The Suffering (USA) default.xbe (Title ID 0x4D570015).
// Implements:
//  - Native-PC Mouse & Keyboard 3D look injection (post-deadzone, framerate-independent,
//    zero-coasting yaw slew bypass, decoupled PC pitch polarity, Scheme 1 dedicated R3 flashlight)
//  - Full native-PC point-and-click, hover, drag, wheel, and right-click-back menu mouse engine
//    inside CUIMgr::Update (sub_0005F9A0) across all Riot Engine UI screens
//  - Sub-microsecond CClock (0x002DC724) timing hooks eliminating 1ms KeTickCount quantization
//    and Unpause dt snowballs
//  - 60 Hz Cloth::Update accumulator (sub_001A6480) for safe native >60 FPS gameplay
//  - XMVDecoder_GetNextFrame VBlank yield (sub_00265C7B)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

#include "app/app.h"
#include "common/mkb_input.h"
#include "common/settings_ui.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

X86_EXTERN_FUNC(__imp__sub_0014E1F0);
X86_EXTERN_FUNC(__imp__sub_001B09A0);
X86_EXTERN_FUNC(__imp__sub_000576A0);
X86_EXTERN_FUNC(__imp__sub_000505D0);
X86_EXTERN_FUNC(__imp__sub_000465F0);
X86_EXTERN_FUNC(__imp__sub_00265C7B);
X86_EXTERN_FUNC(__imp__sub_0005F9A0);
X86_EXTERN_FUNC(__imp__sub_001A3170);
X86_EXTERN_FUNC(__imp__sub_001A31A0);
X86_EXTERN_FUNC(__imp__sub_001A3210);
X86_EXTERN_FUNC(__imp__sub_001A3220);
X86_EXTERN_FUNC(__imp__sub_001A3390);
X86_EXTERN_FUNC(__imp__sub_001A3470);
X86_EXTERN_FUNC(__imp__sub_001A6480);

namespace {

// Verified guest globals (The Suffering USA default.xbe, Title ID 0x4D570015)
constexpr uint32_t kVaUiMgrPtr = 0x002DAD78u;
constexpr uint32_t kVaRendererPtr = 0x002DC75Cu;
constexpr uint32_t kVaUserSettings = 0x002DB218u;
constexpr uint32_t kVaSensMax = 0x002DADA0u;
constexpr uint32_t kVaSensMin = 0x002DADA4u;
constexpr uint32_t kVaMainClock = 0x002DC724u;
constexpr uint32_t kVaGlobalDt = 0x002E01D0u;
constexpr uint32_t kVaGlobalInvDt = 0x002E01D4u;
constexpr uint32_t kVaFrameCount = 0x002E0280u;

// Verified strong-symbol guest UI methods
constexpr uint32_t kFnSetFocusedChild = 0x0005D790u;
constexpr uint32_t kFnPlayUiSound = 0x0005DDB0u;
constexpr uint32_t kFnUserSettingsApply = 0x00016B60u;
constexpr uint32_t kFnUserSettingsCopyFrom = 0x00016D00u;
constexpr uint32_t kFnMapEntryLookup = 0x000356E0u;

// Verified UI sound property IDs for CGUI::PlayUISound (0x0005DDB0)
constexpr uint32_t kSndUiHover = 0x2Au;
constexpr uint32_t kSndUiToggle = 0x2Bu;
constexpr uint32_t kSndUiCancel = 0x2Du;

// Verified GUI action IDs (0x002BE158 / 0x002BE260)
constexpr uint32_t kActGuiUp = 0x3ECu;
constexpr uint32_t kActGuiDown = 0x3EDu;
constexpr uint32_t kActGuiLeft = 0x3EEu;
constexpr uint32_t kActGuiRight = 0x3EFu;
constexpr uint32_t kActMapInvUp = 0x3F0u;
constexpr uint32_t kActMapInvDown = 0x3F1u;
constexpr uint32_t kActGuiSelect = 0x3FCu;
constexpr uint32_t kActAnyButton = 0x400u;
constexpr uint32_t kActGuiCancel = 0x401u;

// Verified CGUI widget primitive vtables
constexpr uint32_t kVtSimpleButton = 0x0029BBB0u;
constexpr uint32_t kVtRadioSlider = 0x0029BC18u;
constexpr uint32_t kVtListbox = 0x0029BD60u;
constexpr uint32_t kVtHorizontalBox = 0x0029BC88u;
constexpr uint32_t kVtGuiEffects = 0x0029BDE8u;  // Animated glow overlay: ALWAYS exclude!

// Verified CGUI screen vtables
constexpr uint32_t kVtOpeningMovies = 0x0029B560u;
constexpr uint32_t kVtMainMenu = 0x0029B4F8u;
constexpr uint32_t kVtPause = 0x0029B690u;
constexpr uint32_t kVtOptions = 0x0029B5F8u;
constexpr uint32_t kVtAudio = 0x0029A958u;
constexpr uint32_t kVtGameplay = 0x0029AE70u;
constexpr uint32_t kVtController = 0x0029AA38u;
constexpr uint32_t kVtScreenAdjust = 0x0029B768u;
constexpr uint32_t kVtLoadSave = 0x0029B2F0u;
constexpr uint32_t kVtSaveDevice = 0x0029B280u;
constexpr uint32_t kVtAlertBox = 0x0029A828u;
constexpr uint32_t kVtArchives = 0x0029A898u;
constexpr uint32_t kVtMap = 0x00299BC0u;
constexpr uint32_t kVtExtras = 0x0029ACC8u;
constexpr uint32_t kVtCredits = 0x0029AC18u;
constexpr uint32_t kVtGameFinished = 0x0029AD88u;
constexpr uint32_t kVtLevelSelect = 0x0029B1A0u;

bool IsNonInteractiveScreenVt(uint32_t vt) {
    switch (vt) {
        case kVtGuiEffects:
        case 0x0029ABB0u:  // Copyright / Legal
        case 0x0029AF10u:  // Quick-Weapon HUD
        case 0x0029AFB8u:  // Quick-Item HUD
        case 0x0029B020u:  // Cross HUD
        case 0x0029ADF0u:  // Help Popup Toast
        case 0x0029B210u:  // Progress / Loading Bar
        case 0x0029ED00u:  // Loading Overlay
            return true;
        default:
            return false;
    }
}

bool ValidGuest(uint32_t va, uint32_t bytes = 4u) {
    return va >= 0x10000u && mem::IsCommitted(va) && (bytes <= 1u || mem::IsCommitted(va + bytes - 1u));
}

uint32_t CallThiscall(X86Context& ctx, uint32_t fn_va, uint32_t this_ptr,
                      std::initializer_list<uint32_t> args = {}) {
    if (fn_va < 0x10000u) return 0u;
    uint32_t saved_ecx = ctx.ecx;
    ctx.ecx = this_ptr;
    uint32_t res = kernel::CallGuest(ctx, fn_va, args);
    ctx.ecx = saved_ecx;
    return res;
}

uint32_t CallOnAction(X86Context& ctx, uint8_t* base, uint32_t widget_va, uint32_t action_id) {
    if (!ValidGuest(widget_va, 4u)) return 0u;
    uint32_t vt = X86_MEM32(widget_va);
    if (!ValidGuest(vt + 0x14u, 4u)) return 0u;
    return CallThiscall(ctx, X86_MEM32(vt + 0x14u), widget_va, {action_id});
}

void CallWidgetUpdate(X86Context& ctx, uint8_t* base, uint32_t widget_va) {
    if (!ValidGuest(widget_va, 4u)) return;
    uint32_t vt = X86_MEM32(widget_va);
    if (!ValidGuest(vt + 0x54u, 4u)) return;
    CallThiscall(ctx, X86_MEM32(vt + 0x54u), widget_va, {});
}

void PlayUiSound(X86Context& ctx, uint32_t ui_mgr, uint32_t sound_id) {
    CallThiscall(ctx, kFnPlayUiSound, ui_mgr, {sound_id});
}

struct UiRect {
    float l = 0.0f;
    float t = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    bool valid = false;

    bool Contains(float x, float y) const { return valid && x >= l && x <= r && y >= t && y <= b; }
};

UiRect ReadRawRect(uint8_t* base, uint32_t w_va) {
    UiRect rc{};
    if (!ValidGuest(w_va, 0x48u)) return rc;
    float x = X86_MEMF32(w_va + 0x38u);
    float y = X86_MEMF32(w_va + 0x3Cu);
    float w = X86_MEMF32(w_va + 0x40u);
    float h = X86_MEMF32(w_va + 0x44u);
    if (std::isfinite(x) && std::isfinite(y) && std::isfinite(w) && std::isfinite(h) && w > 0.5f && h > 0.5f) {
        rc.l = x;
        rc.t = y;
        rc.r = x + w;
        rc.b = y + h;
        rc.valid = true;
    }
    return rc;
}

UiRect ReadWidgetRectWithUnion(uint8_t* base, uint32_t w_va) {
    UiRect rc = ReadRawRect(base, w_va);
    if (rc.valid) return rc;
    if (!ValidGuest(w_va, 0x54u)) return rc;
    int32_t cnt = int32_t(X86_MEM32(w_va + 0x50u));
    uint32_t arr = X86_MEM32(w_va + 0x48u);
    if (cnt <= 0 || cnt > 32 || !ValidGuest(arr, uint32_t(cnt) * 4u)) return rc;
    for (int32_t i = 0; i < cnt; ++i) {
        uint32_t c = X86_MEM32(arr + uint32_t(i) * 4u);
        UiRect cr = ReadRawRect(base, c);
        if (!cr.valid) continue;
        if (!rc.valid) {
            rc = cr;
        } else {
            rc.l = std::min(rc.l, cr.l);
            rc.t = std::min(rc.t, cr.t);
            rc.r = std::max(rc.r, cr.r);
            rc.b = std::max(rc.b, cr.b);
        }
    }
    return rc;
}

UiRect ComputeClippedRect(uint8_t* base, uint32_t w_va, uint32_t stop_ancestor = 0u) {
    UiRect rc = ReadWidgetRectWithUnion(base, w_va);
    if (!rc.valid) return rc;
    uint32_t cur = w_va;
    for (int depth = 0; depth < 8; ++depth) {
        if (!ValidGuest(cur + 0x5Cu, 4u)) break;
        uint32_t p = X86_MEM32(cur + 0x5Cu);
        if (!ValidGuest(p, 0x60u) || p == cur) break;
        // Clip against ancestors with valid bounds (critical for CListbox scroll viewport clipping).
        UiRect pr = ReadRawRect(base, p);
        if (pr.valid) {
            rc.l = std::max(rc.l, pr.l);
            rc.t = std::max(rc.t, pr.t);
            rc.r = std::min(rc.r, pr.r);
            rc.b = std::min(rc.b, pr.b);
            if (rc.r <= rc.l || rc.b <= rc.t) {
                rc.valid = false;
                return rc;
            }
        }
        if (p == stop_ancestor) break;
        cur = p;
    }
    return rc;
}

int32_t FindChildIndex(uint8_t* base, uint32_t parent_va, uint32_t child_va) {
    if (!ValidGuest(parent_va, 0x54u)) return -1;
    int32_t cnt = int32_t(X86_MEM32(parent_va + 0x50u));
    uint32_t arr = X86_MEM32(parent_va + 0x48u);
    if (cnt <= 0 || cnt > 256 || !ValidGuest(arr, uint32_t(cnt) * 4u)) return -1;
    for (int32_t i = 0; i < cnt; ++i) {
        if (X86_MEM32(arr + uint32_t(i) * 4u) == child_va) return i;
    }
    return -1;
}

bool FocusChainToWidget(X86Context& ctx, uint8_t* base, uint32_t ui_mgr, uint32_t top_va, uint32_t target_va,
                        bool play_sound) {
    uint32_t parents[8]{};
    int32_t indices[8]{};
    int depth = 0;
    uint32_t cur = target_va;
    while (cur != top_va && depth < 8) {
        if (!ValidGuest(cur + 0x5Cu, 4u)) break;
        uint32_t p = X86_MEM32(cur + 0x5Cu);
        if (!ValidGuest(p, 0x60u) || p == cur) break;
        int32_t idx = FindChildIndex(base, p, cur);
        if (idx < 0) break;
        parents[depth] = p;
        indices[depth] = idx;
        ++depth;
        cur = p;
    }
    if (cur != top_va || depth == 0) return false;

    bool changed = false;
    for (int i = depth - 1; i >= 0; --i) {
        uint32_t p = parents[i];
        int32_t want_idx = indices[i];
        int32_t cur_idx = int32_t(X86_MEM32(p + 0x08u));
        if (cur_idx != want_idx) {
            uint32_t ok = CallThiscall(ctx, kFnSetFocusedChild, p, {uint32_t(want_idx)});
            if (ok != 0u) changed = true;
        }
    }
    if (changed && play_sound) {
        PlayUiSound(ctx, ui_mgr, kSndUiHover);
    }
    return changed;
}

uint32_t DispatchListboxAction(X86Context& ctx, uint8_t* base, uint32_t ui_mgr, uint32_t listbox_va,
                               uint32_t action_id) {
    if (!ValidGuest(ui_mgr + 0x8Cu, 4u) || !ValidGuest(listbox_va, 4u)) return 0u;
    // CListbox::OnAction (0x00079CC0) gates scrolling on [g_pUiMgr + 0x8C] != 0.
    uint32_t old_rep = X86_MEM32(ui_mgr + 0x8Cu);
    if (old_rep == 0u) X86_MEM32(ui_mgr + 0x8Cu) = 1u;
    uint32_t res = CallOnAction(ctx, base, listbox_va, action_id);
    X86_MEM32(ui_mgr + 0x8Cu) = old_rep;
    return res;
}

uint32_t FindFirstListbox(uint8_t* base, uint32_t node_va, int depth = 0) {
    if (depth > 4 || !ValidGuest(node_va, 0x54u)) return 0u;
    if (X86_MEM32(node_va) == kVtListbox) return node_va;
    int32_t cnt = int32_t(X86_MEM32(node_va + 0x50u));
    uint32_t arr = X86_MEM32(node_va + 0x48u);
    if (cnt <= 0 || cnt > 256 || !ValidGuest(arr, uint32_t(cnt) * 4u)) return 0u;
    for (int32_t i = 0; i < cnt; ++i) {
        uint32_t c = X86_MEM32(arr + uint32_t(i) * 4u);
        if (!ValidGuest(c, 0x60u)) continue;
        uint32_t flags = X86_MEM32(c + 0x04u);
        uint32_t state = X86_MEM32(c + 0x24u);
        if ((flags & 0x01u) == 0u || (state != 4u && state != 5u)) continue;
        uint32_t found = FindFirstListbox(base, c, depth + 1);
        if (found != 0u) return found;
    }
    return 0u;
}

enum class HitKind : uint8_t {
    None = 0,
    Button,
    RadioSlider,
    ListboxRow,
    ListboxScrollUp,
    ListboxScrollDown,
};

struct HitResult {
    HitKind kind = HitKind::None;
    uint32_t widget = 0u;
    uint32_t parent = 0u;
    int32_t child_idx = -1;
};

HitResult HitTestWidgetTree(uint8_t* base, uint32_t top_va, uint32_t node_va, float gx, float gy, bool in_listbox,
                            int depth = 0) {
    HitResult none{};
    if (depth > 6 || !ValidGuest(node_va, 0x60u)) return none;

    int32_t cnt = int32_t(X86_MEM32(node_va + 0x50u));
    uint32_t arr = X86_MEM32(node_va + 0x48u);
    if (cnt <= 0 || cnt > 256 || !ValidGuest(arr, uint32_t(cnt) * 4u)) return none;

    for (int32_t i = cnt - 1; i >= 0; --i) {
        uint32_t c = X86_MEM32(arr + uint32_t(i) * 4u);
        if (!ValidGuest(c, 0x60u)) continue;
        uint32_t vt = X86_MEM32(c);
        if (vt == kVtGuiEffects) continue;
        uint32_t flags = X86_MEM32(c + 0x04u);
        uint32_t state = X86_MEM32(c + 0x24u);
        if ((flags & 0x01u) == 0u || (state != 4u && state != 5u)) continue;

        if (vt == kVtListbox) {
            // Check CListbox Up/Down scroll arrow widgets ([+0x64] / [+0x68]) first
            if (ValidGuest(c, 0x6Cu)) {
                uint32_t up_arrow = X86_MEM32(c + 0x64u);
                if (ValidGuest(up_arrow, 0x48u) && (X86_MEM32(up_arrow + 0x04u) & 0x01u) != 0u &&
                    X86_MEM32(up_arrow + 0x24u) >= 4u) {
                    if (ReadRawRect(base, up_arrow).Contains(gx, gy)) {
                        return HitResult{HitKind::ListboxScrollUp, c, node_va, i};
                    }
                }
                uint32_t dn_arrow = X86_MEM32(c + 0x68u);
                if (ValidGuest(dn_arrow, 0x48u) && (X86_MEM32(dn_arrow + 0x04u) & 0x01u) != 0u &&
                    X86_MEM32(dn_arrow + 0x24u) >= 4u) {
                    if (ReadRawRect(base, dn_arrow).Contains(gx, gy)) {
                        return HitResult{HitKind::ListboxScrollDown, c, node_va, i};
                    }
                }
            }
            HitResult sub = HitTestWidgetTree(base, top_va, c, gx, gy, true, depth + 1);
            if (sub.kind != HitKind::None) return sub;
            continue;
        }

        if (vt == kVtSimpleButton) {
            if (ComputeClippedRect(base, c, top_va).Contains(gx, gy)) {
                return HitResult{HitKind::Button, c, node_va, i};
            }
            continue;
        }

        if (vt == kVtRadioSlider) {
            if (ComputeClippedRect(base, c, top_va).Contains(gx, gy)) {
                return HitResult{HitKind::RadioSlider, c, node_va, i};
            }
            continue;
        }

        if (in_listbox && vt == kVtHorizontalBox) {
            if (ComputeClippedRect(base, c, top_va).Contains(gx, gy)) {
                return HitResult{HitKind::ListboxRow, c, node_va, i};
            }
            continue;
        }

        if (int32_t(X86_MEM32(c + 0x50u)) > 0) {
            HitResult sub = HitTestWidgetTree(base, top_va, c, gx, gy, in_listbox, depth + 1);
            if (sub.kind != HitKind::None) return sub;
        }
    }
    return none;
}

enum class DragKind : uint8_t {
    None = 0,
    AudioSlider,
    GameplaySlider,
    MapPan,
};

struct MenuDragState {
    DragKind kind = DragKind::None;
    uint32_t top_va = 0u;
    uint32_t slider_va = 0u;
    uint32_t meter_va = 0u;
    uint32_t val_off = 0u;
    float last_sound_t = -10.0f;
    float last_gx = 0.0f;
    float last_gy = 0.0f;

    void Clear() {
        kind = DragKind::None;
        top_va = 0u;
        slider_va = 0u;
        meter_va = 0u;
        val_off = 0u;
        last_sound_t = -10.0f;
    }
};

MenuDragState g_menu_drag{};

void ApplyContinuousAudioSlider(X86Context& ctx, uint8_t* base, uint32_t ui_mgr, uint32_t top_va, uint32_t meter_va,
                                uint32_t val_off, float t, bool play_sound) {
    if (!ValidGuest(meter_va, 0xB0u) || !ValidGuest(top_va, 0xD0u)) return;
    t = std::clamp(t, 0.0f, 1.0f);
    X86_MEMF32(meter_va + 0xA8u) = t;
    X86_MEMF32(top_va + val_off) = t;
    CallThiscall(ctx, kFnUserSettingsApply, top_va + 0xB8u, {0u});
    CallThiscall(ctx, kFnUserSettingsCopyFrom, kVaUserSettings, {top_va + 0xB8u});
    if (play_sound) PlayUiSound(ctx, ui_mgr, kSndUiToggle);
}

void ApplyContinuousGameplaySlider(X86Context& ctx, uint8_t* base, uint32_t ui_mgr, uint32_t top_va, uint32_t meter_va,
                                   uint32_t val_off, float t, bool play_sound) {
    if (!ValidGuest(meter_va, 0xB0u) || !ValidGuest(top_va, 0x118u)) return;
    t = std::clamp(t, 0.0f, 1.0f);
    float lo = ValidGuest(kVaSensMin, 4u) ? X86_MEMF32(kVaSensMin) : 0.25f;
    float hi = ValidGuest(kVaSensMax, 4u) ? X86_MEMF32(kVaSensMax) : 4.0f;
    if (!(std::isfinite(lo) && std::isfinite(hi) && hi > lo && lo > 0.0f)) {
        lo = 0.25f;
        hi = 4.0f;
    }
    float val = std::clamp(std::pow(2.0f, 4.0f * t - 2.0f), lo, hi);
    float meter_t = std::clamp((std::log2(val) + 2.0f) * 0.25f, 0.0f, 1.0f);
    X86_MEMF32(meter_va + 0xA8u) = meter_t;
    X86_MEMF32(meter_va + 0xACu) = meter_t;
    X86_MEMF32(top_va + val_off) = val;
    CallThiscall(ctx, kFnUserSettingsApply, top_va + 0xE8u, {1u});
    CallThiscall(ctx, kFnUserSettingsCopyFrom, kVaUserSettings, {top_va + 0xE8u});
    if (play_sound) PlayUiSound(ctx, ui_mgr, kSndUiToggle);
}

struct ContinuousSliderPair {
    uint32_t slider_va = 0u;
    uint32_t meter_va = 0u;
    uint32_t val_off = 0u;
    bool is_audio = false;
    bool valid = false;
};

bool LookupContinuousSlider(uint8_t* base, uint32_t top_va, uint32_t top_vt, uint32_t slider_va,
                            ContinuousSliderPair& out) {
    out = ContinuousSliderPair{};
    if (top_vt == kVtAudio && ValidGuest(top_va, 0x80u)) {
        static constexpr uint32_t kSliders[3] = {0x74u, 0x78u, 0x7Cu};
        static constexpr uint32_t kMeters[3] = {0x68u, 0x6Cu, 0x70u};
        static constexpr uint32_t kVals[3] = {0xC4u, 0xC8u, 0xCCu};
        for (int i = 0; i < 3; ++i) {
            if (X86_MEM32(top_va + kSliders[i]) == slider_va) {
                out.slider_va = slider_va;
                out.meter_va = X86_MEM32(top_va + kMeters[i]);
                out.val_off = kVals[i];
                out.is_audio = true;
                out.valid = ValidGuest(out.meter_va, 0xB0u);
                return out.valid;
            }
        }
    } else if (top_vt == kVtGameplay && ValidGuest(top_va, 0x80u)) {
        static constexpr uint32_t kSliders[2] = {0x78u, 0x7Cu};
        static constexpr uint32_t kMeters[2] = {0x68u, 0x6Cu};
        static constexpr uint32_t kVals[2] = {0x10Cu, 0x110u};
        for (int i = 0; i < 2; ++i) {
            if (X86_MEM32(top_va + kSliders[i]) == slider_va) {
                out.slider_va = slider_va;
                out.meter_va = X86_MEM32(top_va + kMeters[i]);
                out.val_off = kVals[i];
                out.is_audio = false;
                out.valid = ValidGuest(out.meter_va, 0xB0u);
                return out.valid;
            }
        }
    }
    return false;
}

bool HitTestContinuousMeter(uint8_t* base, uint32_t top_va, uint32_t top_vt, float gx, float gy,
                            ContinuousSliderPair& out) {
    out = ContinuousSliderPair{};
    if (top_vt == kVtAudio && ValidGuest(top_va, 0x80u)) {
        static constexpr uint32_t kSliders[3] = {0x74u, 0x78u, 0x7Cu};
        static constexpr uint32_t kMeters[3] = {0x68u, 0x6Cu, 0x70u};
        static constexpr uint32_t kVals[3] = {0xC4u, 0xC8u, 0xCCu};
        for (int i = 0; i < 3; ++i) {
            uint32_t m = X86_MEM32(top_va + kMeters[i]);
            uint32_t s = X86_MEM32(top_va + kSliders[i]);
            if (ValidGuest(m, 0xB0u) && ValidGuest(s, 0xD0u) && ReadRawRect(base, m).Contains(gx, gy)) {
                out = {s, m, kVals[i], true, true};
                return true;
            }
        }
    } else if (top_vt == kVtGameplay && ValidGuest(top_va, 0x80u)) {
        static constexpr uint32_t kSliders[2] = {0x78u, 0x7Cu};
        static constexpr uint32_t kMeters[2] = {0x68u, 0x6Cu};
        static constexpr uint32_t kVals[2] = {0x10Cu, 0x110u};
        for (int i = 0; i < 2; ++i) {
            uint32_t m = X86_MEM32(top_va + kMeters[i]);
            uint32_t s = X86_MEM32(top_va + kSliders[i]);
            if (ValidGuest(m, 0xB0u) && ValidGuest(s, 0xD0u) && ReadRawRect(base, m).Contains(gx, gy)) {
                out = {s, m, kVals[i], false, true};
                return true;
            }
        }
    }
    return false;
}

void CommitOptionsDiscreteToggle(X86Context& ctx, uint8_t* base, uint32_t top_va, uint32_t top_vt,
                                 uint32_t slider_va, uint32_t action_id) {
    CallOnAction(ctx, base, slider_va, action_id);
    // Pump slider->Update() immediately so listener->Notify(slider) updates staged CUserSettings now.
    CallWidgetUpdate(ctx, base, slider_va);
    if (top_vt == kVtAudio) {
        CallThiscall(ctx, kFnUserSettingsCopyFrom, kVaUserSettings, {top_va + 0xB8u});
    } else if (top_vt == kVtGameplay) {
        CallThiscall(ctx, kFnUserSettingsCopyFrom, kVaUserSettings, {top_va + 0xE8u});
    } else if (top_vt == kVtController) {
        CallThiscall(ctx, kFnUserSettingsCopyFrom, kVaUserSettings, {top_va + 0x184u});
    }
}

void StepContinuousSlider(X86Context& ctx, uint8_t* base, uint32_t ui_mgr, uint32_t top_va,
                          const ContinuousSliderPair& sp, float delta_t) {
    if (!sp.valid) return;
    float cur_t = std::clamp(X86_MEMF32(sp.meter_va + 0xA8u), 0.0f, 1.0f);
    float next_t = std::clamp(cur_t + delta_t, 0.0f, 1.0f);
    if (sp.is_audio) {
        ApplyContinuousAudioSlider(ctx, base, ui_mgr, top_va, sp.meter_va, sp.val_off, next_t, true);
    } else {
        ApplyContinuousGameplaySlider(ctx, base, ui_mgr, top_va, sp.meter_va, sp.val_off, next_t, true);
    }
}

uint32_t LookupMapDisplayEntry(X86Context& ctx, uint8_t* base, uint32_t map_disp) {
    if (!ValidGuest(map_disp, 0x90u)) return 0u;
    int32_t map_id = int32_t(X86_MEM32(map_disp + 0x70u));
    int32_t tmpl_id = int32_t(X86_MEM32(map_disp + 0x0Cu));
    uint32_t map_mgr = X86_MEM32(map_disp + 0x8Cu);
    if (map_id < 0 || tmpl_id < 0 || !ValidGuest(map_mgr, 0x10u)) return 0u;
    uint32_t entry = CallThiscall(ctx, kFnMapEntryLookup, map_mgr, {uint32_t(map_id)});
    return ValidGuest(entry, 0x1Cu) ? entry : 0u;
}

void HandleMenuMousePostUpdate(X86Context& ctx, uint8_t* base, uint32_t ui_mgr) {
    if (app::IsReplayOrAutotestActive() ||
        suffering::MkbGetConfig().input_mode == suffering::InputMode::ControllerOnly ||
        suffering::UiIsOverlayOpen() || suffering::MkbIsInActive3DGameplay()) {
        g_menu_drag.Clear();
        return;
    }

    if (!ValidGuest(kVaUiMgrPtr, 4u) || ui_mgr != X86_MEM32(kVaUiMgrPtr) || !ValidGuest(ui_mgr, 0x90u)) {
        g_menu_drag.Clear();
        return;
    }

    float guest_w = 640.0f;
    float guest_h = 480.0f;
    if (ValidGuest(kVaRendererPtr, 4u)) {
        uint32_t rptr = X86_MEM32(kVaRendererPtr);
        if (ValidGuest(rptr + 0xA0u, 4u)) {
            uint32_t gw = X86_MEM32(rptr + 0x9Cu);
            uint32_t gh = X86_MEM32(rptr + 0xA0u);
            if (gw >= 320u && gw <= 1920u && gh >= 240u && gh <= 1080u) {
                guest_w = float(gw);
                guest_h = float(gh);
            }
        }
    }

    suffering::MenuMouseFrame mf = suffering::MkbConsumeMenuMouse(guest_w, guest_h);
    if (!mf.active) {
        g_menu_drag.Clear();
        return;
    }

    int32_t count = int32_t(X86_MEM32(ui_mgr + 0x50u));
    uint32_t arr = X86_MEM32(ui_mgr + 0x48u);
    if (count <= 0 || count > 64 || !ValidGuest(arr, uint32_t(count) * 4u)) {
        g_menu_drag.Clear();
        suffering::MkbSetLastMenuDebugState(0u, -1, mf.pt.gx, mf.pt.gy, mf.pt.in_viewport);
        return;
    }

    const int32_t top_idx = count - 1;
    const uint32_t top = X86_MEM32(arr + uint32_t(top_idx) * 4u);
    if (!ValidGuest(top, 0x60u)) {
        g_menu_drag.Clear();
        return;
    }

    const uint32_t top_vt = X86_MEM32(top);
    const uint32_t top_flags = X86_MEM32(top + 0x04u);
    const uint32_t top_state = X86_MEM32(top + 0x24u);

    int32_t dbg_focus = int32_t(X86_MEM32(top + 0x08u));
    if (top_vt == kVtLoadSave && ValidGuest(top + 0x64u, 4u)) {
        uint32_t lb = X86_MEM32(top + 0x64u);
        if (ValidGuest(lb + 0x08u, 4u)) dbg_focus = int32_t(X86_MEM32(lb + 0x08u));
    }
    suffering::MkbSetLastMenuDebugState(top_vt, dbg_focus, mf.pt.gx, mf.pt.gy, mf.pt.in_viewport);

    if (IsNonInteractiveScreenVt(top_vt) || top_state != 4u || (top_flags & 0x01u) == 0u ||
        (top_flags & 0x10u) != 0u) {
        g_menu_drag.Clear();
        return;
    }

    // 1. Continue or terminate active continuous drag (slider drag / map pan)
    if (g_menu_drag.kind != DragKind::None) {
        if (!mf.lmb_down || mf.lmb_released || g_menu_drag.top_va != top) {
            g_menu_drag.Clear();
        } else if (mf.has_pos) {
            if (g_menu_drag.kind == DragKind::AudioSlider || g_menu_drag.kind == DragKind::GameplaySlider) {
                UiRect mr = ReadRawRect(base, g_menu_drag.meter_va);
                if (mr.valid) {
                    float t = std::clamp((mf.pt.gx_clamped - mr.l) / std::max(mr.r - mr.l, 1.0f), 0.0f, 1.0f);
                    bool snd = std::fabs(t - g_menu_drag.last_sound_t) >= 0.05f;
                    if (snd) g_menu_drag.last_sound_t = t;
                    if (g_menu_drag.kind == DragKind::AudioSlider) {
                        ApplyContinuousAudioSlider(ctx, base, ui_mgr, top, g_menu_drag.meter_va, g_menu_drag.val_off,
                                                   t, snd);
                    } else {
                        ApplyContinuousGameplaySlider(ctx, base, ui_mgr, top, g_menu_drag.meter_va,
                                                      g_menu_drag.val_off, t, snd);
                    }
                }
            } else if (g_menu_drag.kind == DragKind::MapPan && top_vt == kVtMap && ValidGuest(top + 0x60u, 4u)) {
                uint32_t map_disp = X86_MEM32(top + 0x60u);
                uint32_t entry = LookupMapDisplayEntry(ctx, base, map_disp);
                if (entry != 0u) {
                    float dgx = mf.pt.gx_clamped - g_menu_drag.last_gx;
                    float dgy = mf.pt.gy_clamped - g_menu_drag.last_gy;
                    g_menu_drag.last_gx = mf.pt.gx_clamped;
                    g_menu_drag.last_gy = mf.pt.gy_clamped;
                    float zoom = std::max(X86_MEMF32(entry + 0x18u), 1.0f);
                    X86_MEMF32(entry + 0x10u) = std::max(0.0f, X86_MEMF32(entry + 0x10u) - dgx / zoom);
                    X86_MEMF32(entry + 0x14u) = std::max(0.0f, X86_MEMF32(entry + 0x14u) - dgy / zoom);
                    CallWidgetUpdate(ctx, base, map_disp);
                    X86_MEMF32(entry + 0x10u) = X86_MEMF32(map_disp + 0x74u);
                    X86_MEMF32(entry + 0x14u) = X86_MEMF32(map_disp + 0x78u);
                }
            }
            return;
        }
    }

    // 2. Right-Click (RMB) -> Universal Back / Cancel (exact replica of CUIMgr::Update 0x0005FAE6..0x0005FB32)
    if (mf.rmb_pressed) {
        g_menu_drag.Clear();
        if (top_vt == kVtOpeningMovies) {
            CallOnAction(ctx, base, top, kActAnyButton);
            return;
        }
        if (top_vt == kVtMainMenu) {
            // Only send Cancel on Main Menu when the Difficulty submenu ([+0x80]) is visible!
            if (ValidGuest(top + 0x80u, 4u)) {
                uint32_t diff0 = X86_MEM32(top + 0x80u);
                if (ValidGuest(diff0 + 0x24u, 4u)) {
                    uint32_t st = X86_MEM32(diff0 + 0x24u);
                    if (st == 4u || st == 5u) CallOnAction(ctx, base, top, kActGuiCancel);
                }
            }
            return;
        }
        uint32_t can_cancel = 0u;
        if (ValidGuest(top_vt + 0x30u, 4u)) {
            can_cancel = CallThiscall(ctx, X86_MEM32(top_vt + 0x30u), top, {}) & 0xFFu;
        }
        if (can_cancel != 0u) {
            X86_MEM32(ui_mgr + 0x20u) = kActGuiCancel;
            X86_MEM32(top + 0x04u) = top_flags | 0x10u;
            uint32_t ui_mgr_vt = X86_MEM32(ui_mgr);
            if (ValidGuest(ui_mgr_vt + 0x10u, 4u)) {
                CallThiscall(ctx, X86_MEM32(ui_mgr_vt + 0x10u), ui_mgr, {uint32_t(top_idx)});
            }
            PlayUiSound(ctx, ui_mgr, kSndUiCancel);  // Pass ui_mgr, NEVER destroyed top!
        } else {
            CallOnAction(ctx, base, top, kActGuiCancel);
        }
        return;
    }

    // 3. Screen-specific special handlers
    if (top_vt == kVtOpeningMovies) {
        if (mf.lmb_pressed && mf.pt.in_viewport) {
            CallOnAction(ctx, base, top, kActAnyButton);
        }
        return;
    }

    if (top_vt == kVtMainMenu) {
        if (!ValidGuest(top, 0x158u)) return;
        // Interactive gate: [top+0x64] == 0xE and fade timer [top+0x154] == 0.0f
        if (X86_MEM32(top + 0x64u) != 0xEu || X86_MEMF32(top + 0x154u) != 0.0f) return;
        // Press Start phase: [+0x100] != 0 && [+0xFC] == 0 -> LMB anywhere advances via AnyButton (0x400)
        if (X86_MEM32(top + 0x100u) != 0u && X86_MEM32(top + 0xFCu) == 0u) {
            if (mf.lmb_pressed && mf.pt.in_viewport) {
                CallOnAction(ctx, base, top, kActAnyButton);
            }
            return;
        }
        // Main Menu buttons & Difficulty buttons fall through to generic widget hit-testing below.
    }

    if (top_vt == kVtArchives) {
        if (!ValidGuest(top, 0x80u) || X86_MEM32(top + 0x7Cu) != 0x3E8u) return;
        int32_t open_page = int32_t(X86_MEM32(top + 0x70u));
        if (mf.wheel_steps != 0) {
            uint32_t act = (mf.wheel_steps > 0) ? kActGuiLeft : kActGuiRight;
            int n = std::min(std::abs(mf.wheel_steps), 4);
            for (int i = 0; i < n; ++i) CallOnAction(ctx, base, top, act);
        }
        if (mf.lmb_pressed && mf.pt.in_viewport) {
            if (open_page < 0) {
                if (mf.pt.gx < guest_w * 0.28f) CallOnAction(ctx, base, top, kActGuiLeft);
                else if (mf.pt.gx > guest_w * 0.72f) CallOnAction(ctx, base, top, kActGuiRight);
                else CallOnAction(ctx, base, top, kActGuiSelect);
            } else {
                CallOnAction(ctx, base, top, (mf.pt.gx < guest_w * 0.50f) ? kActGuiLeft : kActGuiRight);
            }
        }
        return;
    }

    if (top_vt == kVtMap) {
        if (!ValidGuest(top, 0x68u)) return;
        uint32_t map_disp = X86_MEM32(top + 0x60u);
        uint32_t map_inv = X86_MEM32(top + 0x64u);
        UiRect inv_rc = ReadRawRect(base, map_inv);
        UiRect disp_rc = ReadRawRect(base, map_disp);
        if (mf.wheel_steps != 0) {
            bool over_inv = mf.has_pos && inv_rc.Contains(mf.pt.gx, mf.pt.gy);
            int n = std::min(std::abs(mf.wheel_steps), 4);
            if (over_inv && ValidGuest(map_inv, 4u)) {
                uint32_t act = (mf.wheel_steps > 0) ? kActMapInvUp : kActMapInvDown;
                for (int i = 0; i < n; ++i) CallOnAction(ctx, base, map_inv, act);
            } else {
                uint32_t entry = LookupMapDisplayEntry(ctx, base, map_disp);
                if (entry != 0u) {
                    X86_MEMF32(entry + 0x18u) =
                        std::max(1.0f, X86_MEMF32(entry + 0x18u) + 0.25f * float(mf.wheel_steps));
                    X86_MEMF32(map_disp + 0x80u) = 0.0f;
                    CallWidgetUpdate(ctx, base, map_disp);
                    X86_MEMF32(entry + 0x10u) = X86_MEMF32(map_disp + 0x74u);
                    X86_MEMF32(entry + 0x14u) = X86_MEMF32(map_disp + 0x78u);
                }
            }
        }
        if (mf.lmb_pressed && mf.pt.in_viewport && disp_rc.Contains(mf.pt.gx, mf.pt.gy)) {
            g_menu_drag.kind = DragKind::MapPan;
            g_menu_drag.top_va = top;
            g_menu_drag.last_gx = mf.pt.gx_clamped;
            g_menu_drag.last_gy = mf.pt.gy_clamped;
        }
        return;
    }

    if (top_vt == kVtCredits) {
        if (mf.wheel_steps != 0) {
            uint32_t act = (mf.wheel_steps > 0) ? kActGuiUp : kActGuiDown;
            int n = std::min(std::abs(mf.wheel_steps), 4);
            for (int i = 0; i < n; ++i) CallOnAction(ctx, base, top, act);
        }
        return;
    }

    if (top_vt == kVtScreenAdjust) {
        if (mf.lmb_pressed && mf.pt.in_viewport) {
            CallOnAction(ctx, base, top, kActGuiSelect);
        }
        return;
    }

    if (top_vt == kVtLoadSave) {
        if (!ValidGuest(top, 0x84u) || X86_MEM32(top + 0x80u) != 5u) return;
    }

    // 4. Direct HUD Meter hit-testing on Audio & Gameplay continuous sliders
    ContinuousSliderPair meter_hit{};
    bool hit_meter =
        mf.has_pos && mf.pt.in_viewport && HitTestContinuousMeter(base, top, top_vt, mf.pt.gx, mf.pt.gy, meter_hit);

    HitResult hit{};
    if (mf.has_pos && mf.pt.in_viewport && !hit_meter) {
        hit = HitTestWidgetTree(base, top, top, mf.pt.gx, mf.pt.gy, false);
    }

    // 5. Hover selection (ONLY when mouse physically moved — NEVER fights keyboard/gamepad!)
    if (mf.moved && mf.pt.in_viewport) {
        if (hit_meter) {
            FocusChainToWidget(ctx, base, ui_mgr, top, meter_hit.slider_va, true);
        } else if (hit.kind == HitKind::Button || hit.kind == HitKind::RadioSlider ||
                   hit.kind == HitKind::ListboxRow) {
            FocusChainToWidget(ctx, base, ui_mgr, top, hit.widget, true);
        }
    }

    // 6. Left-Click activation & slider drag/toggle (dead-space clicks do NOTHING!)
    if (mf.lmb_pressed && mf.pt.in_viewport) {
        if (hit_meter) {
            FocusChainToWidget(ctx, base, ui_mgr, top, meter_hit.slider_va, false);
            UiRect mr = ReadRawRect(base, meter_hit.meter_va);
            float t = std::clamp((mf.pt.gx_clamped - mr.l) / std::max(mr.r - mr.l, 1.0f), 0.0f, 1.0f);
            if (meter_hit.is_audio) {
                ApplyContinuousAudioSlider(ctx, base, ui_mgr, top, meter_hit.meter_va, meter_hit.val_off, t, true);
                g_menu_drag.kind = DragKind::AudioSlider;
            } else {
                ApplyContinuousGameplaySlider(ctx, base, ui_mgr, top, meter_hit.meter_va, meter_hit.val_off, t, true);
                g_menu_drag.kind = DragKind::GameplaySlider;
            }
            g_menu_drag.top_va = top;
            g_menu_drag.slider_va = meter_hit.slider_va;
            g_menu_drag.meter_va = meter_hit.meter_va;
            g_menu_drag.val_off = meter_hit.val_off;
            g_menu_drag.last_sound_t = t;
            return;
        }

        if (hit.kind == HitKind::ListboxScrollUp) {
            DispatchListboxAction(ctx, base, ui_mgr, hit.widget, kActGuiUp);
            return;
        }
        if (hit.kind == HitKind::ListboxScrollDown) {
            DispatchListboxAction(ctx, base, ui_mgr, hit.widget, kActGuiDown);
            return;
        }
        if (hit.kind == HitKind::Button || hit.kind == HitKind::ListboxRow) {
            FocusChainToWidget(ctx, base, ui_mgr, top, hit.widget, false);
            CallOnAction(ctx, base, top, kActGuiSelect);
            return;
        }
        if (hit.kind == HitKind::RadioSlider && ValidGuest(hit.widget, 0xD0u)) {
            FocusChainToWidget(ctx, base, ui_mgr, top, hit.widget, false);
            int32_t disc_idx = int32_t(X86_MEM32(hit.widget + 0xBCu));
            uint32_t left_arrow = X86_MEM32(hit.widget + 0xC8u);
            uint32_t right_arrow = X86_MEM32(hit.widget + 0xCCu);
            bool on_left = ValidGuest(left_arrow, 0x48u) && ReadRawRect(base, left_arrow).Contains(mf.pt.gx, mf.pt.gy);
            bool on_right =
                ValidGuest(right_arrow, 0x48u) && ReadRawRect(base, right_arrow).Contains(mf.pt.gx, mf.pt.gy);
            if (disc_idx == -1) {
                // Continuous slider: NEVER send 0x3FC (closes Audio/Gameplay) or rely on GetAnalogValue!
                ContinuousSliderPair sp{};
                if (LookupContinuousSlider(base, top, top_vt, hit.widget, sp)) {
                    if (on_left) {
                        StepContinuousSlider(ctx, base, ui_mgr, top, sp, -0.05f);
                    } else if (on_right) {
                        StepContinuousSlider(ctx, base, ui_mgr, top, sp, +0.05f);
                    } else {
                        UiRect mr = ReadRawRect(base, sp.meter_va);
                        if (mr.valid) {
                            float t = std::clamp((mf.pt.gx_clamped - mr.l) / std::max(mr.r - mr.l, 1.0f), 0.0f, 1.0f);
                            if (sp.is_audio) {
                                ApplyContinuousAudioSlider(ctx, base, ui_mgr, top, sp.meter_va, sp.val_off, t, true);
                                g_menu_drag.kind = DragKind::AudioSlider;
                            } else {
                                ApplyContinuousGameplaySlider(ctx, base, ui_mgr, top, sp.meter_va, sp.val_off, t, true);
                                g_menu_drag.kind = DragKind::GameplaySlider;
                            }
                            g_menu_drag.top_va = top;
                            g_menu_drag.slider_va = sp.slider_va;
                            g_menu_drag.meter_va = sp.meter_va;
                            g_menu_drag.val_off = sp.val_off;
                            g_menu_drag.last_sound_t = t;
                        }
                    }
                }
            } else {
                // Discrete radio toggle: left arrow -> 0x3EE, right arrow or row body -> 0x3EF. NEVER send 0x3FC!
                uint32_t act = on_left ? kActGuiLeft : kActGuiRight;
                CommitOptionsDiscreteToggle(ctx, base, top, top_vt, hit.widget, act);
            }
            return;
        }
    }

    // 7. Mouse Wheel scrolling / slider stepping
    if (mf.wheel_steps != 0) {
        if (top_vt == kVtAudio || top_vt == kVtGameplay || top_vt == kVtController) {
            uint32_t target_slider = 0u;
            if (hit_meter) {
                target_slider = meter_hit.slider_va;
            } else if (hit.kind == HitKind::RadioSlider) {
                target_slider = hit.widget;
            } else {
                int32_t fidx = int32_t(X86_MEM32(top + 0x08u));
                int32_t ccnt = int32_t(X86_MEM32(top + 0x50u));
                uint32_t carr = X86_MEM32(top + 0x48u);
                if (fidx >= 0 && fidx < ccnt && ValidGuest(carr, uint32_t(ccnt) * 4u)) {
                    uint32_t fc = X86_MEM32(carr + uint32_t(fidx) * 4u);
                    if (ValidGuest(fc, 0xD0u) && X86_MEM32(fc) == kVtRadioSlider) target_slider = fc;
                }
            }
            if (target_slider != 0u && ValidGuest(target_slider, 0xD0u)) {
                int32_t disc_idx = int32_t(X86_MEM32(target_slider + 0xBCu));
                if (disc_idx == -1) {
                    ContinuousSliderPair sp{};
                    if (LookupContinuousSlider(base, top, top_vt, target_slider, sp)) {
                        StepContinuousSlider(ctx, base, ui_mgr, top, sp, 0.05f * float(mf.wheel_steps));
                        return;
                    }
                } else {
                    uint32_t act = (mf.wheel_steps > 0) ? kActGuiRight : kActGuiLeft;
                    int n = std::min(std::abs(mf.wheel_steps), 4);
                    for (int i = 0; i < n; ++i) {
                        CommitOptionsDiscreteToggle(ctx, base, top, top_vt, target_slider, act);
                    }
                    return;
                }
            }
        }

        uint32_t listbox = FindFirstListbox(base, top);
        if (listbox != 0u) {
            uint32_t act = (mf.wheel_steps > 0) ? kActGuiUp : kActGuiDown;
            int n = std::min(std::abs(mf.wheel_steps), 6);
            for (int i = 0; i < n; ++i) {
                DispatchListboxAction(ctx, base, ui_mgr, listbox, act);
            }
            return;
        }

        if (top_vt == kVtMainMenu || top_vt == kVtPause || top_vt == kVtOptions || top_vt == kVtExtras ||
            top_vt == kVtGameFinished || top_vt == kVtAlertBox || top_vt == kVtSaveDevice ||
            top_vt == kVtLevelSelect) {
            uint32_t act = (mf.wheel_steps > 0) ? kActGuiUp : kActGuiDown;
            int n = std::min(std::abs(mf.wheel_steps), 6);
            for (int i = 0; i < n; ++i) CallOnAction(ctx, base, top, act);
        }
    }
}

// High-resolution host steady_clock accumulator for the primary guest CClock (0x002DC724).
// Eliminates 1ms KeTickCount quantization, CClock::Unpause dt snowballs / 0.10ms collapses,
// and per-frame SetTimeScale(1.0f) segment-anchor stalls.
struct HighResClockState {
    std::chrono::steady_clock::time_point last_tick{};
    double total_s = 0.0;
    bool valid = false;
    bool just_unpaused = false;

    static std::chrono::steady_clock::duration ClampedPrevDtDuration(uint8_t* base) {
        float prev_dt = X86_MEMF32(kVaGlobalDt);
        double back_s = (std::isfinite(prev_dt) && prev_dt > 0.001f) ? std::min(double(prev_dt), 1.0 / 60.0) : (1.0 / 60.0);
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(back_s));
    }

    void OnCtor() {
        last_tick = std::chrono::steady_clock::now();
        total_s = 0.0;
        valid = true;
        just_unpaused = false;
    }

    void OnReset(uint8_t* base, bool full_reset) {
        auto now = std::chrono::steady_clock::now();
        if (full_reset) total_s = 0.0;
        last_tick = now - ClampedPrevDtDuration(base);
        valid = true;
        just_unpaused = false;
    }

    void OnPause() {
        // CClock::Pause (0x001A3210) only snapshots [esi+0x0C] and does not modify [esi]; leave last_tick untouched.
    }

    void OnUnpause() {
        // Do NOT backdate by 16.67ms: Unpause runs mid-frame before Swap/Pace(), so backdating doubles dt to 33.3ms.
        last_tick = std::chrono::steady_clock::now();
        valid = true;
        just_unpaused = true;
    }

    void OnLoadState(uint8_t* base, uint32_t clock_va) {
        auto now = std::chrono::steady_clock::now();
        float loaded = X86_MEMF32(clock_va + 0x20u);
        total_s = std::isfinite(loaded) ? double(loaded) : 0.0;
        last_tick = now - ClampedPrevDtDuration(base);
        valid = true;
        just_unpaused = false;
    }
};

HighResClockState g_main_clk{};

// 60 Hz Cloth::Update accumulator for native >60 FPS gameplay (sub_001A6480).
struct ClothAccumulatorState {
    uint32_t last_frame_id = UINT32_MAX;
    float accum = 0.0f;
    float step_dt = 1.0f / 60.0f;
    bool step_this_frame = true;
};

ClothAccumulatorState g_cloth_accum{};

}  // namespace

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

// 7. CUIMgr::Update (__thiscall, ret 0):
// Runs native PC menu mouse hover, click, slider drag/step, wheel scroll, and RMB cancel on the
// guest thread immediately after CUIMgr flushes deferred pushes and aligns active screens.
X86_FUNC(sub_0005F9A0) {
    const uint32_t ui_mgr = ctx.ecx;
    __imp__sub_0005F9A0(ctx, base);
    HandleMenuMousePostUpdate(ctx, base, ui_mgr);
}

// 8. Sub-microsecond CClock (0x002DC724) lifecycle & tick hooks (NOT bypassed during replay/autotest):
// Replaces 1ms integer GetTickCount dt sawtooth (16/17/18ms at 60 FPS; 5/6/7ms at 165 FPS) with
// continuous steady_clock double->float dt and prevents CClock::Unpause backdate dt snowballs.
X86_FUNC(sub_001A3170) {
    const uint32_t clock_va = ctx.ecx;
    __imp__sub_001A3170(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnCtor();
}

X86_FUNC(sub_001A31A0) {
    const uint32_t clock_va = ctx.ecx;
    const uint32_t full_reset = X86_MEM32(ctx.esp + 0x04u);
    __imp__sub_001A31A0(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnReset(base, full_reset != 0u);
}

X86_FUNC(sub_001A3210) {
    const uint32_t clock_va = ctx.ecx;
    __imp__sub_001A3210(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnPause();
}

X86_FUNC(sub_001A3220) {
    const uint32_t clock_va = ctx.ecx;
    __imp__sub_001A3220(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnUnpause();
}

X86_FUNC(sub_001A3390) {
    const uint32_t clock_va = ctx.ecx;
    __imp__sub_001A3390(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnLoadState(base, clock_va);
}

X86_FUNC(sub_001A3470) {
    const uint32_t clock_va = ctx.ecx;
    const uint32_t out_dt_va = X86_MEM32(ctx.esp + 0x04u);
    __imp__sub_001A3470(ctx, base);
    if (clock_va != kVaMainClock) return;

    auto now = std::chrono::steady_clock::now();
    double raw_dt = g_main_clk.valid ? std::chrono::duration<double>(now - g_main_clk.last_tick).count() : (1.0 / 60.0);
    g_main_clk.last_tick = now;
    if (g_main_clk.just_unpaused) {
        g_main_clk.just_unpaused = false;
        if (raw_dt < 0.002) raw_dt = 1.0 / 60.0;
    }
    if (!g_main_clk.valid) {
        float seed = X86_MEMF32(clock_va + 0x20u);
        g_main_clk.total_s = std::isfinite(seed) ? double(seed) : 0.0;
        g_main_clk.valid = true;
    }

    raw_dt = std::clamp(raw_dt, 0.0001, 0.25);
    float scale = X86_MEMF32(clock_va + 0x2Cu);
    double dt_d = raw_dt * double(scale);
    g_main_clk.total_s += dt_d;
    if (out_dt_va >= 0x10000u && mem::IsCommitted(out_dt_va)) {
        X86_MEMF32(out_dt_va) = float(dt_d);
    }
    X86_MEMF32(clock_va + 0x20u) = float(g_main_clk.total_s);
    X87_ST(0) = x87_prec(ctx, g_main_clk.total_s);
}

// 9. Cloth::Update (__thiscall, ret 0x04) — 60 Hz fixed accumulator for native >60 FPS (90/120/165 Hz).
// At <= ~71 FPS (g_dt >= 0.014f) passes straight through untouched.
X86_FUNC(sub_001A6480) {
    const float g_dt = X86_MEMF32(kVaGlobalDt);
    if (g_dt >= 0.014f || !(g_dt > 0.0f && std::isfinite(g_dt))) {
        g_cloth_accum.last_frame_id = UINT32_MAX;
        g_cloth_accum.accum = 0.0f;
        g_cloth_accum.step_this_frame = true;
        __imp__sub_001A6480(ctx, base);
        return;
    }

    const uint32_t frame_id = X86_MEM32(kVaFrameCount);
    if (frame_id != g_cloth_accum.last_frame_id) {
        g_cloth_accum.last_frame_id = frame_id;
        g_cloth_accum.accum += g_dt;
        constexpr float kStep60 = 1.0f / 60.0f;
        if (g_cloth_accum.accum >= kStep60) {
            g_cloth_accum.step_this_frame = true;
            g_cloth_accum.step_dt = std::min(g_cloth_accum.accum, 1.0f / 30.0f);
            g_cloth_accum.accum = std::fmod(g_cloth_accum.accum, kStep60);
        } else {
            g_cloth_accum.step_this_frame = false;
        }
    }

    if (!g_cloth_accum.step_this_frame) {
        ctx.esp += 8u;  // __thiscall ret 0x04
        return;
    }

    const float saved_dt = X86_MEMF32(kVaGlobalDt);
    const float saved_inv_dt = X86_MEMF32(kVaGlobalInvDt);
    X86_MEMF32(kVaGlobalDt) = g_cloth_accum.step_dt;
    X86_MEMF32(kVaGlobalInvDt) = 1.0f / g_cloth_accum.step_dt;
    __imp__sub_001A6480(ctx, base);
    X86_MEMF32(kVaGlobalDt) = saved_dt;
    X86_MEMF32(kVaGlobalInvDt) = saved_inv_dt;
}
