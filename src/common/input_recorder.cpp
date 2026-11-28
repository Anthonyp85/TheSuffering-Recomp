// SPDX-License-Identifier: GPL-2.0-or-later
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "common/input_recorder.h"
#include "common/mkb_input.h"
#include "common/settings_ui.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
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

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "core/perf.h"
#include "gpu/backend.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "upscale/upscaler_manager.h"

namespace suffering {

namespace {

std::atomic<bool> g_recording_active{false};
// Keep the input contract until shutdown, even after WM_CLOSE finalizes the file.
std::atomic<bool> g_pad_only_recording_session{false};

// Verified read-only guest .data addresses for The Suffering (USA, Title ID 0x4D570015, XDK 5788.1)
constexpr uint32_t kVaD3dDeviceSwapCount = 0x00216880u + 0x2478u;  // D3D_Device + CDevice_SwapCount
constexpr uint32_t kVaGameSessionState = 0x002DB204u;              // 1=Title/Init, 2=In-Game, 3=LeaveGame
constexpr uint32_t kVaEngineModeFlags = 0x002DC5E4u;               // bit0=UI, bit2=world active, bit3=FMV
constexpr uint32_t kVaCameraPtr = 0x002DC754u;                     // *(u32*) -> +0x00..+0x3F matrix, +0x8C/+0x90/+0x94 (x,y,z)
constexpr uint32_t kVaViewportMgrPtr = 0x002DC75Cu;                // *(u32*) -> +0x40 array, +0x4C idx, stride 0x2C0
constexpr uint32_t kVaLevelFlags = 0x002DEF14u;                    // bit1=level loaded
constexpr uint32_t kVaCurrentLevelPath = 0x002DEF40u;              // char[260]
constexpr uint32_t kVaDeltaTime = 0x002E01D0u;                     // float g_dt
constexpr uint32_t kVaPauseFlags = 0x002E0274u;                    // bit0=paused
constexpr uint32_t kVaFrameCount = 0x002E0280u;                    // uint32_t g_FrameCount
constexpr uint32_t kVaElapsedGameTime = 0x002E0290u;               // float g_ElapsedGameTime

constexpr uint64_t kMaxEffStepUs = 100000ull;  // 100 ms cap per poll absorbs cold-Z: / LoadLevel stalls
constexpr uint64_t kSettlePollsAfterReplay = 30ull;

enum class ReplayMode { Hybrid, Frame, Time };

struct AutoAction {
    uint64_t start_ms = 0;
    uint64_t end_ms = 0;
    std::string token;
};

struct RecSample {
    uint64_t frame_id = 0;
    uint64_t timestamp_us = 0;
    uint64_t eff_us = 0;
    uint32_t swap_frame = 0;
    uint32_t guest_frame = 0;
    input::Pad pad{};
};

struct GuestState {
    uint32_t swap_frame = 0;
    uint32_t guest_frame = 0;
    uint32_t session = 0;
    uint32_t mode = 0;
    uint32_t pause = 0;
    uint32_t lvl_flags = 0;
    float gtime = 0.0f;
    float dt = 0.0f;
    bool cam_valid = false;
    float cam[3] = {0.0f, 0.0f, 0.0f};
    std::string level;
};

struct RecorderState {
    std::mutex lock;
    bool initialized = false;
    bool finalized = false;

    // Configuration
    std::string record_path;
    std::string replay_path;
    std::string state_log_path;
    std::string autotest_path;
    ReplayMode replay_mode = ReplayMode::Hybrid;
    bool replay_exit = false;

    // Runtime streams
    std::FILE* rec_file = nullptr;
    std::FILE* state_file = nullptr;

    // Scripted autotest
    bool autotest_enabled = false;
    std::vector<AutoAction> auto_actions;

    // Loaded replay stream
    bool replay_enabled = false;
    std::vector<RecSample> replay_samples;
    size_t replay_cursor = 0;
    uint64_t rec_total_polls = 0;
    uint64_t rec_duration_us = 0;
    uint64_t rec_eff_duration_us = 0;
    bool replay_finished = false;
    uint64_t replay_done_poll = 0;
    bool exit_posted = false;

    // Guest poll anchor & counters (guest thread 14 only)
    bool anchor_set = false;
    std::chrono::steady_clock::time_point t0{};
    std::chrono::steady_clock::time_point last_poll_tp{};
    uint64_t poll_id = 0;
    uint64_t last_raw_us = 0;
    uint64_t eff_us = 0;

    // Delta-tracking for recording & state transitions
    bool has_last_rec_pad = false;
    input::Pad last_rec_pad{};
    bool has_last_state = false;
    uint32_t last_session = 0;
    uint32_t last_pause = 0;
    uint32_t last_lvl_loaded = 0;
    std::string last_level;

    // Rumble delta tracking
    bool has_last_rumble = false;
    uint16_t last_rumble_l = 0;
    uint16_t last_rumble_r = 0;
    uint64_t rumble_events = 0;

    bool HasVirtualPad() const { return replay_enabled || autotest_enabled; }
    bool IsActive() const { return rec_file != nullptr || replay_enabled || autotest_enabled || state_file != nullptr; }
};

RecorderState& State() {
    static RecorderState st;
    return st;
}

#ifdef _WIN32
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring StripQuotes(std::wstring s) {
    while (!s.empty() && std::iswspace(s.front())) s.erase(s.begin());
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}

std::vector<std::wstring> SplitCommandLine(const wchar_t* cmd) {
    std::vector<std::wstring> args;
    if (!cmd) return args;
    const wchar_t* p = cmd;
    while (*p) {
        while (*p && std::iswspace(*p)) ++p;
        if (!*p) break;
        std::wstring arg;
        bool in_quotes = false;
        while (*p && (in_quotes || !std::iswspace(*p))) {
            if (*p == L'"') in_quotes = !in_quotes;
            else arg.push_back(*p);
            ++p;
        }
        args.push_back(std::move(arg));
    }
    return args;
}

std::string GetEnvUtf8(const wchar_t* name) {
    const wchar_t* v = _wgetenv(name);
    if (!v || !*v) return {};
    return WideToUtf8(StripQuotes(v));
}
#else
std::string StripQuotesUtf8(std::string s) {
    auto is_sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && is_sp(s.front())) s.erase(s.begin());
    while (!s.empty() && is_sp(s.back())) s.pop_back();
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
    return s;
}

std::vector<std::string> ReadProcCmdline() {
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

std::string GetEnvUtf8(const char* name) {
    const char* v = std::getenv(name);
    if (!v || !*v) return {};
    return StripQuotesUtf8(v);
}
#endif

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '"') out += "\\\"";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 0x20 && c < 0x7F) out.push_back(char(c));
    }
    return out;
}

bool PadsEqual(const input::Pad& a, const input::Pad& b) {
    return a.buttons == b.buttons && std::memcmp(a.analog, b.analog, sizeof(a.analog)) == 0 && a.lx == b.lx &&
           a.ly == b.ly && a.rx == b.rx && a.ry == b.ry;
}

const char* ModeName(ReplayMode m) {
    switch (m) {
        case ReplayMode::Frame: return "frame";
        case ReplayMode::Time: return "time";
        case ReplayMode::Hybrid: return "hybrid";
    }
    return "hybrid";
}

bool SafeReadU32(uint32_t va, uint32_t& out) {
    if (!kernel::g_base || va < mem::kUserLow || va + 4 > mem::kUserHigh) return false;
    if (!mem::IsCommitted(va) || !mem::IsCommitted(va + 3)) return false;
    out = kernel::R32(va);
    return true;
}

bool SafeReadF32(uint32_t va, float& out) {
    uint32_t raw = 0;
    if (!SafeReadU32(va, raw)) return false;
    std::memcpy(&out, &raw, 4);
    if (!std::isfinite(out)) out = 0.0f;
    return true;
}

GuestState ReadGuestState() {
    GuestState gs;
    SafeReadU32(kVaD3dDeviceSwapCount, gs.swap_frame);
    SafeReadU32(kVaFrameCount, gs.guest_frame);
    SafeReadU32(kVaGameSessionState, gs.session);
    SafeReadU32(kVaEngineModeFlags, gs.mode);
    uint32_t pause_raw = 0;
    if (SafeReadU32(kVaPauseFlags, pause_raw)) gs.pause = pause_raw & 1u;
    SafeReadU32(kVaLevelFlags, gs.lvl_flags);
    SafeReadF32(kVaElapsedGameTime, gs.gtime);
    SafeReadF32(kVaDeltaTime, gs.dt);

    if (kernel::g_base && mem::IsCommitted(kVaCurrentLevelPath) && mem::IsCommitted(kVaCurrentLevelPath + 259)) {
        const char* p = static_cast<const char*>(kernel::HostPtr(kVaCurrentLevelPath));
        size_t len = 0;
        while (len < 259 && p[len] != '\0') ++len;
        gs.level.assign(p, len);
    }

    uint32_t vp = 0;
    if (SafeReadU32(kVaCameraPtr, vp) && vp >= mem::kUserLow && vp + 0x98 <= mem::kUserHigh &&
        mem::IsCommitted(vp) && mem::IsCommitted(vp + 0x98)) {
        float x = 0, y = 0, z = 0;
        if (SafeReadF32(vp + 0x8Cu, x) && SafeReadF32(vp + 0x90u, y) && SafeReadF32(vp + 0x94u, z)) {
            gs.cam_valid = true;
            gs.cam[0] = x;
            gs.cam[1] = y;
            gs.cam[2] = z;
        }
    }
    return gs;
}

void ApplyToken(input::Pad& pad, const std::string& tok) {
    // Support parameterized tokens: LT:100, RT:255, A:128, LX:-26000, LY:6000, RX:22000, RY:16000
    auto colon = tok.find(':');
    if (colon != std::string::npos) {
        std::string key = tok.substr(0, colon);
        int val = std::atoi(tok.substr(colon + 1).c_str());
        uint8_t u8 = uint8_t(std::clamp(val, 0, 255));
        int16_t i16 = int16_t(std::clamp(val, -32768, 32767));
        if (key == "A") pad.analog[0] = u8;
        else if (key == "B") pad.analog[1] = u8;
        else if (key == "X") pad.analog[2] = u8;
        else if (key == "Y") pad.analog[3] = u8;
        else if (key == "BLACK") pad.analog[4] = u8;
        else if (key == "WHITE") pad.analog[5] = u8;
        else if (key == "LT") pad.analog[6] = u8;
        else if (key == "RT") pad.analog[7] = u8;
        else if (key == "LX") pad.lx = i16;
        else if (key == "LY") pad.ly = i16;
        else if (key == "RX") pad.rx = i16;
        else if (key == "RY") pad.ry = i16;
        return;
    }

    if (tok == "UP") pad.buttons |= 1u << 0;
    else if (tok == "DOWN") pad.buttons |= 1u << 1;
    else if (tok == "LEFT") pad.buttons |= 1u << 2;
    else if (tok == "RIGHT") pad.buttons |= 1u << 3;
    else if (tok == "START") pad.buttons |= 1u << 4;
    else if (tok == "BACK") pad.buttons |= 1u << 5;
    else if (tok == "LTHUMB") pad.buttons |= 1u << 6;
    else if (tok == "RTHUMB") pad.buttons |= 1u << 7;
    else if (tok == "A") pad.analog[0] = 0xFF;
    else if (tok == "B") pad.analog[1] = 0xFF;
    else if (tok == "X") pad.analog[2] = 0xFF;
    else if (tok == "Y") pad.analog[3] = 0xFF;
    else if (tok == "BLACK") pad.analog[4] = 0xFF;
    else if (tok == "WHITE") pad.analog[5] = 0xFF;
    else if (tok == "LT") pad.analog[6] = 0xFF;
    else if (tok == "RT") pad.analog[7] = 0xFF;
    else if (tok == "LX+") pad.lx = 26000;
    else if (tok == "LX-") pad.lx = -26000;
    else if (tok == "LY+") pad.ly = 26000;
    else if (tok == "LY-") pad.ly = -26000;
    else if (tok == "RX+") pad.rx = 22000;
    else if (tok == "RX-") pad.rx = -22000;
    else if (tok == "RY+") pad.ry = 18000;
    else if (tok == "RY-") pad.ry = -18000;
}

bool LoadRecFile(const std::string& path, RecorderState& st) {
    std::ifstream f(path);
    if (!f) {
        LOGE("input_recorder: cannot open replay file %s", path.c_str());
        return false;
    }
    st.replay_samples.clear();
    st.rec_total_polls = 0;
    st.rec_duration_us = 0;
    st.rec_eff_duration_us = 0;

    std::string line;
    bool header_ok = false;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        if (line.rfind("#TSREC", 0) == 0) {
            header_ok = true;
            continue;
        }
        if (line[0] == '#') continue;
        if (line[0] == 'I' && line.size() > 2 && line[1] == ' ') {
            RecSample s;
            unsigned long long fid = 0, tus = 0, eus = 0;
            unsigned btns = 0;
            unsigned a[8] = {};
            int lx = 0, ly = 0, rx = 0, ry = 0;
            int n = std::sscanf(line.c_str() + 2,
                                "%llu %llu %llu %u %u %x %u %u %u %u %u %u %u %u %d %d %d %d",
                                &fid, &tus, &eus, &s.swap_frame, &s.guest_frame, &btns,
                                &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6], &a[7], &lx, &ly, &rx, &ry);
            if (n == 18) {
                s.frame_id = uint64_t(fid);
                s.timestamp_us = uint64_t(tus);
                s.eff_us = uint64_t(eus);
                s.pad.connected = true;
                s.pad.buttons = uint16_t(btns);
                for (int i = 0; i < 8; i++) s.pad.analog[i] = uint8_t(a[i]);
                s.pad.lx = int16_t(lx);
                s.pad.ly = int16_t(ly);
                s.pad.rx = int16_t(rx);
                s.pad.ry = int16_t(ry);
                st.replay_samples.push_back(s);
            }
        } else if (line[0] == 'S' && line.size() > 2 && line[1] == ' ') {
            unsigned long long tp = 0, du = 0, eu = 0, re = 0;
            if (std::sscanf(line.c_str() + 2, "total_polls=%llu duration_us=%llu eff_duration_us=%llu rumble_events=%llu",
                            &tp, &du, &eu, &re) >= 3) {
                st.rec_total_polls = tp;
                st.rec_duration_us = du;
                st.rec_eff_duration_us = eu;
            }
        }
    }
    if (!header_ok || st.replay_samples.empty()) {
        LOGE("input_recorder: invalid or empty replay file %s (header=%d samples=%zu)", path.c_str(), int(header_ok),
             st.replay_samples.size());
        return false;
    }
    if (st.rec_total_polls == 0) st.rec_total_polls = st.replay_samples.back().frame_id + 1;
    if (st.rec_duration_us == 0) st.rec_duration_us = st.replay_samples.back().timestamp_us;
    if (st.rec_eff_duration_us == 0) st.rec_eff_duration_us = st.replay_samples.back().eff_us;
    return true;
}

void WriteStateCheckpoint(RecorderState& st, uint64_t poll_id, uint64_t raw_us, uint64_t eff_us, const GuestState& gs,
                          const char* reason) {
    if (st.rec_file) {
        std::fprintf(st.rec_file,
                     "C %llu %llu session=%u mode=0x%X pause=%u lvl_flags=0x%X gframe=%u gtime=%.3f cam=%.3f,%.3f,%.3f level=\"%s\"\n",
                     (unsigned long long)poll_id, (unsigned long long)raw_us, gs.session, gs.mode, gs.pause, gs.lvl_flags,
                     gs.guest_frame, gs.gtime, gs.cam[0], gs.cam[1], gs.cam[2], JsonEscape(gs.level).c_str());
        std::fflush(st.rec_file);
    }
    if (st.state_file) {
        std::fprintf(st.state_file,
                     "{\"type\":\"checkpoint\",\"reason\":\"%s\",\"poll_id\":%llu,\"timestamp_us\":%llu,\"eff_us\":%llu,"
                     "\"swap_frame\":%u,\"gframe\":%u,\"session\":%u,\"mode\":%u,\"pause\":%u,\"lvl_flags\":%u,"
                     "\"gtime\":%.4f,\"dt\":%.6f,\"cam_valid\":%s,\"cam\":[%.4f,%.4f,%.4f],\"level\":\"%s\"}\n",
                     reason, (unsigned long long)poll_id, (unsigned long long)raw_us, (unsigned long long)eff_us,
                     gs.swap_frame, gs.guest_frame, gs.session, gs.mode, gs.pause, gs.lvl_flags, gs.gtime, gs.dt,
                     gs.cam_valid ? "true" : "false", gs.cam[0], gs.cam[1], gs.cam[2], JsonEscape(gs.level).c_str());
        std::fflush(st.state_file);
    }
}

void OpenRecFile(RecorderState& st) {
    std::error_code ec;
    std::filesystem::path p(st.record_path);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    st.rec_file = std::fopen(st.record_path.c_str(), "wb");
    if (st.rec_file) {
        g_pad_only_recording_session.store(true, std::memory_order_release);
        g_recording_active.store(true, std::memory_order_release);
        std::fprintf(st.rec_file,
                     "#TSREC 1\n"
                     "# title_id=0x4D570015 game=\"The Suffering (USA)\" xdk=5788 poll_port=0\n"
                     "# input_contract=controller_only native_mouse_keyboard=disabled\n");
        std::fflush(st.rec_file);
        LOGR("input_recorder: recording to %s (controller input only; native mouse/keyboard and guest preference overrides disabled; saved controls unchanged)",
             st.record_path.c_str());
    } else {
        LOGE("input_recorder: cannot open record file %s", st.record_path.c_str());
    }
}

void FinalizeLocked(RecorderState& st) {
    if (st.finalized) return;
    st.finalized = true;
    if (st.rec_file) {
        std::fprintf(st.rec_file, "S total_polls=%llu duration_us=%llu eff_duration_us=%llu rumble_events=%llu\n",
                     (unsigned long long)st.poll_id, (unsigned long long)st.last_raw_us, (unsigned long long)st.eff_us,
                     (unsigned long long)st.rumble_events);
        std::fflush(st.rec_file);
        std::fclose(st.rec_file);
        st.rec_file = nullptr;
        g_recording_active.store(false, std::memory_order_release);
        LOGR("input_recorder: finalized %s (total_polls=%llu duration_us=%llu eff_duration_us=%llu rumble=%llu)",
             st.record_path.c_str(), (unsigned long long)st.poll_id, (unsigned long long)st.last_raw_us,
             (unsigned long long)st.eff_us, (unsigned long long)st.rumble_events);
    }
    if (st.state_file) {
        std::fprintf(st.state_file,
                     "{\"type\":\"summary\",\"total_polls\":%llu,\"duration_us\":%llu,\"eff_duration_us\":%llu,"
                     "\"rumble_events\":%llu,\"replay_enabled\":%s,\"replay_mode\":\"%s\",\"replay_finished\":%s}\n",
                     (unsigned long long)st.poll_id, (unsigned long long)st.last_raw_us, (unsigned long long)st.eff_us,
                     (unsigned long long)st.rumble_events, st.replay_enabled ? "true" : "false",
                     ModeName(st.replay_mode), st.replay_finished ? "true" : "false");
        std::fflush(st.state_file);
        std::fclose(st.state_file);
        st.state_file = nullptr;
    }
}

perf::PhaseInfo QueryGuestPerfPhase() {
    GuestState gs = ReadGuestState();
    perf::PhaseInfo pi{};
    pi.g_dt_ms = gs.dt * 1000.0f;
    pi.g_elapsed_s = gs.gtime;
    pi.g_frame = gs.guest_frame;
    pi.session = gs.session;

    if ((gs.mode & 8u) != 0u) {
        pi.phase = perf::Phase::Fmv;
    } else if ((gs.session == 3u && gs.mode != 1u) ||
               (gs.session == 2u && ((gs.lvl_flags & 2u) == 0u || gs.dt >= 0.095f)) ||
               (gs.session == 1u && !gs.level.empty())) {
        pi.phase = perf::Phase::LoadingTransition;
    } else if (gs.session == 2u && (gs.lvl_flags & 2u) != 0u) {
        pi.phase = (gs.pause & 1u) ? perf::Phase::Paused : perf::Phase::GameplayActive;
    } else {
        pi.phase = perf::Phase::TitleMenu;
    }
    return pi;
}

gpu::CameraFrameData QueryGuestCamera() {
    gpu::CameraFrameData out{};
    GuestState gs = ReadGuestState();
    out.guest_frame = gs.guest_frame;
    out.session = gs.session;
    out.dt_ms = (gs.dt > 0.001f && gs.dt < 0.25f) ? (gs.dt * 1000.0f) : 16.667f;
    out.level_path = gs.level;
    out.near_plane = 32.0f;
    out.is_3d_gameplay = (gs.session == 2u) && ((gs.pause & 1u) == 0u) && ((gs.mode & 8u) == 0u) &&
                         ((gs.lvl_flags & 2u) != 0u);
    if (!gs.cam_valid) return out;

    uint32_t cam = 0;
    if (!SafeReadU32(kVaCameraPtr, cam) || cam < mem::kUserLow || cam + 0x98u > mem::kUserHigh ||
        !mem::IsCommitted(cam) || !mem::IsCommitted(cam + 0x97u)) {
        return out;
    }
    float m[16]{};
    for (uint32_t i = 0; i < 16; ++i) {
        if (!SafeReadF32(cam + i * 4u, m[i])) return out;
    }
    // Row-major left-handed D3D View matrix (negate Z column per sub_001C69A0 / sub_001A1EB0)
    out.view[0] = m[0];   out.view[1] = m[1];   out.view[2] = -m[2];   out.view[3] = 0.0f;
    out.view[4] = m[4];   out.view[5] = m[5];   out.view[6] = -m[6];   out.view[7] = 0.0f;
    out.view[8] = m[8];   out.view[9] = m[9];   out.view[10] = -m[10]; out.view[11] = 0.0f;
    out.view[12] = m[12]; out.view[13] = m[13]; out.view[14] = -m[14]; out.view[15] = 1.0f;

    out.cam_pos[0] = gs.cam[0];
    out.cam_pos[1] = gs.cam[1];
    out.cam_pos[2] = gs.cam[2];
    float fx = -m[2], fy = -m[6], fz = -m[10];
    float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
    if (flen > 1e-5f) {
        out.cam_fwd[0] = fx / flen;
        out.cam_fwd[1] = fy / flen;
        out.cam_fwd[2] = fz / flen;
    }

    uint32_t vp_mgr = 0, arr = 0, idx = 0;
    if (!SafeReadU32(kVaViewportMgrPtr, vp_mgr) || vp_mgr < mem::kUserLow || vp_mgr + 0x50u > mem::kUserHigh ||
        !mem::IsCommitted(vp_mgr) || !mem::IsCommitted(vp_mgr + 0x4Fu)) {
        return out;
    }
    if (!SafeReadU32(vp_mgr + 0x40u, arr) || !SafeReadU32(vp_mgr + 0x4Cu, idx) || idx >= 16u) return out;
    uint32_t entry = arr + idx * 0x2C0u;
    if (entry < mem::kUserLow || entry + 0x270u > mem::kUserHigh || !mem::IsCommitted(entry + 0x268u) ||
        !mem::IsCommitted(entry + 0x26Fu)) {
        return out;
    }
    float raw00 = 0.0f, raw11 = 0.0f;
    if (!SafeReadF32(entry + 0x268u, raw00) || !SafeReadF32(entry + 0x26Cu, raw11)) return out;
    float p00 = -raw00;
    float p11 = -raw11;
    if (std::fabs(p00) < 1e-4f || std::fabs(p11) < 1e-4f) return out;

    // Exact Host Reversed-Z Infinite Projection matrix (near = 32.0 -> 1.0, far = +inf -> 0.0)
    out.proj_rev[0] = p00;
    out.proj_rev[5] = p11;
    out.proj_rev[11] = 1.0f;
    out.proj_rev[14] = 32.0f;
    out.fov_y_rad = 2.0f * std::atan(1.0f / std::max(std::fabs(p11), 1e-4f));
    out.valid = true;
    return out;
}

}  // namespace

bool IsInputRecordingActive() { return g_recording_active.load(std::memory_order_acquire); }

bool IsPadOnlyInputSession() {
    return app::IsReplayOrAutotestActive() || g_pad_only_recording_session.load(std::memory_order_acquire);
}

void OnInit() {
    perf::SetPhaseCallback(QueryGuestPerfPhase);
    gpu::SetCameraCallback(QueryGuestCamera);
    upscale::RegisterGlobalUpscaler();
    RecorderState& st = State();
    std::lock_guard<std::mutex> guard(st.lock);
    if (st.initialized) return;
    st.initialized = true;

#ifdef _WIN32
    st.record_path = GetEnvUtf8(L"XBE_INPUT_RECORD");
    st.replay_path = GetEnvUtf8(L"XBE_INPUT_REPLAY");
    st.state_log_path = GetEnvUtf8(L"XBE_STATE_LOG");
    st.autotest_path = GetEnvUtf8(L"SUFFERING_AUTOTEST");
    std::string mode_str = GetEnvUtf8(L"XBE_REPLAY_MODE");
    if (GetEnvUtf8(L"XBE_REPLAY_EXIT") == "1") st.replay_exit = true;

    std::vector<std::wstring> args = SplitCommandLine(GetCommandLineW());
    for (size_t i = 1; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        auto take_val = [&](const wchar_t* flag, std::string& dst) -> bool {
            size_t flen = std::wcslen(flag);
            if (a == flag && i + 1 < args.size()) {
                dst = WideToUtf8(StripQuotes(args[++i]));
                return true;
            }
            if (a.size() > flen + 1 && a.compare(0, flen, flag) == 0 && a[flen] == L'=') {
                dst = WideToUtf8(StripQuotes(a.substr(flen + 1)));
                return true;
            }
            return false;
        };
        if (take_val(L"--record-input", st.record_path)) continue;
        if (take_val(L"--replay-input", st.replay_path)) continue;
        if (take_val(L"--replay-mode", mode_str)) continue;
        if (take_val(L"--state-log", st.state_log_path)) continue;
        if (a == L"--replay-exit") st.replay_exit = true;
    }
#else
    st.record_path = GetEnvUtf8("XBE_INPUT_RECORD");
    st.replay_path = GetEnvUtf8("XBE_INPUT_REPLAY");
    st.state_log_path = GetEnvUtf8("XBE_STATE_LOG");
    st.autotest_path = GetEnvUtf8("SUFFERING_AUTOTEST");
    std::string mode_str = GetEnvUtf8("XBE_REPLAY_MODE");
    if (GetEnvUtf8("XBE_REPLAY_EXIT") == "1") st.replay_exit = true;

    std::vector<std::string> args = ReadProcCmdline();
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto take_val = [&](const char* flag, std::string& dst) -> bool {
            size_t flen = std::strlen(flag);
            if (a == flag && i + 1 < args.size()) {
                dst = StripQuotesUtf8(args[++i]);
                return true;
            }
            if (a.size() > flen + 1 && a.compare(0, flen, flag) == 0 && a[flen] == '=') {
                dst = StripQuotesUtf8(a.substr(flen + 1));
                return true;
            }
            return false;
        };
        if (take_val("--record-input", st.record_path)) continue;
        if (take_val("--replay-input", st.replay_path)) continue;
        if (take_val("--replay-mode", mode_str)) continue;
        if (take_val("--state-log", st.state_log_path)) continue;
        if (a == "--replay-exit") st.replay_exit = true;
    }
#endif

    if (mode_str == "frame") st.replay_mode = ReplayMode::Frame;
    else if (mode_str == "time") st.replay_mode = ReplayMode::Time;
    else st.replay_mode = ReplayMode::Hybrid;

    if (!st.state_log_path.empty()) {
        std::error_code ec;
        std::filesystem::path p(st.state_log_path);
        if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
        st.state_file = std::fopen(st.state_log_path.c_str(), "wb");
        if (!st.state_file) LOGE("input_recorder: cannot open state log %s", st.state_log_path.c_str());
    }

    if (!st.replay_path.empty()) {
        if (LoadRecFile(st.replay_path, st)) {
            st.replay_enabled = true;
            LOGR("input_recorder: replaying %s (mode=%s samples=%zu total_polls=%llu eff_us=%llu exit=%d)",
                 st.replay_path.c_str(), ModeName(st.replay_mode), st.replay_samples.size(),
                 (unsigned long long)st.rec_total_polls, (unsigned long long)st.rec_eff_duration_us,
                 int(st.replay_exit));
        }
    }

    if (!st.autotest_path.empty() && !st.replay_enabled) {
        st.autotest_enabled = true;
        std::ifstream f(st.autotest_path);
        if (f) {
            std::string line;
            while (std::getline(f, line)) {
                auto hash = line.find('#');
                if (hash != std::string::npos) line.resize(hash);
                std::istringstream iss(line);
                AutoAction act;
                if (iss >> act.start_ms >> act.end_ms >> act.token) st.auto_actions.push_back(std::move(act));
            }
            LOGR("autotest: loaded %zu scripted actions from %s", st.auto_actions.size(), st.autotest_path.c_str());
        } else {
            LOGR("autotest: virtual pad active (no script file at %s)", st.autotest_path.c_str());
        }
    }

    if (!st.record_path.empty()) OpenRecFile(st);
    UiInit();
    MkbInit();
}

void OnWindow(void* hwnd) {
    UiOnWindow(hwnd);
    MkbOnWindow(hwnd);
    // Runs on window thread right after input::Init() (main.cpp:288).
    std::error_code ec;
    std::filesystem::path db_path = app::ExeDir() / "gamecontrollerdb.txt";
    int added_mappings = -1;
    if (std::filesystem::is_regular_file(db_path, ec)) {
        added_mappings = SDL_AddGamepadMappingsFromFile(db_path.string().c_str());
    }

    const char* hidapi_hint = SDL_GetHint(SDL_HINT_JOYSTICK_HIDAPI);
    const char* wgi_hint = SDL_GetHint(SDL_HINT_JOYSTICK_WGI);
    bool hidapi_enabled = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_HIDAPI, true);
    bool wgi_enabled = SDL_GetHintBoolean(SDL_HINT_JOYSTICK_WGI, true);
    int ver = SDL_GetVersion();
    int count = 0;
    bool dualsense_attached = false;
    std::string devices_json = "[";
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
            uint16_t vid = SDL_GetGamepadVendorForID(ids[i]);
            uint16_t pid = SDL_GetGamepadProductForID(ids[i]);
            const char* name = SDL_GetGamepadNameForID(ids[i]);
            SDL_GamepadType gtype = SDL_GetGamepadTypeForID(ids[i]);
            bool is_ds5 = (vid == 0x054Cu && (pid == 0x0CE6u || pid == 0x0DF2u)) || gtype == SDL_GAMEPAD_TYPE_PS5;
            if (is_ds5) dualsense_attached = true;
            LOGR("input_audit: gamepad[%d] name=\"%s\" vid=0x%04X pid=0x%04X type=%d dualsense5=%s", i,
                 name ? name : "", vid, pid, int(gtype), is_ds5 ? "true" : "false");
            if (i > 0) devices_json += ",";
            char buf[512];
            std::snprintf(buf, sizeof(buf),
                          "{\"name\":\"%s\",\"vid\":\"0x%04X\",\"pid\":\"0x%04X\",\"type\":%d,\"dualsense5\":%s}",
                          JsonEscape(name ? name : "").c_str(), vid, pid, int(gtype), is_ds5 ? "true" : "false");
            devices_json += buf;
        }
        SDL_free(ids);
    }
    devices_json += "]";

    LOGR("input_audit: SDL3 %d.%d.%d driver=hidapi(%d,hint=%s) wgi(%d,hint=%s) gamepads=%d physical_dualsense_attached=%s db_mappings=%d",
         SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver), int(hidapi_enabled),
         hidapi_hint ? hidapi_hint : "default", int(wgi_enabled), wgi_hint ? wgi_hint : "default", count,
         dualsense_attached ? "true" : "false", added_mappings);

    RecorderState& st = State();
    std::lock_guard<std::mutex> guard(st.lock);
    if (st.state_file) {
        std::fprintf(st.state_file,
                     "{\"type\":\"sdl3_audit\",\"sdl_version\":\"%d.%d.%d\",\"hidapi_enabled\":%s,\"hidapi_hint\":\"%s\","
                     "\"wgi_enabled\":%s,\"wgi_hint\":\"%s\",\"gamepad_count\":%d,\"physical_dualsense_attached\":%s,"
                     "\"db_mappings\":%d,\"devices\":%s}\n",
                     SDL_VERSIONNUM_MAJOR(ver), SDL_VERSIONNUM_MINOR(ver), SDL_VERSIONNUM_MICRO(ver),
                     hidapi_enabled ? "true" : "false", hidapi_hint ? hidapi_hint : "default",
                     wgi_enabled ? "true" : "false", wgi_hint ? wgi_hint : "default", count,
                     dualsense_attached ? "true" : "false", added_mappings, devices_json.c_str());
        std::fflush(st.state_file);
    }
}

bool OnMessage(void* hwnd, uint32_t msg, uintptr_t wp, intptr_t lp, intptr_t* result) {
#ifdef _WIN32
    if (msg == WM_CLOSE || msg == WM_DESTROY) {
#else
    if (msg == SDL_EVENT_QUIT || msg == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
#endif
        RecorderState& st = State();
        std::lock_guard<std::mutex> guard(st.lock);
        FinalizeLocked(st);
    }
    if (UiOnMessage(hwnd, msg, wp, lp, result)) return true;
    return MkbOnMessage(hwnd, msg, wp, lp, result);
}

void OnPad(int port, input::Pad& pad) {
    if (port != 0) return;
    RecorderState& st = State();
    std::lock_guard<std::mutex> guard(st.lock);

    const bool replay_or_auto = st.replay_enabled || st.autotest_enabled;
    const InputMode mode = IsPadOnlyInputSession() ? InputMode::ControllerOnly : MkbGetConfig().input_mode;

    // Detached 250 ms xapi.cpp poller thread (kernel::Current() == nullptr):
    // report virtual pad presence ONLY; never record, merge, or advance replay state from host poller.
    if (kernel::Current() == nullptr) {
        if ((st.IsActive() && st.HasVirtualPad()) || (!replay_or_auto && mode != InputMode::ControllerOnly)) {
            pad.connected = true;
        }
        return;
    }

    auto apply_live_mkb_and_ui = [&] {
        if (mode == InputMode::MouseKeyboardOnly) {
            pad = input::Pad{};
            pad.connected = true;
        } else if (mode == InputMode::Auto) {
            if (pad.connected && std::hypot(float(pad.rx), float(pad.ry)) > 10000.0f) {
                MkbNotifyGamepadRightStickActive();
            }
            pad.connected = true;
        }
        UiFilterPad(port, pad, false);
        static uint64_t s_live_poll_id = 0;
        const bool syn = MkbIsSyntheticTestActive();
        if (syn) MkbAdvanceSyntheticEvents();
        const bool overlay_open = UiIsOverlayOpen();
        if (!overlay_open && mode != InputMode::ControllerOnly) {
            MkbMergeGameplayPad(pad);
        }
        if (syn) MkbLogSyntheticTelemetry(s_live_poll_id, pad, overlay_open);
        ++s_live_poll_id;
    };

    if (!st.IsActive()) {
        apply_live_mkb_and_ui();
        return;
    }

    if (!replay_or_auto) {
        apply_live_mkb_and_ui();
    }

    auto now = std::chrono::steady_clock::now();
    if (!st.anchor_set) {
        st.anchor_set = true;
        st.t0 = now;
        st.last_poll_tp = now;
        st.poll_id = 0;
        st.last_raw_us = 0;
        st.eff_us = 0;
    } else {
        uint64_t dt_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - st.last_poll_tp).count());
        st.last_poll_tp = now;
        st.last_raw_us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - st.t0).count());
        st.eff_us += std::min<uint64_t>(dt_us, kMaxEffStepUs);
    }

    const uint64_t cur_poll = st.poll_id;
    const uint64_t cur_raw_us = st.last_raw_us;
    const uint64_t cur_eff_us = st.eff_us;

    // 1. Apply replay OR scripted autotest
    if (st.replay_enabled) {
        pad = input::Pad{};
        pad.connected = true;
        const auto& samples = st.replay_samples;
        while (st.replay_cursor + 1 < samples.size()) {
            const RecSample& next = samples[st.replay_cursor + 1];
            bool advance = false;
            switch (st.replay_mode) {
                case ReplayMode::Frame: advance = next.frame_id <= cur_poll; break;
                case ReplayMode::Time: advance = next.timestamp_us <= cur_raw_us; break;
                case ReplayMode::Hybrid: advance = next.eff_us <= cur_eff_us; break;
            }
            if (!advance) break;
            ++st.replay_cursor;
        }
        pad = samples[st.replay_cursor].pad;
        pad.connected = true;

        bool reached_end = false;
        switch (st.replay_mode) {
            case ReplayMode::Frame: reached_end = cur_poll >= st.rec_total_polls; break;
            case ReplayMode::Time: reached_end = cur_raw_us >= st.rec_duration_us; break;
            case ReplayMode::Hybrid: reached_end = cur_eff_us >= st.rec_eff_duration_us; break;
        }
        if (reached_end) {
            // Release all controls once stream completes
            pad = input::Pad{};
            pad.connected = true;
            if (!st.replay_finished) {
                st.replay_finished = true;
                st.replay_done_poll = cur_poll;
                LOGR("input_recorder: replay completed at poll=%llu raw_us=%llu eff_us=%llu (mode=%s)",
                     (unsigned long long)cur_poll, (unsigned long long)cur_raw_us, (unsigned long long)cur_eff_us,
                     ModeName(st.replay_mode));
            }
            if (st.replay_exit && !st.exit_posted && cur_poll >= st.replay_done_poll + kSettlePollsAfterReplay) {
                st.exit_posted = true;
#ifdef _WIN32
                if (HWND hwnd = static_cast<HWND>(app::Window())) {
                    PostMessageW(hwnd, WM_CLOSE, 0, 0);
                }
#else
                SDL_Event ev{};
                ev.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&ev);
#endif
            }
        }
    } else if (st.autotest_enabled) {
        pad.connected = true;
        uint64_t ms = cur_raw_us / 1000ull;
        for (const AutoAction& act : st.auto_actions) {
            if (ms >= act.start_ms && ms < act.end_ms) ApplyToken(pad, act.token);
        }
    }

    // 2. Read safe guest .data state snapshot
    GuestState gs = ReadGuestState();
    uint32_t lvl_loaded = (gs.lvl_flags >> 1) & 1u;
    bool state_changed = !st.has_last_state || gs.session != st.last_session || gs.pause != st.last_pause ||
                         lvl_loaded != st.last_lvl_loaded || gs.level != st.last_level;

    // 3. Record input sample (poll 0, pad delta, state transition, or 60-poll heartbeat)
    if (st.rec_file) {
        bool pad_changed = !st.has_last_rec_pad || !PadsEqual(pad, st.last_rec_pad);
        if (cur_poll == 0 || pad_changed || state_changed || (cur_poll % 60ull == 0)) {
            std::fprintf(st.rec_file,
                         "I %llu %llu %llu %u %u %04X %u %u %u %u %u %u %u %u %d %d %d %d\n",
                         (unsigned long long)cur_poll, (unsigned long long)cur_raw_us, (unsigned long long)cur_eff_us,
                         gs.swap_frame, gs.guest_frame, unsigned(pad.buttons), unsigned(pad.analog[0]),
                         unsigned(pad.analog[1]), unsigned(pad.analog[2]), unsigned(pad.analog[3]),
                         unsigned(pad.analog[4]), unsigned(pad.analog[5]), unsigned(pad.analog[6]),
                         unsigned(pad.analog[7]), int(pad.lx), int(pad.ly), int(pad.rx), int(pad.ry));
            if (cur_poll == 0 || state_changed || (cur_poll % 60ull == 0)) std::fflush(st.rec_file);
            st.has_last_rec_pad = true;
            st.last_rec_pad = pad;
        }
    }

    // 4. Emit state checkpoint on transitions + every 120 polls while in active gameplay (session == 2)
    if (state_changed || (gs.session == 2 && (cur_poll % 120ull == 0))) {
        WriteStateCheckpoint(st, cur_poll, cur_raw_us, cur_eff_us, gs, state_changed ? "transition" : "periodic");
        st.has_last_state = true;
        st.last_session = gs.session;
        st.last_pause = gs.pause;
        st.last_lvl_loaded = lvl_loaded;
        st.last_level = gs.level;
    }

    st.poll_id++;
}

void OnRumble(int port, uint16_t& left, uint16_t& right) {
    if (port != 0) return;
    {
        RecorderState& st = State();
        std::lock_guard<std::mutex> guard(st.lock);
        if (st.rec_file || st.state_file) {
            if (!st.has_last_rumble || st.last_rumble_l != left || st.last_rumble_r != right) {
                st.has_last_rumble = true;
                st.last_rumble_l = left;
                st.last_rumble_r = right;
                st.rumble_events++;

                if (st.rec_file) {
                    std::fprintf(st.rec_file, "R %llu %llu %u %u\n", (unsigned long long)st.poll_id,
                                 (unsigned long long)st.last_raw_us, unsigned(left), unsigned(right));
                    std::fflush(st.rec_file);
                }
                if (st.state_file) {
                    std::fprintf(st.state_file,
                                 "{\"type\":\"rumble\",\"poll_id\":%llu,\"timestamp_us\":%llu,\"left\":%u,\"right\":%u}\n",
                                 (unsigned long long)st.poll_id, (unsigned long long)st.last_raw_us, unsigned(left),
                                 unsigned(right));
                    std::fflush(st.state_file);
                }
            }
        }
    }
    UiFilterRumble(port, left, right);
}

}  // namespace suffering
