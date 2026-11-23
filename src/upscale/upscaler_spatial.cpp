// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
// Built-in zero-DLL D3D12 spatial upscalers (cs_5_0 / ps_5_0 via inbox D3DCompile).
// Shares HLSL sources and math helpers via upscale/upscaler_common.{h,cpp}.

#include "upscale/upscaler_common.h"
#include "upscale/upscaler_internal.h"

#include <cstring>
#include <vector>

#include "core/log.h"

namespace upscale {

namespace {

bool CreateNisLutTexture(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const gpu::UpscalerHostOps& host,
                         const std::vector<float>& rgba_data, ComPtr<ID3D12Resource>& out) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 32;
    td.Height = 2;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    td.SampleDesc.Count = 1;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                            IID_PPV_ARGS(&out))))
        return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 row_sz = 0, total = 0;
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_sz, &total);
    void* up_res = nullptr;
    uint64_t up_off = 0, up_gpu = 0;
    uint8_t* up_cpu = nullptr;
    if (!host.upload(host.ctx, total, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &up_res, &up_off, &up_cpu, &up_gpu))
        return false;
    for (UINT y = 0; y < rows; ++y) {
        std::memcpy(up_cpu + size_t(y) * fp.Footprint.RowPitch, rgba_data.data() + size_t(y) * 32 * 4,
                    size_t(32 * 4 * sizeof(float)));
    }
    fp.Offset = up_off;
    D3D12_TEXTURE_COPY_LOCATION dl{out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{static_cast<ID3D12Resource*>(up_res), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    uint32_t st = uint32_t(D3D12_RESOURCE_STATE_COPY_DEST);
    host.transition(host.ctx, out.Get(), &st, uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    return true;
}

bool MakeComputePso(const gpu::UpscalerHostOps& host, ID3D12RootSignature* root, const wchar_t* pso_name,
                    const char* hlsl, const char* entry, ComPtr<ID3D12PipelineState>& out) {
    uint64_t h = 0;
    auto* blob = static_cast<ID3DBlob*>(host.compile(host.ctx, hlsl, entry, "cs_5_0", &h));
    if (!blob) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root;
    d.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
    void* raw = nullptr;
    if (!host.create_or_load_compute_pso(host.ctx, pso_name, &d, &raw) || !raw) return false;
    out.Attach(static_cast<ID3D12PipelineState*>(raw));
    return true;
}

bool MakePresentPso(const gpu::UpscalerHostOps& host, ID3D12RootSignature* root, const wchar_t* pso_name,
                    const char* ps_entry, ComPtr<ID3D12PipelineState>& out) {
    uint64_t hv = 0, hp = 0;
    auto* vs = static_cast<ID3DBlob*>(host.compile(host.ctx, kPresentHlsl, "vs_main", "vs_5_0", &hv));
    auto* ps = static_cast<ID3DBlob*>(host.compile(host.ctx, kPresentHlsl, ps_entry, "ps_5_0", &hp));
    if (!vs || !ps) return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root;
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    void* raw = nullptr;
    if (!host.create_or_load_graphics_pso(host.ctx, pso_name, &d, &raw) || !raw) return false;
    out.Attach(static_cast<ID3D12PipelineState*>(raw));
    return true;
}

}  // namespace

bool InitSpatialPipelines(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const gpu::UpscalerHostOps& host,
                          SpatialPipelines& out) {
    // 1. Unified Compute Root Signature: b0 (CBV), t0..t3 (SRV table), u0..u1 (UAV table), s0 linear + s1 point
    D3D12_DESCRIPTOR_RANGE cs_ranges[2] = {};
    cs_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    cs_ranges[0].NumDescriptors = 4;
    cs_ranges[0].BaseShaderRegister = 0;
    cs_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    cs_ranges[1].NumDescriptors = 2;
    cs_ranges[1].BaseShaderRegister = 0;

    D3D12_ROOT_PARAMETER cs_params[3] = {};
    cs_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    cs_params[0].Descriptor.ShaderRegister = 0;
    cs_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    cs_params[1].DescriptorTable = {1, &cs_ranges[0]};
    cs_params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    cs_params[2].DescriptorTable = {1, &cs_ranges[1]};

    D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].ShaderRegister = 0;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[1].ShaderRegister = 1;
    samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC cs_rsd{};
    cs_rsd.NumParameters = 3;
    cs_rsd.pParameters = cs_params;
    cs_rsd.NumStaticSamplers = 2;
    cs_rsd.pStaticSamplers = samplers;
    ComPtr<ID3DBlob> blob, err;
    if (FAILED(D3D12SerializeRootSignature(&cs_rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                        IID_PPV_ARGS(&out.cs_root)))) {
        LOGE("upscale: failed to create compute root signature");
        return false;
    }

    // 2. Graphics Present Root Signature: b0 (CBV), t0..t1 (SRV table), s0 linear + s1 point
    D3D12_DESCRIPTOR_RANGE ps_range{};
    ps_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ps_range.NumDescriptors = 2;
    ps_range.BaseShaderRegister = 0;
    D3D12_ROOT_PARAMETER ps_params[2] = {};
    ps_params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    ps_params[0].Descriptor.ShaderRegister = 0;
    ps_params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    ps_params[1].DescriptorTable = {1, &ps_range};
    ps_params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC ps_rsd{};
    ps_rsd.NumParameters = 2;
    ps_rsd.pParameters = ps_params;
    ps_rsd.NumStaticSamplers = 2;
    ps_rsd.pStaticSamplers = samplers;
    blob.Reset();
    err.Reset();
    if (FAILED(D3D12SerializeRootSignature(&ps_rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)) ||
        FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                        IID_PPV_ARGS(&out.ps_root)))) {
        LOGE("upscale: failed to create present root signature");
        return false;
    }

    bool ok = true;
    ok &= MakeComputePso(host, out.cs_root.Get(), L"upscale_fsr1_easu", kFsrEasuHlsl, "main", out.fsr_easu);
    ok &= MakeComputePso(host, out.cs_root.Get(), L"upscale_fsr1_rcas", kFsrRcasHlsl, "main", out.fsr_rcas);
    ok &= MakeComputePso(host, out.cs_root.Get(), L"upscale_nis_scaler", kNisHlsl, "NisScaler", out.nis_scaler);
    ok &= MakeComputePso(host, out.cs_root.Get(), L"upscale_nis_sharpen", kNisHlsl, "NisSharpen", out.nis_sharpen);
    ok &= MakeComputePso(host, out.cs_root.Get(), L"upscale_cas", kCasHlsl, "main", out.cas_cs);
    ok &= MakeComputePso(host, out.cs_root.Get(), L"upscale_hud_mask", kHudMaskHlsl, "main", out.hud_mask_cs);
    ok &= MakeComputePso(host, out.cs_root.Get(), L"upscale_motion_vec", kMotionVectorHlsl, "main", out.motion_vec_cs);
    ok &= MakePresentPso(host, out.ps_root.Get(), L"upscale_present_bilinear", "ps_bilinear", out.present_bilinear);
    ok &= MakePresentPso(host, out.ps_root.Get(), L"upscale_present_bicubic", "ps_bicubic", out.present_bicubic);
    ok &= MakePresentPso(host, out.ps_root.Get(), L"upscale_present_integer", "ps_integer", out.present_integer);

    std::vector<float> scaler_tbl, usm_tbl;
    BuildNisCoefTables(scaler_tbl, usm_tbl);
    if (CreateNisLutTexture(dev, cl, host, scaler_tbl, out.nis_coef_scaler) &&
        CreateNisLutTexture(dev, cl, host, usm_tbl, out.nis_coef_usm)) {
        out.nis_coefs_uploaded = true;
    } else {
        LOGW("upscale: failed to upload NIS coefficient tables");
    }
    return ok;
}

}  // namespace upscale
