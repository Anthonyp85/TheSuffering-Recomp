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
#include "common/mkb_input.h"
#include "common/settings_ui.h"
#include "common/ui_common.h"

#include <algorithm>
#include <array>
#include <atomic>
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
};

std::atomic<bool> g_overlay_open{false};
bool g_was_pad_active = false;
int g_requested_tab = -1;
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

// Automated UI screenshot capture CLI/env support (--open-settings, --settings-tab N, --capture-ui-bmp, --capture-ui-exit)
fs::path g_capture_ui_path;
bool g_capture_ui_exit = false;
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

void UpdateMainThreadCursor(void* hwnd) {
    bool open = g_overlay_open.load(std::memory_order_acquire);
    SyncImGuiCursorFlag(open);
    if (open) {
        MkbForceReleaseCursor(hwnd);
    } else {
        MkbSyncCursorOnWindowThread(hwnd);
    }
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

void DrawFpsHud(double fps, float ui_scale) {
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
    ImGui::End();
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
    ImVec2 win_size(std::min(820.0f * ui_scale, io.DisplaySize.x * 0.94f),
                    std::min(580.0f * ui_scale, io.DisplaySize.y * 0.92f));
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
        if (g_requested_tab == idx) {
            g_requested_tab = -1;
            return ImGuiTabItemFlags_SetSelected;
        }
        return 0;
    };

    bool open = true;
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    if (ImGui::Begin("The Suffering v1.3.1 - Settings (F1 or Back + L3 to Close)", &open, kFlags)) {
        if (ImGui::BeginTabBar("##SettingsTabs")) {
            // 1. Upscaling & Graphics Tab
            if (ImGui::BeginTabItem("Upscaling & Graphics", nullptr, tab_flags(0))) {
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
                ImGui::Spacing();
                DrawDisplayGpuPanel(pctx);
                ImGui::SameLine();
                if (ImGui::Button("Apply & Save")) {
                    app::ApplyDisplay();
                    app::Save();
                    SetStatus("Display settings saved to settings.toml.");
                }
                ImGui::EndTabItem();
            }

            // 3. Controls Tab (Mouse & Keyboard + Controller)
            if (ImGui::BeginTabItem("Controls", nullptr, tab_flags(2))) {
                ImGui::Spacing();
                DrawControlsPanel(pctx);
                ImGui::EndTabItem();
            }

            // 4. Audio Tab
            if (ImGui::BeginTabItem("Audio", nullptr, tab_flags(3))) {
                ImGui::Spacing();
                DrawAudioPanel(pctx);
                ImGui::EndTabItem();
            }

            // 5. Launcher & Diagnostics Tab
            if (ImGui::BeginTabItem("Launcher & Diagnostics", nullptr, tab_flags(4))) {
                ImGui::Spacing();
                SharedUiOptions& o = GetSharedUiOptions();
                if (ImGui::Checkbox("Show Live FPS, Frame-Time & Upscaler HUD", &o.show_fps_hud)) {
                    app::Save();
                }
                bool show_launcher = !s.skip_launcher;
                if (ImGui::Checkbox("Show Pre-Game Launcher on Startup", &show_launcher)) {
                    s.skip_launcher = !show_launcher;
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
                ImGui::Text("Shader / PSO Disk Cache: %s (%u shaders, %u PSOs, %.2f MB on disk)",
                            cs.enabled ? "Enabled" : "Disabled", cs.shaders, cs.psos,
                            double(cs.disk_bytes) / (1024.0 * 1024.0));
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
    };
}

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
        } else if (args[i] == L"--settings-tab" && i + 1 < args.size()) {
            g_requested_tab = std::clamp(_wtoi(args[++i].c_str()), 0, 4);
        } else if (args[i].rfind(L"--settings-tab=", 0) == 0) {
            g_requested_tab = std::clamp(_wtoi(args[i].c_str() + 15), 0, 4);
        } else if (args[i] == L"--show-fps-hud") {
            o.show_fps_hud = true;
        } else if (args[i] == L"--capture-ui-bmp" && i + 1 < args.size()) {
            g_capture_ui_path = fs::path(args[++i]);
        } else if (args[i].rfind(L"--capture-ui-bmp=", 0) == 0) {
            g_capture_ui_path = fs::path(args[i].substr(17));
        } else if (args[i] == L"--capture-ui-exit") {
            g_capture_ui_exit = true;
        }
    }
    if (const wchar_t* v = _wgetenv(L"XBE_OPEN_SETTINGS"); v && *v && _wtoi(v) != 0) {
        g_overlay_open.store(true, std::memory_order_release);
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
        } else if (args[i] == "--settings-tab" && i + 1 < args.size()) {
            g_requested_tab = std::clamp(std::atoi(args[++i].c_str()), 0, 4);
        } else if (args[i].rfind("--settings-tab=", 0) == 0) {
            g_requested_tab = std::clamp(std::atoi(args[i].c_str() + 15), 0, 4);
        } else if (args[i] == "--show-fps-hud") {
            o.show_fps_hud = true;
        } else if (args[i] == "--capture-ui-bmp" && i + 1 < args.size()) {
            g_capture_ui_path = fs::path(args[++i]);
        } else if (args[i].rfind("--capture-ui-bmp=", 0) == 0) {
            g_capture_ui_path = fs::path(args[i].substr(17));
        } else if (args[i] == "--capture-ui-exit") {
            g_capture_ui_exit = true;
        }
    }
    if (const char* v = std::getenv("XBE_OPEN_SETTINGS"); v && *v && std::atoi(v) != 0) {
        g_overlay_open.store(true, std::memory_order_release);
    }
    if (const char* v = std::getenv("XBE_CAPTURE_UI_BMP"); v && *v && g_capture_ui_path.empty()) {
        g_capture_ui_path = fs::path(v);
    }
    if (const char* v = std::getenv("XBE_CAPTURE_UI_EXIT"); v && *v && std::atoi(v) != 0) {
        g_capture_ui_exit = true;
    }
#endif
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
        case WM_KEYDOWN:
            if (wp == VK_F1) {
                bool next = !g_overlay_open.load(std::memory_order_relaxed);
                g_overlay_open.store(next, std::memory_order_release);
                SyncImGuiCursorFlag(next);
                MkbOnOverlayToggled(next, hwnd);
                if (result) *result = 0;
                return true;
            }
            if (wp == VK_ESCAPE && g_overlay_open.load(std::memory_order_relaxed)) {
                g_overlay_open.store(false, std::memory_order_release);
                SyncImGuiCursorFlag(false);
                MkbOnOverlayToggled(false, hwnd);
                if (result) *result = 0;
                return true;
            }
            return false;

        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                if (MkbIsCursorCaptured()) {
                    SetCursor(nullptr);
                    if (result) *result = TRUE;
                    return true;
                }
                bool show = g_overlay_open.load(std::memory_order_relaxed) ||
                            app::Get().display_mode == app::DisplayMode::Windowed ||
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
        case SDL_EVENT_KEY_DOWN:
            if (ev->key.repeat) return false;
            if (ev->key.key == SDLK_F1) {
                bool next = !g_overlay_open.load(std::memory_order_relaxed);
                g_overlay_open.store(next, std::memory_order_release);
                if (!next) g_file_browser.Close();
                SyncImGuiCursorFlag(next);
                MkbOnOverlayToggled(next, hwnd_ptr);
                if (result) *result = 0;
                return true;
            }
            if (ev->key.key == SDLK_ESCAPE && g_overlay_open.load(std::memory_order_relaxed)) {
                if (g_file_browser.IsOpen()) return false;  // Let the modal handle Esc first
                g_overlay_open.store(false, std::memory_order_release);
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
                }
            }
            if (result) *result = 0;
            return true;
    }
    return false;
#endif
}

void OnUi(double fps) {
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
    }
    FeedPadToImGui(p, open, g_was_pad_active);

    SharedUiOptions& o = GetSharedUiOptions();
    if (o.show_fps_hud || open) {
        float ui_scale = ComputeInGameUiScale();
        EnsureInGameStyle(ui_scale);
        if (o.show_fps_hud) DrawFpsHud(fps, ui_scale);
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
    g_overlay_open.store(open, std::memory_order_release);
    PostAction(UiAction::RefreshCursor);
}

void UiRequestRefreshCursor() { PostAction(UiAction::RefreshCursor); }

}  // namespace suffering
