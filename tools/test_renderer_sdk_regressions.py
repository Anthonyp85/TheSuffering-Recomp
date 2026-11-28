#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile and exercise production SDK patch functions with recording platform fakes.

No game assets, window, or GPU are needed. The tested C++ bodies are extracted
from the distributable patch on every run; the fakes only replace OS/GPU calls.
This checks CPU state/lifetime ordering and render-pass API arguments, not GPU
execution. Use the normal game replay plus Vulkan validation for that coverage.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def patched_fragments(patch, path):
    section = patch.split(f"diff --git a/{path} b/{path}\n", 1)[1].split("\ndiff --git ", 1)[0]
    return "\n".join(line[1:] for line in section.splitlines()
                     if line.startswith(("+", " ")) and not line.startswith("+++"))


def block(source, start):
    begin = source.index(start)
    opening = source.index("{", begin)
    depth = 1
    pos = opening + 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    return source[begin:pos]


PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#define LOGE(...) ((void)0)
#define LOGR(...) ((void)0)
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
int failures = 0;
template<class F> void Test(const char* name, F fn) {
    try { fn(); std::printf("PASS %s\n", name); }
    catch (const std::exception& e) { ++failures; std::printf("FAIL %s: %s\n", name, e.what()); }
}
namespace app {
@APP_TYPES@
Settings settings;
Settings& Get() { return settings; }
void* Window() { return reinterpret_cast<void*>(1); }
bool Widescreen() { return true; }
int DisplayRefreshHz() { return 60; }
}
'''


RENDERER_FAKES = r'''
int window_w = 1280, window_h = 720;
bool minimized = false, resize_ok = true, gpu_idle = false;
using HWND = void*;
using HRESULT = long;
constexpr HRESULT S_OK = 0;
bool FAILED(HRESULT hr) { return hr < 0; }
struct RECT { long left = 0, top = 0, right = 0, bottom = 0; };
bool IsIconic(HWND) { return minimized; }
bool GetClientRect(HWND, RECT* rc) { rc->right = window_w; rc->bottom = window_h; return true; }
struct SDL_Window {};
constexpr int SDL_WINDOW_MINIMIZED = 1;
int SDL_GetWindowFlags(SDL_Window*) { return minimized ? SDL_WINDOW_MINIMIZED : 0; }
void SDL_GetWindowSizeInPixels(SDL_Window*, int* w, int* h) { *w = window_w; *h = window_h; }
void ImGui_ImplVulkan_SetMinImageCount(uint32_t) {}
constexpr int DXGI_FORMAT_B8G8R8A8_UNORM = 0;
struct FakeResource { void Reset() {} void* Get() { return this; } };
struct FakeDevice { HRESULT GetDeviceRemovedReason() { return 0; } };
struct FakeSwap { HRESULT ResizeBuffers(int, int, int, int, int) { return resize_ok ? 0 : -1; } };
struct Upscaler {
    int resets = 0, prewarms = 0;
    uint32_t rw = 0, rh = 0, vw = 0, vh = 0;
    void ResolveRenderSize(uint32_t w, uint32_t h, uint32_t, uint32_t, bool, uint32_t& x, uint32_t& y) {
        x = w / 2; y = h / 2;
    }
    void ResetHistory() { Check(gpu_idle, "history reset before GPU idle"); ++resets; }
    void Prewarm(void*, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
        Check(gpu_idle, "prewarm before GPU idle"); ++prewarms; rw=a; rh=b; vw=c; vh=d;
    }
};
std::atomic<Upscaler*> g_upscaler{nullptr};
std::atomic<bool> g_cache_reset_requested{false};
namespace texturedumper { bool Update() { return false; } }
struct Target { bool depth = false; unsigned view = 0; };
struct TargetMap : std::unordered_map<uint64_t, Target> {
    void clear() { Check(gpu_idle, "targets destroyed before GPU idle"); std::unordered_map<uint64_t, Target>::clear(); }
};
struct BackendState {
    uint32_t bb_w_=1280, bb_h_=720, guest_w_=640, guest_h_=480;
    uint32_t render_w_=0, render_h_=0, vp_x_=0, vp_y_=0, vp_w_=0, vp_h_=0;
    float scale_=1, scale_x_=1, scale_y_=1;
    bool upscaler_inited_=true, last_aspect_correct_=true;
    app::UpscalerMode last_upscaler_mode_=app::UpscalerMode::Fsr1;
    app::UpscaleQuality last_upscale_quality_=app::UpscaleQuality::NativeAA;
    TargetMap targets_;
    Target* bound_rt_=nullptr; Target* bound_ds_=nullptr;
    int flushes=0;
    void Flush() { gpu_idle = true; ++flushes; }
    void ResetCaches() {}
    void ReleasePackTextures() {}
};
struct Extension { bool Settings() { return false; } template<class T> void Reset(T&) {} };
struct D3D12Backend : BackendState {
    static constexpr int kFrames = 3;
    struct Frame { uint64_t fence=1; };
    std::vector<Frame> frames_{3};
    std::vector<FakeResource> bb_{3};
    FakeSwap swap_value; FakeSwap* swap_=&swap_value;
    FakeDevice dev_value; FakeDevice* dev_=&dev_value;
    FakeResource cl_;
    int swap_flags_=0;
    uint64_t last_scene_color_key_=1, last_scene_depth_key_=2;
    std::vector<unsigned> free_dsv_, free_rtv_;
    Extension* ext_=nullptr;
    void WaitFence(uint64_t) { gpu_idle=true; }
    void CreateBackBufferViews() {}
    void ResizeSwap(); void ApplySettings();
    void ComputeRenderExtents(uint32_t&, uint32_t&, uint32_t&, uint32_t&, uint32_t&, uint32_t&) const;
};
using VkFormat = uint32_t;
using VkRenderPass = uint64_t;
constexpr uint32_t VK_NULL_HANDLE=0, VK_SUCCESS=0, VK_FORMAT_UNDEFINED=0;
constexpr uint32_t VK_SAMPLE_COUNT_1_BIT=1, VK_ATTACHMENT_LOAD_OP_LOAD=1, VK_ATTACHMENT_LOAD_OP_DONT_CARE=2;
constexpr uint32_t VK_ATTACHMENT_STORE_OP_STORE=1, VK_ATTACHMENT_STORE_OP_DONT_CARE=2;
constexpr uint32_t VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL=1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL=2;
constexpr uint32_t VK_PIPELINE_BIND_POINT_GRAPHICS=1, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO=1;
constexpr uint32_t VK_SUBPASS_EXTERNAL=~0u, VK_DEPENDENCY_BY_REGION_BIT=1;
constexpr uint32_t VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT=1, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT=2;
constexpr uint32_t VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT=4;
constexpr uint32_t VK_ACCESS_COLOR_ATTACHMENT_READ_BIT=1, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT=2;
constexpr uint32_t VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT=4, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT=8;
struct VkAttachmentDescription {
    uint32_t flags=0, format=0, samples=0, loadOp=0, storeOp=0, stencilLoadOp=0, stencilStoreOp=0;
    uint32_t initialLayout=0, finalLayout=0;
};
struct VkAttachmentReference { uint32_t attachment=0, layout=0; };
struct VkSubpassDescription {
    uint32_t pipelineBindPoint=0, colorAttachmentCount=0;
    const VkAttachmentReference *pColorAttachments=nullptr, *pDepthStencilAttachment=nullptr;
};
struct VkSubpassDependency {
    uint32_t srcSubpass=0, dstSubpass=0, srcStageMask=0, dstStageMask=0, srcAccessMask=0, dstAccessMask=0, dependencyFlags=0;
};
struct VkRenderPassCreateInfo {
    uint32_t sType=0, attachmentCount=0;
    const VkAttachmentDescription* pAttachments=nullptr;
    uint32_t subpassCount=0;
    const VkSubpassDescription* pSubpasses=nullptr;
    uint32_t dependencyCount=0;
    const VkSubpassDependency* pDependencies=nullptr;
};
struct VulkanApi {
    std::vector<VkSubpassDependency> dependencies;
    void dev_wait_idle(void*) { gpu_idle=true; }
    int create_rp(void*, const VkRenderPassCreateInfo* ci, void*, VkRenderPass* out) {
        dependencies.clear();
        for (uint32_t i=0; i<ci->dependencyCount; ++i) {
            const auto& d=ci->pDependencies[i];
            Check(d.srcStageMask && d.dstStageMask, "legacy subpass dependency has an invalid empty stage mask");
            dependencies.push_back(d);
        }
        *out=1; return VK_SUCCESS;
    }
};
struct VulkanBackend : BackendState {
    VulkanApi vk_;
    void* dev_=nullptr; void* cmd_=nullptr;
    bool swapchain_dirty_=false, imgui_inited_=false;
    app::PresentMode last_present_mode_req_=app::PresentMode::Auto;
    int last_display_hz_=60, last_fps_cap_req_=60;
    std::vector<int> swap_images_{1,2,3};
    VkFormat swap_fmt_=1;
    VkRenderPass present_rp_=0;
    std::unordered_map<uint64_t, VkRenderPass> rp_cache_;
    bool CreateSwapchain(uint32_t w, uint32_t h) {
        if (!resize_ok) return false;
        bb_w_=w; bb_h_=h; swapchain_dirty_=false; return true;
    }
    void ReleaseTarget(TargetMap::iterator it) {
        Check(gpu_idle, "targets released before GPU idle"); targets_.erase(it);
    }
    void ResizeSwap(); void ApplySettings();
    void ComputeRenderExtents(uint32_t&, uint32_t&, uint32_t&, uint32_t&, uint32_t&, uint32_t&) const;
    VkRenderPass GetRenderPass(VkFormat, VkFormat);
    bool CreatePresentRenderPass();
};
template<class T> void CheckResize(bool with_upscaler) {
    app::settings={}; minimized=false; resize_ok=true; window_w=1280; window_h=720;
    app::settings.render_percent=50;
    Upscaler up; g_upscaler=with_upscaler ? &up : nullptr;
    T b;
    b.ComputeRenderExtents(b.render_w_,b.render_h_,b.vp_x_,b.vp_y_,b.vp_w_,b.vp_h_);
    const uint32_t sizes[][6]={{1920,1080,960,540,1920,1080},{800,600,400,240,800,450},{1280,800,640,360,1280,720}};
    for (const auto& s : sizes) {
        b.targets_.emplace(1, Target{});
        auto old_rw=b.render_w_, old_rh=b.render_h_;
        window_w=s[0]; window_h=s[1]; gpu_idle=false;
        b.ResizeSwap();
        Check(b.targets_.size()==1, "resize destroyed the front target before presentation");
        Check(b.render_w_==old_rw && b.render_h_==old_rh, "resize hid pending scene-target invalidation");
        gpu_idle=false; // The resized backbuffer is now presented before ApplySettings.
        const int before=b.flushes;
        b.ApplySettings();
        Check(b.flushes>before, "settings update did not wait for in-flight targets");
        Check(b.targets_.empty(), "scene targets survived a resize");
        Check(b.render_w_==s[2] && b.render_h_==s[3], "incorrect render dimensions");
        Check(b.vp_w_==s[4] && b.vp_h_==s[5], "incorrect viewport dimensions");
        Check(b.vp_x_==(s[0]-s[4])/2 && b.vp_y_==(s[1]-s[5])/2, "viewport letterboxing stayed stale");
        Check(std::fabs(b.scale_x_-float(s[2])/640)<1e-5f && std::fabs(b.scale_y_-float(s[3])/480)<1e-5f,
              "coordinate scales stayed stale");
        Check(b.scale_==b.scale_y_, "legacy coordinate scale stayed stale");
        if (with_upscaler)
            Check(up.rw==s[2] && up.rh==s[3] && up.vw==s[4] && up.vh==s[5], "upscaler prewarm used old dimensions");
    }
    int expected_prewarm=with_upscaler ? 3 : 0;
    Check(up.resets==expected_prewarm && up.prewarms==expected_prewarm, "resize must reset/prewarm history exactly once");
    int before=b.flushes;
    b.ResizeSwap(); b.ApplySettings();
    Check(b.flushes==before && up.prewarms==expected_prewarm, "unchanged size needlessly invalidated targets");
    minimized=true; window_w=0; window_h=0; b.ResizeSwap(); b.ApplySettings();
    Check(b.bb_w_==1280 && b.bb_h_==800 && b.flushes==before, "minimizing destroyed valid extents");
    minimized=false; g_upscaler=nullptr;
}
void CheckDependencies(const VulkanApi& api, uint32_t stages, uint32_t writes, uint32_t reads) {
    bool found=false;
    for (const auto& d : api.dependencies) {
        if (d.srcSubpass==VK_SUBPASS_EXTERNAL && d.dstSubpass==0 &&
            (d.srcStageMask & stages)==stages && (d.dstStageMask & stages)==stages &&
            (d.srcAccessMask & writes)==writes && (d.dstAccessMask & (reads|writes))==(reads|writes)) found=true;
    }
    Check(found, "missing attachment write-to-read/write memory dependency across render passes");
}
'''


def renderer_code(d3d, vk):
    code = RENDERER_FAKES
    for name, source in (("D3D12Backend", d3d), ("VulkanBackend", vk)):
        for function in ("ComputeRenderExtents", "ResizeSwap", "ApplySettings"):
            code += "\n" + block(source, f"void {name}::{function}(")
    code += "\n" + block(vk, "VkRenderPass VulkanBackend::GetRenderPass(")
    present = block(vk, "    {\n        VkAttachmentDescription att{};")
    code += "\nbool VulkanBackend::CreatePresentRenderPass() " + present[:-1] + "return true;\n}\n"
    code += r'''
void RendererTests() {
    Test("D3D12 resize/scale/target lifetime/history", [] { CheckResize<D3D12Backend>(true); CheckResize<D3D12Backend>(false); });
    Test("Vulkan resize/scale/target lifetime/history", [] { CheckResize<VulkanBackend>(true); CheckResize<VulkanBackend>(false); });
    Test("Vulkan upscaler/blit to UI attachment dependency", [] {
        VulkanBackend b; Check(b.CreatePresentRenderPass(), "render-pass creation failed");
        CheckDependencies(b.vk_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
    });
    Test("Vulkan ordinary color/depth render-pass reentry", [] {
        VulkanBackend b;
        for (unsigned color : {0u,1u}) for (unsigned depth : {0u,2u}) {
            b.GetRenderPass(color, depth);
            if (color) CheckDependencies(b.vk_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT);
            if (depth) CheckDependencies(b.vk_, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
        }
    });
}
'''
    return code


def settings_code(settings):
    # Table's entire production initializer is the serialization seam; TOML IO
    # and the unrelated per-game options loop are outside this test's scope.
    table_start = settings.index("toml::table Table() {")
    initializer = settings[table_start:settings.index("    for (const Option& o : kGame.options)", table_start)]
    variables = settings[settings.index("const char* kModes[]"):settings.index("std::atomic<uint32_t> g_guest_core_idx")]
    code = r'''
namespace toml { using value=std::variant<std::string, int64_t, bool>; using table=std::unordered_map<std::string,value>; }
namespace app {
'''
    code += variables + "\n" + initializer + "return t;\n}\n"
    for name in ("DisplayMode", "Resolution", "SkipLauncher"):
        code += block(settings, f"void Mark{name}UserEdited()") + "\n"
    code += r'''
}
void SettingsTests() {
    Test("CLI-only overrides remain temporary during unrelated saves", [] {
        using namespace app;
        settings={}; g_cli_display_mode=g_cli_width=g_cli_height=g_cli_skip_launcher=true;
        g_saved_display_mode=DisplayMode::Borderless; g_saved_width=1280; g_saved_height=720; g_saved_skip_launcher=false;
        settings.display_mode=DisplayMode::Windowed; settings.width=800; settings.height=600; settings.skip_launcher=true;
        auto t=Table();
        Check(std::get<std::string>(t.at("display_mode"))=="borderless", "CLI display mode leaked to saved settings");
        Check(std::get<int64_t>(t.at("width"))==1280 && std::get<int64_t>(t.at("height"))==720, "CLI resolution leaked");
        Check(!std::get<bool>(t.at("skip_launcher")), "CLI launcher override leaked");
    });
    Test("explicit edits replace CLI guards and serialize new values", [] {
        using namespace app;
        settings.display_mode=DisplayMode::Fullscreen; MarkDisplayModeUserEdited();
        Check(!g_cli_display_mode && g_saved_display_mode==DisplayMode::Fullscreen, "display mode guard/baseline stale");
        Check(g_cli_width && g_cli_height && g_cli_skip_launcher, "display mode edit cleared unrelated guards");
        auto mode_only=Table();
        Check(std::get<std::string>(mode_only.at("display_mode"))=="fullscreen" && std::get<int64_t>(mode_only.at("width"))==1280,
              "display mode edit did not serialize independently");
        settings.width=1920; settings.height=1080; MarkResolutionUserEdited();
        Check(!g_cli_width && !g_cli_height && g_saved_width==1920 && g_saved_height==1080, "resolution guard/baseline stale");
        settings.skip_launcher=true; MarkSkipLauncherUserEdited();
        Check(!g_cli_skip_launcher && g_saved_skip_launcher, "launcher guard/baseline stale");
        auto t=Table();
        Check(std::get<int64_t>(t.at("width"))==1920 && std::get<int64_t>(t.at("height"))==1080, "edited resolution not serialized");
        Check(std::get<bool>(t.at("skip_launcher")), "edited launcher visibility not serialized");
    });
}
'''
    return code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--patch", type=Path, default=ROOT / "patches/xboxxberecomp.patch")
    parser.add_argument("--compiler", default=shutil.which("clang++") or shutil.which("g++"))
    parser.add_argument("--suite", choices=("all", "renderer", "settings"), default="all")
    args = parser.parse_args()
    compiler = args.compiler
    if not compiler and os.name == "nt":
        candidate = Path("C:/Program Files/LLVM/bin/clang++.exe")
        if candidate.is_file():
            compiler = str(candidate)
    if not compiler:
        parser.error("C++ compiler unavailable; supply --compiler")
    patch = args.patch.read_text(encoding="utf-8")
    app = patched_fragments(patch, "runtime/app/app.h")
    types = app[app.index("enum class DisplayMode"):app.index("struct GpuAdapterInfo")]
    types += block(app, "struct Settings") + ";\n"
    source = PRELUDE.replace("@APP_TYPES@", types)
    calls = []
    if args.suite != "settings":
        source += renderer_code(patched_fragments(patch, "runtime/gpu/d3d12.cpp"), patched_fragments(patch, "runtime/gpu/vulkan.cpp"))
        calls.append("RendererTests();")
    if args.suite != "renderer":
        source += settings_code(patched_fragments(patch, "runtime/app/settings.cpp"))
        calls.append("SettingsTests();")
    source += "\nint main() { " + " ".join(calls) + " return failures ? 1 : 0; }\n"
    with tempfile.TemporaryDirectory(prefix="renderer-sdk-tests-") as temp:
        cpp = Path(temp) / "regression.cpp"
        exe = Path(temp) / ("regression.exe" if os.name == "nt" else "regression")
        cpp.write_text(source, encoding="utf-8")
        subprocess.run([compiler, "-std=c++20", "-O0", str(cpp), "-o", str(exe)], check=True)
        return subprocess.run([str(exe)], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
