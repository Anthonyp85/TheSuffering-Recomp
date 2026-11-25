// SPDX-License-Identifier: GPL-2.0-or-later
// Game-specific hooks for The Suffering (USA) default.xbe (Title ID 0x4D570015).
// Implements:
//  - Native-PC Mouse & Keyboard 3D look injection (post-deadzone, framerate-independent,
//    zero-coasting yaw slew bypass, decoupled PC pitch polarity, Scheme 1 dedicated R3 flashlight)
//  - Full native-PC point-and-click, hover, drag, wheel, and right-click-back menu mouse engine
//    inside CUIMgr::Update (sub_0005F9A0) across all Riot Engine UI screens
//  - Native "Quit Game" Main Menu option below "Bonus Material" (CMainMenuGui, 0x0029B4F8) across
//    controller, keyboard, and mouse with clean cross-platform host shutdown (bypassed in replay/autotest)
//  - Host-persisted CUserSettings (0x002DB218) synchronization across cold restarts, in-game Options
//    sub-screens (Controller, Gameplay, Audio, Screen Adjust), and live Launcher / F1 UI toggles
//  - Sub-microsecond CClock (0x002DC724) timing hooks eliminating 1ms KeTickCount quantization,
//    Unpause dt snowballs, pause/streaming-pause total_s drift, and SaveState wall-clock drift
//  - 60 Hz Cloth::Update accumulator (sub_001A6480) for safe native >60 FPS gameplay
//  - XMVDecoder_GetNextFrame VBlank yield (sub_00265C7B) and cooperative SRSC / CSTOMP yields
//    (sub_001765E0, sub_00156CA0, sub_001BAB90)
//  - Surgical 2 Cellblocks Dallas (#2152) script race & softlock guards (sub_000D9AE0, sub_001414C0)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <SDL3/SDL.h>
#endif

#include "app/app.h"
#include "common/mkb_input.h"
#include "common/settings_ui.h"
#include "core/log.h"
#include "game/guest_settings.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"

namespace kernel {
void SleepHighResUs(int64_t us);
}

X86_EXTERN_FUNC(__imp__sub_00016B60);
X86_EXTERN_FUNC(__imp__sub_00016CA0);
X86_EXTERN_FUNC(__imp__sub_00016D00);
X86_EXTERN_FUNC(__imp__sub_00016F10);
X86_EXTERN_FUNC(__imp__sub_0014E1F0);
X86_EXTERN_FUNC(__imp__sub_001B09A0);
X86_EXTERN_FUNC(__imp__sub_000576A0);
X86_EXTERN_FUNC(__imp__sub_000505D0);
X86_EXTERN_FUNC(__imp__sub_000465F0);
X86_EXTERN_FUNC(__imp__sub_00265C7B);
X86_EXTERN_FUNC(__imp__sub_0005F9A0);
X86_EXTERN_FUNC(__imp__sub_0006F2A0);
X86_EXTERN_FUNC(__imp__sub_0006F450);
X86_EXTERN_FUNC(__imp__sub_00071B00);
X86_EXTERN_FUNC(__imp__sub_00072120);
X86_EXTERN_FUNC(__imp__sub_000D9AE0);
X86_EXTERN_FUNC(__imp__sub_001414C0);
X86_EXTERN_FUNC(__imp__sub_00156CA0);
X86_EXTERN_FUNC(__imp__sub_001765E0);
X86_EXTERN_FUNC(__imp__sub_001A3170);
X86_EXTERN_FUNC(__imp__sub_001A31A0);
X86_EXTERN_FUNC(__imp__sub_001A3210);
X86_EXTERN_FUNC(__imp__sub_001A3220);
X86_EXTERN_FUNC(__imp__sub_001A3320);
X86_EXTERN_FUNC(__imp__sub_001A3390);
X86_EXTERN_FUNC(__imp__sub_001A3470);
X86_EXTERN_FUNC(__imp__sub_001A6480);
X86_EXTERN_FUNC(__imp__sub_001BAB90);

namespace {

// Verified guest globals (The Suffering USA default.xbe, Title ID 0x4D570015)
constexpr uint32_t kVaLevelMgrPtr = 0x002D0D88u;
constexpr uint32_t kVaWorldPtr = 0x002D0DC0u;
constexpr uint32_t kVaUiMgrPtr = 0x002DAD78u;
constexpr uint32_t kVaMainMenuPtr = 0x002DADACu;
constexpr uint32_t kVaRendererPtr = 0x002DC75Cu;
constexpr uint32_t kVaUserSettings = 0x002DB218u;
constexpr uint32_t kVaSensMax = 0x002DADA0u;
constexpr uint32_t kVaSensMin = 0x002DADA4u;
constexpr uint32_t kVaMainClock = 0x002DC724u;
constexpr uint32_t kVaInstanceIdMap = 0x002DD1B4u;
constexpr uint32_t kVaLevelMgr = 0x002DEF40u;
constexpr uint32_t kVaGlobalDt = 0x002E01D0u;
constexpr uint32_t kVaGlobalInvDt = 0x002E01D4u;
constexpr uint32_t kVaPauseFlags = 0x002E0274u;
constexpr uint32_t kVaFrameCount = 0x002E0280u;
constexpr uint32_t kVaMaxDt = 0x002E0298u;

// Verified logic actor vtables & 2 Cellblocks instance IDs
constexpr uint32_t kVtMessageRelay = 0x0029EFC8u;
constexpr uint32_t kVtTimer = 0x002A2F68u;
constexpr uint32_t kIidDallas = 2152u;
constexpr uint32_t kIidCellblocksStompDoneRelay = 4051u;
constexpr uint32_t kIidCellblocksDallasGateRelay = 11966u;
constexpr uint32_t kIidCellblocksDallasDeadTimer = 12614u;

// Verified strong-symbol guest UI methods
constexpr uint32_t kFnSetFocusedChild = 0x0005D790u;
constexpr uint32_t kFnPlayUiSound = 0x0005DDB0u;
constexpr uint32_t kFnUserSettingsApply = 0x00016B60u;
constexpr uint32_t kFnUserSettingsCopyFrom = 0x00016D00u;
constexpr uint32_t kFnMapEntryLookup = 0x000356E0u;
constexpr uint32_t kFnGuiSetAlphaRec = 0x0003E3E0u;
constexpr uint32_t kFnGuiSetState = 0x0005D450u;
constexpr uint32_t kFnGuiAddChild = 0x0005E200u;
constexpr uint32_t kFnGuiInsertBefore = 0x0005E240u;
constexpr uint32_t kFnGuiEnsureInit = 0x00060170u;
constexpr uint32_t kFnRadioSelectOption = 0x00078810u;
constexpr uint32_t kFnSimpleTextSetWide = 0x00078930u;
constexpr uint32_t kFnSimpleButtonCtor = 0x00079880u;
constexpr uint32_t kFnGuiEffectsCtor = 0x0007A710u;
constexpr uint32_t kFnGuiEffectsEnable = 0x0007A800u;
constexpr uint32_t kFnOperatorNew = 0x001E0860u;

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

void FlushPendingDragSettingsSave();

void ClearMenuDragAndFlush() {
    g_menu_drag.Clear();
    FlushPendingDragSettingsSave();
}

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

// Native Main Menu "Quit Game" button state (injected directly below "Bonus Material" on CMainMenuGui).
// Strictly disabled when app::IsReplayOrAutotestActive() is true to preserve 0.0 replay drift.
struct MainMenuQuitState {
    uint32_t menu_va = 0u;
    uint32_t btn_va = 0u;
    uint32_t effect_va = 0u;

    void Clear() {
        menu_va = 0u;
        btn_va = 0u;
        effect_va = 0u;
    }
};

MainMenuQuitState g_main_menu_quit{};
std::atomic<bool> g_host_quit_requested{false};

void RequestCleanHostQuit() {
    if (app::IsReplayOrAutotestActive()) return;
    if (g_host_quit_requested.exchange(true, std::memory_order_acq_rel)) return;
    LOGR("ui: Main Menu Quit Game activated; requesting clean host window close");
#ifdef _WIN32
    if (void* w = app::Window()) {
        PostMessageW(static_cast<HWND>(w), WM_CLOSE, 0, 0);
    }
#else
    SDL_Event ev{};
    ev.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&ev);
#endif
}

bool SetWidgetWideTextAscii(X86Context& ctx, uint8_t* base, uint32_t widget_va, const char* ascii) {
    if (!ValidGuest(widget_va, 0x84u) || !ascii) return false;
    const uint32_t saved_esp = ctx.esp;
    const uint32_t scratch_esp = (saved_esp - 96u) & ~15u;
    if (!ValidGuest(scratch_esp, 64u)) return false;
    const uint32_t str_va = scratch_esp + 16u;
    size_t len = 0u;
    while (ascii[len] != '\0' && len < 22u) {
        X86_MEM16(str_va + uint32_t(len) * 2u) = uint16_t(static_cast<unsigned char>(ascii[len]));
        ++len;
    }
    X86_MEM16(str_va + uint32_t(len) * 2u) = 0u;
    ctx.esp = scratch_esp;
    CallThiscall(ctx, kFnSimpleTextSetWide, widget_va, {str_va});
    ctx.esp = saved_esp;
    return X86_MEM32(widget_va + 0x60u) != 0u;
}

void LayoutMainMenuQuitButton(uint8_t* base, uint32_t menu_va, uint32_t quit_btn_va) {
    if (!ValidGuest(menu_va, 0x160u) || !ValidGuest(quit_btn_va, 0xB0u)) return;
    const uint32_t bonus_va = X86_MEM32(menu_va + 0x7Cu);
    const uint32_t archives_va = X86_MEM32(menu_va + 0x78u);
    if (!ValidGuest(bonus_va, 0xB0u) || !ValidGuest(archives_va, 0xB0u)) return;

    float bx = X86_MEMF32(bonus_va + 0x38u);
    float by = X86_MEMF32(bonus_va + 0x3Cu);
    const float bw = X86_MEMF32(bonus_va + 0x40u);
    const float bh = X86_MEMF32(bonus_va + 0x44u);
    float ay = X86_MEMF32(archives_va + 0x3Cu);

    const uint32_t bonus_fx = X86_MEM32(menu_va + 0xC0u);
    if (ValidGuest(bonus_fx, 0x98u) && X86_MEM32(bonus_fx) == kVtGuiEffects &&
        X86_MEM32(bonus_fx + 0x80u) != 0xBF800000u) {
        bx = X86_MEMF32(bonus_fx + 0x70u);
        by = X86_MEMF32(bonus_fx + 0x74u);
    }
    const uint32_t archives_fx = X86_MEM32(menu_va + 0xBCu);
    if (ValidGuest(archives_fx, 0x98u) && X86_MEM32(archives_fx) == kVtGuiEffects &&
        X86_MEM32(archives_fx + 0x80u) != 0xBF800000u) {
        ay = X86_MEMF32(archives_fx + 0x74u);
    }

    if (!std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bw) || !std::isfinite(bh) || bw <= 0.5f ||
        bh <= 0.5f) {
        return;
    }

    float step_y = by - ay;
    if (!std::isfinite(step_y) || step_y < 16.0f || step_y > 120.0f) {
        step_y = std::max(bh + 12.0f, 45.0f);
    }

    float guest_h = 480.0f;
    if (ValidGuest(kVaRendererPtr, 4u)) {
        const uint32_t rptr = X86_MEM32(kVaRendererPtr);
        if (ValidGuest(rptr + 0xA0u, 4u)) {
            const uint32_t gh = X86_MEM32(rptr + 0xA0u);
            if (gh >= 240u && gh <= 1080u) guest_h = float(gh);
        }
    }

    const float qx = bx;
    float qy = by + step_y;
    const float max_bottom = guest_h - 8.0f;
    if (qy + bh > max_bottom) {
        const float shift_up = std::min((qy + bh) - max_bottom, 48.0f);
        const uint32_t new_game_va = X86_MEM32(menu_va + 0x68u);
        if (shift_up > 0.0f && ValidGuest(new_game_va, 0x48u) && X86_MEMF32(new_game_va + 0x3Cu) - shift_up >= 16.0f) {
            static constexpr uint32_t kMenuBtnOffsets[6] = {0x68u, 0x6Cu, 0x70u, 0x74u, 0x78u, 0x7Cu};
            for (uint32_t off : kMenuBtnOffsets) {
                const uint32_t bva = X86_MEM32(menu_va + off);
                if (ValidGuest(bva, 0x48u)) {
                    X86_MEMF32(bva + 0x3Cu) -= shift_up;
                }
            }
            qy -= shift_up;
        } else {
            qy = std::max(by + bh + 4.0f, max_bottom - bh);
        }
    }

    // Clear template h_align / v_align (+0x30 / +0x34) so CGUI::Align (0x0005EE90) preserves explicit screen coords.
    X86_MEM32(quit_btn_va + 0x30u) = 0u;
    X86_MEM32(quit_btn_va + 0x34u) = 0u;
    X86_MEMF32(quit_btn_va + 0x38u) = qx;
    X86_MEMF32(quit_btn_va + 0x3Cu) = qy;
    X86_MEMF32(quit_btn_va + 0x40u) = bw;
    X86_MEMF32(quit_btn_va + 0x44u) = bh;
}

void SyncMainMenuQuitButtonState(X86Context& ctx, uint8_t* base, uint32_t menu_va) {
    if (g_main_menu_quit.menu_va != menu_va || !ValidGuest(menu_va, 0x160u) || X86_MEM32(menu_va) != kVtMainMenu) {
        return;
    }
    const uint32_t quit_btn = g_main_menu_quit.btn_va;
    const uint32_t bonus_va = X86_MEM32(menu_va + 0x7Cu);
    if (!ValidGuest(quit_btn, 0xB0u) || X86_MEM32(quit_btn) != kVtSimpleButton || !ValidGuest(bonus_va, 0xB0u)) {
        return;
    }

    const uint32_t bonus_state = X86_MEM32(bonus_va + 0x24u);
    const uint32_t quit_state = X86_MEM32(quit_btn + 0x24u);
    uint32_t want_state = quit_state;
    if (bonus_state == 1u || bonus_state == 2u || bonus_state == 3u) {
        want_state = bonus_state;
    } else if (bonus_state == 4u || bonus_state == 5u) {
        const int32_t quit_idx = FindChildIndex(base, menu_va, quit_btn);
        const int32_t focused_idx = int32_t(X86_MEM32(menu_va + 0x08u));
        want_state = (quit_idx >= 0 && quit_idx == focused_idx) ? 5u : 4u;
    }

    if (bonus_state == 3u) {
        const uint32_t bonus_alpha_bits = X86_MEM32(bonus_va + 0x14u);
        if (X86_MEM32(quit_btn + 0x14u) != bonus_alpha_bits) {
            CallThiscall(ctx, kFnGuiSetAlphaRec, quit_btn, {bonus_alpha_bits});
        }
    } else if ((want_state == 4u || want_state == 5u) && quit_state < 4u) {
        CallThiscall(ctx, kFnGuiSetAlphaRec, quit_btn, {0x3F800000u});
    }

    if (quit_state != want_state) {
        CallThiscall(ctx, kFnGuiSetState, quit_btn, {want_state});
    }

    const uint32_t quit_fx = g_main_menu_quit.effect_va;
    const uint32_t bonus_fx = X86_MEM32(menu_va + 0xC0u);
    if (ValidGuest(quit_fx, 0x98u) && X86_MEM32(quit_fx) == kVtGuiEffects && ValidGuest(bonus_fx, 0x98u) &&
        X86_MEM32(bonus_fx) == kVtGuiEffects) {
        const bool bonus_fx_on =
            (X86_MEM32(bonus_fx + 0x64u) | X86_MEM32(bonus_fx + 0x68u) | X86_MEM32(bonus_fx + 0x6Cu)) != 0u;
        const bool quit_fx_on =
            (X86_MEM32(quit_fx + 0x64u) | X86_MEM32(quit_fx + 0x68u) | X86_MEM32(quit_fx + 0x6Cu)) != 0u;
        if (bonus_fx_on != quit_fx_on) {
            CallThiscall(ctx, kFnGuiEffectsEnable, quit_fx, {bonus_fx_on ? 1u : 0u});
        }
    }
}

void EnsureAndSyncMainMenuQuitButton(X86Context& ctx, uint8_t* base, uint32_t menu_va, bool from_layout) {
    if (app::IsReplayOrAutotestActive()) return;
    if (!ValidGuest(menu_va, 0x160u) || X86_MEM32(menu_va) != kVtMainMenu) return;
    if (!from_layout && (X86_MEM32(menu_va + 0x04u) & 0x01u) == 0u) return;

    if (g_main_menu_quit.menu_va == menu_va && ValidGuest(g_main_menu_quit.btn_va, 0xB0u) &&
        X86_MEM32(g_main_menu_quit.btn_va) == kVtSimpleButton &&
        FindChildIndex(base, menu_va, g_main_menu_quit.btn_va) >= 0) {
        if (from_layout) {
            LayoutMainMenuQuitButton(base, menu_va, g_main_menu_quit.btn_va);
        }
        SyncMainMenuQuitButtonState(ctx, base, menu_va);
        return;
    }

    const uint32_t bonus_va = X86_MEM32(menu_va + 0x7Cu);
    const uint32_t archives_va = X86_MEM32(menu_va + 0x78u);
    if (!ValidGuest(bonus_va, 0xB0u) || !ValidGuest(archives_va, 0xB0u)) return;
    if (!ReadRawRect(base, bonus_va).valid || !ReadRawRect(base, archives_va).valid) return;

    const uint32_t bonus_tmpl_id = X86_MEM32(bonus_va + 0x0Cu);
    const uint32_t btn_alloc = kernel::CallGuest(ctx, kFnOperatorNew, {0x40u, 0xB0u});
    if (!ValidGuest(btn_alloc, 0xB0u)) return;

    const uint32_t quit_btn = CallThiscall(ctx, kFnSimpleButtonCtor, btn_alloc, {bonus_tmpl_id});
    if (!ValidGuest(quit_btn, 0xB0u) || X86_MEM32(quit_btn) != kVtSimpleButton) return;

    X86_MEM32(quit_btn + 0x58u) = menu_va + 0x60u;
    if (!SetWidgetWideTextAscii(ctx, base, quit_btn, "Quit Game")) return;
    LayoutMainMenuQuitButton(base, menu_va, quit_btn);

    const int32_t bonus_idx = FindChildIndex(base, menu_va, bonus_va);
    const int32_t cnt = int32_t(X86_MEM32(menu_va + 0x50u));
    const uint32_t arr = X86_MEM32(menu_va + 0x48u);
    int32_t cur_focus = int32_t(X86_MEM32(menu_va + 0x08u));
    if (bonus_idx >= 0 && bonus_idx + 1 < cnt && ValidGuest(arr, uint32_t(cnt) * 4u)) {
        const uint32_t before_child = X86_MEM32(arr + uint32_t(bonus_idx + 1) * 4u);
        X86_MEM32(quit_btn + 0x5Cu) = menu_va;
        CallThiscall(ctx, kFnGuiInsertBefore, menu_va, {quit_btn, before_child});
        if (cur_focus >= bonus_idx + 1) {
            ++cur_focus;
            X86_MEM32(menu_va + 0x08u) = uint32_t(cur_focus);
        }
    } else {
        CallThiscall(ctx, kFnGuiAddChild, menu_va, {quit_btn});
    }

    uint32_t quit_fx = 0u;
    const uint32_t bonus_fx = X86_MEM32(menu_va + 0xC0u);
    if (ValidGuest(bonus_fx, 0x98u) && X86_MEM32(bonus_fx) == kVtGuiEffects) {
        const uint32_t fx_tmpl_id = X86_MEM32(bonus_fx + 0x0Cu);
        const uint32_t fx_alloc = kernel::CallGuest(ctx, kFnOperatorNew, {0x40u, 0xF4u});
        if (ValidGuest(fx_alloc, 0xF4u)) {
            const int32_t quit_idx_before = FindChildIndex(base, menu_va, quit_btn);
            quit_fx = CallThiscall(ctx, kFnGuiEffectsCtor, fx_alloc, {fx_tmpl_id, quit_btn});
            if (!ValidGuest(quit_fx, 0xF4u) || X86_MEM32(quit_fx) != kVtGuiEffects) {
                quit_fx = 0u;
            } else if (quit_idx_before >= 0 && cur_focus >= quit_idx_before) {
                ++cur_focus;
                X86_MEM32(menu_va + 0x08u) = uint32_t(cur_focus);
            }
        }
    }

    g_main_menu_quit.menu_va = menu_va;
    g_main_menu_quit.btn_va = quit_btn;
    g_main_menu_quit.effect_va = quit_fx;

    SyncMainMenuQuitButtonState(ctx, base, menu_va);
    if (quit_fx != 0u) {
        CallThiscall(ctx, kFnGuiEnsureInit, quit_fx, {});
    }
    CallThiscall(ctx, kFnGuiEnsureInit, quit_btn, {});

    LOGI("ui: injected Main Menu Quit Game button (menu=%08X btn=%08X fx=%08X rect=%.1f,%.1f %.1fx%.1f)", menu_va,
         quit_btn, quit_fx, X86_MEMF32(quit_btn + 0x38u), X86_MEMF32(quit_btn + 0x3Cu),
         X86_MEMF32(quit_btn + 0x40u), X86_MEMF32(quit_btn + 0x44u));
}

void HandleMenuMousePostUpdate(X86Context& ctx, uint8_t* base, uint32_t ui_mgr) {
    if (app::IsReplayOrAutotestActive() ||
        suffering::MkbGetConfig().input_mode == suffering::InputMode::ControllerOnly ||
        suffering::UiIsOverlayOpen() || suffering::MkbIsInActive3DGameplay()) {
        ClearMenuDragAndFlush();
        return;
    }

    if (!ValidGuest(kVaUiMgrPtr, 4u) || ui_mgr != X86_MEM32(kVaUiMgrPtr) || !ValidGuest(ui_mgr, 0x90u)) {
        ClearMenuDragAndFlush();
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
        ClearMenuDragAndFlush();
        return;
    }

    int32_t count = int32_t(X86_MEM32(ui_mgr + 0x50u));
    uint32_t arr = X86_MEM32(ui_mgr + 0x48u);
    if (count <= 0 || count > 64 || !ValidGuest(arr, uint32_t(count) * 4u)) {
        ClearMenuDragAndFlush();
        suffering::MkbSetLastMenuDebugState(0u, -1, mf.pt.gx, mf.pt.gy, mf.pt.in_viewport);
        return;
    }

    const int32_t top_idx = count - 1;
    const uint32_t top = X86_MEM32(arr + uint32_t(top_idx) * 4u);
    if (!ValidGuest(top, 0x60u)) {
        ClearMenuDragAndFlush();
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
        ClearMenuDragAndFlush();
        return;
    }

    // 1. Continue or terminate active continuous drag (slider drag / map pan)
    if (g_menu_drag.kind != DragKind::None) {
        if (!mf.lmb_down || mf.lmb_released || g_menu_drag.top_va != top) {
            ClearMenuDragAndFlush();
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
        ClearMenuDragAndFlush();
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
            g_menu_drag.kind = meter_hit.is_audio ? DragKind::AudioSlider : DragKind::GameplaySlider;
            g_menu_drag.top_va = top;
            g_menu_drag.slider_va = meter_hit.slider_va;
            g_menu_drag.meter_va = meter_hit.meter_va;
            g_menu_drag.val_off = meter_hit.val_off;
            g_menu_drag.last_sound_t = t;
            if (meter_hit.is_audio) {
                ApplyContinuousAudioSlider(ctx, base, ui_mgr, top, meter_hit.meter_va, meter_hit.val_off, t, true);
            } else {
                ApplyContinuousGameplaySlider(ctx, base, ui_mgr, top, meter_hit.meter_va, meter_hit.val_off, t, true);
            }
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
            if (!app::IsReplayOrAutotestActive() && g_main_menu_quit.menu_va == top &&
                hit.widget == g_main_menu_quit.btn_va) {
                RequestCleanHostQuit();
            }
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
                            g_menu_drag.kind = sp.is_audio ? DragKind::AudioSlider : DragKind::GameplaySlider;
                            g_menu_drag.top_va = top;
                            g_menu_drag.slider_va = sp.slider_va;
                            g_menu_drag.meter_va = sp.meter_va;
                            g_menu_drag.val_off = sp.val_off;
                            g_menu_drag.last_sound_t = t;
                            if (sp.is_audio) {
                                ApplyContinuousAudioSlider(ctx, base, ui_mgr, top, sp.meter_va, sp.val_off, t, true);
                            } else {
                                ApplyContinuousGameplaySlider(ctx, base, ui_mgr, top, sp.meter_va, sp.val_off, t, true);
                            }
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

    void SyncGuestClockAnchors(uint8_t* base, uint32_t clock_va) const {
        const float t = float(total_s);
        X86_MEMF32(clock_va + 0x20u) = t;
        X86_MEMF32(clock_va + 0x24u) = t;
        if (kernel::g_tick_count_va >= 0x10000u && mem::IsCommitted(kernel::g_tick_count_va)) {
            X86_MEM32(clock_va + 0x28u) = X86_MEM32(kernel::g_tick_count_va);
        }
    }

    void OnCtor(uint8_t* base, uint32_t clock_va) {
        last_tick = std::chrono::steady_clock::now();
        total_s = 0.0;
        valid = true;
        just_unpaused = false;
        SyncGuestClockAnchors(base, clock_va);
    }

    void OnReset(uint8_t* base, uint32_t clock_va, bool full_reset) {
        auto now = std::chrono::steady_clock::now();
        if (full_reset) total_s = 0.0;
        last_tick = now - ClampedPrevDtDuration(base);
        valid = true;
        just_unpaused = false;
        SyncGuestClockAnchors(base, clock_va);
    }

    void OnPause(uint8_t* base, uint32_t clock_va) {
        // CClock::Pause (0x001A3210) snapshots [esi+0x0C]; keep [+0x20/+0x24] anchored to frozen total_s.
        if (valid) {
            const float t = float(total_s);
            X86_MEMF32(clock_va + 0x20u) = t;
            X86_MEMF32(clock_va + 0x24u) = t;
        }
    }

    void OnUnpause(uint8_t* base, uint32_t clock_va) {
        // Do NOT backdate by 16.67ms: Unpause runs mid-frame before Swap/Pace(), so backdating doubles dt to 33.3ms.
        last_tick = std::chrono::steady_clock::now();
        valid = true;
        just_unpaused = true;
        SyncGuestClockAnchors(base, clock_va);
    }

    void OnLoadState(uint8_t* base, uint32_t clock_va) {
        auto now = std::chrono::steady_clock::now();
        float loaded = X86_MEMF32(clock_va + 0x20u);
        total_s = std::isfinite(loaded) ? double(loaded) : 0.0;
        last_tick = now - ClampedPrevDtDuration(base);
        valid = true;
        just_unpaused = false;
        SyncGuestClockAnchors(base, clock_va);
    }
};

HighResClockState g_main_clk{};

// Cooperatively yields to the SRSC streaming worker while an acquired SRSC resource is in state 2 (LOADING).
void WaitSrscResourceReady(uint8_t* base, uint32_t res_va) {
    if (!ValidGuest(res_va + 0x0Eu, 1u)) return;
    volatile const uint8_t* state_ptr = reinterpret_cast<volatile const uint8_t*>(base + res_va + 0x0Eu);
    uint32_t spins = 0;
    while ((*state_ptr & 0x03u) == 0x02u) {
        if (spins < 16u) {
            std::this_thread::yield();
        } else {
            kernel::SleepHighResUs(50);
        }
        ++spins;
    }
    std::atomic_thread_fence(std::memory_order_acquire);
}

// Identifies the 18 verified Riot Engine call-sites that invoke SRSCResourceMgr::Acquire (sub_001765E0)
// with bAsync=1 (to avoid sub_00176310's 200ms Sleep(0xC8) poll) and then immediately spin in user-mode
// on `([res + 0x0E] & 3) == 2` waiting for the SRSC worker thread to finish loading.
bool IsSyncSrscAcquireCallSite(const X86Context& ctx, uint8_t* base) {
    const uint32_t ret_eip = X86_MEM32(ctx.esp);
    switch (ret_eip) {
        // Direct callers in CAudioMgr & CAnimMgr that spin immediately on ([eax/ecx/esi + 0x0E] & 3) == 2
        case 0x0015A539u:  // sub_0015A4F0
        case 0x0015A5C9u:  // sub_0015A580
        case 0x0015A64Eu:  // sub_0015A610
        case 0x0015AD0Bu:  // sub_0015ACB0
        case 0x0016F9F5u:  // sub_0016F9C0
        case 0x0016FA75u:  // sub_0016FA40
        case 0x0016FB05u:  // sub_0016FAC0
        case 0x0016FBC5u:  // sub_0016FB80
            return true;

        // 1-level typed-acquire wrappers (each allocates 0x10 bytes + pushes 1 arg + return addr -> outer ret at +0x18)
        case 0x001625ECu:  // sub_001625C0
        case 0x0016F8DCu:  // sub_0016F8B0
        case 0x001786AAu:  // sub_00178670
        case 0x0017AD2Cu:  // sub_0017AD00
        case 0x0017B2CAu:  // sub_0017B290
        {
            const uint32_t outer_va = ctx.esp + 0x18u;
            if (!ValidGuest(outer_va, 4u)) return false;
            const uint32_t outer_ret = X86_MEM32(outer_va);
            switch (outer_ret) {
                case 0x000612CDu:  // sub_00061240 (6-texture loop spinning at 0x000612D7..0x000612FC)
                case 0x00064CB6u:  // sub_00064BF0 (CCredits CSV load)
                case 0x000CA12Du:  // sub_000CA0F0
                case 0x00162455u:  // sub_00162120
                case 0x00163850u:  // sub_00163710
                case 0x00166DA8u:  // sub_00166D60
                case 0x0016E750u:  // sub_0016E670 (transition anim sync path; async 0x0016E6C8 is NOT matched)
                case 0x001BA6EFu:  // sub_001BA680
                    return true;
                case 0x00156CCFu:  // sub_00156CA0(int bAsync): spins at 0x00156D20 iff bAsync ([esp+0x2C]) == 0
                    return ValidGuest(ctx.esp + 0x2Cu, 4u) && X86_MEM32(ctx.esp + 0x2Cu) == 0u;
                case 0x00193871u:  // sub_00193840 (spins at 0x00193882 iff ebx == 0)
                    return ctx.ebx == 0u;
                default:
                    return false;
            }
        }
        default:
            return false;
    }
}

// 60 Hz Cloth::Update accumulator for native >60 FPS gameplay (sub_001A6480).
struct ClothAccumulatorState {
    uint32_t last_frame_id = UINT32_MAX;
    float accum = 0.0f;
    float step_dt = 1.0f / 60.0f;
    bool step_this_frame = true;
};

ClothAccumulatorState g_cloth_accum{};

// Uniquely identifies `English/Levels/2 Cellblocks/2 Cellblocks.lvl` via bounded case-insensitive
// substring match on CLevelMgr::m_levelPath ([0x002D0D88] / 0x002DEF40, 0x104 bytes).
bool IsCellblocks2Level(uint8_t* base) {
    uint32_t lvl_va = ValidGuest(kVaLevelMgrPtr, 4u) ? X86_MEM32(kVaLevelMgrPtr) : 0u;
    if (!ValidGuest(lvl_va, 0x104u)) {
        lvl_va = kVaLevelMgr;
        if (!ValidGuest(lvl_va, 0x104u)) return false;
    }
    const char* s = reinterpret_cast<const char*>(base + lvl_va);
    size_t len = 0;
    while (len < 0x104u && s[len] != '\0') ++len;

    constexpr char kNeedle[] = "2 cellblocks";
    constexpr size_t kNeedleLen = sizeof(kNeedle) - 1u;
    if (len < kNeedleLen) return false;
    for (size_t i = 0; i + kNeedleLen <= len; ++i) {
        bool match = true;
        for (size_t j = 0; j < kNeedleLen; ++j) {
            char c = s[i + j];
            if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
            if (c != kNeedle[j]) {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

// Read-only CGameObject* lookup by instance ID across CInstanceIdMap (0x002DD1B4, sub_00163E00)
// and CWorld's 6-byte packed object handle table ([world + 0x0C], capacity [world + 0x14], sub_00149850).
uint32_t FindObjectByIid(uint8_t* base, uint32_t target_iid) {
    if (!ValidGuest(kVaInstanceIdMap, 0x14u) || !ValidGuest(kVaWorldPtr, 4u)) return 0u;
    const int32_t count = int32_t(X86_MEM32(kVaInstanceIdMap + 0x10u));
    const uint32_t handles_va = X86_MEM32(kVaInstanceIdMap + 0x08u);
    if (count <= 0 || count > 65536 || !ValidGuest(handles_va, uint32_t(count) * 2u)) return 0u;

    const uint32_t world_va = X86_MEM32(kVaWorldPtr);
    if (!ValidGuest(world_va + 0x0Cu, 12u)) return 0u;
    const uint32_t tbl_va = X86_MEM32(world_va + 0x0Cu);
    const int32_t tbl_cap = int32_t(X86_MEM32(world_va + 0x14u));
    if (tbl_cap <= 0 || tbl_cap > 65536 || !ValidGuest(tbl_va, uint32_t(tbl_cap) * 6u)) return 0u;

    for (int32_t i = 0; i < count; ++i) {
        const uint16_t h = X86_MEM16(handles_va + uint32_t(i) * 2u);
        if (h == 0u || int32_t(h) >= tbl_cap) continue;
        const uint32_t obj_va = X86_MEM32(tbl_va + uint32_t(h) * 6u);
        if (!ValidGuest(obj_va, 0xECu)) continue;
        if (X86_MEM32(obj_va + 0xD0u) == target_iid) {
            return obj_va;
        }
    }
    return 0u;
}

// Verifies Dallas (#2152) exists in the world, is not flagged destroyed ([+0x08] & 0x80000000, sub_001558D0),
// has entity_kind == CNPC (7, sub_000EF400), and is !CDamageable::IsDead (sub_000AF560).
// Returns false on Evil-morality runs where the player killed Dallas so Timer #12614 stays enabled.
bool IsDallasAlive(uint8_t* base) {
    const uint32_t obj_va = FindObjectByIid(base, kIidDallas);
    if (obj_va == 0u) return false;
    if ((X86_MEM32(obj_va + 0x08u) & 0x80000000u) != 0u) return false;
    if (X86_MEM32(obj_va + 0xE0u) != 7u) return false;

    const uint32_t npc_va = X86_MEM32(obj_va + 0x60u);
    if (!ValidGuest(npc_va, 0x24u)) return false;
    if (X86_MEM32(npc_va + 0x0Eu) != 0u || X86_MEM32(npc_va + 0x1Eu) != 0u) return true;
    const float hp = X86_MEMF32(npc_va + 0x16u);
    return std::isfinite(hp) && hp > 0.0f;
}

// Host-persisted CUserSettings (0x002DB218) runtime state & bidirectional sync helpers.
// Strictly bypassed whenever app::IsReplayOrAutotestActive() is true so recorded traces keep 0.0 drift.
struct GuestSettingsRuntimeState {
    std::mutex mtx;
    suffering::guest_settings::GuestUserSettings current{};
    suffering::guest_settings::GuestUserSettings last_saved{};
    std::atomic<bool> host_to_guest_dirty{false};
    std::atomic<bool> pending_host_save{false};
};

GuestSettingsRuntimeState& GuestSettingsRt() {
    static GuestSettingsRuntimeState s_rt{};
    return s_rt;
}

// Guest-main-thread-only lifecycle flags (single_core_guest = true, CUserSettings runs on main UI/init thread)
bool g_boot_settings_initialized = false;
bool g_applying_host_to_guest = false;
bool g_save_pending_after_drag = false;

bool SettingsValuesEqual(const suffering::guest_settings::GuestUserSettings& a,
                         const suffering::guest_settings::GuestUserSettings& b) {
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

suffering::guest_settings::GuestUserSettings ReadSettingsFromGuestRam(uint8_t* base, uint32_t va) {
    suffering::guest_settings::GuestUserSettings s = suffering::guest_settings::MakeDefaultSettings();
    if (!ValidGuest(va, 0x44u)) return s;
    s.control_scheme = X86_MEM32(va + 0x00u);
    s.screen_offset_x = int32_t(X86_MEM32(va + 0x04u));
    s.screen_offset_y = int32_t(X86_MEM32(va + 0x08u));
    s.effects_volume = X86_MEMF32(va + 0x0Cu);
    s.dialog_volume = X86_MEMF32(va + 0x10u);
    s.music_volume = X86_MEMF32(va + 0x14u);
    s.stereo = X86_MEM32(va + 0x18u) != 0u;
    s.surround = X86_MEM32(va + 0x1Cu) != 0u;
    s.subtitles = X86_MEM32(va + 0x20u) != 0u;
    s.turn_sensitivity = X86_MEMF32(va + 0x24u);
    s.pitch_sensitivity = X86_MEMF32(va + 0x28u);
    s.auto_targeting = X86_MEM32(va + 0x2Cu) != 0u;
    s.invert_pitch = X86_MEM32(va + 0x30u) != 0u;
    s.vibration = X86_MEM32(va + 0x34u) != 0u;
    s.help_text = X86_MEM32(va + 0x38u) != 0u;
    s.auto_pitch_correction = X86_MEM32(va + 0x3Cu) != 0u;
    s.checkpoints = X86_MEM32(va + 0x40u);
    return suffering::guest_settings::SanitizeSettings(s);
}

// Writes user preference fields (+0x00..+0x40) into a guest CUserSettings struct while leaving
// unlocked Archive bitfields (+0x44, +0x48), runtime loaded flag (+0x4C), and Prelude flag (+0x50) untouched.
void WriteSettingsToGuestRam(uint8_t* base, uint32_t va, const suffering::guest_settings::GuestUserSettings& raw) {
    if (!ValidGuest(va, 0x44u)) return;
    const suffering::guest_settings::GuestUserSettings s = suffering::guest_settings::SanitizeSettings(raw);
    X86_MEM32(va + 0x00u) = s.control_scheme;
    X86_MEM32(va + 0x04u) = uint32_t(s.screen_offset_x);
    X86_MEM32(va + 0x08u) = uint32_t(s.screen_offset_y);
    X86_MEMF32(va + 0x0Cu) = s.effects_volume;
    X86_MEMF32(va + 0x10u) = s.dialog_volume;
    X86_MEMF32(va + 0x14u) = s.music_volume;
    X86_MEM32(va + 0x18u) = s.stereo ? 1u : 0u;
    X86_MEM32(va + 0x1Cu) = s.surround ? 1u : 0u;
    X86_MEM32(va + 0x20u) = s.subtitles ? 1u : 0u;
    X86_MEMF32(va + 0x24u) = s.turn_sensitivity;
    X86_MEMF32(va + 0x28u) = s.pitch_sensitivity;
    X86_MEM32(va + 0x2Cu) = s.auto_targeting ? 1u : 0u;
    X86_MEM32(va + 0x30u) = s.invert_pitch ? 1u : 0u;
    X86_MEM32(va + 0x34u) = s.vibration ? 1u : 0u;
    X86_MEM32(va + 0x38u) = s.help_text ? 1u : 0u;
    X86_MEM32(va + 0x3Cu) = s.auto_pitch_correction ? 1u : 0u;
    X86_MEM32(va + 0x40u) = s.checkpoints;
}

void TriggerOrQueueGuestSettingsSave() {
    if (app::IsReplayOrAutotestActive()) return;
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    if (!suffering::UiIsOverlayOpen()) {
        rt.pending_host_save.store(false, std::memory_order_release);
        app::Save();
    } else {
        rt.pending_host_save.store(true, std::memory_order_release);
    }
}

void FlushPendingDragSettingsSave() {
    if (!g_save_pending_after_drag) return;
    g_save_pending_after_drag = false;
    if (app::IsReplayOrAutotestActive()) return;

    bool should_save = false;
    {
        GuestSettingsRuntimeState& rt = GuestSettingsRt();
        std::lock_guard<std::mutex> lock(rt.mtx);
        if (!SettingsValuesEqual(rt.current, rt.last_saved)) {
            rt.current.has_saved_values = true;
            rt.last_saved = rt.current;
            should_save = true;
        }
    }
    if (should_save) {
        TriggerOrQueueGuestSettingsSave();
    }
}

void OnGuestSettingsCommittedFromMenu(uint8_t* base) {
    if (app::IsReplayOrAutotestActive() || !ValidGuest(kVaUserSettings, 0x44u)) return;
    suffering::guest_settings::GuestUserSettings snap = ReadSettingsFromGuestRam(base, kVaUserSettings);
    snap.has_saved_values = true;

    bool should_save_now = false;
    {
        GuestSettingsRuntimeState& rt = GuestSettingsRt();
        std::lock_guard<std::mutex> lock(rt.mtx);
        if (!SettingsValuesEqual(snap, rt.last_saved) || !rt.current.has_saved_values) {
            const bool values_changed = !SettingsValuesEqual(snap, rt.last_saved);
            rt.current = snap;
            if (values_changed) {
                if (g_menu_drag.kind == DragKind::AudioSlider || g_menu_drag.kind == DragKind::GameplaySlider) {
                    g_save_pending_after_drag = true;
                } else {
                    rt.last_saved = snap;
                    g_save_pending_after_drag = false;
                    should_save_now = true;
                }
            }
        }
    }
    if (should_save_now) {
        TriggerOrQueueGuestSettingsSave();
    }
}

void SelectRadioOptionIfValid(X86Context& ctx, uint8_t* base, uint32_t slider_va, uint32_t option_va) {
    if (ValidGuest(slider_va, 0xD0u) && X86_MEM32(slider_va) == kVtRadioSlider && ValidGuest(option_va, 0xACu)) {
        CallThiscall(ctx, kFnRadioSelectOption, slider_va, {option_va});
    }
}

// If an Xbox Options sub-screen is currently open on the UI stack when the player edits an option
// via the F1 host overlay, keep that screen's staged CUserSettings copy and widgets synchronized too.
void SyncOpenOptionsScreenFromSettings(X86Context& ctx, uint8_t* base,
                                       const suffering::guest_settings::GuestUserSettings& s) {
    if (!ValidGuest(kVaUiMgrPtr, 4u)) return;
    const uint32_t ui_mgr = X86_MEM32(kVaUiMgrPtr);
    if (!ValidGuest(ui_mgr, 0x60u)) return;
    const int32_t count = int32_t(X86_MEM32(ui_mgr + 0x50u));
    const uint32_t arr = X86_MEM32(ui_mgr + 0x48u);
    if (count <= 0 || count > 64 || !ValidGuest(arr, uint32_t(count) * 4u)) return;
    const uint32_t top = X86_MEM32(arr + uint32_t(count - 1) * 4u);
    if (!ValidGuest(top, 0x60u)) return;

    const uint32_t top_vt = X86_MEM32(top);
    if (top_vt == kVtController && ValidGuest(top, 0x1DCu)) {
        WriteSettingsToGuestRam(base, top + 0x184u, s);
        if (s.control_scheme <= 4u) {
            SelectRadioOptionIfValid(ctx, base, X86_MEM32(top + 0x68u),
                                     X86_MEM32(top + 0x78u + s.control_scheme * 4u));
        }
        SelectRadioOptionIfValid(ctx, base, X86_MEM32(top + 0x6Cu),
                                 X86_MEM32(top + (s.invert_pitch ? 0x74u : 0x70u)));
    } else if (top_vt == kVtGameplay && ValidGuest(top, 0x140u)) {
        WriteSettingsToGuestRam(base, top + 0xE8u, s);
        if (s.checkpoints <= 2u) {
            SelectRadioOptionIfValid(ctx, base, X86_MEM32(top + 0x70u),
                                     X86_MEM32(top + 0x90u + s.checkpoints * 4u));
        }
        SelectRadioOptionIfValid(ctx, base, X86_MEM32(top + 0x74u),
                                 X86_MEM32(top + (s.auto_targeting ? 0xA0u : 0x9Cu)));
        SelectRadioOptionIfValid(ctx, base, X86_MEM32(top + 0x80u),
                                 X86_MEM32(top + (s.invert_pitch ? 0xA8u : 0xA4u)));
        SelectRadioOptionIfValid(ctx, base, X86_MEM32(top + 0x84u),
                                 X86_MEM32(top + (s.vibration ? 0xB0u : 0xACu)));
        SelectRadioOptionIfValid(ctx, base, X86_MEM32(top + 0x88u),
                                 X86_MEM32(top + (s.help_text ? 0xB8u : 0xB4u)));
        SelectRadioOptionIfValid(ctx, base, X86_MEM32(top + 0x8Cu),
                                 X86_MEM32(top + (s.auto_pitch_correction ? 0xC0u : 0xBCu)));
        const uint32_t m0 = X86_MEM32(top + 0x68u);
        const uint32_t m1 = X86_MEM32(top + 0x6Cu);
        if (ValidGuest(m0, 0xB0u)) {
            const float t0 = std::clamp((std::log2(std::max(s.turn_sensitivity, 0.25f)) + 2.0f) * 0.25f, 0.0f, 1.0f);
            X86_MEMF32(m0 + 0xA8u) = t0;
            X86_MEMF32(m0 + 0xACu) = t0;
        }
        if (ValidGuest(m1, 0xB0u)) {
            const float t1 = std::clamp((std::log2(std::max(s.pitch_sensitivity, 0.25f)) + 2.0f) * 0.25f, 0.0f, 1.0f);
            X86_MEMF32(m1 + 0xA8u) = t1;
            X86_MEMF32(m1 + 0xACu) = t1;
        }
    } else if (top_vt == kVtAudio && ValidGuest(top, 0x104u)) {
        WriteSettingsToGuestRam(base, top + 0xB8u, s);
        const uint32_t m0 = X86_MEM32(top + 0x68u);
        const uint32_t m1 = X86_MEM32(top + 0x6Cu);
        const uint32_t m2 = X86_MEM32(top + 0x70u);
        if (ValidGuest(m0, 0xB0u)) X86_MEMF32(m0 + 0xA8u) = s.effects_volume;
        if (ValidGuest(m1, 0xB0u)) X86_MEMF32(m1 + 0xA8u) = s.dialog_volume;
        if (ValidGuest(m2, 0xB0u)) X86_MEMF32(m2 + 0xA8u) = s.music_volume;
    } else if (top_vt == kVtScreenAdjust && ValidGuest(top, 0xBCu)) {
        WriteSettingsToGuestRam(base, top + 0x64u, s);
    }
}

void SyncHostSettingsToGuestIfDirty(X86Context& ctx, uint8_t* base) {
    if (app::IsReplayOrAutotestActive() || !g_boot_settings_initialized) return;
    GuestSettingsRuntimeState& rt = GuestSettingsRt();

    if (!suffering::UiIsOverlayOpen() && rt.pending_host_save.exchange(false, std::memory_order_acq_rel)) {
        app::Save();
    }

    if (!rt.host_to_guest_dirty.exchange(false, std::memory_order_acq_rel)) return;
    if (!ValidGuest(kVaUserSettings, 0x44u)) return;

    const suffering::guest_settings::GuestUserSettings s = suffering::guest_settings::GetSettings();
    if (!s.has_saved_values) return;

    g_applying_host_to_guest = true;
    WriteSettingsToGuestRam(base, kVaUserSettings, s);
    CallThiscall(ctx, kFnUserSettingsApply, kVaUserSettings, {1u});
    SyncOpenOptionsScreenFromSettings(ctx, base, s);
    g_applying_host_to_guest = false;
}

}  // namespace

namespace suffering::guest_settings {

GuestUserSettings MakeDefaultSettings() { return GuestUserSettings{}; }

GuestUserSettings SanitizeSettings(const GuestUserSettings& in) {
    GuestUserSettings s = in;
    if (s.control_scheme > 4u) s.control_scheme = 0u;
    if (s.checkpoints > 2u) s.checkpoints = 2u;
    s.screen_offset_x = std::clamp(s.screen_offset_x, -64, 64);
    s.screen_offset_y = std::clamp(s.screen_offset_y, -64, 64);
    s.effects_volume = std::isfinite(s.effects_volume) ? std::clamp(s.effects_volume, 0.0f, 1.0f) : 1.0f;
    s.dialog_volume = std::isfinite(s.dialog_volume) ? std::clamp(s.dialog_volume, 0.0f, 1.0f) : 1.0f;
    s.music_volume = std::isfinite(s.music_volume) ? std::clamp(s.music_volume, 0.0f, 1.0f) : 1.0f;
    s.turn_sensitivity = std::isfinite(s.turn_sensitivity) ? std::clamp(s.turn_sensitivity, 0.25f, 4.0f) : 1.0f;
    s.pitch_sensitivity = std::isfinite(s.pitch_sensitivity) ? std::clamp(s.pitch_sensitivity, 0.25f, 4.0f) : 1.0f;
    if (!s.stereo) s.surround = false;  // Mono is always (false, false)
    return s;
}

GuestUserSettings GetSettings() {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    std::lock_guard<std::mutex> lock(rt.mtx);
    return rt.current;
}

void SetSettingsFromConfig(const GuestUserSettings& s) {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    const GuestUserSettings clean = SanitizeSettings(s);
    {
        std::lock_guard<std::mutex> lock(rt.mtx);
        rt.current = clean;
        rt.last_saved = clean;
    }
    if (clean.has_saved_values) {
        rt.host_to_guest_dirty.store(true, std::memory_order_release);
    }
}

void ResetToDefaults(bool push_to_guest) {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    GuestUserSettings def = MakeDefaultSettings();
    def.has_saved_values = push_to_guest;
    {
        std::lock_guard<std::mutex> lock(rt.mtx);
        rt.current = def;
        rt.last_saved = def;
    }
    if (push_to_guest) {
        rt.host_to_guest_dirty.store(true, std::memory_order_release);
    }
}

bool ConsumePendingHostSaveRequest() {
    if (app::IsReplayOrAutotestActive()) return false;
    return GuestSettingsRt().pending_host_save.exchange(false, std::memory_order_acq_rel);
}

bool GetControllerInvertY() { return GetSettings().invert_pitch; }

void SetControllerInvertY(bool inverted) {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    {
        std::lock_guard<std::mutex> lock(rt.mtx);
        rt.current.invert_pitch = inverted;
        rt.current.has_saved_values = true;
        rt.last_saved = rt.current;
    }
    rt.host_to_guest_dirty.store(true, std::memory_order_release);
}

bool GetControllerVibration() { return GetSettings().vibration; }

void SetControllerVibration(bool enabled) {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    {
        std::lock_guard<std::mutex> lock(rt.mtx);
        rt.current.vibration = enabled;
        rt.current.has_saved_values = true;
        rt.last_saved = rt.current;
    }
    rt.host_to_guest_dirty.store(true, std::memory_order_release);
}

uint32_t GetControllerScheme() { return GetSettings().control_scheme; }

void SetControllerScheme(uint32_t scheme) {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    const uint32_t clamped = (scheme <= 4u) ? scheme : 0u;
    {
        std::lock_guard<std::mutex> lock(rt.mtx);
        rt.current.control_scheme = clamped;
        rt.current.has_saved_values = true;
        rt.last_saved = rt.current;
    }
    rt.host_to_guest_dirty.store(true, std::memory_order_release);
}

float GetControllerTurnSensitivity() { return GetSettings().turn_sensitivity; }

void SetControllerTurnSensitivity(float sens) {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    const float clamped = std::isfinite(sens) ? std::clamp(sens, 0.25f, 4.0f) : 1.0f;
    {
        std::lock_guard<std::mutex> lock(rt.mtx);
        rt.current.turn_sensitivity = clamped;
        rt.current.has_saved_values = true;
        rt.last_saved = rt.current;
    }
    rt.host_to_guest_dirty.store(true, std::memory_order_release);
}

float GetControllerPitchSensitivity() { return GetSettings().pitch_sensitivity; }

void SetControllerPitchSensitivity(float sens) {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    const float clamped = std::isfinite(sens) ? std::clamp(sens, 0.25f, 4.0f) : 1.0f;
    {
        std::lock_guard<std::mutex> lock(rt.mtx);
        rt.current.pitch_sensitivity = clamped;
        rt.current.has_saved_values = true;
        rt.last_saved = rt.current;
    }
    rt.host_to_guest_dirty.store(true, std::memory_order_release);
}

bool GetSubtitles() { return GetSettings().subtitles; }

void SetSubtitles(bool enabled) {
    GuestSettingsRuntimeState& rt = GuestSettingsRt();
    {
        std::lock_guard<std::mutex> lock(rt.mtx);
        rt.current.subtitles = enabled;
        rt.current.has_saved_values = true;
        rt.last_saved = rt.current;
    }
    rt.host_to_guest_dirty.store(true, std::memory_order_release);
}

}  // namespace suffering::guest_settings

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

// 2. CXboxInputDevice::Poll (__thiscall) — post-deadzone framerate-independent linear mouse injection
// and per-frame live host-to-guest CUserSettings synchronization (even in ControllerOnly mode).
X86_FUNC(sub_001B09A0) {
    if (!app::IsReplayOrAutotestActive()) {
        SyncHostSettingsToGuestIfDirty(ctx, base);
    }

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
    if (!app::IsReplayOrAutotestActive()) {
        SyncHostSettingsToGuestIfDirty(ctx, base);
    }
    const uint32_t ui_mgr = ctx.ecx;
    __imp__sub_0005F9A0(ctx, base);
    HandleMenuMousePostUpdate(ctx, base, ui_mgr);
}

// 7a. Host-persisted CUserSettings (0x002DB218) lifecycle & persistence hooks
// (strictly bypassed when app::IsReplayOrAutotestActive() is true so replay traces keep 0.0 drift):
//  - sub_00016CA0: CUserSettings::ResetDefaults (__thiscall, ret 0)
//  - sub_00016B60: CUserSettings::Apply(int apply_audio) (__thiscall, ret 4)
//  - sub_00016D00: CUserSettings::CopyFrom(const CUserSettings*) (__thiscall, ret 4)
//  - sub_00016F10: CUserSettings::LoadFromXboxSaveContainer(int slot, int load_audio) (__thiscall, ret 8)
X86_FUNC(sub_00016CA0) {
    const uint32_t self_va = ctx.ecx;
    __imp__sub_00016CA0(ctx, base);
    if (!app::IsReplayOrAutotestActive() && !g_applying_host_to_guest && self_va == kVaUserSettings &&
        ValidGuest(kVaUserSettings, 0x44u)) {
        const suffering::guest_settings::GuestUserSettings s = suffering::guest_settings::GetSettings();
        if (s.has_saved_values) {
            WriteSettingsToGuestRam(base, kVaUserSettings, s);
        }
    }
}

X86_FUNC(sub_00016B60) {
    const uint32_t self_va = ctx.ecx;
    if (!app::IsReplayOrAutotestActive() && !g_applying_host_to_guest && self_va == kVaUserSettings &&
        !g_boot_settings_initialized && ValidGuest(kVaUserSettings, 0x44u)) {
        g_boot_settings_initialized = true;
        GuestSettingsRuntimeState& rt = GuestSettingsRt();
        rt.host_to_guest_dirty.store(false, std::memory_order_release);

        bool applied_persisted = false;
        suffering::guest_settings::GuestUserSettings persisted{};
        {
            std::lock_guard<std::mutex> lock(rt.mtx);
            if (rt.current.has_saved_values) {
                persisted = rt.current;
                applied_persisted = true;
            } else {
                suffering::guest_settings::GuestUserSettings boot_snap =
                    ReadSettingsFromGuestRam(base, kVaUserSettings);
                boot_snap.has_saved_values = false;
                rt.current = boot_snap;
                rt.last_saved = boot_snap;
            }
        }
        if (applied_persisted) {
            WriteSettingsToGuestRam(base, kVaUserSettings, persisted);
            if (ValidGuest(ctx.esp + 0x04u, 4u)) {
                X86_MEM32(ctx.esp + 0x04u) = 1u;  // Ensure persisted audio bus volumes (+0x0C..+0x14) apply on boot
            }
            LOGI("settings: applied persisted [game_options] on boot (invert_pitch=%u, scheme=%u, vib=%u, sub=%u)",
                 persisted.invert_pitch ? 1u : 0u, persisted.control_scheme, persisted.vibration ? 1u : 0u,
                 persisted.subtitles ? 1u : 0u);
        }
    }

    __imp__sub_00016B60(ctx, base);

    if (!app::IsReplayOrAutotestActive() && !g_applying_host_to_guest && self_va == kVaUserSettings &&
        g_save_pending_after_drag) {
        FlushPendingDragSettingsSave();
    }
}

X86_FUNC(sub_00016D00) {
    const uint32_t dst_va = ctx.ecx;
    __imp__sub_00016D00(ctx, base);
    if (!app::IsReplayOrAutotestActive() && !g_applying_host_to_guest && dst_va == kVaUserSettings) {
        OnGuestSettingsCommittedFromMenu(base);
    }
}

X86_FUNC(sub_00016F10) {
    const uint32_t dst_va = ctx.ecx;
    const bool post_boot_explicit_load = g_boot_settings_initialized;
    __imp__sub_00016F10(ctx, base);
    if (!app::IsReplayOrAutotestActive() && !g_applying_host_to_guest && dst_va == kVaUserSettings &&
        ValidGuest(kVaUserSettings, 0x44u)) {
        if (!post_boot_explicit_load) {
            // Cold-boot LoadFromXboxSaveContainer(1, 0) at 0x000BFD3C: keep unlocked Archive/Prelude flags
            // (+0x44/+0x48/+0x50) from any Xbox container, while preserving host settings.toml preferences.
            const suffering::guest_settings::GuestUserSettings s = suffering::guest_settings::GetSettings();
            if (s.has_saved_values) {
                WriteSettingsToGuestRam(base, kVaUserSettings, s);
            }
        } else if (ctx.eax == 0u) {
            // Explicit user "Options -> Load Settings" in-game succeeded: mirror into settings.toml!
            OnGuestSettingsCommittedFromMenu(base);
        }
    }
}

// 7b. CMainMenuGui "Quit Game" lifecycle, layout, state/cross-fade sync, and activation hooks
// (strictly bypassed when app::IsReplayOrAutotestActive() is true so replay traces have 0.0 drift):
//  - sub_0006F2A0: CMainMenuGui::~CMainMenuGui (__thiscall, ret 0)
//  - sub_0006F450: CMainMenuGui::Layout (__thiscall, ret 0, returns 1 in eax once ready)
//  - sub_00071B00: CMainMenuGui::Update (__thiscall, ret 0)
//  - sub_00072120: CMainMenuGui::Notify (__thiscall, ret 4, ecx = menu + 0x60, [esp+4] = sender)
X86_FUNC(sub_0006F2A0) {
    const uint32_t menu_va = ctx.ecx;
    if (menu_va != 0u && menu_va == g_main_menu_quit.menu_va) {
        g_main_menu_quit.Clear();
    }
    __imp__sub_0006F2A0(ctx, base);
}

X86_FUNC(sub_0006F450) {
    const uint32_t menu_va = ctx.ecx;
    __imp__sub_0006F450(ctx, base);
    if (!app::IsReplayOrAutotestActive() && ctx.eax != 0u) {
        const uint32_t saved_eax = ctx.eax;
        EnsureAndSyncMainMenuQuitButton(ctx, base, menu_va, true);
        ctx.eax = saved_eax;
    }
}

X86_FUNC(sub_00071B00) {
    const uint32_t menu_va = ctx.ecx;
    __imp__sub_00071B00(ctx, base);
    if (!app::IsReplayOrAutotestActive()) {
        const uint32_t saved_eax = ctx.eax;
        EnsureAndSyncMainMenuQuitButton(ctx, base, menu_va, false);
        ctx.eax = saved_eax;
    }
}

X86_FUNC(sub_00072120) {
    const uint32_t listener_va = ctx.ecx;
    const uint32_t sender_va = ValidGuest(ctx.esp + 0x04u, 4u) ? X86_MEM32(ctx.esp + 0x04u) : 0u;
    __imp__sub_00072120(ctx, base);
    if (!app::IsReplayOrAutotestActive() && listener_va >= 0x60u &&
        (listener_va - 0x60u) == g_main_menu_quit.menu_va && sender_va != 0u &&
        sender_va == g_main_menu_quit.btn_va) {
        RequestCleanHostQuit();
    }
}

// 8. Sub-microsecond CClock (0x002DC724) lifecycle & tick hooks (NOT bypassed during replay/autotest):
// Replaces 1ms integer GetTickCount dt sawtooth (16/17/18ms at 60 FPS; 5/6/7ms at 165 FPS) with
// continuous steady_clock double->float dt, freezes g_ElapsedGameTime during world/streaming pause,
// matches TimeMgr's 0.10s max_dt clamp, and serializes exact total_s in CClock::SaveState.
X86_FUNC(sub_001A3170) {
    const uint32_t clock_va = ctx.ecx;
    __imp__sub_001A3170(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnCtor(base, clock_va);
}

X86_FUNC(sub_001A31A0) {
    const uint32_t clock_va = ctx.ecx;
    const uint32_t full_reset = X86_MEM32(ctx.esp + 0x04u);
    __imp__sub_001A31A0(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnReset(base, clock_va, full_reset != 0u);
}

X86_FUNC(sub_001A3210) {
    const uint32_t clock_va = ctx.ecx;
    __imp__sub_001A3210(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnPause(base, clock_va);
}

X86_FUNC(sub_001A3220) {
    const uint32_t clock_va = ctx.ecx;
    if (clock_va == kVaMainClock && g_main_clk.valid) {
        const float t = float(g_main_clk.total_s);
        X86_MEMF32(clock_va + 0x20u) = t;
        X86_MEMF32(clock_va + 0x24u) = t;
    }
    __imp__sub_001A3220(ctx, base);
    if (clock_va == kVaMainClock) g_main_clk.OnUnpause(base, clock_va);
}

X86_FUNC(sub_001A3320) {
    const uint32_t clock_va = ctx.ecx;
    if (clock_va != kVaMainClock || !g_main_clk.valid) {
        __imp__sub_001A3320(ctx, base);
        return;
    }

    // Zero [+0x2C] (time_scale) temporarily during SaveState so `(GetTickCount() - [+0x28]) * [+0x2C] + [+0x24]`
    // writes bit-exact `float(g_main_clk.total_s)` into both [+0x20] and the save stream without wall-clock drift.
    const float saved_scale = X86_MEMF32(clock_va + 0x2Cu);
    const float exact_total = float(g_main_clk.total_s);
    X86_MEMF32(clock_va + 0x20u) = exact_total;
    X86_MEMF32(clock_va + 0x24u) = exact_total;
    X86_MEMF32(clock_va + 0x2Cu) = 0.0f;
    __imp__sub_001A3320(ctx, base);
    X86_MEMF32(clock_va + 0x2Cu) = saved_scale;
    g_main_clk.SyncGuestClockAnchors(base, clock_va);
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

    const float guest_max_dt = X86_MEMF32(kVaMaxDt);
    const double max_dt_d =
        (std::isfinite(guest_max_dt) && guest_max_dt > 0.001f) ? std::min(double(guest_max_dt), 0.10) : 0.10;
    raw_dt = std::clamp(raw_dt, 0.0001, max_dt_d);

    const float scale = X86_MEMF32(clock_va + 0x2Cu);
    const double safe_scale = (std::isfinite(scale) && scale >= 0.0f) ? double(scale) : 1.0;
    const double dt_d = std::clamp(raw_dt * safe_scale, 0.0, max_dt_d);

    // Freeze g_ElapsedGameTime (0x002E0290) whenever EngineFrameTick skips World::Update
    // (both Pause Menu and Mid-Level Streaming Pause set bit 0 of 0x002E0274), while keeping
    // *out_dt_va (0x002E01D0) live so CGUI menu screens continue updating smoothly.
    const bool world_paused = (X86_MEM8(kVaPauseFlags) & 0x01u) != 0u;
    if (!world_paused) {
        g_main_clk.total_s += dt_d;
    }
    if (out_dt_va >= 0x10000u && mem::IsCommitted(out_dt_va)) {
        X86_MEMF32(out_dt_va) = float(dt_d);
    }
    g_main_clk.SyncGuestClockAnchors(base, clock_va);
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

// 10. SRSCResourceMgr::Acquire (__thiscall, ret 0x04) & MeshResource::EnsureLoaded (sub_00156CA0, __thiscall, ret 0x04)
// — cooperative yields for synchronous spin-wait callers.
// Async prefetch callers (e.g. 0x0016E6C8, sub_0015A420, sub_00156CA0(1)) return immediately untouched;
// callers that immediately spin on `([res + 0x0E] & 3) == 2` yield cooperatively to the SRSC streaming worker.
X86_FUNC(sub_001765E0) {
    const bool sync_spin_caller = ValidGuest(ctx.esp, 4u) && IsSyncSrscAcquireCallSite(ctx, base);
    __imp__sub_001765E0(ctx, base);
    if (sync_spin_caller) {
        WaitSrscResourceReady(base, ctx.eax);
    }
}

X86_FUNC(sub_00156CA0) {
    // When bAsync == 0 ([esp+0x04] == 0) and [this+0x68] was already cached from an earlier bAsync == 1
    // prefetch while still in SRSC state 2, sub_00156CA0 bypasses sub_0017B290/sub_001765E0 and jumps
    // straight to its spin loop at 0x00156D20. Yield cooperatively before entering __imp__sub_00156CA0.
    if (ValidGuest(ctx.esp + 0x04u, 4u) && X86_MEM32(ctx.esp + 0x04u) == 0u &&
        ValidGuest(ctx.ecx + 0x64u, 8u) && X86_MEM32(ctx.ecx + 0x64u) != 0u) {
        const uint32_t cached_res = X86_MEM32(ctx.ecx + 0x68u);
        if (cached_res != 0u) {
            WaitSrscResourceReady(base, cached_res);
        }
    }
    __imp__sub_00156CA0(ctx, base);
}

// 11. CSTOMPInstance::Start (__thiscall, ret 0) — main-thread SRSC + PostLoad/callback completion guard.
// When called from CSTOMPManager::CreateHandle on the main thread (0x00162C3E -> ret_eip 0x00162C43, zero locks held),
// wait until the SRSC worker has finished both loading (state != 2) and PostLoad + callback dispatch (bit 0x10 set
// at 0x001C2D36 whenever bit 0x04 is clear) so CSTOMPInstance::Start never races main-thread scene/camera/audio state
// from sub_001BB1A0 on multi-core hosts.
X86_FUNC(sub_001BAB90) {
    if (ValidGuest(ctx.esp, 4u) && X86_MEM32(ctx.esp) == 0x00162C43u && ValidGuest(ctx.ecx + 0x0Cu, 4u)) {
        const uint32_t res_va = X86_MEM32(ctx.ecx + 0x0Cu);
        if (ValidGuest(res_va, 0x10u) && X86_MEM32(res_va + 0x04u) != 0u) {
            volatile const uint16_t* flags_ptr = reinterpret_cast<volatile const uint16_t*>(base + res_va + 0x0Eu);
            constexpr uint32_t kMaxStompWaitIters = 100000u;  // ~5.0s bounded safety cap
            for (uint32_t spins = 0; spins < kMaxStompWaitIters; ++spins) {
                const uint16_t flags = *flags_ptr;
                const bool loading = (flags & 0x03u) == 0x02u;
                const bool postload_pending = ((flags & 0x04u) == 0u) && ((flags & 0x10u) == 0u);
                if (!loading && !postload_pending) break;
                if (spins < 16u) {
                    std::this_thread::yield();
                } else {
                    kernel::SleepHighResUs(50);
                }
            }
            std::atomic_thread_fence(std::memory_order_acquire);
        }
    }
    __imp__sub_001BAB90(ctx, base);
}

// 12. Surgical 2 Cellblocks Dallas (#2152) script race & existing-checkpoint softlock guards:
//  - Hook A (MessageRelay::Fire, sub_000D9AE0, __thiscall, ret 0x04): If Torque sprints ahead to STOMP #11777
//    (#4051) or Dallas routes via Door #2932 before crossing #4945 while Dallas is alive, enable one-shot #11966
//    so #4051 -> #11966 -> #12616 disables the "Dallas dead/absent" Timer #12614 and assigns FollowBehavior #4055.
//  - Hook B (Timer::Start, sub_001414C0, __thiscall, ret 0x04): Heals pre-existing v1.4.0 checkpoints where #4051
//    already fired while #11966 was disabled by disarming Timer #12614 ([+0x12] = 0) when Switch #4077 triggers it
//    while Dallas is alive (without re-firing #11966, which would overwrite Exit-Gate escape waypoints #4081..#4085).
X86_FUNC(sub_000D9AE0) {
    const uint32_t relay_va = ctx.ecx;
    if (ValidGuest(relay_va, 0x14u)) {
        const uint32_t obj_va = X86_MEM32(relay_va + 0x04u);
        if (ValidGuest(obj_va + 0xD0u, 4u) && X86_MEM32(obj_va + 0xD0u) == kIidCellblocksStompDoneRelay &&
            X86_MEM32(relay_va) == kVtMessageRelay && X86_MEM32(relay_va + 0x0Cu) != 0u &&
            X86_MEM32(relay_va + 0x10u) != 0u && IsCellblocks2Level(base) && IsDallasAlive(base)) {
            const uint32_t obj11966 = FindObjectByIid(base, kIidCellblocksDallasGateRelay);
            if (obj11966 != 0u) {
                const uint32_t r11966 = X86_MEM32(obj11966 + 0x60u);
                if (ValidGuest(r11966, 0x14u) && X86_MEM32(r11966) == kVtMessageRelay &&
                    X86_MEM32(r11966 + 0x0Cu) == 0u && int32_t(X86_MEM32(r11966 + 0x10u)) > 0) {
                    X86_MEM32(r11966 + 0x0Cu) = 1u;
                }
            }
        }
    }
    __imp__sub_000D9AE0(ctx, base);
}

X86_FUNC(sub_001414C0) {
    const uint32_t timer_va = ctx.ecx;
    if (ValidGuest(timer_va, 0x16u)) {
        const uint32_t obj_va = X86_MEM32(timer_va + 0x04u);
        if (ValidGuest(obj_va + 0xD0u, 4u) && X86_MEM32(obj_va + 0xD0u) == kIidCellblocksDallasDeadTimer &&
            X86_MEM32(timer_va) == kVtTimer && X86_MEM32(timer_va + 0x12u) != 0u &&
            IsCellblocks2Level(base) && IsDallasAlive(base)) {
            X86_MEM32(timer_va + 0x12u) = 0u;
        }
    }
    __imp__sub_001414C0(ctx, base);
}
