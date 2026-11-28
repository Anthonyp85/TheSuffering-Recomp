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

#include "common/mkb_input.h"
#include "common/input_recorder.h"
#include "common/settings_ui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <toml++/toml.hpp>

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace suffering {

namespace {

namespace fs = std::filesystem;

#ifndef _WIN32
// Canonical Win32 VK numeric values preserved identically on Linux so settings.toml bindings are 100% cross-platform.
constexpr uint16_t VK_BACK = 0x08;
constexpr uint16_t VK_TAB = 0x09;
constexpr uint16_t VK_RETURN = 0x0D;
constexpr uint16_t VK_SHIFT = 0x10;
constexpr uint16_t VK_CONTROL = 0x11;
constexpr uint16_t VK_MENU = 0x12;
constexpr uint16_t VK_CAPITAL = 0x14;
constexpr uint16_t VK_ESCAPE = 0x1B;
constexpr uint16_t VK_SPACE = 0x20;
constexpr uint16_t VK_PRIOR = 0x21;
constexpr uint16_t VK_NEXT = 0x22;
constexpr uint16_t VK_END = 0x23;
constexpr uint16_t VK_HOME = 0x24;
constexpr uint16_t VK_LEFT = 0x25;
constexpr uint16_t VK_UP = 0x26;
constexpr uint16_t VK_RIGHT = 0x27;
constexpr uint16_t VK_DOWN = 0x28;
constexpr uint16_t VK_INSERT = 0x2D;
constexpr uint16_t VK_DELETE = 0x2E;
constexpr uint16_t VK_LWIN = 0x5B;
constexpr uint16_t VK_RWIN = 0x5C;
constexpr uint16_t VK_NUMPAD0 = 0x60;
constexpr uint16_t VK_NUMPAD9 = 0x69;
constexpr uint16_t VK_F1 = 0x70;
constexpr uint16_t VK_F4 = 0x73;
constexpr uint16_t VK_F12 = 0x7B;
constexpr uint16_t VK_LSHIFT = 0xA0;
constexpr uint16_t VK_RSHIFT = 0xA1;
constexpr uint16_t VK_LCONTROL = 0xA2;
constexpr uint16_t VK_RCONTROL = 0xA3;
constexpr uint16_t VK_LMENU = 0xA4;
constexpr uint16_t VK_RMENU = 0xA5;
constexpr uint16_t VK_OEM_1 = 0xBA;
constexpr uint16_t VK_OEM_PLUS = 0xBB;
constexpr uint16_t VK_OEM_COMMA = 0xBC;
constexpr uint16_t VK_OEM_MINUS = 0xBD;
constexpr uint16_t VK_OEM_PERIOD = 0xBE;
constexpr uint16_t VK_OEM_2 = 0xBF;
constexpr uint16_t VK_OEM_3 = 0xC0;
constexpr uint16_t VK_OEM_4 = 0xDB;
constexpr uint16_t VK_OEM_5 = 0xDC;
constexpr uint16_t VK_OEM_6 = 0xDD;
constexpr uint16_t VK_OEM_7 = 0xDE;

uint16_t SdlScancodeToVk(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return uint16_t('A' + (sc - SDL_SCANCODE_A));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return uint16_t('1' + (sc - SDL_SCANCODE_1));
    if (sc == SDL_SCANCODE_0) return uint16_t('0');
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return uint16_t(VK_F1 + (sc - SDL_SCANCODE_F1));
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return uint16_t(VK_NUMPAD0 + 1 + (sc - SDL_SCANCODE_KP_1));
    if (sc == SDL_SCANCODE_KP_0) return VK_NUMPAD0;
    switch (sc) {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return VK_RETURN;
        case SDL_SCANCODE_ESCAPE: return VK_ESCAPE;
        case SDL_SCANCODE_BACKSPACE: return VK_BACK;
        case SDL_SCANCODE_TAB: return VK_TAB;
        case SDL_SCANCODE_SPACE: return VK_SPACE;
        case SDL_SCANCODE_MINUS: return VK_OEM_MINUS;
        case SDL_SCANCODE_EQUALS: return VK_OEM_PLUS;
        case SDL_SCANCODE_LEFTBRACKET: return VK_OEM_4;
        case SDL_SCANCODE_RIGHTBRACKET: return VK_OEM_6;
        case SDL_SCANCODE_BACKSLASH:
        case SDL_SCANCODE_NONUSHASH: return VK_OEM_5;
        case SDL_SCANCODE_SEMICOLON: return VK_OEM_1;
        case SDL_SCANCODE_APOSTROPHE: return VK_OEM_7;
        case SDL_SCANCODE_GRAVE: return VK_OEM_3;
        case SDL_SCANCODE_COMMA: return VK_OEM_COMMA;
        case SDL_SCANCODE_PERIOD: return VK_OEM_PERIOD;
        case SDL_SCANCODE_SLASH: return VK_OEM_2;
        case SDL_SCANCODE_CAPSLOCK: return VK_CAPITAL;
        case SDL_SCANCODE_INSERT: return VK_INSERT;
        case SDL_SCANCODE_HOME: return VK_HOME;
        case SDL_SCANCODE_PAGEUP: return VK_PRIOR;
        case SDL_SCANCODE_DELETE: return VK_DELETE;
        case SDL_SCANCODE_END: return VK_END;
        case SDL_SCANCODE_PAGEDOWN: return VK_NEXT;
        case SDL_SCANCODE_RIGHT: return VK_RIGHT;
        case SDL_SCANCODE_LEFT: return VK_LEFT;
        case SDL_SCANCODE_DOWN: return VK_DOWN;
        case SDL_SCANCODE_UP: return VK_UP;
        case SDL_SCANCODE_LCTRL: return VK_LCONTROL;
        case SDL_SCANCODE_LSHIFT: return VK_LSHIFT;
        case SDL_SCANCODE_LALT: return VK_LMENU;
        case SDL_SCANCODE_LGUI: return VK_LWIN;
        case SDL_SCANCODE_RCTRL: return VK_RCONTROL;
        case SDL_SCANCODE_RSHIFT: return VK_RSHIFT;
        case SDL_SCANCODE_RALT: return VK_RMENU;
        case SDL_SCANCODE_RGUI: return VK_RWIN;
        default: return 0;
    }
}

std::vector<std::string> ReadProcCmdlineArgs() {
    std::vector<std::string> args;
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
    return args;
}
#endif

// Verified read-only guest .data addresses for The Suffering (USA, Title ID 0x4D570015, XDK 5788.1)
constexpr uint32_t kVaGameSessionState = 0x002DB204u;  // 1=Title/Frontend, 2=In-Level, 3=LeaveGame
constexpr uint32_t kVaEngineModeFlags = 0x002DC5E4u;   // bit0=UI/render, bit2=world active, bit3=FMV
constexpr uint32_t kVaLoadingScreenPtr = 0x002DC1F4u;  // nonzero while loading screen active
constexpr uint32_t kVaCameraPtr = 0x002DC754u;         // *(u32*) -> +0x00..+0x3F matrix, +0x8C/+0x90/+0x94 pos
constexpr uint32_t kVaLevelFlags = 0x002DEF14u;        // bit1=level loaded
constexpr uint32_t kVaPauseFlags = 0x002E0274u;        // bit0=paused, bit1=transition/input-blocked
constexpr uint32_t kVaUiMgrPtr = 0x002DAD78u;          // *(u32*) -> +0x50 int32 stack_count
constexpr uint32_t kVaObjMgrPtr = 0x002D0DC0u;         // *(u32*) -> +0x12C count, +0x124 -> *ent -> +0x60 player -> +0x9C

// Base angular calibration for mouse look at mouse_sensitivity = 1.0 (~0.09 deg/count => ~4000 counts/360 deg).
// Empirically calibrated in live 3D gameplay (1a PrisonDeathRow) for exact 1:1 horizontal & vertical feel.
// The guest yaw/pitch routines integrate (rate_deg_per_sec * dt * val), so dividing by (dt * guest_sens)
// cancels dt and guest_sens algebraically for exact framerate-independent 1:1 mouse movement.
constexpr float kYawScaleAt1x = 0.000384f;
constexpr float kPitchScaleAt1x = 0.00225f;

// Wheel notch pulse timing (in ~60 Hz guest polls)
constexpr int kWheelHoldPolls = 3;
constexpr int kWheelGapPolls = 2;
constexpr int kMaxWheelQueue = 8;

const std::array<MkbActionInfo, kMkbActionCount> kActionTable = {{
    {MkbAction::MoveForward, "move_forward", "Move Forward", "Left Stick Up"},
    {MkbAction::MoveBack, "move_back", "Move Backward", "Left Stick Down"},
    {MkbAction::MoveLeft, "move_left", "Strafe Left", "Left Stick Left"},
    {MkbAction::MoveRight, "move_right", "Strafe Right", "Left Stick Right"},
    {MkbAction::Fire, "fire", "Fire / Attack", "Right Trigger (RT)"},
    {MkbAction::ThrowGrenade, "throw_grenade", "Throw / Creature Swipe", "Left Trigger (LT)"},
    {MkbAction::Jump, "jump", "Jump / Leap / Dismount", "A Button"},
    {MkbAction::Reload, "reload", "Reload", "B Button"},
    {MkbAction::Interact, "interact", "Use / Interact (Hold: Crate)", "X Button"},
    {MkbAction::Flashlight, "flashlight", "Flashlight Toggle", "R3 (Scheme 1)"},
    {MkbAction::HealXombium, "heal_xombium", "Heal (Xombium)", "Black Button"},
    {MkbAction::InsanityTransform, "insanity_transform", "Insanity / Creature Toggle", "Y Button (Tap)"},
    {MkbAction::CameraToggle, "camera_toggle", "1st / 3rd Person Camera", "White Button"},
    {MkbAction::ArchivesMap, "archives_map", "Archives / Map / Journal", "Back Button"},
    {MkbAction::WeaponCatUp, "weapon_cat_up", "Inventory Category Up", "D-Pad Up"},
    {MkbAction::ItemCatDown, "item_cat_down", "Inventory Category Down", "D-Pad Down"},
    {MkbAction::PrevItemLeft, "prev_item_left", "Previous Weapon / Item", "D-Pad Left"},
    {MkbAction::NextItemRight, "next_item_right", "Next Weapon / Item", "D-Pad Right"},
}};

BindSlot MakeKey(uint16_t vk) { return BindSlot{BindKind::Key, vk}; }
BindSlot MakeMouse(BindKind k) { return BindSlot{k, 0}; }

void PopulateDefaultBindings(std::array<std::array<BindSlot, 2>, kMkbActionCount>& b) {
    for (auto& pair : b) pair = {BindSlot{}, BindSlot{}};
    auto set = [&](MkbAction a, BindSlot p, BindSlot s = {}) {
        b[size_t(a)][0] = p;
        b[size_t(a)][1] = s;
    };
    set(MkbAction::MoveForward, MakeKey('W'));
    set(MkbAction::MoveBack, MakeKey('S'));
    set(MkbAction::MoveLeft, MakeKey('A'));
    set(MkbAction::MoveRight, MakeKey('D'));
    set(MkbAction::Fire, MakeMouse(BindKind::MouseLeft));
    set(MkbAction::ThrowGrenade, MakeMouse(BindKind::MouseRight), MakeKey('G'));
    set(MkbAction::Jump, MakeKey(VK_SPACE));
    set(MkbAction::Reload, MakeKey('R'));
    set(MkbAction::Interact, MakeKey('E'));
    set(MkbAction::Flashlight, MakeKey('F'), MakeKey('L'));
    set(MkbAction::HealXombium, MakeKey('Q'), MakeKey('H'));
    set(MkbAction::InsanityTransform, MakeKey('X'), MakeMouse(BindKind::Mouse4));
    set(MkbAction::CameraToggle, MakeKey('C'), MakeMouse(BindKind::MouseMiddle));
    set(MkbAction::ArchivesMap, MakeKey(VK_TAB), MakeKey('M'));
    set(MkbAction::WeaponCatUp, MakeKey('1'), MakeKey(VK_UP));
    set(MkbAction::ItemCatDown, MakeKey('2'), MakeKey(VK_DOWN));
    set(MkbAction::PrevItemLeft, MakeMouse(BindKind::WheelUp), MakeKey('3'));
    set(MkbAction::NextItemRight, MakeMouse(BindKind::WheelDown), MakeKey('4'));
}

MkbConfig MakeDefaultConfig() {
    MkbConfig c{};
    c.input_mode = InputMode::Auto;
    c.mouse_sensitivity = 1.0f;
    c.mouse_invert_y = false;
    c.mouse_raw_aim = true;
    c.mouse_smoothing = false;
    PopulateDefaultBindings(c.bindings);
    return c;
}

enum class EscLatch : uint8_t {
    None = 0,
    SkipA = 1,
    PauseStart = 2,
    MenuBackB = 3,
};

struct SyntheticEvent {
    uint64_t at_ms = 0;
    std::string cmd;
    std::string arg1;
    int val1 = 0;
    int val2 = 0;
    bool fired = false;
};

struct MkbRuntimeState {
    MkbConfig cfg = MakeDefaultConfig();

    // Non-persistent CLI override protection (mirrors settings.cpp)
    bool cli_input_mode_overridden = false;
    InputMode saved_input_mode = InputMode::Auto;
    bool cli_mouse_sens_overridden = false;
    float saved_mouse_sens = 1.0f;
    bool cli_mouse_invert_y_overridden = false;
    bool saved_mouse_invert_y = false;

    // Live input state (protected by input_lock)
    std::mutex input_lock;
    std::array<bool, 256> keys{};
    std::array<bool, 5> mouse_buttons{};  // 0=L, 1=R, 2=M, 3=X1, 4=X2
    uint8_t suppressed_mouse_mask = 0;    // bits 0..4: latched on menu click or menu->3D transition until button-up
    bool prev_in_3d_gameplay = false;
    int cutscene_lmb_skip_polls = 0;
    double raw_dx = 0.0;
    double raw_dy = 0.0;
    float smooth_dx = 0.0f;
    float smooth_dy = 0.0f;
    int wheel_up_queue = 0;
    int wheel_down_queue = 0;
    int wheel_up_hold = 0;
    int wheel_up_gap = 0;
    int wheel_down_hold = 0;
    int wheel_down_gap = 0;
    bool esc_was_down = false;
    EscLatch esc_latch = EscLatch::None;

    // Native menu mouse state (protected by input_lock)
    float menu_phys_x = 0.0f;
    float menu_phys_y = 0.0f;
    int menu_last_px = INT_MIN;
    int menu_last_py = INT_MIN;
    uint64_t menu_move_seq = 0;
    uint64_t menu_consumed_move_seq = 0;
    bool menu_has_pos = false;
    bool menu_lmb_pressed = false;
    bool menu_lmb_down = false;
    bool menu_lmb_released = false;
    bool menu_rmb_pressed = false;
    int menu_wheel_steps = 0;
    int menu_wheel_rem = 0;
    float menu_wheel_frem = 0.0f;
    bool menu_captured_hwnd = false;

    // Last observed menu UI state for telemetry (updated on guest thread)
    std::atomic<uint32_t> dbg_top_vt{0};
    std::atomic<int32_t> dbg_focused_idx{-1};
    std::atomic<float> dbg_guest_gx{0.0f};
    std::atomic<float> dbg_guest_gy{0.0f};
    std::atomic<bool> dbg_in_vp{false};

    // Focus, window, & cursor capture state
    std::atomic<void*> game_hwnd{nullptr};
    std::atomic<bool> focused{true};
    std::atomic<bool> in_sizemove{false};
    std::atomic<bool> cursor_captured{false};
    std::atomic<bool> last_desired_capture{false};
    std::atomic<bool> mouse_look_active{false};
    std::atomic<bool> scheme1_applied{false};

    // Rebind UI capture state
    std::mutex rebind_lock;
    bool rebinding = false;
    MkbAction rebind_action = MkbAction::MoveForward;
    int rebind_slot = 0;
    std::chrono::steady_clock::time_point rebind_armed_at{};
    bool has_rebind_finished = false;
    std::chrono::steady_clock::time_point rebind_finished_at{};

    // Synthetic M&K verification harness (--test-mkb / --mkb-telemetry)
    bool test_active = false;
    std::vector<SyntheticEvent> test_events;
    bool test_clock_started = false;
    std::chrono::steady_clock::time_point test_t0{};
    FILE* telemetry_file = nullptr;
    uint64_t last_telemetry_poll = UINT64_MAX;
    bool test_exit_posted = false;
    std::string pending_event_tag;
    uint64_t pending_event_ms = 0;
};

MkbRuntimeState& Rt() {
    static MkbRuntimeState s;
    return s;
}

std::string LowerAscii(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

#ifdef _WIN32
std::vector<std::wstring> SplitCmdArgs(const wchar_t* cmd) {
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

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}
#endif

bool SafeReadU32(uint32_t va, uint32_t& out) {
    if (!kernel::g_base || va < 0x10000u || !mem::IsCommitted(va)) return false;
    out = kernel::R32(va);
    return true;
}

bool SafeReadF32(uint32_t va, float& out) {
    uint32_t raw = 0;
    if (!SafeReadU32(va, raw)) return false;
    std::memcpy(&out, &raw, 4);
    return std::isfinite(out);
}

#ifdef _WIN32
uint16_t NormalizeVirtualKey(WPARAM wp, LPARAM lp) {
    uint16_t vk = uint16_t(wp & 0xFFu);
    UINT sc = UINT((lp >> 16) & 0xFFu);
    bool ext = ((lp >> 24) & 1) != 0;
    if (vk == VK_SHIFT) {
        UINT mapped = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
        if (mapped == VK_LSHIFT || mapped == VK_RSHIFT) return uint16_t(mapped);
        return VK_LSHIFT;
    }
    if (vk == VK_CONTROL) return ext ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return ext ? VK_RMENU : VK_LMENU;
    return vk;
}
#endif

void ClearMenuPendingLocked(MkbRuntimeState& rt) {
    rt.menu_lmb_pressed = false;
    rt.menu_lmb_down = false;
    rt.menu_lmb_released = false;
    rt.menu_rmb_pressed = false;
    rt.menu_wheel_steps = 0;
    rt.menu_wheel_rem = 0;
    rt.menu_wheel_frem = 0.0f;
}

void UpdateMenuCursorPhysLocked(MkbRuntimeState& rt, float phys_x, float phys_y) {
    rt.menu_phys_x = phys_x;
    rt.menu_phys_y = phys_y;
    rt.menu_has_pos = true;
    int ix = int(std::lround(phys_x));
    int iy = int(std::lround(phys_y));
    if (ix != rt.menu_last_px || iy != rt.menu_last_py) {
        rt.menu_last_px = ix;
        rt.menu_last_py = iy;
        ++rt.menu_move_seq;
    }
}

void ReleaseMenuDragCapture(MkbRuntimeState& rt, void* hwnd_ptr) {
    if (!rt.menu_captured_hwnd) return;
    rt.menu_captured_hwnd = false;
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(hwnd_ptr);
    if (hwnd && GetCapture() == hwnd) ReleaseCapture();
#else
    (void)hwnd_ptr;
    SDL_CaptureMouse(false);
#endif
}

void ClientToPhysicalCoords(void* hwnd_ptr, float cx, float cy, float& out_px, float& out_py) {
    gpu::UpscalerStatus us = gpu::GetUpscalerStatus();
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(hwnd_ptr);
    RECT rc{};
    if (hwnd && GetClientRect(hwnd, &rc)) {
        int cw = rc.right - rc.left;
        int ch = rc.bottom - rc.top;
        if (cw > 0 && ch > 0 && us.bb_w > 0 && us.bb_h > 0) {
            out_px = cx * (float(us.bb_w) / float(cw));
            out_py = cy * (float(us.bb_h) / float(ch));
            return;
        }
    }
    out_px = cx;
    out_py = cy;
#else
    SDL_Window* win = static_cast<SDL_Window*>(hwnd_ptr);
    if (win) {
        int lw = 0, lh = 0;
        SDL_GetWindowSize(win, &lw, &lh);
        uint32_t bw = us.bb_w;
        uint32_t bh = us.bb_h;
        if (bw == 0 || bh == 0) {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(win, &pw, &ph);
            bw = uint32_t(std::max(pw, 0));
            bh = uint32_t(std::max(ph, 0));
        }
        if (lw > 0 && lh > 0 && bw > 0 && bh > 0) {
            out_px = cx * (float(bw) / float(lw));
            out_py = cy * (float(bh) / float(lh));
            return;
        }
    }
    out_px = cx;
    out_py = cy;
#endif
}

void ClearAllHeldInputsLocked(MkbRuntimeState& rt) {
    rt.keys.fill(false);
    rt.mouse_buttons.fill(false);
    rt.suppressed_mouse_mask = 0;
    rt.cutscene_lmb_skip_polls = 0;
    rt.raw_dx = 0.0;
    rt.raw_dy = 0.0;
    rt.smooth_dx = 0.0f;
    rt.smooth_dy = 0.0f;
    rt.wheel_up_queue = 0;
    rt.wheel_down_queue = 0;
    rt.wheel_up_hold = 0;
    rt.wheel_up_gap = 0;
    rt.wheel_down_hold = 0;
    rt.wheel_down_gap = 0;
    rt.esc_was_down = false;
    rt.esc_latch = EscLatch::None;
    ClearMenuPendingLocked(rt);
}

bool IsKeyPressedLocked(const MkbRuntimeState& rt, uint16_t vk) {
    if (vk >= 256) return false;
    if (rt.keys[vk]) return true;
    // Allow generic VK_SHIFT / VK_CONTROL / VK_MENU bindings to match either side
    if (vk == VK_SHIFT) return rt.keys[VK_LSHIFT] || rt.keys[VK_RSHIFT];
    if (vk == VK_CONTROL) return rt.keys[VK_LCONTROL] || rt.keys[VK_RCONTROL];
    if (vk == VK_MENU) return rt.keys[VK_LMENU] || rt.keys[VK_RMENU];
    return false;
}

bool IsSlotActiveLocked(const MkbRuntimeState& rt, const BindSlot& slot) {
    switch (slot.kind) {
        case BindKind::None: return false;
        case BindKind::Key: return IsKeyPressedLocked(rt, slot.vk);
        case BindKind::MouseLeft: return rt.mouse_buttons[0] && ((rt.suppressed_mouse_mask & uint8_t(1u << 0)) == 0u);
        case BindKind::MouseRight: return rt.mouse_buttons[1] && ((rt.suppressed_mouse_mask & uint8_t(1u << 1)) == 0u);
        case BindKind::MouseMiddle: return rt.mouse_buttons[2] && ((rt.suppressed_mouse_mask & uint8_t(1u << 2)) == 0u);
        case BindKind::Mouse4: return rt.mouse_buttons[3] && ((rt.suppressed_mouse_mask & uint8_t(1u << 3)) == 0u);
        case BindKind::Mouse5: return rt.mouse_buttons[4] && ((rt.suppressed_mouse_mask & uint8_t(1u << 4)) == 0u);
        case BindKind::WheelUp: return rt.wheel_up_hold > 0;
        case BindKind::WheelDown: return rt.wheel_down_hold > 0;
    }
    return false;
}

bool IsActionActiveLocked(const MkbRuntimeState& rt, MkbAction a) {
    const auto& slots = rt.cfg.bindings[size_t(a)];
    return IsSlotActiveLocked(rt, slots[0]) || IsSlotActiveLocked(rt, slots[1]);
}

void AdvanceWheelQueue(int& queue, int& hold, int& gap) {
    if (hold > 0) {
        --hold;
        if (hold == 0 && queue > 0) gap = kWheelGapPolls;
        return;
    }
    if (gap > 0) {
        --gap;
        return;
    }
    if (queue > 0) {
        --queue;
        hold = kWheelHoldPolls;
    }
}

#ifdef _WIN32
void ApplyClipRect(HWND hwnd, bool capture) {
    MkbRuntimeState& rt = Rt();
    if (suffering::IsPadOnlyInputSession()) return;
    if (capture && hwnd && IsWindow(hwnd)) {
        ReleaseMenuDragCapture(rt, hwnd);
        RECT cr{};
        if (GetClientRect(hwnd, &cr) && cr.right > cr.left + 2 && cr.bottom > cr.top + 2) {
            POINT tl{cr.left + 1, cr.top + 1};
            POINT br{cr.right - 1, cr.bottom - 1};
            ClientToScreen(hwnd, &tl);
            ClientToScreen(hwnd, &br);
            RECT clip{tl.x, tl.y, br.x, br.y};
            ClipCursor(&clip);
            SetCursor(nullptr);
            rt.cursor_captured.store(true, std::memory_order_release);
            return;
        }
    }
    ClipCursor(nullptr);
    rt.cursor_captured.store(false, std::memory_order_release);
    if (hwnd && IsWindow(hwnd)) {
        bool show = UiIsOverlayOpen() || rt.cfg.input_mode != InputMode::ControllerOnly;
        if (show) {
            CURSORINFO ci{};
            ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) == 0) {
                for (int i = 0; i < 8 && ShowCursor(TRUE) < 0; ++i) {
                }
            }
        }
        POINT pt{};
        RECT rc{};
        if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt) && GetClientRect(hwnd, &rc) && PtInRect(&rc, pt)) {
            SetCursor(show ? LoadCursorW(nullptr, IDC_ARROW) : nullptr);
        }
    }
}
#else
void ApplyClipRect(void* win_ptr, bool capture) {
    MkbRuntimeState& rt = Rt();
    if (suffering::IsPadOnlyInputSession()) return;
    SDL_Window* win = static_cast<SDL_Window*>(win_ptr);
    if (capture && win) {
        ReleaseMenuDragCapture(rt, win_ptr);
        SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
        if (SDL_SetWindowRelativeMouseMode(win, true)) {
            rt.cursor_captured.store(true, std::memory_order_release);
            return;
        }
    }
    if (win) SDL_SetWindowRelativeMouseMode(win, false);
    rt.cursor_captured.store(false, std::memory_order_release);
    if (win) {
        bool show = UiIsOverlayOpen() || rt.cfg.input_mode != InputMode::ControllerOnly;
        if (show) SDL_ShowCursor();
        else SDL_HideCursor();
    }
}
#endif

void WriteTelemetryLine(MkbRuntimeState& rt, uint64_t poll_id, uint64_t ms, const GuestContext& gc,
                        const input::Pad& pad, bool overlay_open, const char* event_tag) {
    if (!rt.telemetry_file) return;
    float cam_x = 0.0f, cam_y = 0.0f, cam_z = 0.0f;
    float fwd_x = 0.0f, fwd_y = 0.0f, fwd_z = 0.0f;
    uint32_t cam_ptr = 0;
    if (SafeReadU32(kVaCameraPtr, cam_ptr) && cam_ptr >= 0x10000u) {
        // Matrix rows at +0x00..+0x2F (row 2 = forward vector +0x20/+0x24/+0x28), position at +0x8C/+0x90/+0x94
        SafeReadF32(cam_ptr + 0x20u, fwd_x);
        SafeReadF32(cam_ptr + 0x24u, fwd_y);
        SafeReadF32(cam_ptr + 0x28u, fwd_z);
        SafeReadF32(cam_ptr + 0x8Cu, cam_x);
        SafeReadF32(cam_ptr + 0x90u, cam_y);
        SafeReadF32(cam_ptr + 0x94u, cam_z);
    }
    float pitch_deg = 0.0f;
    uint32_t obj = 0, ent_arr = 0, ent = 0, player = 0;
    if (SafeReadU32(kVaObjMgrPtr, obj) && obj >= 0x10000u) {
        uint32_t cnt = 0;
        if (SafeReadU32(obj + 0x12Cu, cnt) && int32_t(cnt) > 0 && SafeReadU32(obj + 0x124u, ent_arr) &&
            SafeReadU32(ent_arr, ent) && ent >= 0x10000u && SafeReadU32(ent + 0x60u, player) && player >= 0x10000u) {
            SafeReadF32(player + 0x59Au, pitch_deg);
        }
    }
    float audio_sfx = -1.0f;
    float sens_x = -1.0f;
    SafeReadF32(0x002DB218u + 0x0Cu, audio_sfx);
    SafeReadF32(0x002DB23Cu, sens_x);
    uint8_t suppressed_mask = 0;
    {
        std::lock_guard<std::mutex> lock(rt.input_lock);
        suppressed_mask = rt.suppressed_mouse_mask;
    }
    uint32_t top_vt = rt.dbg_top_vt.load(std::memory_order_relaxed);
    int32_t focus_idx = rt.dbg_focused_idx.load(std::memory_order_relaxed);
    float guest_gx = rt.dbg_guest_gx.load(std::memory_order_relaxed);
    float guest_gy = rt.dbg_guest_gy.load(std::memory_order_relaxed);
    bool in_vp = rt.dbg_in_vp.load(std::memory_order_relaxed);
    std::fprintf(
        rt.telemetry_file,
        "{\"poll\":%llu,\"ms\":%llu,\"session\":%u,\"mode\":%u,\"pause\":%u,\"ui_stack\":%d,\"loading\":%u,"
        "\"fmv\":%s,\"cutscene\":%s,\"in_3d\":%s,\"overlay\":%s,\"focused\":%s,\"should_capture\":%s,"
        "\"captured\":%s,\"mouse_look\":%s,\"scheme1\":%s,\"pad_conn\":%s,\"pad_btns\":%u,\"pad_a0\":%u,"
        "\"pad_a1\":%u,\"pad_a2\":%u,\"pad_a3\":%u,\"pad_a4\":%u,\"pad_a5\":%u,\"pad_lt\":%u,\"pad_rt\":%u,"
        "\"pad_lx\":%d,\"pad_ly\":%d,\"cam_pos\":[%.4f,%.4f,%.4f],\"cam_fwd\":[%.6f,%.6f,%.6f],"
        "\"pitch\":%.5f,\"top_vt\":%u,\"focus_idx\":%d,\"suppressed_mask\":%u,"
        "\"guest_gx\":%.2f,\"guest_gy\":%.2f,\"in_vp\":%s,\"audio_sfx\":%.3f,\"sens_x\":%.3f,\"event\":\"%s\"}\n",
        (unsigned long long)poll_id, (unsigned long long)ms, gc.session, gc.mode, gc.pause_flags, gc.ui_stack,
        gc.loading_ptr != 0 ? 1u : 0u, gc.fmv ? "true" : "false", gc.cutscene ? "true" : "false",
        gc.in_active_3d_gameplay ? "true" : "false", overlay_open ? "true" : "false",
        rt.focused.load(std::memory_order_relaxed) ? "true" : "false", MkbShouldCaptureCursor() ? "true" : "false",
        rt.cursor_captured.load(std::memory_order_relaxed) ? "true" : "false",
        rt.mouse_look_active.load(std::memory_order_relaxed) ? "true" : "false",
        rt.scheme1_applied.load(std::memory_order_relaxed) ? "true" : "false", pad.connected ? "true" : "false",
        unsigned(pad.buttons), unsigned(pad.analog[0]), unsigned(pad.analog[1]), unsigned(pad.analog[2]),
        unsigned(pad.analog[3]), unsigned(pad.analog[4]), unsigned(pad.analog[5]), unsigned(pad.analog[6]),
        unsigned(pad.analog[7]), int(pad.lx), int(pad.ly), cam_x, cam_y, cam_z, fwd_x, fwd_y, fwd_z, pitch_deg,
        top_vt, focus_idx, unsigned(suppressed_mask), guest_gx, guest_gy, in_vp ? "true" : "false", audio_sfx,
        sens_x, event_tag ? event_tag : "");
    std::fflush(rt.telemetry_file);
}

void LoadSyntheticTestScript(MkbRuntimeState& rt, const fs::path& script_path) {
    std::ifstream f(script_path);
    if (!f) {
        LOGW("mkb: failed to open --test-mkb script: %s", script_path.string().c_str());
        return;
    }
    std::string line;
    while (std::getline(f, line)) {
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::istringstream iss(line);
        SyntheticEvent ev{};
        if (!(iss >> ev.at_ms >> ev.cmd)) continue;
        ev.cmd = LowerAscii(ev.cmd);
        if (ev.cmd == "mouse" || ev.cmd == "menu_cursor") {
            iss >> ev.val1 >> ev.val2;
        } else if (ev.cmd == "wheel" || ev.cmd == "menu_wheel" || ev.cmd == "overlay" || ev.cmd == "focus") {
            iss >> ev.val1;
        } else if (ev.cmd == "keydown" || ev.cmd == "keyup" || ev.cmd == "mbtn_down" || ev.cmd == "mbtn_up") {
            iss >> ev.arg1;
        }
        rt.test_events.push_back(std::move(ev));
    }
    rt.test_active = !rt.test_events.empty();
    LOGR("mkb: loaded %zu synthetic M&K test events from %s", rt.test_events.size(), script_path.string().c_str());
}

}  // namespace

const char* InputModeName(InputMode mode) {
    switch (mode) {
        case InputMode::Auto: return "Auto (Mouse/KB + Controller - Instant Hot-Switch)";
        case InputMode::MouseKeyboardOnly: return "Mouse & Keyboard Only";
        case InputMode::ControllerOnly: return "Controller Only";
    }
    return "Auto (Mouse/KB + Controller - Instant Hot-Switch)";
}

const char* InputModeToken(InputMode mode) {
    switch (mode) {
        case InputMode::Auto: return "auto";
        case InputMode::MouseKeyboardOnly: return "mkb";
        case InputMode::ControllerOnly: return "controller";
    }
    return "auto";
}

const std::array<MkbActionInfo, kMkbActionCount>& MkbActionTable() { return kActionTable; }

std::string BindSlotToToken(const BindSlot& slot) {
    switch (slot.kind) {
        case BindKind::None: return "None";
        case BindKind::MouseLeft: return "MouseLeft";
        case BindKind::MouseRight: return "MouseRight";
        case BindKind::MouseMiddle: return "MouseMiddle";
        case BindKind::Mouse4: return "Mouse4";
        case BindKind::Mouse5: return "Mouse5";
        case BindKind::WheelUp: return "WheelUp";
        case BindKind::WheelDown: return "WheelDown";
        case BindKind::Key: break;
    }
    uint16_t vk = slot.vk;
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
    if (vk >= VK_F1 && vk <= VK_F12) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad" + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
        case VK_SPACE: return "Space";
        case VK_TAB: return "Tab";
        case VK_RETURN: return "Enter";
        case VK_BACK: return "Backspace";
        case VK_LSHIFT: return "LShift";
        case VK_RSHIFT: return "RShift";
        case VK_SHIFT: return "LShift";
        case VK_LCONTROL: return "LCtrl";
        case VK_RCONTROL: return "RCtrl";
        case VK_CONTROL: return "LCtrl";
        case VK_LMENU: return "LAlt";
        case VK_RMENU: return "RAlt";
        case VK_MENU: return "LAlt";
        case VK_UP: return "ArrowUp";
        case VK_DOWN: return "ArrowDown";
        case VK_LEFT: return "ArrowLeft";
        case VK_RIGHT: return "ArrowRight";
        case VK_CAPITAL: return "CapsLock";
        case VK_INSERT: return "Insert";
        case VK_DELETE: return "Delete";
        case VK_HOME: return "Home";
        case VK_END: return "End";
        case VK_PRIOR: return "PageUp";
        case VK_NEXT: return "PageDown";
        case VK_OEM_3: return "Grave";
        case VK_OEM_MINUS: return "Minus";
        case VK_OEM_PLUS: return "Equals";
        case VK_OEM_4: return "LBracket";
        case VK_OEM_6: return "RBracket";
        case VK_OEM_1: return "Semicolon";
        case VK_OEM_7: return "Apostrophe";
        case VK_OEM_COMMA: return "Comma";
        case VK_OEM_PERIOD: return "Period";
        case VK_OEM_2: return "Slash";
        case VK_OEM_5: return "Backslash";
        default: {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "VK_%02X", unsigned(vk));
            return buf;
        }
    }
}

std::string BindSlotToDisplay(const BindSlot& slot) {
    switch (slot.kind) {
        case BindKind::None: return "-";
        case BindKind::MouseLeft: return "LMB";
        case BindKind::MouseRight: return "RMB";
        case BindKind::MouseMiddle: return "MMB";
        case BindKind::Mouse4: return "Mouse 4";
        case BindKind::Mouse5: return "Mouse 5";
        case BindKind::WheelUp: return "Wheel Up";
        case BindKind::WheelDown: return "Wheel Down";
        case BindKind::Key: return BindSlotToToken(slot);
    }
    return "-";
}

BindSlot BindSlotFromToken(const std::string& raw) {
    std::string t = LowerAscii(raw);
    if (t.empty() || t == "none" || t == "-") return {};
    if (t == "mouseleft" || t == "lmb" || t == "left") return MakeMouse(BindKind::MouseLeft);
    if (t == "mouseright" || t == "rmb" || t == "right") return MakeMouse(BindKind::MouseRight);
    if (t == "mousemiddle" || t == "mmb" || t == "middle") return MakeMouse(BindKind::MouseMiddle);
    if (t == "mouse4" || t == "x1" || t == "xbutton1") return MakeMouse(BindKind::Mouse4);
    if (t == "mouse5" || t == "x2" || t == "xbutton2") return MakeMouse(BindKind::Mouse5);
    if (t == "wheelup" || t == "mwheelup") return MakeMouse(BindKind::WheelUp);
    if (t == "wheeldown" || t == "mwheeldown") return MakeMouse(BindKind::WheelDown);
    if (t.size() == 1) {
        char c = char(std::toupper(static_cast<unsigned char>(t[0])));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return MakeKey(uint16_t(c));
    }
    if (t == "space") return MakeKey(VK_SPACE);
    if (t == "tab") return MakeKey(VK_TAB);
    if (t == "enter" || t == "return") return MakeKey(VK_RETURN);
    if (t == "esc" || t == "escape") return MakeKey(VK_ESCAPE);
    if (t == "backspace" || t == "back") return MakeKey(VK_BACK);
    if (t == "lshift" || t == "shift") return MakeKey(VK_LSHIFT);
    if (t == "rshift") return MakeKey(VK_RSHIFT);
    if (t == "lctrl" || t == "ctrl" || t == "control") return MakeKey(VK_LCONTROL);
    if (t == "rctrl") return MakeKey(VK_RCONTROL);
    if (t == "lalt" || t == "alt") return MakeKey(VK_LMENU);
    if (t == "ralt") return MakeKey(VK_RMENU);
    if (t == "arrowup" || t == "up") return MakeKey(VK_UP);
    if (t == "arrowdown" || t == "down") return MakeKey(VK_DOWN);
    if (t == "arrowleft") return MakeKey(VK_LEFT);
    if (t == "arrowright") return MakeKey(VK_RIGHT);
    if (t == "capslock") return MakeKey(VK_CAPITAL);
    if (t == "insert") return MakeKey(VK_INSERT);
    if (t == "delete" || t == "del") return MakeKey(VK_DELETE);
    if (t == "home") return MakeKey(VK_HOME);
    if (t == "end") return MakeKey(VK_END);
    if (t == "pageup") return MakeKey(VK_PRIOR);
    if (t == "pagedown") return MakeKey(VK_NEXT);
    if (t == "grave") return MakeKey(VK_OEM_3);
    if (t == "minus") return MakeKey(VK_OEM_MINUS);
    if (t == "equals") return MakeKey(VK_OEM_PLUS);
    if (t == "lbracket") return MakeKey(VK_OEM_4);
    if (t == "rbracket") return MakeKey(VK_OEM_6);
    if (t == "semicolon") return MakeKey(VK_OEM_1);
    if (t == "apostrophe") return MakeKey(VK_OEM_7);
    if (t == "comma") return MakeKey(VK_OEM_COMMA);
    if (t == "period") return MakeKey(VK_OEM_PERIOD);
    if (t == "slash") return MakeKey(VK_OEM_2);
    if (t == "backslash") return MakeKey(VK_OEM_5);
    if (t.size() >= 2 && t[0] == 'f') {
        int n = std::atoi(t.c_str() + 1);
        if (n >= 1 && n <= 12) return MakeKey(uint16_t(VK_F1 + n - 1));
    }
    if (t.rfind("numpad", 0) == 0 && t.size() == 7 && t[6] >= '0' && t[6] <= '9') {
        return MakeKey(uint16_t(VK_NUMPAD0 + (t[6] - '0')));
    }
    if (t.rfind("vk_", 0) == 0 && t.size() >= 4) {
        unsigned long v = std::strtoul(t.c_str() + 3, nullptr, 16);
        if (v > 0 && v < 256) return MakeKey(uint16_t(v));
    }
    return {};
}

MkbConfig& MkbGetConfig() { return Rt().cfg; }

void MkbResetDefaults() {
    MkbRuntimeState& rt = Rt();
    rt.cfg = MakeDefaultConfig();
    rt.cli_input_mode_overridden = false;
    rt.cli_mouse_sens_overridden = false;
    rt.cli_mouse_invert_y_overridden = false;
    rt.saved_input_mode = rt.cfg.input_mode;
    rt.saved_mouse_sens = rt.cfg.mouse_sensitivity;
    rt.saved_mouse_invert_y = rt.cfg.mouse_invert_y;
}

void MkbMarkInputModeUserEdited() {
    MkbRuntimeState& rt = Rt();
    rt.cli_input_mode_overridden = false;
    rt.saved_input_mode = rt.cfg.input_mode;
}

void MkbMarkMouseSensUserEdited() {
    MkbRuntimeState& rt = Rt();
    rt.cli_mouse_sens_overridden = false;
    rt.saved_mouse_sens = rt.cfg.mouse_sensitivity;
}

void MkbMarkMouseInvertYUserEdited() {
    MkbRuntimeState& rt = Rt();
    rt.cli_mouse_invert_y_overridden = false;
    rt.saved_mouse_invert_y = rt.cfg.mouse_invert_y;
}

void MkbLoadSettings(const void* table_ptr) {
    MkbRuntimeState& rt = Rt();
    rt.cfg = MakeDefaultConfig();
    if (!table_ptr) return;
    const auto& root = *static_cast<const toml::table*>(table_ptr);
    if (const auto* controls = root["controls"].as_table()) {
        if (auto m = (*controls)["input_mode"].value<std::string>()) {
            std::string lm = LowerAscii(*m);
            if (lm == "mkb" || lm == "mouse_keyboard" || lm == "mouse") rt.cfg.input_mode = InputMode::MouseKeyboardOnly;
            else if (lm == "controller" || lm == "gamepad") rt.cfg.input_mode = InputMode::ControllerOnly;
            else rt.cfg.input_mode = InputMode::Auto;
        }
        if (auto sens = (*controls)["mouse_sensitivity"].value<double>()) {
            rt.cfg.mouse_sensitivity = std::clamp(float(*sens), 0.10f, 5.00f);
        }
        if (auto inv = (*controls)["mouse_invert_y"].value<bool>()) {
            rt.cfg.mouse_invert_y = *inv;
        }
        if (auto raw = (*controls)["mouse_raw_aim"].value<bool>()) {
            rt.cfg.mouse_raw_aim = *raw;
        }
        if (auto sm = (*controls)["mouse_smoothing"].value<bool>()) {
            rt.cfg.mouse_smoothing = *sm;
        }
        if (const auto* bindings = (*controls)["bindings"].as_table()) {
            for (const auto& info : kActionTable) {
                if (const auto* arr = (*bindings)[info.key].as_array()) {
                    BindSlot s0{}, s1{};
                    if (arr->size() >= 1) {
                        if (auto tok = (*arr)[0].value<std::string>()) s0 = BindSlotFromToken(*tok);
                    }
                    if (arr->size() >= 2) {
                        if (auto tok = (*arr)[1].value<std::string>()) s1 = BindSlotFromToken(*tok);
                    }
                    rt.cfg.bindings[size_t(info.action)] = {s0, s1};
                } else if (auto tok = (*bindings)[info.key].value<std::string>()) {
                    rt.cfg.bindings[size_t(info.action)] = {BindSlotFromToken(*tok), BindSlot{}};
                }
            }
        }
    }
    rt.saved_input_mode = rt.cfg.input_mode;
    rt.saved_mouse_sens = rt.cfg.mouse_sensitivity;
    rt.saved_mouse_invert_y = rt.cfg.mouse_invert_y;
}

void MkbSaveSettings(void* table_ptr) {
    if (!table_ptr) return;
    MkbRuntimeState& rt = Rt();
    auto& root = *static_cast<toml::table*>(table_ptr);

    InputMode mode_to_save = rt.cli_input_mode_overridden ? rt.saved_input_mode : rt.cfg.input_mode;
    float sens_to_save = rt.cli_mouse_sens_overridden ? rt.saved_mouse_sens : rt.cfg.mouse_sensitivity;
    bool inv_to_save = rt.cli_mouse_invert_y_overridden ? rt.saved_mouse_invert_y : rt.cfg.mouse_invert_y;

    toml::table controls;
    controls.insert("input_mode", std::string(InputModeToken(mode_to_save)));
    controls.insert("mouse_sensitivity", double(std::round(sens_to_save * 100.0f) / 100.0f));
    controls.insert("mouse_invert_y", inv_to_save);
    controls.insert("mouse_raw_aim", rt.cfg.mouse_raw_aim);
    controls.insert("mouse_smoothing", rt.cfg.mouse_smoothing);

    toml::table bindings;
    for (const auto& info : kActionTable) {
        const auto& pair = rt.cfg.bindings[size_t(info.action)];
        toml::array arr;
        arr.push_back(BindSlotToToken(pair[0]));
        arr.push_back(BindSlotToToken(pair[1]));
        bindings.insert(info.key, std::move(arr));
    }
    controls.insert("bindings", std::move(bindings));
    root.insert_or_assign("controls", std::move(controls));
}

void MkbInit() {
    MkbRuntimeState& rt = Rt();
    auto parse_mode = [](const std::string& val) -> InputMode {
        std::string lm = LowerAscii(val);
        if (lm == "mkb" || lm == "mouse_keyboard" || lm == "mouse") return InputMode::MouseKeyboardOnly;
        if (lm == "controller" || lm == "gamepad") return InputMode::ControllerOnly;
        return InputMode::Auto;
    };

    fs::path test_script;
    fs::path telemetry_path;
#ifdef _WIN32
    auto args = SplitCmdArgs(GetCommandLineW());
    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        if (a == L"--input-mode" && i + 1 < args.size()) {
            rt.saved_input_mode = rt.cfg.input_mode;
            rt.cfg.input_mode = parse_mode(WideToUtf8(args[++i]));
            rt.cli_input_mode_overridden = true;
        } else if (a.rfind(L"--input-mode=", 0) == 0) {
            rt.saved_input_mode = rt.cfg.input_mode;
            rt.cfg.input_mode = parse_mode(WideToUtf8(a.substr(13)));
            rt.cli_input_mode_overridden = true;
        } else if (a == L"--mouse-sens" && i + 1 < args.size()) {
            rt.saved_mouse_sens = rt.cfg.mouse_sensitivity;
            rt.cfg.mouse_sensitivity = std::clamp(float(std::atof(WideToUtf8(args[++i]).c_str())), 0.10f, 5.00f);
            rt.cli_mouse_sens_overridden = true;
        } else if (a.rfind(L"--mouse-sens=", 0) == 0) {
            rt.saved_mouse_sens = rt.cfg.mouse_sensitivity;
            rt.cfg.mouse_sensitivity = std::clamp(float(std::atof(WideToUtf8(a.substr(13)).c_str())), 0.10f, 5.00f);
            rt.cli_mouse_sens_overridden = true;
        } else if (a == L"--mouse-invert-y" && i + 1 < args.size()) {
            rt.saved_mouse_invert_y = rt.cfg.mouse_invert_y;
            rt.cfg.mouse_invert_y = (_wtoi(args[++i].c_str()) != 0);
            rt.cli_mouse_invert_y_overridden = true;
        } else if (a.rfind(L"--mouse-invert-y=", 0) == 0) {
            rt.saved_mouse_invert_y = rt.cfg.mouse_invert_y;
            rt.cfg.mouse_invert_y = (_wtoi(a.c_str() + 17) != 0);
            rt.cli_mouse_invert_y_overridden = true;
        } else if (a == L"--test-mkb" && i + 1 < args.size()) {
            test_script = fs::path(args[++i]);
        } else if (a.rfind(L"--test-mkb=", 0) == 0) {
            test_script = fs::path(a.substr(11));
        } else if (a == L"--mkb-telemetry" && i + 1 < args.size()) {
            telemetry_path = fs::path(args[++i]);
        } else if (a.rfind(L"--mkb-telemetry=", 0) == 0) {
            telemetry_path = fs::path(a.substr(16));
        }
    }
#else
    auto args = ReadProcCmdlineArgs();
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--input-mode" && i + 1 < args.size()) {
            rt.saved_input_mode = rt.cfg.input_mode;
            rt.cfg.input_mode = parse_mode(args[++i]);
            rt.cli_input_mode_overridden = true;
        } else if (a.rfind("--input-mode=", 0) == 0) {
            rt.saved_input_mode = rt.cfg.input_mode;
            rt.cfg.input_mode = parse_mode(a.substr(13));
            rt.cli_input_mode_overridden = true;
        } else if (a == "--mouse-sens" && i + 1 < args.size()) {
            rt.saved_mouse_sens = rt.cfg.mouse_sensitivity;
            rt.cfg.mouse_sensitivity = std::clamp(float(std::atof(args[++i].c_str())), 0.10f, 5.00f);
            rt.cli_mouse_sens_overridden = true;
        } else if (a.rfind("--mouse-sens=", 0) == 0) {
            rt.saved_mouse_sens = rt.cfg.mouse_sensitivity;
            rt.cfg.mouse_sensitivity = std::clamp(float(std::atof(a.c_str() + 13)), 0.10f, 5.00f);
            rt.cli_mouse_sens_overridden = true;
        } else if (a == "--mouse-invert-y" && i + 1 < args.size()) {
            rt.saved_mouse_invert_y = rt.cfg.mouse_invert_y;
            rt.cfg.mouse_invert_y = (std::atoi(args[++i].c_str()) != 0);
            rt.cli_mouse_invert_y_overridden = true;
        } else if (a.rfind("--mouse-invert-y=", 0) == 0) {
            rt.saved_mouse_invert_y = rt.cfg.mouse_invert_y;
            rt.cfg.mouse_invert_y = (std::atoi(a.c_str() + 17) != 0);
            rt.cli_mouse_invert_y_overridden = true;
        } else if (a == "--test-mkb" && i + 1 < args.size()) {
            test_script = fs::path(args[++i]);
        } else if (a.rfind("--test-mkb=", 0) == 0) {
            test_script = fs::path(a.substr(11));
        } else if (a == "--mkb-telemetry" && i + 1 < args.size()) {
            telemetry_path = fs::path(args[++i]);
        } else if (a.rfind("--mkb-telemetry=", 0) == 0) {
            telemetry_path = fs::path(a.substr(16));
        }
    }
#endif

    if (!test_script.empty()) LoadSyntheticTestScript(rt, test_script);
    if (!telemetry_path.empty()) {
        std::error_code ec;
        if (telemetry_path.has_parent_path()) fs::create_directories(telemetry_path.parent_path(), ec);
#ifdef _WIN32
        rt.telemetry_file = _wfopen(telemetry_path.c_str(), L"wb");
#else
        rt.telemetry_file = std::fopen(telemetry_path.c_str(), "wb");
#endif
    }

    LOGR("mkb: initialized (mode=%s, sens=%.2f, invert_y=%s, raw_aim=%s, smoothing=%s)",
         InputModeToken(rt.cfg.input_mode), rt.cfg.mouse_sensitivity, rt.cfg.mouse_invert_y ? "true" : "false",
         rt.cfg.mouse_raw_aim ? "true" : "false", rt.cfg.mouse_smoothing ? "true" : "false");
    if (rt.test_active || rt.telemetry_file) {
        bool ok = MkbRunMenuCoordSelfTest();
        LOGR("mkb: menu_coord_selftest=%s", ok ? "PASS" : "FAIL");
    }
}

void MkbOnWindow(void* hwnd_ptr) {
    MkbRuntimeState& rt = Rt();
    rt.game_hwnd.store(hwnd_ptr, std::memory_order_release);
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(hwnd_ptr);
    if (!hwnd) return;

    // Register foreground raw mouse input (dwFlags = 0 preserves legacy mouse messages for ImGui/titlebar)
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;  // HID_USAGE_PAGE_GENERIC
    rid.usUsage = 0x02;      // HID_USAGE_GENERIC_MOUSE
    rid.dwFlags = 0;
    rid.hwndTarget = hwnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        LOGW("mkb: RegisterRawInputDevices(mouse) failed (err=%lu)", GetLastError());
    }
    rt.focused.store(rt.test_active || GetForegroundWindow() == hwnd, std::memory_order_release);
#else
    SDL_Window* win = static_cast<SDL_Window*>(hwnd_ptr);
    if (!win) return;
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_SYSTEM_SCALE, "0");
    rt.focused.store(rt.test_active || (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0,
                     std::memory_order_release);
#endif
}

void MkbBeginRebind(MkbAction action, int slot) {
    MkbRuntimeState& rt = Rt();
    std::lock_guard<std::mutex> lock(rt.rebind_lock);
    auto now = std::chrono::steady_clock::now();
    // Guard against the LMB release of a just-committed LMB rebind (or Esc cancel) re-triggering
    // ImGui::Button on the same frame/hover inside the in-game F1 overlay.
    if (rt.has_rebind_finished) {
        auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - rt.rebind_finished_at).count();
        if (since_ms < 250) return;
    }
    rt.rebinding = true;
    rt.rebind_action = action;
    rt.rebind_slot = (slot == 1) ? 1 : 0;
    rt.rebind_armed_at = now;
}

void MkbCancelRebind() {
    MkbRuntimeState& rt = Rt();
    std::lock_guard<std::mutex> lock(rt.rebind_lock);
    rt.rebinding = false;
}

bool MkbIsRebinding(MkbAction* out_action, int* out_slot) {
    MkbRuntimeState& rt = Rt();
    std::lock_guard<std::mutex> lock(rt.rebind_lock);
    if (!rt.rebinding) return false;
    if (out_action) *out_action = rt.rebind_action;
    if (out_slot) *out_slot = rt.rebind_slot;
    return true;
}

bool MkbHandleRebindMessage(uint32_t msg, uintptr_t wp, intptr_t lp) {
    MkbRuntimeState& rt = Rt();
    std::lock_guard<std::mutex> lock(rt.rebind_lock);
    if (!rt.rebinding) return false;

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - rt.rebind_armed_at)
                          .count();

    BindSlot captured{};
    bool commit = false;
#ifdef _WIN32
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            uint16_t vk = NormalizeVirtualKey(wp, lp);
            if (vk == VK_ESCAPE) {
                rt.rebinding = false;
                rt.has_rebind_finished = true;
                rt.rebind_finished_at = std::chrono::steady_clock::now();
                return true;
            }
            if (vk == VK_DELETE) {
                captured = BindSlot{};
                commit = true;
            } else if (MkbCanBindKey(vk)) {
                captured = MakeKey(vk);
                commit = true;
            } else {
                return true;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            if (elapsed_ms < 80) return true;  // Ignore arming click frame
            captured = MakeMouse(BindKind::MouseLeft);
            commit = true;
            break;
        case WM_LBUTTONUP:
            // Swallow release of the arming click
            return elapsed_ms < 250;
        case WM_RBUTTONDOWN:
            captured = MakeMouse(BindKind::MouseRight);
            commit = true;
            break;
        case WM_MBUTTONDOWN:
            captured = MakeMouse(BindKind::MouseMiddle);
            commit = true;
            break;
        case WM_XBUTTONDOWN: {
            WORD xbtn = HIWORD(wp);
            captured = MakeMouse(xbtn == XBUTTON1 ? BindKind::Mouse4 : BindKind::Mouse5);
            commit = true;
            break;
        }
        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (delta != 0) {
                captured = MakeMouse(delta > 0 ? BindKind::WheelUp : BindKind::WheelDown);
                commit = true;
            }
            break;
        }
        default:
            return false;
    }
#else
    (void)wp;
    const SDL_Event* ev = reinterpret_cast<const SDL_Event*>(lp);
    if (!ev) return false;
    switch (msg) {
        case SDL_EVENT_KEY_DOWN: {
            if (ev->key.repeat) return true;
            uint16_t vk = SdlScancodeToVk(ev->key.scancode);
            if (vk == VK_ESCAPE) {
                rt.rebinding = false;
                rt.has_rebind_finished = true;
                rt.rebind_finished_at = std::chrono::steady_clock::now();
                return true;
            }
            if (vk == VK_DELETE) {
                captured = BindSlot{};
                commit = true;
            } else if (MkbCanBindKey(vk)) {
                captured = MakeKey(vk);
                commit = true;
            } else {
                return true;
            }
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            switch (ev->button.button) {
                case SDL_BUTTON_LEFT:
                    if (elapsed_ms < 80) return true;
                    captured = MakeMouse(BindKind::MouseLeft);
                    commit = true;
                    break;
                case SDL_BUTTON_RIGHT:
                    captured = MakeMouse(BindKind::MouseRight);
                    commit = true;
                    break;
                case SDL_BUTTON_MIDDLE:
                    captured = MakeMouse(BindKind::MouseMiddle);
                    commit = true;
                    break;
                case SDL_BUTTON_X1:
                    captured = MakeMouse(BindKind::Mouse4);
                    commit = true;
                    break;
                case SDL_BUTTON_X2:
                    captured = MakeMouse(BindKind::Mouse5);
                    commit = true;
                    break;
                default:
                    return false;
            }
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
            return ev->button.button == SDL_BUTTON_LEFT && elapsed_ms < 250;
        case SDL_EVENT_MOUSE_WHEEL:
            if (ev->wheel.y != 0.0f) {
                captured = MakeMouse(ev->wheel.y > 0.0f ? BindKind::WheelUp : BindKind::WheelDown);
                commit = true;
            }
            break;
        default:
            return false;
    }
#endif

    if (commit) {
        rt.cfg.bindings[size_t(rt.rebind_action)][size_t(rt.rebind_slot)] = captured;
        rt.rebinding = false;
        rt.has_rebind_finished = true;
        rt.rebind_finished_at = std::chrono::steady_clock::now();
        app::Save();
        return true;
    }
    return false;
}

GuestContext MkbSampleGuestContext() {
    GuestContext gc{};
    if (!kernel::g_base) return gc;
    SafeReadU32(kVaGameSessionState, gc.session);
    SafeReadU32(kVaEngineModeFlags, gc.mode);
    SafeReadU32(kVaLevelFlags, gc.lvl_flags);
    SafeReadU32(kVaPauseFlags, gc.pause_flags);
    SafeReadU32(kVaLoadingScreenPtr, gc.loading_ptr);

    uint32_t ui_mgr = 0;
    int32_t interactive_ui_count = 0;
    if (SafeReadU32(kVaUiMgrPtr, ui_mgr) && ui_mgr >= 0x10000u) {
        uint32_t sc = 0;
        uint32_t arr = 0;
        // Per sub_0005F9A0 (UIMgr::Update, 0x0005FAC2..0x0005FAE0), [ui_mgr+0x48]/[+0x50] holds ALL
        // visible UI screens including 10-11 non-interactive HUD widgets during gameplay. An active
        // interactive menu (Title, Main Menu, Pause, Archives/Map, Save/Load) sits on the stack with
        // ([scr+0x04] & 0x01) != 0 and [scr+0x24] == 4 (active input-consuming state).
        if (SafeReadU32(ui_mgr + 0x50u, sc) && int32_t(sc) > 0 && int32_t(sc) <= 64 &&
            SafeReadU32(ui_mgr + 0x48u, arr) && arr >= 0x10000u) {
            for (int32_t i = 0; i < int32_t(sc); ++i) {
                uint32_t scr = 0;
                uint32_t flags = 0;
                uint32_t state = 0;
                if (SafeReadU32(arr + uint32_t(i) * 4u, scr) && scr >= 0x10000u &&
                    SafeReadU32(scr + 0x04u, flags) && SafeReadU32(scr + 0x24u, state)) {
                    if ((flags & 0x01u) != 0u && state == 4u) {
                        ++interactive_ui_count;
                    }
                }
            }
        }
    }
    gc.ui_stack = interactive_ui_count;

    gc.fmv = (gc.mode & 0x08u) != 0u;

    // Cutscene check (verified from sub_00053DA0):
    // obj = *(u32*)0x002D0DC0; if [obj+0x12C] > 0, ent = **(u32**)(obj+0x124), player = *(u32*)(ent+0x60),
    // cutscene = ((*(u32*)(player+0x9C) >> 1) & 1) != 0.
    uint32_t obj = 0, cnt = 0, ent_arr = 0, ent = 0, player = 0, pflags = 0;
    if (SafeReadU32(kVaObjMgrPtr, obj) && obj >= 0x10000u && SafeReadU32(obj + 0x12Cu, cnt) && int32_t(cnt) > 0 &&
        SafeReadU32(obj + 0x124u, ent_arr) && SafeReadU32(ent_arr, ent) && ent >= 0x10000u &&
        SafeReadU32(ent + 0x60u, player) && player >= 0x10000u && SafeReadU32(player + 0x9Cu, pflags)) {
        gc.cutscene = ((pflags >> 1) & 1u) != 0u;
    }

    // Every blocking/modal UI menu in The Suffering (Title, Main Menu, Pause, Archives/Map/Journal,
    // Save/Load, Death Screen) clears kVaEngineModeFlags bit 2 (mode: 7 -> 1) and/or sets
    // kVaPauseFlags bit 0 (pause: 128 -> 129). Non-blocking HUD widgets (tutorial prompts,
    // weapon-select strip) stay in UIMgr while mode==7 and (pause & 3)==0, so we must NOT gate
    // 3D mouse look or cursor capture on UIMgr non-blocking HUDs.
    gc.in_active_3d_gameplay = (gc.session == 2u) && ((gc.mode & 0x0Cu) == 0x04u) && ((gc.lvl_flags & 0x02u) != 0u) &&
                               ((gc.pause_flags & 0x03u) == 0u) && (gc.loading_ptr == 0u) && !gc.cutscene;
    return gc;
}

bool MkbIsInActive3DGameplay() { return MkbSampleGuestContext().in_active_3d_gameplay; }

bool MkbShouldCaptureCursor() {
    MkbRuntimeState& rt = Rt();
    if (suffering::IsPadOnlyInputSession()) return false;
    if (rt.cfg.input_mode == InputMode::ControllerOnly) return false;
    if (UiIsOverlayOpen()) return false;
    if (!rt.focused.load(std::memory_order_acquire)) return false;
    if (rt.in_sizemove.load(std::memory_order_acquire)) return false;
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(rt.game_hwnd.load(std::memory_order_acquire));
    if (!hwnd || !IsWindow(hwnd) || IsIconic(hwnd)) return false;
    if (!rt.test_active && GetForegroundWindow() != hwnd) return false;
#else
    SDL_Window* win = static_cast<SDL_Window*>(rt.game_hwnd.load(std::memory_order_acquire));
    if (!win) return false;
    SDL_WindowFlags wf = SDL_GetWindowFlags(win);
    if ((wf & SDL_WINDOW_MINIMIZED) != 0) return false;
    if (!rt.test_active && (wf & SDL_WINDOW_INPUT_FOCUS) == 0) return false;
#endif
    return MkbSampleGuestContext().in_active_3d_gameplay;
}

bool MkbIsCursorCaptured() { return Rt().cursor_captured.load(std::memory_order_acquire); }

void MkbSyncCursorOnWindowThread(void* hwnd_ptr) {
    bool want = MkbShouldCaptureCursor();
    Rt().last_desired_capture.store(want, std::memory_order_release);
#ifdef _WIN32
    ApplyClipRect(static_cast<HWND>(hwnd_ptr), want);
#else
    ApplyClipRect(hwnd_ptr, want);
#endif
}

void MkbForceReleaseCursor(void* hwnd_ptr) {
    MkbRuntimeState& rt = Rt();
    {
        std::lock_guard<std::mutex> lock(rt.input_lock);
        ClearAllHeldInputsLocked(rt);
    }
    ReleaseMenuDragCapture(rt, hwnd_ptr);
    rt.last_desired_capture.store(false, std::memory_order_release);
#ifdef _WIN32
    ApplyClipRect(static_cast<HWND>(hwnd_ptr), false);
#else
    ApplyClipRect(hwnd_ptr, false);
#endif
}

void MkbOnOverlayToggled(bool open, void* hwnd_ptr) {
    if (open) {
        MkbForceReleaseCursor(hwnd_ptr);
    } else {
        MkbCancelRebind();
        MkbSyncCursorOnWindowThread(hwnd_ptr);
    }
}

bool MkbOnMessage(void* hwnd_ptr, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
    MkbRuntimeState& rt = Rt();
#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(hwnd_ptr);

    // 1. Always handle focus / lifecycle / sizemove messages and return FALSE so main.cpp runs too!
    switch (msg) {
        case WM_ACTIVATE: {
            if (rt.test_active) return false;
            bool active = LOWORD(wp) != WA_INACTIVE;
            rt.focused.store(active, std::memory_order_release);
            if (!active) MkbForceReleaseCursor(hwnd);
            else MkbSyncCursorOnWindowThread(hwnd);
            return false;
        }
        case WM_ACTIVATEAPP: {
            if (rt.test_active) return false;
            bool active = wp != 0;
            rt.focused.store(active, std::memory_order_release);
            if (!active) MkbForceReleaseCursor(hwnd);
            else MkbSyncCursorOnWindowThread(hwnd);
            return false;
        }
        case WM_KILLFOCUS:
            if (rt.test_active) return false;
            rt.focused.store(false, std::memory_order_release);
            MkbForceReleaseCursor(hwnd);
            return false;
        case WM_SETFOCUS:
            if (rt.test_active) return false;
            rt.focused.store(true, std::memory_order_release);
            MkbSyncCursorOnWindowThread(hwnd);
            return false;
        case WM_ENTERSIZEMOVE:
            rt.in_sizemove.store(true, std::memory_order_release);
            MkbForceReleaseCursor(hwnd);
            return false;
        case WM_EXITSIZEMOVE:
            rt.in_sizemove.store(false, std::memory_order_release);
            MkbSyncCursorOnWindowThread(hwnd);
            return false;
        case WM_SIZE:
            if (rt.cursor_captured.load(std::memory_order_acquire)) MkbSyncCursorOnWindowThread(hwnd);
            return false;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE: {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            if (rt.menu_lmb_down) {
                rt.menu_lmb_down = false;
                rt.menu_lmb_released = true;
            }
            rt.menu_captured_hwnd = false;
            return false;
        }
        case WM_CLOSE:
        case WM_DESTROY:
            MkbForceReleaseCursor(hwnd);
            if (rt.telemetry_file) {
                std::fflush(rt.telemetry_file);
                std::fclose(rt.telemetry_file);
                rt.telemetry_file = nullptr;
            }
            return false;
        default:
            break;
    }

    if (suffering::IsPadOnlyInputSession() || rt.cfg.input_mode == InputMode::ControllerOnly || UiIsOverlayOpen()) {
        return false;
    }

    // 2. Gameplay & Menu mouse/keyboard capture
    switch (msg) {
        case WM_MOUSEMOVE: {
            if (!rt.focused.load(std::memory_order_acquire)) return false;
            if (!MkbIsInActive3DGameplay()) {
                float cx = float(static_cast<int16_t>(LOWORD(lp)));
                float cy = float(static_cast<int16_t>(HIWORD(lp)));
                float px = cx, py = cy;
                ClientToPhysicalCoords(hwnd, cx, cy, px, py);
                std::lock_guard<std::mutex> lock(rt.input_lock);
                UpdateMenuCursorPhysLocked(rt, px, py);
            }
            return false;
        }
        case WM_INPUT: {
            if (!rt.focused.load(std::memory_order_acquire)) return false;
            UINT sz = 0;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER)) !=
                    0 ||
                sz == 0 || sz > 256) {
                return false;
            }
            uint8_t buf[256];
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, buf, &sz, sizeof(RAWINPUTHEADER)) != sz) {
                return false;
            }
            const RAWINPUT* ri = reinterpret_cast<const RAWINPUT*>(buf);
            if (ri->header.dwType == RIM_TYPEMOUSE) {
                const RAWMOUSE& m = ri->data.mouse;
                if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) == 0 && (m.lLastX != 0 || m.lLastY != 0)) {
                    if (MkbIsInActive3DGameplay()) {
                        std::lock_guard<std::mutex> lock(rt.input_lock);
                        rt.raw_dx += double(m.lLastX);
                        rt.raw_dy += double(m.lLastY);
                        rt.mouse_look_active.store(true, std::memory_order_release);
                    }
                }
            }
            return false;
        }
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            if (!MkbCanBindKey(uint16_t(wp))) return false;
            // Allow Alt+Enter / Alt+F4 to flow to Windows/host
            if ((GetKeyState(VK_MENU) & 0x8000) != 0 && (wp == VK_RETURN || wp == VK_F4)) return false;
            uint16_t vk = NormalizeVirtualKey(wp, lp);
            std::lock_guard<std::mutex> lock(rt.input_lock);
            if (vk < 256) rt.keys[vk] = true;
            if (result) *result = 0;
            return true;
        }
        case WM_KEYUP:
        case WM_SYSKEYUP: {
            uint16_t vk = NormalizeVirtualKey(wp, lp);
            std::lock_guard<std::mutex> lock(rt.input_lock);
            if (vk < 256) rt.keys[vk] = false;
            if (vk == VK_SHIFT) {
                rt.keys[VK_LSHIFT] = false;
                rt.keys[VK_RSHIFT] = false;
            } else if (vk == VK_CONTROL) {
                rt.keys[VK_LCONTROL] = false;
                rt.keys[VK_RCONTROL] = false;
            } else if (vk == VK_MENU) {
                rt.keys[VK_LMENU] = false;
                rt.keys[VK_RMENU] = false;
            }
            return false;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            bool in_3d = MkbIsInActive3DGameplay();
            if (in_3d) {
                if (!rt.cursor_captured.load(std::memory_order_acquire) && MkbShouldCaptureCursor()) {
                    MkbSyncCursorOnWindowThread(hwnd);
                }
                std::lock_guard<std::mutex> lock(rt.input_lock);
                rt.mouse_buttons[0] = true;
            } else {
                float cx = float(static_cast<int16_t>(LOWORD(lp)));
                float cy = float(static_cast<int16_t>(HIWORD(lp)));
                float px = cx, py = cy;
                ClientToPhysicalCoords(hwnd, cx, cy, px, py);
                {
                    std::lock_guard<std::mutex> lock(rt.input_lock);
                    UpdateMenuCursorPhysLocked(rt, px, py);
                    rt.mouse_buttons[0] = true;
                    rt.suppressed_mouse_mask |= uint8_t(1u << 0);
                    rt.menu_lmb_pressed = true;
                    rt.menu_lmb_down = true;
                }
                if (hwnd && IsWindow(hwnd)) {
                    SetCapture(hwnd);
                    rt.menu_captured_hwnd = true;
                }
            }
            if (result) *result = 0;
            return true;
        }
        case WM_LBUTTONUP: {
            {
                std::lock_guard<std::mutex> lock(rt.input_lock);
                rt.mouse_buttons[0] = false;
                rt.suppressed_mouse_mask &= ~uint8_t(1u << 0);
                if (rt.menu_lmb_down) {
                    rt.menu_lmb_down = false;
                    rt.menu_lmb_released = true;
                }
            }
            ReleaseMenuDragCapture(rt, hwnd);
            return false;
        }
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK: {
            bool in_3d = MkbIsInActive3DGameplay();
            if (in_3d) {
                std::lock_guard<std::mutex> lock(rt.input_lock);
                rt.mouse_buttons[1] = true;
            } else {
                float cx = float(static_cast<int16_t>(LOWORD(lp)));
                float cy = float(static_cast<int16_t>(HIWORD(lp)));
                float px = cx, py = cy;
                ClientToPhysicalCoords(hwnd, cx, cy, px, py);
                std::lock_guard<std::mutex> lock(rt.input_lock);
                UpdateMenuCursorPhysLocked(rt, px, py);
                rt.mouse_buttons[1] = true;
                rt.suppressed_mouse_mask |= uint8_t(1u << 1);
                rt.menu_rmb_pressed = true;
            }
            if (result) *result = 0;
            return true;
        }
        case WM_RBUTTONUP: {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[1] = false;
            rt.suppressed_mouse_mask &= ~uint8_t(1u << 1);
            return false;
        }
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK: {
            bool in_3d = MkbIsInActive3DGameplay();
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[2] = true;
            if (!in_3d) rt.suppressed_mouse_mask |= uint8_t(1u << 2);
            if (result) *result = 0;
            return true;
        }
        case WM_MBUTTONUP: {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[2] = false;
            rt.suppressed_mouse_mask &= ~uint8_t(1u << 2);
            return false;
        }
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK: {
            WORD xbtn = HIWORD(wp);
            size_t idx = (xbtn == XBUTTON1) ? 3u : 4u;
            bool in_3d = MkbIsInActive3DGameplay();
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[idx] = true;
            if (!in_3d) rt.suppressed_mouse_mask |= uint8_t(1u << idx);
            if (result) *result = TRUE;
            return true;
        }
        case WM_XBUTTONUP: {
            WORD xbtn = HIWORD(wp);
            size_t idx = (xbtn == XBUTTON1) ? 3u : 4u;
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[idx] = false;
            rt.suppressed_mouse_mask &= ~uint8_t(1u << idx);
            return false;
        }
        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wp);
            bool in_3d = MkbIsInActive3DGameplay();
            if (!in_3d) {
                POINT pt{static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp))};
                bool have_pt = hwnd && ScreenToClient(hwnd, &pt);
                float px = 0.0f, py = 0.0f;
                if (have_pt) ClientToPhysicalCoords(hwnd, float(pt.x), float(pt.y), px, py);
                std::lock_guard<std::mutex> lock(rt.input_lock);
                if (have_pt) UpdateMenuCursorPhysLocked(rt, px, py);
                rt.menu_wheel_rem += int(delta);
                int notches = rt.menu_wheel_rem / WHEEL_DELTA;
                rt.menu_wheel_rem -= notches * WHEEL_DELTA;
                if (notches != 0) {
                    rt.menu_wheel_steps = std::clamp(rt.menu_wheel_steps + notches, -16, 16);
                }
            } else {
                int notches = delta / WHEEL_DELTA;
                if (notches == 0 && delta != 0) notches = (delta > 0) ? 1 : -1;
                std::lock_guard<std::mutex> lock(rt.input_lock);
                if (notches > 0) rt.wheel_up_queue = std::min(rt.wheel_up_queue + notches, kMaxWheelQueue);
                else if (notches < 0) rt.wheel_down_queue = std::min(rt.wheel_down_queue - notches, kMaxWheelQueue);
            }
            if (result) *result = 0;
            return true;
        }
        default:
            break;
    }
    return false;
#else
    (void)wp;
    const SDL_Event* ev = reinterpret_cast<const SDL_Event*>(lp);
    if (!ev) return false;

    // 1. Focus / window lifecycle events
    switch (msg) {
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            if (rt.test_active) return false;
            rt.focused.store(true, std::memory_order_release);
            MkbSyncCursorOnWindowThread(hwnd_ptr);
            return false;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            if (rt.test_active) return false;
            rt.focused.store(false, std::memory_order_release);
            MkbForceReleaseCursor(hwnd_ptr);
            return false;
        case SDL_EVENT_WINDOW_MINIMIZED:
            rt.focused.store(false, std::memory_order_release);
            MkbForceReleaseCursor(hwnd_ptr);
            return false;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_RESIZED:
            if (rt.cursor_captured.load(std::memory_order_acquire)) MkbSyncCursorOnWindowThread(hwnd_ptr);
            return false;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            MkbForceReleaseCursor(hwnd_ptr);
            if (rt.telemetry_file) {
                std::fflush(rt.telemetry_file);
                std::fclose(rt.telemetry_file);
                rt.telemetry_file = nullptr;
            }
            return false;
        default:
            break;
    }

    if (suffering::IsPadOnlyInputSession() || rt.cfg.input_mode == InputMode::ControllerOnly || UiIsOverlayOpen()) {
        return false;
    }

    // 2. Gameplay & Menu mouse/keyboard capture via SDL3 relative/logical mouse + scancode->VK mapping
    switch (msg) {
        case SDL_EVENT_MOUSE_MOTION: {
            if (!rt.focused.load(std::memory_order_acquire)) return false;
            if (MkbIsInActive3DGameplay()) {
                if (ev->motion.xrel != 0.0f || ev->motion.yrel != 0.0f) {
                    std::lock_guard<std::mutex> lock(rt.input_lock);
                    rt.raw_dx += double(ev->motion.xrel);
                    rt.raw_dy += double(ev->motion.yrel);
                    rt.mouse_look_active.store(true, std::memory_order_release);
                }
            } else {
                float px = ev->motion.x, py = ev->motion.y;
                ClientToPhysicalCoords(hwnd_ptr, ev->motion.x, ev->motion.y, px, py);
                std::lock_guard<std::mutex> lock(rt.input_lock);
                UpdateMenuCursorPhysLocked(rt, px, py);
            }
            return false;
        }
        case SDL_EVENT_KEY_DOWN: {
            uint16_t vk = SdlScancodeToVk(ev->key.scancode);
            if (!MkbCanBindKey(vk)) return false;
            if ((ev->key.mod & SDL_KMOD_ALT) != 0 && (vk == VK_RETURN || vk == VK_F4)) return false;
            std::lock_guard<std::mutex> lock(rt.input_lock);
            if (vk < 256) rt.keys[vk] = true;
            if (result) *result = 0;
            return true;
        }
        case SDL_EVENT_KEY_UP: {
            uint16_t vk = SdlScancodeToVk(ev->key.scancode);
            std::lock_guard<std::mutex> lock(rt.input_lock);
            if (vk > 0 && vk < 256) rt.keys[vk] = false;
            return false;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN: {
            int idx = -1;
            switch (ev->button.button) {
                case SDL_BUTTON_LEFT: idx = 0; break;
                case SDL_BUTTON_RIGHT: idx = 1; break;
                case SDL_BUTTON_MIDDLE: idx = 2; break;
                case SDL_BUTTON_X1: idx = 3; break;
                case SDL_BUTTON_X2: idx = 4; break;
                default: break;
            }
            if (idx < 0) return false;
            bool in_3d = MkbIsInActive3DGameplay();
            if (in_3d) {
                if (idx == 0 && !rt.cursor_captured.load(std::memory_order_acquire) && MkbShouldCaptureCursor()) {
                    MkbSyncCursorOnWindowThread(hwnd_ptr);
                }
                std::lock_guard<std::mutex> lock(rt.input_lock);
                rt.mouse_buttons[size_t(idx)] = true;
            } else {
                float px = ev->button.x, py = ev->button.y;
                ClientToPhysicalCoords(hwnd_ptr, ev->button.x, ev->button.y, px, py);
                {
                    std::lock_guard<std::mutex> lock(rt.input_lock);
                    UpdateMenuCursorPhysLocked(rt, px, py);
                    rt.mouse_buttons[size_t(idx)] = true;
                    rt.suppressed_mouse_mask |= uint8_t(1u << idx);
                    if (idx == 0) {
                        rt.menu_lmb_pressed = true;
                        rt.menu_lmb_down = true;
                    } else if (idx == 1) {
                        rt.menu_rmb_pressed = true;
                    }
                }
                if (idx == 0) {
                    SDL_CaptureMouse(true);
                    rt.menu_captured_hwnd = true;
                }
            }
            if (result) *result = 0;
            return true;
        }
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            int idx = -1;
            switch (ev->button.button) {
                case SDL_BUTTON_LEFT: idx = 0; break;
                case SDL_BUTTON_RIGHT: idx = 1; break;
                case SDL_BUTTON_MIDDLE: idx = 2; break;
                case SDL_BUTTON_X1: idx = 3; break;
                case SDL_BUTTON_X2: idx = 4; break;
                default: break;
            }
            if (idx < 0) return false;
            {
                std::lock_guard<std::mutex> lock(rt.input_lock);
                rt.mouse_buttons[size_t(idx)] = false;
                rt.suppressed_mouse_mask &= ~uint8_t(1u << idx);
                if (idx == 0 && rt.menu_lmb_down) {
                    rt.menu_lmb_down = false;
                    rt.menu_lmb_released = true;
                }
            }
            if (idx == 0) ReleaseMenuDragCapture(rt, hwnd_ptr);
            return false;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
            bool in_3d = MkbIsInActive3DGameplay();
            if (!in_3d) {
                float px = ev->wheel.mouse_x, py = ev->wheel.mouse_y;
                ClientToPhysicalCoords(hwnd_ptr, ev->wheel.mouse_x, ev->wheel.mouse_y, px, py);
                std::lock_guard<std::mutex> lock(rt.input_lock);
                UpdateMenuCursorPhysLocked(rt, px, py);
                rt.menu_wheel_frem += ev->wheel.y;
                int notches = int(std::trunc(rt.menu_wheel_frem));
                rt.menu_wheel_frem -= float(notches);
                if (notches == 0 && std::fabs(ev->wheel.y) >= 0.5f) {
                    notches = (ev->wheel.y > 0.0f) ? 1 : -1;
                    rt.menu_wheel_frem = 0.0f;
                }
                if (notches != 0) {
                    rt.menu_wheel_steps = std::clamp(rt.menu_wheel_steps + notches, -16, 16);
                }
            } else {
                int notches = int(std::lround(ev->wheel.y));
                if (notches == 0 && ev->wheel.y != 0.0f) notches = (ev->wheel.y > 0.0f) ? 1 : -1;
                std::lock_guard<std::mutex> lock(rt.input_lock);
                if (notches > 0) rt.wheel_up_queue = std::min(rt.wheel_up_queue + notches, kMaxWheelQueue);
                else if (notches < 0) rt.wheel_down_queue = std::min(rt.wheel_down_queue - notches, kMaxWheelQueue);
            }
            if (result) *result = 0;
            return true;
        }
        default:
            break;
    }
    return false;
#endif
}

void MkbMergeGameplayPad(input::Pad& pad) {
    MkbRuntimeState& rt = Rt();
    if (suffering::IsPadOnlyInputSession() || rt.cfg.input_mode == InputMode::ControllerOnly) return;

    GuestContext gc = MkbSampleGuestContext();

    // Sync Win32 cursor capture state whenever gameplay/menu state transitions
    bool want_capture = MkbShouldCaptureCursor();
    bool prev_capture = rt.last_desired_capture.exchange(want_capture, std::memory_order_acq_rel);
    if (want_capture != prev_capture) {
        UiRequestRefreshCursor();
    }

    // Clear mouse look when leaving active 3D gameplay (prevents unintended Archives map zoom)
    if (!gc.in_active_3d_gameplay) {
        std::lock_guard<std::mutex> lock(rt.input_lock);
        rt.raw_dx = 0.0;
        rt.raw_dy = 0.0;
        rt.smooth_dx = 0.0f;
        rt.smooth_dy = 0.0f;
        rt.wheel_up_queue = 0;
        rt.wheel_up_hold = 0;
        rt.wheel_up_gap = 0;
        rt.wheel_down_queue = 0;
        rt.wheel_down_hold = 0;
        rt.wheel_down_gap = 0;
    }

    std::lock_guard<std::mutex> lock(rt.input_lock);

    // Click-through transition guard: when transitioning menu/cutscene -> active 3D gameplay,
    // latch ALL currently held mouse buttons until physically released and discard stale mouse delta.
    if (gc.in_active_3d_gameplay && !rt.prev_in_3d_gameplay) {
        for (size_t i = 0; i < rt.mouse_buttons.size(); ++i) {
            if (rt.mouse_buttons[i]) rt.suppressed_mouse_mask |= uint8_t(1u << i);
        }
        rt.raw_dx = 0.0;
        rt.raw_dy = 0.0;
        rt.smooth_dx = 0.0f;
        rt.smooth_dy = 0.0f;
        ClearMenuPendingLocked(rt);
    }
    rt.prev_in_3d_gameplay = gc.in_active_3d_gameplay;

    // Advance mouse-wheel notch pulse queues (only active in 3D gameplay; zeroed in menus above)
    if (gc.in_active_3d_gameplay) {
        AdvanceWheelQueue(rt.wheel_up_queue, rt.wheel_up_hold, rt.wheel_up_gap);
        AdvanceWheelQueue(rt.wheel_down_queue, rt.wheel_down_hold, rt.wheel_down_gap);
    }

    // 1. Movement (WASD / custom bindings) -> Diagonal-normalized Left Stick
    float fx = 0.0f;
    float fy = 0.0f;
    if (IsActionActiveLocked(rt, MkbAction::MoveForward)) fy += 1.0f;
    if (IsActionActiveLocked(rt, MkbAction::MoveBack)) fy -= 1.0f;
    if (IsActionActiveLocked(rt, MkbAction::MoveRight)) fx += 1.0f;
    if (IsActionActiveLocked(rt, MkbAction::MoveLeft)) fx -= 1.0f;
    if (fx != 0.0f || fy != 0.0f) {
        float len = std::hypot(fx, fy);
        if (len > 1.0f) {
            fx /= len;
            fy /= len;
        }
        pad.lx = int16_t(std::clamp(std::lround(fx * 32767.0f), -32767l, 32767l));
        pad.ly = int16_t(std::clamp(std::lround(fy * 32767.0f), -32767l, 32767l));
    }

    // 2. Gameplay buttons (Fire/ThrowGrenade suppressed outside 3D gameplay & guarded by suppressed_mouse_mask)
    if (gc.in_active_3d_gameplay) {
        if (IsActionActiveLocked(rt, MkbAction::Fire)) pad.analog[7] = 255;          // RT
        if (IsActionActiveLocked(rt, MkbAction::ThrowGrenade)) pad.analog[6] = 255;  // LT
    }
    if (IsActionActiveLocked(rt, MkbAction::Jump)) pad.analog[0] = 255;          // A
    if (IsActionActiveLocked(rt, MkbAction::Reload)) pad.analog[1] = 255;        // B
    if (IsActionActiveLocked(rt, MkbAction::Interact)) pad.analog[2] = 255;      // X
    if (IsActionActiveLocked(rt, MkbAction::InsanityTransform)) pad.analog[3] = 255;  // Y
    if (IsActionActiveLocked(rt, MkbAction::HealXombium)) pad.analog[4] = 255;   // Black
    if (IsActionActiveLocked(rt, MkbAction::CameraToggle)) pad.analog[5] = 255;  // White
    if (IsActionActiveLocked(rt, MkbAction::Flashlight)) pad.buttons |= (1u << 7);  // R3 (Scheme 1 -> 1054)
    if (IsActionActiveLocked(rt, MkbAction::ArchivesMap)) pad.buttons |= (1u << 5); // Back (Archives/Map)
    if (IsActionActiveLocked(rt, MkbAction::WeaponCatUp)) pad.buttons |= (1u << 0); // D-Pad Up
    if (IsActionActiveLocked(rt, MkbAction::ItemCatDown)) pad.buttons |= (1u << 1); // D-Pad Down
    if (IsActionActiveLocked(rt, MkbAction::PrevItemLeft)) pad.buttons |= (1u << 2);// D-Pad Left
    if (IsActionActiveLocked(rt, MkbAction::NextItemRight)) pad.buttons |= (1u << 3);// D-Pad Right

    // 3. Context-aware latched Esc key (latched on keydown transition so a multi-frame press never bounces!)
    bool esc_down = rt.keys[VK_ESCAPE];
    if (esc_down && !rt.esc_was_down) {
        if (gc.fmv || gc.cutscene) {
            rt.esc_latch = EscLatch::SkipA;
        } else if (gc.in_active_3d_gameplay) {
            rt.esc_latch = EscLatch::PauseStart;
        } else if ((gc.pause_flags & 0x01u) != 0u || (gc.mode & 0x04u) == 0u || gc.ui_stack > 0) {
            rt.esc_latch = EscLatch::MenuBackB;
        } else {
            rt.esc_latch = EscLatch::PauseStart;
        }
    } else if (!esc_down) {
        rt.esc_latch = EscLatch::None;
    }
    rt.esc_was_down = esc_down;

    switch (rt.esc_latch) {
        case EscLatch::SkipA: pad.analog[0] = 255; break;          // A (1063 Cutscene/FMV Skip)
        case EscLatch::PauseStart: pad.buttons |= (1u << 4); break;// Start (1002 Pause)
        case EscLatch::MenuBackB: pad.analog[1] = 255; break;      // B (1025 Menu Back / Unpause)
        case EscLatch::None: break;
    }

    // 4. Menu / Cutscene / FMV navigation conveniences when not in active 3D gameplay
    if (!gc.in_active_3d_gameplay) {
        if (rt.keys[VK_RETURN]) pad.analog[0] = 255;  // A = Confirm / Skip
        if (rt.keys[VK_SPACE]) pad.analog[0] = 255;   // Space = Confirm / Skip
        if (rt.keys[VK_BACK]) pad.analog[1] = 255;    // Backspace = Back / Cancel
        if (rt.keys[VK_DELETE]) pad.analog[4] = 255;  // Delete = Black (Delete Save)
        if (rt.keys[VK_UP]) pad.buttons |= (1u << 0);
        if (rt.keys[VK_DOWN]) pad.buttons |= (1u << 1);
        if (rt.keys[VK_LEFT]) pad.buttons |= (1u << 2);
        if (rt.keys[VK_RIGHT]) pad.buttons |= (1u << 3);
        if ((gc.fmv || gc.cutscene) && gc.ui_stack == 0) {
            if (rt.menu_lmb_pressed) {
                rt.menu_lmb_pressed = false;
                rt.cutscene_lmb_skip_polls = 2;
            }
        }
        if (rt.cutscene_lmb_skip_polls > 0) {
            --rt.cutscene_lmb_skip_polls;
            pad.analog[0] = 255;  // A = Skip FMV / Cutscene
        }
    } else {
        rt.cutscene_lmb_skip_polls = 0;
    }
}

void MkbNotifyGamepadRightStickActive() { Rt().mouse_look_active.store(false, std::memory_order_release); }

void MkbNotifyScheme1HookApplied() { Rt().scheme1_applied.store(true, std::memory_order_release); }

bool MkbIsMouseLookActive() {
    MkbRuntimeState& rt = Rt();
    if (suffering::IsPadOnlyInputSession() || rt.cfg.input_mode == InputMode::ControllerOnly) return false;
    return rt.mouse_look_active.load(std::memory_order_acquire);
}

bool MkbConsumeMouseLook(float dt, float guest_sens_x, float guest_sens_y, float& out_look_x, float& out_look_y) {
    MkbRuntimeState& rt = Rt();
    out_look_x = 0.0f;
    out_look_y = 0.0f;
    if (suffering::IsPadOnlyInputSession() || rt.cfg.input_mode == InputMode::ControllerOnly) return false;

    double dx = 0.0, dy = 0.0;
    {
        std::lock_guard<std::mutex> lock(rt.input_lock);
        dx = rt.raw_dx;
        dy = rt.raw_dy;
        rt.raw_dx = 0.0;
        rt.raw_dy = 0.0;
        if (rt.cfg.mouse_smoothing) {
            rt.smooth_dx = 0.5f * float(dx) + 0.5f * rt.smooth_dx;
            rt.smooth_dy = 0.5f * float(dy) + 0.5f * rt.smooth_dy;
            if (std::fabs(rt.smooth_dx) < 0.02f) rt.smooth_dx = 0.0f;
            if (std::fabs(rt.smooth_dy) < 0.02f) rt.smooth_dy = 0.0f;
            dx = rt.smooth_dx;
            dy = rt.smooth_dy;
        }
    }

    if (dx != 0.0 || dy != 0.0) {
        rt.mouse_look_active.store(true, std::memory_order_release);
    }
    if (!rt.mouse_look_active.load(std::memory_order_acquire)) return false;

    // Widen safe_dt to [1e-4, 0.1] so 1/(safe_dt * sx) cancels guest dt 1-to-1 from 10 FPS to 1000+ FPS.
    float safe_dt = std::clamp(dt, 1.0e-4f, 0.1f);
    float sx = std::max(guest_sens_x, 0.1f);
    float sy = std::max(guest_sens_y, 0.1f);
    float user_sens = std::clamp(rt.cfg.mouse_sensitivity, 0.10f, 5.00f);

    // Polarity:
    //   dx > 0 (mouse right) -> out_look_x > 0 -> RS_RIGHT (+0x5C)
    //   dy < 0 (mouse up)    -> out_look_y > 0 when !mouse_invert_y -> RS_UP (+0x50)
    out_look_x = float(dx) * (kYawScaleAt1x * user_sens) / (safe_dt * sx);
    float signed_dy = rt.cfg.mouse_invert_y ? float(dy) : float(-dy);
    out_look_y = signed_dy * (kPitchScaleAt1x * user_sens) / (safe_dt * sy);
    return true;
}

GuestUiPoint MkbTransformPhysicalToGuestUi(float phys_x, float phys_y, uint32_t bb_w, uint32_t bb_h, uint32_t vp_w,
                                           uint32_t vp_h, uint32_t render_w, uint32_t render_h,
                                           uint32_t active_upscaler_mode, bool widescreen, float guest_w,
                                           float guest_h) {
    GuestUiPoint out{};
    if (guest_w <= 1.0f) guest_w = 640.0f;
    if (guest_h <= 1.0f) guest_h = 480.0f;
    out.gx = guest_w * 0.5f;
    out.gy = guest_h * 0.5f;
    out.gx_clamped = out.gx;
    out.gy_clamped = out.gy;
    if (bb_w == 0 || bb_h == 0) return out;

    if (vp_w == 0 || vp_h == 0) {
        float aspect = widescreen ? (16.0f / 9.0f) : (4.0f / 3.0f);
        float vw = std::min(float(bb_w), float(bb_h) * aspect);
        float vh = vw / aspect;
        vp_w = std::max(1u, uint32_t(std::lround(vw)));
        vp_h = std::max(1u, uint32_t(std::lround(vh)));
    }

    uint32_t eff_vpw = vp_w;
    uint32_t eff_vph = vp_h;
    uint32_t eff_vpx = (bb_w > eff_vpw) ? (bb_w - eff_vpw) / 2u : 0u;
    uint32_t eff_vpy = (bb_h > eff_vph) ? (bb_h - eff_vph) / 2u : 0u;

    if (static_cast<app::UpscalerMode>(active_upscaler_mode) == app::UpscalerMode::Integer && render_w > 0 &&
        render_h > 0 && render_w <= bb_w && render_h <= bb_h) {
        uint32_t k = std::max(1u, std::min(bb_w / render_w, bb_h / render_h));
        eff_vpw = render_w * k;
        eff_vph = render_h * k;
        eff_vpx = (bb_w > eff_vpw) ? (bb_w - eff_vpw) / 2u : 0u;
        eff_vpy = (bb_h > eff_vph) ? (bb_h - eff_vph) / 2u : 0u;
    }

    if (eff_vpw == 0 || eff_vph == 0) return out;

    out.in_viewport = (phys_x >= float(eff_vpx) && phys_x < float(eff_vpx + eff_vpw) &&
                       phys_y >= float(eff_vpy) && phys_y < float(eff_vpy + eff_vph));
    out.gx = ((phys_x - float(eff_vpx)) / float(eff_vpw)) * guest_w;
    out.gy = ((phys_y - float(eff_vpy)) / float(eff_vph)) * guest_h;
    out.gx_clamped = std::clamp(out.gx, 0.0f, guest_w);
    out.gy_clamped = std::clamp(out.gy, 0.0f, guest_h);
    return out;
}

MenuMouseFrame MkbConsumeMenuMouse(float guest_w, float guest_h) {
    MkbRuntimeState& rt = Rt();
    MenuMouseFrame f{};
    if (suffering::IsPadOnlyInputSession() || rt.cfg.input_mode == InputMode::ControllerOnly || UiIsOverlayOpen() ||
        !rt.focused.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(rt.input_lock);
        rt.menu_lmb_pressed = false;
        rt.menu_rmb_pressed = false;
        rt.menu_lmb_released = false;
        rt.menu_wheel_steps = 0;
        return f;
    }

    float px = 0.0f, py = 0.0f;
    {
        std::lock_guard<std::mutex> lock(rt.input_lock);
        f.active = true;
        f.has_pos = rt.menu_has_pos;
        px = rt.menu_phys_x;
        py = rt.menu_phys_y;
        f.moved = rt.menu_has_pos && (rt.menu_move_seq != rt.menu_consumed_move_seq);
        rt.menu_consumed_move_seq = rt.menu_move_seq;
        f.lmb_pressed = rt.menu_lmb_pressed;
        rt.menu_lmb_pressed = false;
        f.lmb_down = rt.menu_lmb_down;
        f.lmb_released = rt.menu_lmb_released;
        rt.menu_lmb_released = false;
        f.rmb_pressed = rt.menu_rmb_pressed;
        rt.menu_rmb_pressed = false;
        f.wheel_steps = rt.menu_wheel_steps;
        rt.menu_wheel_steps = 0;
    }

    if (f.has_pos) {
        gpu::UpscalerStatus us = gpu::GetUpscalerStatus();
        uint32_t bb_w = us.bb_w;
        uint32_t bb_h = us.bb_h;
        if (bb_w == 0 || bb_h == 0) {
            app::Mode out_m = app::Output();
            bb_w = out_m.width;
            bb_h = out_m.height;
        }
        f.pt = MkbTransformPhysicalToGuestUi(px, py, bb_w, bb_h, us.vp_w, us.vp_h, us.render_w, us.render_h,
                                             us.active_mode, app::Widescreen(), guest_w, guest_h);
    }
    return f;
}

void MkbSetLastMenuDebugState(uint32_t top_vt, int32_t focused_idx, float guest_gx, float guest_gy, bool in_vp) {
    MkbRuntimeState& rt = Rt();
    rt.dbg_top_vt.store(top_vt, std::memory_order_relaxed);
    rt.dbg_focused_idx.store(focused_idx, std::memory_order_relaxed);
    rt.dbg_guest_gx.store(guest_gx, std::memory_order_relaxed);
    rt.dbg_guest_gy.store(guest_gy, std::memory_order_relaxed);
    rt.dbg_in_vp.store(in_vp, std::memory_order_relaxed);
}

bool MkbRunMenuCoordSelfTest() {
    auto approx_eq = [](float a, float b) { return std::fabs(a - b) < 0.05f; };
    bool ok = true;
    // 1. 2560x1440 16:9 standard -> center (1280,720) maps to (320,240)
    auto p1 = MkbTransformPhysicalToGuestUi(1280.0f, 720.0f, 2560, 1440, 2560, 1440, 2560, 1440, 0, true);
    ok = ok && p1.in_viewport && approx_eq(p1.gx, 320.0f) && approx_eq(p1.gy, 240.0f);
    // 2. 2560x1440 4:3 pillarboxed (1920x1440 @ +320px)
    auto p2a = MkbTransformPhysicalToGuestUi(100.0f, 720.0f, 2560, 1440, 1920, 1440, 640, 480, 0, false);
    auto p2b = MkbTransformPhysicalToGuestUi(320.0f, 0.0f, 2560, 1440, 1920, 1440, 640, 480, 0, false);
    ok = ok && !p2a.in_viewport && approx_eq(p2a.gx_clamped, 0.0f) && p2b.in_viewport && approx_eq(p2b.gx, 0.0f) &&
         approx_eq(p2b.gy, 0.0f);
    // 3. 1280x800 Steam Deck 16:10 -> 1280x720 @ +40px letterbox
    auto p3a = MkbTransformPhysicalToGuestUi(640.0f, 20.0f, 1280, 800, 1280, 720, 1280, 720, 0, true);
    auto p3b = MkbTransformPhysicalToGuestUi(640.0f, 400.0f, 1280, 800, 1280, 720, 1280, 720, 0, true);
    ok = ok && !p3a.in_viewport && p3b.in_viewport && approx_eq(p3b.gx, 320.0f) && approx_eq(p3b.gy, 240.0f);
    // 4. 3440x1440 Ultrawide 21:9 -> 2560x1440 @ +440px pillarbox
    auto p4a = MkbTransformPhysicalToGuestUi(200.0f, 720.0f, 3440, 1440, 2560, 1440, 2560, 1440, 0, true);
    auto p4b = MkbTransformPhysicalToGuestUi(1720.0f, 720.0f, 3440, 1440, 2560, 1440, 2560, 1440, 0, true);
    ok = ok && !p4a.in_viewport && p4b.in_viewport && approx_eq(p4b.gx, 320.0f) && approx_eq(p4b.gy, 240.0f);
    // 5. 1024x768 4:3 exact
    auto p5 = MkbTransformPhysicalToGuestUi(512.0f, 384.0f, 1024, 768, 1024, 768, 640, 480, 0, false);
    ok = ok && p5.in_viewport && approx_eq(p5.gx, 320.0f) && approx_eq(p5.gy, 240.0f);
    // 6. Integer upscaler (2560x1440 with 640x480 render -> k=3 -> 1920x1440 @ +320px)
    uint32_t int_mode = static_cast<uint32_t>(app::UpscalerMode::Integer);
    auto p6a = MkbTransformPhysicalToGuestUi(100.0f, 720.0f, 2560, 1440, 2560, 1440, 640, 480, int_mode, true);
    auto p6b = MkbTransformPhysicalToGuestUi(1280.0f, 720.0f, 2560, 1440, 2560, 1440, 640, 480, int_mode, true);
    ok = ok && !p6a.in_viewport && p6b.in_viewport && approx_eq(p6b.gx, 320.0f) && approx_eq(p6b.gy, 240.0f);
    return ok;
}

bool MkbIsSyntheticTestActive() {
    MkbRuntimeState& rt = Rt();
    return rt.test_active || rt.telemetry_file != nullptr;
}

void MkbAdvanceSyntheticEvents() {
    MkbRuntimeState& rt = Rt();
    if (!rt.test_active && !rt.telemetry_file) return;

    auto now = std::chrono::steady_clock::now();
    if (!rt.test_clock_started) {
        rt.test_clock_started = true;
        rt.test_t0 = now;
    }
    uint64_t ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - rt.test_t0).count());
    rt.pending_event_ms = ms;
    rt.pending_event_tag.clear();

#ifdef _WIN32
    HWND hwnd = static_cast<HWND>(rt.game_hwnd.load(std::memory_order_acquire));
#endif
    for (SyntheticEvent& ev : rt.test_events) {
        if (ev.fired || ms < ev.at_ms) continue;
        ev.fired = true;
        if (!rt.pending_event_tag.empty()) rt.pending_event_tag += ";";
        rt.pending_event_tag += ev.cmd;
        if (ev.cmd == "keydown" || ev.cmd == "keyup") {
            BindSlot bs = BindSlotFromToken(ev.arg1);
            if (bs.kind == BindKind::Key && bs.vk < 256) {
                std::lock_guard<std::mutex> lock(rt.input_lock);
                rt.keys[bs.vk] = (ev.cmd == "keydown");
            }
            rt.pending_event_tag += ":" + ev.arg1;
        } else if (ev.cmd == "mbtn_down" || ev.cmd == "mbtn_up") {
            BindSlot bs = BindSlotFromToken(ev.arg1);
            int idx = -1;
            if (bs.kind == BindKind::MouseLeft) idx = 0;
            else if (bs.kind == BindKind::MouseRight) idx = 1;
            else if (bs.kind == BindKind::MouseMiddle) idx = 2;
            else if (bs.kind == BindKind::Mouse4) idx = 3;
            else if (bs.kind == BindKind::Mouse5) idx = 4;
            if (idx >= 0) {
                std::lock_guard<std::mutex> lock(rt.input_lock);
                rt.mouse_buttons[size_t(idx)] = (ev.cmd == "mbtn_down");
            }
            rt.pending_event_tag += ":" + ev.arg1;
        } else if (ev.cmd == "mouse") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.raw_dx += double(ev.val1);
            rt.raw_dy += double(ev.val2);
            rt.mouse_look_active.store(true, std::memory_order_release);
            rt.pending_event_tag += ":" + std::to_string(ev.val1) + "," + std::to_string(ev.val2);
        } else if (ev.cmd == "wheel") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            if (ev.val1 > 0) rt.wheel_up_queue = std::min(rt.wheel_up_queue + ev.val1, kMaxWheelQueue);
            else if (ev.val1 < 0) rt.wheel_down_queue = std::min(rt.wheel_down_queue - ev.val1, kMaxWheelQueue);
            rt.pending_event_tag += ":" + std::to_string(ev.val1);
        } else if (ev.cmd == "menu_cursor") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            UpdateMenuCursorPhysLocked(rt, float(ev.val1), float(ev.val2));
            rt.pending_event_tag += ":" + std::to_string(ev.val1) + "," + std::to_string(ev.val2);
        } else if (ev.cmd == "menu_lmb_down") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[0] = true;
            rt.suppressed_mouse_mask |= uint8_t(1u << 0);
            rt.menu_lmb_pressed = true;
            rt.menu_lmb_down = true;
        } else if (ev.cmd == "menu_lmb_up") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[0] = false;
            rt.suppressed_mouse_mask &= ~uint8_t(1u << 0);
            if (rt.menu_lmb_down) {
                rt.menu_lmb_down = false;
                rt.menu_lmb_released = true;
            }
        } else if (ev.cmd == "menu_rmb_down") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[1] = true;
            rt.suppressed_mouse_mask |= uint8_t(1u << 1);
            rt.menu_rmb_pressed = true;
        } else if (ev.cmd == "menu_rmb_up") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.mouse_buttons[1] = false;
            rt.suppressed_mouse_mask &= ~uint8_t(1u << 1);
        } else if (ev.cmd == "menu_rmb_click") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.menu_rmb_pressed = true;
        } else if (ev.cmd == "menu_wheel") {
            std::lock_guard<std::mutex> lock(rt.input_lock);
            rt.menu_wheel_steps = std::clamp(rt.menu_wheel_steps + ev.val1, -16, 16);
            rt.pending_event_tag += ":" + std::to_string(ev.val1);
        } else if (ev.cmd == "overlay") {
            UiSetOverlayOpen(ev.val1 != 0);
            rt.pending_event_tag += ":" + std::to_string(ev.val1);
        } else if (ev.cmd == "focus") {
            bool active = (ev.val1 != 0);
            rt.focused.store(active, std::memory_order_release);
            if (!active) {
                std::lock_guard<std::mutex> lock(rt.input_lock);
                ClearAllHeldInputsLocked(rt);
            }
            UiRequestRefreshCursor();
            rt.pending_event_tag += ":" + std::to_string(ev.val1);
        } else if (ev.cmd == "exit") {
            if (!rt.test_exit_posted) {
                rt.test_exit_posted = true;
#ifdef _WIN32
                if (hwnd) PostMessageW(hwnd, WM_CLOSE, 0, 0);
#else
                SDL_Event qev{};
                qev.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&qev);
#endif
            }
        }
    }
}

void MkbLogSyntheticTelemetry(uint64_t poll_id, const input::Pad& merged_pad, bool overlay_open) {
    MkbRuntimeState& rt = Rt();
    if (!rt.telemetry_file) return;
    if (!rt.pending_event_tag.empty() || poll_id == 0 || (poll_id % 3ull == 0)) {
        GuestContext gc = MkbSampleGuestContext();
        WriteTelemetryLine(rt, poll_id, rt.pending_event_ms, gc, merged_pad, overlay_open,
                           rt.pending_event_tag.c_str());
    }
}

}  // namespace suffering
