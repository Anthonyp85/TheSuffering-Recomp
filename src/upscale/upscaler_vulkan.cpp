// SPDX-License-Identifier: GPL-2.0-or-later
// Native Vulkan 1.2 Upscaler Manager for Linux x86_64 & Steam Deck:
//   - All 6 built-in zero-DLL spatial upscalers: AMD FSR 1.0 (EASU + RCAS), NVIDIA NIS v1.0.3,
//     AMD FidelityFX CAS, Catmull-Rom 5-Tap Bicubic (incl. >100% SSAA), Pixel-Perfect Integer,
//     and Bilinear (Original), with gamma LUT present.
//   - Graceful fallback & clear status reporting for Windows D3D12-only vendor plugins (DLSS/FSR 3.1/XeSS).

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "upscale/upscaler_common.h"
#include "upscale/upscaler_manager.h"

namespace upscale {

namespace {

constexpr uint32_t kNumFrames = 3;
constexpr uint32_t kSetsPerFrame = 16;

struct VkFns {
    PFN_vkGetInstanceProcAddr get_inst_proc = nullptr;
    PFN_vkGetDeviceProcAddr get_dev_proc = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties get_phys_mem_props = nullptr;

    PFN_vkCreateImage create_image = nullptr;
    PFN_vkDestroyImage destroy_image = nullptr;
    PFN_vkGetImageMemoryRequirements get_image_mem_req = nullptr;
    PFN_vkAllocateMemory alloc_mem = nullptr;
    PFN_vkFreeMemory free_mem = nullptr;
    PFN_vkBindImageMemory bind_image_mem = nullptr;
    PFN_vkCreateImageView create_image_view = nullptr;
    PFN_vkDestroyImageView destroy_image_view = nullptr;
    PFN_vkCreateSampler create_sampler = nullptr;
    PFN_vkDestroySampler destroy_sampler = nullptr;
    PFN_vkCreateDescriptorSetLayout create_dsl = nullptr;
    PFN_vkDestroyDescriptorSetLayout destroy_dsl = nullptr;
    PFN_vkCreatePipelineLayout create_pipe_layout = nullptr;
    PFN_vkDestroyPipelineLayout destroy_pipe_layout = nullptr;
    PFN_vkDestroyPipeline destroy_pipeline = nullptr;
    PFN_vkCreateDescriptorPool create_desc_pool = nullptr;
    PFN_vkDestroyDescriptorPool destroy_desc_pool = nullptr;
    PFN_vkResetDescriptorPool reset_desc_pool = nullptr;
    PFN_vkAllocateDescriptorSets alloc_desc_sets = nullptr;
    PFN_vkUpdateDescriptorSets update_desc_sets = nullptr;
    PFN_vkCmdBindPipeline cmd_bind_pipeline = nullptr;
    PFN_vkCmdBindDescriptorSets cmd_bind_desc_sets = nullptr;
    PFN_vkCmdDispatch cmd_dispatch = nullptr;
    PFN_vkCmdCopyBufferToImage cmd_copy_buf_to_img = nullptr;
    PFN_vkCmdBeginRenderPass cmd_begin_rp = nullptr;
    PFN_vkCmdEndRenderPass cmd_end_rp = nullptr;
    PFN_vkCmdSetViewport cmd_set_viewport = nullptr;
    PFN_vkCmdSetScissor cmd_set_scissor = nullptr;
    PFN_vkCmdDraw cmd_draw = nullptr;

    bool Load(VkInstance inst, VkDevice dev) {
        get_inst_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
        if (!get_inst_proc || inst == VK_NULL_HANDLE || dev == VK_NULL_HANDLE) return false;
        get_dev_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(get_inst_proc(inst, "vkGetDeviceProcAddr"));
        get_phys_mem_props = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
            get_inst_proc(inst, "vkGetPhysicalDeviceMemoryProperties"));
        if (!get_dev_proc || !get_phys_mem_props) return false;

        auto load = [&](auto& fn, const char* name) -> bool {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(get_dev_proc(dev, name));
            return fn != nullptr;
        };
        bool ok = true;
        ok &= load(create_image, "vkCreateImage");
        ok &= load(destroy_image, "vkDestroyImage");
        ok &= load(get_image_mem_req, "vkGetImageMemoryRequirements");
        ok &= load(alloc_mem, "vkAllocateMemory");
        ok &= load(free_mem, "vkFreeMemory");
        ok &= load(bind_image_mem, "vkBindImageMemory");
        ok &= load(create_image_view, "vkCreateImageView");
        ok &= load(destroy_image_view, "vkDestroyImageView");
        ok &= load(create_sampler, "vkCreateSampler");
        ok &= load(destroy_sampler, "vkDestroySampler");
        ok &= load(create_dsl, "vkCreateDescriptorSetLayout");
        ok &= load(destroy_dsl, "vkDestroyDescriptorSetLayout");
        ok &= load(create_pipe_layout, "vkCreatePipelineLayout");
        ok &= load(destroy_pipe_layout, "vkDestroyPipelineLayout");
        ok &= load(destroy_pipeline, "vkDestroyPipeline");
        ok &= load(create_desc_pool, "vkCreateDescriptorPool");
        ok &= load(destroy_desc_pool, "vkDestroyDescriptorPool");
        ok &= load(reset_desc_pool, "vkResetDescriptorPool");
        ok &= load(alloc_desc_sets, "vkAllocateDescriptorSets");
        ok &= load(update_desc_sets, "vkUpdateDescriptorSets");
        ok &= load(cmd_bind_pipeline, "vkCmdBindPipeline");
        ok &= load(cmd_bind_desc_sets, "vkCmdBindDescriptorSets");
        ok &= load(cmd_dispatch, "vkCmdDispatch");
        ok &= load(cmd_copy_buf_to_img, "vkCmdCopyBufferToImage");
        ok &= load(cmd_begin_rp, "vkCmdBeginRenderPass");
        ok &= load(cmd_end_rp, "vkCmdEndRenderPass");
        ok &= load(cmd_set_viewport, "vkCmdSetViewport");
        ok &= load(cmd_set_scissor, "vkCmdSetScissor");
        ok &= load(cmd_draw, "vkCmdDraw");
        return ok;
    }
};

struct VkTex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
};

class VulkanUpscalerManager final : public gpu::Upscaler {
public:
    bool Init(const gpu::UpscalerInitDesc& desc) override {
        dev_ = reinterpret_cast<VkDevice>(desc.device);
        phys_ = reinterpret_cast<VkPhysicalDevice>(desc.queue);
        caps_ = desc.caps;
        host_ = desc.host;
        auto cmd = reinterpret_cast<VkCommandBuffer>(desc.cmd_list);
        VkInstance inst = host_.get_pipeline_library
                              ? reinterpret_cast<VkInstance>(host_.get_pipeline_library(host_.ctx))
                              : VK_NULL_HANDLE;

        if (!dev_ || !phys_ || !cmd || !inst || !vk_.Load(inst, dev_)) {
            LOGE("upscale/vk: missing Vulkan handles or failed to load Vulkan functions");
            return false;
        }
        vk_.get_phys_mem_props(phys_, &mem_props_);

        // Query swapchain present VkRenderPass from VulkanBackend
        void* rp_ptr = nullptr;
        if (host_.create_or_load_graphics_pso && host_.create_or_load_graphics_pso(host_.ctx, nullptr, nullptr, &rp_ptr)) {
            present_rp_ = reinterpret_cast<VkRenderPass>(rp_ptr);
        }
        if (present_rp_ == VK_NULL_HANDLE) {
            LOGE("upscale/vk: failed to obtain swapchain VkRenderPass from VulkanBackend");
            return false;
        }

        if (!CreateSamplers() || !CreateLayoutsAndPools() || !CreatePipelines()) {
            LOGE("upscale/vk: pipeline/layout creation failed");
            Shutdown();
            return false;
        }

        // Upload static 32x2 RGBA32F NIS v1.0.3 Lanczos/USM coefficient LUTs
        std::vector<float> scaler_tbl, usm_tbl;
        BuildNisCoefTables(scaler_tbl, usm_tbl);
        if (CreateLutImage(cmd, scaler_tbl, nis_scaler_lut_) && CreateLutImage(cmd, usm_tbl, nis_usm_lut_)) {
            nis_coefs_uploaded_ = true;
        } else {
            LOGW("upscale/vk: failed to upload NIS coefficient LUT textures");
        }

        LOGR("upscale/vk: native Vulkan 1.2 spatial upscalers ready (FSR1=%s, NIS=%s, CAS=%s, Bicubic=%s, Integer=%s, Bilinear=ok)",
             (fsr_easu_ != VK_NULL_HANDLE && fsr_rcas_ != VK_NULL_HANDLE) ? "ok" : "fallback",
             (nis_coefs_uploaded_ && nis_scaler_ != VK_NULL_HANDLE && nis_sharpen_ != VK_NULL_HANDLE) ? "ok" : "fallback",
             cas_cs_ != VK_NULL_HANDLE ? "ok" : "fallback",
             present_bicubic_ != VK_NULL_HANDLE ? "ok" : "fallback",
             present_integer_ != VK_NULL_HANDLE ? "ok" : "fallback");
        inited_ = true;
        return true;
    }

    void Shutdown() override {
        if (dev_ == VK_NULL_HANDLE) return;
        if (inited_ && host_.flush) host_.flush(host_.ctx);

        for (uint32_t i = 0; i < kNumFrames; ++i) {
            for (VkImageView v : frame_views_[i]) vk_.destroy_image_view(dev_, v, nullptr);
            frame_views_[i].clear();
            if (desc_pools_[i] != VK_NULL_HANDLE) {
                vk_.destroy_desc_pool(dev_, desc_pools_[i], nullptr);
                desc_pools_[i] = VK_NULL_HANDLE;
            }
        }
        if (lut_view_ != VK_NULL_HANDLE) {
            vk_.destroy_image_view(dev_, lut_view_, nullptr);
            lut_view_ = VK_NULL_HANDLE;
            lut_img_cached_ = VK_NULL_HANDLE;
        }

        ReleaseTargets();
        DestroyTex(nis_scaler_lut_);
        DestroyTex(nis_usm_lut_);

        auto kill_pipe = [&](VkPipeline& p) {
            if (p != VK_NULL_HANDLE) {
                vk_.destroy_pipeline(dev_, p, nullptr);
                p = VK_NULL_HANDLE;
            }
        };
        kill_pipe(fsr_easu_);
        kill_pipe(fsr_rcas_);
        kill_pipe(nis_scaler_);
        kill_pipe(nis_sharpen_);
        kill_pipe(cas_cs_);
        kill_pipe(present_bilinear_);
        kill_pipe(present_bicubic_);
        kill_pipe(present_integer_);

        if (cs_layout_ != VK_NULL_HANDLE) {
            vk_.destroy_pipe_layout(dev_, cs_layout_, nullptr);
            cs_layout_ = VK_NULL_HANDLE;
        }
        if (ps_layout_ != VK_NULL_HANDLE) {
            vk_.destroy_pipe_layout(dev_, ps_layout_, nullptr);
            ps_layout_ = VK_NULL_HANDLE;
        }
        if (cs_dsl_ != VK_NULL_HANDLE) {
            vk_.destroy_dsl(dev_, cs_dsl_, nullptr);
            cs_dsl_ = VK_NULL_HANDLE;
        }
        if (ps_dsl_ != VK_NULL_HANDLE) {
            vk_.destroy_dsl(dev_, ps_dsl_, nullptr);
            ps_dsl_ = VK_NULL_HANDLE;
        }
        if (smp_linear_ != VK_NULL_HANDLE) {
            vk_.destroy_sampler(dev_, smp_linear_, nullptr);
            smp_linear_ = VK_NULL_HANDLE;
        }
        if (smp_point_ != VK_NULL_HANDLE) {
            vk_.destroy_sampler(dev_, smp_point_, nullptr);
            smp_point_ = VK_NULL_HANDLE;
        }

        dev_ = VK_NULL_HANDLE;
        phys_ = VK_NULL_HANDLE;
        present_rp_ = VK_NULL_HANDLE;
        nis_coefs_uploaded_ = false;
        inited_ = false;
    }

    void ResolveRenderSize(uint32_t vp_w, uint32_t vp_h, uint32_t guest_w, uint32_t guest_h, bool /*widescreen*/,
                           uint32_t& out_render_w, uint32_t& out_render_h) override {
        const app::Settings& s = app::Get();
        ResolveUpscaleDimensions(s.upscaler, s.upscale_quality, s.render_percent, s.aspect_correct_render, vp_w, vp_h,
                                 guest_w, guest_h, out_render_w, out_render_h);
    }

    void BeginFrame(uint32_t /*render_w*/, uint32_t /*render_h*/, uint32_t /*vp_w*/, uint32_t /*vp_h*/,
                    const gpu::CameraFrameData& /*cam*/, bool& out_jitter_active, float& out_jitter_x,
                    float& out_jitter_y) override {
        // Spatial upscalers do not use sub-pixel temporal jitter.
        out_jitter_active = false;
        out_jitter_x = 0.0f;
        out_jitter_y = 0.0f;
    }

    void Prewarm(void* /*cmd_list*/, uint32_t render_w, uint32_t render_h, uint32_t vp_w, uint32_t vp_h) override {
        if (!inited_ || vp_w == 0 || vp_h == 0) return;
        const app::Settings& s = app::Get();
        float sharpness = std::clamp(float(s.upscale_sharpness) / 100.0f, 0.0f, 1.0f);
        app::UpscalerMode mode = s.upscaler;
        bool is_ssaa = (render_w > vp_w || render_h > vp_h);
        if (is_ssaa && mode != app::UpscalerMode::Bilinear && mode != app::UpscalerMode::Integer) {
            mode = app::UpscalerMode::Bicubic;
        }
        if (mode == app::UpscalerMode::Dlss || mode == app::UpscalerMode::Fsr3 || mode == app::UpscalerMode::Xess) {
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

    bool Execute(const gpu::UpscalerFrameDesc& f) override {
        if (!inited_ || !f.cmd_list || !f.front_res || !f.backbuffer_res || !f.backbuffer_rtv_ptr) return false;
        auto cmd = reinterpret_cast<VkCommandBuffer>(f.cmd_list);
        const app::Settings& s = app::Get();
        float sharpness = std::clamp(float(s.upscale_sharpness) / 100.0f, 0.0f, 1.0f);

        // Reset this frame slice's descriptor pool and ephemeral front image views
        frame_slot_ = uint32_t(f.frame_index % kNumFrames);
        vk_.reset_desc_pool(dev_, desc_pools_[frame_slot_], 0);
        for (VkImageView v : frame_views_[frame_slot_]) vk_.destroy_image_view(dev_, v, nullptr);
        frame_views_[frame_slot_].clear();

        // Refresh present_rp_ in case swapchain format was recreated
        void* rp_ptr = nullptr;
        if (host_.create_or_load_graphics_pso && host_.create_or_load_graphics_pso(host_.ctx, nullptr, nullptr, &rp_ptr) &&
            rp_ptr) {
            present_rp_ = reinterpret_cast<VkRenderPass>(rp_ptr);
        }

        app::UpscalerMode requested = s.upscaler;
        app::UpscalerMode active = requested;
        bool fallback = false;
        std::string detail;

        bool is_ssaa = (f.render_w > f.vp_w || f.render_h > f.vp_h);
        if (is_ssaa && requested != app::UpscalerMode::Bilinear && requested != app::UpscalerMode::Integer) {
            active = app::UpscalerMode::Bicubic;
            detail = "SSAA Downscale (Catmull-Rom)";
        } else if (requested == app::UpscalerMode::Dlss || requested == app::UpscalerMode::Fsr3 ||
                   requested == app::UpscalerMode::Xess) {
            fallback = true;
            active = app::UpscalerMode::Fsr1;
            detail = "Windows D3D12 Only -> FSR 1.0";
        }

        RunSpatialMode(cmd, f, active, sharpness);

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
            status_.temporal_active = false;
            status_.fallback_active = fallback;
            status_.dlss_available = false;
            status_.fsr3_available = false;
            status_.xess_available = false;
            status_.active_label = UpscalerShortName(active);
            status_.status_detail = detail;
        }
        return true;
    }

    void ResetHistory() override {
        // Spatial upscalers hold no temporal history.
    }

    gpu::UpscalerStatus GetStatus() const override {
        std::lock_guard<std::mutex> lock(status_lock_);
        return status_;
    }

private:
    uint32_t FindMemoryType(uint32_t type_bits, VkMemoryPropertyFlags props) const {
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) && (mem_props_.memoryTypes[i].propertyFlags & props) == props) return i;
        }
        return ~0u;
    }

    bool CreateImageTex(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage, VkTex& out) {
        DestroyTex(out);
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = fmt;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vk_.create_image(dev_, &ci, nullptr, &out.image) != VK_SUCCESS) return false;

        VkMemoryRequirements mr{};
        vk_.get_image_mem_req(dev_, out.image, &mr);
        uint32_t mt = FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (mt == ~0u) mt = FindMemoryType(mr.memoryTypeBits, 0);
        if (mt == ~0u) {
            DestroyTex(out);
            return false;
        }
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = mt;
        if (vk_.alloc_mem(dev_, &ai, nullptr, &out.mem) != VK_SUCCESS ||
            vk_.bind_image_mem(dev_, out.image, out.mem, 0) != VK_SUCCESS) {
            DestroyTex(out);
            return false;
        }

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = out.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk_.create_image_view(dev_, &vi, nullptr, &out.view) != VK_SUCCESS) {
            DestroyTex(out);
            return false;
        }
        out.layout = VK_IMAGE_LAYOUT_UNDEFINED;
        out.format = fmt;
        out.width = w;
        out.height = h;
        return true;
    }

    void DestroyTex(VkTex& t) {
        if (t.view != VK_NULL_HANDLE) vk_.destroy_image_view(dev_, t.view, nullptr);
        if (t.image != VK_NULL_HANDLE) vk_.destroy_image(dev_, t.image, nullptr);
        if (t.mem != VK_NULL_HANDLE) vk_.free_mem(dev_, t.mem, nullptr);
        t = VkTex{};
    }

    bool CreateLutImage(VkCommandBuffer cmd, const std::vector<float>& rgba_data, VkTex& out) {
        if (!CreateImageTex(32, 2, VK_FORMAT_R32G32B32A32_SFLOAT,
                            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, out)) {
            return false;
        }
        void* up_res = nullptr;
        uint64_t up_off = 0, up_gpu = 0;
        uint8_t* up_cpu = nullptr;
        uint64_t bytes = rgba_data.size() * sizeof(float);
        if (!host_.upload(host_.ctx, bytes, 16, &up_res, &up_off, &up_cpu, &up_gpu) || !up_res || !up_cpu) {
            DestroyTex(out);
            return false;
        }
        std::memcpy(up_cpu, rgba_data.data(), size_t(bytes));
        TransitionTex(out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy reg{};
        reg.bufferOffset = up_off;
        reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        reg.imageExtent = {32, 2, 1};
        vk_.cmd_copy_buf_to_img(cmd, reinterpret_cast<VkBuffer>(up_res), out.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                1, &reg);
        TransitionTex(out, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return true;
    }

    bool CreateSamplers() {
        auto make = [&](VkFilter flt, VkSampler& out) -> bool {
            VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            ci.magFilter = flt;
            ci.minFilter = flt;
            ci.mipmapMode = (flt == VK_FILTER_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
            ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            ci.maxLod = VK_LOD_CLAMP_NONE;
            return vk_.create_sampler(dev_, &ci, nullptr, &out) == VK_SUCCESS;
        };
        return make(VK_FILTER_LINEAR, smp_linear_) && make(VK_FILTER_NEAREST, smp_point_);
    }

    bool CreateLayoutsAndPools() {
        // Compute Set 0: b0 (0), t0..t3 (4..7), s0..s1 (12..13), u0..u1 (20..21)
        VkDescriptorSetLayoutBinding cs_b[9]{};
        cs_b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 0; i < 4; ++i)
            cs_b[1 + i] = {4 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 0; i < 2; ++i)
            cs_b[5 + i] = {12 + i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        for (uint32_t i = 0; i < 2; ++i)
            cs_b[7 + i] = {20 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        dci.bindingCount = 9;
        dci.pBindings = cs_b;
        if (vk_.create_dsl(dev_, &dci, nullptr, &cs_dsl_) != VK_SUCCESS) return false;

        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &cs_dsl_;
        if (vk_.create_pipe_layout(dev_, &plci, nullptr, &cs_layout_) != VK_SUCCESS) return false;

        // Present Set 0: b0 (0), t0..t1 (4..5), s0..s1 (12..13)
        VkDescriptorSetLayoutBinding ps_b[5]{};
        ps_b[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                   nullptr};
        ps_b[1] = {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        ps_b[2] = {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        ps_b[3] = {12, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        ps_b[4] = {13, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        dci.bindingCount = 5;
        dci.pBindings = ps_b;
        if (vk_.create_dsl(dev_, &dci, nullptr, &ps_dsl_) != VK_SUCCESS) return false;

        plci.pSetLayouts = &ps_dsl_;
        if (vk_.create_pipe_layout(dev_, &plci, nullptr, &ps_layout_) != VK_SUCCESS) return false;

        VkDescriptorPoolSize sizes[4] = {
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, kSetsPerFrame},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kSetsPerFrame * 4},
            {VK_DESCRIPTOR_TYPE_SAMPLER, kSetsPerFrame * 2},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, kSetsPerFrame * 2},
        };
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets = kSetsPerFrame;
        pci.poolSizeCount = 4;
        pci.pPoolSizes = sizes;
        for (uint32_t i = 0; i < kNumFrames; ++i) {
            if (vk_.create_desc_pool(dev_, &pci, nullptr, &desc_pools_[i]) != VK_SUCCESS) return false;
        }
        return true;
    }

    bool MakeComputePipe(const wchar_t* name, const char* hlsl, const char* entry, VkPipeline& out) {
        uint64_t h = 0;
        auto mod = reinterpret_cast<VkShaderModule>(host_.compile(host_.ctx, hlsl, entry, "cs_5_0", &h));
        if (mod == VK_NULL_HANDLE) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = mod;
        ci.stage.pName = "main";
        ci.layout = cs_layout_;
        void* raw = nullptr;
        if (!host_.create_or_load_compute_pso(host_.ctx, name, &ci, &raw) || !raw) return false;
        out = reinterpret_cast<VkPipeline>(raw);
        return true;
    }

    bool MakePresentPipe(const wchar_t* name, const char* ps_entry, VkPipeline& out) {
        uint64_t hv = 0, hp = 0;
        auto vs = reinterpret_cast<VkShaderModule>(host_.compile(host_.ctx, kPresentHlsl, "vs_main", "vs_5_0", &hv));
        auto ps = reinterpret_cast<VkShaderModule>(host_.compile(host_.ctx, kPresentHlsl, ps_entry, "ps_5_0", &hp));
        if (vs == VK_NULL_HANDLE || ps == VK_NULL_HANDLE) return false;

        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = ps;
        stages[1].pName = "main";

        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &ba;
        VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dyn.dynamicStateCount = 2;
        dyn.pDynamicStates = dyn_states;

        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vi;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vp;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pColorBlendState = &cb;
        gi.pDynamicState = &dyn;
        gi.layout = ps_layout_;
        gi.renderPass = present_rp_;
        gi.subpass = 0;

        void* raw = nullptr;
        if (!host_.create_or_load_graphics_pso(host_.ctx, name, &gi, &raw) || !raw) return false;
        out = reinterpret_cast<VkPipeline>(raw);
        return true;
    }

    bool CreatePipelines() {
        if (!MakePresentPipe(L"vk_upscale_present_bilinear", "ps_bilinear", present_bilinear_)) return false;
        if (!MakePresentPipe(L"vk_upscale_present_bicubic", "ps_bicubic", present_bicubic_))
            LOGW("upscale/vk: bicubic present pipeline unavailable, falling back to bilinear");
        if (!MakePresentPipe(L"vk_upscale_present_integer", "ps_integer", present_integer_))
            LOGW("upscale/vk: integer present pipeline unavailable, falling back to bilinear");
        if (!MakeComputePipe(L"vk_upscale_fsr1_easu", kFsrEasuHlsl, "main", fsr_easu_))
            LOGW("upscale/vk: FSR 1.0 EASU compute pipeline unavailable");
        if (!MakeComputePipe(L"vk_upscale_fsr1_rcas", kFsrRcasHlsl, "main", fsr_rcas_))
            LOGW("upscale/vk: FSR 1.0 RCAS compute pipeline unavailable");
        if (!MakeComputePipe(L"vk_upscale_nis_scaler", kNisHlsl, "NisScaler", nis_scaler_))
            LOGW("upscale/vk: NIS scaler compute pipeline unavailable");
        if (!MakeComputePipe(L"vk_upscale_nis_sharpen", kNisHlsl, "NisSharpen", nis_sharpen_))
            LOGW("upscale/vk: NIS sharpen compute pipeline unavailable");
        if (!MakeComputePipe(L"vk_upscale_cas", kCasHlsl, "main", cas_cs_))
            LOGW("upscale/vk: CAS compute pipeline unavailable");
        return true;
    }

    void ReleaseTargets() {
        DestroyTex(upscale_out_);
        DestroyTex(upscale_tmp_);
        out_w_ = out_h_ = 0;
    }

    bool EnsureOutputTargets(uint32_t vp_w, uint32_t vp_h, bool need_tmp) {
        constexpr VkImageUsageFlags kUavUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (out_w_ != vp_w || out_h_ != vp_h || upscale_out_.image == VK_NULL_HANDLE) {
            if (upscale_out_.image != VK_NULL_HANDLE && host_.flush) host_.flush(host_.ctx);
            ReleaseTargets();
            if (!CreateImageTex(vp_w, vp_h, VK_FORMAT_R8G8B8A8_UNORM, kUavUsage, upscale_out_)) return false;
            out_w_ = vp_w;
            out_h_ = vp_h;
        }
        if (need_tmp && upscale_tmp_.image == VK_NULL_HANDLE) {
            if (!CreateImageTex(vp_w, vp_h, VK_FORMAT_R8G8B8A8_UNORM, kUavUsage, upscale_tmp_)) return false;
        }
        return true;
    }

    void TransitionTex(VkTex& t, VkImageLayout want) {
        uint32_t cur = uint32_t(t.layout);
        host_.transition(host_.ctx, reinterpret_cast<void*>(t.image), &cur, uint32_t(want));
        t.layout = static_cast<VkImageLayout>(cur);
    }

    void TransitionHost(void* img, uint32_t* cur, VkImageLayout want) {
        host_.transition(host_.ctx, img, cur, uint32_t(want));
    }

    VkImageView MakeEphemeralView(VkImage img, VkFormat fmt) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        if (vk_.create_image_view(dev_, &vi, nullptr, &v) == VK_SUCCESS) {
            frame_views_[frame_slot_].push_back(v);
        }
        return v;
    }

    VkImageView GetLutView(void* lut_res) {
        auto img = reinterpret_cast<VkImage>(lut_res);
        if (img == lut_img_cached_ && lut_view_ != VK_NULL_HANDLE) return lut_view_;
        if (lut_view_ != VK_NULL_HANDLE) {
            vk_.destroy_image_view(dev_, lut_view_, nullptr);
            lut_view_ = VK_NULL_HANDLE;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vk_.create_image_view(dev_, &vi, nullptr, &lut_view_) == VK_SUCCESS) {
            lut_img_cached_ = img;
        }
        return lut_view_;
    }

    bool UploadConst(const void* data, size_t bytes, VkBuffer& out_buf, uint64_t& out_off, uint64_t& out_size) {
        void* res = nullptr;
        uint64_t gpu = 0;
        uint8_t* cpu = nullptr;
        size_t aligned = (bytes + 255) & ~size_t(255);
        if (!host_.upload(host_.ctx, aligned, 256, &res, &out_off, &cpu, &gpu) || !res || !cpu) return false;
        std::memset(cpu, 0, aligned);
        std::memcpy(cpu, data, bytes);
        out_buf = reinterpret_cast<VkBuffer>(res);
        out_size = aligned;
        return true;
    }

    void DispatchCompute(VkCommandBuffer cmd, VkPipeline pipe, const void* cb_data, size_t cb_bytes,
                         const VkImageView srvs[4], VkImageView uav0, uint32_t w, uint32_t h) {
        VkBuffer cb_buf = VK_NULL_HANDLE;
        uint64_t cb_off = 0, cb_sz = 0;
        if (!UploadConst(cb_data, cb_bytes, cb_buf, cb_off, cb_sz)) return;

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = desc_pools_[frame_slot_];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &cs_dsl_;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        if (vk_.alloc_desc_sets(dev_, &ai, &ds) != VK_SUCCESS) return;

        VkDescriptorBufferInfo bi{cb_buf, cb_off, cb_sz};
        VkDescriptorImageInfo si[4]{};
        for (int i = 0; i < 4; ++i) si[i] = {VK_NULL_HANDLE, srvs[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo smpi[2] = {
            {smp_linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
            {smp_point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
        };
        VkDescriptorImageInfo ui[2] = {
            {VK_NULL_HANDLE, uav0, VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE, uav0, VK_IMAGE_LAYOUT_GENERAL},
        };

        VkWriteDescriptorSet wr[9]{};
        wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                 nullptr, &bi, nullptr};
        for (uint32_t i = 0; i < 4; ++i) {
            wr[1 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 4 + i, 0, 1,
                         VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &si[i], nullptr, nullptr};
        }
        for (uint32_t i = 0; i < 2; ++i) {
            wr[5 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 12 + i, 0, 1,
                         VK_DESCRIPTOR_TYPE_SAMPLER, &smpi[i], nullptr, nullptr};
        }
        for (uint32_t i = 0; i < 2; ++i) {
            wr[7 + i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 20 + i, 0, 1,
                         VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ui[i], nullptr, nullptr};
        }
        vk_.update_desc_sets(dev_, 9, wr, 0, nullptr);

        vk_.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        vk_.cmd_bind_desc_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cs_layout_, 0, 1, &ds, 0, nullptr);
        vk_.cmd_dispatch(cmd, (w + 7u) / 8u, (h + 7u) / 8u, 1u);
    }

    void RunRcasPass(VkCommandBuffer cmd, VkImageView src_view, VkTex& dst, uint32_t w, uint32_t h, float sharpness) {
        uint32_t con[4]{};
        FillFsrRcasConstants(sharpness, con);
        VkImageView srvs[4] = {src_view, src_view, src_view, src_view};
        DispatchCompute(cmd, fsr_rcas_, con, sizeof(con), srvs, dst.view, w, h);
    }

    void PresentToBackbuffer(VkCommandBuffer cmd, VkPipeline pipe, VkImageView src_view, uint32_t src_w, uint32_t src_h,
                             const gpu::UpscalerFrameDesc& f, uint32_t dst_x, uint32_t dst_y, uint32_t dst_w,
                             uint32_t dst_h) {
        TransitionHost(f.lut_res, f.lut_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        TransitionHost(f.backbuffer_res, f.backbuffer_state, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

        float cb[8] = {float(src_w), float(src_h), 1.0f / float(std::max(src_w, 1u)),
                       1.0f / float(std::max(src_h, 1u)), f.gamma ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f};
        VkBuffer cb_buf = VK_NULL_HANDLE;
        uint64_t cb_off = 0, cb_sz = 0;
        if (!UploadConst(cb, sizeof(cb), cb_buf, cb_off, cb_sz)) return;

        VkImageView lut_v = GetLutView(f.lut_res);
        if (src_view == VK_NULL_HANDLE || lut_v == VK_NULL_HANDLE) return;

        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = desc_pools_[frame_slot_];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &ps_dsl_;
        VkDescriptorSet ds = VK_NULL_HANDLE;
        if (vk_.alloc_desc_sets(dev_, &ai, &ds) != VK_SUCCESS) return;

        VkDescriptorBufferInfo bi{cb_buf, cb_off, cb_sz};
        VkDescriptorImageInfo si[2] = {
            {VK_NULL_HANDLE, src_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
            {VK_NULL_HANDLE, lut_v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        };
        VkDescriptorImageInfo smpi[2] = {
            {smp_linear_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
            {smp_point_, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED},
        };
        VkWriteDescriptorSet wr[5]{};
        wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                 nullptr, &bi, nullptr};
        wr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 4, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 &si[0], nullptr, nullptr};
        wr[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 5, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                 &si[1], nullptr, nullptr};
        wr[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 12, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
                 &smpi[0], nullptr, nullptr};
        wr[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, ds, 13, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER,
                 &smpi[1], nullptr, nullptr};
        vk_.update_desc_sets(dev_, 5, wr, 0, nullptr);

        VkRenderPassBeginInfo rpbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rpbi.renderPass = present_rp_;
        rpbi.framebuffer = reinterpret_cast<VkFramebuffer>(f.backbuffer_rtv_ptr);
        rpbi.renderArea = {{0, 0}, {f.bb_w, f.bb_h}};
        vk_.cmd_begin_rp(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

        // Negative-height Vulkan 1.1+ viewport matches D3D12 clip-Y orientation 1-to-1
        VkViewport vp{float(dst_x), float(dst_y + dst_h), float(dst_w), -float(dst_h), 0.0f, 1.0f};
        VkRect2D sc{{0, 0}, {f.bb_w, f.bb_h}};
        vk_.cmd_set_viewport(cmd, 0, 1, &vp);
        vk_.cmd_set_scissor(cmd, 0, 1, &sc);
        vk_.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vk_.cmd_bind_desc_sets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ps_layout_, 0, 1, &ds, 0, nullptr);
        vk_.cmd_draw(cmd, 3, 1, 0, 0);
        vk_.cmd_end_rp(cmd);
    }

    void RunSpatialMode(VkCommandBuffer cmd, const gpu::UpscalerFrameDesc& f, app::UpscalerMode mode, float sharpness) {
        auto front_img = reinterpret_cast<VkImage>(f.front_res);
        auto front_fmt = f.front_fmt ? static_cast<VkFormat>(f.front_fmt) : VK_FORMAT_B8G8R8A8_UNORM;
        VkImageView front_view =
            f.front_view ? reinterpret_cast<VkImageView>(f.front_view) : MakeEphemeralView(front_img, front_fmt);
        if (front_view == VK_NULL_HANDLE) return;

        bool same_size = (f.render_w == f.vp_w && f.render_h == f.vp_h);

        switch (mode) {
            case app::UpscalerMode::Bilinear: {
                TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                PresentToBackbuffer(cmd, present_bilinear_, front_view, f.render_w, f.render_h, f, f.vp_x, f.vp_y,
                                    f.vp_w, f.vp_h);
                return;
            }
            case app::UpscalerMode::Integer: {
                TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                if (present_integer_ != VK_NULL_HANDLE && f.render_w <= f.bb_w && f.render_h <= f.bb_h) {
                    uint32_t k = std::max(1u, std::min(f.bb_w / f.render_w, f.bb_h / f.render_h));
                    uint32_t iw = f.render_w * k;
                    uint32_t ih = f.render_h * k;
                    uint32_t ix = (f.bb_w - iw) / 2u;
                    uint32_t iy = (f.bb_h - ih) / 2u;
                    PresentToBackbuffer(cmd, present_integer_, front_view, f.render_w, f.render_h, f, ix, iy, iw, ih);
                } else {
                    VkPipeline fb = present_bicubic_ != VK_NULL_HANDLE ? present_bicubic_ : present_bilinear_;
                    PresentToBackbuffer(cmd, fb, front_view, f.render_w, f.render_h, f, f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                }
                return;
            }
            case app::UpscalerMode::Bicubic: {
                if (same_size && sharpness > 0.001f && fsr_rcas_ != VK_NULL_HANDLE &&
                    EnsureOutputTargets(f.vp_w, f.vp_h, false)) {
                    TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_GENERAL);
                    RunRcasPass(cmd, front_view, upscale_out_, f.vp_w, f.vp_h, sharpness);
                    TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    PresentToBackbuffer(cmd, present_bilinear_, upscale_out_.view, f.vp_w, f.vp_h, f, f.vp_x, f.vp_y,
                                        f.vp_w, f.vp_h);
                } else {
                    TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    VkPipeline fb = present_bicubic_ != VK_NULL_HANDLE ? present_bicubic_ : present_bilinear_;
                    PresentToBackbuffer(cmd, fb, front_view, f.render_w, f.render_h, f, f.vp_x, f.vp_y, f.vp_w, f.vp_h);
                }
                return;
            }
            case app::UpscalerMode::Cas: {
                if (cas_cs_ == VK_NULL_HANDLE || !EnsureOutputTargets(f.vp_w, f.vp_h, false)) break;
                TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_GENERAL);
                uint32_t con[8]{};
                FillCasConstants(f.render_w, f.render_h, f.vp_w, f.vp_h, sharpness, con);
                VkImageView srvs[4] = {front_view, front_view, front_view, front_view};
                DispatchCompute(cmd, cas_cs_, con, sizeof(con), srvs, upscale_out_.view, f.vp_w, f.vp_h);
                TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                PresentToBackbuffer(cmd, present_bilinear_, upscale_out_.view, f.vp_w, f.vp_h, f, f.vp_x, f.vp_y,
                                    f.vp_w, f.vp_h);
                return;
            }
            case app::UpscalerMode::Nis: {
                VkPipeline pipe = same_size ? nis_sharpen_ : nis_scaler_;
                if (!nis_coefs_uploaded_ || pipe == VK_NULL_HANDLE || !EnsureOutputTargets(f.vp_w, f.vp_h, false)) break;
                TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_GENERAL);
                float con[16]{};
                FillNisConstants(f.render_w, f.render_h, f.vp_w, f.vp_h, sharpness, con);
                VkImageView srvs[4] = {front_view, nis_scaler_lut_.view, nis_usm_lut_.view, front_view};
                DispatchCompute(cmd, pipe, con, sizeof(con), srvs, upscale_out_.view, f.vp_w, f.vp_h);
                TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                PresentToBackbuffer(cmd, present_bilinear_, upscale_out_.view, f.vp_w, f.vp_h, f, f.vp_x, f.vp_y,
                                    f.vp_w, f.vp_h);
                return;
            }
            case app::UpscalerMode::Fsr1:
            default: {
                bool want_rcas = sharpness > 0.001f && fsr_rcas_ != VK_NULL_HANDLE;
                if (same_size && !want_rcas) {
                    TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    PresentToBackbuffer(cmd, present_bilinear_, front_view, f.render_w, f.render_h, f, f.vp_x, f.vp_y,
                                        f.vp_w, f.vp_h);
                    return;
                }
                if (same_size) {
                    if (!EnsureOutputTargets(f.vp_w, f.vp_h, false)) break;
                    TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_GENERAL);
                    RunRcasPass(cmd, front_view, upscale_out_, f.vp_w, f.vp_h, sharpness);
                    TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    PresentToBackbuffer(cmd, present_bilinear_, upscale_out_.view, f.vp_w, f.vp_h, f, f.vp_x, f.vp_y,
                                        f.vp_w, f.vp_h);
                    return;
                }
                if (fsr_easu_ == VK_NULL_HANDLE || !EnsureOutputTargets(f.vp_w, f.vp_h, want_rcas)) break;
                TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                VkTex& easu_dst = want_rcas ? upscale_tmp_ : upscale_out_;
                TransitionTex(easu_dst, VK_IMAGE_LAYOUT_GENERAL);

                uint32_t con[16]{};
                FillFsrEasuConstants(f.render_w, f.render_h, f.vp_w, f.vp_h, con);
                VkImageView srvs[4] = {front_view, front_view, front_view, front_view};
                DispatchCompute(cmd, fsr_easu_, con, sizeof(con), srvs, easu_dst.view, f.vp_w, f.vp_h);

                if (want_rcas) {
                    TransitionTex(upscale_tmp_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_GENERAL);
                    RunRcasPass(cmd, upscale_tmp_.view, upscale_out_, f.vp_w, f.vp_h, sharpness);
                }
                TransitionTex(upscale_out_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                PresentToBackbuffer(cmd, present_bilinear_, upscale_out_.view, f.vp_w, f.vp_h, f, f.vp_x, f.vp_y,
                                    f.vp_w, f.vp_h);
                return;
            }
        }

        // Safe fallback: bilinear direct
        TransitionHost(f.front_res, f.front_state, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        PresentToBackbuffer(cmd, present_bilinear_, front_view, f.render_w, f.render_h, f, f.vp_x, f.vp_y, f.vp_w,
                            f.vp_h);
    }

    bool inited_ = false;
    VkFns vk_{};
    VkDevice dev_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    VkRenderPass present_rp_ = VK_NULL_HANDLE;
    gpu::GpuCaps caps_{};
    gpu::UpscalerHostOps host_{};

    VkSampler smp_linear_ = VK_NULL_HANDLE;
    VkSampler smp_point_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout cs_dsl_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout ps_dsl_ = VK_NULL_HANDLE;
    VkPipelineLayout cs_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout ps_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool desc_pools_[kNumFrames]{};
    std::vector<VkImageView> frame_views_[kNumFrames];
    uint32_t frame_slot_ = 0;

    VkImage lut_img_cached_ = VK_NULL_HANDLE;
    VkImageView lut_view_ = VK_NULL_HANDLE;

    VkPipeline fsr_easu_ = VK_NULL_HANDLE;
    VkPipeline fsr_rcas_ = VK_NULL_HANDLE;
    VkPipeline nis_scaler_ = VK_NULL_HANDLE;
    VkPipeline nis_sharpen_ = VK_NULL_HANDLE;
    VkPipeline cas_cs_ = VK_NULL_HANDLE;
    VkPipeline present_bilinear_ = VK_NULL_HANDLE;
    VkPipeline present_bicubic_ = VK_NULL_HANDLE;
    VkPipeline present_integer_ = VK_NULL_HANDLE;

    VkTex nis_scaler_lut_{};
    VkTex nis_usm_lut_{};
    bool nis_coefs_uploaded_ = false;

    VkTex upscale_out_{};
    VkTex upscale_tmp_{};
    uint32_t out_w_ = 0, out_h_ = 0;

    mutable std::mutex status_lock_;
    gpu::UpscalerStatus status_{};
};

}  // namespace

PluginAvailability ProbeUpscalerPlugins(uint32_t /*vendor_id*/, uint32_t /*max_shader_model*/) {
    static constexpr const char* kMsg = "Windows D3D12 Only — Use FSR 1.0 / NIS on Native Linux Vulkan";
    PluginAvailability av{};
    av.dlss_info = kMsg;
    av.fsr3_info = kMsg;
    av.xess_info = kMsg;
    return av;
}

gpu::Upscaler& GlobalUpscaler() {
    static VulkanUpscalerManager instance;
    return instance;
}

void RegisterGlobalUpscaler() { gpu::SetUpscaler(&GlobalUpscaler()); }

}  // namespace upscale
