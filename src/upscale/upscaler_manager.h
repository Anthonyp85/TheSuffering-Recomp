// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <string>

#include "app/app.h"
#include "gpu/backend.h"

namespace upscale {

struct PluginAvailability {
    bool dlss_driver_ngx = false;
    bool dlss_dll_found = false;
    bool dlss_ready = false;
    std::string dlss_info;

    bool fsr3_dll_found = false;
    bool fsr3_ready = false;
    std::string fsr3_info;

    bool xess_dll_found = false;
    bool xess_ready = false;
    std::string xess_info;
};

// Probes optional vendor runtime DLLs (plugins\, ExeDir\, and NVIDIA DriverStore _nvngx.dll) without
// creating a D3D12 device. Safe to call from the Pre-Game Launcher and F1 overlay.
PluginAvailability ProbeUpscalerPlugins(uint32_t vendor_id, uint32_t max_shader_model);

// Shared resolution calculation used by Launcher, F1 Overlay, and D3D12Backend/UpscalerManager.
void ResolveUpscaleDimensions(app::UpscalerMode mode, app::UpscaleQuality quality, uint32_t render_percent,
                              bool aspect_correct, uint32_t vp_w, uint32_t vp_h, uint32_t guest_w, uint32_t guest_h,
                              uint32_t& out_rw, uint32_t& out_rh);

float QualityScaleFactor(app::UpscaleQuality quality, uint32_t custom_percent);
const char* UpscalerModeName(app::UpscalerMode mode);
const char* UpscalerShortName(app::UpscalerMode mode);
const char* UpscaleQualityName(app::UpscaleQuality quality);
const char* DlssPresetName(app::DlssPreset preset);

// Registers the global UpscalerManager with XboxXBERecomp's D3D12Backend.
void RegisterGlobalUpscaler();
gpu::Upscaler& GlobalUpscaler();

}  // namespace upscale
