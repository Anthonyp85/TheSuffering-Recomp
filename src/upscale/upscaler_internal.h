// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "gpu/backend.h"
#include "upscale/upscaler_manager.h"

namespace upscale {

using Microsoft::WRL::ComPtr;

// Returns directories to search for vendor upscaler DLLs (in priority order).
std::vector<std::filesystem::path> PluginSearchDirs();

// Helper to locate a file in PluginSearchDirs().
std::filesystem::path FindPluginDll(const std::vector<std::wstring>& names);

// Locate AMD FidelityFX DLL (requires loader+upscaler pair if loader is selected, or standalone upscaler/unified DLL).
std::filesystem::path FindFfxPluginDll();

// Returns whether automatic NVIDIA NGX driver-model fallback (%ProgramData%\NVIDIA\NGX\models\dlss) is permitted.
bool IsNgxDriverFallbackAllowed();

// Locate NVIDIA DriverStore _nvngx.dll via registry (returns empty path if absent).
std::filesystem::path FindNvidiaNgxCoreDll();

// Locate highest-version Authenticode-verified NVIDIA NGX driver DLSS model (.bin) in %ProgramData%\NVIDIA\NGX.
std::filesystem::path FindNvidiaNgxDriverDlssModel();

// Stage verified NVIDIA NGX driver DLSS model into ExeDir\cache\ngx\nvngx_dlss.dll for NGX Init_ProjectID.
std::filesystem::path StageNvidiaNgxDriverDlssDll();

struct SpatialPipelines {
    ComPtr<ID3D12RootSignature> cs_root;
    ComPtr<ID3D12RootSignature> ps_root;
    ComPtr<ID3D12PipelineState> fsr_easu;
    ComPtr<ID3D12PipelineState> fsr_rcas;
    ComPtr<ID3D12PipelineState> nis_scaler;
    ComPtr<ID3D12PipelineState> nis_sharpen;
    ComPtr<ID3D12PipelineState> cas_cs;
    ComPtr<ID3D12PipelineState> hud_mask_cs;
    ComPtr<ID3D12PipelineState> motion_vec_cs;
    ComPtr<ID3D12PipelineState> present_bilinear;
    ComPtr<ID3D12PipelineState> present_bicubic;
    ComPtr<ID3D12PipelineState> present_integer;
    ComPtr<ID3D12Resource> nis_coef_scaler;
    ComPtr<ID3D12Resource> nis_coef_usm;
    bool nis_coefs_uploaded = false;
};

bool InitSpatialPipelines(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const gpu::UpscalerHostOps& host,
                          SpatialPipelines& out);
void FillFsrEasuConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t out_con[16]);
void FillFsrRcasConstants(float sharpness_01, uint32_t out_con[4]);
void FillCasConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float sharpness_01,
                      uint32_t out_con[8]);
void FillNisConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float sharpness_01,
                      float out_con[16]);

struct VendorEvalDesc {
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Resource* color = nullptr;
    DXGI_FORMAT color_fmt = DXGI_FORMAT_B8G8R8A8_UNORM;
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion_vectors = nullptr;
    ID3D12Resource* reactive_mask = nullptr;
    ID3D12Resource* output = nullptr;
    uint32_t render_w = 0;
    uint32_t render_h = 0;
    uint32_t output_w = 0;
    uint32_t output_h = 0;
    float jitter_x = 0.0f;
    float jitter_y = 0.0f;
    float sharpness = 0.0f;
    bool reset_history = false;
    gpu::CameraFrameData camera{};
};

class VendorUpscaler {
public:
    virtual ~VendorUpscaler() = default;
    virtual bool Init(ID3D12Device* dev, ID3D12CommandQueue* queue, const gpu::GpuCaps& caps,
                      const gpu::UpscalerHostOps& host) = 0;
    virtual void Shutdown() = 0;
    virtual bool IsAvailable() const = 0;
    virtual std::string StatusMessage() const = 0;
    virtual bool EnsureFeature(ID3D12GraphicsCommandList* cl, uint32_t render_w, uint32_t render_h, uint32_t output_w,
                               uint32_t output_h, app::UpscaleQuality quality, app::DlssPreset dlss_preset) = 0;
    virtual bool Evaluate(const VendorEvalDesc& desc) = 0;
    virtual void ReleaseFeature() = 0;
};

std::unique_ptr<VendorUpscaler> CreateDlssUpscaler();
std::unique_ptr<VendorUpscaler> CreateFsr3Upscaler();
std::unique_ptr<VendorUpscaler> CreateXessUpscaler();

}  // namespace upscale
