// SPDX-License-Identifier: GPL-2.0-or-later
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

#include "common/file_browser_ui.h"
#include "common/input_recorder.h"
#include "common/mkb_input.h"
#include "common/settings_ui.h"
#include "common/ui_common.h"
#include "common/update_checker.h"
#include "common/version.h"
#include "game/cheats.h"
#include "game/guest_settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "upscale/upscaler_manager.h"
#include "xdk/d3d8.h"

namespace suffering {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr UINT kMsgUiAction = WM_APP + 20;
#else
constexpr int32_t kSdlUserUiAction = 0x53554601;
#endif

enum class UiAction : uintptr_t {
    RefreshCursor = 1,
    RelocateXiso = 2,
    OpenSavesDir = 3,
    OpenLogsDir = 4,
    OpenCrashDir = 5,
    CreateShortcut = 6,
    OpenPluginsDir = 7,
    OpenReleaseUrl = 8,
    QuitForStagedUpdate = 9,
};

constexpr int kSettingsTabCount = 6;
constexpr int kTabCheats = 5;

std::atomic<bool> g_overlay_open{false};
bool g_was_pad_active = false;
bool g_was_overlay_open_ui = false;
bool g_prev_pad_l1 = false;
bool g_prev_pad_r1 = false;
std::atomic<int> g_requested_tab{-1};
std::atomic<int> g_selected_tab{0};
#ifndef _WIN32
XisoFileBrowser g_file_browser;
#endif

// Raw pad snapshot taken on guest thread while overlay is open (fed to ImGui on render thread)
std::mutex g_pad_lock;
input::Pad g_ui_pad{};
bool g_chord_held = false;

// Rumble test pulse state
std::atomic<bool> g_rumble_test_active{false};
std::chrono::steady_clock::time_point g_rumble_test_until{};

// Status banner updated from STA main thread actions, read on render thread
std::mutex g_status_lock;
std::string g_status_banner;
bool g_status_is_error = false;
std::string g_pending_release_url;

// In-game background update notification toast + manual check tracking
std::string g_update_toast_tag;
uint64_t g_update_toast_start_ms = 0;
bool g_update_toast_dismissed_by_overlay = false;
bool g_ingame_upd_manual_pending = false;
UpdateState g_ingame_prev_upd_state = UpdateState::Idle;

struct UpdateCheckerProcessExitGuard {
    ~UpdateCheckerProcessExitGuard() { ShutdownUpdateChecker(); }
} g_update_exit_guard;

// Automated UI screenshot capture CLI/env support (--open-settings, --settings-tab N, --capture-ui-bmp, --capture-ui-exit)
fs::path g_capture_ui_path;
bool g_capture_ui_exit = false;
bool g_suppress_warmup_toast = false;
bool g_force_fps_hud_cli = false;
uint32_t g_ui_frames_drawn = 0;
bool g_capture_ui_requested = false;
bool g_capture_ui_exit_posted = false;

// Cached adapter/monitor/resolution/plugin lists refreshed when overlay opens
std::vector<app::GpuAdapterInfo> g_gpus;
std::vector<app::MonitorInfo> g_monitors;
std::vector<app::Mode> g_resolutions;
std::string g_res_device;
upscale::PluginAvailability g_plugins{};
bool g_lists_inited = false;

// Baked font scale + dynamic style scale tracking
UiFonts g_fonts{};
float g_applied_style_scale = 0.0f;

// Rolling frame-time ring buffer for FPS / 1% frame-time HUD
constexpr size_t kHudRing = 120;
std::array<float, kHudRing> g_ft_ring{};
size_t g_ft_pos = 0;
size_t g_ft_count = 0;

#ifdef _WIN32
std::vector<std::wstring> SplitArgs(const wchar_t* cmd) {
    std::vector<std::wstring> args;
    if (!cmd) return args;
    const wchar_t* p = cmd;
    while (*p) {
        while (*p && std::iswspace(*p)) ++p;
        if (!*p) break;
        std::wstring a;
        bool q = false;
        while (*p && (q || !std::iswspace(*p))) {
            if (*p == L'"') q = !q;
            else a.push_back(*p);
            ++p;
        }
        args.push_back(std::move(a));
    }
    return args;
}
#endif

void SetStatus(const std::string& msg, bool err = false) {
    std::lock_guard<std::mutex> lock(g_status_lock);
    g_status_banner = msg;
    g_status_is_error = err;
}

void PostAction(UiAction act) {
#ifdef _WIN32
    if (HWND hwnd = static_cast<HWND>(app::Window())) {
        PostMessageW(hwnd, kMsgUiAction, static_cast<WPARAM>(act), 0);
    }
#else
    SDL_Event ev{};
    ev.type = SDL_EVENT_USER;
    ev.user.code = kSdlUserUiAction;
    ev.user.data1 = reinterpret_cast<void*>(act);
    SDL_PushEvent(&ev);
#endif
}

void SyncImGuiCursorFlag(bool overlay_open) {
    std::lock_guard<std::recursive_mutex> lock(app::UiLock());
    if (!ImGui::GetCurrentContext()) return;
    ImGuiIO& io = ImGui::GetIO();
    if (overlay_open) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
}

void ClearCachedUiPad() {
    std::lock_guard<std::mutex> lock(g_pad_lock);
    g_ui_pad = input::Pad{};
}

void UpdateMainThreadCursor(void* hwnd) {
    bool open = g_overlay_open.load(std::memory_order_acquire);
    SyncImGuiCursorFlag(open);
    if (!open) {
        ClearCachedUiPad();
#ifndef _WIN32
        g_file_browser.Close();
#endif
    }
    MkbOnOverlayToggled(open, hwnd);
}

void ApplyRadialDeadzone(int16_t& sx, int16_t& sy, uint32_t dz_pct) {
    if (dz_pct == 0) return;
    float fx = float(sx) / 32767.0f;
    float fy = float(sy) / 32767.0f;
    float mag = std::sqrt(fx * fx + fy * fy);
    float dz = std::clamp(float(dz_pct) / 100.0f, 0.0f, 0.45f);
    if (mag <= dz) {
        sx = 0;
        sy = 0;
        return;
    }
    float scaled = std::min((mag - dz) / (1.0f - dz), 1.0f);
    float ratio = scaled / mag;
    sx = int16_t(std::clamp(fx * ratio * 32767.0f, -32767.0f, 32767.0f));
    sy = int16_t(std::clamp(fy * ratio * 32767.0f, -32767.0f, 32767.0f));
}

float ComputeInGameUiScale() {
    ImGuiIO& io = ImGui::GetIO();
    float dpi_s = ComputeWindowDpiScale(app::Window());
    float h_s = io.DisplaySize.y / 840.0f;
    return std::clamp(std::max(dpi_s, h_s), 1.0f, 2.5f);
}

void EnsureInGameStyle(float ui_scale) {
    if (std::fabs(ui_scale - g_applied_style_scale) > 0.05f) {
        ApplyDarkIndustrialStyle(ui_scale);
        g_applied_style_scale = ui_scale;
    }
    ImGuiIO& io = ImGui::GetIO();
    float base = g_fonts.scale > 0.1f ? g_fonts.scale : 1.0f;
    io.FontGlobalScale = std::clamp(ui_scale / base, 0.75f, 2.5f);
}

void DrawStatusHud(double fps, float ui_scale, bool show_fps, bool recording) {
    float dt_ms = ImGui::GetIO().DeltaTime * 1000.0f;
    if (dt_ms > 0.0f && dt_ms < 2000.0f) {
        g_ft_ring[g_ft_pos] = dt_ms;
        g_ft_pos = (g_ft_pos + 1) % kHudRing;
        if (g_ft_count < kHudRing) g_ft_count++;
    }
    float p99_ms = dt_ms;
    if (g_ft_count > 10) {
        std::array<float, kHudRing> tmp = g_ft_ring;
        std::sort(tmp.begin(), tmp.begin() + g_ft_count);
        size_t idx = std::min(g_ft_count - 1, (g_ft_count * 99) / 100);
        p99_ms = tmp[idx];
    }

    ImGui::SetNextWindowPos(ImVec2(12.0f * ui_scale, 12.0f * ui_scale), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.72f);
    constexpr ImGuiWindowFlags kHudFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##SufferingFpsHud", nullptr, kHudFlags)) {
        if (show_fps) {
            gpu::UpscalerStatus us = gpu::GetUpscalerStatus();
            if (us.render_w > 0 && us.vp_w > 0) {
                ImGui::Text("FPS: %.1f | %.2f ms (p99: %.2f) | %s %ux%u -> %ux%u",
                            fps > 0.0 ? fps : (dt_ms > 0.0f ? 1000.0 / dt_ms : 0.0), dt_ms, p99_ms,
                            us.active_label.empty() ? "FSR 1.0" : us.active_label.c_str(), us.render_w, us.render_h,
                            us.vp_w, us.vp_h);
            } else {
                app::Mode out = app::Output();
                ImGui::Text("FPS: %.1f | %.2f ms (p99: %.2f) | %ux%u",
                            fps > 0.0 ? fps : (dt_ms > 0.0f ? 1000.0 / dt_ms : 0.0), dt_ms, p99_ms, out.width, out.height);
            }
        }
        if (recording) {
            ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.35f, 1.0f), "Recording: controller input only");
        }
    }
    ImGui::End();
}

bool ShouldShowShaderWarmupToast(gpu::ShaderWarmupStatus& out_ws, float& out_alpha) {
    if (g_suppress_warmup_toast || !g_capture_ui_path.empty() || app::IsReplayOrAutotestActive()) return false;
    out_ws = gpu::GetShaderWarmupStatus();
    if (out_ws.active) {
        out_alpha = 1.0f;
        return true;
    }
    if (!out_ws.completed || out_ws.completed_ms == 0 || (out_ws.cached_shaders == 0 && out_ws.compiled_new == 0)) {
        return false;
    }
    uint64_t now_ms = uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (now_ms < out_ws.completed_ms) return false;
    uint64_t age_ms = now_ms - out_ws.completed_ms;
    constexpr uint64_t kShowDurationMs = 3500;
    constexpr uint64_t kFadeStartMs = 2750;
    if (age_ms >= kShowDurationMs) return false;
    out_alpha = (age_ms <= kFadeStartMs)
                    ? 1.0f
                    : std::clamp(float(kShowDurationMs - age_ms) / float(kShowDurationMs - kFadeStartMs), 0.0f, 1.0f);
    return out_alpha > 0.02f;
}

void DrawShaderWarmupToast(const gpu::ShaderWarmupStatus& ws, float alpha, float ui_scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f * ui_scale, io.DisplaySize.y - 16.0f * ui_scale),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.82f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    constexpr ImGuiWindowFlags kToastFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##SufferingShaderWarmupToast", nullptr, kToastFlags)) {
        if (ws.active) {
            uint32_t total = std::max(ws.total, 1u);
            uint32_t done = std::min(ws.done, total);
            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.35f, 1.0f), "Compiling & Caching Shaders... (%u / %u)", done,
                               total);
            float frac = float(done) / float(total);
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.76f, 0.20f, 0.22f, 1.0f));
            ImGui::ProgressBar(frac, ImVec2(230.0f * ui_scale, 5.0f * ui_scale), "");
            ImGui::PopStyleColor();
        } else if (ws.compiled_new > 0) {
            ImGui::TextColored(ImVec4(0.52f, 0.92f, 0.60f, 1.0f),
                               "Shader Cache Ready: %u compiled | %u Shaders, %u PSOs (%.0f ms)", ws.compiled_new,
                               ws.cached_shaders, ws.cached_psos, ws.elapsed_ms);
        } else {
            ImGui::TextColored(ImVec4(0.52f, 0.92f, 0.60f, 1.0f),
                               "Shader Cache Loaded: %u Shaders | %u PSOs (%.0f ms)", ws.cached_shaders,
                               ws.cached_psos, ws.elapsed_ms);
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();
}

bool ShouldShowUpdateAvailableToast(const UpdateStatusSnapshot& snap, const std::string& dismissed_tag,
                                    bool overlay_open, float& out_alpha) {
    if (g_suppress_warmup_toast || !g_capture_ui_path.empty() || app::IsReplayOrAutotestActive()) return false;
    if (snap.state != UpdateState::UpdateAvailable || snap.manual_check || snap.latest_tag.empty()) return false;
    if (!dismissed_tag.empty() && snap.latest_tag == dismissed_tag) return false;

    uint64_t now_ms = uint64_t(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
    if (g_update_toast_tag != snap.latest_tag) {
        g_update_toast_tag = snap.latest_tag;
        g_update_toast_start_ms = now_ms;
        g_update_toast_dismissed_by_overlay = false;
    }
    if (overlay_open) {
        g_update_toast_dismissed_by_overlay = true;
        return false;
    }
    if (g_update_toast_dismissed_by_overlay || now_ms < g_update_toast_start_ms) return false;
    uint64_t age_ms = now_ms - g_update_toast_start_ms;
    constexpr uint64_t kShowDurationMs = 6000;
    constexpr uint64_t kFadeStartMs = 5100;
    if (age_ms >= kShowDurationMs) return false;
    out_alpha = (age_ms <= kFadeStartMs)
                    ? 1.0f
                    : std::clamp(float(kShowDurationMs - age_ms) / float(kShowDurationMs - kFadeStartMs), 0.0f, 1.0f);
    return out_alpha > 0.02f;
}

void DrawUpdateAvailableToast(const UpdateStatusSnapshot& snap, float alpha, float ui_scale) {
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f * ui_scale, 16.0f * ui_scale), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f * alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.15f, 0.115f, 0.068f, 0.94f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.94f, 0.70f, 0.22f, 0.92f));
    constexpr ImGuiWindowFlags kToastFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##SufferingUpdateAvailableToast", nullptr, kToastFlags)) {
        ImGui::TextColored(ImVec4(1.00f, 0.84f, 0.36f, 1.0f),
                           "Update %s available - press F1 -> Launcher & Diagnostics", snap.latest_tag.c_str());
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

bool ShouldShowActiveCheatsHud(const SharedUiOptions& o, uint32_t& out_active_count) {
    out_active_count = 0;
    if (!o.show_cheats_hud) return false;
    if (g_suppress_warmup_toast || !g_capture_ui_path.empty() || IsPadOnlyInputSession()) return false;
    uint32_t count = cheats::GetActiveToggleCount();
    if (count == 0) return false;
    out_active_count = count;
    return true;
}

void DrawActiveCheatsHud(uint32_t active_count, float ui_scale) {
    ImGuiIO& io = ImGui::GetIO();
    float s = std::clamp(ui_scale, 1.0f, 2.5f);
    ImGui::SetNextWindowPos(ImVec2(14.0f * s, io.DisplaySize.y - 14.0f * s), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.76f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 4.5f * s));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.11f, 0.068f, 0.078f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.86f, 0.28f, 0.28f, 0.80f));
    constexpr ImGuiWindowFlags kPillFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##SufferingActiveCheatsHud", nullptr, kPillFlags)) {
        const float line_h = ImGui::GetTextLineHeight();
        const float dot_r = std::max(3.0f, std::round(3.5f * s));
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(cur.x + dot_r, cur.y + line_h * 0.5f), dot_r,
                                                    IM_COL32(246, 92, 84, 245));
        ImGui::Dummy(ImVec2(dot_r * 2.0f + 2.0f * s, line_h));
        ImGui::SameLine(0.0f, 4.0f * s);
        ImGui::TextColored(ImVec4(0.98f, 0.84f, 0.58f, 0.96f), "CHEATS ACTIVE (%u)", active_count);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
}

void PollInGameUpdateTransitions(const UpdateStatusSnapshot& snap) {
    if (g_ingame_upd_manual_pending && snap.state != UpdateState::Checking && snap.state != UpdateState::Idle) {
        g_ingame_upd_manual_pending = false;
        if (snap.state == UpdateState::UpToDate) {
            SetStatus(std::string("Up to date (") + SUFFERING_VERSION_TAG + ")", false);
        } else if (snap.state == UpdateState::UpdateAvailable) {
            std::string tag = !snap.latest_tag.empty() ? snap.latest_tag : ("v" + snap.latest_version);
            SetStatus("Update available: " + tag, false);
        } else if (snap.state == UpdateState::Error) {
            SetStatus(snap.status_message.empty() ? "Update check failed (network error)."
                                                  : SanitizeUiText(snap.status_message, true, 140),
                      true);
        }
    } else if (g_ingame_prev_upd_state == UpdateState::Downloading && snap.state != UpdateState::Downloading) {
        if (snap.state == UpdateState::ReadyToRestart) {
            SetStatus("Update " + snap.latest_tag +
                          " staged! Save your game progress first, then click 'Save & Exit to Update'.",
                      false);
        } else if (snap.state == UpdateState::Error) {
            SetStatus(snap.status_message.empty() ? "Self-update download/staging failed."
                                                  : SanitizeUiText(snap.status_message, true, 140),
                      true);
        }
    }
    g_ingame_prev_upd_state = snap.state;
}

void DrawSettingsWindow(double fps, float ui_scale) {
    app::Settings& s = app::Get();
    if (!g_lists_inited) {
        g_lists_inited = true;
        g_gpus = app::EnumerateGpuAdapters();
        gpu::GpuCaps caps = gpu::GetGpuCaps();
        g_plugins = upscale::ProbeUpscalerPlugins(caps.vendor_id, caps.max_shader_model);
    }
    RefreshMonitorLists(g_monitors, g_resolutions, g_res_device, s.monitor);

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 win_size(std::min(860.0f * ui_scale, io.DisplaySize.x * 0.94f),
                    std::min(640.0f * ui_scale, io.DisplaySize.y * 0.94f));
    ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));

    UiPanelContext pctx{};
    pctx.in_launcher = false;
    pctx.gpus = &g_gpus;
    pctx.monitors = &g_monitors;
    pctx.resolutions = &g_resolutions;
    pctx.res_device = &g_res_device;
    pctx.plugins = &g_plugins;
    pctx.set_status = [](const std::string& m, bool e) { SetStatus(m, e); };
    pctx.on_display_changed = [] {
        app::ApplyDisplay();
        PostAction(UiAction::RefreshCursor);
    };
    pctx.on_open_plugins_folder = [] { PostAction(UiAction::OpenPluginsDir); };
    pctx.on_test_rumble = [] {
        g_rumble_test_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        g_rumble_test_active.store(true, std::memory_order_release);
        input::Rumble(0, 45000, 45000);
    };

    auto tab_flags = [](int idx) -> ImGuiTabItemFlags {
        int expected = idx;
        if (g_requested_tab.compare_exchange_strong(expected, -1, std::memory_order_acq_rel)) {
            return ImGuiTabItemFlags_SetSelected;
        }
        return 0;
    };

    UpdateStatusSnapshot upd_snap = GetUpdateSnapshot();
    PollInGameUpdateTransitions(upd_snap);

    bool has_banner_pre = false;
    {
        std::lock_guard<std::mutex> lock(g_status_lock);
        has_banner_pre = !g_status_banner.empty();
    }

    bool open = true;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("The Suffering " SUFFERING_VERSION_TAG " - Settings & Cheats (F1 / F2 or Back + L3 to Close)",
                     &open, kFlags)) {
        if (ImGui::BeginTabBar("##SettingsTabs")) {
            // 1. Upscaling & Graphics Tab
            if (ImGui::BeginTabItem("Upscaling & Graphics", nullptr, tab_flags(0))) {
                g_selected_tab.store(0, std::memory_order_release);
                MkbCancelRebind();
                ImGui::Spacing();
                DrawUpscalingGraphicsPanel(pctx);
                gpu::UpscalerStatus us = gpu::GetUpscalerStatus();
                if (!us.active_label.empty()) {
                    ImGui::TextDisabled("Live Renderer: %s (%ux%u -> %ux%u)%s%s", us.active_label.c_str(), us.render_w,
                                        us.render_h, us.vp_w, us.vp_h, us.status_detail.empty() ? "" : " - ",
                                        us.status_detail.c_str());
                }
                ImGui::EndTabItem();
            }

            // 2. Display & GPU Tab
            if (ImGui::BeginTabItem("Display & GPU", nullptr, tab_flags(1))) {
                g_selected_tab.store(1, std::memory_order_release);
                MkbCancelRebind();
                ImGui::Spacing();
                DrawDisplayGpuPanel(pctx);
                ImGui::SameLine();
                if (ImGui::Button("Apply & Save")) {
                    app::ApplyDisplay();
                    app::Save();
                    PostAction(UiAction::RefreshCursor);
                    SetStatus("Display settings saved to settings.toml.");
                }
                ImGui::EndTabItem();
            }

            // 3. Controls Tab (Mouse & Keyboard + Controller)
            if (ImGui::BeginTabItem("Controls", nullptr, tab_flags(2))) {
                g_selected_tab.store(2, std::memory_order_release);
                ImGui::Spacing();
                DrawControlsPanel(pctx);
                ImGui::EndTabItem();
            }

            // 4. Audio Tab
            if (ImGui::BeginTabItem("Audio", nullptr, tab_flags(3))) {
                g_selected_tab.store(3, std::memory_order_release);
                MkbCancelRebind();
                ImGui::Spacing();
                DrawAudioPanel(pctx);
                ImGui::EndTabItem();
            }

            // 5. Launcher & Diagnostics Tab
            if (ImGui::BeginTabItem("Launcher & Diagnostics", nullptr, tab_flags(4))) {
                g_selected_tab.store(4, std::memory_order_release);
                MkbCancelRebind();
                ImGui::Spacing();
                SharedUiOptions& o = GetSharedUiOptions();

                // --- Updates Section ---
                ImGui::TextUnformatted("Updates (" SUFFERING_VERSION_TAG "):");
                if (ShouldShowUpdateBanner(upd_snap, o.dismissed_update_tag)) {
                    UpdateCardContext uctx{};
                    uctx.in_launcher = false;
                    uctx.ui_scale = ui_scale;
                    uctx.on_update_now = [&] {
                        if (StartDownloadAndStageUpdate()) {
                            SetStatus("Downloading & staging " +
                                          (upd_snap.asset_name.empty() ? upd_snap.latest_tag : upd_snap.asset_name) +
                                          " in the background...",
                                      false);
                        } else {
                            UpdateStatusSnapshot err_s = GetUpdateSnapshot();
                            SetStatus(err_s.status_message.empty() ? "Could not start self-update download."
                                                                   : SanitizeUiText(err_s.status_message, true, 140),
                                      true);
                        }
                    };
                    uctx.on_apply_and_restart = [&] {
                        app::Save();
                        if (ApplyStagedUpdateAndRelaunch()) {
                            PostAction(UiAction::QuitForStagedUpdate);
                        } else {
                            UpdateStatusSnapshot err_s = GetUpdateSnapshot();
                            SetStatus(err_s.status_message.empty() ? "Failed to launch staged update helper."
                                                                   : SanitizeUiText(err_s.status_message, true, 140),
                                      true);
                        }
                    };
                    uctx.on_open_release_page = [](const std::string& url) {
                        {
                            std::lock_guard<std::mutex> lock(g_status_lock);
                            g_pending_release_url = url.empty() ? std::string(SUFFERING_RELEASES_URL) : url;
                        }
                        PostAction(UiAction::OpenReleaseUrl);
                    };
                    uctx.on_dismiss = [&](const std::string& tag) {
                        o.dismissed_update_tag = tag;
                        app::Save();
                        SetStatus("Dismissed update alert for " + tag + " (click 'Check for Updates' to show again).",
                                  false);
                    };
                    DrawUpdateAlertCard(upd_snap, uctx);
                    ImGui::Spacing();
                }

                const bool upd_busy =
                    (upd_snap.state == UpdateState::Checking || upd_snap.state == UpdateState::Downloading);
                if (upd_busy) ImGui::BeginDisabled(true);
                if (ImGui::Button(upd_snap.state == UpdateState::Checking ? "Checking...##InGameUpdBtn"
                                                                          : "Check for Updates##InGameUpdBtn")) {
                    if (!o.dismissed_update_tag.empty()) {
                        o.dismissed_update_tag.clear();
                        app::Save();
                    }
                    g_ingame_upd_manual_pending = true;
                    SetStatus("Checking GitHub Releases for updates...", false);
                    StartUpdateCheck(true);
                }
                if (upd_busy) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Open Release Page##InGameRelBtn")) {
                    {
                        std::lock_guard<std::mutex> lock(g_status_lock);
                        g_pending_release_url =
                            upd_snap.html_url.empty() ? std::string(SUFFERING_RELEASES_URL) : upd_snap.html_url;
                    }
                    PostAction(UiAction::OpenReleaseUrl);
                }
                ImGui::SameLine();
                if (ImGui::Checkbox("Automatically Check for Updates on Startup", &o.check_updates_on_startup)) {
                    app::Save();
                }

                switch (upd_snap.state) {
                    case UpdateState::Checking:
                        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.36f, 1.0f),
                                           "Status: Querying GitHub Releases (%s)...", SUFFERING_GITHUB_REPO);
                        break;
                    case UpdateState::UpToDate:
                        ImGui::TextColored(ImVec4(0.45f, 0.92f, 0.55f, 1.0f), "Status: Up to date (%s)",
                                           SUFFERING_VERSION_TAG);
                        break;
                    case UpdateState::UpdateAvailable:
                        ImGui::TextColored(
                            ImVec4(1.00f, 0.84f, 0.36f, 1.0f),
                            "Status: Update available (%s) - save your progress in-game before updating.",
                            upd_snap.latest_tag.c_str());
                        break;
                    case UpdateState::Downloading:
                        ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.36f, 1.0f), "Status: Downloading %s...",
                                           upd_snap.asset_name.c_str());
                        break;
                    case UpdateState::ReadyToRestart:
                        ImGui::TextColored(
                            ImVec4(0.48f, 0.96f, 0.60f, 1.0f),
                            "Status: Update %s staged! Save your game progress, then click 'Save & Exit to Update'.",
                            upd_snap.latest_tag.c_str());
                        break;
                    case UpdateState::Error:
                        ImGui::TextColored(
                            ImVec4(0.95f, 0.38f, 0.38f, 1.0f), "Status: %s",
                            upd_snap.status_message.empty()
                                ? "Update check failed."
                                : SanitizeUiText(upd_snap.status_message, true, 120).c_str());
                        break;
                    case UpdateState::Idle:
                    default:
                        ImGui::TextDisabled("Installed Version: %s  |  GitHub: %s", SUFFERING_VERSION_TAG,
                                            SUFFERING_GITHUB_REPO);
                        break;
                }
                ImGui::Separator();

                if (ImGui::Checkbox("Show Live FPS, Frame-Time & Upscaler HUD", &o.show_fps_hud)) {
                    app::Save();
                }
                bool show_launcher = !s.skip_launcher;
                if (ImGui::Checkbox("Show Pre-Game Launcher on Startup", &show_launcher)) {
                    s.skip_launcher = !show_launcher;
                    app::MarkSkipLauncherUserEdited();
                    app::Save();
                }
                ImGui::TextDisabled("Current render FPS: %.1f  |  Guest Core: CPU %u (mask 0x%llX)", fps,
                                    app::GuestCoreIndex(), (unsigned long long)app::GuestCoreMask());

                ImGui::Separator();
                gpu::GpuCaps caps = gpu::GetGpuCaps();
#ifdef _WIN32
                ImGui::Text("Active GPU: %s (FL 0x%X, SM 0x%X, %s, Upload Ring %llu MiB)",
                            caps.adapter_name.empty() ? "D3D12" : caps.adapter_name.c_str(), caps.max_feature_level,
                            caps.max_shader_model, caps.uma ? "UMA" : "Discrete",
                            (unsigned long long)(caps.upload_ring_bytes >> 20));
#else
                ImGui::Text("Active GPU: %s (Vulkan 1.2, %s, Upload Ring %llu MiB)",
                            caps.adapter_name.empty() ? "Vulkan 1.2" : caps.adapter_name.c_str(),
                            caps.uma ? "UMA" : "Discrete",
                            (unsigned long long)(caps.upload_ring_bytes >> 20));
#endif
                d3d::CacheStats cs = d3d::GetCacheStats();
                gpu::ShaderWarmupStatus ws = gpu::GetShaderWarmupStatus();
                ImGui::Text("Shader / PSO Disk Cache: %s (%u shaders, %u PSOs, %.2f MB on disk)",
                            cs.enabled ? "Enabled" : "Disabled", cs.shaders, cs.psos,
                            double(cs.disk_bytes) / (1024.0 * 1024.0));
                ImGui::TextDisabled("Startup Pre-Compilation: %s (%u new compiled at boot, %.1f ms warmup)",
                                    ws.active ? "Running..." : "Ready", ws.compiled_new, ws.elapsed_ms);
                if (ImGui::Button("Reset Shader & PSO Cache")) {
                    d3d::RequestCacheReset();
                    SetStatus("Shader & PSO cache cleared; shaders will rebuild on demand.");
                }

                ImGui::Separator();
                ImGui::TextWrapped("Configured Disc Image (iso_path): %s",
                                   s.iso_path.empty() ? "(auto-detected / CLI override)" : s.iso_path.c_str());
                if (ImGui::Button("Relocate Disc Image (.xiso.iso / .iso)...")) {
#ifdef _WIN32
                    PostAction(UiAction::RelocateXiso);
#else
                    g_file_browser.Open();
#endif
                }

                ImGui::Separator();
#ifdef _WIN32
                ImGui::TextUnformatted("Folders & Windows Integration:");
#else
                ImGui::TextUnformatted("Folders & Desktop Integration:");
#endif
                if (ImGui::Button("Open Save Data")) PostAction(UiAction::OpenSavesDir);
                ImGui::SameLine();
                if (ImGui::Button("Open Logs")) PostAction(UiAction::OpenLogsDir);
                ImGui::SameLine();
                if (ImGui::Button("Open CrashDumps")) PostAction(UiAction::OpenCrashDir);
#ifdef _WIN32
                ImGui::SameLine();
                if (ImGui::Button("Open plugins\\")) PostAction(UiAction::OpenPluginsDir);
#endif

                if (ImGui::Button("Create Desktop Shortcut")) {
                    PostAction(UiAction::CreateShortcut);
                }
#ifdef _WIN32
                ImGui::TextDisabled("Saves: hdd\\E\\UDATA\\4d570015  |  Settings: settings.toml  |  Cache: cache\\");
                ImGui::Separator();
                ImGui::TextDisabled(
                    "Credits: XboxXBERecomp (MIT), SDL3, Dear ImGui, NVIDIA DLSS / GeForce RTX (TM of NVIDIA Corp.), "
                    "AMD FidelityFX, Intel(R) XeSS. Full terms in LICENSE & THIRD_PARTY_NOTICES.md.");
#else
                ImGui::TextDisabled("Saves: hdd/E/UDATA/4d570015  |  Settings: settings.toml  |  Cache: cache/");
                ImGui::Separator();
                ImGui::TextDisabled(
                    "Credits: XboxXBERecomp (MIT), SDL3, Dear ImGui, glslang/SPIRV-Tools, AMD FidelityFX FSR 1.0/CAS, "
                    "NVIDIA NIS. Full terms in LICENSE & THIRD_PARTY_NOTICES.md.");
#endif
                ImGui::EndTabItem();
            }

            // 6. Cheats Tab (Quick-Toggle via F2)
            if (ImGui::BeginTabItem("Cheats", nullptr, tab_flags(kTabCheats))) {
                g_selected_tab.store(kTabCheats, std::memory_order_release);
                MkbCancelRebind();
                float body_h = has_banner_pre ? -std::round(28.0f * ui_scale) : 0.0f;
                if (ImGui::BeginChild("##CheatsTabBody", ImVec2(0.0f, body_h), ImGuiChildFlags_NavFlattened)) {
                    DrawCheatsPanel(ui_scale, pctx.set_status);
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

#ifndef _WIN32
        if (g_file_browser.IsOpen()) {
            fs::path picked;
            auto sys_dlg = [](fs::path& out) -> bool { return app::BrowseDiscImage(app::Window(), out); };
            if (g_file_browser.Draw(ui_scale, picked, sys_dlg)) {
                app::DiscValidation v{};
                if (app::ValidateDiscImage(picked, v)) {
                    app::Get().iso_path = picked.string();
                    app::Save();
                    SetStatus("Disc image saved (applies on next launch): " + picked.filename().string() + " [" +
                              v.revision + "]");
                } else {
                    SetStatus("Invalid disc image: " + v.error, true);
                }
            }
        }
#endif

        // Status banner at bottom of settings window
        std::string banner;
        bool is_err = false;
        {
            std::lock_guard<std::mutex> lock(g_status_lock);
            banner = g_status_banner;
            is_err = g_status_is_error;
        }
        if (!banner.empty()) {
            ImGui::Separator();
            ImGui::TextColored(is_err ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.45f, 0.90f, 0.55f, 1.0f),
                               "%s", banner.c_str());
        }
    }
    ImGui::End();

    if (!open) {
        g_overlay_open.store(false, std::memory_order_release);
        PostAction(UiAction::RefreshCursor);
    }
}

}  // namespace

std::vector<app::Option> UiOptions() {
    SharedUiOptions& o = GetSharedUiOptions();
    return {
        {"rumble_enabled", &o.rumble_enabled, nullptr},
        {"rumble_strength", nullptr, &o.rumble_strength},
        {"stick_deadzone", nullptr, &o.stick_deadzone},
        {"touchpad_as_back", &o.touchpad_as_back, nullptr},
        {"show_fps_hud", &o.show_fps_hud, nullptr},
        {"show_cheats_hud", &o.show_cheats_hud, nullptr},
        {"check_updates_on_startup", &o.check_updates_on_startup, nullptr},
    };
}

namespace {

int ParseSettingsTabArg(const std::string& val) {
    std::string low = val;
    for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (low == "cheats" || low == "cheat") return kTabCheats;
    return std::clamp(std::atoi(val.c_str()), 0, kSettingsTabCount - 1);
}

void ToggleOrSwitchCheatsOverlay(void* hwnd_ptr) {
    bool was_open = g_overlay_open.load(std::memory_order_acquire);
    int cur_tab = g_selected_tab.load(std::memory_order_acquire);
    int req_tab = g_requested_tab.load(std::memory_order_acquire);
    int effective_tab = (req_tab >= 0) ? req_tab : cur_tab;
    if (!was_open) {
        ClearCachedUiPad();
        g_requested_tab.store(kTabCheats, std::memory_order_release);
        g_selected_tab.store(kTabCheats, std::memory_order_release);
        g_overlay_open.store(true, std::memory_order_release);
        SyncImGuiCursorFlag(true);
        MkbOnOverlayToggled(true, hwnd_ptr);
    } else if (effective_tab == kTabCheats) {
        g_overlay_open.store(false, std::memory_order_release);
        ClearCachedUiPad();
#ifndef _WIN32
        g_file_browser.Close();
#endif
        SyncImGuiCursorFlag(false);
        MkbOnOverlayToggled(false, hwnd_ptr);
    } else {
        MkbCancelRebind();
#ifndef _WIN32
        g_file_browser.Close();
#endif
        g_requested_tab.store(kTabCheats, std::memory_order_release);
        g_selected_tab.store(kTabCheats, std::memory_order_release);
    }
}

}  // namespace

void UiInit() {
    SharedUiOptions& o = GetSharedUiOptions();
    o.rumble_strength = std::min(o.rumble_strength, 100u);
    o.stick_deadzone = std::min(o.stick_deadzone, 30u);
    input::SetTouchpadAsBack(o.touchpad_as_back);

#ifdef _WIN32
    auto args = SplitArgs(GetCommandLineW());
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--open-settings") {
            g_overlay_open.store(true, std::memory_order_release);
        } else if (args[i] == L"--open-cheats") {
            g_overlay_open.store(true, std::memory_order_release);
            g_requested_tab.store(kTabCheats, std::memory_order_release);
            g_selected_tab.store(kTabCheats, std::memory_order_release);
        } else if (args[i] == L"--settings-tab" && i + 1 < args.size()) {
            int t = ParseSettingsTabArg(WideToUtf8Str(args[++i]));
            g_requested_tab.store(t, std::memory_order_release);
            g_selected_tab.store(t, std::memory_order_release);
        } else if (args[i].rfind(L"--settings-tab=", 0) == 0) {
            int t = ParseSettingsTabArg(WideToUtf8Str(args[i].substr(15)));
            g_requested_tab.store(t, std::memory_order_release);
            g_selected_tab.store(t, std::memory_order_release);
        } else if (args[i] == L"--show-fps-hud") {
            g_force_fps_hud_cli = true;
        } else if (args[i] == L"--capture-ui-bmp" && i + 1 < args.size()) {
            g_capture_ui_path = fs::path(args[++i]);
        } else if (args[i].rfind(L"--capture-ui-bmp=", 0) == 0) {
            g_capture_ui_path = fs::path(args[i].substr(17));
        } else if (args[i] == L"--capture-ui-exit") {
            g_capture_ui_exit = true;
        } else if (args[i].rfind(L"--capture-", 0) == 0 || args[i].rfind(L"--replay-input", 0) == 0) {
            g_suppress_warmup_toast = true;
        }
    }
    if (const wchar_t* v = _wgetenv(L"XBE_OPEN_SETTINGS"); v && *v && _wtoi(v) != 0) {
        g_overlay_open.store(true, std::memory_order_release);
    }
    if (const wchar_t* v = _wgetenv(L"XBE_OPEN_CHEATS"); v && *v && _wtoi(v) != 0) {
        g_overlay_open.store(true, std::memory_order_release);
        g_requested_tab.store(kTabCheats, std::memory_order_release);
        g_selected_tab.store(kTabCheats, std::memory_order_release);
    }
    if (const wchar_t* v = _wgetenv(L"XBE_CAPTURE_UI_BMP"); v && *v && g_capture_ui_path.empty()) {
        g_capture_ui_path = fs::path(v);
    }
    if (const wchar_t* v = _wgetenv(L"XBE_CAPTURE_UI_EXIT"); v && *v && _wtoi(v) != 0) {
        g_capture_ui_exit = true;
    }
#else
    auto args = GetProcessArgsUtf8();
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--open-settings") {
            g_overlay_open.store(true, std::memory_order_release);
        } else if (args[i] == "--open-cheats") {
            g_overlay_open.store(true, std::memory_order_release);
            g_requested_tab.store(kTabCheats, std::memory_order_release);
            g_selected_tab.store(kTabCheats, std::memory_order_release);
        } else if (args[i] == "--settings-tab" && i + 1 < args.size()) {
            int t = ParseSettingsTabArg(args[++i]);
            g_requested_tab.store(t, std::memory_order_release);
            g_selected_tab.store(t, std::memory_order_release);
        } else if (args[i].rfind("--settings-tab=", 0) == 0) {
            int t = ParseSettingsTabArg(args[i].substr(15));
            g_requested_tab.store(t, std::memory_order_release);
            g_selected_tab.store(t, std::memory_order_release);
        } else if (args[i] == "--show-fps-hud") {
            g_force_fps_hud_cli = true;
        } else if (args[i] == "--capture-ui-bmp" && i + 1 < args.size()) {
            g_capture_ui_path = fs::path(args[++i]);
        } else if (args[i].rfind("--capture-ui-bmp=", 0) == 0) {
            g_capture_ui_path = fs::path(args[i].substr(17));
        } else if (args[i] == "--capture-ui-exit") {
            g_capture_ui_exit = true;
        } else if (args[i].rfind("--capture-", 0) == 0 || args[i].rfind("--replay-input", 0) == 0) {
            g_suppress_warmup_toast = true;
        }
    }
    if (const char* v = std::getenv("XBE_OPEN_SETTINGS"); v && *v && std::atoi(v) != 0) {
        g_overlay_open.store(true, std::memory_order_release);
    }
    if (const char* v = std::getenv("XBE_OPEN_CHEATS"); v && *v && std::atoi(v) != 0) {
        g_overlay_open.store(true, std::memory_order_release);
        g_requested_tab.store(kTabCheats, std::memory_order_release);
        g_selected_tab.store(kTabCheats, std::memory_order_release);
    }
    if (const char* v = std::getenv("XBE_CAPTURE_UI_BMP"); v && *v && g_capture_ui_path.empty()) {
        g_capture_ui_path = fs::path(v);
    }
    if (const char* v = std::getenv("XBE_CAPTURE_UI_EXIT"); v && *v && std::atoi(v) != 0) {
        g_capture_ui_exit = true;
    }
#endif

#ifdef _WIN32
    const wchar_t* mock_upd = _wgetenv(L"SUFFERING_UPDATE_MOCK_JSON");
    const bool has_mock_upd = mock_upd && *mock_upd;
#else
    const char* mock_upd = std::getenv("SUFFERING_UPDATE_MOCK_JSON");
    const bool has_mock_upd = mock_upd && *mock_upd;
#endif
    if ((!g_suppress_warmup_toast && g_capture_ui_path.empty() && !app::IsReplayOrAutotestActive()) || has_mock_upd) {
        if ((o.check_updates_on_startup && ShouldAutoCheckForUpdates()) || has_mock_upd) {
            if (GetUpdateSnapshot().state == UpdateState::Idle) {
                StartUpdateCheck(false);
            }
        }
    }
}

void UiOnWindow(void* hwnd) {
    // Runs on window thread right after ImGui::CreateContext() and before D3D12Backend::Init() uploads the font atlas.
    std::lock_guard<std::recursive_mutex> lock(app::UiLock());
    if (!ImGui::GetCurrentContext()) return;
    float s = ComputeWindowDpiScale(hwnd);
    LoadCrispUiFonts(ImGui::GetIO(), s, g_fonts);
    ApplyDarkIndustrialStyle(s);
    g_applied_style_scale = s;
    SyncImGuiCursorFlag(g_overlay_open.load(std::memory_order_acquire));
}

bool UiOnMessage(void* hwnd_ptr, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    // Interactive key/mouse rebind capture MUST run BEFORE Esc/F1 close handling
    if (g_overlay_open.load(std::memory_order_acquire) && MkbHandleRebindMessage(msg, wp, lp)) {
        if (result) *result = 0;
        return true;
    }

#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(hwnd_ptr);
    switch (msg) {
        case WM_CLOSE:
        case WM_DESTROY:
            ShutdownUpdateChecker();
            return false;

        case WM_KEYDOWN:
            if (wp == VK_F1) {
                bool next = !g_overlay_open.load(std::memory_order_relaxed);
                ClearCachedUiPad();
                g_overlay_open.store(next, std::memory_order_release);
                SyncImGuiCursorFlag(next);
                MkbOnOverlayToggled(next, hwnd);
                if (result) *result = 0;
                return true;
            }
            if (wp == VK_F2 && !IsPadOnlyInputSession()) {
                if ((lp & (1ll << 30)) == 0) {
                    ToggleOrSwitchCheatsOverlay(hwnd);
                }
                if (result) *result = 0;
                return true;
            }
            if (wp == VK_ESCAPE && g_overlay_open.load(std::memory_order_relaxed)) {
                g_overlay_open.store(false, std::memory_order_release);
                ClearCachedUiPad();
                SyncImGuiCursorFlag(false);
                MkbOnOverlayToggled(false, hwnd);
                if (result) *result = 0;
                return true;
            }
            return false;

        case WM_SETCURSOR:
            if (app::IsReplayOrAutotestActive()) return false;
            if (LOWORD(lp) == HTCLIENT) {
                if (MkbIsCursorCaptured()) {
                    SetCursor(nullptr);
                    if (result) *result = TRUE;
                    return true;
                }
                bool show = g_overlay_open.load(std::memory_order_relaxed) ||
                            MkbGetConfig().input_mode != InputMode::ControllerOnly;
                SetCursor(show ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
                if (result) *result = TRUE;
                return true;
            }
            return false;

        case kMsgUiAction: {
            auto st_cb = [](const std::string& m, bool e) { SetStatus(m, e); };
            switch (static_cast<UiAction>(wp)) {
                case UiAction::RefreshCursor:
                    UpdateMainThreadCursor(hwnd);
                    break;
                case UiAction::RelocateXiso: {
                    fs::path picked;
                    if (app::BrowseDiscImage(hwnd, picked)) {
                        app::DiscValidation v;
                        if (app::ValidateDiscImage(picked, v)) {
                            app::Get().iso_path = WideToUtf8Str(picked.wstring());
                            app::Save();
                            SetStatus("Disc image saved (applies on next launch): " + picked.filename().string() +
                                      " [" + v.revision + "]");
                        } else {
                            std::wstring err = L"Invalid disc image:\n" + picked.wstring() + L"\n\n" +
                                               Utf8ToWideStr(v.error);
                            MessageBoxW(hwnd, err.c_str(), app::kGame.short_title, MB_ICONERROR);
                            SetStatus("Invalid disc image: " + v.error, true);
                        }
                    }
                    break;
                }
                case UiAction::OpenSavesDir:
                    OpenDirectoryInExplorer(hwnd, app::ExeDir() / "hdd" / "E" / "UDATA" / "4d570015", "Save Data",
                                            st_cb);
                    break;
                case UiAction::OpenLogsDir:
                    OpenDirectoryInExplorer(hwnd, app::ExeDir() / "logs", "Logs", st_cb);
                    break;
                case UiAction::OpenCrashDir:
                    OpenDirectoryInExplorer(hwnd, app::ExeDir() / "CrashDumps", "CrashDumps", st_cb);
                    break;
                case UiAction::OpenPluginsDir:
                    OpenDirectoryInExplorer(hwnd, app::ExeDir() / "plugins", "Plugins", st_cb);
                    break;
                case UiAction::CreateShortcut: {
                    fs::path out_lnk;
                    std::string err;
                    if (app::CreateShortcut({}, out_lnk, err)) {
                        SetStatus("Desktop shortcut created: " + out_lnk.string());
                    } else {
                        SetStatus("Shortcut creation failed: " + err, true);
                    }
                    break;
                }
                case UiAction::OpenReleaseUrl: {
                    std::string url;
                    {
                        std::lock_guard<std::mutex> lock(g_status_lock);
                        url = g_pending_release_url;
                    }
                    if (url.empty()) url = SUFFERING_RELEASES_URL;
                    OpenReleasePageInBrowser(url);
                    SetStatus("Opened release page in browser: " + url);
                    break;
                }
                case UiAction::QuitForStagedUpdate:
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                    break;
            }
            if (result) *result = 0;
            return true;
        }
    }
    return false;
#else
    (void)wp;
    const SDL_Event* ev = reinterpret_cast<const SDL_Event*>(lp);
    if (!ev) return false;
    switch (msg) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            ShutdownUpdateChecker();
            return false;

        case SDL_EVENT_KEY_DOWN:
            if (ev->key.repeat) return false;
            if (ev->key.key == SDLK_F1) {
                bool next = !g_overlay_open.load(std::memory_order_relaxed);
                ClearCachedUiPad();
                g_overlay_open.store(next, std::memory_order_release);
                if (!next) g_file_browser.Close();
                SyncImGuiCursorFlag(next);
                MkbOnOverlayToggled(next, hwnd_ptr);
                if (result) *result = 0;
                return true;
            }
            if (ev->key.key == SDLK_F2 && !IsPadOnlyInputSession()) {
                ToggleOrSwitchCheatsOverlay(hwnd_ptr);
                if (result) *result = 0;
                return true;
            }
            if (ev->key.key == SDLK_ESCAPE && g_overlay_open.load(std::memory_order_relaxed)) {
                if (g_file_browser.IsOpen()) return false;  // Let the modal handle Esc first
                g_overlay_open.store(false, std::memory_order_release);
                ClearCachedUiPad();
                SyncImGuiCursorFlag(false);
                MkbOnOverlayToggled(false, hwnd_ptr);
                if (result) *result = 0;
                return true;
            }
            return false;

        case SDL_EVENT_USER:
            if (ev->user.code != kSdlUserUiAction) return false;
            {
                auto st_cb = [](const std::string& m, bool e) { SetStatus(m, e); };
                UiAction act = static_cast<UiAction>(reinterpret_cast<uintptr_t>(ev->user.data1));
                switch (act) {
                    case UiAction::RefreshCursor:
                        UpdateMainThreadCursor(hwnd_ptr);
                        break;
                    case UiAction::RelocateXiso:
                        g_file_browser.Open();
                        break;
                    case UiAction::OpenSavesDir:
                        OpenDirectoryInExplorer(hwnd_ptr, app::ExeDir() / "hdd" / "E" / "UDATA" / "4d570015",
                                                "Save Data", st_cb);
                        break;
                    case UiAction::OpenLogsDir:
                        OpenDirectoryInExplorer(hwnd_ptr, app::ExeDir() / "logs", "Logs", st_cb);
                        break;
                    case UiAction::OpenCrashDir:
                        OpenDirectoryInExplorer(hwnd_ptr, app::ExeDir() / "CrashDumps", "CrashDumps", st_cb);
                        break;
                    case UiAction::OpenPluginsDir:
                        break;
                    case UiAction::CreateShortcut: {
                        fs::path out_lnk;
                        std::string err;
                        if (app::CreateShortcut({}, out_lnk, err)) {
                            SetStatus("Desktop shortcut created: " + out_lnk.string());
                        } else {
                            SetStatus("Shortcut creation failed: " + err, true);
                        }
                        break;
                    }
                    case UiAction::OpenReleaseUrl: {
                        std::string url;
                        {
                            std::lock_guard<std::mutex> lock(g_status_lock);
                            url = g_pending_release_url;
                        }
                        if (url.empty()) url = SUFFERING_RELEASES_URL;
                        OpenReleasePageInBrowser(url);
                        if (IsGamescope()) {
                            SetStatus("Release page: " + url + " (browse in Desktop Mode if hidden)");
                        } else {
                            SetStatus("Opened release page in browser: " + url);
                        }
                        break;
                    }
                    case UiAction::QuitForStagedUpdate: {
                        SDL_Event qev{};
                        qev.type = SDL_EVENT_QUIT;
                        SDL_PushEvent(&qev);
                        break;
                    }
                }
            }
            if (result) *result = 0;
            return true;
    }
    return false;
#endif
}

void OnUi(double fps) {
    if (guest_settings::ConsumePendingHostSaveRequest()) {
        app::Save();
    }

    if (g_rumble_test_active.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() >= g_rumble_test_until) {
        g_rumble_test_active.store(false, std::memory_order_release);
        input::Rumble(0, 0, 0);
    }

    bool open = g_overlay_open.load(std::memory_order_acquire);
    if (ImGui::GetCurrentContext()) {
        ImGuiIO& io = ImGui::GetIO();
        if (open) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
    input::Pad p{};
    if (open) {
        std::lock_guard<std::mutex> lock(g_pad_lock);
        p = g_ui_pad;
    } else if (g_was_overlay_open_ui) {
        ClearCachedUiPad();
    }
    FeedPadToImGui(p, open, g_was_pad_active);

    if (open) {
        bool modal_blocking = MkbIsRebinding();
#ifndef _WIN32
        if (g_file_browser.IsOpen()) modal_blocking = true;
#endif
        bool l1 = p.connected && !modal_blocking && p.analog[5] > 64;
        bool r1 = p.connected && !modal_blocking && p.analog[4] > 64;
        if (g_was_overlay_open_ui) {
            int cur_tab = g_selected_tab.load(std::memory_order_acquire);
            int req_tab = g_requested_tab.load(std::memory_order_acquire);
            int base_tab = (req_tab >= 0) ? req_tab : cur_tab;
            if (l1 && !g_prev_pad_l1) {
                int next_tab = (base_tab + kSettingsTabCount - 1) % kSettingsTabCount;
                g_requested_tab.store(next_tab, std::memory_order_release);
                g_selected_tab.store(next_tab, std::memory_order_release);
            } else if (r1 && !g_prev_pad_r1) {
                int next_tab = (base_tab + 1) % kSettingsTabCount;
                g_requested_tab.store(next_tab, std::memory_order_release);
                g_selected_tab.store(next_tab, std::memory_order_release);
            }
        }
        g_prev_pad_l1 = l1;
        g_prev_pad_r1 = r1;
    } else {
        g_prev_pad_l1 = false;
        g_prev_pad_r1 = false;
    }
    g_was_overlay_open_ui = open;

    SharedUiOptions& o = GetSharedUiOptions();
    gpu::ShaderWarmupStatus warmup_ws{};
    float warmup_alpha = 0.0f;
    bool show_warmup = ShouldShowShaderWarmupToast(warmup_ws, warmup_alpha);
    UpdateStatusSnapshot toast_upd_snap{};
    float update_toast_alpha = 0.0f;
    bool show_update_toast = false;
    if (!g_suppress_warmup_toast && g_capture_ui_path.empty() && !app::IsReplayOrAutotestActive()) {
        toast_upd_snap = GetUpdateSnapshot();
        show_update_toast =
            ShouldShowUpdateAvailableToast(toast_upd_snap, o.dismissed_update_tag, open, update_toast_alpha);
    }
    uint32_t active_cheats_count = 0;
    bool show_cheats_hud = ShouldShowActiveCheatsHud(o, active_cheats_count);
    bool show_fps = o.show_fps_hud || g_force_fps_hud_cli;
    bool recording = IsInputRecordingActive();
    if (show_fps || recording || open || show_warmup || show_update_toast || show_cheats_hud) {
        float ui_scale = ComputeInGameUiScale();
        EnsureInGameStyle(ui_scale);
        if (show_fps || recording) DrawStatusHud(fps, ui_scale, show_fps, recording);
        if (show_cheats_hud) DrawActiveCheatsHud(active_cheats_count, ui_scale);
        if (show_warmup) DrawShaderWarmupToast(warmup_ws, warmup_alpha, ui_scale);
        if (show_update_toast) DrawUpdateAvailableToast(toast_upd_snap, update_toast_alpha, ui_scale);
        if (open) DrawSettingsWindow(fps, ui_scale);
    }

    // Automated UI screenshot hook (runs only when --capture-ui-bmp is passed)
    if (!g_capture_ui_path.empty()) {
        g_ui_frames_drawn++;
        if (!g_capture_ui_requested && g_ui_frames_drawn >= 90) {
            g_capture_ui_requested = true;
            d3d::RequestBackbufferCapture(g_capture_ui_path);
        } else if (g_capture_ui_exit && g_capture_ui_requested && !g_capture_ui_exit_posted && g_ui_frames_drawn >= 96) {
            g_capture_ui_exit_posted = true;
#ifdef _WIN32
            if (HWND hwnd = static_cast<HWND>(app::Window())) PostMessageW(hwnd, WM_CLOSE, 0, 0);
#else
            SDL_Event qev{};
            qev.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&qev);
#endif
        }
    }
}

void UiFilterPad(int port, input::Pad& pad, bool scripted) {
    if (port != 0 || scripted) return;

    // Edge-triggered controller chord: BACK (bit 5) + LTHUMB (bit 6)
    bool chord = pad.connected && ((pad.buttons & 0x60u) == 0x60u);
    if (chord && !g_chord_held) {
        bool next = !g_overlay_open.load(std::memory_order_relaxed);
        ClearCachedUiPad();
        g_overlay_open.store(next, std::memory_order_release);
        PostAction(UiAction::RefreshCursor);
    }
    g_chord_held = chord;

    if (g_overlay_open.load(std::memory_order_acquire)) {
        {
            std::lock_guard<std::mutex> lock(g_pad_lock);
            g_ui_pad = pad;
        }
        bool conn = pad.connected;
        pad = input::Pad{};
        pad.connected = conn;
        return;
    }

    SharedUiOptions& o = GetSharedUiOptions();
    if (pad.connected && o.stick_deadzone > 0) {
        ApplyRadialDeadzone(pad.lx, pad.ly, o.stick_deadzone);
        ApplyRadialDeadzone(pad.rx, pad.ry, o.stick_deadzone);
    }
}

void UiFilterRumble(int /*port*/, uint16_t& left, uint16_t& right) {
    SharedUiOptions& o = GetSharedUiOptions();
    if (!o.rumble_enabled || o.rumble_strength == 0) {
        left = 0;
        right = 0;
        return;
    }
    if (o.rumble_strength < 100) {
        left = uint16_t((uint32_t(left) * o.rumble_strength) / 100u);
        right = uint16_t((uint32_t(right) * o.rumble_strength) / 100u);
    }
}

bool UiIsOverlayOpen() { return g_overlay_open.load(std::memory_order_acquire); }

void UiSetOverlayOpen(bool open) {
    ClearCachedUiPad();
    g_overlay_open.store(open, std::memory_order_release);
    PostAction(UiAction::RefreshCursor);
}

void UiRequestRefreshCursor() { PostAction(UiAction::RefreshCursor); }

}  // namespace suffering
