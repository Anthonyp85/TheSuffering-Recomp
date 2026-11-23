// SPDX-License-Identifier: GPL-2.0-or-later
// Clean-room C ABI interoperability declarations for dynamic loading of Intel XeSS (libxess.dll).
#pragma once

#include <cstdint>

struct ID3D12DescriptorHeap;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12PipelineLibrary;
struct ID3D12Heap;
struct ID3D12Resource;

namespace upscale::xess {

using ContextHandle = void*;

enum Result : int32_t {
    kSuccess = 0,
    kWarningNonExistingFolder = 1,
    kWarningOldDriver = 2,
};

inline bool Succeeded(int32_t r) { return r >= 0; }

struct Version {
    uint16_t major = 0;
    uint16_t minor = 0;
    uint16_t patch = 0;
    uint16_t reserved = 0;
};

struct Coord2D {
    uint32_t x = 0;
    uint32_t y = 0;
};

enum QualitySettings : int32_t {
    kQualityUltraPerformance = 100,
    kQualityPerformance = 101,
    kQualityBalanced = 102,
    kQualityQuality = 103,
    kQualityUltraQuality = 104,
    kQualityUltraQualityPlus = 105,
    kQualityNativeAA = 106,
};

enum InitFlags : uint32_t {
    kInitNone = 0u,
    kInitHighResMv = 1u << 0,
    kInitInvertedDepth = 1u << 1,
    kInitExposureScaleTexture = 1u << 2,
    kInitUseResponsivePixelMask = 1u << 3,
    kInitUseNdcVelocity = 1u << 4,
    kInitUseExternalDescriptorHeap = 1u << 5,
    kInitLdrInputColor = 1u << 6,
    kInitJitteredMv = 1u << 7,
    kInitEnableAutoExposure = 1u << 8,
};

struct D3D12InitParams {
    Coord2D outputResolution{};
    QualitySettings qualitySetting = kQualityQuality;
    uint32_t initFlags = kInitNone;
    uint32_t creationNodeMask = 0;
    uint32_t visibleNodeMask = 0;
    ID3D12Heap* pTempBufferHeap = nullptr;
    uint64_t bufferHeapOffset = 0;
    ID3D12Heap* pTempTextureHeap = nullptr;
    uint64_t textureHeapOffset = 0;
    ID3D12PipelineLibrary* pPipelineLibrary = nullptr;
};

struct D3D12ExecuteParams {
    ID3D12Resource* pColorTexture = nullptr;
    ID3D12Resource* pVelocityTexture = nullptr;
    ID3D12Resource* pDepthTexture = nullptr;
    ID3D12Resource* pExposureScaleTexture = nullptr;
    ID3D12Resource* pResponsivePixelMaskTexture = nullptr;
    ID3D12Resource* pOutputTexture = nullptr;
    float jitterOffsetX = 0.0f;
    float jitterOffsetY = 0.0f;
    float exposureScale = 1.0f;
    uint32_t resetHistory = 0;
    uint32_t inputWidth = 0;
    uint32_t inputHeight = 0;
    Coord2D inputColorBase{};
    Coord2D inputMotionVectorBase{};
    Coord2D inputDepthBase{};
    Coord2D inputResponsiveMaskBase{};
    Coord2D reserved0{};
    Coord2D outputColorBase{};
    ID3D12DescriptorHeap* pDescriptorHeap = nullptr;
    uint32_t descriptorHeapOffset = 0;
};

using PfnGetVersion = int32_t(__cdecl*)(Version* out_version);
using PfnD3D12CreateContext = int32_t(__cdecl*)(ID3D12Device* device, ContextHandle* out_context);
using PfnDestroyContext = int32_t(__cdecl*)(ContextHandle context);
using PfnGetOptimalInputResolution = int32_t(__cdecl*)(ContextHandle context, const Coord2D* output_res,
                                                       QualitySettings quality, Coord2D* out_opt, Coord2D* out_min,
                                                       Coord2D* out_max);
using PfnD3D12Init = int32_t(__cdecl*)(ContextHandle context, const D3D12InitParams* params);
using PfnSetVelocityScale = int32_t(__cdecl*)(ContextHandle context, float x, float y);
using PfnSetJitterScale = int32_t(__cdecl*)(ContextHandle context, float x, float y);
using PfnForceLegacyScaleFactors = int32_t(__cdecl*)(ContextHandle context, bool force);
using PfnD3D12Execute = int32_t(__cdecl*)(ContextHandle context, ID3D12GraphicsCommandList* cl,
                                          const D3D12ExecuteParams* params);

}  // namespace upscale::xess
