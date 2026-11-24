// SPDX-License-Identifier: GPL-2.0-or-later
// Dynamic NVIDIA DLSS Super Resolution (DLSS 3.7+ / DLSS 4 Transformer Presets J/K & DLAA) plugin loader.
// Loads DriverStore _nvngx.dll via Windows Registry + user-supplied plugins\nvngx_dlss.dll via clean-room C ABI.

#include "upscale/ngx_abi.h"
#include "upscale/upscaler_internal.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "common/version.h"
#include "core/log.h"

namespace upscale {

namespace {

namespace fs = std::filesystem;

ngx::PerfQuality MapDlssQuality(app::UpscaleQuality q, uint32_t render_w, uint32_t render_h, uint32_t output_w,
                                uint32_t output_h) {
    if (output_w > 0 && output_h > 0 && render_w + 1u >= output_w && render_h + 1u >= output_h) {
        return ngx::kDlaa;
    }
    switch (q) {
        case app::UpscaleQuality::NativeAA: return ngx::kDlaa;
        // NVIDIA reserved NVSDK_NGX_PerfQuality_Value_UltraQuality (4) as unimplemented in nvngx_dlss.dll
        // (returns 0xBAD00010 InvalidParameter); Ultra Quality (77%) uses kMaxQuality with 77% input dimensions.
        case app::UpscaleQuality::UltraQuality: return ngx::kMaxQuality;
        case app::UpscaleQuality::Quality: return ngx::kMaxQuality;
        case app::UpscaleQuality::Balanced: return ngx::kBalanced;
        case app::UpscaleQuality::Performance: return ngx::kMaxPerf;
        case app::UpscaleQuality::UltraPerformance: return ngx::kUltraPerformance;
        case app::UpscaleQuality::Custom: {
            float ratio = float(output_w) / float(std::max(render_w, 1u));
            if (ratio <= 1.03f) return ngx::kDlaa;
            if (ratio <= 1.58f) return ngx::kMaxQuality;
            if (ratio <= 1.85f) return ngx::kBalanced;
            if (ratio <= 2.35f) return ngx::kMaxPerf;
            return ngx::kUltraPerformance;
        }
    }
    return ngx::kMaxQuality;
}

uint32_t MapDlssPreset(app::DlssPreset p) {
    switch (p) {
        case app::DlssPreset::Auto: return ngx::kPresetDefault;
        case app::DlssPreset::PresetE: return ngx::kPresetE;
        case app::DlssPreset::PresetJ: return ngx::kPresetJ;
        case app::DlssPreset::PresetK: return ngx::kPresetK;
    }
    return ngx::kPresetK;
}

class DlssUpscaler final : public VendorUpscaler {
public:
    ~DlssUpscaler() override { Shutdown(); }

    bool Init(ID3D12Device* dev, ID3D12CommandQueue* /*queue*/, const gpu::GpuCaps& caps,
              const gpu::UpscalerHostOps& host) override {
        dev_ = dev;
        host_ = host;
        if (caps.vendor_id != 0x10DEu) {
            status_ = "Requires NVIDIA RTX GPU";
            return false;
        }
        fs::path ngx_core = FindNvidiaNgxCoreDll();
        if (ngx_core.empty()) {
            status_ = "NVIDIA NGX driver runtime (_nvngx.dll) not found";
            LOGR("upscale/dlss: %s", status_.c_str());
            return false;
        }
        LOGR("upscale/dlss: discovered DriverStore _nvngx.dll at %s", ngx_core.string().c_str());

        bool using_driver_cache = false;
        fs::path dlss_dll = FindPluginDll({L"nvngx_dlss.dll"});
        if (dlss_dll.empty() && IsNgxDriverFallbackAllowed()) {
            dlss_dll = StageNvidiaNgxDriverDlssDll();
            using_driver_cache = !dlss_dll.empty();
        }
        if (dlss_dll.empty()) {
            status_ = "Place nvngx_dlss.dll in plugins\\ folder (DriverStore _nvngx.dll ready)";
            LOGR("upscale/dlss: _nvngx.dll found, but nvngx_dlss.dll absent from plugins\\; falling back cleanly");
            return false;
        }

        ngx_mod_ = LoadLibraryExW(ngx_core.c_str(), nullptr,
                                  LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!ngx_mod_) {
            ngx_mod_ = LoadLibraryW(ngx_core.c_str());
        }
        if (!ngx_mod_) {
            status_ = "Failed to load _nvngx.dll";
            LOGW("upscale/dlss: LoadLibraryW(%s) failed (err=%lu)", ngx_core.string().c_str(), GetLastError());
            return false;
        }

        pfn_init_ = reinterpret_cast<ngx::PfnInitProjectId>(GetProcAddress(ngx_mod_, "NVSDK_NGX_D3D12_Init_ProjectID"));
        pfn_get_caps_ = reinterpret_cast<ngx::PfnGetCapabilityParameters>(
            GetProcAddress(ngx_mod_, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
        pfn_alloc_ =
            reinterpret_cast<ngx::PfnAllocateParameters>(GetProcAddress(ngx_mod_, "NVSDK_NGX_D3D12_AllocateParameters"));
        pfn_destroy_ =
            reinterpret_cast<ngx::PfnDestroyParameters>(GetProcAddress(ngx_mod_, "NVSDK_NGX_D3D12_DestroyParameters"));
        pfn_create_ =
            reinterpret_cast<ngx::PfnCreateFeature>(GetProcAddress(ngx_mod_, "NVSDK_NGX_D3D12_CreateFeature"));
        pfn_eval_ =
            reinterpret_cast<ngx::PfnEvaluateFeature>(GetProcAddress(ngx_mod_, "NVSDK_NGX_D3D12_EvaluateFeature"));
        pfn_release_ =
            reinterpret_cast<ngx::PfnReleaseFeature>(GetProcAddress(ngx_mod_, "NVSDK_NGX_D3D12_ReleaseFeature"));
        pfn_shutdown_ = reinterpret_cast<ngx::PfnShutdown1>(GetProcAddress(ngx_mod_, "NVSDK_NGX_D3D12_Shutdown1"));

        if (!pfn_init_ || !pfn_get_caps_ || !pfn_alloc_ || !pfn_destroy_ || !pfn_create_ || !pfn_eval_ ||
            !pfn_release_ || !pfn_shutdown_) {
            status_ = "_nvngx.dll missing required D3D12 exports";
            LOGW("upscale/dlss: %s", status_.c_str());
            Shutdown();
            return false;
        }

        std::vector<fs::path> search_dirs = PluginSearchDirs();
        if (using_driver_cache) {
            search_dirs.push_back(dlss_dll.parent_path());
        }
        std::vector<std::wstring> dir_strings;
        std::vector<const wchar_t*> dir_ptrs;
        for (const auto& d : search_dirs) {
            dir_strings.push_back(d.wstring());
        }
        for (const auto& s : dir_strings) dir_ptrs.push_back(s.c_str());

        ngx::FeatureCommonInfo info{};
        info.path_list.path = dir_ptrs.data();
        info.path_list.length = uint32_t(dir_ptrs.size());

        std::error_code ec;
        fs::path app_data_dir = app::ExeDir() / "logs";
        fs::create_directories(app_data_dir, ec);
        std::wstring app_data = app_data_dir.wstring();
        static constexpr uint32_t kApiVersions[] = {0x0000015u, 0x0000014u, 0x0000013u};
        uint32_t init_res = ngx::kFailBase;
        for (uint32_t ver : kApiVersions) {
            init_res = pfn_init_("4d570015-5788-4b02-9a3e-535546464552", ngx::kEngineCustom, SUFFERING_VERSION_STR,
                                 app_data.c_str(), dev_, ver, &info);
            if (ngx::Succeeded(init_res)) break;
        }
        if (!ngx::Succeeded(init_res)) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "NGX D3D12 init failed (0x%08X)", init_res);
            status_ = buf;
            LOGW("upscale/dlss: %s", status_.c_str());
            Shutdown();
            return false;
        }
        ngx_inited_ = true;

        ngx::Parameter* caps_params = nullptr;
        if (!ngx::Succeeded(pfn_get_caps_(&caps_params)) || !caps_params) {
            status_ = "NGX GetCapabilityParameters failed";
            Shutdown();
            return false;
        }
        int dlss_avail = 0;
        caps_params->Get("SuperSampling.Available", &dlss_avail);
        pfn_destroy_(caps_params);
        caps_params = nullptr;
        if (!dlss_avail) {
            status_ = "DLSS Super Resolution unsupported on this GPU/driver";
            LOGR("upscale/dlss: %s", status_.c_str());
            Shutdown();
            return false;
        }
        if (!ngx::Succeeded(pfn_alloc_(&params_)) || !params_) {
            status_ = "NGX AllocateParameters failed";
            Shutdown();
            return false;
        }

        available_ = true;
        status_ = using_driver_cache ? "Ready (NGX driver model)" : ("Ready (" + dlss_dll.filename().string() + ")");
        LOGR("upscale/dlss: initialized successfully (%s, status=%s)", dlss_dll.string().c_str(), status_.c_str());
        return true;
    }

    void Shutdown() override {
        if (eval_frames_ > 0) {
            LOGR("upscale/dlss: shutdown (evaluated %llu frames)", (unsigned long long)eval_frames_);
        }
        ReleaseFeature();
        if (params_ && pfn_destroy_) {
            pfn_destroy_(params_);
            params_ = nullptr;
        }
        if (ngx_inited_ && pfn_shutdown_ && dev_) {
            uint32_t refcount = 0;
            pfn_shutdown_(dev_, &refcount);
            ngx_inited_ = false;
        }
        if (ngx_mod_) {
            FreeLibrary(ngx_mod_);
            ngx_mod_ = nullptr;
        }
        available_ = false;
    }

    bool IsAvailable() const override { return available_; }
    std::string StatusMessage() const override { return status_; }

    bool EnsureFeature(ID3D12GraphicsCommandList* cl, uint32_t render_w, uint32_t render_h, uint32_t output_w,
                       uint32_t output_h, app::UpscaleQuality quality, app::DlssPreset dlss_preset) override {
        if (!available_ || !params_) return false;
        if (cur_rw_ == render_w && cur_rh_ == render_h && cur_ow_ == output_w && cur_oh_ == output_h &&
            cur_quality_ == quality && cur_preset_ == dlss_preset) {
            if (handle_) return true;
            if (feature_failed_) return false;
        }
        ReleaseFeature();
        feature_failed_ = false;

        auto try_create = [&](ngx::PerfQuality pq) -> uint32_t {
            params_->Reset();
            params_->Set("Width", render_w);
            params_->Set("Height", render_h);
            params_->Set("OutWidth", output_w);
            params_->Set("OutHeight", output_h);
            params_->Set("PerfQualityValue", int(pq));
            uint32_t flags = ngx::kFlagMvLowRes | ngx::kFlagDepthInverted | ngx::kFlagAutoExposure;
            params_->Set("DLSS.Feature.Create.Flags", int(flags));
            params_->Set("CreationNodeMask", 1u);
            params_->Set("VisibilityNodeMask", 1u);

            uint32_t preset = MapDlssPreset(dlss_preset);
            params_->Set("DLSS.Hint.Render.Preset.DLAA", preset);
            params_->Set("DLSS.Hint.Render.Preset.UltraQuality", preset);
            params_->Set("DLSS.Hint.Render.Preset.Quality", preset);
            params_->Set("DLSS.Hint.Render.Preset.Balanced", preset);
            params_->Set("DLSS.Hint.Render.Preset.Performance", preset);
            params_->Set("DLSS.Hint.Render.Preset.UltraPerformance", preset);
            return pfn_create_(cl, ngx::kFeatureSuperSampling, params_, &handle_);
        };

        ngx::PerfQuality mapped_pq = MapDlssQuality(quality, render_w, render_h, output_w, output_h);
        uint32_t r = try_create(mapped_pq);
        if ((!ngx::Succeeded(r) || !handle_) && mapped_pq != ngx::kMaxQuality) {
            handle_ = nullptr;
            ngx::PerfQuality fb_pq =
                (render_w >= output_w && render_h >= output_h) ? ngx::kDlaa : ngx::kMaxQuality;
            if (fb_pq != mapped_pq) r = try_create(fb_pq);
        }

        cur_rw_ = render_w;
        cur_rh_ = render_h;
        cur_ow_ = output_w;
        cur_oh_ = output_h;
        cur_quality_ = quality;
        cur_preset_ = dlss_preset;

        if (!ngx::Succeeded(r) || !handle_) {
            LOGW("upscale/dlss: CreateFeature(%ux%u -> %ux%u) failed (0x%08X)", render_w, render_h, output_w, output_h, r);
            handle_ = nullptr;
            feature_failed_ = true;
            return false;
        }
        // Flush CreateFeature upload commands once so DLSS weights are resident before frame 1
        if (host_.flush) host_.flush(host_.ctx);
        LOGR("upscale/dlss: feature ready (%ux%u -> %ux%u, quality=%s, preset=%s)", render_w, render_h, output_w,
             output_h, UpscaleQualityName(quality), DlssPresetName(dlss_preset));
        return true;
    }

    bool Evaluate(const VendorEvalDesc& d) override {
        if (!available_ || !handle_ || !params_) return false;
        params_->Set("Color", d.color);
        params_->Set("Output", d.output);
        params_->Set("Depth", d.depth);
        params_->Set("MotionVectors", d.motion_vectors);
        if (d.reactive_mask) params_->Set("DLSS.Input.Bias.Current.Color.Mask", d.reactive_mask);
        params_->Set("Jitter.Offset.X", -d.jitter_x);
        params_->Set("Jitter.Offset.Y", -d.jitter_y);
        params_->Set("MV.Scale.X", float(d.render_w));
        params_->Set("MV.Scale.Y", float(d.render_h));
        params_->Set("Reset", d.reset_history ? 1 : 0);
        params_->Set("DLSS.Render.Subrect.Dimensions.Width", d.render_w);
        params_->Set("DLSS.Render.Subrect.Dimensions.Height", d.render_h);
        params_->Set("Sharpness", 0.0f);  // Post-RCAS handles sharpening cleanly for DLSS 3.7+/4
        uint32_t r = pfn_eval_(d.cl, handle_, params_, nullptr);
        if (ngx::Succeeded(r)) {
            if (eval_frames_ == 0) {
                LOGR("upscale/dlss: first EvaluateFeature succeeded (%ux%u -> %ux%u)", d.render_w, d.render_h,
                     d.output_w, d.output_h);
            }
            ++eval_frames_;
            return true;
        }
        return false;
    }

    void ReleaseFeature() override {
        if (handle_ && pfn_release_) {
            if (host_.flush) host_.flush(host_.ctx);
            pfn_release_(handle_);
            handle_ = nullptr;
        }
    }

private:
    ID3D12Device* dev_ = nullptr;
    gpu::UpscalerHostOps host_{};
    HMODULE ngx_mod_ = nullptr;
    bool ngx_inited_ = false;
    bool available_ = false;
    std::string status_ = "Unavailable";

    ngx::PfnInitProjectId pfn_init_ = nullptr;
    ngx::PfnGetCapabilityParameters pfn_get_caps_ = nullptr;
    ngx::PfnAllocateParameters pfn_alloc_ = nullptr;
    ngx::PfnDestroyParameters pfn_destroy_ = nullptr;
    ngx::PfnCreateFeature pfn_create_ = nullptr;
    ngx::PfnEvaluateFeature pfn_eval_ = nullptr;
    ngx::PfnReleaseFeature pfn_release_ = nullptr;
    ngx::PfnShutdown1 pfn_shutdown_ = nullptr;

    ngx::Parameter* params_ = nullptr;
    ngx::Handle* handle_ = nullptr;
    uint32_t cur_rw_ = 0, cur_rh_ = 0, cur_ow_ = 0, cur_oh_ = 0;
    uint64_t eval_frames_ = 0;
    bool feature_failed_ = false;
    app::UpscaleQuality cur_quality_ = app::UpscaleQuality::Quality;
    app::DlssPreset cur_preset_ = app::DlssPreset::PresetK;
};

}  // namespace

std::unique_ptr<VendorUpscaler> CreateDlssUpscaler() { return std::make_unique<DlssUpscaler>(); }

}  // namespace upscale
