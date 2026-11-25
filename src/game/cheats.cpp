// SPDX-License-Identifier: GPL-2.0-or-later
// Thread-safe In-Game Cheat System Backend for The Suffering (USA) default.xbe (Title ID 0x4D570015).
//
// Reverse-engineered guest architecture & verified VAs:
//  - World & Player lookup: kVaWorldPtr (0x002D0DC0) -> [+0x12C] count, [+0x124] CGameObject** -> player_obj,
//    template class hash 0xC8BC44E1 (Player / CTorque via CTemplateMgr 0x002D0DBC sub_00060E80),
//    non-destroyed check ([obj+0x08] & 0x80000000) == 0, [player_obj+0x60] -> CTorque* (vtable 0x00299FC8).
//  - Health & Damage (CDamageable / CTorque / CNPC):
//      [+0x0E] m_bCannotDie, [+0x16] float m_health, [+0x1A] float m_maxHealth, [+0x1E] m_bInvulnerable.
//      CTorque::SetHealth = sub_0003E860, CTorque::TakeDamage = sub_0003EDC0, CTorque::ApplyHealthLoss = sub_0003EBE0,
//      CNPC::ApplyHealthLoss = sub_000F1E80, canonical AI/auto-aim hostility check AreHostile = sub_000F25E0.
//  - Weapons, Clip, Reserve Ammo, Throwables & Xombium:
//      [torque+0x108] CPlayerInventory (sub_0005CD30 GetCount, sub_0005D000 AddItem, sub_0005CE80 Consume),
//      [torque+0x114] int32 active clip ammo (decremented in sub_0003A850; max clip in sub_00048AC0),
//      [torque+0x3C6] active weapon handle, [torque+0x3C8] offhand dual-wield handle,
//      [torque+0x658] owned weapon slots (stride 0x0C, count [+0x660]),
//      native XBE cheat helpers: sub_00053500(0..8) item/weapon grants, sub_0004B120 max Xombium (prop 0x77, max 9),
//      sub_00048CB0 active weapon reserve ammo, sub_00052A60 flashlight + batteries, sub_00040CA0 HUD refresh.
//  - Flashlight: [torque+0x134] handle -> sub_000173A0 CFlashlight* ([+0x9C] flags, [+0xA0] full timer, [+0xA4] dim timer,
//      sub_00025540 RechargeBattery, sub_00040AA0 HUD refresh).
//  - Insanity: [torque+0x458] float meter, max = sub_000436B0 (prop 0x100), setter = sub_00043EC0,
//      Beast transformed bit = ([torque+0xA4] & 0x100000) != 0, untransforming bit = ([torque+0xA8] & 0x02) != 0.
//  - Morality (Reputation): [torque+0x454] float reputation (min Evil = prop 0x46, max Good = prop 0x47),
//      sub_00051C80 AddMorality, sub_00051CD0 SetMaxEvil, sub_00051CA0 SetMaxGood, sub_0003C460 ReputationSuite::Refresh.
//  - Blood & Visuals: [torque+0x42] bloodiness float (sub_0003C150 Bloodsoaked, 0.0f Clean),
//      sub_0014B8A0 Psychedelic PostFX ([0x002DC75C]+0x18), 0x002D14CC (+0x0C) Murnau Old Movie Projector Effect.

#include "game/cheats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

// Hooked guest functions (verified 0 overlap with src/game/hooks.cpp)
X86_EXTERN_FUNC(__imp__sub_000BF6D0);
X86_EXTERN_FUNC(__imp__sub_0003EBE0);
X86_EXTERN_FUNC(__imp__sub_0003EDC0);
X86_EXTERN_FUNC(__imp__sub_0003A850);
X86_EXTERN_FUNC(__imp__sub_0005CE80);
X86_EXTERN_FUNC(__imp__sub_000F1E80);

namespace suffering::cheats {

namespace {

// Verified guest globals
constexpr uint32_t kVaTemplateMgrPtr = 0x002D0DBCu;
constexpr uint32_t kVaWorldPtr = 0x002D0DC0u;
constexpr uint32_t kVaProjectorEffect = 0x002D14CCu;
constexpr uint32_t kVaGameSessionState = 0x002DB204u;  // 1=Title/Frontend, 2=In-Level, 3=LeaveGame
constexpr uint32_t kVaLoadingScreenPtr = 0x002DC1F4u;
constexpr uint32_t kVaEngineModeFlags = 0x002DC5E4u;   // bit 2 (0x04) = active world, bit 3 (0x08) = FMV
constexpr uint32_t kVaRendererPtr = 0x002DC75Cu;
constexpr uint32_t kVaLevelFlags = 0x002DEF14u;        // bit 1 (0x02) = level loaded

// Verified class hashes & vtables
constexpr uint32_t kHashPlayer = 0xC8BC44E1u;
constexpr uint32_t kHashRangedWeapon = 0x9710D816u;
constexpr uint32_t kVtTorquePrimary = 0x00299FC8u;
constexpr uint32_t kVtHermes = 0x0029C460u;    // CHermes (0x04AD652D) — environmental burner boss / apparition
constexpr uint32_t kVtHorace = 0x0029C5B0u;    // CHorace (0xE589963F) — environmental generator boss / apparition
constexpr uint32_t kVtKilljoy = 0x0029C858u;   // CKilljoy (0x490379CD) — environmental projector boss / apparition
constexpr uint32_t kIidDallas = 2152u;
constexpr float kSentinelBossOrSetpieceHp = 5000.0f;  // Surreal sentinel HP (9999..999999) on puzzle bosses & cutscene NPCs

// Verified guest helper functions (called exclusively on Guest Main Thread 0x14 via CallGuest)
constexpr uint32_t kFnCastToFlashlight = 0x000173A0u;      // __cdecl(CGameObject*) -> CFlashlight*
constexpr uint32_t kFnRechargeFlashlight = 0x00025540u;    // __thiscall(CFlashlight*)
constexpr uint32_t kFnMakeBloodsoaked = 0x0003C150u;       // __thiscall(CTorque*)
constexpr uint32_t kFnRefreshReputation = 0x0003C460u;     // __thiscall(CTorque + 0x3E)
constexpr uint32_t kFnSetTorqueHealth = 0x0003E860u;       // __thiscall(CTorque*, float)
constexpr uint32_t kFnUpdateAmmoXombiumHud = 0x00040520u;  // __thiscall(CTorque*)
constexpr uint32_t kFnUpdateFlashlightHud = 0x00040AA0u;   // __thiscall(CTorque*)
constexpr uint32_t kFnRefreshAllHudMeters = 0x00040CA0u;   // __thiscall(CTorque*)
constexpr uint32_t kFnGetMaxInsanity = 0x000436B0u;        // __thiscall(CTorque*) -> ST(0) float
constexpr uint32_t kFnSetInsanity = 0x00043EC0u;           // __thiscall(CTorque*, float)
constexpr uint32_t kFnSyncClipToSlot = 0x00047E60u;        // __thiscall(CTorque*)
constexpr uint32_t kFnResolveInvTplIdx = 0x000489B0u;      // __thiscall(CTorque*, uint32 tpl_idx) -> int32
constexpr uint32_t kFnGetActiveMaxClip = 0x00048AC0u;      // __thiscall(CTorque*) -> int32
constexpr uint32_t kFnGetActiveReserve = 0x00048B60u;      // __thiscall(CTorque*) -> int32
constexpr uint32_t kFnAddActiveReserve = 0x00048CB0u;      // __thiscall(CTorque*, int32 delta)
constexpr uint32_t kFnGiveMaxXombium = 0x0004B120u;        // __thiscall(CTorque*)
constexpr uint32_t kFnAddMorality = 0x00051C80u;           // __thiscall(CTorque*, float delta)
constexpr uint32_t kFnSetMaxGoodMorality = 0x00051CA0u;    // __thiscall(CTorque*)
constexpr uint32_t kFnSetMaxEvilMorality = 0x00051CD0u;    // __thiscall(CTorque*)
constexpr uint32_t kFnGiveFlashlightBatts = 0x00052A60u;   // __thiscall(CTorque*)
constexpr uint32_t kFnExecItemWeaponCheat = 0x00053500u;   // __thiscall(CTorque*, int32 group_idx)
constexpr uint32_t kFnInvGetCount = 0x0005CD30u;           // __thiscall(CPlayerInventory*, int32 tpl_idx) -> int32
constexpr uint32_t kFnInvAddItem = 0x0005D000u;            // __thiscall(CPlayerInventory*, int32 tpl_idx, int32 count) -> int32
constexpr uint32_t kFnGetTemplate = 0x00060E80u;           // __thiscall(CTemplateMgr*, uint32 tpl_idx) -> CTemplate*
constexpr uint32_t kFnAreHostile = 0x000F25E0u;            // __cdecl(CGameObject* src, CGameObject* dst, float max_dist) -> int32
constexpr uint32_t kFnSetPsychedelic = 0x0014B8A0u;        // __stdcall(int32 enable)
constexpr uint32_t kFnGetLevelTemplateObj = 0x00169D50u;   // __thiscall(CLevelSub*, uint32 id) -> CGameObject*

// Verified CPlayerInventory::Consume (sub_0005CE80) return addresses for surgical filtering
constexpr uint32_t kRetConsumeThrowable = 0x0003BF36u;     // Throwing Grenade / Molotov / Dynamite / Flare
constexpr uint32_t kRetConsumeReserveAmmo = 0x00048D25u;   // Reloading active ranged weapon
constexpr uint32_t kRetConsumeBattery = 0x000549A0u;       // Recharging flashlight battery
constexpr uint32_t kRetConsumeXombium = 0x000598B4u;       // Drinking Xombium bottle to heal

const std::array<CheatToggleInfo, kCheatToggleCount> kToggleInfos = {{
    {CheatToggle::GodMode, "god_mode", "God Mode (Infinite Health)",
     "Zero damage from enemies, fire, explosions, falls, and insanity drain.", false},
    {CheatToggle::InfiniteReserveAmmo, "infinite_reserve_ammo", "Infinite Reserve Ammo",
     "Reserve ammo never decreases on reload and stays maxed for owned guns.", false},
    {CheatToggle::BottomlessClip, "bottomless_clip", "Bottomless Clip (No Reload)",
     "Equipped weapon magazine and flamethrower fuel stay 100% full.", false},
    {CheatToggle::InfiniteItems, "infinite_items", "Infinite Items & Throwables",
     "Xombium bottles, Grenades, Molotovs, Dynamite, and Flares never deplete.", false},
    {CheatToggle::InfiniteFlashlight, "infinite_flashlight", "Infinite Flashlight Battery",
     "Flashlight beam stays at 100% intensity and never dims or drains.", false},
    {CheatToggle::InfiniteInsanity, "infinite_insanity", "Infinite Insanity",
     "Insanity meter stays 100% ready and Beast form never drains over time.", false},
    {CheatToggle::OneHitKills, "one_hit_kills", "One-Hit Kills (Hostiles Only)",
     "Hostile enemies hit by Torque die instantly (allies & props protected).", false},
    {CheatToggle::PsychedelicMode, "psychedelic_mode", "Psychedelic Mode (\"Tripping\")",
     "Built-in XBE rainbow trail post-processing filter at normal 1.0x speed.", true},
    {CheatToggle::OldMovieMode, "old_movie_mode", "Old Movie Projector (\"Murnau\")",
     "Built-in XBE vintage black-and-white film grain & scratches filter.", true},
}};

const std::array<CheatActionInfo, kCheatActionCount> kActionInfos = {{
    {CheatAction::FullHealAndXombium, CheatActionCategory::Loadout, "Full Heal + Max Xombium",
     "Restore Torque to 100% HP and fill Xombium pills to 9/9."},
    {CheatAction::GiveAllWeaponsAndAmmo, CheatActionCategory::Loadout, "Give All Weapons & Max Ammo",
     "Grant full standard arsenal (melee, firearms, items) with full ammo."},
    {CheatAction::MaxAllAmmo, CheatActionCategory::Loadout, "Max All Firearm Ammo",
     "Refill reserve ammo and magazines for all owned firearms."},
    {CheatAction::GiveGonzoGun, CheatActionCategory::Loadout, "Give Gonzo Gun (Secret)",
     "Grant the secret chicken-launching Gonzo Gun with full ammo."},
    {CheatAction::RefillAllItems, CheatActionCategory::Loadout, "Refill All Throwables & Items",
     "Max Xombium, Shrapnel Grenades, Molotovs, Dynamite, Flares, and Batteries."},
    {CheatAction::FillInsanityMeter, CheatActionCategory::Loadout, "Fill Insanity Meter",
     "Instantly charge Torque's Insanity meter to 100%."},
    {CheatAction::ShiftMoralityGood, CheatActionCategory::Morality, "Shift Good (+50)",
     "Shift Torque's morality +50 toward Good."},
    {CheatAction::ResetMoralityNeutral, CheatActionCategory::Morality, "Reset Neutral (0)",
     "Reset Torque's morality score to exact Neutral."},
    {CheatAction::ShiftMoralityEvil, CheatActionCategory::Morality, "Shift Evil (-50)",
     "Shift Torque's morality -50 toward Evil."},
    {CheatAction::SetMaxMoralityGood, CheatActionCategory::Morality, "Set Max Good",
     "Set Torque's morality to 100% Good."},
    {CheatAction::SetMaxMoralityEvil, CheatActionCategory::Morality, "Set Max Evil",
     "Set Torque's morality to 100% Evil."},
    {CheatAction::BloodsoakedTorque, CheatActionCategory::Visual, "Bloodsoaked Torque",
     "Drench Torque completely in blood."},
    {CheatAction::CleanTorque, CheatActionCategory::Visual, "Clean Torque",
     "Wash all blood off Torque's skin and clothes."},
}};

std::array<std::atomic<bool>, kCheatToggleCount> g_toggles{};

std::mutex g_state_mutex;
std::vector<CheatAction> g_pending_actions;
CheatRuntimeStatus g_runtime_status{};
uint64_t g_feedback_seq = 0;
bool g_feedback_is_warning = false;
std::string g_feedback_text;

// Guest-thread-only tracking for visual post-process toggles
bool g_guest_psychedelic_applied = false;
uint32_t g_last_postfx_va = 0u;
bool g_guest_movie_applied = false;

void SetFeedbackLocked(const std::string& msg, bool is_warning = false) {
    ++g_feedback_seq;
    g_feedback_is_warning = is_warning;
    g_feedback_text = msg;
}

void RecordFeedback(const std::string& msg, bool is_warning = false) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    SetFeedbackLocked(msg, is_warning);
}

bool ValidGuest(uint32_t va, uint32_t bytes = 4u) {
    if (kernel::g_base == nullptr || bytes == 0u || va < 0x10000u) return false;
    const uint32_t last = va + (bytes - 1u);
    if (last < va) return false;
    return mem::IsCommitted(va) && (bytes == 1u || mem::IsCommitted(last));
}

uint32_t FloatBits(float f) {
    uint32_t u = 0u;
    std::memcpy(&u, &f, sizeof(u));
    return u;
}

struct SavedX87State {
    int top = 0;
    double st[8]{};

    static SavedX87State Capture(const X86Context& ctx) {
        SavedX87State s{};
        s.top = ctx.top;
        std::memcpy(s.st, ctx.st, sizeof(s.st));
        return s;
    }

    void Restore(X86Context& ctx) const {
        ctx.top = top;
        std::memcpy(ctx.st, st, sizeof(st));
    }
};

uint32_t CallThiscall(X86Context& ctx, uint32_t fn_va, uint32_t this_ptr,
                      std::initializer_list<uint32_t> args = {}) {
    if (!ValidGuest(fn_va, 1u) || !ValidGuest(this_ptr, 4u)) return 0u;
    const uint32_t saved_ecx = ctx.ecx;
    const SavedX87State saved_fpu = SavedX87State::Capture(ctx);
    ctx.ecx = this_ptr;
    const uint32_t res = kernel::CallGuest(ctx, fn_va, args);
    ctx.ecx = saved_ecx;
    saved_fpu.Restore(ctx);
    return res;
}

float CallThiscallFloat(X86Context& ctx, uint32_t fn_va, uint32_t this_ptr,
                        std::initializer_list<uint32_t> args = {}) {
    if (!ValidGuest(fn_va, 1u) || !ValidGuest(this_ptr, 4u)) return 0.0f;
    const uint32_t saved_ecx = ctx.ecx;
    const SavedX87State saved_fpu = SavedX87State::Capture(ctx);
    ctx.ecx = this_ptr;
    kernel::CallGuest(ctx, fn_va, args);
    const float val = float(X87_ST(0));
    ctx.ecx = saved_ecx;
    saved_fpu.Restore(ctx);
    return std::isfinite(val) ? val : 0.0f;
}

uint32_t CallGuestFreeFn(X86Context& ctx, uint32_t fn_va, std::initializer_list<uint32_t> args = {}) {
    if (!ValidGuest(fn_va, 1u)) return 0u;
    const SavedX87State saved_fpu = SavedX87State::Capture(ctx);
    const uint32_t res = kernel::CallGuest(ctx, fn_va, args);
    saved_fpu.Restore(ctx);
    return res;
}

uint32_t ResolveWorldHandle(uint8_t* base, uint16_t handle) {
    if (handle == 0u || !ValidGuest(kVaWorldPtr, 4u)) return 0u;
    const uint32_t world_va = X86_MEM32(kVaWorldPtr);
    if (!ValidGuest(world_va + 0x0Cu, 12u)) return 0u;
    const uint32_t tbl_va = X86_MEM32(world_va + 0x0Cu);
    const int32_t tbl_cap = int32_t(X86_MEM32(world_va + 0x14u));
    if (tbl_cap <= 0 || tbl_cap > 65536 || int32_t(handle) >= tbl_cap) return 0u;
    if (!ValidGuest(tbl_va, uint32_t(tbl_cap) * 6u)) return 0u;
    const uint32_t obj_va = X86_MEM32(tbl_va + uint32_t(handle) * 6u);
    if (!ValidGuest(obj_va, 0xF0u)) return 0u;
    if ((X86_MEM32(obj_va + 0x08u) & 0x80000000u) != 0u) return 0u;
    return obj_va;
}

uint32_t LookupTemplate(X86Context& ctx, uint8_t* base, uint32_t tpl_idx) {
    if (tpl_idx >= 0xFFFFu || !ValidGuest(kVaTemplateMgrPtr, 4u)) return 0u;
    const uint32_t tpl_mgr = X86_MEM32(kVaTemplateMgrPtr);
    if (!ValidGuest(tpl_mgr, 0x20u)) return 0u;
    const int32_t max_tpl = int32_t(X86_MEM32(tpl_mgr + 0x18u));
    if (max_tpl <= 0 || int32_t(tpl_idx) >= max_tpl) return 0u;
    const uint32_t tpl_va = CallThiscall(ctx, kFnGetTemplate, tpl_mgr, {tpl_idx});
    return ValidGuest(tpl_va, 0x20u) ? tpl_va : 0u;
}

bool HasClassHash(X86Context& ctx, uint8_t* base, uint32_t obj_va, uint32_t expected_hash) {
    if (!ValidGuest(obj_va, 0xF0u)) return false;
    if ((X86_MEM32(obj_va + 0x08u) & 0x80000000u) != 0u) return false;
    const uint16_t tpl_idx = X86_MEM16(obj_va + 0xE4u);
    if (tpl_idx == 0xFFFFu) return false;
    const uint32_t tpl_va = LookupTemplate(ctx, base, uint32_t(tpl_idx));
    if (tpl_va == 0u) return false;
    return X86_MEM32(tpl_va + 0x0Cu) == expected_hash;
}

uint32_t ReadPropValuePtr(X86Context& ctx, uint8_t* base, uint32_t prop_bag_va, uint32_t prop_id) {
    if (!ValidGuest(prop_bag_va, 4u)) return 0u;
    const uint32_t vt = X86_MEM32(prop_bag_va);
    if (!ValidGuest(vt + 0x08u, 4u)) return 0u;
    const uint32_t fn = X86_MEM32(vt + 0x08u);
    const uint32_t val_va = CallThiscall(ctx, fn, prop_bag_va, {prop_id, 0xFFFFFFFFu});
    return ValidGuest(val_va, 4u) ? val_va : 0u;
}

float ReadObjectPropF32(X86Context& ctx, uint8_t* base, uint32_t obj_va, uint32_t prop_id, float fallback) {
    if (!ValidGuest(obj_va + 0xECu, 4u)) return fallback;
    const uint32_t bag = X86_MEM32(obj_va + 0xECu);
    const uint32_t val_va = ReadPropValuePtr(ctx, base, bag, prop_id);
    if (val_va == 0u) return fallback;
    const float v = X86_MEMF32(val_va);
    return std::isfinite(v) ? v : fallback;
}

int32_t ReadObjectPropI32(X86Context& ctx, uint8_t* base, uint32_t obj_va, uint32_t prop_id, int32_t fallback) {
    if (!ValidGuest(obj_va + 0xECu, 4u)) return fallback;
    const uint32_t bag = X86_MEM32(obj_va + 0xECu);
    const uint32_t val_va = ReadPropValuePtr(ctx, base, bag, prop_id);
    if (val_va == 0u) return fallback;
    return int32_t(X86_MEM32(val_va));
}

bool IsDamageableAlive(uint8_t* base, uint32_t dmg_va) {
    if (!ValidGuest(dmg_va, 0x24u)) return false;
    if (X86_MEM32(dmg_va + 0x0Eu) != 0u || X86_MEM32(dmg_va + 0x1Eu) != 0u) return true;
    const float hp = X86_MEMF32(dmg_va + 0x16u);
    return std::isfinite(hp) && hp > 0.0f;
}

void LocateActiveTorque(X86Context& ctx, uint8_t* base, bool& out_in_level, bool& out_alive,
                        uint32_t& out_player_obj, uint32_t& out_torque) {
    out_in_level = false;
    out_alive = false;
    out_player_obj = 0u;
    out_torque = 0u;

    if (!ValidGuest(kVaGameSessionState, 4u) || !ValidGuest(kVaEngineModeFlags, 4u) ||
        !ValidGuest(kVaLevelFlags, 4u) || !ValidGuest(kVaLoadingScreenPtr, 4u) || !ValidGuest(kVaWorldPtr, 4u)) {
        return;
    }

    const uint32_t session = X86_MEM32(kVaGameSessionState);
    const uint32_t mode = X86_MEM32(kVaEngineModeFlags);
    const uint32_t lvl_flags = X86_MEM32(kVaLevelFlags);
    const uint32_t loading_ptr = X86_MEM32(kVaLoadingScreenPtr);

    // Note: Pause / Archives / Save-Load menus clear bit 2 of kVaEngineModeFlags (mode: 7 -> 1) while
    // keeping session == 2 and (lvl_flags & 2) != 0. Only reject FMV playback ((mode & 0x08) != 0) so
    // players can inspect live stats and trigger loadout cheats both mid-combat and while paused!
    if (session != 2u || (mode & 0x08u) != 0u || (lvl_flags & 0x02u) == 0u || loading_ptr != 0u) {
        return;
    }
    out_in_level = true;

    const uint32_t world_va = X86_MEM32(kVaWorldPtr);
    if (!ValidGuest(world_va + 0x124u, 12u)) return;
    const int32_t player_cnt = int32_t(X86_MEM32(world_va + 0x12Cu));
    const uint32_t player_arr = X86_MEM32(world_va + 0x124u);
    if (player_cnt <= 0 || !ValidGuest(player_arr, 4u)) return;

    const uint32_t player_obj = X86_MEM32(player_arr);
    if (!HasClassHash(ctx, base, player_obj, kHashPlayer)) return;

    const uint32_t torque_va = X86_MEM32(player_obj + 0x60u);
    if (!ValidGuest(torque_va, 0x790u)) return;
    if (X86_MEM32(torque_va) != kVtTorquePrimary) return;

    out_player_obj = player_obj;
    out_torque = torque_va;
    out_alive = IsDamageableAlive(base, torque_va);
}

int32_t ResolveXombiumInvSlot(X86Context& ctx, uint8_t* base, uint32_t player_obj, uint32_t torque_va) {
    const int32_t xombium_ref = ReadObjectPropI32(ctx, base, player_obj, 0x77u, -1);
    if (xombium_ref < 0) return -1;
    if (!ValidGuest(player_obj + 0xE8u, 4u)) return -1;
    const uint32_t lvl_sub = X86_MEM32(player_obj + 0xE8u);
    if (!ValidGuest(lvl_sub, 0x10u)) return -1;
    const uint32_t tpl_obj = CallThiscall(ctx, kFnGetLevelTemplateObj, lvl_sub, {uint32_t(xombium_ref)});
    if (!ValidGuest(tpl_obj + 0xE4u, 2u)) return -1;
    const uint16_t tpl_idx = X86_MEM16(tpl_obj + 0xE4u);
    if (tpl_idx == 0xFFFFu) return -1;
    return int32_t(CallThiscall(ctx, kFnResolveInvTplIdx, torque_va, {uint32_t(tpl_idx)}));
}

int32_t GetXombiumCount(X86Context& ctx, uint8_t* base, uint32_t player_obj, uint32_t torque_va) {
    const int32_t slot = ResolveXombiumInvSlot(ctx, base, player_obj, torque_va);
    if (slot < 0) return 0;
    const int32_t cnt = int32_t(CallThiscall(ctx, kFnInvGetCount, torque_va + 0x108u, {uint32_t(slot)}));
    return std::max(cnt, 0);
}

void TopUpWeaponTemplateReserveAmmo(X86Context& ctx, uint8_t* base, uint32_t torque_va, int32_t weap_tpl_idx,
                                    bool& any_added) {
    if (weap_tpl_idx < 0) return;
    const uint32_t weap_tpl = LookupTemplate(ctx, base, uint32_t(weap_tpl_idx));
    if (weap_tpl == 0u || X86_MEM32(weap_tpl + 0x0Cu) != kHashRangedWeapon) return;

    // Ensure template instance data is initialized (matches sub_00048BE0 @ 0x00048C21)
    if (int32_t(X86_MEM32(weap_tpl + 0x1Cu)) >= 0) {
        const uint32_t vt = X86_MEM32(weap_tpl);
        if (ValidGuest(vt, 4u)) {
            CallThiscall(ctx, X86_MEM32(vt), weap_tpl, {});
        }
    }

    const uint32_t weap_bag = X86_MEM32(weap_tpl + 0x04u);
    const uint32_t ammo_idx_va = ReadPropValuePtr(ctx, base, weap_bag, 0x44u);
    if (ammo_idx_va == 0u) return;
    const int32_t ammo_tpl_idx = int32_t(X86_MEM32(ammo_idx_va));
    if (ammo_tpl_idx < 0) return;

    const uint32_t ammo_tpl = LookupTemplate(ctx, base, uint32_t(ammo_tpl_idx));
    if (ammo_tpl == 0u) return;
    if (int32_t(X86_MEM32(ammo_tpl + 0x1Cu)) >= 0) {
        const uint32_t vt = X86_MEM32(ammo_tpl);
        if (ValidGuest(vt, 4u)) {
            CallThiscall(ctx, X86_MEM32(vt), ammo_tpl, {});
        }
    }
    if (X86_MEM32(ammo_tpl + 0x08u) != 0x0Au) return;  // Must be an inventory item template

    const uint32_t ammo_bag = X86_MEM32(ammo_tpl + 0x04u);
    const uint32_t max_cap_va = ReadPropValuePtr(ctx, base, ammo_bag, 0x2Cu);
    const int32_t max_cap = (max_cap_va != 0u) ? int32_t(X86_MEM32(max_cap_va)) : 0;
    if (max_cap <= 0) return;

    const int32_t cur = int32_t(CallThiscall(ctx, kFnInvGetCount, torque_va + 0x108u, {uint32_t(ammo_tpl_idx)}));
    if (cur < max_cap) {
        CallThiscall(ctx, kFnInvAddItem, torque_va + 0x108u, {uint32_t(ammo_tpl_idx), uint32_t(max_cap)});
        any_added = true;
    }
}

void TopUpAllOwnedFirearmsAmmo(X86Context& ctx, uint8_t* base, uint32_t torque_va, bool refill_clips) {
    bool any_added = false;

    // 1. Active weapon
    const uint32_t active_obj = ResolveWorldHandle(base, X86_MEM16(torque_va + 0x3C6u));
    if (active_obj != 0u) {
        const uint16_t tpl_idx = X86_MEM16(active_obj + 0xE4u);
        if (tpl_idx != 0xFFFFu) {
            TopUpWeaponTemplateReserveAmmo(ctx, base, torque_va, int32_t(tpl_idx), any_added);
        }
    }

    // 2. Owned weapon slots ([torque+0x658], stride 0x0C, count [torque+0x660])
    const int32_t slot_cnt = int32_t(X86_MEM32(torque_va + 0x660u));
    const uint32_t slot_arr = X86_MEM32(torque_va + 0x658u);
    if (slot_cnt > 0 && slot_cnt <= 64 && ValidGuest(slot_arr, uint32_t(slot_cnt) * 0x0Cu)) {
        for (int32_t i = 0; i < slot_cnt; ++i) {
            const uint32_t entry_va = slot_arr + uint32_t(i) * 0x0Cu;
            const int32_t weap_tpl_idx = int32_t(X86_MEM32(entry_va + 0x04u));
            TopUpWeaponTemplateReserveAmmo(ctx, base, torque_va, weap_tpl_idx, any_added);

            if (refill_clips && weap_tpl_idx >= 0) {
                const uint32_t weap_tpl = LookupTemplate(ctx, base, uint32_t(weap_tpl_idx));
                if (weap_tpl != 0u && X86_MEM32(weap_tpl + 0x0Cu) == kHashRangedWeapon) {
                    const uint32_t bag = X86_MEM32(weap_tpl + 0x04u);
                    const uint32_t clip_va = ReadPropValuePtr(ctx, base, bag, 0x45u);
                    if (clip_va != 0u) {
                        const int32_t single_clip = int32_t(X86_MEM32(clip_va));
                        if (single_clip > 0 && int32_t(X86_MEM32(entry_va + 0x08u)) < single_clip) {
                            X86_MEM32(entry_va + 0x08u) = uint32_t(single_clip);
                            any_added = true;
                        }
                    }
                }
            }
        }
    }

    if (refill_clips) {
        const int32_t max_clip = int32_t(CallThiscall(ctx, kFnGetActiveMaxClip, torque_va));
        if (max_clip > 0 && int32_t(X86_MEM32(torque_va + 0x114u)) < max_clip) {
            X86_MEM32(torque_va + 0x114u) = uint32_t(max_clip);
            CallThiscall(ctx, kFnSyncClipToSlot, torque_va);
            any_added = true;
        }
    }

    if (any_added) {
        CallThiscall(ctx, kFnUpdateAmmoXombiumHud, torque_va);
    }
}

float ReadAndMaintainFlashlightPct(X86Context& ctx, uint8_t* base, uint32_t torque_va, bool infinite_battery) {
    const uint16_t fl_handle = X86_MEM16(torque_va + 0x134u);
    const uint32_t fl_obj = ResolveWorldHandle(base, fl_handle);
    if (fl_obj == 0u) return 100.0f;

    const uint32_t fl_va = CallGuestFreeFn(ctx, kFnCastToFlashlight, {fl_obj});
    if (!ValidGuest(fl_va, 0xB0u)) return 100.0f;

    const float max_full = std::max(ReadObjectPropF32(ctx, base, fl_obj, 0x43u, 60.0f), 0.001f);
    const float max_dim = std::max(ReadObjectPropF32(ctx, base, fl_obj, 0x44u, 15.0f), 0.0f);

    if (infinite_battery) {
        const uint32_t flags = X86_MEM32(fl_va + 0x9Cu);
        if ((flags & 0x06u) != 0u) {
            // Restore spotlight actor intensity if battery had already dimmed or died
            CallThiscall(ctx, kFnRechargeFlashlight, fl_va);
        }
        const float cur_full = X86_MEMF32(fl_va + 0xA0u);
        const float cur_dim = X86_MEMF32(fl_va + 0xA4u);
        if (!std::isfinite(cur_full) || cur_full < max_full || !std::isfinite(cur_dim) || cur_dim < max_dim) {
            X86_MEMF32(fl_va + 0xA0u) = max_full;
            X86_MEMF32(fl_va + 0xA4u) = max_dim;
            X86_MEM32(fl_va + 0x9Cu) = X86_MEM32(fl_va + 0x9Cu) & ~0x06u;
            if ((X86_MEM32(fl_va + 0x9Cu) & 0x01u) != 0u) {
                CallThiscall(ctx, kFnUpdateFlashlightHud, torque_va);
            }
        }
        return 100.0f;
    }

    const uint32_t flags = X86_MEM32(fl_va + 0x9Cu);
    if ((flags & 0x04u) != 0u) return 0.0f;
    const float cur_full = std::clamp(X86_MEMF32(fl_va + 0xA0u), 0.0f, max_full);
    const float cur_dim = std::clamp(X86_MEMF32(fl_va + 0xA4u), 0.0f, max_dim);
    const float total = max_full + max_dim;
    return (total > 0.0f) ? std::clamp(((cur_full + cur_dim) / total) * 100.0f, 0.0f, 100.0f) : 100.0f;
}

void SyncVisualTogglesOnGuest(X86Context& ctx, uint8_t* base, bool in_level) {
    const bool want_psych = in_level && GetToggle(CheatToggle::PsychedelicMode);
    if (ValidGuest(kVaRendererPtr, 4u)) {
        const uint32_t rend_va = X86_MEM32(kVaRendererPtr);
        if (ValidGuest(rend_va + 0x18u, 4u)) {
            const uint32_t postfx_va = X86_MEM32(rend_va + 0x18u);
            if (ValidGuest(postfx_va, 4u)) {
                const uint32_t vt = X86_MEM32(postfx_va);
                if (ValidGuest(vt + 0x28u, 4u)) {
                    if (want_psych) {
                        if (!g_guest_psychedelic_applied || g_last_postfx_va != postfx_va) {
                            CallGuestFreeFn(ctx, kFnSetPsychedelic, {1u});
                            g_guest_psychedelic_applied = true;
                            g_last_postfx_va = postfx_va;
                        }
                    } else if (g_guest_psychedelic_applied) {
                        CallGuestFreeFn(ctx, kFnSetPsychedelic, {0u});
                        g_guest_psychedelic_applied = false;
                        g_last_postfx_va = postfx_va;
                    }
                }
            }
        }
    }

    const bool want_movie = in_level && GetToggle(CheatToggle::OldMovieMode);
    if (ValidGuest(kVaProjectorEffect + 0x0Cu, 4u)) {
        if (want_movie) {
            if (X86_MEM32(kVaProjectorEffect + 0x0Cu) == 0u) {
                X86_MEM32(kVaProjectorEffect + 0x0Cu) = 1u;
            }
            g_guest_movie_applied = true;
        } else if (g_guest_movie_applied) {
            X86_MEM32(kVaProjectorEffect + 0x0Cu) = 0u;
            g_guest_movie_applied = false;
        }
    }
}

void ClampAndRefreshMorality(X86Context& ctx, uint8_t* base, uint32_t player_obj, uint32_t torque_va) {
    float lo = ReadObjectPropF32(ctx, base, player_obj, 0x46u, -100.0f);
    float hi = ReadObjectPropF32(ctx, base, player_obj, 0x47u, 100.0f);
    if (lo > hi) std::swap(lo, hi);
    const float cur = X86_MEMF32(torque_va + 0x454u);
    const float clamped = std::isfinite(cur) ? std::clamp(cur, lo, hi) : 0.0f;
    X86_MEMF32(torque_va + 0x454u) = clamped;
    CallThiscall(ctx, kFnRefreshReputation, torque_va + 0x3Eu);
}

void ExecuteOneShotActionOnGuest(X86Context& ctx, uint8_t* base, CheatAction action,
                                 uint32_t player_obj, uint32_t torque_va) {
    switch (action) {
        case CheatAction::FullHealAndXombium: {
            const float max_hp = std::max(X86_MEMF32(torque_va + 0x1Au), 100.0f);
            CallThiscall(ctx, kFnSetTorqueHealth, torque_va, {FloatBits(max_hp)});
            CallThiscall(ctx, kFnGiveMaxXombium, torque_va);
            CallThiscall(ctx, kFnRefreshAllHudMeters, torque_va);
            RecordFeedback("Restored 100% Health and filled Xombium bottles.");
            break;
        }
        case CheatAction::GiveAllWeaponsAndAmmo: {
            CallThiscall(ctx, kFnExecItemWeaponCheat, torque_va, {5u});  // Native full arsenal (Player prop 0x138)
            CallThiscall(ctx, kFnExecItemWeaponCheat, torque_va, {4u});  // Native ranged ammo bundle
            CallThiscall(ctx, kFnAddActiveReserve, torque_va, {1000000u});
            TopUpAllOwnedFirearmsAmmo(ctx, base, torque_va, true);
            CallThiscall(ctx, kFnRefreshAllHudMeters, torque_va);
            RecordFeedback("Granted all standard weapons & items with full ammo.");
            break;
        }
        case CheatAction::MaxAllAmmo: {
            CallThiscall(ctx, kFnExecItemWeaponCheat, torque_va, {4u});
            CallThiscall(ctx, kFnAddActiveReserve, torque_va, {1000000u});
            TopUpAllOwnedFirearmsAmmo(ctx, base, torque_va, true);
            CallThiscall(ctx, kFnRefreshAllHudMeters, torque_va);
            RecordFeedback("Refilled clips and reserve ammo for all owned firearms.");
            break;
        }
        case CheatAction::GiveGonzoGun: {
            CallThiscall(ctx, kFnExecItemWeaponCheat, torque_va, {8u});  // Native Gonzo Gun ("Free Range")
            TopUpAllOwnedFirearmsAmmo(ctx, base, torque_va, true);
            CallThiscall(ctx, kFnRefreshAllHudMeters, torque_va);
            RecordFeedback("Granted secret Gonzo Gun (\"Free Range\").");
            break;
        }
        case CheatAction::RefillAllItems: {
            CallThiscall(ctx, kFnGiveMaxXombium, torque_va);
            for (uint32_t grp = 0u; grp < 4u; ++grp) {
                CallThiscall(ctx, kFnExecItemWeaponCheat, torque_va, {grp});  // Grenades, Molotovs, Flares, Dynamite
            }
            CallThiscall(ctx, kFnGiveFlashlightBatts, torque_va);
            ReadAndMaintainFlashlightPct(ctx, base, torque_va, true);
            CallThiscall(ctx, kFnRefreshAllHudMeters, torque_va);
            RecordFeedback("Refilled Xombium, Grenades, Molotovs, Dynamite, Flares, and Batteries.");
            break;
        }
        case CheatAction::FillInsanityMeter: {
            const float max_ins = std::max(CallThiscallFloat(ctx, kFnGetMaxInsanity, torque_va), 100.0f);
            CallThiscall(ctx, kFnSetInsanity, torque_va, {FloatBits(max_ins)});
            CallThiscall(ctx, kFnRefreshAllHudMeters, torque_va);
            RecordFeedback("Insanity meter charged to 100%.");
            break;
        }
        case CheatAction::ShiftMoralityGood: {
            CallThiscall(ctx, kFnAddMorality, torque_va, {FloatBits(50.0f)});
            ClampAndRefreshMorality(ctx, base, player_obj, torque_va);
            RecordFeedback("Shifted morality +50 toward Good.");
            break;
        }
        case CheatAction::ResetMoralityNeutral: {
            float lo = ReadObjectPropF32(ctx, base, player_obj, 0x46u, -100.0f);
            float hi = ReadObjectPropF32(ctx, base, player_obj, 0x47u, 100.0f);
            X86_MEMF32(torque_va + 0x454u) = (lo + hi) * 0.5f;
            ClampAndRefreshMorality(ctx, base, player_obj, torque_va);
            RecordFeedback("Reset morality to Neutral.");
            break;
        }
        case CheatAction::ShiftMoralityEvil: {
            CallThiscall(ctx, kFnAddMorality, torque_va, {FloatBits(-50.0f)});
            ClampAndRefreshMorality(ctx, base, player_obj, torque_va);
            RecordFeedback("Shifted morality -50 toward Evil.");
            break;
        }
        case CheatAction::SetMaxMoralityGood: {
            CallThiscall(ctx, kFnSetMaxGoodMorality, torque_va);
            ClampAndRefreshMorality(ctx, base, player_obj, torque_va);
            RecordFeedback("Morality set to 100% Good.");
            break;
        }
        case CheatAction::SetMaxMoralityEvil: {
            CallThiscall(ctx, kFnSetMaxEvilMorality, torque_va);
            ClampAndRefreshMorality(ctx, base, player_obj, torque_va);
            RecordFeedback("Morality set to 100% Evil.");
            break;
        }
        case CheatAction::BloodsoakedTorque: {
            CallThiscall(ctx, kFnMakeBloodsoaked, torque_va);
            RecordFeedback("Applied Bloodsoaked Torque effect.");
            break;
        }
        case CheatAction::CleanTorque: {
            X86_MEMF32(torque_va + 0x42u) = 0.0f;
            RecordFeedback("Cleaned all blood off Torque.");
            break;
        }
        case CheatAction::Count:
            break;
    }
}

uint32_t CountRawArmedToggles() {
    uint32_t n = 0u;
    for (const auto& flag : g_toggles) {
        if (flag.load(std::memory_order_relaxed)) ++n;
    }
    return n;
}

}  // namespace

const std::array<CheatToggleInfo, kCheatToggleCount>& GetToggleInfos() { return kToggleInfos; }

const std::array<CheatActionInfo, kCheatActionCount>& GetActionInfos() { return kActionInfos; }

const char* MoralityTierName(MoralityTier tier) {
    switch (tier) {
        case MoralityTier::Evil: return "Evil";
        case MoralityTier::Neutral: return "Neutral";
        case MoralityTier::Good: return "Good";
    }
    return "Neutral";
}

void SetToggle(CheatToggle toggle, bool enabled) {
    const size_t idx = static_cast<size_t>(toggle);
    if (idx >= kCheatToggleCount) return;
    g_toggles[idx].store(enabled, std::memory_order_release);
}

bool GetToggle(CheatToggle toggle) {
    const size_t idx = static_cast<size_t>(toggle);
    if (idx >= kCheatToggleCount) return false;
    return g_toggles[idx].load(std::memory_order_acquire);
}

bool IsToggleEffective(CheatToggle toggle) {
    if (app::IsReplayOrAutotestActive()) return false;
    return GetToggle(toggle);
}

bool ToggleCheat(CheatToggle toggle) {
    const size_t idx = static_cast<size_t>(toggle);
    if (idx >= kCheatToggleCount) return false;
    bool prev = g_toggles[idx].load(std::memory_order_relaxed);
    while (!g_toggles[idx].compare_exchange_weak(prev, !prev, std::memory_order_acq_rel, std::memory_order_relaxed)) {
    }
    return !prev;
}

void DisableAllCheats() {
    for (auto& flag : g_toggles) {
        flag.store(false, std::memory_order_release);
    }
    RecordFeedback("All persistent cheats disabled.");
}

uint32_t GetActiveToggleCount() {
    if (app::IsReplayOrAutotestActive()) return 0u;
    return CountRawArmedToggles();
}

void EnqueueAction(CheatAction action) {
    if (static_cast<size_t>(action) >= kCheatActionCount) return;
    if (app::IsReplayOrAutotestActive()) return;
    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_pending_actions.size() < 32u) {
        g_pending_actions.push_back(action);
    }
}

void ClearActionFeedback() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    g_feedback_text.clear();
    g_feedback_is_warning = false;
}

CheatRuntimeStatus GetRuntimeStatus() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    CheatRuntimeStatus snap = g_runtime_status;
    snap.replay_active = app::IsReplayOrAutotestActive();
    snap.active_toggle_count = snap.replay_active ? 0u : CountRawArmedToggles();
    snap.last_action_seq = g_feedback_seq;
    snap.last_action_is_warning = g_feedback_is_warning;
    snap.last_action_feedback = g_feedback_text;
    return snap;
}

}  // namespace suffering::cheats

// ============================================================================
// Guest Hooks (executed on Guest Main Thread 0x14; 100% bypassed during replay)
// ============================================================================

// 1. GameApp::OnFrameTick (__thiscall, ret 0) — runs once per frame on Guest Main Thread 0x14
// across Title/Main Menu, Pause Menu, Archives, and Active 3D Gameplay.
X86_FUNC(sub_000BF6D0) {
    __imp__sub_000BF6D0(ctx, base);

    using namespace suffering::cheats;

    if (app::IsReplayOrAutotestActive()) {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_pending_actions.clear();
        g_runtime_status = CheatRuntimeStatus{};
        g_runtime_status.replay_active = true;
        return;
    }

    bool in_level = false;
    bool player_alive = false;
    uint32_t player_obj = 0u;
    uint32_t torque_va = 0u;
    LocateActiveTorque(ctx, base, in_level, player_alive, player_obj, torque_va);

    // Sync persistent visual filters (auto-clears on Main Menu, auto-applies in active levels)
    SyncVisualTogglesOnGuest(ctx, base, in_level);

    // Drain any queued one-shot UI actions on Guest Main Thread 0x14
    std::vector<CheatAction> actions_to_run;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        if (!g_pending_actions.empty()) {
            actions_to_run.swap(g_pending_actions);
        }
    }

    if (!actions_to_run.empty()) {
        if (in_level && player_alive && player_obj != 0u && torque_va != 0u) {
            for (CheatAction act : actions_to_run) {
                ExecuteOneShotActionOnGuest(ctx, base, act, player_obj, torque_va);
            }
        } else if (!in_level) {
            RecordFeedback("Load into a level first to use instant loadout/player actions.", true);
        } else {
            RecordFeedback("Cannot apply instant player action while Torque is dead.", true);
        }
    }

    CheatRuntimeStatus next{};
    next.replay_active = false;
    next.in_level = in_level;
    next.player_alive = player_alive;

    if (in_level && torque_va != 0u && player_obj != 0u) {
        if (player_alive) {
            // Enforce persistent gameplay toggles every frame (including while paused)
            if (GetToggle(CheatToggle::GodMode)) {
                const float cur_hp = X86_MEMF32(torque_va + 0x16u);
                const float max_hp = std::max(X86_MEMF32(torque_va + 0x1Au), 100.0f);
                if (!std::isfinite(cur_hp) || cur_hp < max_hp) {
                    CallThiscall(ctx, kFnSetTorqueHealth, torque_va, {FloatBits(max_hp)});
                }
            }

            if (GetToggle(CheatToggle::BottomlessClip)) {
                const int32_t max_clip = int32_t(CallThiscall(ctx, kFnGetActiveMaxClip, torque_va));
                if (max_clip > 0 && int32_t(X86_MEM32(torque_va + 0x114u)) < max_clip) {
                    X86_MEM32(torque_va + 0x114u) = uint32_t(max_clip);
                    CallThiscall(ctx, kFnSyncClipToSlot, torque_va);
                    CallThiscall(ctx, kFnUpdateAmmoXombiumHud, torque_va);
                }
            }

            if (GetToggle(CheatToggle::InfiniteReserveAmmo)) {
                TopUpAllOwnedFirearmsAmmo(ctx, base, torque_va, false);
            }

            if (GetToggle(CheatToggle::InfiniteInsanity)) {
                // Skip refilling only during the voluntary revert-to-human animation state (bit 0x21 -> [+0xA8] & 2)
                const bool reverting_to_human = (X86_MEM32(torque_va + 0xA8u) & 0x02u) != 0u;
                if (!reverting_to_human) {
                    const float max_ins = std::max(CallThiscallFloat(ctx, kFnGetMaxInsanity, torque_va), 100.0f);
                    const float cur_ins = X86_MEMF32(torque_va + 0x458u);
                    if (!std::isfinite(cur_ins) || cur_ins < max_ins) {
                        CallThiscall(ctx, kFnSetInsanity, torque_va, {FloatBits(max_ins)});
                    }
                }
            }
        }

        // Populate live snapshot for UI
        next.is_transformed = (X86_MEM32(torque_va + 0xA4u) & 0x100000u) != 0u;
        next.max_health = std::max(X86_MEMF32(torque_va + 0x1Au), 1.0f);
        const float raw_hp = X86_MEMF32(torque_va + 0x16u);
        next.health = std::isfinite(raw_hp) ? std::clamp(raw_hp, 0.0f, next.max_health) : 0.0f;

        next.clip_ammo = std::max(int32_t(X86_MEM32(torque_va + 0x114u)), 0);
        next.max_clip_ammo = std::max(int32_t(CallThiscall(ctx, kFnGetActiveMaxClip, torque_va)), 0);
        next.reserve_ammo = std::max(int32_t(CallThiscall(ctx, kFnGetActiveReserve, torque_va)), 0);
        next.xombium_count = GetXombiumCount(ctx, base, player_obj, torque_va);
        next.max_xombium = 9;

        next.max_insanity = std::max(CallThiscallFloat(ctx, kFnGetMaxInsanity, torque_va), 1.0f);
        const float raw_ins = X86_MEMF32(torque_va + 0x458u);
        next.insanity = std::isfinite(raw_ins) ? std::clamp(raw_ins, 0.0f, next.max_insanity) : 0.0f;
        next.insanity_pct = std::clamp((next.insanity / next.max_insanity) * 100.0f, 0.0f, 100.0f);

        next.flashlight_pct =
            ReadAndMaintainFlashlightPct(ctx, base, torque_va, player_alive && GetToggle(CheatToggle::InfiniteFlashlight));

        float m_min = ReadObjectPropF32(ctx, base, player_obj, 0x46u, -100.0f);
        float m_max = ReadObjectPropF32(ctx, base, player_obj, 0x47u, 100.0f);
        if (m_min > m_max) std::swap(m_min, m_max);
        next.morality_min = m_min;
        next.morality_max = m_max;
        const float raw_mor = X86_MEMF32(torque_va + 0x454u);
        next.morality_value = std::isfinite(raw_mor) ? std::clamp(raw_mor, m_min, m_max) : 0.0f;
        const float half_span = std::max((m_max - m_min) * 0.5f, 1.0f);
        const float mid = (m_min + m_max) * 0.5f;
        next.morality_normalized = std::clamp((next.morality_value - mid) / half_span, -1.0f, 1.0f);
        if (next.morality_normalized <= -0.20f) {
            next.morality_tier = MoralityTier::Evil;
        } else if (next.morality_normalized >= 0.20f) {
            next.morality_tier = MoralityTier::Good;
        } else {
            next.morality_tier = MoralityTier::Neutral;
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_runtime_status = std::move(next);
    }
}

// 2. CTorque::TakeDamage (__thiscall, ret 0x04 -> ST(0) float)
// Suppresses incoming hit/fall/explosion/fire/melee damage & hit-flinch when God Mode is enabled.
X86_FUNC(sub_0003EDC0) {
    if (suffering::cheats::IsToggleEffective(suffering::cheats::CheatToggle::GodMode)) {
        x87_push(ctx, 0.0);
        ctx.esp += 8u;  // ret 0x04
        return;
    }
    __imp__sub_0003EDC0(ctx, base);
}

// 3. CTorque::ApplyHealthLoss (__thiscall, ret 0x08 -> ST(0) float)
// Universal bottom-level player health deduction gate (also covers Insanity-overstay drain).
X86_FUNC(sub_0003EBE0) {
    if (suffering::cheats::IsToggleEffective(suffering::cheats::CheatToggle::GodMode)) {
        x87_push(ctx, 0.0);
        ctx.esp += 12u;  // ret 0x08
        return;
    }
    __imp__sub_0003EBE0(ctx, base);
}

// 4. CTorque::OnWeaponShotFired (__thiscall, ret 0x04)
// Pre-compensates the single-round [torque+0x114] decrement so clip & slot stay at max_clip with zero reload.
X86_FUNC(sub_0003A850) {
    using namespace suffering::cheats;
    if (IsToggleEffective(CheatToggle::BottomlessClip) && ValidGuest(ctx.ecx, 0x790u) &&
        X86_MEM32(ctx.ecx) == kVtTorquePrimary) {
        const int32_t max_clip = int32_t(CallThiscall(ctx, kFnGetActiveMaxClip, ctx.ecx));
        if (max_clip > 0) {
            X86_MEM32(ctx.ecx + 0x114u) = uint32_t(max_clip + 1);
        }
    }
    __imp__sub_0003A850(ctx, base);
}

// 5. CPlayerInventory::Consume (__thiscall, ret 0x08: int32 item_tpl_idx, int32 amount)
// Surgically blocks item/ammo/battery depletion at the exact player consumption call-sites while leaving
// Flamethrower part crafting (0x0004AFA7) and level-script quest item filters (0x000276xx / 0x0013F79F) untouched.
X86_FUNC(sub_0005CE80) {
    using namespace suffering::cheats;
    if (!app::IsReplayOrAutotestActive() && ValidGuest(ctx.esp, 12u) && ValidGuest(ctx.ecx, 12u)) {
        const uint32_t ret_eip = X86_MEM32(ctx.esp);
        bool prevent_consume = false;
        if (ret_eip == kRetConsumeReserveAmmo) {
            prevent_consume = GetToggle(CheatToggle::InfiniteReserveAmmo);
        } else if (ret_eip == kRetConsumeThrowable || ret_eip == kRetConsumeXombium) {
            prevent_consume = GetToggle(CheatToggle::InfiniteItems);
        } else if (ret_eip == kRetConsumeBattery) {
            prevent_consume = GetToggle(CheatToggle::InfiniteFlashlight);
        }

        if (prevent_consume) {
            const uint32_t item_tpl_idx = X86_MEM32(ctx.esp + 0x04u);
            const uint32_t cur_cnt = CallThiscall(ctx, kFnInvGetCount, ctx.ecx, {item_tpl_idx});
            ctx.eax = (int32_t(cur_cnt) > 0) ? cur_cnt : 1u;
            ctx.esp += 12u;  // ret 0x08
            return;
        }
    }
    __imp__sub_0005CE80(ctx, base);
}

// 6. CNPC::ApplyHealthLoss (__thiscall, ret 0x08: float dmg, CGameObject* attacker_obj -> ST(0) float)
// One-Hit Kills strictly scoped to:
//  - Live CNPC targets (entity_kind == 7; never CTorque, Black One, props, doors, switches, or breakables),
//  - Explicitly excluding Dallas (#2152), environmental-puzzle bosses (Hermes/Horace/Killjoy — e.g. 8b Bluff
//    Hermes #7667..#9515 has m_bInvulnerable == 0 & 999,999 HP and softlocks if killed outside burner relay #7949),
//    plot-protected NPCs (m_bCannotDie [+0x0E] or m_bInvulnerable [+0x1E]), and sentinel-HP setpiece actors (>=5000 HP),
//  - Verified player attacker (attacker_obj == live CTorque CGameObject*, class hash 0xC8BC44E1),
//  - Verified active hostility toward Torque via Surreal's canonical AreHostile(player_obj, npc_obj, -1.0f) (sub_000F25E0).
X86_FUNC(sub_000F1E80) {
    using namespace suffering::cheats;
    if (IsToggleEffective(CheatToggle::OneHitKills) && ValidGuest(ctx.esp, 12u)) {
        const uint32_t npc_va = ctx.ecx;
        const float incoming_dmg = X86_MEMF32(ctx.esp + 0x04u);
        const uint32_t attacker_obj = X86_MEM32(ctx.esp + 0x08u);

        if (std::isfinite(incoming_dmg) && incoming_dmg > 0.0f && ValidGuest(npc_va, 0x60u)) {
            const uint32_t npc_vt = X86_MEM32(npc_va);
            const uint32_t npc_obj = X86_MEM32(npc_va + 0x04u);
            const float raw_cur_hp = X86_MEMF32(npc_va + 0x16u);
            const float raw_max_hp = X86_MEMF32(npc_va + 0x1Au);
            if (npc_vt != kVtHermes && npc_vt != kVtHorace && npc_vt != kVtKilljoy &&
                std::isfinite(raw_cur_hp) && std::isfinite(raw_max_hp) &&
                raw_max_hp > 0.0f && raw_max_hp < kSentinelBossOrSetpieceHp &&
                raw_cur_hp < kSentinelBossOrSetpieceHp &&
                ValidGuest(npc_obj, 0xF0u) && npc_obj != attacker_obj &&
                (X86_MEM32(npc_obj + 0x08u) & 0x80000000u) == 0u &&
                X86_MEM32(npc_obj + 0xE0u) == 7u &&
                X86_MEM32(npc_obj + 0xD0u) != kIidDallas &&
                X86_MEM32(npc_va + 0x0Eu) == 0u &&
                X86_MEM32(npc_va + 0x1Eu) == 0u) {
                bool in_level = false;
                bool player_alive = false;
                uint32_t player_obj = 0u;
                uint32_t torque_va = 0u;
                LocateActiveTorque(ctx, base, in_level, player_alive, player_obj, torque_va);

                if (in_level && player_obj != 0u && attacker_obj == player_obj) {
                    const uint32_t is_hostile =
                        CallGuestFreeFn(ctx, kFnAreHostile, {player_obj, npc_obj, FloatBits(-1.0f)});
                    if (is_hostile != 0u) {
                        const float cur_hp = std::max(raw_cur_hp, 1.0f);
                        const float max_hp = std::max(raw_max_hp, cur_hp);
                        const float lethal_dmg = std::max(incoming_dmg, max_hp * 4.0f + 1000.0f);
                        X86_MEMF32(ctx.esp + 0x04u) = lethal_dmg;
                    }
                }
            }
        }
    }
    __imp__sub_000F1E80(ctx, base);
}
