// SPDX-License-Identifier: GPL-2.0-or-later
// Dynamic Intel XeSS 1.3 / 2.0 (libxess.dll) plugin loader.
// Uses Intel XMX on Intel Arc GPUs and universal DP4a on all NVIDIA/AMD/Intel SM 6.4+ D3D12 GPUs.

#include "upscale/upscaler_internal.h"
#include "upscale/xess_abi.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/log.h"

namespace upscale {

namespace {

namespace fs = std::filesystem;

xess::QualitySettings MapXessQuality(app::UpscaleQuality q, uint32_t render_w, uint32_t output_w) {
    switch (q) {
        case app::UpscaleQuality::NativeAA: return xess::kQualityNativeAA;
        case app::UpscaleQuality::UltraQuality: return xess::kQualityUltraQuality;
        case app::UpscaleQuality::Quality: return xess::kQualityQuality;
        case app::UpscaleQuality::Balanced: return xess::kQualityBalanced;
        case app::UpscaleQuality::Performance: return xess::kQualityPerformance;
        case app::UpscaleQuality::UltraPerformance: return xess::kQualityUltraPerformance;
        case app::UpscaleQuality::Custom: {
            float ratio = float(output_w) / float(std::max(render_w, 1u));
            if (ratio <= 1.05f) return xess::kQualityNativeAA;
            if (ratio <= 1.35f) return xess::kQualityUltraQuality;
            if (ratio <= 1.55f) return xess::kQualityQuality;
            if (ratio <= 1.75f) return xess::kQualityBalanced;
            if (ratio <= 2.15f) return xess::kQualityPerformance;
            return xess::kQualityUltraPerformance;
        }
    }
    return xess::kQualityQuality;
}

class XessUpscaler final : public VendorUpscaler {
public:
    ~XessUpscaler() override { Shutdown(); }

    bool Init(ID3D12Device* dev, ID3D12CommandQueue* /*queue*/, const gpu::GpuCaps& caps,
              const gpu::UpscalerHostOps& host) override {
        dev_ = dev;
        host_ = host;
        if (caps.vendor_id != 0x8086u && caps.max_shader_model < uint32_t(D3D_SHADER_MODEL_6_4)) {
            status_ = "Requires Intel Arc (XMX) or SM 6.4+ GPU (DP4a)";
            return false;
        }
        fs::path dll = FindPluginDll({L"libxess.dll"});
        if (dll.empty()) {
            status_ = "Place libxess.dll in plugins\\ folder";
            LOGR("upscale/xess: libxess.dll not found in plugins\\; falling back cleanly");
            return false;
        }

        mod_ = LoadLibraryExW(dll.c_str(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!mod_) mod_ = LoadLibraryW(dll.c_str());
        if (!mod_) {
            status_ = "Failed to load libxess.dll";
            LOGW("upscale/xess: LoadLibraryW(%s) failed (err=%lu)", dll.string().c_str(), GetLastError());
            return false;
        }

        pfn_ver_ = reinterpret_cast<xess::PfnGetVersion>(GetProcAddress(mod_, "xessGetVersion"));
        pfn_create_ = reinterpret_cast<xess::PfnD3D12CreateContext>(GetProcAddress(mod_, "xessD3D12CreateContext"));
        pfn_destroy_ = reinterpret_cast<xess::PfnDestroyContext>(GetProcAddress(mod_, "xessDestroyContext"));
        pfn_opt_ = reinterpret_cast<xess::PfnGetOptimalInputResolution>(
            GetProcAddress(mod_, "xessGetOptimalInputResolution"));
        pfn_init_ = reinterpret_cast<xess::PfnD3D12Init>(GetProcAddress(mod_, "xessD3D12Init"));
        pfn_vel_scale_ = reinterpret_cast<xess::PfnSetVelocityScale>(GetProcAddress(mod_, "xessSetVelocityScale"));
        pfn_jit_scale_ = reinterpret_cast<xess::PfnSetJitterScale>(GetProcAddress(mod_, "xessSetJitterScale"));
        pfn_force_legacy_ =
            reinterpret_cast<xess::PfnForceLegacyScaleFactors>(GetProcAddress(mod_, "xessForceLegacyScaleFactors"));
        pfn_exec_ = reinterpret_cast<xess::PfnD3D12Execute>(GetProcAddress(mod_, "xessD3D12Execute"));

        if (!pfn_create_ || !pfn_destroy_ || !pfn_init_ || !pfn_exec_) {
            status_ = "libxess.dll missing required D3D12 exports";
            LOGW("upscale/xess: %s", status_.c_str());
            Shutdown();
            return false;
        }

        xess::Version ver{};
        if (pfn_ver_ && xess::Succeeded(pfn_ver_(&ver))) {
            char vbuf[64];
            std::snprintf(vbuf, sizeof(vbuf), "Ready (XeSS %u.%u.%u)", ver.major, ver.minor, ver.patch);
            status_ = vbuf;
        } else {
            status_ = "Ready (libxess.dll)";
        }

        int32_t rc = pfn_create_(dev_, &ctx_);
        if (!xess::Succeeded(rc) || !ctx_) {
            char ebuf[96];
            std::snprintf(ebuf, sizeof(ebuf), "xessD3D12CreateContext failed (%d)", rc);
            status_ = ebuf;
            LOGW("upscale/xess: %s", status_.c_str());
            Shutdown();
            return false;
        }
        if (pfn_force_legacy_) {
            int32_t fl_rc = pfn_force_legacy_(ctx_, true);
            if (!xess::Succeeded(fl_rc)) {
                LOGW("upscale/xess: xessForceLegacyScaleFactors(true) returned %d", fl_rc);
            }
        }

        available_ = true;
        LOGR("upscale/xess: loaded %s (%s)", dll.string().c_str(), status_.c_str());
        return true;
    }

    void Shutdown() override {
        if (eval_frames_ > 0) {
            LOGR("upscale/xess: shutdown (executed %llu frames)", (unsigned long long)eval_frames_);
        }
        ReleaseFeature();
        if (ctx_ && pfn_destroy_) {
            if (host_.flush) host_.flush(host_.ctx);
            pfn_destroy_(ctx_);
            ctx_ = nullptr;
        }
        if (mod_) {
            FreeLibrary(mod_);
            mod_ = nullptr;
        }
        available_ = false;
    }

    bool IsAvailable() const override { return available_; }
    std::string StatusMessage() const override { return status_; }

    bool EnsureFeature(ID3D12GraphicsCommandList* /*cl*/, uint32_t render_w, uint32_t render_h, uint32_t output_w,
                       uint32_t output_h, app::UpscaleQuality quality, app::DlssPreset /*dlss_preset*/) override {
        if (!available_ || !ctx_) return false;
        if (inited_ && cur_rw_ == render_w && cur_rh_ == render_h && cur_ow_ == output_w && cur_oh_ == output_h &&
            cur_quality_ == quality) {
            return true;
        }
        if (inited_ && host_.flush) host_.flush(host_.ctx);

        xess::D3D12InitParams ip{};
        ip.outputResolution = {output_w, output_h};
        ip.qualitySetting = MapXessQuality(quality, render_w, output_w);
        ip.initFlags = xess::kInitInvertedDepth | xess::kInitUseResponsivePixelMask | xess::kInitLdrInputColor;
        ip.pPipelineLibrary =
            host_.get_pipeline_library ? static_cast<ID3D12PipelineLibrary*>(host_.get_pipeline_library(host_.ctx))
                                       : nullptr;

        int32_t rc = pfn_init_(ctx_, &ip);
        if (!xess::Succeeded(rc)) {
            LOGW("upscale/xess: xessD3D12Init(%ux%u) failed (%d)", output_w, output_h, rc);
            inited_ = false;
            return false;
        }
        if (pfn_vel_scale_) pfn_vel_scale_(ctx_, float(render_w), float(render_h));
        if (pfn_jit_scale_) pfn_jit_scale_(ctx_, 1.0f, 1.0f);

        inited_ = true;
        cur_rw_ = render_w;
        cur_rh_ = render_h;
        cur_ow_ = output_w;
        cur_oh_ = output_h;
        cur_quality_ = quality;
        LOGR("upscale/xess: initialized (%ux%u -> %ux%u, quality=%s)", render_w, render_h, output_w, output_h,
             UpscaleQualityName(quality));
        return true;
    }

    bool Evaluate(const VendorEvalDesc& d) override {
        if (!available_ || !ctx_ || !inited_) return false;
        xess::D3D12ExecuteParams ep{};
        ep.pColorTexture = d.color;
        ep.pVelocityTexture = d.motion_vectors;
        ep.pDepthTexture = d.depth;
        ep.pResponsivePixelMaskTexture = d.reactive_mask;
        ep.pOutputTexture = d.output;
        ep.jitterOffsetX = d.jitter_x;
        ep.jitterOffsetY = d.jitter_y;
        ep.exposureScale = 1.0f;
        ep.resetHistory = d.reset_history ? 1u : 0u;
        ep.inputWidth = d.render_w;
        ep.inputHeight = d.render_h;
        int32_t rc = pfn_exec_(ctx_, d.cl, &ep);
        if (xess::Succeeded(rc)) {
            if (eval_frames_ == 0) {
                LOGR("upscale/xess: first xessD3D12Execute succeeded (%ux%u -> %ux%u)", d.render_w, d.render_h,
                     d.output_w, d.output_h);
            }
            ++eval_frames_;
            return true;
        }
        if (eval_frames_ == 0) {
            LOGW("upscale/xess: xessD3D12Execute failed (%d)", rc);
        }
        return false;
    }

    void ReleaseFeature() override { inited_ = false; }

private:
    ID3D12Device* dev_ = nullptr;
    gpu::UpscalerHostOps host_{};
    HMODULE mod_ = nullptr;
    bool available_ = false;
    bool inited_ = false;
    std::string status_ = "Unavailable";

    xess::PfnGetVersion pfn_ver_ = nullptr;
    xess::PfnD3D12CreateContext pfn_create_ = nullptr;
    xess::PfnDestroyContext pfn_destroy_ = nullptr;
    xess::PfnGetOptimalInputResolution pfn_opt_ = nullptr;
    xess::PfnD3D12Init pfn_init_ = nullptr;
    xess::PfnSetVelocityScale pfn_vel_scale_ = nullptr;
    xess::PfnSetJitterScale pfn_jit_scale_ = nullptr;
    xess::PfnForceLegacyScaleFactors pfn_force_legacy_ = nullptr;
    xess::PfnD3D12Execute pfn_exec_ = nullptr;

    xess::ContextHandle ctx_ = nullptr;
    uint32_t cur_rw_ = 0, cur_rh_ = 0, cur_ow_ = 0, cur_oh_ = 0;
    uint64_t eval_frames_ = 0;
    app::UpscaleQuality cur_quality_ = app::UpscaleQuality::Quality;
};

}  // namespace

std::unique_ptr<VendorUpscaler> CreateXessUpscaler() { return std::make_unique<XessUpscaler>(); }

}  // namespace upscale
