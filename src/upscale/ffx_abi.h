// SPDX-License-Identifier: GPL-2.0-or-later AND MIT
// AMD FidelityFX API (ffx_api) C ABI definitions for dynamic loading of FSR 3.1 / FSR 4 DLLs.
// Derived from AMD FidelityFX SDK ffx_api headers (MIT License, Copyright Advanced Micro Devices, Inc.).
#pragma once

#include <cstddef>
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace upscale::ffx {

using Context = void*;
using ReturnCode = uint32_t;

inline constexpr ReturnCode kOk = 0u;

struct Header {
    uint64_t type = 0;
    Header* pNext = nullptr;
};

struct AllocationCallbacks {
    void* pUserData = nullptr;
    void* (*alloc)(void* pUserData, uint64_t size) = nullptr;
    void (*dealloc)(void* pUserData, void* pMem) = nullptr;
};

struct Dimensions2D {
    uint32_t width = 0;
    uint32_t height = 0;
};

struct FloatCoords2D {
    float x = 0.0f;
    float y = 0.0f;
};

enum SurfaceFormat : uint32_t {
    kFormatUnknown = 0,
    kFormatR32G32B32A32Typeless = 1,
    kFormatR32G32B32A32Uint = 2,
    kFormatR32G32B32A32Float = 3,
    kFormatR16G16B16A16Float = 4,
    kFormatR32G32B32Float = 5,
    kFormatR32G32Float = 6,
    kFormatR8Uint = 7,
    kFormatR32Uint = 8,
    kFormatR8G8B8A8Typeless = 9,
    kFormatR8G8B8A8Unorm = 10,
    kFormatR8G8B8A8Snorm = 11,
    kFormatR8G8B8A8Srgb = 12,
    kFormatB8G8R8A8Typeless = 13,
    kFormatB8G8R8A8Unorm = 14,
    kFormatB8G8R8A8Srgb = 15,
    kFormatR11G11B10Float = 16,
    kFormatR10G10B10A2Unorm = 17,
    kFormatR16G16Float = 18,
    kFormatR16G16Uint = 19,
    kFormatR16G16Sint = 20,
    kFormatR16Float = 21,
    kFormatR16Uint = 22,
    kFormatR16Unorm = 23,
    kFormatR16Snorm = 24,
    kFormatR8Unorm = 25,
    kFormatR8G8Unorm = 26,
    kFormatR8G8Uint = 27,
    kFormatR32Float = 28,
};

enum ResourceUsage : uint32_t {
    kUsageReadOnly = 0u,
    kUsageRenderTarget = 1u << 0,
    kUsageUav = 1u << 1,
    kUsageDepthTarget = 1u << 2,
    kUsageStencilTarget = 1u << 5,
};

enum ResourceState : uint32_t {
    kStateCommon = 1u << 0,
    kStateUnorderedAccess = 1u << 1,
    kStateComputeRead = 1u << 2,
    kStatePixelRead = 1u << 3,
    kStatePixelComputeRead = kStateComputeRead | kStatePixelRead,
    kStateCopySrc = 1u << 4,
    kStateCopyDest = 1u << 5,
    kStateGenericRead = kStateCopySrc | kStateComputeRead,
    kStatePresent = 1u << 7,
    kStateRenderTarget = 1u << 8,
    kStateDepthAttachment = 1u << 9,
};

enum ResourceType : uint32_t {
    kTypeBuffer = 0,
    kTypeTexture1D = 1,
    kTypeTexture2D = 2,
    kTypeTextureCube = 3,
    kTypeTexture3D = 4,
};

struct ResourceDescription {
    uint32_t type = kTypeTexture2D;
    uint32_t format = kFormatUnknown;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 1;
    uint32_t mipCount = 1;
    uint32_t flags = 0;
    uint32_t usage = kUsageReadOnly;
};

struct Resource {
    void* resource = nullptr;
    ResourceDescription description{};
    uint32_t state = kStateCommon;
};

// ffx_api structure type IDs (FidelityFX SDK 1.1+ / 2.x)
inline constexpr uint64_t kStructTypeCreateBackendDx12 = 0x00000002ull;
inline constexpr uint64_t kStructTypeQueryGetProviderVersion = 0x00000006ull;
inline constexpr uint64_t kStructTypeCreateContextUpscale = 0x00010000ull;
inline constexpr uint64_t kStructTypeDispatchUpscale = 0x00010001ull;
inline constexpr uint64_t kStructTypeQueryUpscaleJitterPhaseCount = 0x00010004ull;

enum UpscaleContextFlags : uint32_t {
    kEnableHighDynamicRange = 1u << 0,
    kEnableDisplayResolutionMotionVectors = 1u << 1,
    kEnableMotionVectorsJitterCancellation = 1u << 2,
    kEnableDepthInverted = 1u << 3,
    kEnableDepthInfinite = 1u << 4,
    kEnableAutoExposure = 1u << 5,
    kEnableDynamicResolution = 1u << 6,
};

enum UpscaleDispatchFlags : uint32_t {
    kDispatchNone = 0u,
};

using MessageCallback = void(__cdecl*)(uint32_t type, const wchar_t* message);

struct CreateBackendDx12Desc {
    Header header{kStructTypeCreateBackendDx12, nullptr};
    ID3D12Device* device = nullptr;
};

struct CreateContextDescUpscale {
    Header header{kStructTypeCreateContextUpscale, nullptr};
    uint32_t flags = 0;
    Dimensions2D maxRenderSize{};
    Dimensions2D maxUpscaleSize{};
    MessageCallback fpMessage = nullptr;
};

struct DispatchDescUpscale {
    Header header{kStructTypeDispatchUpscale, nullptr};
    void* commandList = nullptr;
    Resource color{};
    Resource depth{};
    Resource motionVectors{};
    Resource exposure{};
    Resource reactive{};
    Resource transparencyAndComposition{};
    Resource output{};
    FloatCoords2D jitterOffset{};
    FloatCoords2D motionVectorScale{};
    Dimensions2D renderSize{};
    Dimensions2D upscaleSize{};
    bool enableSharpening = false;
    float sharpness = 0.0f;
    float frameTimeDelta = 16.667f;
    float preExposure = 1.0f;
    bool reset = false;
    float cameraNear = 32.0f;
    float cameraFar = 1e30f;
    float cameraFovAngleVertical = 1.0f;
    float viewSpaceToMetersFactor = 0.0254f;
    uint32_t flags = kDispatchNone;
};

struct QueryGetProviderVersion {
    Header header{kStructTypeQueryGetProviderVersion, nullptr};
    uint64_t versionId = 0;
    const char* versionName = nullptr;
};

struct QueryDescUpscaleJitterPhaseCount {
    Header header{kStructTypeQueryUpscaleJitterPhaseCount, nullptr};
    uint32_t renderWidth = 0;
    uint32_t displayWidth = 0;
    int32_t* pOutPhaseCount = nullptr;
};

using PfnCreateContext = ReturnCode(__cdecl*)(Context* context, Header* desc, const AllocationCallbacks* memCb);
using PfnDestroyContext = ReturnCode(__cdecl*)(Context* context, const AllocationCallbacks* memCb);
using PfnConfigure = ReturnCode(__cdecl*)(Context* context, const Header* desc);
using PfnQuery = ReturnCode(__cdecl*)(Context* context, Header* desc);
using PfnDispatch = ReturnCode(__cdecl*)(Context* context, const Header* desc);

}  // namespace upscale::ffx
