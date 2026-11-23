// SPDX-License-Identifier: GPL-2.0-or-later
// Clean-room C ABI interoperability declarations for dynamic loading of NVIDIA NGX (_nvngx.dll)
// and optional user-supplied DLSS Super Resolution plugin (nvngx_dlss.dll).
// Zero NVIDIA SDK headers or proprietary binaries are included in this repository.
#pragma once

#include <cstdint>

struct ID3D11Resource;
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;

namespace upscale::ngx {

enum Result : uint32_t {
    kSuccess = 0x00000001u,
    kFailBase = 0xBAD00000u,
};

inline bool Succeeded(uint32_t r) { return r == kSuccess; }

enum Feature : int32_t {
    kFeatureSuperSampling = 1,
};

enum EngineType : int32_t {
    kEngineCustom = 0,
};

enum PerfQuality : int32_t {
    kMaxPerf = 0,
    kBalanced = 1,
    kMaxQuality = 2,
    kUltraPerformance = 3,
    kUltraQuality = 4,
    kDlaa = 5,
};

enum DlssFeatureFlags : uint32_t {
    kFlagNone = 0u,
    kFlagIsHdr = 1u << 0,
    kFlagMvLowRes = 1u << 1,
    kFlagMvJittered = 1u << 2,
    kFlagDepthInverted = 1u << 3,
    kFlagAutoExposure = 1u << 6,
};

enum RenderPreset : uint32_t {
    kPresetDefault = 0u,
    kPresetE = 5u,
    kPresetF = 6u,
    kPresetJ = 10u,
    kPresetK = 11u,
};

struct PathListInfo {
    const wchar_t* const* path = nullptr;
    uint32_t length = 0;
};

using LogCallback = void(__cdecl*)(const char* message, int level, int component);

struct LoggingInfo {
    LogCallback callback = nullptr;
    int minimum_level = 0;
    bool disable_other_sinks = false;
};

struct FeatureCommonInfo {
    PathListInfo path_list{};
    void* internal_data = nullptr;
    LoggingInfo logging{};
};

struct Handle {
    uint32_t id;
};

// MSVC x64 pure-virtual parameter bag ABI exported by _nvngx.dll.
struct Parameter {
    virtual void Set(const char* name, unsigned long long value) = 0;
    virtual void Set(const char* name, float value) = 0;
    virtual void Set(const char* name, double value) = 0;
    virtual void Set(const char* name, unsigned int value) = 0;
    virtual void Set(const char* name, int value) = 0;
    virtual void Set(const char* name, ID3D11Resource* value) = 0;
    virtual void Set(const char* name, ID3D12Resource* value) = 0;
    virtual void Set(const char* name, void* value) = 0;

    virtual uint32_t Get(const char* name, unsigned long long* out) const = 0;
    virtual uint32_t Get(const char* name, float* out) const = 0;
    virtual uint32_t Get(const char* name, double* out) const = 0;
    virtual uint32_t Get(const char* name, unsigned int* out) const = 0;
    virtual uint32_t Get(const char* name, int* out) const = 0;
    virtual uint32_t Get(const char* name, ID3D11Resource** out) const = 0;
    virtual uint32_t Get(const char* name, ID3D12Resource** out) const = 0;
    virtual uint32_t Get(const char* name, void** out) const = 0;

    virtual void Reset() = 0;
};

using ProgressCallback = void(__cdecl*)(float progress, bool& should_cancel);

// Exported C functions resolved dynamically from _nvngx.dll via GetProcAddress.
// Note: Raw _nvngx.dll export NVSDK_NGX_D3D12_Init_ProjectID takes (InSDKVersion, InFeatureInfo) in
// slots 6 and 7 (verified directly against _nvngx.dll disassembly), and NVSDK_NGX_D3D12_Shutdown1
// takes (InD3DDevice, OutRefCount).
using PfnInitProjectId = uint32_t(__cdecl*)(const char* project_id, int32_t engine_type, const char* engine_version,
                                            const wchar_t* app_data_path, ID3D12Device* device, uint32_t api_version,
                                            const FeatureCommonInfo* feature_info);
using PfnGetCapabilityParameters = uint32_t(__cdecl*)(Parameter** out_params);
using PfnAllocateParameters = uint32_t(__cdecl*)(Parameter** out_params);
using PfnDestroyParameters = uint32_t(__cdecl*)(Parameter* params);
using PfnCreateFeature = uint32_t(__cdecl*)(ID3D12GraphicsCommandList* cl, int32_t feature, Parameter* params,
                                            Handle** out_handle);
using PfnEvaluateFeature = uint32_t(__cdecl*)(ID3D12GraphicsCommandList* cl, const Handle* handle,
                                              const Parameter* params, ProgressCallback cb);
using PfnReleaseFeature = uint32_t(__cdecl*)(Handle* handle);
using PfnShutdown1 = uint32_t(__cdecl*)(ID3D12Device* device, uint32_t* out_refcount);

}  // namespace upscale::ngx
