// SPDX-License-Identifier: GPL-2.0-or-later
// Self-contained zero-flicker Pre-Game Graphical Launcher (Win32 + D3D12 on Windows; SDL3 + SDL_Renderer on Linux).
// Tears down all launcher rendering/ImGui/window resources cleanly before returning control to the host
// so the game initializes directly on the chosen GPU adapter without device conflicts.

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#else
#include <SDL3/SDL.h>
#include <unistd.h>
#include <zlib.h>
#endif

#include "common/file_browser_ui.h"
#include "common/launcher_ui.h"
#include "common/mkb_input.h"
#include "common/ui_common.h"
#include "common/update_checker.h"
#include "common/version.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <imgui.h>
#ifdef _WIN32
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>
#else
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#endif

#include "app/app.h"
#include "core/input.h"
#include "core/log.h"
#include "upscale/upscaler_manager.h"

#ifdef _WIN32
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
#endif

namespace suffering {

namespace {

namespace fs = std::filesystem;
#ifdef _WIN32
using Microsoft::WRL::ComPtr;

constexpr int kBufCount = 2;
#endif

std::string LowerAsciiStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

struct LauncherCli {
    int initial_tab = 0;
    fs::path capture_bmp;
    bool exit_after_capture = false;
    bool open_file_browser = false;
};

LauncherCli ParseLauncherCli() {
    LauncherCli cli{};
    auto args = GetProcessArgsUtf8();
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--launcher-tab" && i + 1 < args.size()) {
            cli.initial_tab = std::clamp(std::atoi(args[++i].c_str()), 0, 5);
        } else if (a.rfind("--launcher-tab=", 0) == 0) {
            cli.initial_tab = std::clamp(std::atoi(a.c_str() + 15), 0, 5);
        } else if (a == "--capture-launcher-bmp" && i + 1 < args.size()) {
            cli.capture_bmp = fs::path(args[++i]);
        } else if (a.rfind("--capture-launcher-bmp=", 0) == 0) {
            cli.capture_bmp = fs::path(a.substr(23));
        } else if (a == "--launcher-exit") {
            cli.exit_after_capture = true;
        } else if (a == "--open-file-browser") {
            cli.open_file_browser = true;
        }
    }
    return cli;
}

bool WriteBmp32(const fs::path& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& px) {
    if (!w || !h || px.size() < size_t(w) * h * 4) return false;
    std::error_code ec;
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), ec);
#ifdef _WIN32
    FILE* f = _wfopen(path.c_str(), L"wb");
#else
    FILE* f = std::fopen(path.c_str(), "wb");
#endif
    if (!f) return false;
    uint8_t hd[54] = {'B', 'M'};
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    uint32_t img_bytes = uint32_t(size_t(w) * h * 4);
    put(2, 54u + img_bytes);
    put(10, 54u);
    put(14, 40u);
    put(18, w);
    put(22, uint32_t(-int32_t(h)));
    hd[26] = 1;
    hd[28] = 32;
    put(34, img_bytes);
    bool ok = std::fwrite(hd, 1, 54, f) == 54 && std::fwrite(px.data(), 1, img_bytes, f) == img_bytes;
    std::fclose(f);
    return ok;
}

struct DiscStatus {
    bool checked = false;
    bool valid = false;
    bool from_assets_xbe = false;
    fs::path path;
    app::DiscValidation val{};
    std::string summary;
};

fs::path CliOrEnvIsoPath() {
    auto args = GetProcessArgsUtf8();
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--iso" && i + 1 < args.size()) {
            return fs::path(args[i + 1]);
        }
        if (a.rfind("--iso=", 0) == 0 && a.size() > 6) {
            return fs::path(a.substr(6));
        }
    }
#ifdef _WIN32
    if (const wchar_t* env = _wgetenv(L"XBE_ISO_PATH"); env && *env) {
        return fs::path(env);
    }
#else
    if (const char* env = std::getenv("XBE_ISO_PATH"); env && *env) {
        return fs::path(env);
    }
#endif
    return {};
}

DiscStatus CheckCurrentDiscStatus() {
    DiscStatus ds{};
    ds.checked = true;
    const app::Settings& s = app::Get();
    auto try_iso = [&](const fs::path& p) -> bool {
        if (p.empty()) return false;
        app::DiscValidation v{};
        if (app::ValidateDiscImage(p, v)) {
            ds.valid = true;
            ds.path = p;
            ds.val = v;
            ds.summary = v.title_name + " [" + v.revision + "] (Title ID 0x" +
                         [&] {
                             char b[16];
                             std::snprintf(b, sizeof(b), "%08X", v.title_id);
                             return std::string(b);
                         }() +
                         ", XDK " + std::to_string(v.xdk_build) + ")";
            return true;
        }
        ds.val = v;
        return false;
    };

    // 1. Explicit CLI --iso / XBE_ISO_PATH override (matches game_files.cpp Priority 1)
    if (fs::path cli_iso = CliOrEnvIsoPath(); !cli_iso.empty()) {
        if (cli_iso.is_relative()) cli_iso = (fs::current_path() / cli_iso).lexically_normal();
        if (try_iso(cli_iso)) return ds;
        ds.path = cli_iso;
        if (ds.val.error.empty()) ds.summary = "Invalid Xbox disc image passed via --iso / XBE_ISO_PATH.";
        else ds.summary = ds.val.error;
        return ds;
    }

    // 2. settings.toml iso_path
    if (!s.iso_path.empty()) {
        fs::path p(Utf8ToWideStr(s.iso_path));
        if (p.is_relative()) p = (app::ExeDir() / p).lexically_normal();
        if (try_iso(p)) return ds;
        ds.path = p;
    }

    // 3. assets/ beside exe (or ancestor in dev build), case-insensitive default.xbe / .iso scan
    fs::path dir = app::ExeDir();
    std::error_code ec;
    for (int i = 0; i < 6; ++i) {
        fs::path assets = dir / "assets";
        if (fs::is_directory(assets, ec)) {
            for (auto& e : fs::directory_iterator(assets, ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string lname = LowerAsciiStr(e.path().filename().string());
                if (lname == "default.xbe" && e.file_size(ec) > 0) {
                    ds.valid = true;
                    ds.from_assets_xbe = true;
                    ds.path = e.path();
                    ds.summary = "Extracted default.xbe found in " + assets.string();
                    return ds;
                }
            }
            for (auto& e : fs::directory_iterator(assets, ec)) {
                if (!e.is_regular_file(ec)) continue;
                std::string ext = LowerAsciiStr(e.path().extension().string());
                if (ext == ".iso" && try_iso(e.path())) return ds;
            }
            break;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }

    if (ds.val.error.empty()) {
        ds.summary = "No Xbox disc image configured. Click 'Browse Disc Image...' to select your .xiso.iso / .iso.";
    } else {
        ds.summary = ds.val.error;
    }
    return ds;
}

uint64_t ComputeCacheDiskBytes() {
    uint64_t total = 0;
    std::error_code ec;
    fs::path cache_dir = app::ExeDir() / "cache";
    if (fs::is_directory(cache_dir, ec)) {
        for (auto& e : fs::directory_iterator(cache_dir, ec)) {
            if (e.is_regular_file(ec)) total += e.file_size(ec);
        }
    }
    return total;
}

bool HasMockUpdateEnv() {
#ifdef _WIN32
    const wchar_t* v = _wgetenv(L"SUFFERING_UPDATE_MOCK_JSON");
    return v && *v;
#else
    const char* v = std::getenv("SUFFERING_UPDATE_MOCK_JSON");
    return v && *v;
#endif
}

void MaybeStartLauncherUpdateCheck() {
    if ((GetSharedUiOptions().check_updates_on_startup && ShouldAutoCheckForUpdates()) || HasMockUpdateEnv()) {
        if (GetUpdateSnapshot().state == UpdateState::Idle) {
            StartUpdateCheck(false);
        }
    }
}

struct LauncherUpdateUiTracker {
    bool manual_pending = false;
    UpdateState prev_state = UpdateState::Idle;
};

void PollLauncherUpdateTransitions(LauncherUpdateUiTracker& tr, const UpdateStatusSnapshot& snap,
                                   const std::function<void(const std::string&, bool)>& set_status) {
    if (tr.manual_pending && snap.state != UpdateState::Checking && snap.state != UpdateState::Idle) {
        tr.manual_pending = false;
        if (snap.state == UpdateState::UpToDate) {
            set_status(std::string("Up to date (") + SUFFERING_VERSION_TAG + ")", false);
        } else if (snap.state == UpdateState::UpdateAvailable) {
            std::string tag = !snap.latest_tag.empty() ? snap.latest_tag : ("v" + snap.latest_version);
            set_status("Update available: " + tag, false);
        } else if (snap.state == UpdateState::Error) {
            set_status(snap.status_message.empty() ? "Update check failed (network error)."
                                                   : SanitizeUiText(snap.status_message, true, 140),
                       true);
        }
    } else if (tr.prev_state == UpdateState::Downloading && snap.state != UpdateState::Downloading) {
        if (snap.state == UpdateState::ReadyToRestart) {
            set_status("Update " + snap.latest_tag + " verified & staged! Click 'Restart & Apply Update' to install.",
                       false);
        } else if (snap.state == UpdateState::Error) {
            set_status(snap.status_message.empty() ? "Self-update download/staging failed."
                                                   : SanitizeUiText(snap.status_message, true, 140),
                       true);
        }
    }
    tr.prev_state = snap.state;
}

bool DrawLauncherUpdateAlertIfNeeded(const UpdateStatusSnapshot& snap, float ui_scale,
                                     const std::function<void(const std::string&, bool)>& set_status,
                                     bool& launch_game, bool& quit) {
    if (!ShouldShowUpdateBanner(snap, GetSharedUiOptions().dismissed_update_tag)) {
        return false;
    }
    UpdateCardContext uctx{};
    uctx.in_launcher = true;
    uctx.ui_scale = ui_scale;
    uctx.on_update_now = [&] {
        if (StartDownloadAndStageUpdate()) {
            set_status("Downloading & staging " +
                           (snap.asset_name.empty() ? snap.latest_tag : snap.asset_name) + "...",
                       false);
        } else {
            UpdateStatusSnapshot err_s = GetUpdateSnapshot();
            set_status(err_s.status_message.empty() ? "Could not start self-update download."
                                                    : SanitizeUiText(err_s.status_message, true, 140),
                       true);
        }
    };
    uctx.on_apply_and_restart = [&] {
        app::Save();
        if (ApplyStagedUpdateAndRelaunch()) {
            launch_game = false;
            quit = true;
        } else {
            UpdateStatusSnapshot err_s = GetUpdateSnapshot();
            set_status(err_s.status_message.empty() ? "Failed to launch staged update helper."
                                                    : SanitizeUiText(err_s.status_message, true, 140),
                       true);
        }
    };
    uctx.on_open_release_page = [&](const std::string& url) {
        std::string target = url.empty() ? std::string(SUFFERING_RELEASES_URL) : url;
        OpenReleasePageInBrowser(target);
        if (IsGamescope()) {
            set_status("Release page: " + target + " (browse in Desktop Mode if hidden)", false);
        } else {
            set_status("Opened release page in browser: " + target, false);
        }
    };
    uctx.on_dismiss = [&](const std::string& tag) {
        GetSharedUiOptions().dismissed_update_tag = tag;
        if (!HasMockUpdateEnv()) app::Save();
        set_status("Dismissed update alert for " + tag + " (click 'Check for Updates' to show again).", false);
    };
    DrawUpdateAlertCard(snap, uctx);
    ImGui::Spacing();
    return true;
}

#ifdef _WIN32
struct LauncherCtx {
    HWND hwnd = nullptr;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    uint32_t rtv_inc = 0;
    ComPtr<ID3D12Resource> bb[kBufCount];
    ComPtr<ID3D12CommandAllocator> alloc[kBufCount];
    ComPtr<ID3D12GraphicsCommandList> cl;
    ComPtr<ID3D12Fence> fence;
    HANDLE fence_event = nullptr;
    uint64_t fence_val = 0;
    uint32_t width = 920;
    uint32_t height = 620;
    float ui_scale = 1.0f;
    bool dpi_changed = false;
    bool resize_needed = false;
    bool quit = false;
    bool launch_game = false;
    UiFonts fonts{};

    void WaitForGpu() {
        if (!queue || !fence || !fence_event) return;
        const uint64_t v = ++fence_val;
        if (SUCCEEDED(queue->Signal(fence.Get(), v))) {
            if (fence->GetCompletedValue() < v) {
                fence->SetEventOnCompletion(v, fence_event);
                WaitForSingleObject(fence_event, INFINITE);
            }
        }
    }

    void CreateRtvs() {
        for (int i = 0; i < kBufCount; ++i) {
            if (SUCCEEDED(swap->GetBuffer(i, IID_PPV_ARGS(&bb[i]))) && bb[i]) {
                D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
                h.ptr += size_t(i) * rtv_inc;
                dev->CreateRenderTargetView(bb[i].Get(), nullptr, h);
            }
        }
    }

    bool InitD3D12(HWND wnd) {
        hwnd = wnd;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return false;
        ComPtr<IDXGIFactory6> f6;
        ComPtr<IDXGIAdapter1> ad;
        if (SUCCEEDED(factory.As(&f6))) {
            for (UINT i = 0; SUCCEEDED(f6->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                      IID_PPV_ARGS(&ad)));
                 ++i) {
                DXGI_ADAPTER_DESC1 d{};
                ad->GetDesc1(&d);
                if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                    ad.Reset();
                    continue;
                }
                if (SUCCEEDED(D3D12CreateDevice(ad.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) break;
                ad.Reset();
            }
        }
        if (!dev && FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)))) return false;

        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)))) return false;

        RECT rc{};
        GetClientRect(hwnd, &rc);
        width = std::max<uint32_t>(uint32_t(rc.right - rc.left), 320u);
        height = std::max<uint32_t>(uint32_t(rc.bottom - rc.top), 240u);

        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = width;
        sd.Height = height;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = kBufCount;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc1;
        if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) return false;
        factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(sc1.As(&swap))) return false;

        D3D12_DESCRIPTOR_HEAP_DESC rhd{};
        rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rhd.NumDescriptors = kBufCount;
        if (FAILED(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtv_heap)))) return false;
        rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC shd{};
        shd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        shd.NumDescriptors = 64;
        shd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev->CreateDescriptorHeap(&shd, IID_PPV_ARGS(&srv_heap)))) return false;

        CreateRtvs();
        for (int i = 0; i < kBufCount; ++i) {
            if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc[i]))))
                return false;
        }
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0].Get(), nullptr,
                                          IID_PPV_ARGS(&cl))))
            return false;
        cl->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return fence_event != nullptr;
    }

    void ResizeSwapIfNeeded() {
        if (!resize_needed || !swap || IsIconic(hwnd)) return;
        resize_needed = false;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        uint32_t nw = uint32_t(std::max<LONG>(rc.right - rc.left, 0));
        uint32_t nh = uint32_t(std::max<LONG>(rc.bottom - rc.top, 0));
        if (nw == 0 || nh == 0 || (nw == width && nh == height)) return;
        WaitForGpu();
        for (auto& b : bb) b.Reset();
        if (SUCCEEDED(swap->ResizeBuffers(kBufCount, nw, nh, DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            width = nw;
            height = nh;
            CreateRtvs();
        }
    }

    bool CaptureBackbufferBmp(uint32_t bi, const fs::path& dst_path) {
        if (!bb[bi]) return false;
        D3D12_RESOURCE_DESC rd = bb[bi]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 row_sz = 0, total = 0;
        dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_sz, &total);

        ComPtr<ID3D12Resource> rb;
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = 1;
        bd.DepthOrArraySize = 1;
        bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                IID_PPV_ARGS(&rb))))
            return false;

        WaitForGpu();
        alloc[bi]->Reset();
        cl->Reset(alloc[bi].Get(), nullptr);

        D3D12_RESOURCE_BARRIER b0{};
        b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b0.Transition = {bb[bi].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                         D3D12_RESOURCE_STATE_COPY_SOURCE};
        cl->ResourceBarrier(1, &b0);

        D3D12_TEXTURE_COPY_LOCATION dl{rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dl.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION sl{bb[bi].Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);

        D3D12_RESOURCE_BARRIER b1 = b0;
        b1.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b1);
        cl->Close();
        ID3D12CommandList* lists[] = {cl.Get()};
        queue->ExecuteCommandLists(1, lists);
        WaitForGpu();

        uint8_t* p = nullptr;
        if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p))) || !p) return false;
        std::vector<uint8_t> px(size_t(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
            std::memcpy(&px[size_t(y) * width * 4], p + size_t(y) * fp.Footprint.RowPitch, width * 4);
        }
        rb->Unmap(0, nullptr);
        bool ok = WriteBmp32(dst_path, width, height, px);
        LOGR("launcher: captured %s (%ux%u, %s)", dst_path.string().c_str(), width, height, ok ? "ok" : "failed");
        return ok;
    }

    void ShutdownD3D12() {
        WaitForGpu();
        if (fence_event) {
            CloseHandle(fence_event);
            fence_event = nullptr;
        }
        fence.Reset();
        cl.Reset();
        for (auto& a : alloc) a.Reset();
        for (auto& b : bb) b.Reset();
        srv_heap.Reset();
        rtv_heap.Reset();
        swap.Reset();
        queue.Reset();
        dev.Reset();
        factory.Reset();
    }
};

LauncherCtx* g_lctx = nullptr;

LRESULT CALLBACK LauncherWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (MkbHandleRebindMessage(msg, wp, lp)) return 0;
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    switch (msg) {
        case WM_DPICHANGED:
            if (g_lctx && lp) {
                const RECT* r = reinterpret_cast<const RECT*>(lp);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                g_lctx->dpi_changed = true;
                g_lctx->resize_needed = true;
            }
            return 0;
        case WM_SIZE:
            if (g_lctx && wp != SIZE_MINIMIZED) g_lctx->resize_needed = true;
            return 0;
        case WM_CLOSE:
            if (g_lctx) {
                g_lctx->quit = true;
                g_lctx->launch_game = false;
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
#else
uint64_t ReadLinuxTotalRamBytes() {
    std::ifstream f("/proc/meminfo");
    std::string key;
    uint64_t val = 0;
    std::string unit;
    while (f >> key >> val >> unit) {
        if (key == "MemTotal:") return val * 1024ull;
    }
    return 16ull << 30;
}

uint32_t ReadBe32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint8_t PaethPred(uint8_t a, uint8_t b, uint8_t c) {
    int p = int(a) + int(b) - int(c);
    int pa = std::abs(p - int(a));
    int pb = std::abs(p - int(b));
    int pc = std::abs(p - int(c));
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

void TrySetSdlLauncherIcon(SDL_Window* win) {
    if (!win) return;
    std::error_code ec;
    fs::path dir = app::ExeDir();
    fs::path icon_path;
    for (int i = 0; i < 5; ++i) {
        if (fs::is_regular_file(dir / "suffering.png", ec)) {
            icon_path = dir / "suffering.png";
            break;
        }
        if (fs::is_regular_file(dir / "src" / "res" / "suffering.png", ec)) {
            icon_path = dir / "src" / "res" / "suffering.png";
            break;
        }
        if (!dir.has_parent_path() || dir.parent_path() == dir) break;
        dir = dir.parent_path();
    }
    if (icon_path.empty()) return;
    std::ifstream f(icon_path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    static constexpr uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (data.size() < 33 || std::memcmp(data.data(), kSig, 8) != 0) return;

    uint32_t w = 0, h = 0;
    uint8_t bd = 0, ct = 0;
    std::vector<uint8_t> idat;
    size_t pos = 8;
    while (pos + 12 <= data.size()) {
        uint32_t len = ReadBe32(data.data() + pos);
        const uint8_t* tag = data.data() + pos + 4;
        if (pos + 12 + len > data.size()) break;
        const uint8_t* body = data.data() + pos + 8;
        if (std::memcmp(tag, "IHDR", 4) == 0 && len >= 13) {
            w = ReadBe32(body);
            h = ReadBe32(body + 4);
            bd = body[8];
            ct = body[9];
        } else if (std::memcmp(tag, "IDAT", 4) == 0) {
            idat.insert(idat.end(), body, body + len);
        } else if (std::memcmp(tag, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + size_t(len);
    }
    if (w == 0 || h == 0 || w > 1024 || h > 1024 || bd != 8 || ct != 6 || idat.empty()) return;
    size_t stride = size_t(w) * 4;
    uLongf raw_len = uLongf(size_t(h) * (stride + 1));
    std::vector<uint8_t> raw(raw_len);
    if (uncompress(raw.data(), &raw_len, idat.data(), uLong(idat.size())) != Z_OK ||
        raw_len != size_t(h) * (stride + 1))
        return;

    std::vector<uint8_t> rgba(size_t(h) * stride);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t ftype = raw[size_t(y) * (stride + 1)];
        const uint8_t* src = raw.data() + size_t(y) * (stride + 1) + 1;
        uint8_t* dst = rgba.data() + size_t(y) * stride;
        const uint8_t* prev = (y > 0) ? (rgba.data() + size_t(y - 1) * stride) : nullptr;
        for (size_t x = 0; x < stride; ++x) {
            uint8_t a = (x >= 4) ? dst[x - 4] : 0;
            uint8_t b = prev ? prev[x] : 0;
            uint8_t c = (prev && x >= 4) ? prev[x - 4] : 0;
            uint8_t v = src[x];
            switch (ftype) {
                case 0: dst[x] = v; break;
                case 1: dst[x] = uint8_t(v + a); break;
                case 2: dst[x] = uint8_t(v + b); break;
                case 3: dst[x] = uint8_t(v + ((int(a) + int(b)) >> 1)); break;
                case 4: dst[x] = uint8_t(v + PaethPred(a, b, c)); break;
                default: return;
            }
        }
    }
    if (SDL_Surface* surf = SDL_CreateSurfaceFrom(int(w), int(h), SDL_PIXELFORMAT_RGBA32, rgba.data(), int(stride))) {
        SDL_SetWindowIcon(win, surf);
        SDL_DestroySurface(surf);
    }
}

bool CaptureSdlRendererBmp(SDL_Renderer* renderer, const fs::path& dst_path) {
    if (!renderer) return false;
    SDL_Surface* raw = SDL_RenderReadPixels(renderer, nullptr);
    if (!raw) return false;
    SDL_Surface* bgra = SDL_ConvertSurface(raw, SDL_PIXELFORMAT_BGRA32);
    SDL_DestroySurface(raw);
    if (!bgra) return false;
    uint32_t w = uint32_t(bgra->w);
    uint32_t h = uint32_t(bgra->h);
    std::vector<uint8_t> px(size_t(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(bgra->pixels);
    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(&px[size_t(y) * w * 4], src + size_t(y) * size_t(bgra->pitch), size_t(w) * 4);
    }
    SDL_DestroySurface(bgra);
    bool ok = WriteBmp32(dst_path, w, h, px);
    LOGR("launcher: captured %s (%ux%u, %s)", dst_path.string().c_str(), w, h, ok ? "ok" : "failed");
    return ok;
}
#endif

}  // namespace

#ifdef _WIN32
bool RunLauncher(void* hinstance) {
    HINSTANCE inst = static_cast<HINSTANCE>(hinstance);
    LauncherCli cli = ParseLauncherCli();
    LOGR("launcher: opening Pre-Game Graphical Launcher (initial_tab=%d, capture=%s)", cli.initial_tab,
         cli.capture_bmp.empty() ? "none" : cli.capture_bmp.string().c_str());

    // Create hidden window sized for monitor DPI and clamped to 92% of rcWork
    HMONITOR mon = static_cast<HMONITOR>(app::TargetMonitor());
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    GetMonitorInfoW(mon, &mi);
    const RECT& work = mi.rcWork;
    int work_w = std::max<int>(640, work.right - work.left);
    int work_h = std::max<int>(480, work.bottom - work.top);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = LauncherWndProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    std::wstring class_name = std::wstring(app::kGame.name) + L"LauncherV11";
    wc.lpszClassName = class_name.c_str();
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    std::wstring win_title = L"The Suffering - PC Recompilation Launcher (" + Utf8ToWideStr(SUFFERING_VERSION_TAG) + L")";
    HWND hwnd = CreateWindowExW(0, class_name.c_str(), win_title.c_str(), style, work.left + 40, work.top + 40, 920,
                                620, nullptr, nullptr, inst, nullptr);
    if (!hwnd) {
        UnregisterClassW(class_name.c_str(), inst);
        return true;  // Fail open to game if launcher window cannot be created
    }

    // Apply dark Win10/11 titlebar
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));

    float ui_scale = ComputeWindowDpiScale(hwnd);
    int target_cw = std::min(int(std::lround(920.0f * ui_scale)), int(work_w * 0.92));
    int target_ch = std::min(int(std::lround(630.0f * ui_scale)), int(work_h * 0.92));
    RECT r{0, 0, target_cw, target_ch};
    AdjustWindowRect(&r, style, FALSE);
    int win_w = std::min(int(r.right - r.left), work_w);
    int win_h = std::min(int(r.bottom - r.top), work_h);
    SetWindowPos(hwnd, nullptr, work.left + (work_w - win_w) / 2, work.top + (work_h - win_h) / 2, win_w, win_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    LauncherCtx lctx{};
    lctx.ui_scale = ui_scale;
    g_lctx = &lctx;
    if (!lctx.InitD3D12(hwnd)) {
        g_lctx = nullptr;
        DestroyWindow(hwnd);
        UnregisterClassW(class_name.c_str(), inst);
        LOGW("launcher: lightweight D3D12 init failed; continuing directly to game");
        return true;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    LoadCrispUiFonts(io, ui_scale, lctx.fonts);
    ApplyDarkIndustrialStyle(ui_scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(lctx.dev.Get(), kBufCount, DXGI_FORMAT_B8G8R8A8_UNORM, lctx.srv_heap.Get(),
                        lctx.srv_heap->GetCPUDescriptorHandleForHeapStart(),
                        lctx.srv_heap->GetGPUDescriptorHandleForHeapStart());

    input::Init();

    // Gather hardware / disc / plugin state once up-front
    std::vector<app::GpuAdapterInfo> gpus = app::EnumerateGpuAdapters();
    std::vector<app::MonitorInfo> monitors = app::EnumerateMonitors();
    std::vector<app::Mode> resolutions;
    std::string res_device;
    RefreshMonitorLists(monitors, resolutions, res_device, app::Get().monitor);

    MEMORYSTATUSEX mem{sizeof(mem)};
    GlobalMemoryStatusEx(&mem);
    DWORD logical_cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    uint32_t compile_jobs = app::RecommendedCompileJobs();
    app::Mode desktop = app::Desktop();
    uint32_t desk_hz = app::DisplayRefreshHz();

    const app::GpuAdapterInfo* active_gpu = FindSelectedGpu(gpus, app::Get().gpu_adapter);
    upscale::PluginAvailability plugins =
        upscale::ProbeUpscalerPlugins(active_gpu ? active_gpu->vendor_id : 0u, uint32_t(D3D_SHADER_MODEL_6_6));
    app::HardwarePreset recommended_preset = DetectHardwarePreset(active_gpu, desktop, mem.ullTotalPhys);

    DiscStatus disc = CheckCurrentDiscStatus();
    LOGR("launcher: disc status valid=%s (%s)", disc.valid ? "true" : "false", disc.summary.c_str());
    std::error_code ec;
    bool dll_ready = fs::is_regular_file(app::ExeDir() / "suffering.dll", ec);
    uint64_t cache_bytes = ComputeCacheDiskBytes();

    std::string status_banner;
    bool status_is_err = false;
    auto set_status = [&](const std::string& msg, bool err) {
        status_banner = msg;
        status_is_err = err;
    };

    MaybeStartLauncherUpdateCheck();
    LauncherUpdateUiTracker upd_tracker{};
    upd_tracker.prev_state = GetUpdateSnapshot().state;

    bool rumble_active = false;
    std::chrono::steady_clock::time_point rumble_until{};
    bool was_pad_active = false;
    bool prev_l1 = false, prev_r1 = false, prev_start = false;
    int selected_tab = cli.initial_tab;
    int requested_tab = cli.initial_tab;
    bool first_frame_shown = false;
    uint32_t frames_rendered = 0;
    bool capture_done = false;

    while (!lctx.quit) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                lctx.quit = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (lctx.quit) break;

        if (IsIconic(hwnd) && first_frame_shown && cli.capture_bmp.empty()) {
            Sleep(30);
            continue;
        }

        if (rumble_active && std::chrono::steady_clock::now() >= rumble_until) {
            rumble_active = false;
            input::Rumble(0, 0, 0);
        }

        if (lctx.dpi_changed) {
            lctx.dpi_changed = false;
            lctx.WaitForGpu();
            lctx.ui_scale = ComputeWindowDpiScale(hwnd);
            LoadCrispUiFonts(io, lctx.ui_scale, lctx.fonts);
            ApplyDarkIndustrialStyle(lctx.ui_scale);
            ImGui_ImplDX12_InvalidateDeviceObjects();
            ImGui_ImplDX12_CreateDeviceObjects();
        }
        lctx.ResizeSwapIfNeeded();

        // Poll SDL3 gamepad (kernel::Current() == nullptr here, so safe outside boot)
        input::Pad pad = input::Poll(0);
        FeedPadToImGui(pad, true, was_pad_active);
        bool l1 = pad.connected && pad.analog[5] > 64;
        bool r1 = pad.connected && pad.analog[4] > 64;
        bool start = pad.connected && ((pad.buttons & (1u << 4)) != 0);
        if (l1 && !prev_l1) requested_tab = (selected_tab + 5) % 6;
        if (r1 && !prev_r1) requested_tab = (selected_tab + 1) % 6;
        if (start && !prev_start && disc.valid) {
            lctx.launch_game = true;
            lctx.quit = true;
        }
        prev_l1 = l1;
        prev_r1 = r1;
        prev_start = start;

        // Refresh monitor/GPU derived data cleanly
        app::Settings& s = app::Get();
        RefreshMonitorLists(monitors, resolutions, res_device, s.monitor);
        active_gpu = FindSelectedGpu(gpus, s.gpu_adapter);
        UpdateStatusSnapshot upd_snap = GetUpdateSnapshot();
        PollLauncherUpdateTransitions(upd_tracker, upd_snap, set_status);

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(float(lctx.width), float(lctx.height)), ImGuiCond_Always);
        constexpr ImGuiWindowFlags kRootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##SufferingLauncherRoot", nullptr, kRootFlags)) {
            // --- Header ---
            if (lctx.fonts.heading) ImGui::PushFont(lctx.fonts.heading);
            ImGui::TextColored(ImVec4(0.96f, 0.34f, 0.34f, 1.0f), "THE SUFFERING");
            ImGui::SameLine();
            ImGui::TextUnformatted("- PC Recompilation Launcher " SUFFERING_VERSION_TAG);
            if (lctx.fonts.heading) ImGui::PopFont();
            DrawLauncherHeaderUpdateButton(upd_snap, lctx.ui_scale, [&] {
                if (!GetSharedUiOptions().dismissed_update_tag.empty()) {
                    GetSharedUiOptions().dismissed_update_tag.clear();
                    if (!HasMockUpdateEnv()) app::Save();
                }
                upd_tracker.manual_pending = true;
                set_status("Checking GitHub Releases for updates...", false);
                StartUpdateCheck(true);
            });

            std::string gpu_line = active_gpu ? active_gpu->name : "Default D3D12 GPU";
            double vram_gb = active_gpu ? (double(active_gpu->dedicated_vram) / (1024.0 * 1024.0 * 1024.0)) : 0.0;
            double ram_gb = double(mem.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
            ImGui::TextDisabled("Hardware: %s (%.1f GB %s)  |  %lu Threads  |  %.1f GB RAM  |  %ux%u @ %u Hz",
                                gpu_line.c_str(), vram_gb, (active_gpu && active_gpu->likely_uma) ? "UMA" : "VRAM",
                                (unsigned long)logical_cpus, ram_gb, desktop.width, desktop.height, desk_hz);

            // One-click Hardware Preset bar
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.36f, 0.22f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.50f, 0.30f, 1.00f));
            std::string auto_lbl = std::string("Auto-Detect Preset (") + HardwarePresetName(recommended_preset) + ")";
            if (ImGui::Button(auto_lbl.c_str())) {
                ApplyHardwarePreset(recommended_preset, active_gpu, plugins, desktop);
                set_status(std::string("Applied Auto-Detected Hardware Preset: ") +
                               HardwarePresetName(recommended_preset),
                           false);
            }
            ImGui::PopStyleColor(2);
            static constexpr app::HardwarePreset kTiers[] = {
                app::HardwarePreset::HandheldIgpu, app::HardwarePreset::Mainstream1080p,
                app::HardwarePreset::High1440p, app::HardwarePreset::Ultra4k};
            for (app::HardwarePreset tier : kTiers) {
                ImGui::SameLine();
                bool active = (s.hardware_preset == tier);
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.56f, 0.16f, 0.18f, 1.0f));
                if (ImGui::Button(HardwarePresetName(tier))) {
                    ApplyHardwarePreset(tier, active_gpu, plugins, desktop);
                    set_status(std::string("Applied Hardware Preset: ") + HardwarePresetName(tier), false);
                }
                if (active) ImGui::PopStyleColor();
            }
            ImGui::Separator();

            // --- Tabbed Content Body (reserve space for sticky footer) ---
            float footer_h = (status_banner.empty() ? 52.0f : 78.0f) * lctx.ui_scale;
            if (ImGui::BeginChild("##LauncherBody", ImVec2(0.0f, -footer_h), false)) {
                bool show_upd_banner = DrawLauncherUpdateAlertIfNeeded(upd_snap, lctx.ui_scale, set_status,
                                                                       lctx.launch_game, lctx.quit);

                UiPanelContext pctx{};
                pctx.in_launcher = true;
                pctx.gpus = &gpus;
                pctx.monitors = &monitors;
                pctx.resolutions = &resolutions;
                pctx.res_device = &res_device;
                pctx.plugins = &plugins;
                pctx.set_status = set_status;
                pctx.on_open_plugins_folder = [&] {
                    OpenDirectoryInExplorer(hwnd, app::ExeDir() / "plugins", "Plugins", set_status);
                    plugins = upscale::ProbeUpscalerPlugins(active_gpu ? active_gpu->vendor_id : 0u,
                                                            uint32_t(D3D_SHADER_MODEL_6_6));
                };
                pctx.on_test_rumble = [&] {
                    rumble_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    rumble_active = true;
                    input::Rumble(0, 45000, 45000);
                };

                auto tab_flags = [&](int idx) -> ImGuiTabItemFlags {
                    if (requested_tab == idx) {
                        requested_tab = -1;
                        return ImGuiTabItemFlags_SetSelected;
                    }
                    return 0;
                };

                if (ImGui::BeginTabBar("##LauncherTabs")) {
                    // Tab 0: Play & Game Disc
                    if (ImGui::BeginTabItem("Play & Game Disc", nullptr, tab_flags(0))) {
                        selected_tab = 0;
                        ImGui::Spacing();
                        ImGui::TextUnformatted("Original Xbox Disc Image Status (Opened Strictly Read-Only):");
                        float disc_card_h = (show_upd_banner ? 94.0f : 114.0f) * lctx.ui_scale;
                        if (ImGui::BeginChild("##DiscCard", ImVec2(0.0f, disc_card_h), true)) {
                            if (disc.valid) {
                                ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.45f, 1.0f), "READY: %s",
                                                   disc.summary.c_str());
                                ImGui::TextWrapped("Path: %s", disc.path.string().c_str());
                                if (!disc.val.sha1.empty()) {
                                    ImGui::TextDisabled("SHA-1: %s", disc.val.sha1.c_str());
                                }
                            } else {
                                ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.0f), "DISC IMAGE REQUIRED");
                                ImGui::TextWrapped("%s", disc.summary.c_str());
                            }
                        }
                        ImGui::EndChild();

                        if (ImGui::Button("Browse Disc Image (.xiso.iso / .iso)...")) {
                            fs::path picked;
                            if (app::BrowseDiscImage(hwnd, picked)) {
                                app::DiscValidation v{};
                                if (app::ValidateDiscImage(picked, v)) {
                                    s.iso_path = WideToUtf8Str(picked.wstring());
                                    app::Save();
                                    disc = CheckCurrentDiscStatus();
                                    set_status("Validated & saved disc image: " + picked.filename().string(), false);
                                } else {
                                    set_status("Invalid disc image: " + v.error, true);
                                }
                            }
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Re-Check Disc & Plugins")) {
                            disc = CheckCurrentDiscStatus();
                            plugins = upscale::ProbeUpscalerPlugins(active_gpu ? active_gpu->vendor_id : 0u,
                                                                    uint32_t(D3D_SHADER_MODEL_6_6));
                            set_status("Re-scanned disc image and upscaler plugins.", false);
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::TextUnformatted("First-Launch Native Code Status:");
                        if (dll_ready) {
                            ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.45f, 1.0f),
                                               "suffering.dll is compiled and ready for instant launch.");
                        } else {
                            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.32f, 1.0f),
                                               "suffering.dll will be compiled once on first launch using %u RAM-safe parallel clang jobs.",
                                               compile_jobs);
                        }
                        ImGui::TextDisabled("Recommended RAM-bounded compile concurrency: %u jobs (override with --compile-jobs N)",
                                            compile_jobs);

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::TextUnformatted("Active Configuration Summary:");
                        app::Mode out = app::Output();
                        bool wide = (out.width * 2 >= out.height * 3);
                        float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
                        uint32_t vpw = uint32_t(std::lround(std::min(float(out.width), float(out.height) * aspect)));
                        uint32_t vph = uint32_t(std::lround(float(vpw) / aspect));
                        uint32_t rw = 0, rh = 0;
                        upscale::ResolveUpscaleDimensions(s.upscaler, s.upscale_quality, s.render_percent,
                                                          s.aspect_correct_render, vpw, vph, 640, 480, rw, rh);
                        ImGui::BulletText("Upscaler: %s  |  Quality: %s  |  Sharpness: %u%%",
                                          upscale::UpscalerModeName(s.upscaler),
                                          upscale::UpscaleQualityName(s.upscale_quality), s.upscale_sharpness);
                        ImGui::BulletText("Resolution: %ux%u Internal -> %ux%u Viewport (%ux%u Output, %s)", rw, rh, vpw,
                                          vph, out.width, out.height, PresentModeName(s.present_mode));
                        ImGui::BulletText("Controls: Mouse/Keyboard or Gamepad (L1/R1 switch tabs, START plays)");
                        ImGui::EndTabItem();
                    }

                    // Tab 1: Upscaling & Graphics
                    if (ImGui::BeginTabItem("Upscaling & Graphics", nullptr, tab_flags(1))) {
                        selected_tab = 1;
                        ImGui::Spacing();
                        DrawUpscalingGraphicsPanel(pctx);
                        ImGui::EndTabItem();
                    }

                    // Tab 2: Display & GPU
                    if (ImGui::BeginTabItem("Display & GPU", nullptr, tab_flags(2))) {
                        selected_tab = 2;
                        ImGui::Spacing();
                        DrawDisplayGpuPanel(pctx);
                        ImGui::EndTabItem();
                    }

                    // Tab 3: Audio
                    if (ImGui::BeginTabItem("Audio", nullptr, tab_flags(3))) {
                        selected_tab = 3;
                        ImGui::Spacing();
                        DrawAudioPanel(pctx);
                        ImGui::EndTabItem();
                    }

                    // Tab 4: Controls (Mouse & Keyboard + Controller)
                    if (ImGui::BeginTabItem("Controls", nullptr, tab_flags(4))) {
                        selected_tab = 4;
                        ImGui::Spacing();
                        DrawControlsPanel(pctx);
                        ImGui::EndTabItem();
                    }

                    // Tab 5: Tools & Diagnostics
                    if (ImGui::BeginTabItem("Tools & Diagnostics", nullptr, tab_flags(5))) {
                        selected_tab = 5;
                        ImGui::Spacing();
                        SharedUiOptions& o = GetSharedUiOptions();
                        if (ImGui::Checkbox("Show Live FPS, Frame-Time & Upscaler HUD In-Game", &o.show_fps_hud)) {
                            app::Save();
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Show Active Cheats Badge on HUD##LnchCheatsHud", &o.show_cheats_hud)) {
                            app::Save();
                        }
                        if (ImGui::Checkbox("Automatically Check for Updates on Startup", &o.check_updates_on_startup)) {
                            app::Save();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Open GitHub Releases")) {
                            OpenReleasePageInBrowser(SUFFERING_RELEASES_URL);
                            set_status(std::string("Opened GitHub Releases: ") + SUFFERING_RELEASES_URL, false);
                        }
                        ImGui::Separator();
                        ImGui::Text("Shader / PSO Cache on Disk: %.2f MB (cache\\)",
                                    double(cache_bytes) / (1024.0 * 1024.0));
                        ImGui::TextDisabled(
                            "Startup Pre-Compilation: Enabled (parallel shader & PSO warmup runs automatically on launch)");
                        if (ImGui::Button("Clear Shader & PSO Cache")) {
                            fs::path cdir = app::ExeDir() / "cache";
                            std::error_code rec;
                            for (auto& e : fs::directory_iterator(cdir, rec)) {
                                if (e.is_regular_file(rec)) fs::remove(e.path(), rec);
                            }
                            cache_bytes = ComputeCacheDiskBytes();
                            set_status("Cleared cache\\ shader and PSO files.", false);
                        }
                        ImGui::Separator();
                        ImGui::TextUnformatted("Folders & Windows Integration:");
                        if (ImGui::Button("Open Save Data")) {
                            OpenDirectoryInExplorer(hwnd, app::ExeDir() / "hdd" / "E" / "UDATA" / "4d570015",
                                                    "Save Data", set_status);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Open Logs")) {
                            OpenDirectoryInExplorer(hwnd, app::ExeDir() / "logs", "Logs", set_status);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Open CrashDumps")) {
                            OpenDirectoryInExplorer(hwnd, app::ExeDir() / "CrashDumps", "CrashDumps", set_status);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Open plugins\\")) {
                            OpenDirectoryInExplorer(hwnd, app::ExeDir() / "plugins", "Plugins", set_status);
                        }
                        if (ImGui::Button("Create Desktop Shortcut")) {
                            fs::path out_lnk;
                            std::string err;
                            if (app::CreateShortcut({}, out_lnk, err)) {
                                set_status("Desktop shortcut created: " + out_lnk.string(), false);
                            } else {
                                set_status("Shortcut creation failed: " + err, true);
                            }
                        }
                        ImGui::Separator();
                        ImGui::TextUnformatted("Detected Platform Capabilities:");
                        ImGui::BulletText("Environment: %s", app::IsWineOrProton() ? "Wine / Proton" : "Native Windows");
                        ImGui::BulletText("Selected GPU: %s (Max FL 0x%X, %s)",
                                          active_gpu ? active_gpu->name.c_str() : "Auto",
                                          active_gpu ? active_gpu->max_feature_level : 0xB000u,
                                          (active_gpu && active_gpu->likely_uma) ? "UMA / Integrated" : "Discrete");
                        ImGui::BulletText("Vendor Plugins: DLSS [%s]  |  FSR 3.1 [%s]  |  XeSS [%s]",
                                          plugins.dlss_ready ? "Ready" : (plugins.dlss_driver_ngx ? "NGX OK / Need DLL" : "Absent"),
                                          plugins.fsr3_ready ? "Ready" : "Need DLL",
                                          plugins.xess_ready ? "Ready" : "Need DLL");
                        ImGui::BulletText("Parallel Compiler Slots: %u (RAM-bounded)", compile_jobs);
                        ImGui::Separator();
                        ImGui::TextUnformatted("About & Third-Party Credits:");
                        ImGui::TextWrapped(
                            "Wrapper & UI licensed under GPL-2.0-or-later (with Runtime Upscaler Plugin Exception). "
                            "Powered by XboxXBERecomp (MIT), SDL3 (zlib), Dear ImGui (MIT), Zydis (MIT), and LLVM. "
                            "Uses NVIDIA DLSS / NGX SDK (NVIDIA, GeForce RTX, and DLSS are trademarks of NVIDIA "
                            "Corporation), AMD FidelityFX SDK (FSR 1.0 / CAS / FSR 3.1), and Intel(R) XeSS SDK. "
                            "See LICENSE and THIRD_PARTY_NOTICES.md for full terms.");
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();

            // --- Sticky Footer ---
            ImGui::Separator();
            if (!status_banner.empty()) {
                ImGui::TextColored(status_is_err ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.45f, 0.92f, 0.55f, 1.0f),
                                   "%s", status_banner.c_str());
            }

            if (ImGui::Checkbox("Skip Launcher on Startup (hold Shift to re-open)", &s.skip_launcher)) {
                app::Save();
            }

            float btn_reset_w = 122.0f * lctx.ui_scale;
            float btn_exit_w = 78.0f * lctx.ui_scale;
            float btn_play_w = 148.0f * lctx.ui_scale;
            float right_total = btn_reset_w + btn_exit_w + btn_play_w + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            float avail = ImGui::GetContentRegionAvail().x;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - right_total - 16.0f));
            (void)avail;

            if (ImGui::Button("Reset Defaults", ImVec2(btn_reset_w, 0.0f))) {
                ResetAllSettingsToDefaults();
                set_status("All settings reset to safe defaults (disc path preserved).", false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Exit", ImVec2(btn_exit_w, 0.0f))) {
                lctx.launch_game = false;
                lctx.quit = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!disc.valid);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.14f, 0.16f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.18f, 0.20f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.88f, 0.22f, 0.24f, 1.00f));
            if (ImGui::Button("PLAY GAME", ImVec2(btn_play_w, 0.0f))) {
                app::Save();
                lctx.launch_game = true;
                lctx.quit = true;
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            if (!disc.valid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Select a valid Xbox disc image (.xiso.iso / .iso) in 'Play & Game Disc' first.");
            }
        }
        ImGui::End();

        // Render D3D12 frame
        ImGui::Render();
        uint32_t bi = lctx.swap->GetCurrentBackBufferIndex();
        lctx.WaitForGpu();
        lctx.alloc[bi]->Reset();
        lctx.cl->Reset(lctx.alloc[bi].Get(), nullptr);

        D3D12_RESOURCE_BARRIER b0{};
        b0.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b0.Transition = {lctx.bb[bi].Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, D3D12_RESOURCE_STATE_PRESENT,
                         D3D12_RESOURCE_STATE_RENDER_TARGET};
        lctx.cl->ResourceBarrier(1, &b0);

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = lctx.rtv_heap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(bi) * lctx.rtv_inc;
        const float clear_col[4] = {0.06f, 0.06f, 0.08f, 1.0f};
        lctx.cl->ClearRenderTargetView(rtv, clear_col, 0, nullptr);
        lctx.cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        ID3D12DescriptorHeap* heaps[] = {lctx.srv_heap.Get()};
        lctx.cl->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), lctx.cl.Get());

        D3D12_RESOURCE_BARRIER b1 = b0;
        b1.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b1.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        lctx.cl->ResourceBarrier(1, &b1);
        lctx.cl->Close();

        ID3D12CommandList* lists[] = {lctx.cl.Get()};
        lctx.queue->ExecuteCommandLists(1, lists);
        lctx.swap->Present(1, 0);

        // Zero-flicker reveal: show window only AFTER Frame 1 has been presented!
        if (!first_frame_shown) {
            first_frame_shown = true;
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        }

        frames_rendered++;
        if (!cli.capture_bmp.empty() && !capture_done && frames_rendered >= 10) {
            capture_done = true;
            lctx.CaptureBackbufferBmp(bi, cli.capture_bmp);
            if (cli.exit_after_capture) {
                lctx.launch_game = false;
                lctx.quit = true;
            }
        }
    }

    if (rumble_active) input::Rumble(0, 0, 0);
    if (lctx.launch_game) {
        app::Save();
    } else {
        ShutdownUpdateChecker();
    }

    // Complete teardown & GPU flush before returning to wWinMain
    lctx.WaitForGpu();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    lctx.ShutdownD3D12();
    g_lctx = nullptr;
    DestroyWindow(hwnd);
    UnregisterClassW(class_name.c_str(), inst);

    LOGR("launcher: closed (launch_game=%s)", lctx.launch_game ? "true" : "false");
    return lctx.launch_game;
}
#else
bool RunLauncher(void* /*hinstance*/) {
    LauncherCli cli = ParseLauncherCli();
    LOGR("launcher: opening Native Linux Pre-Game Graphical Launcher (initial_tab=%d, capture=%s, deck=%s, gamescope=%s)",
         cli.initial_tab, cli.capture_bmp.empty() ? "none" : cli.capture_bmp.string().c_str(),
         IsSteamDeck() ? "true" : "false", IsGamescope() ? "true" : "false");

    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
            LOGW("launcher: SDL_InitSubSystem(VIDEO) failed (%s); continuing directly to game", SDL_GetError());
            return true;
        }
    }

    const bool deck_or_scope = IsSteamDeck() || IsGamescope();
    SDL_DisplayID did = static_cast<SDL_DisplayID>(reinterpret_cast<uintptr_t>(app::TargetMonitor()));
    if (!did) did = SDL_GetPrimaryDisplay();
    SDL_Rect usable{0, 0, 1280, 800};
    if (did) SDL_GetDisplayUsableBounds(did, &usable);
    int work_w = std::max(640, usable.w);
    int work_h = std::max(480, usable.h);

    int init_w = deck_or_scope ? work_w : std::min(920, int(work_w * 0.92));
    int init_h = deck_or_scope ? work_h : std::min(630, int(work_h * 0.92));
    SDL_WindowFlags wflags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    if (deck_or_scope) {
        wflags |= SDL_WINDOW_FULLSCREEN;
    } else {
        wflags |= SDL_WINDOW_RESIZABLE;
    }

    SDL_Window* win =
        SDL_CreateWindow("The Suffering - PC Recompilation Launcher (" SUFFERING_VERSION_TAG ")", init_w, init_h, wflags);
    if (!win) {
        LOGW("launcher: SDL_CreateWindow failed (%s); continuing directly to game", SDL_GetError());
        return true;
    }
    TrySetSdlLauncherIcon(win);

    float ui_scale = ComputeWindowDpiScale(win);
    if (!deck_or_scope) {
        int scaled_w = std::min(int(std::lround(920.0f * ui_scale)), int(work_w * 0.92));
        int scaled_h = std::min(int(std::lround(630.0f * ui_scale)), int(work_h * 0.92));
        SDL_SetWindowSize(win, scaled_w, scaled_h);
        SDL_SetWindowPosition(win, usable.x + (work_w - scaled_w) / 2, usable.y + (work_h - scaled_h) / 2);
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(win, nullptr);
    if (!renderer) {
        // Headless/software fallback (e.g. Xvfb without hardware GL)
        renderer = SDL_CreateRenderer(win, "software");
    }
    if (!renderer) {
        LOGW("launcher: SDL_CreateRenderer failed (%s); continuing directly to game", SDL_GetError());
        SDL_DestroyWindow(win);
        return true;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    UiFonts fonts{};
    LoadCrispUiFonts(io, ui_scale, fonts);
    ApplyDarkIndustrialStyle(ui_scale);
    ImGui_ImplSDL3_InitForSDLRenderer(win, renderer);
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    ImGui_ImplSDLRenderer3_Init(renderer);

    input::Init();

    std::vector<app::GpuAdapterInfo> gpus = app::EnumerateGpuAdapters();
    std::vector<app::MonitorInfo> monitors = app::EnumerateMonitors();
    std::vector<app::Mode> resolutions;
    std::string res_device;
    RefreshMonitorLists(monitors, resolutions, res_device, app::Get().monitor);

    uint64_t total_ram = ReadLinuxTotalRamBytes();
    long cpus_raw = sysconf(_SC_NPROCESSORS_ONLN);
    unsigned long logical_cpus = (cpus_raw > 0) ? static_cast<unsigned long>(cpus_raw) : 4ul;
    uint32_t compile_jobs = app::RecommendedCompileJobs();
    app::Mode desktop = app::Desktop();
    uint32_t desk_hz = app::DisplayRefreshHz();

    const app::GpuAdapterInfo* active_gpu = FindSelectedGpu(gpus, app::Get().gpu_adapter);
    upscale::PluginAvailability plugins = upscale::ProbeUpscalerPlugins(active_gpu ? active_gpu->vendor_id : 0u, 0u);
    app::HardwarePreset recommended_preset = DetectHardwarePreset(active_gpu, desktop, total_ram);

    DiscStatus disc = CheckCurrentDiscStatus();
    LOGR("launcher: disc status valid=%s (%s)", disc.valid ? "true" : "false", disc.summary.c_str());
    std::error_code ec;
    bool so_ready = fs::is_regular_file(app::ExeDir() / "libsuffering.so", ec);
    uint64_t cache_bytes = ComputeCacheDiskBytes();

    std::string status_banner;
    bool status_is_err = false;
    auto set_status = [&](const std::string& msg, bool err) {
        status_banner = msg;
        status_is_err = err;
    };

    MaybeStartLauncherUpdateCheck();
    LauncherUpdateUiTracker upd_tracker{};
    upd_tracker.prev_state = GetUpdateSnapshot().state;

    XisoFileBrowser file_browser;
    if (cli.open_file_browser) file_browser.Open();

    bool quit = false;
    bool launch_game = false;
    bool rumble_active = false;
    std::chrono::steady_clock::time_point rumble_until{};
    bool was_pad_active = false;
    bool prev_l1 = false, prev_r1 = false, prev_start = false;
    int selected_tab = cli.initial_tab;
    int requested_tab = cli.initial_tab;
    bool first_frame_shown = false;
    uint32_t frames_rendered = 0;
    bool capture_done = false;

    while (!quit) {
        SDL_Event ev{};
        while (SDL_PollEvent(&ev)) {
            if (MkbHandleRebindMessage(ev.type, 0, reinterpret_cast<intptr_t>(&ev))) continue;
            ImGui_ImplSDL3_ProcessEvent(&ev);
            if (ev.type == SDL_EVENT_QUIT ||
                (ev.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && ev.window.windowID == SDL_GetWindowID(win))) {
                quit = true;
                launch_game = false;
            } else if (ev.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED &&
                       ev.window.windowID == SDL_GetWindowID(win)) {
                float new_scale = ComputeWindowDpiScale(win);
                if (std::fabs(new_scale - ui_scale) > 0.05f) {
                    ui_scale = new_scale;
                    LoadCrispUiFonts(io, ui_scale, fonts);
                    ApplyDarkIndustrialStyle(ui_scale);
                    ImGui_ImplSDLRenderer3_DestroyFontsTexture();
                    ImGui_ImplSDLRenderer3_CreateFontsTexture();
                }
            }
        }
        if (quit) break;

        if (rumble_active && std::chrono::steady_clock::now() >= rumble_until) {
            rumble_active = false;
            input::Rumble(0, 0, 0);
        }

        input::Pad pad = input::Poll(0);
        FeedPadToImGui(pad, true, was_pad_active);
        bool l1 = pad.connected && pad.analog[5] > 64;
        bool r1 = pad.connected && pad.analog[4] > 64;
        bool start = pad.connected && ((pad.buttons & (1u << 4)) != 0);
        if (!file_browser.IsOpen()) {
            if (l1 && !prev_l1) requested_tab = (selected_tab + 5) % 6;
            if (r1 && !prev_r1) requested_tab = (selected_tab + 1) % 6;
            if (start && !prev_start && disc.valid) {
                launch_game = true;
                quit = true;
            }
        }
        prev_l1 = l1;
        prev_r1 = r1;
        prev_start = start;

        app::Settings& s = app::Get();
        RefreshMonitorLists(monitors, resolutions, res_device, s.monitor);
        active_gpu = FindSelectedGpu(gpus, s.gpu_adapter);
        UpdateStatusSnapshot upd_snap = GetUpdateSnapshot();
        PollLauncherUpdateTransitions(upd_tracker, upd_snap, set_status);

        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        int win_w = 920, win_h = 630;
        SDL_GetWindowSize(win, &win_w, &win_h);
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(float(win_w), float(win_h)), ImGuiCond_Always);
        constexpr ImGuiWindowFlags kRootFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                                ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("##SufferingLauncherRoot", nullptr, kRootFlags)) {
            if (fonts.heading) ImGui::PushFont(fonts.heading);
            ImGui::TextColored(ImVec4(0.96f, 0.34f, 0.34f, 1.0f), "THE SUFFERING");
            ImGui::SameLine();
            ImGui::TextUnformatted("- PC Recompilation Launcher " SUFFERING_VERSION_TAG);
            if (fonts.heading) ImGui::PopFont();
            DrawLauncherHeaderUpdateButton(upd_snap, ui_scale, [&] {
                if (!GetSharedUiOptions().dismissed_update_tag.empty()) {
                    GetSharedUiOptions().dismissed_update_tag.clear();
                    if (!HasMockUpdateEnv()) app::Save();
                }
                upd_tracker.manual_pending = true;
                set_status("Checking GitHub Releases for updates...", false);
                StartUpdateCheck(true);
            });

            std::string gpu_line = active_gpu ? active_gpu->name : "Default Vulkan 1.2 GPU";
            double vram_gb = active_gpu ? (double(active_gpu->dedicated_vram) / (1024.0 * 1024.0 * 1024.0)) : 0.0;
            double ram_gb = double(total_ram) / (1024.0 * 1024.0 * 1024.0);
            ImGui::TextDisabled("Hardware: %s (%.1f GB %s)  |  %lu Threads  |  %.1f GB RAM  |  %ux%u @ %u Hz%s",
                                gpu_line.c_str(), vram_gb, (active_gpu && active_gpu->likely_uma) ? "UMA" : "VRAM",
                                logical_cpus, ram_gb, desktop.width, desktop.height, desk_hz,
                                IsSteamDeck() ? "  |  Steam Deck" : "");

            // One-click Hardware Preset bar
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.36f, 0.22f, 0.95f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.22f, 0.50f, 0.30f, 1.00f));
            std::string auto_lbl = std::string("Auto-Detect Preset (") + HardwarePresetName(recommended_preset) + ")";
            if (ImGui::Button(auto_lbl.c_str())) {
                ApplyHardwarePreset(recommended_preset, active_gpu, plugins, desktop);
                set_status(std::string("Applied Auto-Detected Hardware Preset: ") +
                               HardwarePresetName(recommended_preset),
                           false);
            }
            ImGui::PopStyleColor(2);
            static constexpr app::HardwarePreset kTiers[] = {
                app::HardwarePreset::HandheldIgpu, app::HardwarePreset::Mainstream1080p,
                app::HardwarePreset::High1440p, app::HardwarePreset::Ultra4k};
            for (app::HardwarePreset tier : kTiers) {
                ImGui::SameLine();
                bool active = (s.hardware_preset == tier);
                if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.56f, 0.16f, 0.18f, 1.0f));
                if (ImGui::Button(HardwarePresetName(tier))) {
                    ApplyHardwarePreset(tier, active_gpu, plugins, desktop);
                    set_status(std::string("Applied Hardware Preset: ") + HardwarePresetName(tier), false);
                }
                if (active) ImGui::PopStyleColor();
            }
            ImGui::Separator();

            float footer_h = (status_banner.empty() ? 52.0f : 78.0f) * ui_scale;
            if (ImGui::BeginChild("##LauncherBody", ImVec2(0.0f, -footer_h), false)) {
                bool show_upd_banner = DrawLauncherUpdateAlertIfNeeded(upd_snap, ui_scale, set_status, launch_game,
                                                                       quit);

                UiPanelContext pctx{};
                pctx.in_launcher = true;
                pctx.gpus = &gpus;
                pctx.monitors = &monitors;
                pctx.resolutions = &resolutions;
                pctx.res_device = &res_device;
                pctx.plugins = &plugins;
                pctx.set_status = set_status;
                pctx.on_test_rumble = [&] {
                    rumble_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
                    rumble_active = true;
                    input::Rumble(0, 45000, 45000);
                };

                auto tab_flags = [&](int idx) -> ImGuiTabItemFlags {
                    if (requested_tab == idx) {
                        requested_tab = -1;
                        return ImGuiTabItemFlags_SetSelected;
                    }
                    return 0;
                };

                if (ImGui::BeginTabBar("##LauncherTabs")) {
                    // Tab 0: Play & Game Disc
                    if (ImGui::BeginTabItem("Play & Game Disc", nullptr, tab_flags(0))) {
                        selected_tab = 0;
                        ImGui::Spacing();
                        ImGui::TextUnformatted("Original Xbox Disc Image Status (Opened Strictly Read-Only):");
                        float disc_card_h = (show_upd_banner ? 94.0f : 114.0f) * ui_scale;
                        if (ImGui::BeginChild("##DiscCard", ImVec2(0.0f, disc_card_h), true)) {
                            if (disc.valid) {
                                ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.45f, 1.0f), "READY: %s",
                                                   disc.summary.c_str());
                                ImGui::TextWrapped("Path: %s", disc.path.string().c_str());
                                if (!disc.val.sha1.empty()) {
                                    ImGui::TextDisabled("SHA-1: %s", disc.val.sha1.c_str());
                                }
                            } else {
                                ImGui::TextColored(ImVec4(0.95f, 0.38f, 0.35f, 1.0f), "DISC IMAGE REQUIRED");
                                ImGui::TextWrapped("%s", disc.summary.c_str());
                            }
                        }
                        ImGui::EndChild();

                        if (ImGui::Button("Browse Disc Image (.xiso.iso / .iso)...")) {
                            file_browser.Open(disc.path);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Re-Check Disc Image")) {
                            disc = CheckCurrentDiscStatus();
                            set_status("Re-scanned disc image configuration.", false);
                        }

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::TextUnformatted("First-Launch Native Code Status:");
                        if (so_ready) {
                            ImGui::TextColored(ImVec4(0.35f, 0.92f, 0.45f, 1.0f),
                                               "libsuffering.so is compiled and ready for instant launch.");
                        } else {
                            ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.32f, 1.0f),
                                               "libsuffering.so will be compiled once on first launch using %u RAM-safe parallel clang jobs.",
                                               compile_jobs);
                        }
                        ImGui::TextDisabled("Recommended RAM-bounded compile concurrency: %u jobs (override with --compile-jobs N)",
                                            compile_jobs);

                        ImGui::Spacing();
                        ImGui::Separator();
                        ImGui::TextUnformatted("Active Configuration Summary:");
                        app::Mode out = app::Output();
                        bool wide = (out.width * 2 >= out.height * 3);
                        float aspect = wide ? 16.0f / 9.0f : 4.0f / 3.0f;
                        uint32_t vpw = uint32_t(std::lround(std::min(float(out.width), float(out.height) * aspect)));
                        uint32_t vph = uint32_t(std::lround(float(vpw) / aspect));
                        uint32_t rw = 0, rh = 0;
                        upscale::ResolveUpscaleDimensions(s.upscaler, s.upscale_quality, s.render_percent,
                                                          s.aspect_correct_render, vpw, vph, 640, 480, rw, rh);
                        ImGui::BulletText("Upscaler: %s  |  Quality: %s  |  Sharpness: %u%%",
                                          upscale::UpscalerModeName(s.upscaler),
                                          upscale::UpscaleQualityName(s.upscale_quality), s.upscale_sharpness);
                        ImGui::BulletText("Resolution: %ux%u Internal -> %ux%u Viewport (%ux%u Output, %s)", rw, rh, vpw,
                                          vph, out.width, out.height, PresentModeName(s.present_mode));
                        ImGui::BulletText("Controls: Mouse/Keyboard or Gamepad (L1/R1 switch tabs, START plays)");
                        ImGui::EndTabItem();
                    }

                    // Tab 1: Upscaling & Graphics
                    if (ImGui::BeginTabItem("Upscaling & Graphics", nullptr, tab_flags(1))) {
                        selected_tab = 1;
                        ImGui::Spacing();
                        DrawUpscalingGraphicsPanel(pctx);
                        ImGui::EndTabItem();
                    }

                    // Tab 2: Display & GPU
                    if (ImGui::BeginTabItem("Display & GPU", nullptr, tab_flags(2))) {
                        selected_tab = 2;
                        ImGui::Spacing();
                        DrawDisplayGpuPanel(pctx);
                        ImGui::EndTabItem();
                    }

                    // Tab 3: Audio
                    if (ImGui::BeginTabItem("Audio", nullptr, tab_flags(3))) {
                        selected_tab = 3;
                        ImGui::Spacing();
                        DrawAudioPanel(pctx);
                        ImGui::EndTabItem();
                    }

                    // Tab 4: Controls
                    if (ImGui::BeginTabItem("Controls", nullptr, tab_flags(4))) {
                        selected_tab = 4;
                        ImGui::Spacing();
                        DrawControlsPanel(pctx);
                        ImGui::EndTabItem();
                    }

                    // Tab 5: Tools & Diagnostics
                    if (ImGui::BeginTabItem("Tools & Diagnostics", nullptr, tab_flags(5))) {
                        selected_tab = 5;
                        ImGui::Spacing();
                        SharedUiOptions& o = GetSharedUiOptions();
                        if (ImGui::Checkbox("Show Live FPS, Frame-Time & Upscaler HUD In-Game", &o.show_fps_hud)) {
                            app::Save();
                        }
                        ImGui::SameLine();
                        if (ImGui::Checkbox("Show Active Cheats Badge on HUD##LnchCheatsHud", &o.show_cheats_hud)) {
                            app::Save();
                        }
                        if (ImGui::Checkbox("Automatically Check for Updates on Startup", &o.check_updates_on_startup)) {
                            app::Save();
                        }
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Open GitHub Releases")) {
                            OpenReleasePageInBrowser(SUFFERING_RELEASES_URL);
                            if (IsGamescope()) {
                                set_status(std::string("GitHub Releases: ") + SUFFERING_RELEASES_URL +
                                               " (browse in Desktop Mode if hidden)",
                                           false);
                            } else {
                                set_status(std::string("Opened GitHub Releases: ") + SUFFERING_RELEASES_URL, false);
                            }
                        }
                        ImGui::Separator();
                        ImGui::Text("Shader / PSO Cache on Disk: %.2f MB (cache/)",
                                    double(cache_bytes) / (1024.0 * 1024.0));
                        ImGui::TextDisabled(
                            "Startup Pre-Compilation: Enabled (parallel shader & PSO warmup runs automatically on launch)");
                        if (ImGui::Button("Clear Shader & PSO Cache")) {
                            fs::path cdir = app::ExeDir() / "cache";
                            std::error_code rec;
                            for (auto& e : fs::directory_iterator(cdir, rec)) {
                                if (e.is_regular_file(rec)) fs::remove(e.path(), rec);
                            }
                            cache_bytes = ComputeCacheDiskBytes();
                            set_status("Cleared cache/ shader and PSO files.", false);
                        }
                        ImGui::Separator();
                        ImGui::TextUnformatted("Folders & Desktop Integration:");
                        if (ImGui::Button("Open Save Data")) {
                            OpenDirectoryInExplorer(win, app::ExeDir() / "hdd" / "E" / "UDATA" / "4d570015",
                                                    "Save Data", set_status);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Open Logs")) {
                            OpenDirectoryInExplorer(win, app::ExeDir() / "logs", "Logs", set_status);
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Open CrashDumps")) {
                            OpenDirectoryInExplorer(win, app::ExeDir() / "CrashDumps", "CrashDumps", set_status);
                        }
                        if (ImGui::Button("Create Desktop Entry (.desktop)")) {
                            fs::path out_lnk;
                            std::string err;
                            if (app::CreateShortcut({}, out_lnk, err)) {
                                set_status("Desktop entry created: " + out_lnk.string(), false);
                            } else {
                                set_status("Desktop entry creation failed: " + err, true);
                            }
                        }
                        ImGui::Separator();
                        ImGui::TextUnformatted("Detected Platform Capabilities:");
                        ImGui::BulletText("Environment: Native Linux x86_64 (Vulkan 1.2 + SDL3)%s%s",
                                          IsSteamDeck() ? " [Steam Deck]" : "", IsGamescope() ? " [Gamescope]" : "");
                        ImGui::BulletText("Selected GPU: %s (%s)", active_gpu ? active_gpu->name.c_str() : "Auto",
                                          (active_gpu && active_gpu->is_software)
                                              ? "Software Vulkan"
                                              : ((active_gpu && active_gpu->likely_uma) ? "UMA / Integrated" : "Discrete"));
                        ImGui::BulletText("Built-In Vulkan Upscalers: FSR 1.0, NIS, CAS, Bicubic, Integer, Bilinear");
                        ImGui::BulletText("Parallel Compiler Slots: %u (RAM-bounded)", compile_jobs);
                        ImGui::Separator();
                        ImGui::TextUnformatted("About & Third-Party Credits:");
                        ImGui::TextWrapped(
                            "Wrapper & UI licensed under GPL-2.0-or-later (with Runtime Upscaler Plugin Exception). "
                            "Powered by XboxXBERecomp (MIT), SDL3 (zlib), Dear ImGui (MIT), Zydis (MIT), and LLVM. "
                            "Uses NVIDIA DLSS / NGX SDK (NVIDIA, GeForce RTX, and DLSS are trademarks of NVIDIA "
                            "Corporation), AMD FidelityFX SDK (FSR 1.0 / CAS / FSR 3.1), and Intel(R) XeSS SDK. "
                            "See LICENSE and THIRD_PARTY_NOTICES.md for full terms.");
                        ImGui::EndTabItem();
                    }

                    ImGui::EndTabBar();
                }
            }
            ImGui::EndChild();

            // Embedded In-App Gamepad + Touch + Mouse XISO File Browser Modal
            if (file_browser.IsOpen()) {
                fs::path picked;
                auto sys_dlg = [&](fs::path& out) -> bool { return app::BrowseDiscImage(win, out); };
                if (file_browser.Draw(ui_scale, picked, sys_dlg)) {
                    app::DiscValidation v{};
                    if (app::ValidateDiscImage(picked, v)) {
                        s.iso_path = picked.string();
                        app::Save();
                        disc = CheckCurrentDiscStatus();
                        set_status("Validated & saved disc image: " + picked.filename().string(), false);
                    } else {
                        set_status("Invalid disc image: " + v.error, true);
                    }
                }
            }

            // --- Sticky Footer ---
            ImGui::Separator();
            if (!status_banner.empty()) {
                ImGui::TextColored(status_is_err ? ImVec4(0.95f, 0.35f, 0.35f, 1.0f) : ImVec4(0.45f, 0.92f, 0.55f, 1.0f),
                                   "%s", status_banner.c_str());
            }

            if (ImGui::Checkbox("Skip Launcher on Startup (hold Shift to re-open)", &s.skip_launcher)) {
                app::Save();
            }

            float btn_reset_w = 122.0f * ui_scale;
            float btn_exit_w = 78.0f * ui_scale;
            float btn_play_w = 148.0f * ui_scale;
            float right_total = btn_reset_w + btn_exit_w + btn_play_w + ImGui::GetStyle().ItemSpacing.x * 2.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, ImGui::GetWindowWidth() - right_total - 16.0f));

            if (ImGui::Button("Reset Defaults", ImVec2(btn_reset_w, 0.0f))) {
                ResetAllSettingsToDefaults();
                set_status("All settings reset to safe defaults (disc path preserved).", false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Exit", ImVec2(btn_exit_w, 0.0f))) {
                launch_game = false;
                quit = true;
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!disc.valid);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.14f, 0.16f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.78f, 0.18f, 0.20f, 1.00f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.88f, 0.22f, 0.24f, 1.00f));
            if (ImGui::Button("PLAY GAME", ImVec2(btn_play_w, 0.0f))) {
                app::Save();
                launch_game = true;
                quit = true;
            }
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            if (!disc.valid && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Select a valid Xbox disc image (.xiso.iso / .iso) in 'Play & Game Disc' first.");
            }
        }
        ImGui::End();

        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 15, 15, 20, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);

        frames_rendered++;
        if (!cli.capture_bmp.empty() && !capture_done && frames_rendered >= 10) {
            capture_done = true;
            CaptureSdlRendererBmp(renderer, cli.capture_bmp);
            if (cli.exit_after_capture) {
                launch_game = false;
                quit = true;
            }
        }

        SDL_RenderPresent(renderer);

        if (!first_frame_shown) {
            first_frame_shown = true;
            SDL_ShowWindow(win);
            SDL_RaiseWindow(win);
        }
    }

    if (rumble_active) input::Rumble(0, 0, 0);
    if (launch_game) {
        app::Save();
    } else {
        ShutdownUpdateChecker();
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(win);

    LOGR("launcher: closed (launch_game=%s)", launch_game ? "true" : "false");
    return launch_game;
}
#endif

}  // namespace suffering
