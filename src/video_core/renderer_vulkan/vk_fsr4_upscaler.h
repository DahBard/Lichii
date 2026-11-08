// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;

/// AMD FSR 4, the machine-learning upscaler of AMD FSR SDK 2.4, through the AMD FSR API. Windows
/// only: the API's loader (amd_fidelityfx_loader_vk.dll) and AMD's signed upscaler
/// (amd_fidelityfx_upscaler_vk.dll) are loaded from the executable's folder, and the upscaler
/// records its own passes into the scheduler's command buffer. LICHII_FSR4=0 keeps FSR 3.1;
/// LICHII_FSR4_VERSION=<name> picks another upscaler version the API offers ("3.1.5", ...);
/// LICHII_FSR4_DEBUG=0 turns off the API's checks of each dispatch.
class Fsr4Upscaler {
public:
    struct Image {
        VkImage image{};
        VkFormat format{};
    };
    struct Inputs {
        Image color;          ///< Render size.
        Image depth;          ///< D32 device depth, 0 at the near plane and 1 at the far one.
        Image motion_vectors; ///< Render size, motion to the previous frame.
        Image reactive;       ///< Optional render-size mask.
        Image transparency;   ///< Optional render-size mask.
        VkExtent2D render_size;
        VkExtent2D upscale_size;
        std::array<f32, 2> jitter;
        /// Turns a motion vector into render pixels.
        std::array<f32, 2> motion_vector_scale;
        f32 frame_time_ms;
        bool reset;
        bool sharpen;
        f32 sharpness;
        f32 camera_near;
        f32 camera_far;
        f32 vertical_fov; ///< Radians.
    };

    /// Whether FSR 4 is tried before FSR 3.1: on Windows unless LICHII_FSR4=0.
    [[nodiscard]] static bool Wanted();

    /// Loads the API and creates an upscale context with its newest FSR 4. Null, with the reason
    /// logged, when any step fails.
    [[nodiscard]] static std::unique_ptr<Fsr4Upscaler> Create(const Device& device,
                                                              Scheduler& scheduler,
                                                              MemoryAllocator& allocator,
                                                              VkExtent2D render_size,
                                                              VkExtent2D upscale_size);

    ~Fsr4Upscaler();

    Fsr4Upscaler(const Fsr4Upscaler&) = delete;
    Fsr4Upscaler& operator=(const Fsr4Upscaler&) = delete;

    /// Records one evaluation into the output image: RGBA16F at the upscale size, GENERAL layout,
    /// ready to be sampled. False when new sizes needed a context that could not be made.
    bool Upscale(const Inputs& inputs);

    /// True once the API refused a dispatch or a new context; the caller moves to FSR 3.1.
    [[nodiscard]] bool Failed() const noexcept {
        return failed.load(std::memory_order_relaxed);
    }

    [[nodiscard]] VkImageView OutputView() const noexcept {
        return *output_view;
    }

    /// Nearest-neighbour clamp sampler (normalized coordinates), for reading the output.
    [[nodiscard]] VkSampler PointSampler() const noexcept {
        return *point_sampler;
    }

    /// The API's entry points, from its loader.
    struct Api;

private:
    Fsr4Upscaler(const Device& device, Scheduler& scheduler, MemoryAllocator& allocator,
                 const Api& api, u64 version_id);

    bool CreateContext(VkExtent2D render_size, VkExtent2D upscale_size);
    void DestroyContext();

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& allocator;
    const Api& api;
    const u64 version_id;
    void* context{};
    VkExtent2D max_render_size{};
    VkExtent2D upscale_size{};
    vk::Image output;
    vk::ImageView output_view;
    vk::Image exposure;
    vk::Sampler point_sampler;
    bool images_initialized{};
    std::atomic<bool> failed{};
};

} // namespace Vulkan
