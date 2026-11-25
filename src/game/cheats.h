// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace suffering::cheats {

// Persistent cheat toggles (thread-safe atomic reads/writes from any host or guest thread).
// Automatically suppressed whenever app::IsReplayOrAutotestActive() is true.
enum class CheatToggle : uint8_t {
    GodMode = 0,              // Torque takes zero damage from all sources and stays at max HP
    InfiniteReserveAmmo = 1,  // Reserve ammo never depletes on reload and stays full for owned firearms
    BottomlessClip = 2,       // Active weapon clip/fuel never depletes and never triggers reload
    InfiniteItems = 3,        // Xombium bottles, Grenades, Molotovs, Dynamite, and Flares never decrease when used
    InfiniteFlashlight = 4,   // Flashlight battery stays at 100% full beam intensity and never dims/drains
    InfiniteInsanity = 5,     // Insanity meter stays 100% ready and Beast transformation never drains
    OneHitKills = 6,          // Hostile enemies damaged by Torque die instantly (excludes Torque, Dallas #2152, allies, props)
    PsychedelicMode = 7,      // Native XBE "Tripping the Light Fantastic" rainbow/blur post-process filter (1.0x speed)
    OldMovieMode = 8,         // Native XBE "Murnau Mode" vintage B&W film grain & projector overlay (1.0x speed)
    Count = 9
};

inline constexpr size_t kCheatToggleCount = static_cast<size_t>(CheatToggle::Count);

struct CheatToggleInfo {
    CheatToggle id;
    const char* key;
    const char* label;
    const char* description;
    bool is_visual;
};

// Queued one-shot actions (thread-safe enqueue from UI thread; executed exclusively on Guest Main Thread 0x14
// inside GameApp::OnFrameTick when in an active level with a valid live CTorque instance).
enum class CheatAction : uint8_t {
    FullHealAndXombium = 0,     // Restore 100% HP + max Xombium bottles (9)
    GiveAllWeaponsAndAmmo = 1,  // Native XBE full weapon/item grant + max ammo & clip
    MaxAllAmmo = 2,             // Max reserve ammo for all owned firearms + active magazine
    GiveGonzoGun = 3,           // Native XBE secret chicken-firing Gonzo Gun ("Free Range")
    RefillAllItems = 4,         // Max Xombium (9), Shrapnel Grenades, Molotovs, Dynamite, Flares, & Flashlight Batteries
    FillInsanityMeter = 5,      // Fill Insanity meter to 100%
    ShiftMoralityGood = 6,      // Shift Reputation/Morality +50 toward Good
    ResetMoralityNeutral = 7,   // Reset Reputation/Morality to exact Neutral (0)
    ShiftMoralityEvil = 8,      // Shift Reputation/Morality -50 toward Evil
    SetMaxMoralityGood = 9,     // Set Reputation/Morality to 100% Max Good
    SetMaxMoralityEvil = 10,    // Set Reputation/Morality to 100% Max Evil
    BloodsoakedTorque = 11,     // Cover Torque head-to-toe in blood ("Oh God, mother, blood, no!")
    CleanTorque = 12,           // Wash all blood off Torque ("Wash It All Away")
    Count = 13
};

inline constexpr size_t kCheatActionCount = static_cast<size_t>(CheatAction::Count);

enum class CheatActionCategory : uint8_t {
    Loadout = 0,
    Morality = 1,
    Visual = 2
};

struct CheatActionInfo {
    CheatAction id;
    CheatActionCategory category;
    const char* label;
    const char* description;
};

enum class MoralityTier : uint8_t {
    Evil = 0,
    Neutral = 1,
    Good = 2
};

// Thread-safe per-frame snapshot updated once per guest frame on Guest Main Thread 0x14.
// Safe to call from Dear ImGui / render / window threads without touching raw guest RAM.
struct CheatRuntimeStatus {
    bool replay_active = false;         // true when --replay / --autotest is active (all cheats suppressed)
    bool in_level = false;              // true when loaded into an active gameplay level (incl. Pause Menu)
    bool player_alive = false;          // true when live non-destroyed CTorque instance is present & alive
    bool is_transformed = false;        // true when Torque is currently in Beast/Monster form
    float health = 0.0f;
    float max_health = 100.0f;
    int32_t clip_ammo = 0;
    int32_t max_clip_ammo = 0;
    int32_t reserve_ammo = 0;
    int32_t xombium_count = 0;
    int32_t max_xombium = 9;
    float insanity = 0.0f;
    float max_insanity = 100.0f;
    float insanity_pct = 0.0f;          // 0.0f .. 100.0f
    float flashlight_pct = 100.0f;      // 0.0f .. 100.0f
    float morality_value = 0.0f;        // Raw reputation points (typically -100.0f .. +100.0f)
    float morality_min = -100.0f;       // Player template prop 0x46 (Max Evil)
    float morality_max = 100.0f;        // Player template prop 0x47 (Max Good)
    float morality_normalized = 0.0f;   // -1.0f (Max Evil) .. 0.0f (Neutral) .. +1.0f (Max Good)
    MoralityTier morality_tier = MoralityTier::Neutral;
    uint32_t active_toggle_count = 0;   // 0 when replay_active is true
    uint64_t last_action_seq = 0;       // Increments whenever feedback updates
    bool last_action_is_warning = false;
    std::string last_action_feedback;
};

// Descriptor tables for UI iteration
const std::array<CheatToggleInfo, kCheatToggleCount>& GetToggleInfos();
const std::array<CheatActionInfo, kCheatActionCount>& GetActionInfos();
const char* MoralityTierName(MoralityTier tier);

// Generic persistent toggle API (thread-safe lock-free atomics)
void SetToggle(CheatToggle toggle, bool enabled);
bool GetToggle(CheatToggle toggle);          // Raw user-armed toggle state (even before entering a level)
bool IsToggleEffective(CheatToggle toggle);  // Effective state (false when app::IsReplayOrAutotestActive())
bool ToggleCheat(CheatToggle toggle);        // Flips toggle and returns new state
void DisableAllCheats();
uint32_t GetActiveToggleCount();             // Returns 0 when app::IsReplayOrAutotestActive()

// Named convenience wrappers for persistent toggles
inline void SetGodMode(bool on) { SetToggle(CheatToggle::GodMode, on); }
inline bool GetGodMode() { return GetToggle(CheatToggle::GodMode); }

inline void SetInfiniteReserveAmmo(bool on) { SetToggle(CheatToggle::InfiniteReserveAmmo, on); }
inline bool GetInfiniteReserveAmmo() { return GetToggle(CheatToggle::InfiniteReserveAmmo); }

inline void SetBottomlessClip(bool on) { SetToggle(CheatToggle::BottomlessClip, on); }
inline bool GetBottomlessClip() { return GetToggle(CheatToggle::BottomlessClip); }

inline void SetInfiniteItems(bool on) { SetToggle(CheatToggle::InfiniteItems, on); }
inline bool GetInfiniteItems() { return GetToggle(CheatToggle::InfiniteItems); }

inline void SetInfiniteFlashlight(bool on) { SetToggle(CheatToggle::InfiniteFlashlight, on); }
inline bool GetInfiniteFlashlight() { return GetToggle(CheatToggle::InfiniteFlashlight); }

inline void SetInfiniteInsanity(bool on) { SetToggle(CheatToggle::InfiniteInsanity, on); }
inline bool GetInfiniteInsanity() { return GetToggle(CheatToggle::InfiniteInsanity); }

inline void SetOneHitKills(bool on) { SetToggle(CheatToggle::OneHitKills, on); }
inline bool GetOneHitKills() { return GetToggle(CheatToggle::OneHitKills); }

inline void SetPsychedelicMode(bool on) { SetToggle(CheatToggle::PsychedelicMode, on); }
inline bool GetPsychedelicMode() { return GetToggle(CheatToggle::PsychedelicMode); }

inline void SetOldMovieMode(bool on) { SetToggle(CheatToggle::OldMovieMode, on); }
inline bool GetOldMovieMode() { return GetToggle(CheatToggle::OldMovieMode); }

// Thread-safe one-shot action queue & live runtime snapshot
void EnqueueAction(CheatAction action);
void ClearActionFeedback();
CheatRuntimeStatus GetRuntimeStatus();

}  // namespace suffering::cheats
