// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <variant>

#include "common/dynamic_library.h"
#include "common/settings.h"
#include "video_core/frame_time_stats.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_base.h"
#ifdef HAS_LSFG
#include "video_core/renderer_vulkan/present/frame_gen.h"
#endif
#include "video_core/renderer_vulkan/vk_amd_frame_gen.h"
#include "video_core/renderer_vulkan/vk_blit_screen.h"
#include "video_core/renderer_vulkan/vk_fsr4_upscaler.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "video_core/renderer_vulkan/vk_turbo_mode.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Core::Memory {
class Memory;
}

namespace Tegra {
class GPU;
}

namespace Vulkan {

Device CreateDevice(const vk::Instance& instance, const vk::InstanceDispatch& dld,
                    VkSurfaceKHR surface);

class RendererVulkan final : public VideoCore::RendererBase {
public:
    explicit RendererVulkan(Core::Frontend::EmuWindow& emu_window,
                            Tegra::MaxwellDeviceMemoryManager& device_memory_, Tegra::GPU& gpu_,
                            std::unique_ptr<Core::Frontend::GraphicsContext> context_);
    ~RendererVulkan() override;

    void Composite(std::span<const Tegra::FramebufferConfig> framebuffers) override;

    std::vector<u8> GetAppletCaptureBuffer() override;

    VideoCore::RasterizerInterface* ReadRasterizer() override {
        return &rasterizer;
    }

    [[nodiscard]] std::string GetDeviceVendor() const override {
        return device.GetDriverName();
    }

    [[nodiscard]] VideoCore::FrameGenStatus GetFrameGenStatus() const override {
        if (Settings::values.frame_gen.GetValue() &&
            Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::AmdFsr) {
            const u64 generated = AmdFrameGen::GeneratedFrames();
            const auto [tag, reason] = rasterizer.AmdFrameGenIdleReason();
            return {.state = generated != 0 ? VideoCore::FrameGenState::Running
                                            : VideoCore::FrameGenState::Pending,
                    .generated_frames = generated,
                    .idle_tag = tag,
                    .idle_reason = reason};
        }
#ifdef HAS_LSFG
        return frame_gen.CurrentStatus();
#else
        return {.state = VideoCore::FrameGenState::UnsupportedBuild};
#endif
    }

    [[nodiscard]] VideoCore::UpscalerStatus GetUpscalerStatus() const override {
        VideoCore::UpscalerStatus status;
#ifdef _WIN32
        status.supported = true; // AMD ships its FSR runtime for Windows only
#endif
        status.upscaled_frames = rasterizer.UpscaledFrames();
        const u64 sizes = rasterizer.UpscaleSizes();
        status.render_width = static_cast<u32>((sizes >> 48) & 0xffff);
        status.render_height = static_cast<u32>((sizes >> 32) & 0xffff);
        status.output_width = static_cast<u32>((sizes >> 16) & 0xffff);
        status.output_height = static_cast<u32>(sizes & 0xffff);
        const std::string version = Fsr4Upscaler::RunningVersion();
        status.name = Settings::values.nvidia_dlss.GetValue() && version.empty()
                          ? std::string{"DLSS"}
                          : "FSR" + (version.empty() ? std::string{} : " " + version);
        const auto [tag, reason] = rasterizer.UpscalerIdleReason();
        status.idle_tag = tag;
        status.idle_reason = reason;
        return status;
    }

    // Enhanced platform-specific initialization
    void InitializePlatformSpecific();

private:
    Common::DkbFrameTrace::RateLimit trace_cpu_rate;
    std::chrono::steady_clock::time_point trace_last_frame{};
    VideoCommon::FrameTimeStats frame_time_stats;
    u64 trace_frame{};

    void InterpolateFrames(Frame* prev_frame, Frame* curr_frame);
    Frame* previous_frame = nullptr;  // Store the previous frame for interpolation
    VkCommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(VkCommandBuffer command_buffer);
    void Report() const;

    vk::Buffer RenderToBuffer(std::span<const Tegra::FramebufferConfig> framebuffers,
                              const Layout::FramebufferLayout& layout, VkFormat format,
                              VkDeviceSize buffer_size);
    void RenderScreenshot(std::span<const Tegra::FramebufferConfig> framebuffers);
    void RenderAppletCaptureLayer(std::span<const Tegra::FramebufferConfig> framebuffers);

    std::vector<Tegra::FramebufferConfig> applet_capture_layers;
    std::vector<Tegra::FramebufferConfig> screenshot_layer_scratch;

    Tegra::MaxwellDeviceMemoryManager& device_memory;
    Tegra::GPU& gpu;

    std::shared_ptr<Common::DynamicLibrary> library;
    vk::InstanceDispatch dld;
#ifdef EGLANTINE_STREAMLINE
    bool streamline_started; ///< NVIDIA Streamline starts before the instance.
#endif

    // Keep original handles for compatibility with existing code
    vk::Instance instance;

    vk::DebugUtilsMessenger debug_messenger;

    vk::SurfaceKHR surface;

    Device device;
    MemoryAllocator memory_allocator;
    StateTracker state_tracker;
    Scheduler scheduler;
    Swapchain swapchain;
    PresentManager present_manager;
    BlitScreen blit_swapchain;
    BlitScreen blit_capture;
    BlitScreen blit_applet;
    RasterizerVulkan rasterizer;
#ifdef HAS_LSFG
    FrameGen frame_gen;
#endif
    std::optional<TurboMode> turbo_mode;

    Frame applet_frame;
};

} // namespace Vulkan
