// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <vector>

#include "upscale/upscaler_manager.h"

namespace upscale {

// Shared pure SM 5.0 HLSL sources for all built-in zero-DLL spatial upscalers & temporal helpers.
// Consumed directly by D3DCompile (Windows D3D12) and by glslang HLSL->SPIR-V (Linux Vulkan).
extern const char* const kFsrEasuHlsl;
extern const char* const kFsrRcasHlsl;
extern const char* const kCasHlsl;
extern const char* const kNisHlsl;
extern const char* const kHudMaskHlsl;
extern const char* const kMotionVectorHlsl;
extern const char* const kPresentHlsl;

// Shared pure-C++ coefficient table generator & constant buffer builders.
void BuildNisCoefTables(std::vector<float>& scaler_rgba, std::vector<float>& usm_rgba);
void FillFsrEasuConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, uint32_t out_con[16]);
void FillFsrRcasConstants(float sharpness_01, uint32_t out_con[4]);
void FillCasConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float sharpness_01,
                      uint32_t out_con[8]);
void FillNisConstants(uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float sharpness_01,
                      float out_con[16]);

}  // namespace upscale
