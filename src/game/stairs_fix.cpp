// SPDX-License-Identifier: GPL-2.0-or-later
// Surgical Staircase Top-Edge Step-Up & Quarry Staircase Anti-Clipping / Anti-Wedge Fixes
// for The Suffering (USA) default.xbe (Title ID 0x4D570015).
//
// Root causes proven from XBE disassembly & 4a/4b Quarry_level.mdu collision geometry:
//  1. Unwired CCollider::m_flags bit 0x08 (STEP_UP_ENABLED) & missing 0x8000 on form re-init:
//     - CTorque::Init (0x00041F80) sets [col] |= 0x8000, while CTorque::ReinitCollider (0x00054EF4)
//       omits 0x8000, and neither sets bit 0x08 (STEP_UP_ENABLED).
//     - With bit 0x08 == 0, IsWalkableOrStepTriangle (sub_00179D60 @ 0x00179D7C) rejects all
//       0.40 < ny <= 0.76604 (40..66.4 deg, height < 400) staircase ramps (such as 4a Quarry sub=401 /
//       4b Quarry sub=27 T30/T31 ny=0.741..0.750, sub=142 ny=0.747, sub=681 ny=0.749..0.753), and
//       ShouldSkipStepTriangle (sub_001709C0 @ 0x001709C3) always returns 0.
//     - Worse, in GatherObjectMeshTriangles (sub_00173730 @ 0x001740E9..0x001740F4), when [esp+0x1C]==0
//       and num_spheres > 1, Torque's foot sphere skips those 40.5..45 deg Quarry stair ramps WITHOUT
//       adding them to step_candidate_tris (sub_00171F70), causing Torque's feet/legs to clip straight
//       through Quarry staircases until his upper-body sphere wedges!
//  2. Unchecked post-collision slide addition & coarse spatial substep at 60+ FPS in CCollider::SweepStep
//     (sub_00174340 @ 0x00174346..0x0017436E & 0x0017447F -> 0x001744B4):
//     - substep_size = min(40.0f, max_radius / 30.0f) ([col+0x40]) is fixed in world space while
//       desired_dist = speed * dt shrinks at 60/120/240 FPS, collapsing num_substeps to 1..2 and
//       max_iterations to 2..3.
//     - After colliding at substep >= 1 and deflecting remaining_delta onto an UNTESTED slide vector,
//       0x0017447F (jnp 0x001744B4) jumps to the no-collision completion block whenever
//       |remaining_delta|^2 < max(substep_size^2, 0.01 * desired_dist^2), blindly adding the unswept
//       post-collision vector into collision_correction and pushing Torque into staircase creases/walls.
//  3. Staircase top-edge (crest / lip) horizontal block under ON_GROUND:
//     - ShouldSkipStepTriangle (sub_001709C0 @ 0x00170A2B) unconditionally returns 0 (never skips)
//       whenever tri.normal.y >= 0.76604f, which includes every flat horizontal top-landing triangle!
//     - Meanwhile, when ON_GROUND ([torque+0x98] & 1 -> [col] & 0x10), sub_00170D20 (0x00170DDA) zeroes
//       desired_delta.y and sub_00170700 (0x001707C6) zeroes slide.y. When Torque's foot sphere rim hits
//       the horizontal front edge of a top landing of height h (0 < h <= step_height) at horizontal
//       distance sqrt(R^2 - (ly - cy)^2) ahead of Torque's (x, z) center, ComputeSlide (sub_001712C0)
//       destroys the upward slide component and hard-stops horizontal motion before (x, z) enters the
//       top landing triangle for GroundSnap (sub_00172C00) — until jumping clears ON_GROUND.
//  4. Unwelded Quarry staircase seams (sub_0017A400) + TryLandOnMeshGround (sub_0003CE90 @ 0x0003CF00)
//     refusing to land on ny <= 0.76604f stair slopes -> permanent State 9 (Fall) lock (Jump disabled):
//     - Quarry stair meshes have unwelded top/intermediate landings (0 shared vertex indices in sub=401
//       T30/T31 <-> T20/T21 <-> T43/T44). Crossing an unwelded seam above .wdu terrain drops
//       CGroundSupport in sub_0003D760 (0x0003D85A) and enters State 9 (Fall, ON_GROUND=0, Jump blocked),
//       while sub_0003CE90 rejects landing when supportNormal.y <= 0.76604f even when bStepUpSlope is set.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "app/app.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

// Hooked guest functions (verified 0 overlap with src/game/hooks.cpp and src/game/cheats.cpp)
X86_EXTERN_FUNC(__imp__sub_0005BF60);
X86_EXTERN_FUNC(__imp__sub_0003CE90);
X86_EXTERN_FUNC(__imp__sub_00174530);
X86_EXTERN_FUNC(__imp__sub_00174340);

namespace {

// Verified guest globals & vtables (The Suffering USA default.xbe, Title ID 0x4D570015)
constexpr uint32_t kVaWorldPtr = 0x002D0DC0u;
constexpr uint32_t kVaGlobalDt = 0x002E01D0u;
constexpr uint32_t kVtTorquePrimary = 0x00299FC8u;

// Verified guest helper functions (called exclusively on the Guest Main Thread)
constexpr uint32_t kFnLandOnGround = 0x0004B640u;          // __thiscall(CTorque*)
constexpr uint32_t kFnQueryHeightmapFloor = 0x0005CC10u;   // __thiscall(CTorque*)
constexpr uint32_t kFnGetMutableTransform = 0x00155EC0u;   // __thiscall(CGameObject*) -> CMatrix4x3* (pos at +0x30)
constexpr uint32_t kFnInvalidateSkeleton = 0x0016B520u;    // __thiscall(CSkeletonInstance*)
constexpr uint32_t kFnWriteFloorHitInfo = 0x001707E0u;     // __thiscall(CCollider*, STriEntry*) [ret 0x04]
constexpr uint32_t kFnSweepSubstepsAndSlide = 0x00172B90u; // __thiscall(CCollider*) -> int32 safe_steps_or_neg1
constexpr uint32_t kFnClearGroundSupport = 0x001798E0u;    // __thiscall(CGroundSupport*)
constexpr uint32_t kFnSetGroundFromHitInfo = 0x0017A2B0u;  // __thiscall(CGroundSupport*, SFloorHitInfo*) [ret 0x04]

// Walkability & staircase geometry constants (world units; 2048.0f units == 1 .wdu grid cell)
constexpr float kMinWalkableNormalY = 0.76604f;  // cos(40 deg), [0x002CB858]
constexpr float kMinStairRampNormalY = 0.62f;    // cos(51.7 deg), covers 40..51 deg Quarry stair ramps
constexpr float kMaxStairTriSpanY = 400.0f;      // [0x002A451C] native step-slope max triangle height
constexpr float kGridUnitScale = 2048.0f;        // [0x00295138]
constexpr float kDefaultStepHeight = 160.0f;
constexpr float kMinStepHeight = 64.0f;
constexpr float kMaxStepHeight = 240.0f;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 VAdd(const Vec3& a, const Vec3& b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 VSub(const Vec3& a, const Vec3& b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 VScale(const Vec3& v, float s) {
    return {v.x * s, v.y * s, v.z * s};
}

inline float VDot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 VCross(const Vec3& a, const Vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

inline float VLenSq(const Vec3& v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

inline float VLen(const Vec3& v) {
    return std::sqrt(VLenSq(v));
}

inline bool IsFiniteVec3(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool ValidGuest(uint32_t va, uint32_t bytes = 4u) {
    return kernel::g_base != nullptr && va >= 0x10000u && mem::IsCommitted(va) &&
           (bytes <= 1u || mem::IsCommitted(va + bytes - 1u));
}

Vec3 ReadVec3(uint8_t* base, uint32_t va) {
    return {
        X86_MEMF32(va + 0x00u),
        X86_MEMF32(va + 0x04u),
        X86_MEMF32(va + 0x08u),
    };
}

void WriteVec3(uint8_t* base, uint32_t va, const Vec3& v) {
    X86_MEMF32(va + 0x00u) = v.x;
    X86_MEMF32(va + 0x04u) = v.y;
    X86_MEMF32(va + 0x08u) = v.z;
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
    if (fn_va < 0x10000u || this_ptr < 0x10000u) return 0u;
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
    if (fn_va < 0x10000u || this_ptr < 0x10000u) return 0.0f;
    const uint32_t saved_ecx = ctx.ecx;
    const SavedX87State saved_fpu = SavedX87State::Capture(ctx);
    ctx.ecx = this_ptr;
    (void)kernel::CallGuest(ctx, fn_va, args);
    const float ret_f = static_cast<float>(X87_ST(0));
    ctx.ecx = saved_ecx;
    saved_fpu.Restore(ctx);
    return std::isfinite(ret_f) ? ret_f : 0.0f;
}

uint32_t ResolveObjectHandle(uint8_t* base, uint16_t handle) {
    if (handle == 0u) return 0u;
    const uint32_t world_va = ValidGuest(kVaWorldPtr, 4u) ? X86_MEM32(kVaWorldPtr) : 0u;
    if (!ValidGuest(world_va, 0x24u)) return 0u;
    const uint32_t table_va = X86_MEM32(world_va + 0x0Cu);
    const int32_t table_cap = static_cast<int32_t>(X86_MEM32(world_va + 0x14u));
    if (table_va == 0u || static_cast<int32_t>(handle) >= table_cap) return 0u;
    const uint32_t slot_va = table_va + static_cast<uint32_t>(handle) * 6u;
    if (!ValidGuest(slot_va, 4u)) return 0u;
    const uint32_t obj_va = X86_MEM32(slot_va);
    return ValidGuest(obj_va, 0xF0u) ? obj_va : 0u;
}

bool IsActivePlayerObject(uint8_t* base, uint32_t obj_va) {
    if (!ValidGuest(obj_va, 0xF0u)) return false;
    if ((X86_MEM32(obj_va + 0x08u) & 0x80000000u) != 0u) return false;
    const uint32_t world_va = ValidGuest(kVaWorldPtr, 4u) ? X86_MEM32(kVaWorldPtr) : 0u;
    if (!ValidGuest(world_va, 0x138u)) return false;
    const int32_t player_count = static_cast<int32_t>(X86_MEM32(world_va + 0x12Cu));
    const uint32_t player_list_va = X86_MEM32(world_va + 0x124u);
    if (player_count <= 0 || !ValidGuest(player_list_va, static_cast<uint32_t>(player_count) * 4u)) {
        return false;
    }
    for (int32_t i = 0; i < player_count && i < 4; ++i) {
        if (X86_MEM32(player_list_va + static_cast<uint32_t>(i) * 4u) == obj_va) {
            return true;
        }
    }
    return false;
}

float QueryTorqueStepHeight(X86Context& ctx, uint8_t* base, uint32_t obj_va) {
    if (!ValidGuest(obj_va + 0xECu, 4u)) return kDefaultStepHeight;
    const uint32_t prop_mgr_va = X86_MEM32(obj_va + 0xECu);
    if (!ValidGuest(prop_mgr_va, 4u)) return kDefaultStepHeight;
    const uint32_t vt_va = X86_MEM32(prop_mgr_va);
    if (!ValidGuest(vt_va + 0x08u, 4u)) return kDefaultStepHeight;
    const uint32_t get_prop_fn = X86_MEM32(vt_va + 0x08u);
    const uint32_t val_va = CallThiscall(ctx, get_prop_fn, prop_mgr_va, {0x5Cu, 0xFFFFFFFFu});
    if (!ValidGuest(val_va, 4u)) return kDefaultStepHeight;
    const float raw_grid = X86_MEMF32(val_va);
    if (!std::isfinite(raw_grid) || raw_grid <= 0.0f) return kDefaultStepHeight;
    return std::clamp(raw_grid * kGridUnitScale, kMinStepHeight, kMaxStepHeight);
}

void SetObjectWorldPos(X86Context& ctx, uint8_t* base, uint32_t obj_va, const Vec3& pos) {
    if (!ValidGuest(obj_va, 0xF0u) || !IsFiniteVec3(pos)) return;
    const uint32_t tf_va = CallThiscall(ctx, kFnGetMutableTransform, obj_va);
    if (ValidGuest(tf_va + 0x30u, 12u)) {
        WriteVec3(base, tf_va + 0x30u, pos);
    } else {
        WriteVec3(base, obj_va + 0x44u, pos);
    }
}

struct SnapSphere {
    bool is_foot = false;
    float radius = 0.0f;
    Vec3 local_pos{};
};

struct SnapTri {
    uint32_t tri_entry_va = 0u;
    uint32_t owner_obj_va = 0u;
    int32_t pool_index = 0;
    Vec3 v0{};  // in CCollider ref_origin-relative space
    Vec3 v1{};
    Vec3 v2{};
    Vec3 normal{};
    float min_y = 0.0f;
    float max_y = 0.0f;
    bool is_walkable_floor = false;
    bool is_stair_ramp = false;
};

struct ClimbAssistState {
    uint32_t torque_va = 0u;
    bool active = false;
    float lifted_y = 0.0f;
    float target_land_y = 0.0f;
    float dir_x = 0.0f;
    float dir_z = 0.0f;
    int idle_frames = 0;
};

struct WedgeTrackerState {
    uint32_t torque_va = 0u;
    int frames = 0;
    float accum_s = 0.0f;
    Vec3 anchor_pos{};
};

// Guest-main-thread-only active collision scope & snapshots
uint32_t g_active_torque_va = 0u;
uint32_t g_active_collider_va = 0u;
bool g_active_sweep_had_hit = false;
bool g_active_sweep_hard_stopped = false;
bool g_snapshot_valid = false;
Vec3 g_ref_origin{};
float g_foot_radius = 90.0f;
std::vector<SnapSphere> g_snap_spheres;
std::vector<SnapTri> g_snap_tris;
ClimbAssistState g_climb{};
WedgeTrackerState g_wedge{};

void CaptureColliderSnapshot(uint8_t* base, uint32_t col_va) {
    g_snapshot_valid = false;
    g_snap_spheres.clear();
    g_snap_tris.clear();

    if (!ValidGuest(col_va, 0x19Eu)) return;

    g_ref_origin = ReadVec3(base, col_va + 0xC0u);
    if (!IsFiniteVec3(g_ref_origin)) {
        return;
    }

    const int32_t foot_bone_id = static_cast<int32_t>(X86_MEM32(col_va + 0x0Cu));
    const uint32_t spheres_va = X86_MEM32(col_va + 0x122u);
    const int32_t sphere_count = static_cast<int32_t>(X86_MEM32(col_va + 0x12Au));
    if (sphere_count <= 0 || sphere_count > 32 ||
        !ValidGuest(spheres_va, static_cast<uint32_t>(sphere_count) * 0x3Cu)) {
        return;
    }

    float found_foot_r = 0.0f;
    g_snap_spheres.reserve(static_cast<size_t>(sphere_count));
    for (int32_t i = 0; i < sphere_count; ++i) {
        const uint32_t s_va = spheres_va + static_cast<uint32_t>(i) * 0x3Cu;
        if (X86_MEM32(s_va + 0x00u) == 0u) continue;
        const int32_t bone_id = static_cast<int32_t>(X86_MEM32(s_va + 0x04u));
        const float radius = X86_MEMF32(s_va + 0x08u);
        const Vec3 local_pos = ReadVec3(base, s_va + 0x0Cu);
        if (!std::isfinite(radius) || radius <= 0.1f || !IsFiniteVec3(local_pos)) continue;

        SnapSphere ss{};
        ss.is_foot = (bone_id == foot_bone_id);
        ss.radius = radius;
        ss.local_pos = local_pos;
        if (ss.is_foot) found_foot_r = radius;
        g_snap_spheres.push_back(ss);
    }
    if (g_snap_spheres.empty()) return;
    if (found_foot_r <= 0.1f) {
        found_foot_r = g_snap_spheres.front().radius;
        g_snap_spheres.front().is_foot = true;
    }
    g_foot_radius = std::clamp(found_foot_r, 30.0f, 250.0f);

    const uint32_t pool_arr_va = X86_MEM32(col_va + 0x18Au);
    if (!ValidGuest(pool_arr_va, 0x30u)) return;

    constexpr int32_t kMaxTrisPerPool = 512;
    for (int32_t p = 0; p < 3; ++p) {
        const uint32_t hdr_va = pool_arr_va + static_cast<uint32_t>(p) * 0x10u;
        const uint32_t data_va = X86_MEM32(hdr_va + 0x00u);
        const int32_t count = static_cast<int32_t>(X86_MEM32(hdr_va + 0x08u));
        const uint32_t is_chunked = X86_MEM32(hdr_va + 0x0Cu);
        if (count <= 0 || is_chunked != 0u || data_va == 0u) continue;
        const int32_t capped = std::min(count, kMaxTrisPerPool);
        if (!ValidGuest(data_va, static_cast<uint32_t>(capped) * 0x40u)) continue;

        for (int32_t t = 0; t < capped; ++t) {
            const uint32_t t_va = data_va + static_cast<uint32_t>(t) * 0x40u;
            const uint32_t t_flags = X86_MEM32(t_va + 0x00u);
            const Vec3 v0 = ReadVec3(base, t_va + 0x08u);
            const Vec3 v1 = ReadVec3(base, t_va + 0x14u);
            const Vec3 v2 = ReadVec3(base, t_va + 0x20u);
            if (!IsFiniteVec3(v0) || !IsFiniteVec3(v1) || !IsFiniteVec3(v2)) continue;

            Vec3 n{};
            if ((t_flags & 0x02u) != 0u) {
                n = ReadVec3(base, t_va + 0x2Cu);
            } else {
                // Riot Engine CW outward normal: normalize((v2 - v0) x (v1 - v0)) (sub_0001F2C0)
                const Vec3 raw_n = VCross(VSub(v2, v0), VSub(v1, v0));
                const float nl = VLen(raw_n);
                if (nl <= 1e-5f) continue;
                n = VScale(raw_n, 1.0f / nl);
            }
            if (!IsFiniteVec3(n) || VLenSq(n) < 0.5f) continue;

            const float min_y = std::min({v0.y, v1.y, v2.y});
            const float max_y = std::max({v0.y, v1.y, v2.y});
            const float span_y = (max_y - min_y);
            const uint32_t tri_owner = X86_MEM32(t_va + 0x3Cu);

            SnapTri st{};
            st.tri_entry_va = t_va;
            st.owner_obj_va = (p == 0 && ValidGuest(tri_owner, 0x60u)) ? tri_owner : 0u;
            st.pool_index = p;
            st.v0 = v0;
            st.v1 = v1;
            st.v2 = v2;
            st.normal = n;
            st.min_y = min_y;
            st.max_y = max_y;
            st.is_walkable_floor = (n.y >= kMinWalkableNormalY);
            st.is_stair_ramp = (!st.is_walkable_floor && n.y >= kMinStairRampNormalY && span_y < kMaxStairTriSpanY);
            g_snap_tris.push_back(st);
        }
    }

    g_snapshot_valid = !g_snap_tris.empty();
}

Vec3 ClosestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    const Vec3 ab = VSub(b, a);
    const Vec3 ac = VSub(c, a);
    const Vec3 ap = VSub(p, a);
    const float d1 = VDot(ab, ap);
    const float d2 = VDot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;

    const Vec3 bp = VSub(p, b);
    const float d3 = VDot(ab, bp);
    const float d4 = VDot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        const float denom = d1 - d3;
        const float v = (denom > 1e-6f) ? (d1 / denom) : 0.0f;
        return VAdd(a, VScale(ab, v));
    }

    const Vec3 cp = VSub(p, c);
    const float d5 = VDot(ab, cp);
    const float d6 = VDot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        const float denom = d2 - d6;
        const float w = (denom > 1e-6f) ? (d2 / denom) : 0.0f;
        return VAdd(a, VScale(ac, w));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        const Vec3 bc = VSub(c, b);
        const float denom = (d4 - d3) + (d5 - d6);
        const float w = (denom > 1e-6f) ? ((d4 - d3) / denom) : 0.0f;
        return VAdd(b, VScale(bc, w));
    }

    const float denom_all = va + vb + vc;
    if (std::fabs(denom_all) <= 1e-6f) return a;
    const float inv_denom = 1.0f / denom_all;
    const float v = vb * inv_denom;
    const float w = vc * inv_denom;
    return VAdd(a, VAdd(VScale(ab, v), VScale(ac, w)));
}

bool PointInTriXZ(float rel_x, float rel_z, const SnapTri& t, float edge_eps, float& out_rel_y) {
    if (t.normal.y <= 0.25f) return false;
    const float ax = t.v0.x, az = t.v0.z;
    const float bx = t.v1.x, bz = t.v1.z;
    const float cx = t.v2.x, cz = t.v2.z;

    const float e0 = (bx - ax) * (rel_z - az) - (bz - az) * (rel_x - ax);
    const float e1 = (cx - bx) * (rel_z - bz) - (cz - bz) * (rel_x - bx);
    const float e2 = (ax - cx) * (rel_z - cz) - (az - cz) * (rel_x - cx);

    const float area2 = (bx - ax) * (cz - az) - (bz - az) * (cx - ax);
    const float abs_area2 = std::fabs(area2);
    if (abs_area2 < 1e-3f) return false;

    // Convert world-unit edge tolerance into scaled edge-function units
    const float len0 = std::hypot(bx - ax, bz - az);
    const float len1 = std::hypot(cx - bx, cz - bz);
    const float len2 = std::hypot(ax - cx, az - cz);
    if (area2 > 0.0f) {
        if (e0 < -edge_eps * len0 || e1 < -edge_eps * len1 || e2 < -edge_eps * len2) return false;
    } else {
        if (e0 > edge_eps * len0 || e1 > edge_eps * len1 || e2 > edge_eps * len2) return false;
    }

    const float raw_y = t.v0.y - (t.normal.x * (rel_x - t.v0.x) + t.normal.z * (rel_z - t.v0.z)) / t.normal.y;
    out_rel_y = std::clamp(raw_y, t.min_y - 1.0f, t.max_y + 1.0f);
    return true;
}

bool IsPoseClearOfObstacles(const Vec3& cand_world_pos, float support_world_y, float radius_shrink = 1.5f) {
    if (!g_snapshot_valid) return false;
    const Vec3 cand_rel_base = VSub(cand_world_pos, g_ref_origin);
    const float support_rel_y = support_world_y - g_ref_origin.y;
    const bool multi_sphere = (g_snap_spheres.size() > 1u);

    for (const SnapSphere& s : g_snap_spheres) {
        const Vec3 center_rel = VAdd(cand_rel_base, s.local_pos);
        const float eff_r = std::max(s.radius - radius_shrink, s.radius * 0.80f);
        const float eff_r_sq = eff_r * eff_r;

        for (const SnapTri& t : g_snap_tris) {
            // 1. Triangles lying completely at or below the target support plane (such as the staircase
            //    riser, lower tread, or top-landing floor we are stepping onto) lie beneath Torque's feet
            //    at support_world_y, while any wall, railing, closed gate, or ceiling extends above it.
            if (t.max_y <= support_rel_y + 6.0f) continue;

            // 2. Mirror GatherObjectMeshTriangles (sub_00173730 @ 0x001740E9..0x00174159): when Torque
            //    has upper-body spheres, walkable floor and staircase-ramp triangles route to ground
            //    candidates for the foot sphere while upper spheres still collide solidly against them.
            if (s.is_foot && (t.is_walkable_floor || t.is_stair_ramp)) {
                if (multi_sphere) continue;
                const float centroid_y = (t.v0.y + t.v1.y + t.v2.y) * (1.0f / 3.0f);
                if (centroid_y <= center_rel.y + 4.0f) continue;
            }

            if (center_rel.y + eff_r < t.min_y || center_rel.y - eff_r > t.max_y) continue;

            const Vec3 cp = ClosestPointOnTriangle(center_rel, t.v0, t.v1, t.v2);
            const Vec3 diff = VSub(center_rel, cp);
            const float d_sq = VLenSq(diff);
            if (d_sq < eff_r_sq) {
                // Ignore pure back-face interior grazes behind single-sided floor shells
                const float signed_n = VDot(diff, t.normal);
                if (signed_n >= -eff_r * 0.40f) {
                    return false;
                }
            }
        }
    }
    return true;
}

const SnapTri* FindBestSupportTriangleAt(float world_x, float world_z, float min_world_y,
                                         float max_world_y, float prefer_near_y,
                                         bool allow_stair_ramps, float edge_eps,
                                         float& out_world_y) {
    if (!g_snapshot_valid) return nullptr;
    const float rel_x = world_x - g_ref_origin.x;
    const float rel_z = world_z - g_ref_origin.z;
    const float min_rel_y = min_world_y - g_ref_origin.y;
    const float max_rel_y = max_world_y - g_ref_origin.y;
    const float pref_rel_y = prefer_near_y - g_ref_origin.y;

    const SnapTri* best = nullptr;
    float best_score = 1e30f;
    float best_rel_y = 0.0f;

    for (const SnapTri& t : g_snap_tris) {
        if (!t.is_walkable_floor && !(allow_stair_ramps && t.is_stair_ramp)) continue;
        if (t.max_y < min_rel_y - 2.0f || t.min_y > max_rel_y + 2.0f) continue;

        float surf_rel_y = 0.0f;
        if (!PointInTriXZ(rel_x, rel_z, t, edge_eps, surf_rel_y)) continue;
        if (surf_rel_y < min_rel_y || surf_rel_y > max_rel_y) continue;

        // Prefer highest valid support when stepping up, or closest to prefer_near_y when bridging seams
        const float dist_pref = std::fabs(surf_rel_y - pref_rel_y);
        const float slope_penalty = t.is_walkable_floor ? 0.0f : 12.0f;
        const float score = dist_pref + slope_penalty;
        if (score < best_score) {
            best_score = score;
            best = &t;
            best_rel_y = surf_rel_y;
        }
    }

    if (best != nullptr) {
        out_world_y = best_rel_y + g_ref_origin.y;
    }
    return best;
}

void ApplyTorquePositionAdjustment(X86Context& ctx, uint8_t* base, uint32_t torque_va,
                                   uint32_t obj_va, uint32_t col_va,
                                   const Vec3& prev_after_pos, const Vec3& new_pos) {
    const Vec3 adj = VSub(new_pos, prev_after_pos);
    if (VLenSq(adj) <= 1e-8f) return;

    // 1. Move Torque's CGameObject and invalidate its skeleton bone world matrices
    SetObjectWorldPos(ctx, base, obj_va, new_pos);
    const uint32_t skel_va = X86_MEM32(obj_va + 0xE8u);
    if (ValidGuest(skel_va, 0x20u)) {
        CallThiscall(ctx, kFnInvalidateSkeleton, skel_va);
    }

    // 2. Move CCollider's collision proxy object ([col+0xD8]) by the exact same delta and sync prev_pos
    const uint16_t proxy_handle = X86_MEM16(col_va + 0xD8u);
    const uint32_t proxy_va = ResolveObjectHandle(base, proxy_handle);
    if (proxy_va != 0u && proxy_va != obj_va) {
        const Vec3 proxy_pos = ReadVec3(base, proxy_va + 0x44u);
        SetObjectWorldPos(ctx, base, proxy_va, VAdd(proxy_pos, adj));
    }
    WriteVec3(base, col_va + 0x84u, new_pos);

    // 3. Keep first-person camera ([world+0x130]) locked to Torque when first-person bit (0x10000) is active
    //    (mirrors 0x0005C031..0x0005C088 at the tail of sub_0005BF60)
    if ((X86_MEM32(torque_va + 0xA4u) & 0x10000u) != 0u) {
        const uint32_t world_va = ValidGuest(kVaWorldPtr, 4u) ? X86_MEM32(kVaWorldPtr) : 0u;
        const uint32_t cam_va = ValidGuest(world_va + 0x130u, 4u) ? X86_MEM32(world_va + 0x130u) : 0u;
        if (ValidGuest(cam_va, 0x60u) && cam_va != obj_va) {
            const Vec3 cam_pos = ReadVec3(base, cam_va + 0x44u);
            SetObjectWorldPos(ctx, base, cam_va, VAdd(cam_pos, adj));
        }
    }
}

void AttachTorqueGroundSupport(X86Context& ctx, uint8_t* base, uint32_t torque_va,
                               uint32_t col_va, uint32_t gs_va, const SnapTri* support_tri) {
    if (!ValidGuest(torque_va, 0x790u) || !ValidGuest(col_va, 0x19Eu) || !ValidGuest(gs_va, 0xA4u)) {
        return;
    }

    if (support_tri != nullptr && support_tri->owner_obj_va != 0u &&
        ValidGuest(support_tri->tri_entry_va, 0x40u) &&
        X86_MEM32(support_tri->tri_entry_va + 0x3Cu) == support_tri->owner_obj_va) {
        // Mesh triangle (.mdu / .odu): populate SFloorHitInfo via sub_001707E0 and bind CGroundSupport
        CallThiscall(ctx, kFnWriteFloorHitInfo, col_va, {support_tri->tri_entry_va});
        (void)CallThiscallFloat(ctx, kFnSetGroundFromHitInfo, gs_va, {col_va + 0x146u});
    } else {
        // Terrain triangle (.wdu): refresh heightmap floor and clear mesh CGroundSupport
        CallThiscall(ctx, kFnQueryHeightmapFloor, torque_va);
        CallThiscall(ctx, kFnClearGroundSupport, gs_va);
    }

    // Clamp downward velocity and transition cleanly out of Fall/Jump into ON_GROUND
    if (X86_MEMF32(torque_va + 0x15Eu) < 0.0f) {
        X86_MEMF32(torque_va + 0x15Eu) = 0.0f;
    }
    X86_MEM32(col_va) &= ~0x4000u;
    CallThiscall(ctx, kFnLandOnGround, torque_va);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. CCollider::SweepStep (__thiscall, ret 0: ecx = CCollider* -> eax = continue_loop)
//    - Snapshots gathered CTriPool triangles & sphere offsets on iteration 0 for CTorque's collider.
//    - Ensures at least 12 slide iterations so high-FPS micro-steps never hit 0x00174370 movement cancel.
//    - Fixes 0x0017447F -> 0x001744B4 post-collision teleport bug: after a sphere collision (safe_steps >= 0),
//      when |remaining_delta|^2 < threshold, stops cleanly at the last verified collision-free substep
//      (0x001744D5) instead of blindly adding the unswept post-collision slide vector into solid geometry.
// ---------------------------------------------------------------------------
X86_FUNC(sub_00174340) {
    if (app::IsReplayOrAutotestActive() || ctx.ecx == 0u || ctx.ecx != g_active_collider_va ||
        !ValidGuest(ctx.ecx, 0x19Eu)) {
        __imp__sub_00174340(ctx, base);
        return;
    }

    const uint32_t col_va = ctx.ecx;
    const int32_t prev_iter = static_cast<int32_t>(X86_MEM32(col_va + 0x18u));
    if (prev_iter < 0 && ValidGuest(g_active_torque_va, 0x790u)) {
        CaptureColliderSnapshot(base, col_va);
    }

    const float rem_dist = X86_MEMF32(col_va + 0x3Cu);
    const float substep_size = std::max(X86_MEMF32(col_va + 0x40u), 0.1f);
    const int32_t num_substeps = std::max(static_cast<int32_t>(rem_dist / substep_size) + 1, 1);
    X86_MEM32(col_va + 0x10u) = static_cast<uint32_t>(num_substeps);

    int32_t max_iters = static_cast<int32_t>(X86_MEM32(col_va + 0x14u));
    if (max_iters < 0) {
        max_iters = std::max(num_substeps + 1, 12);
        X86_MEM32(col_va + 0x14u) = static_cast<uint32_t>(max_iters);
    }

    const int32_t cur_iter = prev_iter + 1;
    X86_MEM32(col_va + 0x18u) = static_cast<uint32_t>(cur_iter);
    if (cur_iter > max_iters) {
        // Keep already-verified safe travel accumulated in collision_correction rather than resetting to -desired_delta
        ctx.eax = 0u;
        ctx.esp += 4u;
        return;
    }

    const int32_t safe_steps = static_cast<int32_t>(CallThiscall(ctx, kFnSweepSubstepsAndSlide, col_va));
    if (safe_steps < 0) {
        // Zero collisions along the entire remaining_delta: safe to apply remaining_delta (0x001744AF..0x001744D5)
        if ((X86_MEM32(col_va) & 0x01u) == 0u) {
            const Vec3 rem = ReadVec3(base, col_va + 0x60u);
            const Vec3 corr = ReadVec3(base, col_va + 0x78u);
            WriteVec3(base, col_va + 0x78u, VAdd(corr, rem));
        }
        ctx.eax = 0u;
        ctx.esp += 4u;
        return;
    }

    // Collision occurred at substep (safe_steps + 1)
    g_active_sweep_had_hit = true;
    if ((X86_MEM32(col_va) & 0x01u) != 0u) {
        g_active_sweep_hard_stopped = true;
    }

    const float t_safe = std::clamp(static_cast<float>(safe_steps) / static_cast<float>(num_substeps), 0.0f, 1.0f);
    const Vec3 pre_slide = ReadVec3(base, col_va + 0x6Cu);
    const Vec3 corr = ReadVec3(base, col_va + 0x78u);
    WriteVec3(base, col_va + 0x78u, VAdd(corr, VScale(pre_slide, t_safe)));

    const Vec3 rem_scaled = VScale(ReadVec3(base, col_va + 0x60u), 1.0f - t_safe);
    WriteVec3(base, col_va + 0x60u, rem_scaled);

    if ((X86_MEM32(col_va) & 0x01u) != 0u) {
        ctx.eax = 0u;
        ctx.esp += 4u;
        return;
    }

    const Vec3 desired = ReadVec3(base, col_va + 0x54u);
    if (VDot(desired, rem_scaled) < 0.0f) {
        X86_MEM32(col_va) |= 0x02u;
        ctx.eax = 0u;
        ctx.esp += 4u;
        return;
    }

    const float desired_dist = X86_MEMF32(col_va + 0x38u);
    const float thresh_sq = std::max(substep_size * substep_size, 0.01f * desired_dist * desired_dist);
    const float rem_len_sq = VLenSq(rem_scaled);
    if (rem_len_sq < thresh_sq) {
        // Post-collision early exit: stop at the last verified safe substep (0x001744D5) WITHOUT adding
        // the untested post-collision rem_scaled into collision_correction!
        ctx.eax = 0u;
        ctx.esp += 4u;
        return;
    }

    X86_MEMF32(col_va + 0x3Cu) = std::sqrt(rem_len_sq);
    ctx.eax = 1u;
    ctx.esp += 4u;
}

// ---------------------------------------------------------------------------
// 2. CCollider::ResolveCollision (__thiscall, ret 0x08: Vec3* out_corr, CGameObject* obj)
//    Scales substep_size ([col+0x40]) proportionally when desired_dist ([col+0x38]) is small at
//    60/120/240 FPS so Torque always gets >= 8 spatial substeps per frame (matching 30 FPS precision).
// ---------------------------------------------------------------------------
X86_FUNC(sub_00174530) {
    if (app::IsReplayOrAutotestActive() || ctx.ecx == 0u || ctx.ecx != g_active_collider_va ||
        !ValidGuest(ctx.ecx, 0x19Eu)) {
        __imp__sub_00174530(ctx, base);
        return;
    }

    const uint32_t col_va = ctx.ecx;
    const float saved_substep = X86_MEMF32(col_va + 0x40u);
    const float desired_dist = X86_MEMF32(col_va + 0x38u);
    bool modified_substep = false;

    if (std::isfinite(saved_substep) && saved_substep > 0.25f &&
        std::isfinite(desired_dist) && desired_dist > 1e-4f) {
        const float target_substep = std::clamp(std::min(saved_substep, desired_dist * 0.125f), 0.25f, saved_substep);
        if (target_substep < saved_substep) {
            X86_MEMF32(col_va + 0x40u) = target_substep;
            modified_substep = true;
        }
    }

    __imp__sub_00174530(ctx, base);

    if (modified_substep && ValidGuest(col_va + 0x40u, 4u)) {
        X86_MEMF32(col_va + 0x40u) = saved_substep;
    }
}

// ---------------------------------------------------------------------------
// 3. CTorque::TryLandOnMeshGround (__thiscall, ret 0: ecx = CTorque*)
//    Allows landing onto verified staircase slopes (ny >= 0.62f, tri_height < 400.0f -> bStepUpSlope == 1
//    at [col+0x14A]) so Quarry's 40.5..45 deg staircases (ny = 0.707..0.760) do not leave Torque stuck
//    in State 9 (Fall) with Jump disabled.
// ---------------------------------------------------------------------------
X86_FUNC(sub_0003CE90) {
    if (app::IsReplayOrAutotestActive() || ctx.ecx == 0u || ctx.ecx != g_active_torque_va ||
        !ValidGuest(ctx.ecx, 0x790u)) {
        __imp__sub_0003CE90(ctx, base);
        return;
    }

    const uint32_t torque_va = ctx.ecx;
    const uint32_t col_va = X86_MEM32(torque_va + 0xD0u);
    const uint32_t gs_va = X86_MEM32(torque_va + 0xD4u);
    float saved_min_ny = kMinWalkableNormalY;
    bool lowered_threshold = false;

    if (ValidGuest(col_va, 0x180u) && ValidGuest(gs_va, 0xA4u) && X86_MEM32(col_va + 0x14Au) != 0u) {
        saved_min_ny = X86_MEMF32(gs_va + 0x0Cu);
        if (std::isfinite(saved_min_ny) && saved_min_ny > kMinStairRampNormalY) {
            X86_MEMF32(gs_va + 0x0Cu) = kMinStairRampNormalY;
            lowered_threshold = true;
        }
    }

    __imp__sub_0003CE90(ctx, base);

    if (lowered_threshold && ValidGuest(gs_va + 0x0Cu, 4u)) {
        X86_MEMF32(gs_va + 0x0Cu) = saved_min_ny;
    }
}

// ---------------------------------------------------------------------------
// 4. CTorque::PostMoveCollisionTick (__thiscall, ret 0: ecx = CTorque*)
//    Orchestrates:
//     A. Enabling CCollider bit 0x08 (STEP_UP_ENABLED) & restoring 0x8000 before GatherCollisionTriangles.
//     B. Unwelded staircase seam ground-bridging when sub_0003D760 drops support across unwelded .mdu tris.
//     C. Smooth, framerate-independent staircase top-edge (crest / lip) step-up assist with full all-sphere
//        headroom/wall/railing clearance verification.
//     D. Quarry V-crease anti-wedge recovery if pinned in State 9 (Fall) with near-zero displacement.
// ---------------------------------------------------------------------------
X86_FUNC(sub_0005BF60) {
    if (app::IsReplayOrAutotestActive() || !ValidGuest(ctx.ecx, 0x790u) ||
        X86_MEM32(ctx.ecx) != kVtTorquePrimary) {
        __imp__sub_0005BF60(ctx, base);
        return;
    }

    const uint32_t torque_va = ctx.ecx;
    const uint32_t obj_va = X86_MEM32(torque_va + 0x04u);
    const uint32_t col_va = X86_MEM32(torque_va + 0xD0u);
    const uint32_t gs_va = X86_MEM32(torque_va + 0xD4u);

    if (!IsActivePlayerObject(base, obj_va) || !ValidGuest(col_va, 0x19Eu) || !ValidGuest(gs_va, 0xA4u)) {
        __imp__sub_0005BF60(ctx, base);
        return;
    }

    const uint32_t phys_flags_before = X86_MEM32(torque_va + 0x98u);
    const uint32_t misc_flags_before = X86_MEM32(torque_va + 0x9Cu);
    const uint32_t state_bits_before = X86_MEM32(torque_va + 0xA4u);
    const float hp = X86_MEMF32(torque_va + 0x16u);

    // Bypass custom logic during noclip (0x40), swimming (0xC000), cutscene/disabled (misc & 0x02),
    // ladder/ledge climb (State 5 == 0x20 or climb handle [+0x664] != 0), scripted grapple ([+0x72C] != -1),
    // streaming proxy mesh ([col] & 0x400), or when dead.
    const uint32_t col_flags_entry = X86_MEM32(col_va);
    const bool special_mode =
        !(hp > 0.0f) ||
        (phys_flags_before & 0xC040u) != 0u ||
        (misc_flags_before & 0x02u) != 0u ||
        (state_bits_before & (1u << 5)) != 0u ||
        X86_MEM16(torque_va + 0x664u) != 0u ||
        X86_MEM32(torque_va + 0x72Cu) != 0xFFFFFFFFu ||
        (col_flags_entry & 0x400u) != 0u;

    if (special_mode) {
        g_climb.active = false;
        g_wedge.frames = 0;
        g_wedge.accum_s = 0.0f;
        __imp__sub_0005BF60(ctx, base);
        return;
    }

    if (g_climb.torque_va != torque_va) {
        g_climb = ClimbAssistState{};
        g_climb.torque_va = torque_va;
    }
    if (g_wedge.torque_va != torque_va) {
        g_wedge = WedgeTrackerState{};
        g_wedge.torque_va = torque_va;
    }

    // Capture pre-collision start_pos (col->prev_pos) and post-integration desired_pos (obj->pos)
    const Vec3 prev_pos_entry = ReadVec3(base, col_va + 0x84u);
    const Vec3 desired_pos_entry = ReadVec3(base, obj_va + 0x44u);
    const bool has_valid_prev = (VLenSq(prev_pos_entry) > 1e-20f) && IsFiniteVec3(prev_pos_entry);
    const Vec3 start_pos = has_valid_prev ? prev_pos_entry : desired_pos_entry;
    const float vel_y_before = X86_MEMF32(torque_va + 0x15Eu);
    const bool was_on_ground = (phys_flags_before & 0x01u) != 0u;
    const bool was_jumping = (phys_flags_before & 0x1900u) != 0u || (state_bits_before & (1u << 8)) != 0u;

    // Pillar 1: Ensure STEP_UP_ENABLED (0x08) and 0x8000 (omitted in 0x00054EF4) are active on Torque's collider
    X86_MEM32(col_va) = col_flags_entry | 0x8008u;

    // Arm active scope for sub_00174530, sub_00174340, and sub_0003CE90
    g_active_torque_va = torque_va;
    g_active_collider_va = col_va;
    g_active_sweep_had_hit = false;
    g_active_sweep_hard_stopped = false;
    g_snapshot_valid = false;

    __imp__sub_0005BF60(ctx, base);

    g_active_torque_va = 0u;
    g_active_collider_va = 0u;

    if (!g_snapshot_valid || !ValidGuest(obj_va, 0xF0u) || !ValidGuest(col_va, 0x19Eu) || !ValidGuest(gs_va, 0xA4u)) {
        return;
    }

    Vec3 after_pos = ReadVec3(base, obj_va + 0x44u);
    if (!IsFiniteVec3(after_pos)) return;

    const float step_height = QueryTorqueStepHeight(ctx, base, obj_va);
    uint32_t phys_flags_after = X86_MEM32(torque_va + 0x98u);
    uint32_t state_bits_after = X86_MEM32(torque_va + 0xA4u);
    bool is_on_ground_after = (phys_flags_after & 0x01u) != 0u;

    // -----------------------------------------------------------------------
    // Pillar 2: Unwelded Staircase Seam Ground-Bridge
    // When walking across an unwelded .mdu seam (0 shared vertex indices in sub_0017A400) high above
    // .wdu terrain, sub_0003D760 drops CGroundSupport and forces State 9 (Fall, ON_GROUND = 0).
    // Immediately re-bridge support if a valid floor/stair triangle lies directly beneath (x, z).
    // -----------------------------------------------------------------------
    if (was_on_ground && !was_jumping && vel_y_before <= 5.0f && !is_on_ground_after &&
        (state_bits_after & (1u << 9)) != 0u) {
        float seam_y = 0.0f;
        const float max_down = std::min(step_height * 0.65f, 96.0f);
        const float max_up = std::min(step_height * 0.45f, 64.0f);
        const SnapTri* seam_tri = FindBestSupportTriangleAt(
            after_pos.x, after_pos.z, after_pos.y - max_down, after_pos.y + max_up,
            after_pos.y, /*allow_stair_ramps=*/true, /*edge_eps=*/4.0f, seam_y);

        if (seam_tri != nullptr) {
            const Vec3 bridged_pos = {after_pos.x, seam_y, after_pos.z};
            if (IsPoseClearOfObstacles(bridged_pos, seam_y, /*radius_shrink=*/2.5f)) {
                ApplyTorquePositionAdjustment(ctx, base, torque_va, obj_va, col_va, after_pos, bridged_pos);
                AttachTorqueGroundSupport(ctx, base, torque_va, col_va, gs_va, seam_tri);
                after_pos = bridged_pos;
                phys_flags_after = X86_MEM32(torque_va + 0x98u);
                state_bits_after = X86_MEM32(torque_va + 0xA4u);
                is_on_ground_after = (phys_flags_after & 0x01u) != 0u;
            }
        }
    }

    // -----------------------------------------------------------------------
    // Pillar 3: Smooth, Framerate-Independent Staircase Top-Edge (Crest / Lip) Step-Up Assist
    // Handles cases where Torque's foot/shin sphere rim hits a staircase top-edge lip at horizontal
    // distance sqrt(R^2 - h^2) ahead of Torque's (x, z) center while ON_GROUND zeroes slide.y.
    // -----------------------------------------------------------------------
    const float desired_dx = desired_pos_entry.x - start_pos.x;
    const float desired_dz = desired_pos_entry.z - start_pos.z;
    const float desired_horiz = std::hypot(desired_dx, desired_dz);
    const float actual_horiz = std::hypot(after_pos.x - start_pos.x, after_pos.z - start_pos.z);

    if (was_jumping) {
        g_climb.active = false;
    } else if (desired_horiz > 0.05f && (was_on_ground || is_on_ground_after || g_climb.active)) {
        const float dir_x = desired_dx / desired_horiz;
        const float dir_z = desired_dz / desired_horiz;

        if (g_climb.active && (dir_x * g_climb.dir_x + dir_z * g_climb.dir_z) < 0.25f) {
            g_climb.active = false;
        }

        const bool horiz_impeded =
            (g_active_sweep_had_hit || g_climb.active) &&
            (actual_horiz < desired_horiz * 0.60f || g_active_sweep_hard_stopped || g_climb.active);

        if (horiz_impeded) {
            const float base_y = g_climb.active ? std::max(after_pos.y, g_climb.lifted_y) : after_pos.y;
            const float min_land_y = g_climb.active
                                         ? std::min(after_pos.y + 2.0f, g_climb.target_land_y - 4.0f)
                                         : (after_pos.y + 2.0f);
            const float max_land_y = start_pos.y + step_height;
            const float pref_land_y = g_climb.active ? g_climb.target_land_y : (after_pos.y + 24.0f);

            if (max_land_y > min_land_y) {
                // Probe from Torque's desired step out to just past the foot sphere's front rim
                const std::array<float, 5> probe_dists = {
                    desired_horiz,
                    std::max(desired_horiz, g_foot_radius * 0.35f),
                    std::max(desired_horiz, g_foot_radius * 0.65f),
                    std::max(desired_horiz, g_foot_radius * 0.90f),
                    std::max(desired_horiz, g_foot_radius * 1.15f),
                };

                const SnapTri* best_land_tri = nullptr;
                float best_land_y = 0.0f;
                float best_probe_d = 0.0f;

                for (float pd : probe_dists) {
                    const float px = start_pos.x + dir_x * pd;
                    const float pz = start_pos.z + dir_z * pd;
                    float cand_land_y = 0.0f;
                    const SnapTri* tri = FindBestSupportTriangleAt(
                        px, pz, min_land_y, max_land_y, pref_land_y,
                        /*allow_stair_ramps=*/true, /*edge_eps=*/2.5f, cand_land_y);
                    if (tri == nullptr) continue;

                    // Verify full 3D clearance across ALL Torque spheres:
                    //  (1) Upward headroom at start_pos lifted to cand_land_y
                    //  (2) Open space on the target landing itself (rejects railings, fences, closed doors, walls!)
                    const Vec3 up_pos = {start_pos.x, cand_land_y + 2.0f, start_pos.z};
                    const Vec3 land_probe_pos = {px, cand_land_y + 2.0f, pz};
                    if (!IsPoseClearOfObstacles(up_pos, cand_land_y) ||
                        !IsPoseClearOfObstacles(land_probe_pos, cand_land_y)) {
                        continue;
                    }

                    best_land_tri = tri;
                    best_land_y = cand_land_y;
                    best_probe_d = pd;
                    break;
                }

                if (best_land_tri != nullptr) {
                    const float next_x = start_pos.x + desired_dx;
                    const float next_z = start_pos.z + desired_dz;

                    // Check if (next_x, next_z) is already directly over the target landing triangle
                    float direct_y = 0.0f;
                    const bool over_landing = PointInTriXZ(
                        next_x - g_ref_origin.x, next_z - g_ref_origin.z, *best_land_tri, 3.0f, direct_y);

                    if (over_landing) {
                        const float snapped_y = direct_y + g_ref_origin.y;
                        const Vec3 final_pos = {next_x, snapped_y, next_z};
                        if (IsPoseClearOfObstacles(final_pos, snapped_y)) {
                            ApplyTorquePositionAdjustment(ctx, base, torque_va, obj_va, col_va, after_pos, final_pos);
                            AttachTorqueGroundSupport(ctx, base, torque_va, col_va, gs_va, best_land_tri);
                            after_pos = final_pos;
                            is_on_ground_after = true;
                            g_climb.active = false;
                        }
                    } else if (best_probe_d <= g_foot_radius * 1.20f) {
                        // Progressive framerate-independent ramp lift (desired_horiz == speed * dt) across the lip
                        const float climb_step = std::max(desired_horiz * 1.35f, 2.0f);
                        const float next_y = std::min(best_land_y, base_y + climb_step);
                        const Vec3 check_lifted = {next_x, best_land_y + 2.0f, next_z};
                        const Vec3 step_pos = {next_x, next_y, next_z};

                        if (IsPoseClearOfObstacles(check_lifted, best_land_y)) {
                            ApplyTorquePositionAdjustment(ctx, base, torque_va, obj_va, col_va, after_pos, step_pos);
                            if (X86_MEMF32(torque_va + 0x15Eu) < 0.0f) {
                                X86_MEMF32(torque_va + 0x15Eu) = 0.0f;
                            }
                            X86_MEM32(col_va) &= ~0x4000u;
                            CallThiscall(ctx, kFnLandOnGround, torque_va);

                            after_pos = step_pos;
                            is_on_ground_after = true;
                            g_climb.active = true;
                            g_climb.lifted_y = next_y;
                            g_climb.target_land_y = best_land_y;
                            g_climb.dir_x = dir_x;
                            g_climb.dir_z = dir_z;
                            g_climb.idle_frames = 0;
                        }
                    }
                } else {
                    g_climb.active = false;
                }
            }
        } else if (actual_horiz >= desired_horiz * 0.75f) {
            g_climb.active = false;
        }
    } else if (g_climb.active) {
        if (++g_climb.idle_frames > 3) {
            g_climb.active = false;
        }
    }

    // -----------------------------------------------------------------------
    // Pillar 4: Quarry Staircase V-Crease Anti-Wedge / Unstuck Safety
    // If Torque ever becomes wedged in State 9 (Fall, ON_GROUND == 0, Jump disabled) between opposing
    // non-walkable / crease planes with downward velocity and near-zero 3D displacement for >= 0.12s,
    // push out along the averaged contact normal + upward lift onto clear support.
    // -----------------------------------------------------------------------
    phys_flags_after = X86_MEM32(torque_va + 0x98u);
    state_bits_after = X86_MEM32(torque_va + 0xA4u);
    is_on_ground_after = (phys_flags_after & 0x01u) != 0u;
    const float vel_y_after = X86_MEMF32(torque_va + 0x15Eu);
    const float dt = std::clamp(X86_MEMF32(kVaGlobalDt), 0.001f, 0.10f);
    const float moved_3d = VLen(VSub(after_pos, start_pos));
    const float desired_3d = VLen(VSub(desired_pos_entry, start_pos));

    const bool pinned_in_fall =
        !is_on_ground_after &&
        (state_bits_after & (1u << 9)) != 0u &&
        vel_y_after < -25.0f &&
        g_active_sweep_had_hit &&
        moved_3d < std::max(1.5f, desired_3d * 0.10f);

    if (!pinned_in_fall) {
        g_wedge.frames = 0;
        g_wedge.accum_s = 0.0f;
    } else {
        if (g_wedge.frames == 0) {
            g_wedge.anchor_pos = after_pos;
        }
        ++g_wedge.frames;
        g_wedge.accum_s += dt;

        const float drift_from_anchor = VLen(VSub(after_pos, g_wedge.anchor_pos));
        if (drift_from_anchor > g_foot_radius * 0.50f) {
            g_wedge.anchor_pos = after_pos;
            g_wedge.frames = 1;
            g_wedge.accum_s = dt;
        } else if (g_wedge.frames >= 5 && g_wedge.accum_s >= 0.12f) {
            // Accumulate outward depenetration direction from all nearby / contacting triangles
            Vec3 push_dir = {0.0f, 1.0f, 0.0f};
            const Vec3 after_rel_base = VSub(after_pos, g_ref_origin);
            for (const SnapSphere& s : g_snap_spheres) {
                const Vec3 c_rel = VAdd(after_rel_base, s.local_pos);
                const float probe_r = s.radius + 8.0f;
                const float probe_r_sq = probe_r * probe_r;
                for (const SnapTri& t : g_snap_tris) {
                    if (c_rel.y + probe_r < t.min_y || c_rel.y - probe_r > t.max_y) continue;
                    const Vec3 cp = ClosestPointOnTriangle(c_rel, t.v0, t.v1, t.v2);
                    const Vec3 diff = VSub(c_rel, cp);
                    const float d_sq = VLenSq(diff);
                    if (d_sq < probe_r_sq) {
                        const float d = std::sqrt(std::max(d_sq, 1e-6f));
                        const float weight = (probe_r - d) + 1.0f;
                        const Vec3 n_out = (d > 1e-3f && VDot(diff, t.normal) > 0.0f) ? VScale(diff, 1.0f / d) : t.normal;
                        push_dir = VAdd(push_dir, VScale(n_out, weight));
                    }
                }
            }

            float push_horiz = std::hypot(push_dir.x, push_dir.z);
            const float nx = (push_horiz > 1e-3f) ? (push_dir.x / push_horiz) : 0.0f;
            const float nz = (push_horiz > 1e-3f) ? (push_dir.z / push_horiz) : 0.0f;

            constexpr std::array<float, 4> kLiftHeights = {20.0f, 44.0f, 76.0f, 116.0f};
            constexpr std::array<float, 4> kHorizRadii = {0.0f, 14.0f, 30.0f, 52.0f};
            constexpr std::array<std::pair<float, float>, 5> kDirs = {{
                {1.0f, 0.0f},
                {0.7071f, 0.7071f},
                {0.7071f, -0.7071f},
                {0.0f, 1.0f},
                {-1.0f, 0.0f},
            }};

            bool unstuck_ok = false;
            for (float dy : kLiftHeights) {
                for (float hr : kHorizRadii) {
                    const size_t dir_limit = (hr == 0.0f) ? 1u : kDirs.size();
                    for (size_t di = 0; di < dir_limit; ++di) {
                        const float c = kDirs[di].first;
                        const float s = kDirs[di].second;
                        const float wx = (nx * c - nz * s) * hr;
                        const float wz = (nx * s + nz * c) * hr;
                        const Vec3 cand = {after_pos.x + wx, after_pos.y + dy, after_pos.z + wz};

                        float support_y = cand.y;
                        const SnapTri* floor_tri = FindBestSupportTriangleAt(
                            cand.x, cand.z, after_pos.y - 32.0f, cand.y + 16.0f,
                            cand.y, /*allow_stair_ramps=*/true, /*edge_eps=*/4.0f, support_y);

                        const Vec3 resolved = (floor_tri != nullptr)
                                                  ? Vec3{cand.x, std::max(cand.y, support_y), cand.z}
                                                  : cand;
                        if (!IsPoseClearOfObstacles(resolved, (floor_tri != nullptr) ? support_y : (resolved.y - 8.0f), 2.5f)) {
                            continue;
                        }

                        ApplyTorquePositionAdjustment(ctx, base, torque_va, obj_va, col_va, after_pos, resolved);
                        if (floor_tri != nullptr) {
                            const Vec3 snapped = {resolved.x, support_y, resolved.z};
                            if (IsPoseClearOfObstacles(snapped, support_y, 2.5f)) {
                                ApplyTorquePositionAdjustment(ctx, base, torque_va, obj_va, col_va, resolved, snapped);
                            }
                            AttachTorqueGroundSupport(ctx, base, torque_va, col_va, gs_va, floor_tri);
                        } else if (X86_MEMF32(torque_va + 0x15Eu) < 0.0f) {
                            X86_MEMF32(torque_va + 0x15Eu) = 0.0f;
                        }

                        g_wedge.frames = 0;
                        g_wedge.accum_s = 0.0f;
                        unstuck_ok = true;
                        break;
                    }
                    if (unstuck_ok) break;
                }
                if (unstuck_ok) break;
            }
        }
    }
}
