// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <imgui.h>

#include "app/app.h"
#include "common/update_checker.h"
#include "upscale/upscaler_manager.h"

namespace input {
struct Pad;
}

namespace suffering {

// Shared persistent UI options (bound to settings.toml via UiOptions() in settings_ui.cpp)
struct SharedUiOptions {
    bool rumble_enabled = true;
    uint32_t rumble_strength = 100;
    uint32_t stick_deadzone = 0;
    bool touchpad_as_back = true;
    bool show_fps_hud = false;
    bool check_updates_on_startup = true;
    std::string dismissed_update_tag;
};

SharedUiOptions& GetSharedUiOptions();

// Font + DPI styling helpers shared by Pre-Game Launcher and In-Game F1 Overlay.
struct UiFonts {
    ImFont* regular = nullptr;
    ImFont* heading = nullptr;
    float scale = 1.0f;
};

float ComputeWindowDpiScale(void* hwnd);
void LoadCrispUiFonts(ImGuiIO& io, float ui_scale, UiFonts& out);
void ApplyDarkIndustrialStyle(float ui_scale);
void FeedPadToImGui(const input::Pad& pad, bool active, bool& was_active);

// Hardware preset detection and application.
const char* HardwarePresetName(app::HardwarePreset preset);
const char* PresentModeName(app::PresentMode mode);
app::HardwarePreset DetectHardwarePreset(const app::GpuAdapterInfo* selected_gpu, app::Mode desktop,
                                         uint64_t total_ram_bytes);
void ApplyHardwarePreset(app::HardwarePreset preset, const app::GpuAdapterInfo* selected_gpu,
                         const upscale::PluginAvailability& plugins, app::Mode desktop);
void ResetAllSettingsToDefaults();

struct UiPanelContext {
    bool in_launcher = false;
    std::vector<app::GpuAdapterInfo>* gpus = nullptr;
    std::vector<app::MonitorInfo>* monitors = nullptr;
    std::vector<app::Mode>* resolutions = nullptr;
    std::string* res_device = nullptr;
    upscale::PluginAvailability* plugins = nullptr;
    std::function<void(const std::string&, bool)> set_status;
    std::function<void()> on_display_changed;
    std::function<void()> on_open_plugins_folder;
    std::function<void()> on_test_rumble;
};

void RefreshMonitorLists(std::vector<app::MonitorInfo>& monitors, std::vector<app::Mode>& resolutions,
                         std::string& res_device, const std::string& monitor_device);
const app::GpuAdapterInfo* FindSelectedGpu(const std::vector<app::GpuAdapterInfo>& gpus, const std::string& key);

void DrawUpscalingGraphicsPanel(const UiPanelContext& ctx);
void DrawDisplayGpuPanel(const UiPanelContext& ctx);
void DrawAudioPanel(const UiPanelContext& ctx);
void DrawControlsPanel(const UiPanelContext& ctx);

std::string WideToUtf8Str(const std::wstring& w);
std::wstring Utf8ToWideStr(const std::string& s);
std::vector<std::string> GetProcessArgsUtf8();
bool IsSteamDeck();
bool IsGamescope();
void OpenDirectoryInExplorer(void* hwnd, const std::filesystem::path& dir, const char* label,
                             const std::function<void(const std::string&, bool)>& set_status);

// Update Checker & Self-Updater UI components shared by Pre-Game Launcher and In-Game F1 Overlay.
struct UpdateCardContext {
    bool in_launcher = true;
    float ui_scale = 1.0f;
    std::function<void()> on_update_now;
    std::function<void()> on_apply_and_restart;
    std::function<void(const std::string&)> on_open_release_page;
    std::function<void(const std::string&)> on_dismiss;
};

std::string SanitizeUiText(const std::string& s, bool single_line = true, size_t max_chars = 160);
bool ShouldShowUpdateBanner(const UpdateStatusSnapshot& snap, const std::string& dismissed_tag);
void DrawLauncherHeaderUpdateButton(const UpdateStatusSnapshot& snap, float ui_scale,
                                    const std::function<void()>& on_manual_check);
void DrawUpdateAlertCard(const UpdateStatusSnapshot& snap, const UpdateCardContext& uctx);

}  // namespace suffering
