// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <SDL3/SDL.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "common/mkb_input.h"
#include "common/ui_common.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>

#include "audio/audio.h"
#include "core/input.h"
#include "gpu/backend.h"

namespace suffering {

namespace {

namespace fs = std::filesystem;

std::string LowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

SharedUiOptions& GetSharedUiOptions() {
    static SharedUiOptions opts;
    return opts;
}

std::string WideToUtf8Str(const std::wstring& w) {
    if (w.empty()) return {};
#ifdef _WIN32
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
#else
    std::string s;
    s.reserve(w.size());
    for (wchar_t wc : w) {
        uint32_t cp = static_cast<uint32_t>(wc);
        if (cp < 0x80u) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800u) {
            s.push_back(static_cast<char>(0xC0u | (cp >> 6)));
            s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp < 0x10000u) {
            s.push_back(static_cast<char>(0xE0u | (cp >> 12)));
            s.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        } else if (cp <= 0x10FFFFu) {
            s.push_back(static_cast<char>(0xF0u | (cp >> 18)));
            s.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
            s.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
            s.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
        }
    }
    return s;
#endif
}

std::wstring Utf8ToWideStr(const std::string& s) {
    if (s.empty()) return {};
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
#else
    std::wstring w;
    w.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = static_cast<uint8_t>(s[i]);
        uint32_t cp = 0;
        if (c < 0x80u) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0u) == 0xC0u && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (uint8_t(s[i + 1]) & 0x3Fu);
            i += 2;
        } else if ((c & 0xF0u) == 0xE0u && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((uint8_t(s[i + 1]) & 0x3Fu) << 6) | (uint8_t(s[i + 2]) & 0x3Fu);
            i += 3;
        } else if ((c & 0xF8u) == 0xF0u && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((uint8_t(s[i + 1]) & 0x3Fu) << 12) | ((uint8_t(s[i + 2]) & 0x3Fu) << 6) |
                 (uint8_t(s[i + 3]) & 0x3Fu);
            i += 4;
        } else {
            cp = c;
            i += 1;
        }
        w.push_back(static_cast<wchar_t>(cp));
    }
    return w;
#endif
}

std::vector<std::string> GetProcessArgsUtf8() {
    std::vector<std::string> args;
#ifdef _WIN32
    const wchar_t* cmd = GetCommandLineW();
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
        args.push_back(WideToUtf8Str(a));
    }
#else
    std::ifstream f("/proc/self/cmdline", std::ios::binary);
    if (!f) return args;
    std::string buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 0;
    while (pos < buf.size()) {
        size_t end = buf.find('\0', pos);
        if (end == std::string::npos) end = buf.size();
        args.emplace_back(buf.data() + pos, end - pos);
        pos = end + 1;
    }
#endif
    return args;
}

bool IsSteamDeck() {
    static const bool detected = [] {
        if (const char* sd = std::getenv("SteamDeck"); sd && std::strcmp(sd, "1") == 0) return true;
#ifndef _WIN32
        auto read_trim = [](const char* path) -> std::string {
            std::ifstream f(path);
            std::string s;
            if (f) std::getline(f, s);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            return s;
        };
        std::string vendor = read_trim("/sys/class/dmi/id/sys_vendor");
        std::string product = read_trim("/sys/class/dmi/id/product_name");
        if (vendor == "Valve" && (product.find("Jupiter") != std::string::npos || product.find("Galileo") != std::string::npos)) {
            return true;
        }
        std::error_code ec;
        for (const auto& e : fs::directory_iterator("/sys/bus/pci/devices", fs::directory_options::skip_permission_denied, ec)) {
            std::string v = LowerAscii(read_trim((e.path() / "vendor").string().c_str()));
            if (v != "0x1002") continue;
            std::string d = LowerAscii(read_trim((e.path() / "device").string().c_str()));
            if (d == "0x163f" || d == "0x1435") return true;
        }
#endif
        return false;
    }();
    return detected;
}

bool IsGamescope() {
#ifdef _WIN32
    return false;
#else
    static const bool detected = [] {
        if (const char* g = std::getenv("GAMESCOPE_WAYLAND_DISPLAY"); g && *g) return true;
        if (const char* g = std::getenv("STEAM_GAMEPADUI"); g && std::strcmp(g, "1") == 0) return true;
        return false;
    }();
    return detected;
#endif
}

float ComputeWindowDpiScale(void* hwnd) {
#ifdef _WIN32
    UINT dpi = 96;
    if (hwnd) {
        using GetDpiForWindowFn = UINT(WINAPI*)(HWND);
        static auto fn = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (fn) {
            UINT d = fn(static_cast<HWND>(hwnd));
            if (d >= 72) dpi = d;
        }
    }
    return std::clamp(float(dpi) / 96.0f, 1.0f, 2.25f);
#else
    float scale = 1.0f;
    if (hwnd) {
        float s = SDL_GetWindowDisplayScale(static_cast<SDL_Window*>(hwnd));
        if (s > 0.1f) scale = s;
    }
    return std::clamp(scale, 1.0f, 2.25f);
#endif
}

void LoadCrispUiFonts(ImGuiIO& io, float ui_scale, UiFonts& out) {
    io.Fonts->Clear();
    float s = std::clamp(ui_scale, 1.0f, 2.5f);
    float body_px = std::round(16.0f * s);
    float head_px = std::round(21.0f * s);

    ImFontConfig cfg{};
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;

    std::error_code ec;
#ifdef _WIN32
    wchar_t win_dir[MAX_PATH]{};
    GetWindowsDirectoryW(win_dir, MAX_PATH);
    fs::path fonts_dir = fs::path(win_dir) / L"Fonts";
    fs::path ttf_path;
    if (fs::is_regular_file(fonts_dir / L"segoeui.ttf", ec)) ttf_path = fonts_dir / L"segoeui.ttf";
    else if (fs::is_regular_file(fonts_dir / L"arial.ttf", ec)) ttf_path = fonts_dir / L"arial.ttf";

    if (!ttf_path.empty()) {
        std::string p = WideToUtf8Str(ttf_path.wstring());
        out.regular = io.Fonts->AddFontFromFileTTF(p.c_str(), body_px, &cfg);
        fs::path bold_path = fonts_dir / L"segoeuib.ttf";
        if (fs::is_regular_file(bold_path, ec)) {
            std::string bp = WideToUtf8Str(bold_path.wstring());
            out.heading = io.Fonts->AddFontFromFileTTF(bp.c_str(), head_px, &cfg);
        } else {
            out.heading = io.Fonts->AddFontFromFileTTF(p.c_str(), head_px, &cfg);
        }
    }
#else
    static constexpr const char* kRegCandidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/NotoSans-Regular.ttf",
    };
    static constexpr const char* kBoldCandidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/liberation2/LiberationSans-Bold.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
        "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    };
    fs::path reg_path, bold_path;
    for (const char* p : kRegCandidates) {
        if (fs::is_regular_file(p, ec)) {
            reg_path = p;
            break;
        }
    }
    for (const char* p : kBoldCandidates) {
        if (fs::is_regular_file(p, ec)) {
            bold_path = p;
            break;
        }
    }
    if (!reg_path.empty()) {
        std::string rp = reg_path.string();
        out.regular = io.Fonts->AddFontFromFileTTF(rp.c_str(), body_px, &cfg);
        if (!bold_path.empty()) {
            std::string bp = bold_path.string();
            out.heading = io.Fonts->AddFontFromFileTTF(bp.c_str(), head_px, &cfg);
        } else {
            out.heading = io.Fonts->AddFontFromFileTTF(rp.c_str(), head_px, &cfg);
        }
    }
#endif
    if (!out.regular) {
        ImFontConfig def{};
        def.SizePixels = std::round(13.0f * s);
        out.regular = io.Fonts->AddFontDefault(&def);
        out.heading = out.regular;
    }
    io.FontDefault = out.regular;
    io.FontGlobalScale = 1.0f;
    out.scale = s;
}

void ApplyDarkIndustrialStyle(float ui_scale) {
    ImGuiStyle st{};
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 6.0f;
    st.ChildRounding = 5.0f;
    st.FrameRounding = 4.0f;
    st.TabRounding = 4.0f;
    st.GrabRounding = 3.0f;
    st.ScrollbarRounding = 4.0f;
    st.WindowBorderSize = 1.0f;
    st.FrameBorderSize = 1.0f;
    st.WindowPadding = ImVec2(14.0f, 12.0f);
    st.FramePadding = ImVec2(8.0f, 5.0f);
    st.ItemSpacing = ImVec2(10.0f, 7.0f);

    st.Colors[ImGuiCol_WindowBg] = ImVec4(0.075f, 0.078f, 0.095f, 0.96f);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0.105f, 0.110f, 0.130f, 0.92f);
    st.Colors[ImGuiCol_Border] = ImVec4(0.28f, 0.22f, 0.23f, 0.65f);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.17f, 0.95f);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.16f, 0.17f, 0.95f);
    st.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.36f, 0.14f, 0.16f, 1.00f);
    st.Colors[ImGuiCol_TitleBg] = ImVec4(0.14f, 0.06f, 0.07f, 0.98f);
    st.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.38f, 0.09f, 0.11f, 0.98f);
    st.Colors[ImGuiCol_Tab] = ImVec4(0.15f, 0.14f, 0.17f, 0.92f);
    st.Colors[ImGuiCol_TabHovered] = ImVec4(0.56f, 0.15f, 0.17f, 0.96f);
    st.Colors[ImGuiCol_TabSelected] = ImVec4(0.44f, 0.11f, 0.13f, 0.98f);
    st.Colors[ImGuiCol_Button] = ImVec4(0.25f, 0.12f, 0.13f, 0.92f);
    st.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.54f, 0.15f, 0.17f, 0.98f);
    st.Colors[ImGuiCol_ButtonActive] = ImVec4(0.68f, 0.17f, 0.19f, 1.00f);
    st.Colors[ImGuiCol_Header] = ImVec4(0.32f, 0.12f, 0.14f, 0.85f);
    st.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.52f, 0.15f, 0.17f, 0.95f);
    st.Colors[ImGuiCol_HeaderActive] = ImVec4(0.64f, 0.17f, 0.19f, 1.00f);
    st.Colors[ImGuiCol_CheckMark] = ImVec4(0.95f, 0.30f, 0.30f, 1.00f);
    st.Colors[ImGuiCol_SliderGrab] = ImVec4(0.82f, 0.22f, 0.24f, 1.00f);
    st.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.96f, 0.30f, 0.32f, 1.00f);
    st.Colors[ImGuiCol_Separator] = ImVec4(0.32f, 0.22f, 0.23f, 0.65f);

    st.ScaleAllSizes(std::clamp(ui_scale, 1.0f, 2.5f));
    ImGui::GetStyle() = st;
}

void FeedPadToImGui(const input::Pad& p, bool active, bool& was_active) {
    ImGuiIO& io = ImGui::GetIO();
    if (!active) {
        if (was_active) {
            was_active = false;
            io.AddKeyEvent(ImGuiKey_GamepadDpadUp, false);
            io.AddKeyEvent(ImGuiKey_GamepadDpadDown, false);
            io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, false);
            io.AddKeyEvent(ImGuiKey_GamepadDpadRight, false);
            io.AddKeyEvent(ImGuiKey_GamepadFaceDown, false);
            io.AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
            io.AddKeyEvent(ImGuiKey_GamepadL1, false);
            io.AddKeyEvent(ImGuiKey_GamepadR1, false);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, false, 0.0f);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, false, 0.0f);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, false, 0.0f);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, false, 0.0f);
        }
        return;
    }
    was_active = true;
    if (!p.connected) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (p.buttons & (1u << 0)) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (p.buttons & (1u << 1)) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (p.buttons & (1u << 2)) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (p.buttons & (1u << 3)) != 0);
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, p.analog[0] > 64);
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, p.analog[1] > 64);
    io.AddKeyEvent(ImGuiKey_GamepadL1, p.analog[5] > 64);
    io.AddKeyEvent(ImGuiKey_GamepadR1, p.analog[4] > 64);

    auto norm = [](int16_t v) -> float { return std::clamp(float(v) / 32767.0f, -1.0f, 1.0f); };
    float lx = norm(p.lx), ly = norm(p.ly);
    constexpr float kDz = 0.28f;
    auto axis = [&](float val) -> float { return val > kDz ? (val - kDz) / (1.0f - kDz) : 0.0f; };
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, axis(-lx) > 0.0f, axis(-lx));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, axis(lx) > 0.0f, axis(lx));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, axis(ly) > 0.0f, axis(ly));
    io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, axis(-ly) > 0.0f, axis(-ly));
}

const char* HardwarePresetName(app::HardwarePreset preset) {
    switch (preset) {
        case app::HardwarePreset::Custom: return "Custom";
        case app::HardwarePreset::HandheldIgpu:
            return IsSteamDeck() ? "Steam Deck (1280x800)" : "Handheld / iGPU";
        case app::HardwarePreset::Mainstream1080p: return "1080p Mainstream";
        case app::HardwarePreset::High1440p: return "1440p High";
        case app::HardwarePreset::Ultra4k: return "4K Ultra";
    }
    return "Custom";
}

const char* PresentModeName(app::PresentMode mode) {
    switch (mode) {
        case app::PresentMode::Auto: return "Auto (Smart VSync / VRR)";
        case app::PresentMode::VSync: return "VSync On (Locked)";
        case app::PresentMode::VrrAllowTearing: return "VRR / Low-Latency (Allow Tearing)";
#ifdef _WIN32
        case app::PresentMode::TearFreeMailbox: return "Tear-Free Mailbox (DWM Sync)";
#else
        case app::PresentMode::TearFreeMailbox: return "Tear-Free Mailbox (Vulkan Mailbox)";
#endif
    }
    return "Auto (Smart VSync / VRR)";
}

app::HardwarePreset DetectHardwarePreset(const app::GpuAdapterInfo* gpu, app::Mode desktop, uint64_t total_ram_bytes) {
    if (IsSteamDeck()) return app::HardwarePreset::HandheldIgpu;
    bool uma = gpu ? gpu->likely_uma : false;
    uint64_t vram = gpu ? gpu->dedicated_vram : 0;
    if (uma || vram <= (2560ull << 20) || (desktop.width <= 1366 && desktop.height <= 820) ||
        total_ram_bytes <= (8704ull << 20)) {
        return app::HardwarePreset::HandheldIgpu;
    }
    if (vram >= (9728ull << 20) && desktop.height >= 2000) {
        return app::HardwarePreset::Ultra4k;
    }
    if (vram >= (6656ull << 20) && desktop.height > 1080) {
        return app::HardwarePreset::High1440p;
    }
    return app::HardwarePreset::Mainstream1080p;
}

void ApplyHardwarePreset(app::HardwarePreset preset, const app::GpuAdapterInfo* gpu,
                         const upscale::PluginAvailability& plugins, app::Mode desktop) {
    app::Settings& s = app::Get();
    s.hardware_preset = preset;
    s.present_mode = app::PresentMode::Auto;
    s.vsync = true;
    s.fps_cap = 60;
    s.aspect_correct_render = true;

    auto pick_best_temporal_or = [&](app::UpscalerMode fallback) -> app::UpscalerMode {
        uint32_t vid = gpu ? gpu->vendor_id : 0;
        if (vid == 0x10DEu && plugins.dlss_ready) return app::UpscalerMode::Dlss;
        if (vid == 0x1002u && plugins.fsr3_ready) return app::UpscalerMode::Fsr3;
        if (vid == 0x8086u && plugins.xess_ready) return app::UpscalerMode::Xess;
        if (plugins.fsr3_ready) return app::UpscalerMode::Fsr3;
        return fallback;
    };

    switch (preset) {
        case app::HardwarePreset::HandheldIgpu:
            s.upscaler = pick_best_temporal_or(app::UpscalerMode::Fsr1);
            s.upscale_quality = app::UpscaleQuality::Quality;
            s.upscale_sharpness = 35;
            s.aniso = 4;
            break;
        case app::HardwarePreset::Mainstream1080p:
            s.upscaler = pick_best_temporal_or(app::UpscalerMode::Cas);
            s.upscale_quality =
                (desktop.height <= 1080) ? app::UpscaleQuality::NativeAA : app::UpscaleQuality::UltraQuality;
            s.upscale_sharpness = 25;
            s.aniso = 8;
            break;
        case app::HardwarePreset::High1440p:
            s.upscaler = pick_best_temporal_or(app::UpscalerMode::Cas);
            s.upscale_quality = app::UpscaleQuality::NativeAA;
            s.upscale_sharpness = 20;
            s.aniso = 16;
            break;
        case app::HardwarePreset::Ultra4k:
            s.upscaler = pick_best_temporal_or(app::UpscalerMode::Fsr1);
            s.upscale_quality = app::UpscaleQuality::Quality;
            s.upscale_sharpness = 15;
            s.aniso = 16;
            break;
        default:
            break;
    }
    app::MarkHardwarePresetUserEdited();
    app::MarkFpsCapUserEdited();
    app::Save();
}

void ResetAllSettingsToDefaults() {
    app::Settings& s = app::Get();
    std::string keep_iso = s.iso_path;
    s = app::Settings{};
    s.iso_path = keep_iso;
    SharedUiOptions& o = GetSharedUiOptions();
    o = SharedUiOptions{};
    input::SetTouchpadAsBack(o.touchpad_as_back);
    audio::SetOutputMode(s.audio_output);
    audio::SetMasterVolume(float(s.master_volume) / 100.0f);
    audio::SetMuted(s.audio_mute);
    app::MarkFpsCapUserEdited();
    app::MarkPresentModeUserEdited();
    app::MarkUpscalerUserEdited();
    app::MarkGpuAdapterUserEdited();
    app::MarkHardwarePresetUserEdited();
    MkbResetDefaults();
    app::Save();
}

void RefreshMonitorLists(std::vector<app::MonitorInfo>& monitors, std::vector<app::Mode>& resolutions,
                         std::string& res_device, const std::string& monitor_device) {
    if (monitors.empty()) monitors = app::EnumerateMonitors();
    if (resolutions.empty() || res_device != monitor_device) {
        resolutions = app::EnumerateResolutions(monitor_device);
        res_device = monitor_device;
    }
}

const app::GpuAdapterInfo* FindSelectedGpu(const std::vector<app::GpuAdapterInfo>& gpus, const std::string& key) {
    if (gpus.empty()) return nullptr;
    if (!key.empty() && LowerAscii(key) != "auto") {
        std::string lk = LowerAscii(key);
        for (const auto& g : gpus)
            if (!g.is_software && LowerAscii(g.key) == lk) return &g;
    }
    for (const auto& g : gpus)
        if (!g.is_software && g.d3d12_fl11) return &g;
    return &gpus.front();
}

void OpenDirectoryInExplorer(void* hwnd, const fs::path& dir, const char* label,
                             const std::function<void(const std::string&, bool)>& set_status) {
    std::error_code ec;
    fs::create_directories(dir, ec);
#ifdef _WIN32
    HINSTANCE res = ShellExecuteW(static_cast<HWND>(hwnd), L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (set_status) {
        if (reinterpret_cast<intptr_t>(res) <= 32) {
            set_status(std::string("Could not open ") + label + " folder: " + dir.string(), true);
        } else {
            set_status(std::string("Opened ") + label + " folder: " + dir.string(), false);
        }
    }
#else
    (void)hwnd;
    if (IsGamescope()) {
        if (set_status) {
            set_status(std::string(label) + " folder: " + dir.string() + " (browse in Desktop Mode)", false);
        }
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        pid_t grand = fork();
        if (grand == 0) {
            setsid();
            execlp("xdg-open", "xdg-open", dir.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) {
        int st = 0;
        (void)waitpid(pid, &st, 0);
        if (set_status) set_status(std::string("Opened ") + label + " folder: " + dir.string(), false);
    } else if (set_status) {
        set_status(std::string("Could not launch xdg-open for ") + label + ": " + dir.string(), true);
    }
#endif
}

void DrawUpscalingGraphicsPanel(const UiPanelContext& ctx) {
    app::Settings& s = app::Get();
    const auto* sel_gpu = ctx.gpus ? FindSelectedGpu(*ctx.gpus, s.gpu_adapter) : nullptr;
    upscale::PluginAvailability local_plugins{};
    const upscale::PluginAvailability& plugins = ctx.plugins ? *ctx.plugins : local_plugins;

    // 1. Hardware Preset quick bar
    if (!ctx.in_launcher) {
        ImGui::TextUnformatted("Hardware Preset:");
        static constexpr app::HardwarePreset kTiers[] = {
            app::HardwarePreset::HandheldIgpu, app::HardwarePreset::Mainstream1080p,
            app::HardwarePreset::High1440p, app::HardwarePreset::Ultra4k};
        for (app::HardwarePreset tier : kTiers) {
            ImGui::SameLine();
            bool active = (s.hardware_preset == tier);
            if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.54f, 0.15f, 0.17f, 1.0f));
            if (ImGui::SmallButton(HardwarePresetName(tier))) {
                ApplyHardwarePreset(tier, sel_gpu, plugins, app::Desktop());
                if (ctx.set_status) ctx.set_status(std::string("Applied preset: ") + HardwarePresetName(tier), false);
            }
            if (active) ImGui::PopStyleColor();
        }
        ImGui::Separator();
    }

    // 2. Upscaling Method combo (all 9 methods with live badges)
    auto badge_for = [&](app::UpscalerMode m) -> std::string {
#ifdef _WIN32
        switch (m) {
            case app::UpscalerMode::Dlss:
                return plugins.dlss_ready ? " [Ready]" : " [Plugin Needed -> FSR 1.0]";
            case app::UpscalerMode::Fsr3:
                return plugins.fsr3_ready ? " [Ready]" : " [Plugin Needed -> FSR 1.0]";
            case app::UpscalerMode::Xess:
                return plugins.xess_ready ? " [Ready]" : " [Plugin Needed -> FSR 1.0]";
            default:
                return " [Built-in]";
        }
#else
        switch (m) {
            case app::UpscalerMode::Dlss:
            case app::UpscalerMode::Fsr3:
            case app::UpscalerMode::Xess:
                return " [Windows D3D12 Only - Use FSR 1.0 / NIS on Native Linux Vulkan]";
            default:
                return " [Built-in Vulkan]";
        }
#endif
    };
    std::string cur_up = std::string(upscale::UpscalerModeName(s.upscaler)) + badge_for(s.upscaler);
    if (ImGui::BeginCombo("Upscaling Method", cur_up.c_str())) {
        static constexpr app::UpscalerMode kOrder[] = {
            app::UpscalerMode::Fsr1,    app::UpscalerMode::Nis,      app::UpscalerMode::Cas,
            app::UpscalerMode::Bicubic, app::UpscalerMode::Integer,  app::UpscalerMode::Bilinear,
            app::UpscalerMode::Dlss,    app::UpscalerMode::Fsr3,     app::UpscalerMode::Xess,
        };
        for (app::UpscalerMode m : kOrder) {
            std::string lbl = std::string(upscale::UpscalerModeName(m)) + badge_for(m);
            bool sel = (s.upscaler == m);
#ifndef _WIN32
            bool win_only = (m == app::UpscalerMode::Dlss || m == app::UpscalerMode::Fsr3 || m == app::UpscalerMode::Xess);
            if (win_only) ImGui::BeginDisabled(true);
#endif
            if (ImGui::Selectable(lbl.c_str(), sel)) {
                s.upscaler = m;
                s.hardware_preset = app::HardwarePreset::Custom;
                app::MarkUpscalerUserEdited();
                app::Save();
            }
#ifndef _WIN32
            if (win_only) ImGui::EndDisabled();
#endif
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
#ifdef _WIN32
                if (m == app::UpscalerMode::Dlss) ImGui::SetTooltip("%s", plugins.dlss_info.c_str());
                else if (m == app::UpscalerMode::Fsr3) ImGui::SetTooltip("%s", plugins.fsr3_info.c_str());
                else if (m == app::UpscalerMode::Xess) ImGui::SetTooltip("%s", plugins.xess_info.c_str());
#else
                if (win_only) {
                    ImGui::SetTooltip("Windows D3D12 Only - Use FSR 1.0 / NIS on Native Linux Vulkan");
                }
#endif
            }
        }
        ImGui::EndCombo();
    }

    // Plugin status hint line for temporal modes
#ifdef _WIN32
    if (s.upscaler == app::UpscalerMode::Dlss) {
        ImGui::TextColored(plugins.dlss_ready ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f) : ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                           "DLSS Status: %s", plugins.dlss_info.c_str());
    } else if (s.upscaler == app::UpscalerMode::Fsr3) {
        ImGui::TextColored(plugins.fsr3_ready ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f) : ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                           "FSR 3.1 Status: %s", plugins.fsr3_info.c_str());
    } else if (s.upscaler == app::UpscalerMode::Xess) {
        ImGui::TextColored(plugins.xess_ready ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f) : ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                           "XeSS Status: %s", plugins.xess_info.c_str());
    }
#else
    if (s.upscaler == app::UpscalerMode::Dlss || s.upscaler == app::UpscalerMode::Fsr3 ||
        s.upscaler == app::UpscalerMode::Xess) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.28f, 1.0f),
                           "Notice: Windows D3D12 Only - Use FSR 1.0 / NIS on Native Linux Vulkan (falling back to FSR 1.0)");
    }
#endif

    // 3. Quality Preset combo
    if (ImGui::BeginCombo("Quality Preset", upscale::UpscaleQualityName(s.upscale_quality))) {
        for (int q = 0; q <= int(app::UpscaleQuality::Custom); ++q) {
            auto qm = app::UpscaleQuality(q);
            if (ImGui::Selectable(upscale::UpscaleQualityName(qm), s.upscale_quality == qm)) {
                s.upscale_quality = qm;
                s.hardware_preset = app::HardwarePreset::Custom;
                app::MarkUpscalerUserEdited();
                app::Save();
            }
        }
        ImGui::EndCombo();
    }

    // Custom % slider (shown when Custom quality is selected)
    if (s.upscale_quality == app::UpscaleQuality::Custom) {
        int rp = int(s.render_percent);
        if (ImGui::SliderInt("Custom Render Scale (%)", &rp, 33, 200, "%d%%")) {
            s.render_percent = uint32_t(rp);
            s.hardware_preset = app::HardwarePreset::Custom;
            app::MarkUpscalerUserEdited();
            app::Save();
        }
        static constexpr uint32_t kQuick[] = {50, 67, 77, 100, 150, 200};
        ImGui::TextUnformatted("Quick Scale:");
        for (uint32_t p : kQuick) {
            ImGui::SameLine();
            char b[16];
            std::snprintf(b, sizeof(b), "%u%%", p);
            if (ImGui::SmallButton(b)) {
                s.render_percent = p;
                app::MarkUpscalerUserEdited();
                app::Save();
            }
        }
    }

    // 4. Sharpness slider
    int sharp = int(s.upscale_sharpness);
    if (ImGui::SliderInt("Upscaler Sharpness", &sharp, 0, 100, "%d%%")) {
        s.upscale_sharpness = uint32_t(sharp);
        app::MarkUpscalerUserEdited();
        app::Save();
    }

    // 5. DLSS Preset (shown when DLSS selected)
    if (s.upscaler == app::UpscalerMode::Dlss) {
        if (ImGui::BeginCombo("DLSS Model Preset", upscale::DlssPresetName(s.dlss_preset))) {
            for (int dp = 0; dp <= int(app::DlssPreset::PresetK); ++dp) {
                auto dm = app::DlssPreset(dp);
                if (ImGui::Selectable(upscale::DlssPresetName(dm), s.dlss_preset == dm)) {
                    s.dlss_preset = dm;
                    app::MarkUpscalerUserEdited();
                    app::Save();
                }
            }
            ImGui::EndCombo();
        }
    }

    if (ImGui::Checkbox("Square-Pixel 16:9 Internal Targets (Recommended)", &s.aspect_correct_render)) {
        app::MarkUpscalerUserEdited();
        app::Save();
    }

    // Live effective internal -> output resolution readout
    app::Mode out = app::Output();
    bool wide = (out.width * 2 >= out.height * 3);
    float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
    uint32_t vp_w = uint32_t(std::lround(std::min(float(out.width), float(out.height) * aspect)));
    uint32_t vp_h = uint32_t(std::lround(float(vp_w) / aspect));
    uint32_t rw = 0, rh = 0;
    upscale::ResolveUpscaleDimensions(s.upscaler, s.upscale_quality, s.render_percent, s.aspect_correct_render, vp_w,
                                      vp_h, 640, 480, rw, rh);
    ImGui::TextColored(ImVec4(0.68f, 0.86f, 1.0f, 1.0f), "Pipeline: %ux%u Internal -> %ux%u Game Viewport (%s) -> %ux%u Output",
                       rw, rh, vp_w, vp_h, wide ? "16:9" : "4:3", out.width, out.height);

#ifdef _WIN32
    if (ctx.on_open_plugins_folder) {
        if (ImGui::SmallButton("Open plugins\\ Folder (Bundled DLSS / FSR 3.1 / XeSS DLLs)")) {
            ctx.on_open_plugins_folder();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Vendor plugin DLLs & licenses live in plugins\\");
    }
#endif

    ImGui::Separator();
    const char* aniso_labels[] = {"Game Default", "2x", "4x", "8x", "16x"};
    const uint32_t aniso_vals[] = {0, 2, 4, 8, 16};
    int aniso_idx = 0;
    for (int i = 0; i < 5; ++i)
        if (s.aniso == aniso_vals[i]) aniso_idx = i;
    if (ImGui::Combo("Anisotropic Filtering", &aniso_idx, aniso_labels, 5)) {
        s.aniso = aniso_vals[aniso_idx];
        s.hardware_preset = app::HardwarePreset::Custom;
        app::MarkUpscalerUserEdited();
        app::Save();
    }
#ifdef _WIN32
    if (ImGui::Checkbox("Enable Custom Replacement Textures (textures\\replacement\\)", &s.custom_textures)) {
#else
    if (ImGui::Checkbox("Enable Custom Replacement Textures (textures/replacement/)", &s.custom_textures)) {
#endif
        app::Save();
    }

    ImGui::Separator();
    ImGui::TextUnformatted("Safe Frame-Rate Cap (Simulation-Fidelity Enforced):");
    bool cap60 = (s.fps_cap != 30);
    if (ImGui::RadioButton("60 FPS - Native Original Xbox Target (Recommended)", cap60)) {
        s.fps_cap = 60;
        app::MarkFpsCapUserEdited();
        app::Save();
    }
    if (ImGui::RadioButton("30 FPS - Half-Rate Cap (Battery / Low-Power)", !cap60)) {
        s.fps_cap = 30;
        app::MarkFpsCapUserEdited();
        app::Save();
    }
}

void DrawDisplayGpuPanel(const UiPanelContext& ctx) {
    app::Settings& s = app::Get();

    // 1. GPU Adapter selector
    std::string cur_gpu_label = "Auto (High Performance)";
    if (ctx.gpus && !s.gpu_adapter.empty()) {
        for (const auto& g : *ctx.gpus) {
            if (g.key == s.gpu_adapter) {
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s (%.1f GB %s)", g.name.c_str(),
                              double(g.dedicated_vram) / (1024.0 * 1024.0 * 1024.0), g.likely_uma ? "UMA" : "VRAM");
                cur_gpu_label = buf;
                break;
            }
        }
    }
    if (ImGui::BeginCombo("GPU Adapter", cur_gpu_label.c_str())) {
        if (ImGui::Selectable("Auto (High Performance)", s.gpu_adapter.empty())) {
            s.gpu_adapter.clear();
            app::MarkGpuAdapterUserEdited();
            app::Save();
        }
        if (ctx.gpus) {
            bool any_hw = false;
            for (const auto& g : *ctx.gpus) {
                if (!g.is_software && g.d3d12_fl11) {
                    any_hw = true;
                    break;
                }
            }
            for (const auto& g : *ctx.gpus) {
                if (!g.d3d12_fl11) continue;
                if (g.is_software && any_hw) continue;
                char buf[256];
                std::snprintf(buf, sizeof(buf), "%s (%.1f GB %s)", g.name.c_str(),
                              double(g.dedicated_vram) / (1024.0 * 1024.0 * 1024.0),
                              g.is_software ? "Software Vulkan" : (g.likely_uma ? "Integrated/UMA" : "Dedicated VRAM"));
                bool sel = (s.gpu_adapter == g.key);
                if (ImGui::Selectable(buf, sel)) {
                    s.gpu_adapter = g.key;
                    app::MarkGpuAdapterUserEdited();
                    app::Save();
                }
            }
        }
        ImGui::EndCombo();
    }
    if (!ctx.in_launcher) {
        ImGui::TextDisabled("Changing GPU adapter applies on next game launch.");
    }

    ImGui::Separator();
    const char* mode_names[] = {"Fullscreen", "Borderless", "Windowed"};
    int mode_idx = int(s.display_mode);
    if (ImGui::Combo("Display Mode", &mode_idx, mode_names, 3)) {
        s.display_mode = app::DisplayMode(mode_idx);
        app::Save();
        if (ctx.on_display_changed) ctx.on_display_changed();
    }

    // Monitor selector
    std::string cur_mon_label = "Primary / Auto";
    if (ctx.monitors) {
        for (const auto& m : *ctx.monitors) {
            if (m.device == s.monitor) {
                cur_mon_label = m.label;
                break;
            }
        }
    }
    if (ImGui::BeginCombo("Monitor", cur_mon_label.c_str())) {
        if (ImGui::Selectable("Primary / Auto", s.monitor.empty())) {
            s.monitor.clear();
            if (ctx.resolutions) ctx.resolutions->clear();
            app::Save();
            if (ctx.on_display_changed) ctx.on_display_changed();
        }
        if (ctx.monitors) {
            for (const auto& m : *ctx.monitors) {
                if (ImGui::Selectable(m.label.c_str(), s.monitor == m.device)) {
                    s.monitor = m.device;
                    if (ctx.resolutions) ctx.resolutions->clear();
                    app::Save();
                    if (ctx.on_display_changed) ctx.on_display_changed();
                }
            }
        }
        ImGui::EndCombo();
    }

    // Resolution selector
    if (s.display_mode == app::DisplayMode::Borderless) {
        app::Mode desk = app::Desktop();
        ImGui::BeginDisabled();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%ux%u (Monitor Native)", desk.width, desk.height);
        ImGui::LabelText("Screen Resolution", "%s", buf);
        ImGui::EndDisabled();
    } else {
        char cur_res[64];
        if (s.width && s.height) std::snprintf(cur_res, sizeof(cur_res), "%ux%u", s.width, s.height);
        else {
            app::Mode d = app::Desktop();
            std::snprintf(cur_res, sizeof(cur_res), "Desktop Native (%ux%u)", d.width, d.height);
        }
        if (ImGui::BeginCombo("Screen Resolution", cur_res)) {
            if (ImGui::Selectable("Desktop Native", s.width == 0 && s.height == 0)) {
                s.width = 0;
                s.height = 0;
                app::Save();
                if (ctx.on_display_changed) ctx.on_display_changed();
            }
            if (ctx.resolutions) {
                for (const auto& r : *ctx.resolutions) {
                    char opt[48];
                    std::snprintf(opt, sizeof(opt), "%ux%u", r.width, r.height);
                    if (ImGui::Selectable(opt, s.width == r.width && s.height == r.height)) {
                        s.width = r.width;
                        s.height = r.height;
                        app::Save();
                        if (ctx.on_display_changed) ctx.on_display_changed();
                    }
                }
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();
    if (ImGui::BeginCombo("Presentation / VSync Mode", PresentModeName(s.present_mode))) {
        for (int pm = 0; pm <= int(app::PresentMode::TearFreeMailbox); ++pm) {
            auto p = app::PresentMode(pm);
            if (ImGui::Selectable(PresentModeName(p), s.present_mode == p)) {
                s.present_mode = p;
                s.vsync = (p == app::PresentMode::Auto || p == app::PresentMode::VSync);
                app::MarkPresentModeUserEdited();
                app::Save();
            }
        }
        ImGui::EndCombo();
    }
    uint32_t hz = app::DisplayRefreshHz();
    ImGui::TextDisabled("Active display refresh: %u Hz  |  Auto uses integer VSync on 60/120/180/240 Hz and VRR on 75/90/144/165 Hz.",
                        hz);

    if (ImGui::Button("Refresh Monitors & GPUs")) {
        if (ctx.gpus) *ctx.gpus = app::EnumerateGpuAdapters();
        if (ctx.monitors) ctx.monitors->clear();
        if (ctx.resolutions) ctx.resolutions->clear();
    }
}

void DrawAudioPanel(const UiPanelContext& /*ctx*/) {
    app::Settings& s = app::Get();
    int vol = int(s.master_volume);
    if (ImGui::SliderInt("Master Volume (%)", &vol, 0, 100, "%d%%")) {
        s.master_volume = uint32_t(vol);
        audio::SetMasterVolume(float(s.master_volume) / 100.0f);
        app::Save();
    }
    if (ImGui::Checkbox("Mute All Audio", &s.audio_mute)) {
        audio::SetMuted(s.audio_mute);
        app::Save();
    }
    if (ImGui::Checkbox("Mute Audio When Window Is Unfocused", &s.mute_unfocused)) {
        if (!s.mute_unfocused) audio::SetFocusMuted(false);
        app::Save();
    }
    ImGui::Separator();
#ifdef _WIN32
    const char* spk_modes[] = {"Auto (Match Windows)", "Stereo (2.0)", "5.1 Surround", "Mono"};
#else
    const char* spk_modes[] = {"Auto (Match System)", "Stereo (2.0)", "5.1 Surround", "Mono"};
#endif
    int spk = int(s.audio_output);
    if (ImGui::Combo("Speaker Output Mode", &spk, spk_modes, 4)) {
        s.audio_output = uint32_t(spk);
        audio::SetOutputMode(s.audio_output);
        app::Save();
    }
#ifdef _WIN32
    ImGui::TextDisabled("Active mix: %s (48 kHz XAudio2, default processor)",
                        audio::IsMono() ? "Mono" : (audio::IsSurround() ? "5.1 Surround" : "Stereo"));
#else
    ImGui::TextDisabled("Active mix: %s (48 kHz SDL3 Audio)",
                        audio::IsMono() ? "Mono" : (audio::IsSurround() ? "5.1 Surround" : "Stereo"));
#endif
}

void DrawControlsPanel(const UiPanelContext& ctx) {
#ifdef _WIN32
    app::Settings& s = app::Get();
#endif
    SharedUiOptions& o = GetSharedUiOptions();
    MkbConfig& mkb = MkbGetConfig();

    // 1. Unified Input Mode selector + live M&K / Controller status badges
    const char* mode_items[] = {
        InputModeName(InputMode::Auto),
        InputModeName(InputMode::MouseKeyboardOnly),
        InputModeName(InputMode::ControllerOnly),
    };
    int cur_mode = int(mkb.input_mode);
    ImGui::SetNextItemWidth(std::min(430.0f, ImGui::GetContentRegionAvail().x * 0.65f));
    if (ImGui::Combo("Input Mode", &cur_mode, mode_items, 3)) {
        mkb.input_mode = static_cast<InputMode>(cur_mode);
        MkbMarkInputModeUserEdited();
        app::Save();
    }

    input::DeviceInfo dev = input::GetDeviceInfo(0);
    bool mkb_enabled = (mkb.input_mode != InputMode::ControllerOnly);
    ImGui::TextColored(mkb_enabled ? ImVec4(0.35f, 0.90f, 0.45f, 1.0f) : ImVec4(0.65f, 0.65f, 0.70f, 1.0f),
                       "Mouse & Keyboard: %s",
#ifdef _WIN32
                       mkb_enabled ? "Active (Win32 Raw Input, 1:1 Linear Aim)" : "Disabled (Controller Only)");
#else
                       mkb_enabled ? "Active (SDL3 Relative Mouse, 1:1 Linear Aim)" : "Disabled (Controller Only)");
#endif
    ImGui::SameLine(0.0f, 18.0f);
    if (dev.connected) {
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "Port 1 Pad: %s%s", dev.name.c_str(),
                           dev.is_dualsense ? " [DualSense]" : "");
    } else {
        ImGui::TextColored(ImVec4(0.92f, 0.72f, 0.30f, 1.0f), "Port 1 Pad: Not Connected");
    }

    ImGui::Spacing();
    if (ImGui::BeginTabBar("##ControlsSubTabs")) {
        // Sub-tab 1: Mouse & Keyboard (default)
        if (ImGui::BeginTabItem("Mouse & Keyboard")) {
            ImGui::Spacing();
            ImGui::BeginDisabled(!mkb_enabled);

            // Sensitivity slider with approximate cm/360 @ 800 DPI readout
            float approx_cm_360 = (4000.0f / std::max(mkb.mouse_sensitivity, 0.1f)) / 800.0f * 2.54f;
            char sens_fmt[64];
            std::snprintf(sens_fmt, sizeof(sens_fmt), "%.2fx  (~%.1f cm/360 @ 800 DPI)", mkb.mouse_sensitivity,
                          approx_cm_360);
            ImGui::SetNextItemWidth(std::min(360.0f, ImGui::GetContentRegionAvail().x * 0.60f));
            if (ImGui::SliderFloat("Mouse Sensitivity", &mkb.mouse_sensitivity, 0.10f, 5.00f, sens_fmt)) {
                MkbMarkMouseSensUserEdited();
                app::Save();
            }

            if (ImGui::Checkbox("Invert Mouse Y-Axis", &mkb.mouse_invert_y)) {
                MkbMarkMouseInvertYUserEdited();
                app::Save();
            }
            ImGui::SameLine(0.0f, 22.0f);
            if (ImGui::Checkbox("Raw 1:1 Mouse Aim (Bypass Controller Sticky Slowdown)", &mkb.mouse_raw_aim)) {
                app::Save();
            }
            if (ImGui::Checkbox("Mouse Smoothing (2-Frame EMA)", &mkb.mouse_smoothing)) {
                app::Save();
            }
            ImGui::SameLine(0.0f, 22.0f);
            if (ImGui::Button("Reset Mouse & Keybindings to Default")) {
                MkbCancelRebind();
                MkbResetDefaults();
                app::Save();
                if (ctx.set_status) ctx.set_status("Mouse & keyboard bindings reset to PC defaults.", false);
            }

            MkbAction rebind_act = MkbAction::MoveForward;
            int rebind_slot = 0;
            bool is_rebinding = MkbIsRebinding(&rebind_act, &rebind_slot);
            if (is_rebinding) {
                ImGui::TextColored(ImVec4(0.98f, 0.78f, 0.25f, 1.0f),
                                   "Listening for input on '%s' (%s) - Press Key / Mouse / Wheel (Esc Cancel, Del Clear)",
                                   MkbActionTable()[size_t(rebind_act)].label,
                                   rebind_slot == 0 ? "Primary" : "Secondary");
            } else {
                ImGui::TextDisabled(
                    "Click any slot to rebind (Esc cancels, Del clears). Menus: Enter=Confirm, Esc=Pause/Back, F1=Overlay.");
            }

            // Scrollable Click-to-Rebind Table
            float table_h = std::max(140.0f, ImGui::GetContentRegionAvail().y - 4.0f);
            constexpr ImGuiTableFlags kTblFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
            if (ImGui::BeginTable("##MkbBindingsTable", 4, kTblFlags, ImVec2(0.0f, table_h))) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.65f);
                ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 0.85f);
                ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 0.85f);
                ImGui::TableSetupColumn("Xbox Action", ImGuiTableColumnFlags_WidthStretch, 1.20f);
                ImGui::TableHeadersRow();

                const auto& table = MkbActionTable();
                for (size_t i = 0; i < table.size(); ++i) {
                    const MkbActionInfo& info = table[i];
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(info.label);

                    for (int slot = 0; slot < 2; ++slot) {
                        ImGui::TableSetColumnIndex(1 + slot);
                        bool capturing_this = is_rebinding && rebind_act == info.action && rebind_slot == slot;
                        std::string btn_label =
                            capturing_this ? "Press Input..." : BindSlotToDisplay(mkb.bindings[i][size_t(slot)]);
                        std::string btn_id = btn_label + "##rebind_" + std::to_string(i) + "_" + std::to_string(slot);
                        if (capturing_this) {
                            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.12f, 1.0f));
                        }
                        if (ImGui::Button(btn_id.c_str(), ImVec2(-1.0f, 0.0f))) {
                            MkbBeginRebind(info.action, slot);
                        }
                        if (capturing_this) {
                            ImGui::PopStyleColor();
                        }
                    }

                    ImGui::TableSetColumnIndex(3);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s", info.xbox_action);
                }
                ImGui::EndTable();
            }

            ImGui::EndDisabled();
            ImGui::EndTabItem();
        }

        // Sub-tab 2: Controller (DualSense / Xbox)
        if (ImGui::BeginTabItem("Controller (DualSense / Xbox)")) {
            ImGui::Spacing();
            if (dev.connected) {
                ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "Port 1 Connected: %s", dev.name.c_str());
                ImGui::TextDisabled("USB/BT ID: VID 0x%04X / PID 0x%04X  |  Backend: SDL3 %s%s", dev.vid, dev.pid,
                                    input::BackendLabel(), dev.is_dualsense ? "  [DualSense 5]" : "");
            } else {
                ImGui::TextColored(ImVec4(0.92f, 0.72f, 0.30f, 1.0f),
                                   "Port 1: No controller detected (plug in DualSense 5, Xbox, or Steam Deck pad)");
                ImGui::TextDisabled("Backend: SDL3 %s (auto-detects on hot-plug)", input::BackendLabel());
            }

            ImGui::Separator();
            if (ImGui::Checkbox("Enable Controller Rumble", &o.rumble_enabled)) {
                if (!o.rumble_enabled) input::Rumble(0, 0, 0);
                app::Save();
            }
            ImGui::BeginDisabled(!o.rumble_enabled);
            int rstr = int(o.rumble_strength);
            if (ImGui::SliderInt("Rumble Strength (%)", &rstr, 0, 100, "%d%%")) {
                o.rumble_strength = uint32_t(rstr);
                app::Save();
            }
            if (ctx.on_test_rumble && ImGui::Button("Test Rumble Pulse (300 ms)")) {
                ctx.on_test_rumble();
            }
            ImGui::EndDisabled();

            ImGui::Separator();
            int dz = int(o.stick_deadzone);
            if (ImGui::SliderInt("Extra Host Stick Deadzone (%)", &dz, 0, 30, "%d%%")) {
                o.stick_deadzone = uint32_t(dz);
                app::Save();
            }
            ImGui::TextDisabled("Applied on top of the game's built-in 30%% deadzone (bypassed during replay).");

            if (ImGui::Checkbox("Map DualSense Touchpad Click to Xbox BACK Button", &o.touchpad_as_back)) {
                input::SetTouchpadAsBack(o.touchpad_as_back);
                app::Save();
            }
#ifdef _WIN32
            if (ImGui::Checkbox("XInput Compatibility Mode (Steam Deck / Proton / 360 / Virtual Pads)",
                                &s.controller_xinput_compat)) {
                app::Save();
                if (ctx.in_launcher) {
                    input::ReinitIfNeeded();
                } else if (ctx.set_status) {
                    ctx.set_status("XInput compatibility setting saved (applies on next launch).", false);
                }
            }
#endif
            ImGui::TextDisabled("In-Game Overlay Shortcut: Keyboard F1  |  Controller BACK + L3");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

}  // namespace suffering
