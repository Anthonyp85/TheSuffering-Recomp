// SPDX-License-Identifier: GPL-2.0-or-later
// Unified D3D12 Upscaler Manager: resolution decoupling, Halton(2,3) 3D jitter, depth-reprojected
// motion vectors (RG16F), HUD reactive mask (R8_UNORM), 6 built-in zero-DLL spatial upscalers,
// and 3 dynamic vendor temporal/AI plugins (DLSS, FSR 3.1, XeSS) with graceful FSR 1.0 fallback.

#include "upscale/upscaler_common.h"
#include "upscale/upscaler_internal.h"

#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <mutex>
#include <string_view>

#include "core/log.h"

namespace upscale {

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kDescPerFrame = 64;
constexpr uint32_t kNumFrames = 3;
constexpr uint32_t kTotalDescs = kDescPerFrame * kNumFrames;  // 192 descriptors

std::wstring ReadRegString(HKEY root, const wchar_t* subkey, const wchar_t* value_name) {
    HKEY hkey = nullptr;
    if (RegOpenKeyExW(root, subkey, 0, KEY_READ | KEY_WOW64_64KEY, &hkey) != ERROR_SUCCESS) return {};
    DWORD type = 0;
    DWORD bytes = 0;
    std::wstring out;
    if (RegQueryValueExW(hkey, value_name, nullptr, &type, nullptr, &bytes) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ) && bytes >= 2) {
        out.resize(bytes / sizeof(wchar_t));
        if (RegQueryValueExW(hkey, value_name, nullptr, &type, reinterpret_cast<LPBYTE>(out.data()), &bytes) ==
            ERROR_SUCCESS) {
            while (!out.empty() && out.back() == L'\0') out.pop_back();
        } else {
            out.clear();
        }
    }
    RegCloseKey(hkey);
    return out;
}

float Halton(uint32_t index, uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= float(base);
        r += f * float(index % base);
        index /= base;
    }
    return r;
}

void MatMul4d(const double a[16], const double b[16], double out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
    }
}

bool Invert4d(const double m[16], double inv[16]) {
    double a[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            a[r][c] = m[r * 4 + c];
            a[r][4 + c] = (r == c) ? 1.0 : 0.0;
        }
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        double best = std::fabs(a[col][col]);
        for (int r = col + 1; r < 4; ++r) {
            double v = std::fabs(a[r][col]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-12) return false;
        if (piv != col) {
            for (int c = 0; c < 8; ++c) std::swap(a[col][c], a[piv][c]);
        }
        double diag = a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] /= diag;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            double f = a[r][col];
            if (f == 0.0) continue;
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) inv[r * 4 + c] = a[r][4 + c];
    return true;
}

class UpscalerManager final : public gpu::Upscaler {
public:
    bool Init(const gpu::UpscalerInitDesc& desc) override {
        dev_ = static_cast<ID3D12Device*>(desc.device);
        queue_ = static_cast<ID3D12CommandQueue*>(desc.queue);
        caps_ = desc.caps;
        host_ = desc.host;
        auto* cl = static_cast<ID3D12GraphicsCommandList*>(desc.cmd_list);
        if (!dev_ || !cl) return false;

        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kTotalDescs;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
            LOGE("upscale: failed to create compute descriptor heap");
            return false;
        }
        desc_inc_ = dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        if (!InitSpatialPipelines(dev_, cl, host_, spatial_)) {
            LOGE("upscale: spatial pipeline initialization failed");
            return false;
        }

        dlss_ = CreateDlssUpscaler();
        fsr3_ = CreateFsr3Upscaler();
        xess_ = CreateXessUpscaler();
        if (dlss_) dlss_->Init(dev_, queue_, caps_, host_);
        if (fsr3_) fsr3_->Init(dev_, queue_, caps_, host_);
        if (xess_) xess_->Init(dev_, queue_, caps_, host_);

        LOGR("upscale: manager ready (dlss=%s [%s], fsr3=%s [%s], xess=%s [%s])",
             (dlss_ && dlss_->IsAvailable()) ? "ready" : "fallback",
             dlss_ ? dlss_->StatusMessage().c_str() : "n/a",
             (fsr3_ && fsr3_->IsAvailable()) ? "ready" : "fallback",
             fsr3_ ? fsr3_->StatusMessage().c_str() : "n/a",
             (xess_ && xess_->IsAvailable()) ? "ready" : "fallback",
             xess_ ? xess_->StatusMessage().c_str() : "n/a");
        inited_ = true;
        return true;
    }

    void Shutdown() override {
        if (!inited_) return;
        if (dlss_) dlss_->Shutdown();
        if (fsr3_) fsr3_->Shutdown();
        if (xess_) xess_->Shutdown();
        dlss_.reset();
        fsr3_.reset();
        xess_.reset();
        ReleaseTargets();
        spatial_ = SpatialPipelines{};
        heap_.Reset();
        dev_ = nullptr;
        queue_ = nullptr;
        inited_ = false;
    }

    void ResolveRenderSize(uint32_t vp_w, uint32_t vp_h, uint32_t guest_w, uint32_t guest_h, bool /*widescreen*/,
                           uint32_t& out_render_w, uint32_t& out_render_h) override {
        const app::Settings& s = app::Get();
        ResolveUpscaleDimensions(s.upscaler, s.upscale_quality, s.render_percent, s.aspect_correct_render, vp_w, vp_h,
                                 guest_w, guest_h, out_render_w, out_render_h);
    }

    void BeginFrame(uint32_t render_w, uint32_t render_h, uint32_t vp_w, uint32_t vp_h,
                    const gpu::CameraFrameData& cam, bool& out_jitter_active, float& out_jitter_x,
                    float& out_jitter_y) override {
        out_jitter_active = false;
        out_jitter_x = 0.0f;
        out_jitter_y = 0.0f;
        if (!inited_) return;
        const app::Settings& s = app::Get();
        if (render_w > vp_w || render_h > vp_h) return;  // SSAA downscale -> no temporal jitter
        VendorUpscaler* v = ActiveVendor(s.upscaler);
        if (!v || !v->IsAvailable()) return;
        if (!cam.valid || !cam.is_3d_gameplay) {
            need_reset_ = true;
            return;
        }
        double ratio = (render_w > 0) ? (double(vp_w) / double(render_w)) : 1.0;
        uint32_t phase_count = std::clamp(uint32_t(std::ceil(8.0 * ratio * ratio)), 8u, 64u);
        uint32_t idx = (jitter_frame_++ % phase_count) + 1u;
        out_jitter_x = Halton(idx, 2u) - 0.5f;
        out_jitter_y = Halton(idx, 3u) - 0.5f;
        out_jitter_active = true;
    }

    bool Execute(const gpu::UpscalerFrameDesc& f) override {
        if (!inited_ || !f.cmd_list || !f.front_res || !f.backbuffer_res) return false;
        auto* cl = static_cast<ID3D12GraphicsCommandList*>(f.cmd_list);
        const app::Settings& s = app::Get();
        float sharpness = std::clamp(float(s.upscale_sharpness) / 100.0f, 0.0f, 1.0f);

        // Reset per-frame descriptor bump allocator slice
        frame_slot_base_ = uint32_t(f.frame_index % kNumFrames) * kDescPerFrame;
        frame_slot_used_ = 0;

        app::UpscalerMode requested = s.upscaler;
        app::UpscalerMode active = requested;
        bool fallback = false;
        bool temporal_ran = false;
        std::string detail;

        // SSAA (>100% custom scale) routes cleanly through Catmull-Rom 5-Tap Bicubic
        bool is_ssaa = (f.render_w > f.vp_w || f.render_h > f.vp_h);
        if (is_ssaa && requested != app::UpscalerMode::Bilinear && requested != app::UpscalerMode::Integer) {
            active = app::UpscalerMode::Bicubic;
            detail = "SSAA Downscale (Catmull-Rom)";
        }

        // Handle temporal vendor upscalers (DLSS / FSR 3.1 / XeSS)
        if (IsTemporalMode(active)) {
            VendorUpscaler* vendor = ActiveVendor(active);
            if (!vendor || !vendor->IsAvailable()) {
                fallback = true;
                detail = vendor ? (vendor->StatusMessage() + " -> FSR 1.0") : "Plugin unavailable -> FSR 1.0";
                active = app::UpscalerMode::Fsr1;
            } else if (!f.camera.valid || !f.camera.is_3d_gameplay || !f.scene_depth_res) {
                need_reset_ = true;
                detail = "2D/Menu/FMV Spatial Bypass (FSR 1.0)";
                active = app::UpscalerMode::Fsr1;
            } else {
                if (RunTemporalVendor(cl, f, vendor, active, sharpness, detail)) {
                    temporal_ran = true;
                } else {
                    fallback = true;
                    detail = "Vendor Evaluate fallback -> FSR 1.0";
                    active = app::UpscalerMode::Fsr1;
                }
            }
        } else {
            need_reset_ = true;
            has_prev_cam_ = false;
        }

        if (!temporal_ran) {
            RunSpatialMode(cl, f, active, sharpness);
        }

        // Publish live status snapshot
        {
            std::lock_guard<std::mutex> lock(status_lock_);
            status_.requested_mode = uint32_t(requested);
            status_.active_mode = uint32_t(active);
            status_.render_w = f.render_w;
            status_.render_h = f.render_h;
            status_.vp_w = f.vp_w;
            status_.vp_h = f.vp_h;
            status_.bb_w = f.bb_w;
            status_.bb_h = f.bb_h;
            status_.temporal_active = temporal_ran;
            status_.fallback_active = fallback;
            status_.dlss_available = dlss_ && dlss_->IsAvailable();
            status_.fsr3_available = fsr3_ && fsr3_->IsAvailable();
            status_.xess_available = xess_ && xess_->IsAvailable();
            status_.active_label = UpscalerShortName(active);
            status_.status_detail = detail;
        }
        return true;
    }

    void Prewarm(void* cmd_list, uint32_t render_w, uint32_t render_h, uint32_t vp_w, uint32_t vp_h) override {
        if (!inited_ || vp_w == 0 || vp_h == 0) return;
        const app::Settings& s = app::Get();
        float sharpness = std::clamp(float(s.upscale_sharpness) / 100.0f, 0.0f, 1.0f);
        app::UpscalerMode mode = s.upscaler;
        bool is_ssaa = (render_w > vp_w || render_h > vp_h);
        if (is_ssaa && mode != app::UpscalerMode::Bilinear && mode != app::UpscalerMode::Integer) {
            mode = app::UpscalerMode::Bicubic;
        }
        if (IsTemporalMode(mode)) {
            VendorUpscaler* vendor = ActiveVendor(mode);
            if (vendor && vendor->IsAvailable() && render_w > 0 && render_h > 0 && render_w <= vp_w &&
                render_h <= vp_h) {
                bool post_rcas = (mode != app::UpscalerMode::Fsr3) && (sharpness > 0.001f);
                bool need_tmp = post_rcas || (render_w != vp_w && sharpness > 0.001f);
                EnsureOutputTargets(vp_w, vp_h, need_tmp);
                EnsureTemporalTargets(render_w, render_h);
                if (cmd_list) {
                    vendor->EnsureFeature(static_cast<ID3D12GraphicsCommandList*>(cmd_list), render_w, render_h, vp_w,
                                          vp_h, s.upscale_quality, s.dlss_preset);
                }
                return;
            }
            mode = app::UpscalerMode::Fsr1;
        }
        bool same_size = (render_w == vp_w && render_h == vp_h);
        if (mode == app::UpscalerMode::Fsr1) {
            if (same_size && sharpness <= 0.001f) return;
            EnsureOutputTargets(vp_w, vp_h, !same_size && sharpness > 0.001f);
        } else if (mode == app::UpscalerMode::Cas || mode == app::UpscalerMode::Nis ||
                   (mode == app::UpscalerMode::Bicubic && same_size && sharpness > 0.001f)) {
            EnsureOutputTargets(vp_w, vp_h, false);
        }
    }

    void ResetHistory() override {
        need_reset_ = true;
        has_prev_cam_ = false;
        jitter_frame_ = 0;
    }

    gpu::UpscalerStatus GetStatus() const override {
        std::lock_guard<std::mutex> lock(status_lock_);
        return status_;
    }

private:
    static bool IsTemporalMode(app::UpscalerMode m) {
        return m == app::UpscalerMode::Dlss || m == app::UpscalerMode::Fsr3 || m == app::UpscalerMode::Xess;
    }

    VendorUpscaler* ActiveVendor(app::UpscalerMode m) const {
        if (m == app::UpscalerMode::Dlss) return dlss_.get();
        if (m == app::UpscalerMode::Fsr3) return fsr3_.get();
        if (m == app::UpscalerMode::Xess) return xess_.get();
        return nullptr;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t slot) const {
        auto h = heap_->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(slot) * desc_inc_;
        return h;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t slot) const {
        auto h = heap_->GetGPUDescriptorHandleForHeapStart();
        h.ptr += uint64_t(slot) * desc_inc_;
        return h;
    }

    uint32_t AllocSlots(uint32_t n) {
        if (frame_slot_used_ + n > kDescPerFrame) frame_slot_used_ = 0;
        uint32_t s = frame_slot_base_ + frame_slot_used_;
        frame_slot_used_ += n;
        return s;
    }

    bool UploadConst(const void* data, size_t bytes, uint64_t& out_gpu) {
        void* res = nullptr;
        uint64_t off = 0;
        uint8_t* cpu = nullptr;
        uint64_t gpu = 0;
        size_t aligned = (bytes + 255) & ~size_t(255);
        if (!host_.upload(host_.ctx, aligned, 256, &res, &off, &cpu, &gpu) || !cpu) return false;
        std::memset(cpu, 0, aligned);
        std::memcpy(cpu, data, bytes);
        out_gpu = gpu;
        return true;
    }

    void TransitionRes(ID3D12Resource* res, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
        uint32_t st = uint32_t(cur);
        host_.transition(host_.ctx, res, &st, uint32_t(want));
        cur = static_cast<D3D12_RESOURCE_STATES>(st);
    }

    void TransitionHost(void* res, uint32_t* cur, D3D12_RESOURCE_STATES want) {
        host_.transition(host_.ctx, res, cur, uint32_t(want));
    }

    bool CreateUavTex(uint32_t w, uint32_t h, DXGI_FORMAT fmt, ComPtr<ID3D12Resource>& out,
                      D3D12_RESOURCE_STATES& out_state) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        out_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        return SUCCEEDED(
            dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, out_state, nullptr, IID_PPV_ARGS(&out)));
    }

    void ReleaseTargets() {
        upscale_out_.Reset();
        upscale_tmp_.Reset();
        motion_vec_.Reset();
        hud_mask_.Reset();
        out_w_ = out_h_ = 0;
        mv_w_ = mv_h_ = 0;
    }

    bool EnsureOutputTargets(uint32_t vp_w, uint32_t vp_h, bool need_tmp) {
        if (out_w_ != vp_w || out_h_ != vp_h || !upscale_out_) {
            if ((upscale_out_ || upscale_tmp_) && host_.flush) host_.flush(host_.ctx);
            upscale_out_.Reset();
            upscale_tmp_.Reset();
            if (!CreateUavTex(vp_w, vp_h, DXGI_FORMAT_R8G8B8A8_UNORM, upscale_out_, upscale_out_state_)) return false;
            out_w_ = vp_w;
            out_h_ = vp_h;
        }
        if (need_tmp && !upscale_tmp_) {
            if (!CreateUavTex(vp_w, vp_h, DXGI_FORMAT_R8G8B8A8_UNORM, upscale_tmp_, upscale_tmp_state_)) return false;
        }
        return true;
    }

    bool EnsureTemporalTargets(uint32_t rw, uint32_t rh) {
        if (mv_w_ != rw || mv_h_ != rh || !motion_vec_ || !hud_mask_) {
            if ((motion_vec_ || hud_mask_) && host_.flush) host_.flush(host_.ctx);
            motion_vec_.Reset();
            hud_mask_.Reset();
            if (!CreateUavTex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, motion_vec_, motion_vec_state_)) return false;
            if (!CreateUavTex(rw, rh, DXGI_FORMAT_R8_UNORM, hud_mask_, hud_mask_state_)) return false;
            mv_w_ = rw;
            mv_h_ = rh;
        }
        return true;
    }

    void CreateSrv2D(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT fmt) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = fmt;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(res, &sv, CpuHandle(slot));
    }

    void CreateUav2D(uint32_t slot, ID3D12Resource* res, DXGI_FORMAT fmt) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};
        uv.Format = fmt;
        uv.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev_->CreateUnorderedAccessView(res, nullptr, &uv, CpuHandle(slot));
    }

    void DispatchCompute(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso, uint64_t cb_gpu, uint32_t srv_base,
                         uint32_t uav_base, uint32_t w, uint32_t h) {
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetComputeRootSignature(spatial_.cs_root.Get());
        cl->SetPipelineState(pso);
        cl->SetComputeRootConstantBufferView(0, cb_gpu);
        cl->SetComputeRootDescriptorTable(1, GpuHandle(srv_base));
        cl->SetComputeRootDescriptorTable(2, GpuHandle(uav_base));
        cl->Dispatch((w + 7u) / 8u, (h + 7u) / 8u, 1u);
    }

    void PresentToBackbuffer(ID3D12GraphicsCommandList* cl, ID3D12PipelineState* pso, ID3D12Resource* src,
                             DXGI_FORMAT src_fmt, uint32_t src_w, uint32_t src_h, const gpu::UpscalerFrameDesc& f,
                             uint32_t dst_x, uint32_t dst_y, uint32_t dst_w, uint32_t dst_h) {
        TransitionHost(f.lut_res, f.lut_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        TransitionHost(f.backbuffer_res, f.backbuffer_state, D3D12_RESOURCE_STATE_RENDER_TARGET);

        uint32_t srv_slot = AllocSlots(2);
        CreateSrv2D(srv_slot, src, src_fmt);
        CreateSrv2D(srv_slot + 1, static_cast<ID3D12Resource*>(f.lut_res), DXGI_FORMAT_B8G8R8A8_UNORM);

        float cb[8] = {float(src_w), float(src_h), 1.0f / float(std::max(src_w, 1u)),
                       1.0f / float(std::max(src_h, 1u)), f.gamma ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        uint64_t cb_gpu = 0;
        if (!UploadConst(cb, sizeof(cb), cb_gpu)) return;

        D3D12_CPU_DESCRIPTOR_HANDLE rtv{f.backbuffer_rtv_ptr};
        cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        D3D12_VIEWPORT vp{float(dst_x), float(dst_y), float(dst_w), float(dst_h), 0.0f, 1.0f};
        D3D12_RECT sc{0, 0, LONG(f.bb_w), LONG(f.bb_h)};
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);

        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootSignature(spatial_.ps_root.Get());
        cl->SetPipelineState(pso);
        cl->SetGraphicsRootConstantBufferView(0, cb_gpu);
        cl->SetGraphicsRootDescriptorTable(1, GpuHandle(srv_slot));
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->DrawInstanced(3, 1, 0, 0);
    }

    void RunRcasPass(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, DXGI_FORMAT src_fmt, ID3D12Resource* dst,
                     uint32_t w, uint32_t h, float sharpness) {
        uint32_t con[4]{};
        FillFsrRcasConstants(sharpness, con);
        uint64_t cb_gpu = 0;
        if (!UploadConst(con, sizeof(con), cb_gpu)) return;
        uint32_t base = AllocSlots(6);
        CreateSrv2D(base + 0, src, src_fmt);
        CreateSrv2D(base + 1, src, src_fmt);
        CreateSrv2D(base + 2, src, src_fmt);
        CreateSrv2D(base + 3, src, src_fmt);
        CreateUav2D(base + 4, dst, DXGI_FORMAT_R8G8B8A8_UNORM);
        CreateUav2D(base + 5, dst, DXGI_FORMAT_R8G8B8A8_UNORM);
        DispatchCompute(cl, spatial_.fsr_rcas.Get(), cb_gpu, base, base + 4, w, h);
    }

    bool RunTemporalVendor(ID3D12GraphicsCommandList* cl, const gpu::UpscalerFrameDesc& f, VendorUpscaler* vendor,
                           app::UpscalerMode mode, float sharpness, std::string& out_detail) {
        bool post_rcas = (mode != app::UpscalerMode::Fsr3) && (sharpness > 0.001f);
        if (!EnsureOutputTargets(f.vp_w, f.vp_h, post_rcas)) return false;
        if (!EnsureTemporalTargets(f.render_w, f.render_h)) return false;

        // Compute cur_vp_rev = V * P_rev in double precision
        double cur_v[16], cur_p[16], cur_vp[16];
        for (int i = 0; i < 16; ++i) {
            cur_v[i] = double(f.camera.view[i]);
            cur_p[i] = double(f.camera.proj_rev[i]);
        }
        MatMul4d(cur_v, cur_p, cur_vp);

        bool reset = need_reset_ || !has_prev_cam_;
        if (!reset) {
            float dx = f.camera.cam_pos[0] - prev_cam_pos_[0];
            float dy = f.camera.cam_pos[1] - prev_cam_pos_[1];
            float dz = f.camera.cam_pos[2] - prev_cam_pos_[2];
            float dist2 = dx * dx + dy * dy + dz * dz;
            float fwd_dot = f.camera.cam_fwd[0] * prev_cam_fwd_[0] + f.camera.cam_fwd[1] * prev_cam_fwd_[1] +
                            f.camera.cam_fwd[2] * prev_cam_fwd_[2];
            if (dist2 > (160.0f * 160.0f) || fwd_dot < 0.5f || f.camera.level_path != prev_level_) {
                reset = true;
            }
        }

        double inv_cur[16], cur_to_prev[16];
        if (reset || !Invert4d(cur_vp, inv_cur)) {
            reset = true;
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) cur_to_prev[r * 4 + c] = (r == c) ? 1.0 : 0.0;
        } else {
            MatMul4d(inv_cur, prev_vp_, cur_to_prev);
        }

        // 1. HudMaskCS: isolate 2D HUD overlays comparing g_buffers[0] (front) vs g_buffers[1] (pre-HUD 3D scene)
        TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (f.scene_color_res)
            TransitionHost(f.scene_color_res, f.scene_color_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionRes(hud_mask_.Get(), hud_mask_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        {
            uint32_t hud_cb[4] = {f.render_w, f.render_h, f.scene_color_res ? 1u : 0u, 0u};
            uint64_t cb_gpu = 0;
            if (!UploadConst(hud_cb, sizeof(hud_cb), cb_gpu)) return false;
            uint32_t base = AllocSlots(6);
            auto front_fmt = static_cast<DXGI_FORMAT>(f.front_fmt);
            auto scene_fmt = f.scene_color_res ? static_cast<DXGI_FORMAT>(f.scene_color_fmt) : front_fmt;
            auto* front_r = static_cast<ID3D12Resource*>(f.front_res);
            auto* scene_r = f.scene_color_res ? static_cast<ID3D12Resource*>(f.scene_color_res) : front_r;
            CreateSrv2D(base + 0, front_r, front_fmt);
            CreateSrv2D(base + 1, scene_r, scene_fmt);
            CreateSrv2D(base + 2, front_r, front_fmt);
            CreateSrv2D(base + 3, front_r, front_fmt);
            CreateUav2D(base + 4, hud_mask_.Get(), DXGI_FORMAT_R8_UNORM);
            CreateUav2D(base + 5, hud_mask_.Get(), DXGI_FORMAT_R8_UNORM);
            DispatchCompute(cl, spatial_.hud_mask_cs.Get(), cb_gpu, base, base + 4, f.render_w, f.render_h);
        }

        // 2. MotionVectorCS: depth-reprojected RG16F motion vectors in UV delta space (cur -> prev)
        TransitionHost(f.scene_depth_res, f.scene_depth_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionRes(hud_mask_.Get(), hud_mask_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionRes(motion_vec_.Get(), motion_vec_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        {
            struct MvCb {
                float cur_to_prev[16];
                float inv_w, inv_h;
                float jx_uv, jy_uv;
                float prev_jx_uv, prev_jy_uv;
                float mv_sx, mv_sy;
            } mv_cb{};
            for (int i = 0; i < 16; ++i) mv_cb.cur_to_prev[i] = float(cur_to_prev[i]);
            mv_cb.inv_w = 1.0f / float(std::max(f.render_w, 1u));
            mv_cb.inv_h = 1.0f / float(std::max(f.render_h, 1u));
            mv_cb.jx_uv = f.jitter_x * mv_cb.inv_w;
            mv_cb.jy_uv = f.jitter_y * mv_cb.inv_h;
            mv_cb.prev_jx_uv = prev_jx_uv_;
            mv_cb.prev_jy_uv = prev_jy_uv_;
            mv_cb.mv_sx = 1.0f;
            mv_cb.mv_sy = 1.0f;
            uint64_t cb_gpu = 0;
            if (!UploadConst(&mv_cb, sizeof(mv_cb), cb_gpu)) return false;
            uint32_t base = AllocSlots(6);
            auto* depth_r = static_cast<ID3D12Resource*>(f.scene_depth_res);
            CreateSrv2D(base + 0, depth_r, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS);
            CreateSrv2D(base + 1, hud_mask_.Get(), DXGI_FORMAT_R8_UNORM);
            CreateSrv2D(base + 2, hud_mask_.Get(), DXGI_FORMAT_R8_UNORM);
            CreateSrv2D(base + 3, hud_mask_.Get(), DXGI_FORMAT_R8_UNORM);
            CreateUav2D(base + 4, motion_vec_.Get(), DXGI_FORMAT_R16G16_FLOAT);
            CreateUav2D(base + 5, motion_vec_.Get(), DXGI_FORMAT_R16G16_FLOAT);
            DispatchCompute(cl, spatial_.motion_vec_cs.Get(), cb_gpu, base, base + 4, f.render_w, f.render_h);
        }

        // 3. Vendor temporal upscale evaluation
        TransitionRes(motion_vec_.Get(), motion_vec_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        const app::Settings& s = app::Get();
        if (!vendor->EnsureFeature(cl, f.render_w, f.render_h, f.vp_w, f.vp_h, s.upscale_quality, s.dlss_preset)) {
            return false;
        }

        VendorEvalDesc vd{};
        vd.cl = cl;
        vd.color = static_cast<ID3D12Resource*>(f.front_res);
        vd.color_fmt = static_cast<DXGI_FORMAT>(f.front_fmt);
        vd.depth = static_cast<ID3D12Resource*>(f.scene_depth_res);
        vd.motion_vectors = motion_vec_.Get();
        vd.reactive_mask = hud_mask_.Get();
        vd.output = upscale_out_.Get();
        vd.render_w = f.render_w;
        vd.render_h = f.render_h;
        vd.output_w = f.vp_w;
        vd.output_h = f.vp_h;
        vd.jitter_x = f.jitter_x;
        vd.jitter_y = f.jitter_y;
        vd.sharpness = sharpness;
        vd.reset_history = reset;
        vd.camera = f.camera;

        if (!vendor->Evaluate(vd)) return false;

        // Save camera state for next frame's reprojection
        std::memcpy(prev_vp_, cur_vp, sizeof(cur_vp));
        std::memcpy(prev_cam_pos_, f.camera.cam_pos, sizeof(prev_cam_pos_));
        std::memcpy(prev_cam_fwd_, f.camera.cam_fwd, sizeof(prev_cam_fwd_));
        prev_level_ = f.camera.level_path;
        prev_jx_uv_ = f.jitter_x / float(std::max(f.render_w, 1u));
        prev_jy_uv_ = f.jitter_y / float(std::max(f.render_h, 1u));
        has_prev_cam_ = true;
        need_reset_ = false;

        // 4. Optional RCAS post-sharpening (DLSS/XeSS) + final gamma present to bb_[bi]
        ID3D12Resource* final_src = upscale_out_.Get();
        if (post_rcas) {
            TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            TransitionRes(upscale_tmp_.Get(), upscale_tmp_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            RunRcasPass(cl, upscale_out_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, upscale_tmp_.Get(), f.vp_w, f.vp_h,
                        sharpness);
            TransitionRes(upscale_tmp_.Get(), upscale_tmp_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            final_src = upscale_tmp_.Get();
        } else {
            TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }

        PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), final_src, DXGI_FORMAT_R8G8B8A8_UNORM, f.vp_w, f.vp_h,
                            f, f.vp_x, f.vp_y, f.vp_w, f.vp_h);
        out_detail = vendor->StatusMessage();
        return true;
    }

    void RunSpatialMode(ID3D12GraphicsCommandList* cl, const gpu::UpscalerFrameDesc& f, app::UpscalerMode mode,
                        float sharpness) {
        auto* front = static_cast<ID3D12Resource*>(f.front_res);
        auto front_fmt = static_cast<DXGI_FORMAT>(f.front_fmt);
        bool same_size = (f.render_w == f.vp_w && f.render_h == f.vp_h);

        switch (mode) {
            case app::UpscalerMode::Bilinear: {
                TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), front, front_fmt, f.render_w, f.render_h, f,
                                    f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                return;
            }
            case app::UpscalerMode::Integer: {
                TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                if (f.render_w <= f.bb_w && f.render_h <= f.bb_h) {
                    uint32_t k = std::max(1u, std::min(f.bb_w / f.render_w, f.bb_h / f.render_h));
                    uint32_t iw = f.render_w * k;
                    uint32_t ih = f.render_h * k;
                    uint32_t ix = (f.bb_w - iw) / 2u;
                    uint32_t iy = (f.bb_h - ih) / 2u;
                    PresentToBackbuffer(cl, spatial_.present_integer.Get(), front, front_fmt, f.render_w, f.render_h, f,
                                        ix, iy, iw, ih);
                } else {
                    PresentToBackbuffer(cl, spatial_.present_bicubic.Get(), front, front_fmt, f.render_w, f.render_h, f,
                                        f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                }
                return;
            }
            case app::UpscalerMode::Bicubic: {
                if (same_size && sharpness > 0.001f && EnsureOutputTargets(f.vp_w, f.vp_h, false)) {
                    TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    RunRcasPass(cl, front, front_fmt, upscale_out_.Get(), f.vp_w, f.vp_h, sharpness);
                    TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), upscale_out_.Get(),
                                        DXGI_FORMAT_R8G8B8A8_UNORM, f.vp_w, f.vp_h, f, f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                } else {
                    TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    PresentToBackbuffer(cl, spatial_.present_bicubic.Get(), front, front_fmt, f.render_w, f.render_h, f,
                                        f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                }
                return;
            }
            case app::UpscalerMode::Cas: {
                if (!EnsureOutputTargets(f.vp_w, f.vp_h, false)) break;
                TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                uint32_t con[8]{};
                FillCasConstants(f.render_w, f.render_h, f.vp_w, f.vp_h, sharpness, con);
                uint64_t cb_gpu = 0;
                if (!UploadConst(con, sizeof(con), cb_gpu)) break;
                uint32_t base = AllocSlots(6);
                CreateSrv2D(base + 0, front, front_fmt);
                CreateSrv2D(base + 1, front, front_fmt);
                CreateSrv2D(base + 2, front, front_fmt);
                CreateSrv2D(base + 3, front, front_fmt);
                CreateUav2D(base + 4, upscale_out_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
                CreateUav2D(base + 5, upscale_out_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
                DispatchCompute(cl, spatial_.cas_cs.Get(), cb_gpu, base, base + 4, f.vp_w, f.vp_h);
                TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), upscale_out_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                    f.vp_w, f.vp_h, f, f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                return;
            }
            case app::UpscalerMode::Nis: {
                if (!spatial_.nis_coefs_uploaded || !EnsureOutputTargets(f.vp_w, f.vp_h, false)) break;
                TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                float con[16]{};
                FillNisConstants(f.render_w, f.render_h, f.vp_w, f.vp_h, sharpness, con);
                uint64_t cb_gpu = 0;
                if (!UploadConst(con, sizeof(con), cb_gpu)) break;
                uint32_t base = AllocSlots(6);
                CreateSrv2D(base + 0, front, front_fmt);
                CreateSrv2D(base + 1, spatial_.nis_coef_scaler.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
                CreateSrv2D(base + 2, spatial_.nis_coef_usm.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT);
                CreateSrv2D(base + 3, front, front_fmt);
                CreateUav2D(base + 4, upscale_out_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
                CreateUav2D(base + 5, upscale_out_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM);
                ID3D12PipelineState* pso = same_size ? spatial_.nis_sharpen.Get() : spatial_.nis_scaler.Get();
                DispatchCompute(cl, pso, cb_gpu, base, base + 4, f.vp_w, f.vp_h);
                TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), upscale_out_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                    f.vp_w, f.vp_h, f, f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                return;
            }
            case app::UpscalerMode::Fsr1:
            default: {
                bool want_rcas = sharpness > 0.001f;
                if (same_size && !want_rcas) {
                    TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), front, front_fmt, f.render_w, f.render_h, f,
                                        f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                    return;
                }
                if (same_size) {
                    // 100% scale: skip EASU and run RCAS directly
                    if (!EnsureOutputTargets(f.vp_w, f.vp_h, false)) break;
                    TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    RunRcasPass(cl, front, front_fmt, upscale_out_.Get(), f.vp_w, f.vp_h, sharpness);
                    TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), upscale_out_.Get(),
                                        DXGI_FORMAT_R8G8B8A8_UNORM, f.vp_w, f.vp_h, f, f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                    return;
                }
                // Sub-100% scale: EASU -> optional RCAS -> gamma present
                if (!EnsureOutputTargets(f.vp_w, f.vp_h, want_rcas)) break;
                TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                ID3D12Resource* easu_dst = want_rcas ? upscale_tmp_.Get() : upscale_out_.Get();
                D3D12_RESOURCE_STATES& easu_st = want_rcas ? upscale_tmp_state_ : upscale_out_state_;
                TransitionRes(easu_dst, easu_st, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

                uint32_t con[16]{};
                FillFsrEasuConstants(f.render_w, f.render_h, f.vp_w, f.vp_h, con);
                uint64_t cb_gpu = 0;
                if (!UploadConst(con, sizeof(con), cb_gpu)) break;
                uint32_t base = AllocSlots(6);
                CreateSrv2D(base + 0, front, front_fmt);
                CreateSrv2D(base + 1, front, front_fmt);
                CreateSrv2D(base + 2, front, front_fmt);
                CreateSrv2D(base + 3, front, front_fmt);
                CreateUav2D(base + 4, easu_dst, DXGI_FORMAT_R8G8B8A8_UNORM);
                CreateUav2D(base + 5, easu_dst, DXGI_FORMAT_R8G8B8A8_UNORM);
                DispatchCompute(cl, spatial_.fsr_easu.Get(), cb_gpu, base, base + 4, f.vp_w, f.vp_h);

                if (want_rcas) {
                    TransitionRes(upscale_tmp_.Get(), upscale_tmp_state_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    RunRcasPass(cl, upscale_tmp_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, upscale_out_.Get(), f.vp_w, f.vp_h,
                                sharpness);
                }
                TransitionRes(upscale_out_.Get(), upscale_out_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), upscale_out_.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                                    f.vp_w, f.vp_h, f, f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                return;
            }
        }

        // Ultimate safe fallback: bilinear direct
        TransitionHost(f.front_res, f.front_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        PresentToBackbuffer(cl, spatial_.present_bilinear.Get(), front, front_fmt, f.render_w, f.render_h, f, f.vp_x,
                            f.vp_y, f.vp_w, f.vp_h);
    }

    bool inited_ = false;
    ID3D12Device* dev_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    gpu::GpuCaps caps_{};
    gpu::UpscalerHostOps host_{};

    ComPtr<ID3D12DescriptorHeap> heap_;
    uint32_t desc_inc_ = 0;
    uint32_t frame_slot_base_ = 0;
    uint32_t frame_slot_used_ = 0;

    SpatialPipelines spatial_{};
    ComPtr<ID3D12Resource> upscale_out_;
    ComPtr<ID3D12Resource> upscale_tmp_;
    ComPtr<ID3D12Resource> motion_vec_;
    ComPtr<ID3D12Resource> hud_mask_;
    D3D12_RESOURCE_STATES upscale_out_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES upscale_tmp_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES motion_vec_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    D3D12_RESOURCE_STATES hud_mask_state_ = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    uint32_t out_w_ = 0, out_h_ = 0;
    uint32_t mv_w_ = 0, mv_h_ = 0;

    std::unique_ptr<VendorUpscaler> dlss_;
    std::unique_ptr<VendorUpscaler> fsr3_;
    std::unique_ptr<VendorUpscaler> xess_;

    bool need_reset_ = true;
    bool has_prev_cam_ = false;
    uint32_t jitter_frame_ = 0;
    double prev_vp_[16]{};
    float prev_cam_pos_[3]{};
    float prev_cam_fwd_[3]{};
    std::string prev_level_;
    float prev_jx_uv_ = 0.0f;
    float prev_jy_uv_ = 0.0f;

    mutable std::mutex status_lock_;
    gpu::UpscalerStatus status_{};
};

}  // namespace

std::vector<fs::path> PluginSearchDirs() {
    std::vector<fs::path> dirs;
    if (const wchar_t* env = _wgetenv(L"XBE_UPSCALER_PLUGINS_DIR"); env && *env) {
        // Explicit override isolates plugin lookup (used by regression/fallback/mock tests)
        dirs.emplace_back(env);
        return dirs;
    }
    dirs.push_back(app::ExeDir() / "plugins");
    dirs.push_back(app::ExeDir());
    return dirs;
}

fs::path FindPluginDll(const std::vector<std::wstring>& names) {
    std::error_code ec;
    for (const fs::path& dir : PluginSearchDirs()) {
        for (const std::wstring& n : names) {
            fs::path cand = dir / n;
            if (fs::is_regular_file(cand, ec)) return cand;
        }
    }
    return {};
}

fs::path FindFfxPluginDll() {
    std::error_code ec;
    for (const fs::path& dir : PluginSearchDirs()) {
        fs::path loader = dir / L"amd_fidelityfx_loader_dx12.dll";
        fs::path upscaler = dir / L"amd_fidelityfx_upscaler_dx12.dll";
        fs::path unified = dir / L"amd_fidelityfx_dx12.dll";
        if (fs::is_regular_file(loader, ec) && fs::is_regular_file(upscaler, ec)) return loader;
        if (fs::is_regular_file(upscaler, ec)) return upscaler;
        if (fs::is_regular_file(unified, ec)) return unified;
    }
    return {};
}

bool IsNgxDriverFallbackAllowed() {
    if (const wchar_t* v = _wgetenv(L"XBE_NGX_DRIVER_FALLBACK"); v && *v) {
        return _wtoi(v) != 0;
    }
    const wchar_t* pdir = _wgetenv(L"XBE_UPSCALER_PLUGINS_DIR");
    return !pdir || !*pdir;
}

namespace {

bool VerifySignedPe64Clean(const fs::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;

    // 1. Validate PE32+ (x64) header
    uint8_t dos_hdr[64]{};
    if (!ifs.read(reinterpret_cast<char*>(dos_hdr), sizeof(dos_hdr))) return false;
    if (dos_hdr[0] != 'M' || dos_hdr[1] != 'Z') return false;
    uint32_t e_lfanew = 0;
    std::memcpy(&e_lfanew, dos_hdr + 60, sizeof(e_lfanew));
    if (e_lfanew < 64 || e_lfanew > 4096) return false;

    ifs.seekg(std::streamoff(e_lfanew), std::ios::beg);
    uint8_t pe_hdr[26]{};
    if (!ifs.read(reinterpret_cast<char*>(pe_hdr), sizeof(pe_hdr))) return false;
    if (pe_hdr[0] != 'P' || pe_hdr[1] != 'E' || pe_hdr[2] != 0 || pe_hdr[3] != 0) return false;
    uint16_t machine = 0;
    uint16_t opt_magic = 0;
    std::memcpy(&machine, pe_hdr + 4, sizeof(machine));
    std::memcpy(&opt_magic, pe_hdr + 24, sizeof(opt_magic));
    if (machine != 0x8664u || opt_magic != 0x020Bu) return false;

    // 2. Verify 0 occurrences of "DO NOT DISTRIBUTE" (ASCII & UTF-16LE)
    static constexpr std::string_view kAsciiNeedle = "DO NOT DISTRIBUTE";
    static constexpr char kUtf16Needle[] = {'D', 0, 'O', 0, ' ', 0, 'N', 0, 'O', 0, 'T', 0, ' ', 0, 'D', 0, 'I', 0,
                                            'S', 0, 'T', 0, 'R', 0, 'I', 0, 'B', 0, 'U', 0, 'T', 0, 'E', 0};
    static constexpr std::string_view kWideNeedle(kUtf16Needle, sizeof(kUtf16Needle));

    ifs.clear();
    ifs.seekg(0, std::ios::beg);
    constexpr size_t kChunk = 1024 * 1024;
    constexpr size_t kOverlap = 64;
    std::string buf(kChunk + kOverlap, '\0');
    size_t carry = 0;
    while (ifs) {
        ifs.read(buf.data() + carry, std::streamsize(kChunk));
        size_t got = size_t(ifs.gcount());
        if (got == 0) break;
        std::string_view view(buf.data(), carry + got);
        if (view.find(kAsciiNeedle) != std::string_view::npos || view.find(kWideNeedle) != std::string_view::npos) {
            return false;
        }
        if (view.size() > kOverlap) {
            std::memmove(buf.data(), view.data() + (view.size() - kOverlap), kOverlap);
            carry = kOverlap;
        } else {
            carry = view.size();
        }
    }
    ifs.close();

    // 3. Offline Windows Authenticode verification
    WINTRUST_FILE_INFO fi{};
    fi.cbStruct = sizeof(fi);
    fi.pcwszFilePath = path.c_str();
    WINTRUST_DATA wd{};
    wd.cbStruct = sizeof(wd);
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    wd.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    LONG status = WinVerifyTrust(nullptr, &action, &wd);
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action, &wd);
    return status == ERROR_SUCCESS;
}

}  // namespace

fs::path FindNvidiaNgxCoreDll() {
    std::error_code ec;
    std::wstring p = ReadRegString(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore",
                                   L"NGXPath");
    if (p.empty()) {
        p = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath");
    }
    if (!p.empty()) {
        fs::path dll = fs::path(p) / L"_nvngx.dll";
        if (fs::is_regular_file(dll, ec)) return dll;
    }
    return {};
}

fs::path FindNvidiaNgxDriverDlssModel() {
    std::error_code ec;
    fs::path base;
    if (const wchar_t* override_dir = _wgetenv(L"XBE_NGX_MODELS_DIR"); override_dir && *override_dir) {
        base = fs::path(override_dir);
    } else {
        const wchar_t* pd = _wgetenv(L"ProgramData");
        fs::path prog_data = (pd && *pd) ? fs::path(pd) : fs::path(L"C:\\ProgramData");
        base = prog_data / L"NVIDIA" / L"NGX" / L"models" / L"dlss" / L"versions";
    }
    if (!fs::is_directory(base, ec)) return {};

    struct Candidate {
        bool is_160 = false;
        uint64_t ver_num = 0;
        uint64_t file_size = 0;
        std::wstring filename;
        fs::path path;
    };
    std::vector<Candidate> cands;
    for (const auto& ver_entry : fs::directory_iterator(base, ec)) {
        if (!ver_entry.is_directory(ec)) continue;
        std::wstring ver_name = ver_entry.path().filename().wstring();
        uint64_t ver_num = 0;
        for (wchar_t ch : ver_name) {
            if (ch >= L'0' && ch <= L'9') ver_num = ver_num * 10u + uint64_t(ch - L'0');
        }
        fs::path files_dir = ver_entry.path() / L"files";
        if (!fs::is_directory(files_dir, ec)) continue;
        for (const auto& fe : fs::directory_iterator(files_dir, ec)) {
            if (!fe.is_regular_file(ec)) continue;
            fs::path fp = fe.path();
            std::wstring ext = fp.extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext != L".bin") continue;
            uint64_t sz = fe.file_size(ec);
            if (ec || sz < 1024ull * 1024ull) continue;
            std::wstring fname = fp.filename().wstring();
            bool is_160 = fname.rfind(L"160_", 0) == 0;
            cands.push_back({is_160, ver_num, sz, fname, fp});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) {
        if (a.is_160 != b.is_160) return a.is_160 > b.is_160;
        if (a.ver_num != b.ver_num) return a.ver_num > b.ver_num;
        if (a.file_size != b.file_size) return a.file_size > b.file_size;
        return a.filename > b.filename;
    });
    for (const auto& c : cands) {
        if (VerifySignedPe64Clean(c.path)) return c.path;
    }
    return {};
}

fs::path StageNvidiaNgxDriverDlssDll() {
    std::error_code ec;
    fs::path src = FindNvidiaNgxDriverDlssModel();
    if (src.empty()) return {};
    uint64_t src_size = fs::file_size(src, ec);
    if (ec || src_size < 1024ull * 1024ull) return {};
    auto src_mtime = fs::last_write_time(src, ec).time_since_epoch().count();

    fs::path dst_dir = app::ExeDir() / "cache" / "ngx";
    fs::create_directories(dst_dir, ec);
    if (ec) return {};

    fs::path dst_dll = dst_dir / L"nvngx_dlss.dll";
    fs::path stamp_file = dst_dir / L"source.txt";
    std::string stamp = src.string() + "|" + std::to_string(src_size) + "|" + std::to_string(src_mtime) + "\n";

    if (fs::is_regular_file(dst_dll, ec) && fs::file_size(dst_dll, ec) == src_size &&
        fs::is_regular_file(stamp_file, ec)) {
        std::ifstream sfs(stamp_file, std::ios::binary);
        std::string existing((std::istreambuf_iterator<char>(sfs)), std::istreambuf_iterator<char>());
        if (existing == stamp && VerifySignedPe64Clean(dst_dll)) {
            LOGR("upscale/dlss: reusing verified NGX driver model cache %s", dst_dll.string().c_str());
            return dst_dll;
        }
    }

    fs::copy_file(src, dst_dll, fs::copy_options::overwrite_existing, ec);
    if (ec || !VerifySignedPe64Clean(dst_dll)) {
        LOGW("upscale/dlss: failed staging NGX driver model from %s", src.string().c_str());
        fs::remove(dst_dll, ec);
        return {};
    }
    std::ofstream ofs(stamp_file, std::ios::binary | std::ios::trunc);
    if (ofs) ofs << stamp;
    LOGR("upscale/dlss: auto-staged signed NGX driver model %s -> %s (%llu B)", src.string().c_str(),
         dst_dll.string().c_str(), (unsigned long long)src_size);
    return dst_dll;
}

PluginAvailability ProbeUpscalerPlugins(uint32_t vendor_id, uint32_t max_shader_model) {
    PluginAvailability av{};
    // DLSS
    fs::path ngx = FindNvidiaNgxCoreDll();
    fs::path dlss = FindPluginDll({L"nvngx_dlss.dll"});
    av.dlss_driver_ngx = !ngx.empty();
    av.dlss_dll_found = !dlss.empty();
    if (vendor_id != 0 && vendor_id != 0x10DEu) {
        av.dlss_info = "Requires NVIDIA RTX GPU";
    } else if (!av.dlss_driver_ngx) {
        av.dlss_info = "NVIDIA NGX driver (_nvngx.dll) not detected";
    } else if (!av.dlss_dll_found) {
        if (IsNgxDriverFallbackAllowed() && !FindNvidiaNgxDriverDlssModel().empty()) {
            av.dlss_ready = true;
            av.dlss_info = "Ready (NVIDIA NGX driver cache)";
        } else {
            av.dlss_info = "Driver _nvngx.dll ready; place nvngx_dlss.dll in plugins\\";
        }
    } else {
        av.dlss_ready = true;
        av.dlss_info = "Ready (" + dlss.filename().string() + ")";
    }

    // FSR 3.1
    fs::path fsr3 = FindFfxPluginDll();
    av.fsr3_dll_found = !fsr3.empty();
    if (max_shader_model != 0 && max_shader_model < uint32_t(D3D_SHADER_MODEL_6_2)) {
        av.fsr3_info = "Requires D3D12 Shader Model 6.2+";
    } else if (!av.fsr3_dll_found) {
        av.fsr3_info = "Place amd_fidelityfx_*_dx12.dll in plugins\\";
    } else {
        av.fsr3_ready = true;
        av.fsr3_info = "Ready (" + fsr3.filename().string() + ")";
    }

    // XeSS
    fs::path xess = FindPluginDll({L"libxess.dll"});
    av.xess_dll_found = !xess.empty();
    if (vendor_id != 0 && vendor_id != 0x8086u && max_shader_model != 0 &&
        max_shader_model < uint32_t(D3D_SHADER_MODEL_6_4)) {
        av.xess_info = "Requires Intel Arc (XMX) or SM 6.4+ GPU";
    } else if (!av.xess_dll_found) {
        av.xess_info = "Place libxess.dll in plugins\\";
    } else {
        av.xess_ready = true;
        av.xess_info = "Ready (libxess.dll)";
    }
    return av;
}

gpu::Upscaler& GlobalUpscaler() {
    static UpscalerManager instance;
    return instance;
}

void RegisterGlobalUpscaler() { gpu::SetUpscaler(&GlobalUpscaler()); }

}  // namespace upscale
