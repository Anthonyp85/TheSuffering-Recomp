// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
// Dynamic AMD FidelityFX Super Resolution 3.1 / FSR 4 (ffx_api) plugin loader.
// Searches plugins\ then ExeDir\ for amd_fidelityfx_loader_dx12.dll -> amd_fidelityfx_upscaler_dx12.dll
// -> amd_fidelityfx_dx12.dll. Runs on ALL NVIDIA/AMD/Intel SM 6.2+ D3D12 GPUs.

#include "upscale/ffx_abi.h"
#include "upscale/upscaler_internal.h"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/log.h"

namespace upscale {

namespace {

namespace fs = std::filesystem;

ffx::SurfaceFormat MapFfxFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
        case DXGI_FORMAT_B8G8R8A8_UNORM: return ffx::kFormatB8G8R8A8Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM: return ffx::kFormatR8G8B8A8Unorm;
        case DXGI_FORMAT_R16G16_FLOAT: return ffx::kFormatR16G16Float;
        case DXGI_FORMAT_R8_UNORM: return ffx::kFormatR8Unorm;
        case DXGI_FORMAT_R32_FLOAT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return ffx::kFormatR32Float;
        default: return ffx::kFormatR8G8B8A8Unorm;
    }
}

ffx::Resource WrapResource(ID3D12Resource* res, uint32_t w, uint32_t h, ffx::SurfaceFormat fmt, uint32_t state,
                           uint32_t usage) {
    ffx::Resource r{};
    r.resource = res;
    r.description.type = ffx::kTypeTexture2D;
    r.description.format = fmt;
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.usage = usage;
    r.state = state;
    return r;
}

class Fsr3Upscaler final : public VendorUpscaler {
public:
    ~Fsr3Upscaler() override { Shutdown(); }

    bool Init(ID3D12Device* dev, ID3D12CommandQueue* /*queue*/, const gpu::GpuCaps& caps,
              const gpu::UpscalerHostOps& host) override {
        dev_ = dev;
        host_ = host;
        if (caps.max_shader_model < uint32_t(D3D_SHADER_MODEL_6_2)) {
            status_ = "Requires D3D12 Shader Model 6.2+";
            return false;
        }
        fs::path dll = FindFfxPluginDll();
        if (dll.empty()) {
            status_ = "Place amd_fidelityfx_*_dx12.dll in plugins\\ folder";
            LOGR("upscale/fsr3: no AMD FidelityFX DLL found in plugins\\; falling back cleanly");
            return false;
        }

        mod_ = LoadLibraryExW(dll.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod_) mod_ = LoadLibraryW(dll.c_str());
        if (!mod_) {
            status_ = "Failed to load " + dll.filename().string();
            LOGW("upscale/fsr3: LoadLibraryW(%s) failed (err=%lu)", dll.string().c_str(), GetLastError());
            return false;
        }

        pfn_create_ = reinterpret_cast<ffx::PfnCreateContext>(GetProcAddress(mod_, "ffxCreateContext"));
        pfn_destroy_ = reinterpret_cast<ffx::PfnDestroyContext>(GetProcAddress(mod_, "ffxDestroyContext"));
        pfn_dispatch_ = reinterpret_cast<ffx::PfnDispatch>(GetProcAddress(mod_, "ffxDispatch"));
        pfn_query_ = reinterpret_cast<ffx::PfnQuery>(GetProcAddress(mod_, "ffxQuery"));
        pfn_configure_ = reinterpret_cast<ffx::PfnConfigure>(GetProcAddress(mod_, "ffxConfigure"));

        if (!pfn_create_ || !pfn_destroy_ || !pfn_dispatch_) {
            status_ = dll.filename().string() + " missing ffx_api C exports";
            LOGW("upscale/fsr3: %s", status_.c_str());
            Shutdown();
            return false;
        }

        available_ = true;
        dll_name_ = dll.filename().string();
        status_ = "Ready (" + dll_name_ + ")";
        LOGR("upscale/fsr3: loaded %s", dll.string().c_str());
        return true;
    }

    void Shutdown() override {
        if (eval_frames_ > 0) {
            LOGR("upscale/fsr3: shutdown (dispatched %llu frames)", (unsigned long long)eval_frames_);
        }
        ReleaseFeature();
        if (mod_) {
            FreeLibrary(mod_);
            mod_ = nullptr;
        }
        available_ = false;
    }

    bool IsAvailable() const override { return available_; }
    std::string StatusMessage() const override { return status_; }

    bool EnsureFeature(ID3D12GraphicsCommandList* /*cl*/, uint32_t /*render_w*/, uint32_t /*render_h*/,
                       uint32_t output_w, uint32_t output_h, app::UpscaleQuality /*quality*/,
                       app::DlssPreset /*dlss_preset*/) override {
        if (!available_ || !dev_) return false;
        if (ctx_ && cur_ow_ == output_w && cur_oh_ == output_h) return true;
        ReleaseFeature();

        ffx::CreateBackendDx12Desc backend{};
        backend.device = dev_;

        ffx::CreateContextDescUpscale up{};
        up.header.pNext = &backend.header;
        up.flags = ffx::kEnableDepthInverted | ffx::kEnableDepthInfinite | ffx::kEnableAutoExposure;
        up.maxRenderSize = {output_w, output_h};
        up.maxUpscaleSize = {output_w, output_h};

        ffx::ReturnCode rc = pfn_create_(&ctx_, &up.header, nullptr);
        if (rc != ffx::kOk || !ctx_) {
            LOGW("upscale/fsr3: ffxCreateContext(%ux%u) failed (rc=%u)", output_w, output_h, rc);
            ctx_ = nullptr;
            return false;
        }
        cur_ow_ = output_w;
        cur_oh_ = output_h;
        std::string provider_ver;
        if (pfn_query_) {
            ffx::QueryGetProviderVersion qpv{};
            if (pfn_query_(&ctx_, &qpv.header) == ffx::kOk && qpv.versionName && *qpv.versionName) {
                provider_ver = qpv.versionName;
                status_ = "Ready (FSR " + provider_ver + ")";
            }
        }
        if (!provider_ver.empty()) {
            LOGR("upscale/fsr3: context ready (max=%ux%u, dll=%s, provider=%s)", output_w, output_h,
                 dll_name_.c_str(), provider_ver.c_str());
        } else {
            LOGR("upscale/fsr3: context ready (max=%ux%u, dll=%s)", output_w, output_h, dll_name_.c_str());
        }
        return true;
    }

    bool Evaluate(const VendorEvalDesc& d) override {
        if (!available_ || !ctx_) return false;
        ffx::DispatchDescUpscale desc{};
        desc.commandList = d.cl;
        desc.color = WrapResource(d.color, d.render_w, d.render_h, MapFfxFormat(d.color_fmt), ffx::kStateComputeRead,
                                  ffx::kUsageReadOnly);
        uint32_t depth_usage = ffx::kUsageDepthTarget;
        if (d.depth) {
            DXGI_FORMAT dfmt = d.depth->GetDesc().Format;
            if (dfmt == DXGI_FORMAT_R32G8X24_TYPELESS || dfmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT ||
                dfmt == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS) {
                depth_usage |= ffx::kUsageStencilTarget;
            }
        }
        desc.depth = WrapResource(d.depth, d.render_w, d.render_h, ffx::kFormatR32Float, ffx::kStateComputeRead,
                                  depth_usage);
        desc.motionVectors = WrapResource(d.motion_vectors, d.render_w, d.render_h, ffx::kFormatR16G16Float,
                                          ffx::kStateComputeRead, ffx::kUsageReadOnly);
        if (d.reactive_mask) {
            desc.reactive = WrapResource(d.reactive_mask, d.render_w, d.render_h, ffx::kFormatR8Unorm,
                                         ffx::kStateComputeRead, ffx::kUsageReadOnly);
        }
        desc.output = WrapResource(d.output, d.output_w, d.output_h, ffx::kFormatR8G8B8A8Unorm,
                                   ffx::kStateUnorderedAccess, ffx::kUsageUav);
        desc.jitterOffset = {d.jitter_x, d.jitter_y};
        desc.motionVectorScale = {float(d.render_w), float(d.render_h)};
        desc.renderSize = {d.render_w, d.render_h};
        desc.upscaleSize = {d.output_w, d.output_h};
        desc.enableSharpening = d.sharpness > 0.001f;
        desc.sharpness = std::clamp(d.sharpness, 0.0f, 1.0f);
        desc.frameTimeDelta = std::clamp(d.camera.dt_ms, 1.0f, 100.0f);
        desc.preExposure = 1.0f;
        desc.reset = d.reset_history;
        desc.cameraNear = d.camera.near_plane > 0.0f ? d.camera.near_plane : 32.0f;
        desc.cameraFar = FLT_MAX;
        desc.cameraFovAngleVertical = std::clamp(d.camera.fov_y_rad, 0.1f, 3.0f);
        desc.viewSpaceToMetersFactor = 0.0254f;
        desc.flags = ffx::kDispatchNone;

        ffx::ReturnCode rc = pfn_dispatch_(&ctx_, &desc.header);
        if (rc == ffx::kOk) {
            if (eval_frames_ == 0) {
                LOGR("upscale/fsr3: first ffxDispatch succeeded (%ux%u -> %ux%u)", d.render_w, d.render_h, d.output_w,
                     d.output_h);
            }
            ++eval_frames_;
            return true;
        }
        if (eval_frames_ == 0) {
            LOGW("upscale/fsr3: ffxDispatch failed (rc=%u)", rc);
        }
        return false;
    }

    void ReleaseFeature() override {
        if (ctx_ && pfn_destroy_) {
            if (host_.flush) host_.flush(host_.ctx);
            pfn_destroy_(&ctx_, nullptr);
            ctx_ = nullptr;
        }
    }

private:
    ID3D12Device* dev_ = nullptr;
    gpu::UpscalerHostOps host_{};
    HMODULE mod_ = nullptr;
    bool available_ = false;
    std::string dll_name_;
    std::string status_ = "Unavailable";

    ffx::PfnCreateContext pfn_create_ = nullptr;
    ffx::PfnDestroyContext pfn_destroy_ = nullptr;
    ffx::PfnDispatch pfn_dispatch_ = nullptr;
    ffx::PfnQuery pfn_query_ = nullptr;
    ffx::PfnConfigure pfn_configure_ = nullptr;

    ffx::Context ctx_ = nullptr;
    uint32_t cur_ow_ = 0, cur_oh_ = 0;
    uint64_t eval_frames_ = 0;
};

}  // namespace

std::unique_ptr<VendorUpscaler> CreateFsr3Upscaler() { return std::make_unique<Fsr3Upscaler>(); }

}  // namespace upscale
