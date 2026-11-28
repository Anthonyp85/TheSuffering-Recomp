// SPDX-License-Identifier: GPL-2.0-or-later
// Surgical Staircase Top-Edge Step-Up, Docks Multi-Flight / Railed Staircase & Quarry Anti-Wedge Fixes
// for The Suffering (USA) default.xbe (Title ID 0x4D570015).
//
// Root causes proven from XBE disassembly & 10 Docks / 4a-4b Quarry .mdu collision geometry:
//  1. Unwired CCollider::m_flags bit 0x08 (STEP_UP_ENABLED), missing 0x8000 on form re-init, AND
//     inverted 400.0f slope-length comparison in IsWalkableOrStepTriangle (sub_00179D60 @ 0x00179E00):
//     - CTorque::Init (0x00041F80) sets [col] |= 0x8000, while CTorque::ReinitCollider (0x00054EF4)
//       omits 0x8000, and neither sets bit 0x08 (STEP_UP_ENABLED).
//     - Worse, in sub_00179D60 (0x00179DFB..0x00179E0E), ComputeSlopeLength (sub_001797B0) returns
//       ST(0) = (max_y - min_y) / sqrt(1 - ny^2), and 0x00179E00 executes:
//         fcomp [0x002A451C] (400.0f); fnstsw ax; test ah, 0x41; jnz return_0
//       Because slope_len <= 400.0f sets C0=1 or C3=1 (ah & 0x41 != 0), native sub_00179D60 REJECTS
//       every 0.54 <= ny <= 0.76604 staircase ramp with slope_len <= 400.0f (Docks id0=21, 200, 204,
//       339/343, 353, 544, 586..592 & Quarry sub=401/27 T31), causing GatherObjectMeshTriangles
//       (0x001740E9..0x001740F4) to skip adding them to step_candidate_tris so Torque's feet clip
//       through the stairs until his upper-body sphere wedges! Meanwhile CGroundSupport::AdvanceOnMesh
//       (0x0017A81C) passes gs->hit_info.bStepUpSlope ([gs+0x68] == 0 on flat landings) as allow_step,
//       dropping CGroundSupport whenever stepping from a flat landing onto a welded stair ramp.
//  2. ShouldSkipStepTriangle (sub_001709C0 @ 0x00170A3E..0x00170A90) centroid_y >= foot_center_y
//     rejection on long Docks staircase flights:
//     - Docks wooden staircases span 400..1301.3 Y units in single triangles (e.g. id0=593 switchback
//       ny=0.6645 span_y=896.5; id0=602 ny=0.7474 span_y=819.2; id0=354..356 ny=0.705 span_y=1301.3;
//       id0=544 ny=0.580 span_y=1226; id0=586..591 ny=0.563..0.750 span_y=287..512; id0=60 span_y=881).
//     - On the lower half/two-thirds of any long flight, tri_centroid_y >= foot_center_y (and at
//       0x00174152 STriEntry.normal at +0x2C is still uninitialized zero!), so sub_001709C0 returns 0
//       and adds the staircase ramp itself to [foot_sphere+0x24] as a solid wall while ON_GROUND zeroes
//       desired_delta.y and slide.y — wedging Torque on the flight and against side railings.
//  3. Unchecked post-collision slide addition & coarse spatial substep at 60+ FPS in CCollider::SweepStep
//     (sub_00174340 @ 0x0017447F -> 0x001744B4), plus CTriPool snapshot truncation on dense Docks meshes
//     (id0=21 660 tris, id0=544 1072 tris, id0=353 1076 tris, id0=13/548 2178 tris).
//  4. Staircase top-edge (crest / header trim) horizontal block on narrow railed Docks stairs (205..255
//     units wide): rubbing a side handrail/baluster post while reaching a top landing or raised header
//     plank stalled top-edge step-up when over_landing failed clearance at snapped_y or when single-ray
//     probing hit a side railing/post.
//  5. Unwelded staircase seams (sub_0017A400) + TryLandOnMeshGround (sub_0003CE90 @ 0x0003CF00) refusing
//     to land on ny <= 0.76604f stair slopes + State 9 (Fall) / ON_GROUND V-pinch wedges.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <vector>

#include "app/app.h"
#include "common/input_recorder.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

// Hooked guest functions (verified 0 overlap with src/game/hooks.cpp and src/game/cheats.cpp)
X86_EXTERN_FUNC(__imp__sub_0005BF60);
X86_EXTERN_FUNC(__imp__sub_0003CE90);
X86_EXTERN_FUNC(__imp__sub_00174530);
X86_EXTERN_FUNC(__imp__sub_00174340);
X86_EXTERN_FUNC(__imp__sub_00179D60);
X86_EXTERN_FUNC(__imp__sub_001709C0);

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
constexpr float kMinWalkableNormalY = 0.76604f;       // cos(40 deg), [0x002CB858]
constexpr float kMinStairRampNormalY = 0.54f;         // cos(57.3 deg), covers 40..57 deg Docks & Quarry stair ramps
constexpr float kMaxModerateStairSpanY = 1800.0f;     // covers long flights (e.g. Docks id0=354 span_y=1301.3)
constexpr float kMaxSteepStairSpanY = 1350.0f;        // covers steeper dock/ship flights (e.g. Docks id0=544 span_y=1226)
constexpr float kGridUnitScale = 2048.0f;             // [0x00295138]
constexpr float kDefaultStepHeight = 160.0f;
constexpr float kMinStepHeight = 64.0f;
constexpr float kMaxStepHeight = 240.0f;

inline bool IsValidStairRamp(float ny, float span_y) {
    if (!std::isfinite(ny) || !std::isfinite(span_y) || span_y < 0.0f) return false;
    if (ny >= kMinWalkableNormalY || ny < kMinStairRampNormalY) return false;
    const float max_span = (ny >= 0.60f) ? kMaxModerateStairSpanY : kMaxSteepStairSpanY;
    return span_y < max_span;
}

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
    if (kernel::g_base == nullptr || va < 0x10000u) return false;
    const uint32_t last = (bytes > 1u) ? (va + (bytes - 1u)) : va;
    if (last < va) return false;
    return mem::IsCommitted(va) && (bytes <= 1u || mem::IsCommitted(last));
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

    // Capture full CTriPool (native CCollider gathers up to 1000+ triangles across dense Docks meshes
    // such as id0=21 [660], id0=544 [1072], id0=353 [1076], and id0=13/548 [2178]).
    constexpr int32_t kMaxTrisPerPool = 2048;
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
            st.is_stair_ramp = (!st.is_walkable_floor && IsValidStairRamp(n.y, span_y));
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

bool IsPoseClearOfObstacles(const Vec3& cand_world_pos, float support_world_y, float radius_shrink = 1.5f,
                            const Vec3* baseline_world_pos = nullptr) {
    if (!g_snapshot_valid) return false;
    const Vec3 cand_rel_base = VSub(cand_world_pos, g_ref_origin);
    const Vec3 base_rel_base =
        (baseline_world_pos != nullptr) ? VSub(*baseline_world_pos, g_ref_origin) : Vec3{};
    const float support_rel_y = support_world_y - g_ref_origin.y;
    const bool multi_sphere = (g_snap_spheres.size() > 1u);

    for (const SnapSphere& s : g_snap_spheres) {
        const Vec3 center_rel = VAdd(cand_rel_base, s.local_pos);
        const Vec3 base_center_rel =
            (baseline_world_pos != nullptr) ? VAdd(base_rel_base, s.local_pos) : Vec3{};
        const float eff_r = std::max(s.radius - radius_shrink, s.radius * 0.80f);
        const float eff_r_sq = eff_r * eff_r;
        const float hard_min_r = s.radius * 0.65f;
        const float hard_min_r_sq = hard_min_r * hard_min_r;

        for (const SnapTri& t : g_snap_tris) {
            // 1. Triangles lying completely at or below the target support plane (such as the staircase
            //    riser, lower tread, header trim, or top-landing floor we are stepping onto) lie beneath
            //    Torque's feet at support_world_y, while walls, railings, closed gates, or ceilings rise above it.
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
                if (signed_n < -eff_r * 0.40f) {
                    continue;
                }

                // Narrow Docks wooden staircases (205..255 units wide, e.g. id0=586..592, 602, 544, 354)
                // keep Torque's spheres continually grazing the side handrail/baluster posts. If Torque
                // was ALREADY within eff_r of this vertical side plane at baseline_world_pos, allow poses
                // that slide parallel to or away from it (d_cand >= d_base - 0.5f) while still rejecting
                // any ceiling hit, deep penetration, new obstacle, or forward push into a wall/door.
                if (baseline_world_pos != nullptr && t.normal.y >= -0.30f &&
                    std::fabs(t.normal.y) < 0.45f && d_sq >= hard_min_r_sq) {
                    const Vec3 cp_base = ClosestPointOnTriangle(base_center_rel, t.v0, t.v1, t.v2);
                    const float d_base = VLen(VSub(base_center_rel, cp_base));
                    const float d_cand = std::sqrt(std::max(d_sq, 0.0f));
                    if (d_base < eff_r && d_cand >= d_base - 0.5f) {
                        continue;
                    }
                }

                return false;
            }
        }
    }
    return true;
}

bool IsGeometricallyPinchedAt(const Vec3& world_pos) {
    if (!g_snapshot_valid) return false;
    const Vec3 rel_base = VSub(world_pos, g_ref_origin);
    std::array<Vec3, 24> contact_normals{};
    size_t contact_count = 0u;

    for (const SnapSphere& s : g_snap_spheres) {
        const Vec3 c_rel = VAdd(rel_base, s.local_pos);
        const float probe_r = s.radius + 2.0f;
        const float probe_r_sq = probe_r * probe_r;
        const float deep_r = s.radius * 0.78f;

        for (const SnapTri& t : g_snap_tris) {
            if (s.is_foot && (t.is_walkable_floor || t.is_stair_ramp)) continue;
            if (c_rel.y + probe_r < t.min_y || c_rel.y - probe_r > t.max_y) continue;

            const Vec3 cp = ClosestPointOnTriangle(c_rel, t.v0, t.v1, t.v2);
            const Vec3 diff = VSub(c_rel, cp);
            const float d_sq = VLenSq(diff);
            if (d_sq >= probe_r_sq) continue;
            if (VDot(diff, t.normal) < -s.radius * 0.35f) continue;

            const float d = std::sqrt(std::max(d_sq, 1e-6f));
            if (d < deep_r) {
                return true;
            }

            const Vec3 n_out = (d > 1e-3f && VDot(diff, t.normal) > 0.0f) ? VScale(diff, 1.0f / d) : t.normal;
            for (size_t i = 0u; i < contact_count; ++i) {
                // Opposing obstacle normals (> ~117 deg apart) indicate a true V-channel / railing pinch
                // rather than a flat wall (dot = +1) or normal 90-deg room corner (dot = 0).
                if (VDot(n_out, contact_normals[i]) < -0.45f) {
                    return true;
                }
            }
            if (contact_count < contact_normals.size()) {
                contact_normals[contact_count++] = n_out;
            }
        }
    }
    return false;
}

float FindForwardLowStepCrestY(const Vec3& start_pos, float after_y, float max_crest_world_y,
                               float dir_x, float dir_z) {
    if (!g_snapshot_valid) return after_y;
    const Vec3 start_rel = VSub(start_pos, g_ref_origin);
    Vec3 foot_local{};
    for (const SnapSphere& s : g_snap_spheres) {
        if (s.is_foot) {
            foot_local = s.local_pos;
            break;
        }
    }
    const Vec3 foot_rel = VAdd(start_rel, foot_local);
    float best_crest_y = after_y;

    for (const SnapTri& t : g_snap_tris) {
        const float world_max_y = t.max_y + g_ref_origin.y;
        const float world_min_y = t.min_y + g_ref_origin.y;
        if (world_max_y <= after_y + 2.0f || world_max_y > max_crest_world_y) continue;
        if (world_min_y > after_y + g_foot_radius * 1.15f) continue;

        const Vec3 cp = ClosestPointOnTriangle(foot_rel, t.v0, t.v1, t.v2);
        const float dx = cp.x - start_rel.x;
        const float dz = cp.z - start_rel.z;
        const float fwd = dx * dir_x + dz * dir_z;
        const float lat = std::fabs(-dx * dir_z + dz * dir_x);
        if (fwd >= -8.0f && fwd <= g_foot_radius * 1.35f && lat <= g_foot_radius * 0.95f) {
            best_crest_y = std::max(best_crest_y, world_max_y);
        }
    }
    return best_crest_y;
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

struct ScopedActiveTorque {
    ScopedActiveTorque(uint32_t torque_va, uint32_t col_va) {
        g_active_torque_va = torque_va;
        g_active_collider_va = col_va;
    }
    ~ScopedActiveTorque() {
        g_active_torque_va = 0u;
        g_active_collider_va = 0u;
    }
};

struct ScopedGuestFloatRestore {
    uint8_t* base = nullptr;
    uint32_t va = 0u;
    float saved = 0.0f;
    bool active = false;

    ~ScopedGuestFloatRestore() {
        if (active && base != nullptr && ValidGuest(va, 4u)) {
            X86_MEMF32(va) = saved;
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// 0A. IsWalkableOrStepTriangle (__cdecl, ret 0: 9 stack args -> eax = 0 or 1)
//     Fixes two native XBE bugs in sub_00179D60 (0x00179D60..0x00179E1E):
//      1. Inverted 400.0f slope-length branch at 0x00179E00..0x00179E0E (`fcomp [0x002A451C];
//         fnstsw ax; test ah, 0x41; jnz return_0`), which rejected EVERY staircase ramp with
//         slope_len <= 400.0f (causing Torque's feet to clip straight through Docks & Quarry stairs)
//         while accepting huge >400 rock cliffs.
//      2. CGroundSupport::AdvanceOnMesh (0x0017A81C) passing gs->hit_info.bStepUpSlope ([gs+0x68] == 0
//         when standing on a flat landing) as allow_step, dropping CGroundSupport on welded landing->ramp seams.
// ---------------------------------------------------------------------------
X86_FUNC(sub_00179D60) {
    if (suffering::IsPadOnlyInputSession() || g_active_torque_va == 0u ||
        !ValidGuest(ctx.esp + 0x04u, 0x24u)) {
        __imp__sub_00179D60(ctx, base);
        return;
    }

    const float ny = X86_MEMF32(ctx.esp + 0x04u);
    const uint32_t v0_va = X86_MEM32(ctx.esp + 0x08u);
    const uint32_t v1_va = X86_MEM32(ctx.esp + 0x0Cu);
    const uint32_t v2_va = X86_MEM32(ctx.esp + 0x10u);
    const uint32_t owner_obj_va = X86_MEM32(ctx.esp + 0x14u);
    const uint32_t allow_step_arg = X86_MEM32(ctx.esp + 0x18u);
    const float min_walk_ny = X86_MEMF32(ctx.esp + 0x1Cu);
    const float min_step_ny = X86_MEMF32(ctx.esp + 0x20u);
    const uint32_t verts_already_world = X86_MEM32(ctx.esp + 0x24u);

    if (!std::isfinite(ny) || !std::isfinite(min_walk_ny) || !std::isfinite(min_step_ny)) {
        __imp__sub_00179D60(ctx, base);
        return;
    }

    if (ny > min_walk_ny) {
        ctx.eax = 1u;
        ctx.esp += 4u;
        return;
    }

    const bool step_enabled =
        (allow_step_arg != 0u) ||
        (ValidGuest(g_active_collider_va, 4u) && (X86_MEM32(g_active_collider_va) & 0x08u) != 0u);
    const float eff_min_step_ny = std::max(min_step_ny, kMinStairRampNormalY);
    if (!step_enabled || !(ny > eff_min_step_ny)) {
        ctx.eax = 0u;
        ctx.esp += 4u;
        return;
    }

    if (!ValidGuest(v0_va, 12u) || !ValidGuest(v1_va, 12u) || !ValidGuest(v2_va, 12u)) {
        __imp__sub_00179D60(ctx, base);
        return;
    }

    const Vec3 p0 = ReadVec3(base, v0_va);
    const Vec3 p1 = ReadVec3(base, v1_va);
    const Vec3 p2 = ReadVec3(base, v2_va);
    if (!IsFiniteVec3(p0) || !IsFiniteVec3(p1) || !IsFiniteVec3(p2)) {
        ctx.eax = 0u;
        ctx.esp += 4u;
        return;
    }

    float y0 = p0.y;
    float y1 = p1.y;
    float y2 = p2.y;
    if (verts_already_world == 0u && owner_obj_va != 0u) {
        if (!ValidGuest(owner_obj_va + 0x14u, 0x2Cu)) {
            __imp__sub_00179D60(ctx, base);
            return;
        }
        // Matches CMatrix4x3::RotateVector (sub_00018690 @ 0x000186AE..0x000186C3)
        const float m01 = X86_MEMF32(owner_obj_va + 0x18u);
        const float m11 = X86_MEMF32(owner_obj_va + 0x28u);
        const float m21 = X86_MEMF32(owner_obj_va + 0x38u);
        y0 = m01 * p0.x + m11 * p0.y + m21 * p0.z;
        y1 = m01 * p1.x + m11 * p1.y + m21 * p1.z;
        y2 = m01 * p2.x + m11 * p2.y + m21 * p2.z;
    }

    const float span_y = std::max({y0, y1, y2}) - std::min({y0, y1, y2});
    const float max_span = (ny >= 0.60f) ? kMaxModerateStairSpanY : kMaxSteepStairSpanY;
    ctx.eax = (std::isfinite(span_y) && span_y >= 0.0f && span_y < max_span) ? 1u : 0u;
    ctx.esp += 4u;
}

// ---------------------------------------------------------------------------
// 0B. CCollider::ShouldSkipStepTriangle (__thiscall, ret 0x04: uint32_t packed_tri_id -> eax = skip)
//     On long Docks wooden staircase flights (span_y = 400..1301.3, e.g. id0=593, 602, 354..356, 544,
//     586..591, 60, 13/548), tri_centroid_y exceeds foot_center_y across the lower two-thirds of the
//     flight, and STriEntry.normal (+0x2C) is not yet populated at 0x00174152. Native sub_001709C0
//     returned 0, adding the staircase ramp itself as a solid wall to [foot_sphere+0x24] while ON_GROUND
//     zeroed desired_delta.y and slide.y. Skipping solid foot-wall registration when Torque is on the
//     front/top side of a reachable stair ramp lets GroundSnap (sub_00172C00) climb cleanly while
//     preserving upper-body sphere & underside collision.
// ---------------------------------------------------------------------------
X86_FUNC(sub_001709C0) {
    if (suffering::IsPadOnlyInputSession() || ctx.ecx == 0u || ctx.ecx != g_active_collider_va ||
        !ValidGuest(ctx.ecx, 0x19Eu) || !ValidGuest(ctx.esp + 0x04u, 4u)) {
        __imp__sub_001709C0(ctx, base);
        return;
    }

    const uint32_t col_va = ctx.ecx;
    if ((X86_MEM8(col_va) & 0x08u) == 0u) {
        ctx.eax = 0u;
        ctx.esp += 8u;
        return;
    }

    const uint32_t packed = X86_MEM32(ctx.esp + 0x04u);
    const int32_t pool_idx = static_cast<int32_t>(packed) >> 16;
    const int32_t tri_idx = static_cast<int32_t>(packed) - (pool_idx << 16);
    if (pool_idx != 0 || tri_idx < 0) {
        // Preserve native terrain-pool handling (pools 1 & 2)
        __imp__sub_001709C0(ctx, base);
        return;
    }

    const uint32_t pool_arr_va = X86_MEM32(col_va + 0x18Au);
    if (!ValidGuest(pool_arr_va, 0x10u)) {
        __imp__sub_001709C0(ctx, base);
        return;
    }
    const uint32_t data_va = X86_MEM32(pool_arr_va + 0x00u);
    const int32_t count = static_cast<int32_t>(X86_MEM32(pool_arr_va + 0x08u));
    const uint32_t is_chunked = X86_MEM32(pool_arr_va + 0x0Cu);
    if (is_chunked != 0u || data_va == 0u || tri_idx >= count) {
        __imp__sub_001709C0(ctx, base);
        return;
    }

    const uint32_t t_va = data_va + static_cast<uint32_t>(tri_idx) * 0x40u;
    if (!ValidGuest(t_va, 0x40u)) {
        __imp__sub_001709C0(ctx, base);
        return;
    }

    const uint32_t t_flags = X86_MEM32(t_va + 0x00u);
    const Vec3 v0 = ReadVec3(base, t_va + 0x08u);
    const Vec3 v1 = ReadVec3(base, t_va + 0x14u);
    const Vec3 v2 = ReadVec3(base, t_va + 0x20u);
    if (!IsFiniteVec3(v0) || !IsFiniteVec3(v1) || !IsFiniteVec3(v2)) {
        __imp__sub_001709C0(ctx, base);
        return;
    }

    Vec3 n{};
    if ((t_flags & 0x02u) != 0u) {
        n = ReadVec3(base, t_va + 0x2Cu);
    } else {
        const Vec3 raw_n = VCross(VSub(v2, v0), VSub(v1, v0));
        const float nl = VLen(raw_n);
        if (nl > 1e-5f) {
            n = VScale(raw_n, 1.0f / nl);
        }
    }
    if (!IsFiniteVec3(n) || VLenSq(n) < 0.5f) {
        __imp__sub_001709C0(ctx, base);
        return;
    }

    const float min_y = std::min({v0.y, v1.y, v2.y});
    const float max_y = std::max({v0.y, v1.y, v2.y});
    const float span_y = max_y - min_y;
    if (!IsValidStairRamp(n.y, span_y)) {
        __imp__sub_001709C0(ctx, base);
        return;
    }

    const int32_t foot_bone_id = static_cast<int32_t>(X86_MEM32(col_va + 0x0Cu));
    const uint32_t spheres_va = X86_MEM32(col_va + 0x122u);
    const int32_t sphere_count = static_cast<int32_t>(X86_MEM32(col_va + 0x12Au));
    if (sphere_count <= 0 || sphere_count > 32 ||
        !ValidGuest(spheres_va, static_cast<uint32_t>(sphere_count) * 0x3Cu)) {
        __imp__sub_001709C0(ctx, base);
        return;
    }

    uint32_t foot_s_va = 0u;
    for (int32_t i = 0; i < sphere_count; ++i) {
        const uint32_t s_va = spheres_va + static_cast<uint32_t>(i) * 0x3Cu;
        if (static_cast<int32_t>(X86_MEM32(s_va + 0x04u)) == foot_bone_id) {
            foot_s_va = s_va;
            break;
        }
    }
    if (foot_s_va == 0u) {
        foot_s_va = spheres_va;
    }

    const float foot_r = std::clamp(X86_MEMF32(foot_s_va + 0x08u), 30.0f, 250.0f);
    const Vec3 col_rel_pos = ReadVec3(base, col_va + 0xCCu);
    const Vec3 foot_local = ReadVec3(base, foot_s_va + 0x0Cu);
    const Vec3 foot_center_rel = VAdd(col_rel_pos, foot_local);
    if (!IsFiniteVec3(foot_center_rel)) {
        __imp__sub_001709C0(ctx, base);
        return;
    }

    const float centroid_y = (v0.y + v1.y + v2.y) * (1.0f / 3.0f);
    const float signed_plane_d = VDot(VSub(foot_center_rel, v0), n);
    const bool front_or_top_side = (signed_plane_d >= -foot_r * 0.35f);
    const bool reachable_base = (min_y <= foot_center_rel.y + std::max(foot_r * 0.65f, 64.0f));

    ctx.eax = (centroid_y < foot_center_rel.y || (front_or_top_side && reachable_base)) ? 1u : 0u;
    ctx.esp += 8u;
}

// ---------------------------------------------------------------------------
// 1. CCollider::SweepStep (__thiscall, ret 0: ecx = CCollider* -> eax = continue_loop)
//    - Snapshots gathered CTriPool triangles & sphere offsets on iteration 0 for CTorque's collider.
//    - Ensures at least 12 slide iterations so high-FPS micro-steps never hit 0x00174370 movement cancel.
//    - Fixes 0x0017447F -> 0x001744B4 post-collision teleport bug: after a sphere collision (safe_steps >= 0),
//      when |remaining_delta|^2 < threshold, stops cleanly at the last verified collision-free substep
//      (0x001744D5) instead of blindly adding the unswept post-collision slide vector into solid geometry.
// ---------------------------------------------------------------------------
X86_FUNC(sub_00174340) {
    if (suffering::IsPadOnlyInputSession() || ctx.ecx == 0u || ctx.ecx != g_active_collider_va ||
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
    if (suffering::IsPadOnlyInputSession() || ctx.ecx == 0u || ctx.ecx != g_active_collider_va ||
        !ValidGuest(ctx.ecx, 0x19Eu)) {
        __imp__sub_00174530(ctx, base);
        return;
    }

    const uint32_t col_va = ctx.ecx;
    const float saved_substep = X86_MEMF32(col_va + 0x40u);
    const float desired_dist = X86_MEMF32(col_va + 0x38u);
    ScopedGuestFloatRestore restore{base, col_va + 0x40u, saved_substep, false};

    if (std::isfinite(saved_substep) && saved_substep > 0.25f &&
        std::isfinite(desired_dist) && desired_dist > 1e-4f) {
        const float target_substep = std::clamp(std::min(saved_substep, desired_dist * 0.125f), 0.25f, saved_substep);
        if (target_substep < saved_substep) {
            X86_MEMF32(col_va + 0x40u) = target_substep;
            restore.active = true;
        }
    }

    __imp__sub_00174530(ctx, base);
}

// ---------------------------------------------------------------------------
// 3. CTorque::TryLandOnMeshGround (__thiscall, ret 0: ecx = CTorque*)
//    Allows landing onto verified staircase slopes (ny >= 0.54f -> bStepUpSlope == 1 at [col+0x14A])
//    so Docks & Quarry 40..57 deg staircases do not leave Torque stuck in State 9 (Fall).
// ---------------------------------------------------------------------------
X86_FUNC(sub_0003CE90) {
    if (suffering::IsPadOnlyInputSession() || ctx.ecx == 0u || ctx.ecx != g_active_torque_va ||
        !ValidGuest(ctx.ecx, 0x790u)) {
        __imp__sub_0003CE90(ctx, base);
        return;
    }

    const uint32_t torque_va = ctx.ecx;
    const uint32_t col_va = X86_MEM32(torque_va + 0xD0u);
    const uint32_t gs_va = X86_MEM32(torque_va + 0xD4u);
    ScopedGuestFloatRestore restore{base, gs_va + 0x0Cu, kMinWalkableNormalY, false};

    if (ValidGuest(col_va, 0x180u) && ValidGuest(gs_va, 0xA4u) && X86_MEM32(col_va + 0x14Au) != 0u) {
        const float saved_min_ny = X86_MEMF32(gs_va + 0x0Cu);
        if (std::isfinite(saved_min_ny) && saved_min_ny > kMinStairRampNormalY) {
            restore.saved = saved_min_ny;
            restore.active = true;
            X86_MEMF32(gs_va + 0x0Cu) = kMinStairRampNormalY;
        }
    }

    __imp__sub_0003CE90(ctx, base);
}

// ---------------------------------------------------------------------------
// 4. CTorque::PostMoveCollisionTick (__thiscall, ret 0: ecx = CTorque*)
//    Orchestrates:
//     A. Enabling CCollider bit 0x08 (STEP_UP_ENABLED) & restoring 0x8000 before GatherCollisionTriangles.
//     B. Unwelded staircase seam ground-bridging when sub_0003D760 drops support across unwelded .mdu tris.
//     C. Smooth, framerate-independent staircase top-edge / header-trim step-up assist with narrow-railing
//        deflection and full all-sphere clearance verification.
//     D. Staircase V-crease / railing anti-wedge recovery in both State 9 (Fall) and ON_GROUND pinches.
// ---------------------------------------------------------------------------
X86_FUNC(sub_0005BF60) {
    if (suffering::IsPadOnlyInputSession() || !ValidGuest(ctx.ecx, 0x790u) ||
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

    g_active_sweep_had_hit = false;
    g_active_sweep_hard_stopped = false;
    g_snapshot_valid = false;
    {
        const ScopedActiveTorque active_scope(torque_va, col_va);
        __imp__sub_0005BF60(ctx, base);
    }

    if (!g_snapshot_valid || !ValidGuest(obj_va, 0xF0u) || !ValidGuest(col_va, 0x19Eu) || !ValidGuest(gs_va, 0xA4u)) {
        return;
    }

    Vec3 after_pos = ReadVec3(base, obj_va + 0x44u);
    if (!IsFiniteVec3(after_pos)) return;

    const float step_height = QueryTorqueStepHeight(ctx, base, obj_va);
    const float effective_step_h = std::max(step_height, 172.0f);
    uint32_t phys_flags_after = X86_MEM32(torque_va + 0x98u);
    uint32_t state_bits_after = X86_MEM32(torque_va + 0xA4u);
    bool is_on_ground_after = (phys_flags_after & 0x01u) != 0u;

    // -----------------------------------------------------------------------
    // Pillar 2: Unwelded Staircase Seam Ground-Bridge
    // When walking across an unwelded .mdu seam (0 shared vertex indices in sub_0017A400, e.g. Docks
    // id0=593 switchback or Quarry sub=401) high above .wdu terrain, sub_0003D760 drops CGroundSupport
    // and forces State 9 (Fall, ON_GROUND = 0). Re-bridge support if a valid floor/stair triangle lies beneath.
    // -----------------------------------------------------------------------
    if (was_on_ground && !was_jumping && vel_y_before <= 5.0f && !is_on_ground_after &&
        (state_bits_after & (1u << 9)) != 0u) {
        float seam_y = 0.0f;
        const float max_down = std::min(effective_step_h, 165.0f);
        const float max_up = std::min(effective_step_h * 0.65f, 96.0f);
        const SnapTri* seam_tri = FindBestSupportTriangleAt(
            after_pos.x, after_pos.z, after_pos.y - max_down, after_pos.y + max_up,
            after_pos.y, /*allow_stair_ramps=*/true, /*edge_eps=*/8.0f, seam_y);

        if (seam_tri != nullptr) {
            const Vec3 bridged_pos = {after_pos.x, seam_y, after_pos.z};
            if (IsPoseClearOfObstacles(bridged_pos, seam_y, /*radius_shrink=*/2.5f, &after_pos)) {
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
    // Pillar 3: Smooth, Framerate-Independent Staircase Top-Edge / Header-Trim Step-Up Assist
    // Handles cases where Torque's foot/shin sphere rim hits a staircase top-edge lip or raised wooden
    // header trim plank while ON_GROUND zeroes slide.y, including slight inward yaw deflection when
    // grazing narrow Docks side handrails/baluster posts.
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
            const float max_land_y = start_pos.y + effective_step_h;
            const float pref_land_y = g_climb.active ? g_climb.target_land_y : (after_pos.y + 24.0f);

            // Probe from Torque's desired step out past the foot sphere rim & wooden landing header planks,
            // testing 0 deg first and small +/-18 deg, +/-36 deg inward deflections if rubbing a side railing/post.
            const std::array<float, 6> probe_dists = {
                desired_horiz,
                std::max(desired_horiz, g_foot_radius * 0.35f),
                std::max(desired_horiz, g_foot_radius * 0.65f),
                std::max(desired_horiz, g_foot_radius * 0.90f),
                std::max(desired_horiz, g_foot_radius * 1.15f),
                std::max(desired_horiz, g_foot_radius * 1.40f),
            };
            constexpr std::array<std::pair<float, float>, 5> kYawDeflections = {{
                {1.0f, 0.0f},          //   0 deg
                {0.95106f, 0.30902f},  // +18 deg
                {0.95106f, -0.30902f}, // -18 deg
                {0.80902f, 0.58779f},  // +36 deg
                {0.80902f, -0.58779f}, // -36 deg
            }};

            const SnapTri* best_land_tri = nullptr;
            float best_land_y = 0.0f;
            float best_clearance_y = 0.0f;
            float best_probe_d = 0.0f;
            float best_dir_x = dir_x;
            float best_dir_z = dir_z;

            for (const auto& [cos_a, sin_a] : kYawDeflections) {
                const float pdir_x = dir_x * cos_a - dir_z * sin_a;
                const float pdir_z = dir_x * sin_a + dir_z * cos_a;
                const float crest_y =
                    FindForwardLowStepCrestY(start_pos, after_pos.y, max_land_y, pdir_x, pdir_z);
                const bool has_low_crest = (crest_y > after_pos.y + 2.0f);
                const float min_land_y =
                    g_climb.active
                        ? std::min(after_pos.y - 8.0f, g_climb.target_land_y - 8.0f)
                        : (has_low_crest ? (after_pos.y - 16.0f) : (after_pos.y + 2.0f));
                if (max_land_y <= min_land_y) continue;

                for (float pd : probe_dists) {
                    const float px = start_pos.x + pdir_x * pd;
                    const float pz = start_pos.z + pdir_z * pd;
                    float cand_land_y = 0.0f;
                    const SnapTri* tri = FindBestSupportTriangleAt(
                        px, pz, min_land_y, max_land_y, pref_land_y,
                        /*allow_stair_ramps=*/true, /*edge_eps=*/3.5f, cand_land_y);
                    if (tri == nullptr) continue;

                    const float clearance_y = std::max(cand_land_y, crest_y);
                    if (clearance_y <= after_pos.y + 1.5f && !g_climb.active) continue;

                    // Verify full 3D clearance across ALL Torque spheres:
                    //  (1) Upward headroom at start_pos lifted to clearance_y
                    //  (2) Open space on the target landing itself (rejects railings, fences, closed doors, walls!)
                    const Vec3 up_pos = {start_pos.x, clearance_y + 2.0f, start_pos.z};
                    const Vec3 land_probe_pos = {px, clearance_y + 2.0f, pz};
                    if (!IsPoseClearOfObstacles(up_pos, clearance_y, 1.5f, &start_pos) ||
                        !IsPoseClearOfObstacles(land_probe_pos, clearance_y, 1.5f, &start_pos)) {
                        continue;
                    }

                    best_land_tri = tri;
                    best_land_y = cand_land_y;
                    best_clearance_y = clearance_y;
                    best_probe_d = pd;
                    best_dir_x = pdir_x;
                    best_dir_z = pdir_z;
                    break;
                }
                if (best_land_tri != nullptr) break;
            }

            if (best_land_tri != nullptr) {
                const float move_scale =
                    desired_horiz * std::max(dir_x * best_dir_x + dir_z * best_dir_z, 0.55f);
                const float next_x = start_pos.x + best_dir_x * move_scale;
                const float next_z = start_pos.z + best_dir_z * move_scale;

                // Check if (next_x, next_z) is already directly over the target landing triangle AND clear
                // of any raised landing-edge header trim at snapped_y; if a trim plank still overlaps at
                // snapped_y, fall through to the progressive lift branch so Torque steps cleanly over it.
                bool landed_directly = false;
                float direct_y = 0.0f;
                const bool over_landing = PointInTriXZ(
                    next_x - g_ref_origin.x, next_z - g_ref_origin.z, *best_land_tri, 3.5f, direct_y);

                if (over_landing) {
                    const float snapped_y = direct_y + g_ref_origin.y;
                    const Vec3 final_pos = {next_x, snapped_y, next_z};
                    if (IsPoseClearOfObstacles(final_pos, snapped_y, 1.5f, &start_pos)) {
                        ApplyTorquePositionAdjustment(ctx, base, torque_va, obj_va, col_va, after_pos, final_pos);
                        AttachTorqueGroundSupport(ctx, base, torque_va, col_va, gs_va, best_land_tri);
                        after_pos = final_pos;
                        is_on_ground_after = true;
                        g_climb.active = false;
                        landed_directly = true;
                    }
                }

                if (!landed_directly && best_probe_d <= g_foot_radius * 1.45f) {
                    // Progressive framerate-independent ramp lift (desired_horiz == speed * dt) across the lip/trim
                    const float climb_step = std::max(desired_horiz * 1.35f, 2.5f);
                    const float next_y = std::min(best_clearance_y, base_y + climb_step);
                    const Vec3 check_lifted = {next_x, best_clearance_y + 2.0f, next_z};
                    const Vec3 step_pos = {next_x, next_y, next_z};

                    if (IsPoseClearOfObstacles(check_lifted, best_clearance_y, 1.5f, &start_pos)) {
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
        } else if (actual_horiz >= desired_horiz * 0.75f) {
            g_climb.active = false;
        }
    } else if (g_climb.active) {
        if (++g_climb.idle_frames > 3) {
            g_climb.active = false;
        }
    }

    // -----------------------------------------------------------------------
    // Pillar 4: Staircase V-Crease & Narrow-Railing Pinch Anti-Wedge Safety
    // Rescues Torque if wedged either:
    //  (a) in State 9 (Fall, ON_GROUND == 0, Jump disabled) with near-zero displacement for >= 0.12s, OR
    //  (b) ON_GROUND while actively moving (desired_horiz > 1.0f) with near-zero displacement for >= 0.30s
    //      inside a verified opposing-normal V-pinch (dot < -0.45f) or deep solid sphere embed (< 0.78*R)
    //      (never triggers against flat walls, locked doors, or normal 90-deg corners).
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

    const bool pinned_on_ground_wedge =
        is_on_ground_after &&
        !was_jumping &&
        desired_horiz > 1.0f &&
        g_active_sweep_had_hit &&
        moved_3d < std::max(1.0f, desired_horiz * 0.08f) &&
        IsGeometricallyPinchedAt(after_pos);

    if (!pinned_in_fall && !pinned_on_ground_wedge) {
        g_wedge.frames = 0;
        g_wedge.accum_s = 0.0f;
    } else {
        if (g_wedge.frames == 0) {
            g_wedge.anchor_pos = after_pos;
        }
        ++g_wedge.frames;
        g_wedge.accum_s += dt;

        const float max_anchor_drift = pinned_in_fall ? (g_foot_radius * 0.50f) : (g_foot_radius * 0.35f);
        const int req_frames = pinned_in_fall ? 5 : 15;
        const float req_seconds = pinned_in_fall ? 0.12f : 0.30f;

        const float drift_from_anchor = VLen(VSub(after_pos, g_wedge.anchor_pos));
        if (drift_from_anchor > max_anchor_drift) {
            g_wedge.anchor_pos = after_pos;
            g_wedge.frames = 1;
            g_wedge.accum_s = dt;
        } else if (g_wedge.frames >= req_frames && g_wedge.accum_s >= req_seconds) {
            // Accumulate outward depenetration direction from all nearby / contacting obstacle triangles
            Vec3 push_dir = {0.0f, 1.0f, 0.0f};
            const Vec3 after_rel_base = VSub(after_pos, g_ref_origin);
            for (const SnapSphere& s : g_snap_spheres) {
                const Vec3 c_rel = VAdd(after_rel_base, s.local_pos);
                const float probe_r = s.radius + 8.0f;
                const float probe_r_sq = probe_r * probe_r;
                for (const SnapTri& t : g_snap_tris) {
                    if (s.is_foot && (t.is_walkable_floor || t.is_stair_ramp)) continue;
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

            const float push_horiz = std::hypot(push_dir.x, push_dir.z);
            const float nx = (push_horiz > 1e-3f) ? (push_dir.x / push_horiz) : (desired_horiz > 0.05f ? (desired_dx / desired_horiz) : 1.0f);
            const float nz = (push_horiz > 1e-3f) ? (push_dir.z / push_horiz) : (desired_horiz > 0.05f ? (desired_dz / desired_horiz) : 0.0f);

            constexpr std::array<float, 4> kLiftHeights = {20.0f, 44.0f, 76.0f, 116.0f};
            constexpr std::array<float, 5> kHorizRadii = {0.0f, 14.0f, 30.0f, 52.0f, 76.0f};
            constexpr std::array<std::pair<float, float>, 8> kDirs = {{
                {1.0f, 0.0f},
                {0.7071f, 0.7071f},
                {0.7071f, -0.7071f},
                {0.0f, 1.0f},
                {0.0f, -1.0f},
                {-0.7071f, 0.7071f},
                {-0.7071f, -0.7071f},
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
                            cand.y, /*allow_stair_ramps=*/true, /*edge_eps=*/5.0f, support_y);

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
