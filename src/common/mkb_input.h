// SPDX-License-Identifier: GPL-2.0-or-later
// Native-PC Mouse & Keyboard (M&K) input system for The Suffering (USA, Title ID 0x4D570015).
// Supports Win32 Raw Input (WM_INPUT), dynamic cursor capture/clip, diagonal-normalized WASD,
// framerate-independent post-deadzone linear mouse look, queued mouse-wheel actions, context-aware
// Esc/menu UX, seamless hot-switching with SDL3 gamepads, interactive UI rebinding, and [controls]
// persistence in settings.toml.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace input {
struct Pad;
}

namespace suffering {

enum class InputMode : uint32_t {
    Auto = 0,               // Mouse/KB + Controller simultaneous with instant hot-switch
    MouseKeyboardOnly = 1,  // Ignore physical controller, always present virtual Port 0 for M&K
    ControllerOnly = 2,     // Pure gamepad mode; disables all M&K gameplay merging & hooks
};

const char* InputModeName(InputMode mode);
const char* InputModeToken(InputMode mode);

enum class BindKind : uint8_t {
    None = 0,
    Key = 1,
    MouseLeft = 2,
    MouseRight = 3,
    MouseMiddle = 4,
    Mouse4 = 5,
    Mouse5 = 6,
    WheelUp = 7,
    WheelDown = 8,
};

struct BindSlot {
    BindKind kind = BindKind::None;
    uint16_t vk = 0;  // Used only when kind == BindKind::Key (supports VK_LSHIFT/RSHIFT/etc.)

    bool IsEmpty() const { return kind == BindKind::None; }
    bool operator==(const BindSlot& o) const { return kind == o.kind && vk == o.vk; }
};

enum class MkbAction : uint32_t {
    MoveForward = 0,
    MoveBack,
    MoveLeft,
    MoveRight,
    Fire,
    ThrowGrenade,
    Jump,
    Reload,
    Interact,
    Flashlight,
    HealXombium,
    InsanityTransform,
    CameraToggle,
    ArchivesMap,
    WeaponCatUp,
    ItemCatDown,
    PrevItemLeft,
    NextItemRight,
    Count,
};

constexpr size_t kMkbActionCount = static_cast<size_t>(MkbAction::Count);

struct MkbActionInfo {
    MkbAction action;
    const char* key;          // settings.toml key under [controls.bindings]
    const char* label;        // Human-readable action name in UI
    const char* xbox_action;  // Original Xbox mapping note
};

const std::array<MkbActionInfo, kMkbActionCount>& MkbActionTable();
std::string BindSlotToToken(const BindSlot& slot);
std::string BindSlotToDisplay(const BindSlot& slot);
BindSlot BindSlotFromToken(const std::string& token);

struct MkbConfig {
    InputMode input_mode = InputMode::Auto;
    float mouse_sensitivity = 1.0f;  // 0.10 .. 5.00 (~0.09 deg/count at 1.0)
    bool mouse_invert_y = false;     // false = PC standard (Mouse UP looks UP)
    bool mouse_raw_aim = true;       // true = bypass controller sticky slowdown [player+0x664] during mouse look
    bool mouse_smoothing = false;    // false = 1:1 raw counts; true = light 2-frame EMA
    std::array<std::array<BindSlot, 2>, kMkbActionCount> bindings{};
};

MkbConfig& MkbGetConfig();
void MkbResetDefaults();

// Persistence hooks wired into app::kGame.load / app::kGame.save (toml::table*)
void MkbLoadSettings(const void* table);
void MkbSaveSettings(void* table);

// Mark fields edited via UI so temporary CLI overrides do not overwrite saved settings.toml values.
void MkbMarkInputModeUserEdited();
void MkbMarkMouseSensUserEdited();
void MkbMarkMouseInvertYUserEdited();

// Lifecycle & Win32 hooks
void MkbInit();
void MkbOnWindow(void* hwnd);
bool MkbOnMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result);

// Interactive Click-to-Rebind state machine (shared by Pre-Game Launcher and In-Game F1 Overlay)
void MkbBeginRebind(MkbAction action, int slot);  // slot = 0 (Primary) or 1 (Secondary)
void MkbCancelRebind();
bool MkbIsRebinding(MkbAction* out_action = nullptr, int* out_slot = nullptr);
// Returns true if message was consumed by active rebind capture. Call BEFORE Esc/F1 handling!
bool MkbHandleRebindMessage(uint32_t msg, uintptr_t wp, intptr_t lp);

// Cursor capture & clip coordination (called on Win32 window thread)
bool MkbShouldCaptureCursor();
bool MkbIsCursorCaptured();
void MkbSyncCursorOnWindowThread(void* hwnd);
void MkbForceReleaseCursor(void* hwnd);
void MkbOnOverlayToggled(bool open, void* hwnd);

// Guest context & gameplay pad merging
struct GuestContext {
    uint32_t session = 0;
    uint32_t mode = 0;
    uint32_t lvl_flags = 0;
    uint32_t pause_flags = 0;
    uint32_t loading_ptr = 0;
    int32_t ui_stack = 0;
    bool fmv = false;
    bool cutscene = false;
    bool in_active_3d_gameplay = false;
};

GuestContext MkbSampleGuestContext();
bool MkbIsInActive3DGameplay();

// Called on guest poll thread in OnPad() when M&K is enabled and overlay is closed.
void MkbMergeGameplayPad(input::Pad& pad);
void MkbNotifyGamepadRightStickActive();
void MkbNotifyScheme1HookApplied();

// Called from src/game/hooks.cpp inside CXboxInputDevice::Poll / Yaw / Pitch / Turret hooks.
bool MkbIsMouseLookActive();
bool MkbConsumeMouseLook(float dt, float guest_sens_x, float guest_sens_y, float& out_look_x, float& out_look_y);

// Automated test harness support (--test-mkb / --mkb-telemetry)
bool MkbIsSyntheticTestActive();
void MkbAdvanceSyntheticEvents();
void MkbLogSyntheticTelemetry(uint64_t poll_id, const input::Pad& merged_pad, bool overlay_open);

}  // namespace suffering
