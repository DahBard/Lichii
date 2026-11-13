// SPDX-FileCopyrightText: Copyright 2026 CheezyEmu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// AMD FSR frame generation (FidelityFX SDK 2.4, Vulkan).
//
// AMD's frame generation swapchain replaces the emulator's swapchain: it presents each real frame
// plus one generated in between, paced on its own threads. The generated frame comes from the
// game's depth and motion vectors, which only Ocarina of Time's live upscale pipeline provides, so
// other games present through it with generation off. The DLL picks the version for the GPU:
// FSR Frame Generation 4 on Radeon RX 9000, FSR 3.1 everywhere else (other AMD, NVIDIA, Intel).

#pragma once

#include <array>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

namespace AmdFrameGen {

/// The user picked AMD FSR frame generation (Windows; the device and swapchain set up at startup).
[[nodiscard]] bool Requested();

/// Frames AMD's swapchain was asked to generate so far (one per game frame presented while
/// generating; with HDR10 output, one per generation dispatch that succeeded), for the frame rate
/// shown with the game's.
[[nodiscard]] u64 GeneratedFrames();

/// Whether AMD's frame generation swapchain is in use (it is set up when the emulator starts).
/// Lock free, so the rasterizer gates the frame generation passes on it.
[[nodiscard]] bool SwapchainActive();

/// The swapchain output AMD's frame generation presents: SDR, and HDR10, whose PQ frames its
/// generation decodes up to the HDR10 metadata's peak. scRGB and any other HDR output get a
/// normal swapchain.
enum class Output : u8 {
    Sdr,
    Hdr10,
    Unsupported,
};
[[nodiscard]] Output OutputOf(VkSurfaceFormatKHR format);

/// OutputOf for a swapchain made while AMD's is asked for: whether AMD's may present it. Logs
/// why not, and keeps the output for LastOffReason.
[[nodiscard]] bool CanPresent(VkSurfaceFormatKHR format);

/// Why AMD's frame generation is off with HDR output, for the status bar.
enum class OffReason : u8 {
    None,      ///< No HDR reason
    HdrFormat, ///< HDR output in a format it doesn't present (scRGB)
    HdrFailed, ///< HDR10 output, and its swapchain or its generation couldn't start or failed
};
[[nodiscard]] OffReason LastOffReason();

/// What AMD's swapchain hands its generation to decode an HDR10 frame with.
struct HdrDecode {
    u32 transfer_function; ///< FfxApiBackbufferTransferFunction: 1 is PQ
    f32 min_luminance;     ///< nits
    f32 max_luminance;     ///< nits; PQ decodes to 1.0 there

    bool operator==(const HdrDecode&) const = default;
};
/// That decode with what AMD's swapchain left out filled in: PQ, and 0 to `peak` nits (the peak
/// the game tone maps to) while it has no luminance range, which generation divides by.
[[nodiscard]] HdrDecode CompleteHdr10Decode(const HdrDecode& given, f32 peak);

/// Creates AMD's frame generation swapchain in place of vkCreateSwapchainKHR. Returns
/// VK_NULL_HANDLE when it can't (missing DLL, no spare present queue, creation failed); the caller
/// then creates a normal swapchain.
[[nodiscard]] VkSwapchainKHR CreateSwapchain(const Device& device, std::mutex& submit_mutex,
                                             const VkSwapchainCreateInfoKHR& create_info);

/// What became of HDR10 metadata given to AMD's swapchain.
enum class HdrMetadataResult : u8 {
    Sent,          ///< It has it (also when it had the same already)
    NotSent,       ///< Its version takes none (logged when it started)
    NextSwapchain, ///< It presents from its own threads while generating: a new one takes it
};
/// HDR10 metadata for AMD's swapchain, through its replacement of vkSetHdrMetadataEXT (its handle
/// isn't the driver's). It hands the metadata on to the display, and its generation decodes PQ
/// up to the metadata's peak.
[[nodiscard]] HdrMetadataResult SetHdrMetadata(VkSwapchainKHR swapchain,
                                               const VkHdrMetadataEXT& metadata);

/// Waits for AMD's presents and the last recorded prepare pass, then destroys the swapchain and
/// its context. scheduler is null when nothing recorded will be submitted any more (shutdown).
void DestroySwapchain(const Device& device, Scheduler* scheduler, VkSwapchainKHR swapchain);

/// Lets AMD's in-flight presents finish (before the device idles or the swapchain is replaced).
void WaitForPresents();

/// Destroys every frame generation context once the device is idle and lets go of AMD's runtime
/// (emulator shutdown; does nothing when AMD's runtime was never loaded).
void Shutdown();

[[nodiscard]] std::vector<VkImage> GetImages(const Device& device, VkSwapchainKHR swapchain);

[[nodiscard]] VkResult AcquireNextImage(const Device& device, VkSwapchainKHR swapchain,
                                        VkSemaphore semaphore, u32* image_index);

struct PresentTarget {
    u64 frame_id;  ///< The prepared game frame this present shows; 0 when none (no generation).
    VkRect2D rect; ///< Where the game picture sits in the swapchain image.
    /// The presented image without the game's HUD (swapchain size and format, shader readable),
    /// or null.
    VkImage hudless{};
    VkFormat hudless_format{};
    VkExtent2D hudless_extent{};
};

/// Presents through AMD's swapchain. Must not be called with the scheduler's submit mutex held:
/// AMD submits to the graphics queue from inside, under that mutex.
[[nodiscard]] VkResult Present(VkQueue queue, const VkPresentInfoKHR& info,
                               const PresentTarget& target);

struct PrepareImage {
    VkImage image;
    VkFormat format;
    VkImageAspectFlags aspect;
};

struct PrepareInputs {
    PrepareImage depth;          ///< Device depth, [0,1] growing with distance, GENERAL layout.
    PrepareImage motion_vectors; ///< Pixel motion to the previous frame, GENERAL layout.
    VkExtent2D render_size;
    std::array<f32, 16> view_projection; ///< Row-major, [-1,1] clip depth.
    bool reset;
};

/// Records AMD's prepare pass for one game frame on the scheduler.
void Prepare(Scheduler& scheduler, const PrepareInputs& inputs);

/// The scheduler tick the last prepare pass was recorded at, or 0.
[[nodiscard]] u64 LastPrepareTick();

/// On the scheduler worker, in order with the recorded commands: the frame prepared since the
/// last call, or 0.
[[nodiscard]] u64 TakePreparedFrame();

} // namespace AmdFrameGen
} // namespace Vulkan
