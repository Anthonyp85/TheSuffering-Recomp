// SPDX-License-Identifier: GPL-2.0-or-later
// Widescreen UI, FMV, HUD & Crosshair Aspect-Ratio hooks for The Suffering (USA) default.xbe.
//
// Hook ownership (strictly disjoint from hooks.cpp, cheats.cpp, stairs_fix.cpp):
//  - sub_00177BB0 : Shared 2D XYZRHW quad vertex builder (sole path for sub_00177E30 & sub_00177FB0)
//  - sub_0005DF40 : CUIMgr::Render (screen stack classification, pillarbox bars, 4:3 menu mouse sync)
//  - sub_00060320 : CGUI::Render (menu vs HUD vs crosshair transform scoping)
//  - sub_00076BE0 : 3D HUD item mesh render (aligns 3D HUD meshes with 4:3 HUD frames; restores memory)
//  - sub_001CEFB0 : MovieXBox::Update FMV draw (4:3 centered video; sidebars cleared in CUIMgr::Render)
//  - sub_00029590 / sub_000289C0 : Weapon reticle HUD widget creation/destruction tracking
//
// 3D post-FX (bloom sub_001A27C0/sub_001A2B90, motion blur sub_001A2F80, 3D billboards sub_0017BBA0 /
// sub_0019B8D0, occlusion queries sub_001C33F0), lens flares (sub_000CF300), full-screen flashes
// (sub_000CE860), and Screen Fader (sub_0005E040) execute outside CUIMgr::Render / RiotXmvPlayer and
// therefore automatically stay full 16:9.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "app/app.h"
#include "common/ui_aspect.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "xdk/hle.h"

X86_EXTERN_FUNC(__imp__sub_00177BB0);
X86_EXTERN_FUNC(__imp__sub_0005DF40);
X86_EXTERN_FUNC(__imp__sub_00060320);
X86_EXTERN_FUNC(__imp__sub_00076BE0);
X86_EXTERN_FUNC(__imp__sub_001CEFB0);
X86_EXTERN_FUNC(__imp__sub_00029590);
X86_EXTERN_FUNC(__imp__sub_000289C0);

namespace {

constexpr uint32_t kGuestRendererPtrVA = 0x002DC75Cu;
constexpr uint32_t kRendererFlagsOffset = 0x64u;
constexpr uint32_t kRendererFlagWidescreen = 0x00200000u;

// Known Riot Engine CGUI vtables in The Suffering (USA) default.xbe.
constexpr uint32_t kVtHudBase        = 0x00295D20u;
constexpr uint32_t kVtHudAmmo        = 0x00295DA0u;
constexpr uint32_t kVtMapView        = 0x00299B50u;
constexpr uint32_t kVtMapScreen      = 0x00299BC0u;
constexpr uint32_t kVtMapChild       = 0x00299C28u;
constexpr uint32_t kVtHudMeterBase   = 0x00299DD0u;
constexpr uint32_t kVtHudMeter       = 0x00299E58u;
constexpr uint32_t kVtAlertBox       = 0x0029A828u;
constexpr uint32_t kVtArchives       = 0x0029A898u;
constexpr uint32_t kVtAudio          = 0x0029A958u;
constexpr uint32_t kVtController     = 0x0029AA38u;
constexpr uint32_t kVtCopyright      = 0x0029ABB0u;
constexpr uint32_t kVtCredits        = 0x0029AC18u;
constexpr uint32_t kVtExtras         = 0x0029ACC8u;
constexpr uint32_t kVtGameFinished   = 0x0029AD88u;
constexpr uint32_t kVtHelpToast      = 0x0029ADF0u;
constexpr uint32_t kVtGameplay       = 0x0029AE70u;
constexpr uint32_t kVtQuickWeaponHud = 0x0029AF10u;
constexpr uint32_t kVtQuickItemHud   = 0x0029AFB8u;
constexpr uint32_t kVtCrossHud       = 0x0029B020u;
constexpr uint32_t kVtLevelSelect    = 0x0029B1A0u;
constexpr uint32_t kVtLoadingBar     = 0x0029B210u;
constexpr uint32_t kVtSaveDevice     = 0x0029B280u;
constexpr uint32_t kVtLoadSave       = 0x0029B2F0u;
constexpr uint32_t kVtMainMenu       = 0x0029B4F8u;
constexpr uint32_t kVtOpeningMovies  = 0x0029B560u;
constexpr uint32_t kVtOptions        = 0x0029B5F8u;
constexpr uint32_t kVtPause          = 0x0029B690u;
constexpr uint32_t kVtLoadingScreen  = 0x0029B6F8u;
constexpr uint32_t kVtScreenAdjust   = 0x0029B768u;
constexpr uint32_t kVtHud3DItem      = 0x0029B868u;
constexpr uint32_t kVtHudTexture     = 0x0029B910u;
constexpr uint32_t kVtHudMultiTex    = 0x0029B990u;
constexpr uint32_t kVtSubtitles      = 0x0029BA10u;
constexpr uint32_t kVtSimpleTextHud  = 0x0029BB30u;
constexpr uint32_t kVtLoadingOverlay = 0x0029ED00u;

inline bool ValidGuest(uint32_t va, uint32_t bytes = 4u) {
    if (kernel::g_base == nullptr || bytes == 0u || va < 0x00010000u) return false;
    const uint32_t last = va + (bytes - 1u);
    if (last < va) return false;
    return mem::IsCommitted(va) && (bytes == 1u || mem::IsCommitted(last));
}

inline bool IsHudTopLevelVtable(uint32_t vt) {
    switch (vt) {
        case kVtHudBase:
        case kVtHudAmmo:
        case kVtHudMeterBase:
        case kVtHudMeter:
        case kVtHud3DItem:
        case kVtHudTexture:
        case kVtHudMultiTex:
        case kVtSubtitles:
        case kVtSimpleTextHud:
        case kVtQuickWeaponHud:
        case kVtQuickItemHud:
        case kVtCrossHud:
        case kVtHelpToast:
            return true;
        default:
            return false;
    }
}

inline bool IsInteractiveMenuVtable(uint32_t vt) {
    switch (vt) {
        case kVtMainMenu:
        case kVtPause:
        case kVtOptions:
        case kVtAudio:
        case kVtGameplay:
        case kVtController:
        case kVtScreenAdjust:
        case kVtLoadSave:
        case kVtSaveDevice:
        case kVtAlertBox:
        case kVtArchives:
        case kVtMapScreen:
        case kVtMapView:
        case kVtMapChild:
        case kVtExtras:
        case kVtCredits:
        case kVtGameFinished:
        case kVtLevelSelect:
            return true;
        default:
            return false;
    }
}

// Screens that cover the full 640x480 frame with opaque 2D artwork/video and therefore need
// clean black pillarbox sidebars on [0..80] and [560..640].
inline bool IsOpaqueFullScreen2DVtable(uint32_t vt) {
    switch (vt) {
        case kVtOpeningMovies:
        case kVtCopyright:
        case kVtMainMenu:
        case kVtCredits:
        case kVtGameFinished:
        case kVtLoadingBar:
        case kVtLoadingScreen:
        case kVtLoadingOverlay:
        case kVtArchives:
        case kVtMapScreen:
        case kVtExtras:
        case kVtLevelSelect:
            return true;
        default:
            return false;
    }
}

inline bool IsWidescreenActive(uint8_t* base) {
    if (app::Widescreen()) return true;
    if (!ValidGuest(kGuestRendererPtrVA, 4)) return false;
    const uint32_t renderer = X86_MEM32(kGuestRendererPtrVA);
    if (!ValidGuest(renderer, kRendererFlagsOffset + 4)) return false;
    return (X86_MEM32(renderer + kRendererFlagsOffset) & kRendererFlagWidescreen) != 0;
}

// Preserve x87 FPU state around nested calls into guest/HLE functions.
struct SavedX87State {
    double st[8];
    int top;
    uint16_t fsw;
    uint16_t fcw;

    explicit SavedX87State(const X86Context& ctx)
        : top(ctx.top), fsw(ctx.fsw), fcw(ctx.fcw) {
        std::memcpy(st, ctx.st, sizeof(st));
    }
    void Restore(X86Context& ctx) const {
        std::memcpy(ctx.st, st, sizeof(st));
        ctx.top = top;
        ctx.fsw = fsw;
        ctx.fcw = fcw;
    }
};

enum class QuadTransform : uint8_t {
    None = 0,
    Pillarbox43 = 1,
    LocalScale075 = 2,
};

thread_local QuadTransform g_quad_transform = QuadTransform::None;
thread_local int g_cgui_depth = 0;
thread_local int g_menu_screen_depth = 0;
thread_local bool g_in_ui_mgr_render = false;
thread_local bool g_ui_overlay_over_3d = false;

// Small fixed guest-thread set of active weapon reticle HUD widget VAs ([weapon+0x19A], [weapon+0x19E]).
constexpr size_t kMaxReticleWidgets = 32;
thread_local std::array<uint32_t, kMaxReticleWidgets> g_reticle_widgets{};
thread_local size_t g_reticle_count = 0;

void RegisterReticleWidget(uint32_t va) {
    if (!ValidGuest(va, 0x48)) return;
    for (size_t i = 0; i < g_reticle_count; ++i) {
        if (g_reticle_widgets[i] == va) return;
    }
    if (g_reticle_count < kMaxReticleWidgets) {
        g_reticle_widgets[g_reticle_count++] = va;
    } else {
        // Evict oldest slot if a long play session somehow accumulated orphaned pointers.
        for (size_t i = 1; i < kMaxReticleWidgets; ++i) {
            g_reticle_widgets[i - 1] = g_reticle_widgets[i];
        }
        g_reticle_widgets[kMaxReticleWidgets - 1] = va;
    }
}

void UnregisterReticleWidget(uint32_t va) {
    if (!va) return;
    for (size_t i = 0; i < g_reticle_count; ++i) {
        if (g_reticle_widgets[i] == va) {
            g_reticle_widgets[i] = g_reticle_widgets[--g_reticle_count];
            return;
        }
    }
}

bool IsTrackedReticleWidget(uint32_t va) {
    for (size_t i = 0; i < g_reticle_count; ++i) {
        if (g_reticle_widgets[i] == va) return true;
    }
    return false;
}

bool IsCrosshairWidget(uint8_t* base, uint32_t widget_va, uint32_t vt) {
    if (IsTrackedReticleWidget(widget_va)) return true;
    // Fallback for any weapon/turret/first-person reticle HUD texture centered near (320, 240)
    // while rendering gameplay HUD (never fires inside menus).
    if (g_menu_screen_depth > 0) return false;
    if (vt != kVtHudTexture && vt != kVtHudMultiTex) return false;
    if (!ValidGuest(widget_va, 0x48)) return false;
    const float x = X86_MEMF32(widget_va + 0x38u);
    const float y = X86_MEMF32(widget_va + 0x3Cu);
    const float w = X86_MEMF32(widget_va + 0x40u);
    const float h = X86_MEMF32(widget_va + 0x44u);
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)) return false;
    if (w <= 1.0f || h <= 1.0f || w > 128.0f || h > 128.0f) return false;
    const float cx = x + w * 0.5f;
    const float cy = y + h * 0.5f;
    return std::fabs(cx - 320.0f) <= 24.0f && std::fabs(cy - 240.0f) <= 40.0f;
}

struct ScopedQuadTransform {
    QuadTransform prev;
    explicit ScopedQuadTransform(QuadTransform next) : prev(g_quad_transform) { g_quad_transform = next; }
    ~ScopedQuadTransform() { g_quad_transform = prev; }
};

struct ScopedIntInc {
    int& ref;
    bool active;
    ScopedIntInc(int& r, bool enable = true) : ref(r), active(enable) {
        if (active) ++ref;
    }
    ~ScopedIntInc() {
        if (active) --ref;
    }
};

struct ScopedGuestU32Restore {
    uint8_t* base;
    uint32_t va1;
    uint32_t orig1;
    uint32_t va2;
    uint32_t orig2;
    bool active;

    ScopedGuestU32Restore(uint8_t* b, uint32_t a1, uint32_t a2, bool enable)
        : base(b), va1(a1), orig1(0), va2(a2), orig2(0), active(enable) {
        if (active) {
            orig1 = X86_MEM32(va1);
            orig2 = X86_MEM32(va2);
        }
    }
    ~ScopedGuestU32Restore() {
        if (active) {
            X86_MEM32(va1) = orig1;
            X86_MEM32(va2) = orig2;
        }
    }
};

constexpr uint32_t kFnD3DDeviceClearVA = 0x00209ED0u;

// Clear left [0..80] and right [560..640] pillarbox sidebars to opaque black (color-only)
// via HLE D3DDevice_Clear (works identically on Windows D3D12 and Linux Vulkan).
void ClearPillarboxSidebars(X86Context& ctx, uint8_t* base) {
    if (ctx.esp < 0x00010200u || !ValidGuest(ctx.esp - 128u, 128u)) return;

    const SavedX87State saved_x87(ctx);
    const uint32_t saved_eax = ctx.eax;
    const uint32_t saved_ecx = ctx.ecx;
    const uint32_t saved_edx = ctx.edx;
    const uint32_t saved_esp = ctx.esp;

    // Reserve two 16-byte D3DRECT structs on the guest stack below a small safety gap.
    ctx.esp -= 48u;
    const uint32_t rects_va = ctx.esp;
    // Left pillarbox bar: [0, 0, 80, 480]
    X86_MEM32(rects_va + 0u) = 0u;
    X86_MEM32(rects_va + 4u) = 0u;
    X86_MEM32(rects_va + 8u) = 80u;
    X86_MEM32(rects_va + 12u) = 480u;
    // Right pillarbox bar: [560, 0, 640, 480]
    X86_MEM32(rects_va + 16u) = 560u;
    X86_MEM32(rects_va + 20u) = 0u;
    X86_MEM32(rects_va + 24u) = 640u;
    X86_MEM32(rects_va + 28u) = 480u;

    // D3DDevice_Clear(Count, pRects, Flags, Color, Z, Stencil) — stdcall via kernel::CallGuest.
    kernel::CallGuest(ctx, kFnD3DDeviceClearVA,
                      {2u, rects_va, 0x000000F0u, 0xFF000000u, 0x3F800000u, 0u});

    ctx.esp = saved_esp;
    ctx.eax = saved_eax;
    ctx.ecx = saved_ecx;
    ctx.edx = saved_edx;
    saved_x87.Restore(ctx);
}

}  // namespace

// Shared 2D XYZRHW quad vertex builder (called exclusively by sub_00177E30 & sub_00177FB0).
X86_FUNC(sub_00177BB0) {
    const QuadTransform xf = g_quad_transform;
    uint32_t out_verts = 0;
    if (xf != QuadTransform::None && ValidGuest(ctx.esp + 4u, 4u)) {
        out_verts = X86_MEM32(ctx.esp + 4u);
    }

    __imp__sub_00177BB0(ctx, base);

    if (xf == QuadTransform::None || !ValidGuest(out_verts, 0x80u)) {
        return;
    }

    float xs[4];
    float ys[4];
    for (uint32_t i = 0; i < 4u; ++i) {
        xs[i] = X86_MEMF32(out_verts + i * 0x20u);
        ys[i] = X86_MEMF32(out_verts + i * 0x20u + 4u);
        if (!std::isfinite(xs[i]) || !std::isfinite(ys[i])) return;
    }

    const float min_x = std::min({xs[0], xs[1], xs[2], xs[3]});
    const float max_x = std::max({xs[0], xs[1], xs[2], xs[3]});
    const float min_y = std::min({ys[0], ys[1], ys[2], ys[3]});
    const float max_y = std::max({ys[0], ys[1], ys[2], ys[3]});

    if (xf == QuadTransform::Pillarbox43) {
        // When an overlay menu (such as Pause) renders over the live 16:9 3D world, keep full-screen
        // semi-transparent dark dim quads spanning full 16:9 so the 3D background dims uniformly.
        if (g_ui_overlay_over_3d && min_x <= 2.0f && max_x >= 638.0f && min_y <= 2.0f && max_y >= 478.0f) {
            const uint32_t color0 = X86_MEM32(out_verts + 0x10u);
            const uint8_t alpha = static_cast<uint8_t>((color0 >> 24) & 0xFFu);
            if (alpha > 0u && alpha < 250u) {
                return;
            }
        }
        for (uint32_t i = 0; i < 4u; ++i) {
            X86_MEMF32(out_verts + i * 0x20u) = suffering::ui_aspect::MapGuestXToPillarbox43(xs[i]);
        }
    } else if (xf == QuadTransform::LocalScale075) {
        const float cx = 0.5f * (min_x + max_x);
        for (uint32_t i = 0; i < 4u; ++i) {
            X86_MEMF32(out_verts + i * 0x20u) = suffering::ui_aspect::MapQuadXLocalScale075(xs[i], cx);
        }
    }
}

// CUIMgr::Render — inspects active screen stack, clears pillarbox bars on opaque 2D screens,
// and publishes 4:3 menu pillarbox state for PC menu mouse coordinate mapping.
X86_FUNC(sub_0005DF40) {
    const uint32_t ui_mgr = ctx.ecx;
    const bool wide = IsWidescreenActive(base);
    const auto mode = suffering::ui_aspect::GetMode();
    const bool pillarbox_menus = wide && suffering::ui_aspect::ShouldPillarboxMenus(mode);

    bool any_menu_active = false;
    bool has_opaque_2d_screen = false;
    bool has_main_menu_in_stack = false;
    bool has_pause_or_hud = false;

    if (ValidGuest(ui_mgr, 0x54u)) {
        const uint32_t arr = X86_MEM32(ui_mgr + 0x48u);
        const int32_t count = static_cast<int32_t>(X86_MEM32(ui_mgr + 0x50u));
        if (count > 0 && count <= 128 && ValidGuest(arr, static_cast<uint32_t>(count) * 4u)) {
            for (int32_t i = 0; i < count; ++i) {
                const uint32_t child = X86_MEM32(arr + static_cast<uint32_t>(i) * 4u);
                if (!ValidGuest(child, 0x28u)) continue;
                const uint32_t vt = X86_MEM32(child);
                const int32_t state = static_cast<int32_t>(X86_MEM32(child + 0x24u));
                if (vt == kVtMainMenu) has_main_menu_in_stack = true;
                if (state <= 2) continue;  // hidden / inactive screen (matches CUIMgr::Render line 0005DF87)

                if (IsInteractiveMenuVtable(vt)) any_menu_active = true;
                if (IsOpaqueFullScreen2DVtable(vt)) has_opaque_2d_screen = true;
                if (vt == kVtPause || IsHudTopLevelVtable(vt)) has_pause_or_hud = true;
            }
        }
    }

    // Main-menu-rooted submenus (Options/Audio/Controller/LoadSave/AlertBox from Main Menu) have no
    // live 3D game behind them and need opaque black pillarbox sidebars.
    const bool need_opaque_sidebars =
        pillarbox_menus && (has_opaque_2d_screen || (any_menu_active && has_main_menu_in_stack && !has_pause_or_hud));
    const bool overlay_over_3d = pillarbox_menus && any_menu_active && !need_opaque_sidebars;

    suffering::ui_aspect::SetMenuPillarboxed43Active(pillarbox_menus && any_menu_active);

    if (need_opaque_sidebars) {
        ClearPillarboxSidebars(ctx, base);
    }

    const bool prev_in_ui = g_in_ui_mgr_render;
    const bool prev_overlay = g_ui_overlay_over_3d;
    g_in_ui_mgr_render = true;
    g_ui_overlay_over_3d = overlay_over_3d;
    try {
        __imp__sub_0005DF40(ctx, base);
    } catch (...) {
        g_in_ui_mgr_render = prev_in_ui;
        g_ui_overlay_over_3d = prev_overlay;
        throw;
    }
    g_in_ui_mgr_render = prev_in_ui;
    g_ui_overlay_over_3d = prev_overlay;
}

// CGUI::Render — scopes QuadTransform per top-level screen (Menu vs Gameplay HUD) and per Crosshair widget.
X86_FUNC(sub_00060320) {
    const uint32_t widget = ctx.ecx;
    const bool wide = IsWidescreenActive(base);
    const auto mode = suffering::ui_aspect::GetMode();

    if (!wide || mode == suffering::ui_aspect::Mode::Stretched16x9 || !ValidGuest(widget, 4u)) {
        __imp__sub_00060320(ctx, base);
        return;
    }

    const uint32_t vt = X86_MEM32(widget);
    const bool is_top_level = (g_cgui_depth == 0);
    const bool is_hud_root = is_top_level && IsHudTopLevelVtable(vt);
    const bool is_menu_root = is_top_level && !is_hud_root;

    ScopedIntInc depth_guard(g_cgui_depth, true);
    ScopedIntInc menu_guard(g_menu_screen_depth, is_menu_root);

    QuadTransform next_xf = g_quad_transform;
    if (IsCrosshairWidget(base, widget, vt) && suffering::ui_aspect::ShouldCorrectCrosshair(mode)) {
        // Crosshair / Aim Reticle: 1:1 unstretched local width centered on true aim coordinate.
        next_xf = QuadTransform::LocalScale075;
    } else if (is_menu_root) {
        next_xf = suffering::ui_aspect::ShouldPillarboxMenus(mode) ? QuadTransform::Pillarbox43 : QuadTransform::None;
    } else if (is_hud_root) {
        next_xf = suffering::ui_aspect::ShouldPillarboxHud(mode) ? QuadTransform::Pillarbox43 : QuadTransform::None;
    }

    ScopedQuadTransform xf_guard(next_xf);
    __imp__sub_00060320(ctx, base);
}

// 3D HUD Item Mesh (vt 0x0029B868, e.g. Quick-Weapon / Quick-Item 3D weapon & bottle models).
// Temporarily maps widget rect [x, w] into 4:3 pillarbox space while rendering, then restores exact bits.
X86_FUNC(sub_00076BE0) {
    const uint32_t widget = ctx.ecx;
    const bool wide = IsWidescreenActive(base);
    const auto mode = suffering::ui_aspect::GetMode();
    const bool pillarbox_this =
        wide && ((g_menu_screen_depth > 0) ? suffering::ui_aspect::ShouldPillarboxMenus(mode)
                                           : suffering::ui_aspect::ShouldPillarboxHud(mode));

    const bool can_patch = pillarbox_this && ValidGuest(widget, 0x48u);
    ScopedGuestU32Restore restore(base, widget + 0x38u, widget + 0x40u, can_patch);
    if (can_patch) {
        const float orig_x = X86_MEMF32(widget + 0x38u);
        const float orig_w = X86_MEMF32(widget + 0x40u);
        if (std::isfinite(orig_x) && std::isfinite(orig_w)) {
            X86_MEMF32(widget + 0x38u) = suffering::ui_aspect::MapGuestXToPillarbox43(orig_x);
            X86_MEMF32(widget + 0x40u) = orig_w * suffering::ui_aspect::kPillarboxScaleX;
        }
    }
    __imp__sub_00076BE0(ctx, base);
}

// MovieXBox::Update — maps XMV playback quads into the 4:3 pillarbox region while leaving
// sidebar clearing to CUIMgr::Render so XMV frame polling stays untouched.
X86_FUNC(sub_001CEFB0) {
    const bool wide = IsWidescreenActive(base);
    const auto mode = suffering::ui_aspect::GetMode();
    const bool active = wide && suffering::ui_aspect::ShouldPillarboxMenus(mode);
    ScopedQuadTransform scope(active ? QuadTransform::Pillarbox43 : g_quad_transform);
    __imp__sub_001CEFB0(ctx, base);
}

// Weapon reticle HUD creation (sub_00029590) & teardown (sub_000289C0):
// tracks [weapon+0x19A] (normal reticle) and [weapon+0x19E] (locked-on reticle).
X86_FUNC(sub_00029590) {
    const uint32_t weapon = ctx.ecx;
    __imp__sub_00029590(ctx, base);
    if (ValidGuest(weapon, 0x1A6u)) {
        RegisterReticleWidget(X86_MEM32(weapon + 0x19Au));
        RegisterReticleWidget(X86_MEM32(weapon + 0x19Eu));
    }
}

X86_FUNC(sub_000289C0) {
    const uint32_t weapon = ctx.ecx;
    if (ValidGuest(weapon, 0x1A6u)) {
        UnregisterReticleWidget(X86_MEM32(weapon + 0x19Au));
        UnregisterReticleWidget(X86_MEM32(weapon + 0x19Eu));
    }
    __imp__sub_000289C0(ctx, base);
}
