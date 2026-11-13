// SPDX-FileCopyrightText: Copyright 2026 CheezyEmu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "common/display_hdr.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_amd_frame_gen.h"
#include "video_core/renderer_vulkan/vk_ffx_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vk_enum_string_helper.h"
#include "video_core/vulkan_common/vulkan_device.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "api/include/ffx_api.h"
#include "api/include/ffx_api_types.h"
#include "api/include/vk/ffx_api_vk.h"
// AMD's header wraps deprecated structs in warning pragmas defined only in its own build.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#endif
#include "framegeneration/include/ffx_framegeneration.h"
#include "framegeneration/include/vk/ffx_api_framegeneration_vk.h"
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#endif

namespace Vulkan::AmdFrameGen {

namespace {
std::atomic<u64> generated_frames{0};
constexpr u32 TRANSFER_FUNCTION_PQ = 1; // FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ
} // Anonymous namespace

u64 GeneratedFrames() {
    return generated_frames.load(std::memory_order_relaxed);
}

Output OutputOf(VkSurfaceFormatKHR format) {
    if (format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        return Output::Sdr;
    }
    // Not scRGB: Lichii sends it no HDR metadata, so AMD's swapchain passes a 0-0 nits range,
    // which its scRGB decode divides by. The callback could fill that in as for HDR10, but FSR
    // FG 4's scRGB path is closed source and untested. HDR10 only in the format AMD's runtime has
    // (no A2R10G10B10).
    if (format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
        format.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32) {
        return Output::Hdr10;
    }
    return Output::Unsupported;
}

HdrDecode CompleteHdr10Decode(const HdrDecode& given, f32 peak) {
    HdrDecode decode = given;
    decode.transfer_function = TRANSFER_FUNCTION_PQ;
    if (!(given.max_luminance > 0.0f)) {
        decode.min_luminance = 0.0f;
        decode.max_luminance = peak;
    }
    return decode;
}

bool Requested() {
#ifdef _WIN32
    return Settings::values.frame_gen.GetValue() &&
           Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::AmdFsr;
#else
    return false; // AMD ships the frame generation runtime for Windows only
#endif
}

#ifdef _WIN32

namespace {

using Api = FfxRuntime::Api;

/// AMD's swapchain was created and is in use (lock free: the rasterizer asks per frame).
std::atomic<bool> swapchain_active{false};
/// A generation context takes prepare passes (read on the recording thread without the lock).
std::atomic<bool> prepare_ready{false};
/// The scheduler tick of the last prepare pass recorded, so a swapchain replacement can wait
/// for the command buffer that holds it.
std::atomic<u64> last_prepare_tick{0};
/// The output of the last swapchain made while AMD's was asked for (CanPresent), and whether its
/// swapchain or its generation last failed to start with HDR10 output: for LastOffReason.
std::atomic<Output> last_output{Output::Sdr};
std::atomic<bool> hdr_failed{false};

static_assert(TRANSFER_FUNCTION_PQ == FFX_API_BACKBUFFER_TRANSFER_FUNCTION_PQ);

/// EDEN_NX2_AMD_FG_DEBUG=1 draws AMD's tear lines (coloured bars on generated frames), =2 its
/// debug view (the motion vectors, depth and generated frame it works from), =3 turns on its
/// parameter checks, which report to the log. 1 and 2 write raw values, which HDR10 shows at up
/// to 10000 nits.
char DebugMode() {
    static const char mode = [] {
        const char* const value = std::getenv("EDEN_NX2_AMD_FG_DEBUG");
        return value ? value[0] : '0';
    }();
    return mode;
}

bool SameHdrMetadata(const VkHdrMetadataEXT& a, const VkHdrMetadataEXT& b) {
    const auto same = [](const VkXYColorEXT& p, const VkXYColorEXT& q) {
        return p.x == q.x && p.y == q.y;
    };
    return same(a.displayPrimaryRed, b.displayPrimaryRed) &&
           same(a.displayPrimaryGreen, b.displayPrimaryGreen) &&
           same(a.displayPrimaryBlue, b.displayPrimaryBlue) && same(a.whitePoint, b.whitePoint) &&
           a.maxLuminance == b.maxLuminance && a.minLuminance == b.minLuminance &&
           a.maxContentLightLevel == b.maxContentLightLevel &&
           a.maxFrameAverageLightLevel == b.maxFrameAverageLightLevel;
}

VkDeviceContext DeviceContext(const Device& device) {
    return VkDeviceContext{
        .vkDevice = *device.GetLogical(),
        .vkPhysicalDevice = static_cast<VkPhysicalDevice>(device.GetPhysical()),
        .vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr,
    };
}

/// A generation context taken out of use; destroyed once frames recorded against it are done.
struct Retired {
    ffxContext context;
    u64 present_count;
};

struct State {
    // AMD's contexts aren't thread safe: Prepare runs on the scheduler worker, Present on the
    // present thread (and AMD's frame generation callback inside it).
    std::mutex mutex;
    std::shared_ptr<const Api> runtime; ///< Held from the first swapchain to Shutdown
    const Api* api{};
    VkDevice device{};
    const Device* owner{};          ///< For recreating the generation context after a failure.
    VkExtent2D display{};
    VkFormat format{};
    VkColorSpaceKHR color_space{}; ///< The swapchain's: sRGB, or HDR10 (PQ frames)
    /// HDR10: the nits generation decodes PQ to 1.0 at, the peak the game tone maps to. Atomic:
    /// AMD's frame generation callback reads it.
    std::atomic<f32> hdr_peak{};
    bool hdr_checked{}; ///< The decode its swapchain passes with HDR10 output was logged
    std::optional<VkHdrMetadataEXT> hdr_metadata; ///< The HDR10 metadata it has
    u32 context_retries{};
    ffxContext swapchain_context{};
    ffxContext frame_gen_context{};
    VkSwapchainKHR swapchain{};
    ffxQueryDescSwapchainReplacementFunctionsVK functions{};
    std::vector<Retired> retired;
    u64 present_count{};
    u64 next_frame_id = 1;
    u64 configured_frame_id{};
    bool generating{};
    bool prepare_failed{};
    bool suspended{}; ///< The swapchain is being replaced: recorded prepare passes do nothing
    std::string version;
    // AMD submits to the graphics queue from its own threads; it goes through the emulator's lock.
    VkQueue game_queue{};
    PFN_vkQueueSubmit queue_submit{};
    std::mutex* submit_mutex{};
    bool used_by_earlier_game{}; ///< A game that stopped had AMD's swapchain in this process
};

State& GetState() {
    static State state;
    return state;
}

/// Called with state.mutex held whenever the generation context or its failure flags change.
void PublishPrepareReady(const State& state) {
    prepare_ready.store(state.frame_gen_context && !state.prepare_failed && !state.suspended,
                        std::memory_order_release);
}

std::atomic<u64> prepared_frame{0};

VkResult SubmitToGameQueue(uint32_t count, const VkSubmitInfo* submits, VkFence fence) {
    State& state = GetState();
    std::scoped_lock lock{*state.submit_mutex};
    return state.queue_submit(state.game_queue, count, submits, fence);
}

ffxReturnCode_t DispatchFrameGeneration(ffxDispatchDescFrameGeneration* params, void* user) {
    State& state = *static_cast<State*>(user);
    if (state.color_space == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
        // AMD's swapchain takes the transfer function from the colour space and the luminance
        // range from its vkSetHdrMetadataEXT, 0 until that is called. The description is AMD's
        // copy for this dispatch: what is missing is filled in here.
        const HdrDecode given{params->backbufferTransferFunction, params->minMaxLuminance[0],
                              params->minMaxLuminance[1]};
        const HdrDecode decode =
            CompleteHdr10Decode(given, state.hdr_peak.load(std::memory_order_relaxed));
        if (!std::exchange(state.hdr_checked, true)) {
            LOG_INFO(Render_Vulkan,
                     "AMD frame generation: its swapchain decodes HDR10 frames with transfer "
                     "function {} (1 is PQ), {}-{} nits{}",
                     given.transfer_function, given.min_luminance, given.max_luminance,
                     decode == given ? std::string{}
                                     : fmt::format("; using PQ, {}-{} nits", decode.min_luminance,
                                                   decode.max_luminance));
        }
        params->backbufferTransferFunction = decode.transfer_function;
        params->minMaxLuminance[0] = decode.min_luminance;
        params->minMaxLuminance[1] = decode.max_luminance;
    }
    return state.api->dispatch(&state.frame_gen_context, &params->header);
}

void DestroyRetired(State& state, bool all) {
    std::erase_if(state.retired, [&](Retired& entry) {
        // Prepare passes recorded against it were submitted frames ago by now.
        if (!all && state.present_count < entry.present_count + 16) {
            return false;
        }
        state.api->destroy_context(&entry.context, nullptr);
        return true;
    });
}

/// Turns generation off for this context and takes it out of use (AMD destroys the generation
/// context before the swapchain; frames already recorded keep it alive a little longer).
void RetireFrameGenContext(State& state) {
    if (!state.frame_gen_context) {
        return;
    }
    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = state.swapchain;
    config.frameGenerationEnabled = false;
    config.frameID = state.configured_frame_id;
    state.api->configure(&state.frame_gen_context, &config.header);
    state.retired.push_back({state.frame_gen_context, state.present_count});
    state.frame_gen_context = nullptr;
    state.generating = false;
    PublishPrepareReady(state);
}

/// FSR 3.1 frame generation: chosen in the settings, or (Auto) on a GPU without FP8 shader math.
/// FSR 4's frame generation is for RDNA 4; AMD's runtime still offers it first on other GPUs,
/// where it started but never made a frame, so Auto used to leave frame generation off there
/// until a player picked FSR 3.1 by hand.
bool WantFsr31(const Device& device) {
    if (Settings::values.amd_frame_gen_version.GetValue() ==
        Settings::AmdFrameGenVersion::Fsr31) {
        return true;
    }
    const bool fsr4_capable = device.IsFloat8Supported();
    static std::once_flag logged;
    std::call_once(logged, [&] {
        LOG_INFO(Render_Vulkan, "AMD frame generation: Auto picks FSR {} on {} ({})",
                 fsr4_capable ? "4" : "3.1", device.GetModelName(),
                 fsr4_capable ? "has FP8 shader math" : "no FP8 shader math, so not FSR 4's");
    });
    return !fsr4_capable;
}

/// The newest 3.x version AMD's runtime offers for `create_type` on this device, if any.
std::optional<std::pair<u64, std::string>> NewestFsr3(const Api& api, VkDeviceContext& vk_context,
                                                      u64 create_type) {
    u64 count = 0;
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = create_type;
    versions.device = &vk_context;
    versions.outputCount = &count;
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK || count == 0) {
        return std::nullopt;
    }
    std::vector<u64> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    if (api.query(nullptr, &versions.header) != FFX_API_RETURN_OK) {
        return std::nullopt;
    }
    for (u64 i = 0; i < count; ++i) { // newest first
        const std::string name = names[i] ? names[i] : "";
        if (name.starts_with("3.")) {
            return std::pair{ids[i], name};
        }
    }
    return std::nullopt;
}

bool CreateFrameGenContext(State& state, const Device& device, VkExtent2D display,
                           VkFormat format) {
    VkDeviceContext vk_context = DeviceContext(device);
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = vk_context;
    ffxCreateContextDescFrameGenerationVersion version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    version.version = FFX_FRAMEGENERATION_VERSION;
    ffxCreateContextDescFrameGeneration create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    // Depth grows with distance and motion is unjittered. HDR10 frames are PQ: AMD's flag for HDR
    // colour input, as its sample sets.
    const bool hdr = state.color_space != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    create.flags = hdr ? u32{FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE} : 0u;
    if (DebugMode() == '3') {
        create.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    }
    create.displaySize = {display.width, display.height};
    create.maxRenderSize = {std::max(display.width, 3840u), std::max(display.height, 2160u)};
    create.backBufferFormat = ffxApiGetSurfaceFormatVK(format);
    // Chain: frame generation <- version <- [override <- ] backend.
    create.header.pNext = &version.header;
    version.header.pNext = &backend.header;
    ffxOverrideVersion override_version{};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    if (WantFsr31(device)) {
        if (const auto fsr3 = NewestFsr3(*state.api, vk_context,
                                         FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION)) {
            override_version.versionId = fsr3->first;
            override_version.header.pNext = &backend.header;
            version.header.pNext = &override_version.header;
            LOG_INFO(Render_Vulkan, "AMD frame generation: using FSR {}", fsr3->second);
        }
    }
    const ffxReturnCode_t result =
        state.api->create_context(&state.frame_gen_context, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK) {
        LOG_ERROR(Render_Vulkan, "AMD frame generation: context creation failed ({})", result);
        state.frame_gen_context = nullptr;
        return false;
    }
    ffxQueryGetProviderVersion provider{};
    provider.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    state.api->query(&state.frame_gen_context, &provider.header);
    state.version = provider.versionName ? provider.versionName : "unknown";
    LOG_INFO(Render_Vulkan, "AMD frame generation: FSR frame generation {} ready at {}x{} on {}{}",
             state.version, display.width, display.height, device.GetModelName(),
             hdr ? fmt::format(", HDR output {} {} (PQ)", string_VkFormat(format),
                               string_VkColorSpaceKHR(state.color_space))
                 : std::string{});
    if (hdr) {
        hdr_failed.store(false, std::memory_order_relaxed);
    }
    PublishPrepareReady(state);
    return true;
}

FfxApiResource ImageResource(const PrepareImage& image, VkExtent2D size, u32 usage) {
    FfxApiResourceDescription description{};
    description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    description.format = ffxApiGetSurfaceFormatVK(image.format);
    description.width = size.width;
    description.height = size.height;
    description.depth = 1;
    description.mipCount = 1;
    description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    description.usage = usage;
    return ffxApiGetResourceVK(reinterpret_cast<void*>(image.image), description,
                               FFX_API_RESOURCE_STATE_COMPUTE_READ);
}

VkImageMemoryBarrier Barrier(const PrepareImage& image, VkAccessFlags src, VkAccessFlags dst,
                             VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image.image,
        .subresourceRange{
            .aspectMask = image.aspect,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
}

/// The camera FSR frame generation asks for, taken apart from the game's view-projection.
struct Camera {
    f32 near_plane = 0.1f;
    f32 far_plane = 10000.0f;
    f32 fov = 1.0f;
    std::array<f32, 3> position{};
    std::array<f32, 3> up{0, 1, 0};
    std::array<f32, 3> right{1, 0, 0};
    std::array<f32, 3> forward{0, 0, 1};
};

Camera ExtractCamera(const std::array<f32, 16>& m) {
    using Vec = std::array<f64, 4>;
    const auto row = [&m](size_t r) {
        return Vec{m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]};
    };
    const auto dot3 = [](const Vec& a, const Vec& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    Vec r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    Camera camera;
    const f64 length = std::sqrt(dot3(r3, r3));
    if (!(length > 1e-9) || !std::isfinite(length)) {
        return camera;
    }
    // Scale so clip w is the distance along the view axis; projective results don't change.
    for (Vec* v : {&r0, &r1, &r2, &r3}) {
        for (f64& x : *v) {
            x /= length;
        }
    }
    const auto unit = [&](Vec v) -> std::optional<std::array<f32, 3>> {
        const f64 along = dot3(v, r3);
        for (size_t i = 0; i < 3; ++i) {
            v[i] -= along * r3[i];
        }
        const f64 len = std::sqrt(dot3(v, v));
        if (!(len > 1e-9)) {
            return std::nullopt;
        }
        return std::array<f32, 3>{static_cast<f32>(v[0] / len), static_cast<f32>(v[1] / len),
                                  static_cast<f32>(v[2] / len)};
    };
    camera.forward = {static_cast<f32>(r3[0]), static_cast<f32>(r3[1]), static_cast<f32>(r3[2])};
    if (const auto right = unit(r0)) {
        camera.right = *right;
    }
    if (const auto up = unit(r1)) {
        camera.up = *up;
    }
    // Vertical focal length: row 1 without its forward (off-centre) part.
    Vec r1_lateral = r1;
    const f64 r1_along = dot3(r1, r3);
    for (size_t i = 0; i < 3; ++i) {
        r1_lateral[i] -= r1_along * r3[i];
    }
    const f64 focal = std::sqrt(dot3(r1_lateral, r1_lateral));
    if (focal > 1e-6 && std::isfinite(focal)) {
        camera.fov = static_cast<f32>(2.0 * std::atan(1.0 / focal));
    }
    // Clip z = a * w + c, so NDC z = a + c / w with [-1, 1] depth.
    const f64 a = dot3(r2, r3);
    const f64 c = r2[3] - a * r3[3];
    const f64 near_plane = c / (-1.0 - a);
    const f64 far_plane = std::abs(1.0 - a) > 1e-9 ? c / (1.0 - a) : 1e9;
    if (std::isfinite(near_plane) && near_plane > 0.0 && near_plane < 1e6) {
        camera.near_plane = static_cast<f32>(near_plane);
        camera.far_plane = static_cast<f32>(
            std::isfinite(far_plane) && far_plane > near_plane ? std::min(far_plane, 1e7)
                                                               : near_plane * 1e5);
    }
    // The eye is where clip x, y and w are all zero.
    const std::array<std::array<f64, 3>, 3> eq{{
        {r0[0], r0[1], r0[2]},
        {r1[0], r1[1], r1[2]},
        {r3[0], r3[1], r3[2]},
    }};
    const std::array<f64, 3> rhs{-r0[3], -r1[3], -r3[3]};
    const auto det3 = [](const std::array<std::array<f64, 3>, 3>& q) {
        return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) -
               q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
               q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    const f64 det = det3(eq);
    if (std::abs(det) > 1e-12) {
        for (size_t axis = 0; axis < 3; ++axis) {
            auto q = eq;
            for (size_t r = 0; r < 3; ++r) {
                q[r][axis] = rhs[r];
            }
            camera.position[axis] = static_cast<f32>(det3(q) / det);
        }
    }
    return camera;
}

} // Anonymous namespace

void ResetFrames(State& state);

VkSwapchainKHR CreateSwapchain(const Device& device, std::mutex& submit_mutex,
                               const VkSwapchainCreateInfoKHR& create_info) {
    if (!Requested()) {
        return VK_NULL_HANDLE;
    }
    if (!device.GetFrameGenPresentQueue()) {
        return VK_NULL_HANDLE; // logged by the device
    }
    const bool hdr = create_info.imageColorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    // AMD's runtime kept something of a stopped game's device: the next game in the same process
    // crashed in the Vulkan loader (a null handle) after its generation context was made. It is
    // unloaded at Shutdown now; should it have stayed loaded, present without it.
    if (state.used_by_earlier_game) {
        static bool logged = false;
        if (!std::exchange(logged, true)) {
            LOG_WARNING(Render_Vulkan, "AMD frame generation: off for this game; restart Lichii "
                                       "to use it again after stopping a game");
        }
        return VK_NULL_HANDLE;
    }
    if (!state.runtime) {
        state.runtime = FfxRuntime::Acquire();
    }
    state.api = state.runtime.get();
    if (!state.api) {
        return VK_NULL_HANDLE;
    }
    state.device = *device.GetLogical();
    state.game_queue = *device.GetGraphicsQueue();
    state.queue_submit = device.GetDispatchLoader().vkQueueSubmit;
    state.submit_mutex = &submit_mutex;
    const VkQueueInfoFFXAPI game{state.game_queue, device.GetGraphicsFamily(), SubmitToGameQueue};

    ffxCreateContextDescFrameGenerationSwapChainVersionVK version{};
    version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
    version.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    ffxCreateContextDescFrameGenerationSwapChainVK desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
    desc.header.pNext = &version.header;
    desc.physicalDevice = static_cast<VkPhysicalDevice>(device.GetPhysical());
    desc.device = state.device;
    desc.swapchain = &swapchain;
    desc.allocator = nullptr;
    desc.createInfo = create_info;
    desc.createInfo.oldSwapchain = VK_NULL_HANDLE;
    // AMD's sample gives each of its threads a queue of its own; share the game queue only for
    // roles the GPU has no spare queue for.
    const u32 frame_gen_family = device.GetFrameGenPresentFamily();
    const auto own_queue = [&](size_t index) {
        const VkQueue queue = device.GetFrameGenQueue(index);
        return queue ? VkQueueInfoFFXAPI{queue, frame_gen_family, nullptr} : game;
    };
    desc.gameQueue = game;
    desc.presentQueue = own_queue(0);
    desc.asyncComputeQueue = own_queue(1);
    desc.imageAcquireQueue = own_queue(2);
    // AMD's runtime explains its failures through this callback.
    static const ffxApiMessage log_message = [](uint32_t type, const wchar_t* message) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
        std::string text(size > 0 ? size - 1 : 0, '\0');
        if (size > 1) {
            WideCharToMultiByte(CP_UTF8, 0, message, -1, text.data(), size, nullptr, nullptr);
        }
        if (type == FFX_API_MESSAGE_TYPE_ERROR) {
            LOG_ERROR(Render_Vulkan, "AMD FidelityFX: {}", text);
        } else {
            LOG_WARNING(Render_Vulkan, "AMD FidelityFX: {}", text);
        }
    };
    for (const u64 effect : {u64{FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN_VK},
                             u64{FFX_API_EFFECT_ID_FRAMEGENERATION}}) {
        ffxConfigureDescGlobalDebug debug{};
        debug.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
        debug.effectId = effect;
        debug.fpMessage = log_message;
        debug.debugLevel = 0xffffffffu; // everything
        state.api->configure(nullptr, &debug.header);
    }
    const auto& ci = desc.createInfo;
    LOG_INFO(Render_Vulkan,
             "AMD frame generation: creating its swapchain {}x{} format {} colour space {} usage "
             "{:#x} present mode {} images {} sharing {} transform {:#x} alpha {:#x}; game queue "
             "family {}, present queue family {}",
             ci.imageExtent.width, ci.imageExtent.height, static_cast<int>(ci.imageFormat),
             static_cast<int>(ci.imageColorSpace), ci.imageUsage, static_cast<int>(ci.presentMode),
             ci.minImageCount, static_cast<int>(ci.imageSharingMode),
             static_cast<u32>(ci.preTransform), static_cast<u32>(ci.compositeAlpha),
             device.GetGraphicsFamily(), device.GetFrameGenPresentFamily());
    // The swapchain versions the DLL offers this GPU, newest first: try the default, then each.
    VkDeviceContext vk_context = DeviceContext(device);
    std::vector<std::pair<u64, std::string>> offered;
    {
        u64 count = 0;
        ffxQueryDescGetVersions versions{};
        versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        versions.device = &vk_context;
        versions.outputCount = &count;
        if (state.api->query(nullptr, &versions.header) == FFX_API_RETURN_OK && count > 0) {
            std::vector<u64> ids(count);
            std::vector<const char*> names(count);
            versions.versionIds = ids.data();
            versions.versionNames = names.data();
            if (state.api->query(nullptr, &versions.header) == FFX_API_RETURN_OK) {
                for (u64 i = 0; i < count; ++i) {
                    offered.emplace_back(ids[i], names[i] ? names[i] : "?");
                }
            }
        }
        std::string list;
        for (const auto& [id, name] : offered) {
            list += (list.empty() ? "" : ", ") + name;
        }
        LOG_INFO(Render_Vulkan, "AMD frame generation: swapchain versions offered: {}",
                 list.empty() ? "none" : list);
    }
    std::vector<std::optional<std::pair<u64, std::string>>> attempts{std::nullopt};
    for (const auto& entry : offered) {
        attempts.emplace_back(entry);
    }
    if (WantFsr31(device)) {
        // FSR 3.1's swapchain, to match its frame generator (the default may be FSR 4's).
        std::vector<std::optional<std::pair<u64, std::string>>> fsr3;
        for (const auto& entry : offered) {
            if (entry.second.starts_with("3.")) {
                fsr3.emplace_back(entry);
            }
        }
        if (!fsr3.empty()) {
            attempts = std::move(fsr3);
        }
    }
    ffxReturnCode_t result = FFX_API_RETURN_ERROR;
    for (const auto& attempt : attempts) {
        ffxOverrideVersion override_version{};
        override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
        version.header.pNext = nullptr;
        if (attempt) {
            override_version.versionId = attempt->first;
            version.header.pNext = &override_version.header;
        }
        swapchain = VK_NULL_HANDLE;
        state.swapchain_context = nullptr;
        result = state.api->create_context(&state.swapchain_context, &desc.header, nullptr);
        const std::string name = attempt ? attempt->second : std::string{"default"};
        if (result == FFX_API_RETURN_OK && swapchain != VK_NULL_HANDLE) {
            // The functions frames go through. Destroy is optional (checked where it is called):
            // AMD's FSR 4 swapchain (driver 2.0.416, RX 7000) answered without it and frame
            // generation stayed off for everyone on that driver. Without the others, the next
            // offered version (FSR 3.1's) is tried instead of giving up.
            state.functions = {};
            state.functions.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
            const ffxReturnCode_t query =
                state.api->query(&state.swapchain_context, &state.functions.header);
            if (query == FFX_API_RETURN_OK && state.functions.pOutQueuePresentKHR &&
                state.functions.pOutAcquireNextImageKHR &&
                state.functions.pOutGetSwapchainImagesKHR) {
                LOG_INFO(Render_Vulkan, "AMD frame generation: swapchain {} started{}", name,
                         state.functions.pOutDestroySwapchainFFXAPI
                             ? ""
                             : " (no destroy function: the context destroys it)");
                if (hdr && !state.functions.pOutSetHdrMetadataEXT) {
                    LOG_WARNING(Render_Vulkan,
                                "AMD frame generation: swapchain {} takes no HDR metadata: the "
                                "display gets none, and generation decodes PQ to the peak the "
                                "game was told",
                                name);
                }
                break;
            }
            LOG_WARNING(Render_Vulkan,
                        "AMD frame generation: swapchain {} gave no functions ({}: present {}, "
                        "acquire {}, images {}), trying the next version",
                        name, query, state.functions.pOutQueuePresentKHR != nullptr,
                        state.functions.pOutAcquireNextImageKHR != nullptr,
                        state.functions.pOutGetSwapchainImagesKHR != nullptr);
            // Its real swapchain holds the window until it is destroyed, which its context
            // doesn't do: the next version's, or a normal swapchain, couldn't be made. In the
            // order DestroySwapchain uses.
            state.api->destroy_context(&state.swapchain_context, nullptr);
            state.swapchain_context = nullptr;
            if (state.functions.pOutDestroySwapchainFFXAPI) {
                state.functions.pOutDestroySwapchainFFXAPI(state.device, swapchain, nullptr,
                                                           nullptr);
            }
            state.functions = {};
            result = FFX_API_RETURN_ERROR;
        } else {
            LOG_WARNING(Render_Vulkan, "AMD frame generation: swapchain {} failed ({})", name,
                        result);
        }
        if (state.swapchain_context) {
            state.api->destroy_context(&state.swapchain_context, nullptr);
        }
        state.swapchain_context = nullptr;
        swapchain = VK_NULL_HANDLE;
    }
    if (result != FFX_API_RETURN_OK || swapchain == VK_NULL_HANDLE) {
        LOG_ERROR(Render_Vulkan, "AMD frame generation: no swapchain version worked ({}), using "
                                 "a normal swapchain{}",
                  result, hdr ? " (HDR output without frame generation)" : "");
        state.swapchain_context = nullptr;
        if (hdr) {
            hdr_failed.store(true, std::memory_order_relaxed);
        }
        return VK_NULL_HANDLE;
    }
    state.swapchain = swapchain;
    state.owner = &device;
    ResetFrames(state);
    state.display = create_info.imageExtent;
    state.format = create_info.imageFormat;
    state.color_space = create_info.imageColorSpace;
    state.hdr_checked = false;
    state.hdr_metadata.reset();
    // Until the HDR10 metadata comes (SetHdrMetadata): the peak the game was told, which the
    // metadata carries.
    state.hdr_peak.store(
        hdr ? static_cast<f32>(Common::ResolveHdrOutput(
                                   Common::DisplayHdrMonitor::Instance().Current(),
                                   Settings::HdrMode::On,
                                   Settings::values.hdr_peak_luminance.GetValue(),
                                   Settings::values.hdr_reference_white.GetValue())
                                   .peak_luminance)
            : 0.0f,
        std::memory_order_relaxed);
    state.context_retries = 0;
    state.suspended = false;
    if (!CreateFrameGenContext(state, device, create_info.imageExtent, create_info.imageFormat)) {
        LOG_WARNING(Render_Vulkan, "AMD frame generation: presenting without generation");
    }
    swapchain_active.store(true, std::memory_order_release);
    return swapchain;
}

bool CanPresent(VkSurfaceFormatKHR format) {
    const Output output = OutputOf(format);
    last_output.store(output, std::memory_order_relaxed);
    if (output != Output::Unsupported) {
        return true;
    }
    LOG_INFO(Render_Vulkan,
             "AMD frame generation: off with {} {} output; set the HDR format to HDR10 to use it",
             string_VkFormat(format.format), string_VkColorSpaceKHR(format.colorSpace));
    return false;
}

OffReason LastOffReason() {
    switch (last_output.load(std::memory_order_relaxed)) {
    case Output::Unsupported:
        return OffReason::HdrFormat;
    case Output::Hdr10:
        return hdr_failed.load(std::memory_order_relaxed) ? OffReason::HdrFailed
                                                          : OffReason::None;
    case Output::Sdr:
        break;
    }
    return OffReason::None;
}

HdrMetadataResult SetHdrMetadata(VkSwapchainKHR swapchain, const VkHdrMetadataEXT& metadata) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (swapchain != state.swapchain) {
        return HdrMetadataResult::NotSent;
    }
    if (!state.functions.pOutSetHdrMetadataEXT) {
        // Logged when it started. Generation decodes to the peak all the same (the callback).
        state.hdr_peak.store(metadata.maxLuminance, std::memory_order_relaxed);
        return HdrMetadataResult::NotSent;
    }
    if (state.hdr_metadata && SameHdrMetadata(*state.hdr_metadata, metadata)) {
        return HdrMetadataResult::Sent;
    }
    // While it generates, its own threads acquire and present its real swapchain under a lock of
    // their own, and its vkSetHdrMetadataEXT hands the metadata to that swapchain without it.
    // Vulkan forbids that at the same time as a present, so a new swapchain takes the metadata
    // before it generates again.
    if (state.generating) {
        LOG_INFO(Render_Vulkan,
                 "AMD frame generation: new HDR10 metadata (peak {} nits) while generating; its "
                 "next swapchain takes it",
                 metadata.maxLuminance);
        return HdrMetadataResult::NextSwapchain;
    }
    state.hdr_metadata = metadata;
    state.hdr_peak.store(metadata.maxLuminance, std::memory_order_relaxed);
    state.functions.pOutSetHdrMetadataEXT(state.device, 1, &swapchain, &metadata);
    LOG_INFO(Render_Vulkan,
             "AMD frame generation: HDR10 metadata through its swapchain, peak {} nits",
             metadata.maxLuminance);
    return HdrMetadataResult::Sent;
}

void WaitForPresents() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    if (!state.swapchain_context) {
        return;
    }
    ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK wait{};
    wait.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
    state.api->dispatch(&state.swapchain_context, &wait.header);
}

void DestroySwapchain(const Device& device, Scheduler* scheduler, VkSwapchainKHR swapchain) {
    WaitForPresents();
    State& state = GetState();
    swapchain_active.store(false, std::memory_order_release);
    {
        std::scoped_lock lock{state.mutex};
        state.suspended = true;
        PublishPrepareReady(state);
    }
    // The idled device covers submitted work only: a prepare pass the worker already recorded
    // into a command buffer the recording thread hasn't submitted yet would run against the
    // destroyed context. Wait for it (the recording thread submits before it waits for a free
    // frame, see PresentManager::GetRenderFrame), without the lock the worker takes.
    const u64 tick = last_prepare_tick.load(std::memory_order_acquire);
    if (scheduler && tick != 0 && !scheduler->IsFree(tick)) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
        bool submitted = scheduler->IsSubmitted(tick);
        while (!submitted && std::chrono::steady_clock::now() < deadline) {
            submitted = scheduler->WaitForSubmission(tick);
        }
        if (submitted) {
            scheduler->Wait(tick, 0.0, false);
        } else {
            LOG_WARNING(Render_Vulkan, "AMD frame generation: the last prepare pass (tick {}) "
                                       "wasn't submitted in time; replacing the swapchain anyway",
                        tick);
        }
    }
    std::scoped_lock lock{state.mutex};
    // AMD destroys the generation context before its swapchain. Retired, it was destroyed 16
    // presents later, on the next swapchain, after its own was gone: going fullscreen (a new
    // swapchain) lost the device seconds later. The caller has idled the device and the last
    // prepare pass is done, so nothing recorded against it is still running.
    RetireFrameGenContext(state);
    DestroyRetired(state, true);
    if (state.swapchain_context) {
        state.api->destroy_context(&state.swapchain_context, nullptr);
        state.swapchain_context = nullptr;
    }
    if (swapchain && state.functions.pOutDestroySwapchainFFXAPI) {
        state.functions.pOutDestroySwapchainFFXAPI(*device.GetLogical(), swapchain, nullptr,
                                                   nullptr);
    }
    state.swapchain = VK_NULL_HANDLE;
    state.functions = {};
}

/// Forgets one game's frames: the next game's first present asked AMD to generate from the last
/// frame the previous game prepared, in a context that never saw it (the emulator closed on the
/// next boot without being restarted).
void ResetFrames(State& state) {
    prepared_frame.store(0);
    state.next_frame_id = 1;
    state.configured_frame_id = 0;
    state.generating = false;
    state.prepare_failed = false;
    state.context_retries = 0;
    state.present_count = 0;
}

void Shutdown() {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    swapchain_active.store(false, std::memory_order_release);
    // The next game counts its own generated frames (the status bar starts from 0 at boot), and
    // its scheduler counts ticks from the start.
    generated_frames.store(0, std::memory_order_relaxed);
    last_prepare_tick.store(0, std::memory_order_release);
    last_output.store(Output::Sdr, std::memory_order_relaxed);
    hdr_failed.store(false, std::memory_order_relaxed);
    if (!state.api) {
        return;
    }
    RetireFrameGenContext(state);
    DestroyRetired(state, true);
    ResetFrames(state);
    state.owner = nullptr;
    // The next game loads AMD's runtime afresh once the last holder (FSR 4 goes first) lets go.
    // Should frame generation stay loaded anyway, the next game presents without it.
    state.api = nullptr;
    state.runtime.reset();
    state.used_by_earlier_game = FfxRuntime::FrameGenerationLoaded();
    state.device = VK_NULL_HANDLE;
    state.swapchain = VK_NULL_HANDLE;
    state.game_queue = VK_NULL_HANDLE;
    state.queue_submit = nullptr;
    state.submit_mutex = nullptr;
    state.suspended = false;
    PublishPrepareReady(state);
}

std::vector<VkImage> GetImages(const Device& device, VkSwapchainKHR swapchain) {
    State& state = GetState();
    u32 count = 0;
    vk::Check(state.functions.pOutGetSwapchainImagesKHR(*device.GetLogical(), swapchain, &count,
                                                        nullptr));
    std::vector<VkImage> images(count);
    vk::Check(state.functions.pOutGetSwapchainImagesKHR(*device.GetLogical(), swapchain, &count,
                                                        images.data()));
    images.resize(count);
    return images;
}

VkResult AcquireNextImage(const Device& device, VkSwapchainKHR swapchain, VkSemaphore semaphore,
                          u32* image_index) {
    State& state = GetState();
    return state.functions.pOutAcquireNextImageKHR(*device.GetLogical(), swapchain,
                                                   (std::numeric_limits<u64>::max)(), semaphore,
                                                   VK_NULL_HANDLE, image_index);
}

VkResult Present(VkQueue queue, const VkPresentInfoKHR& info, const PresentTarget& target) {
    State& state = GetState();
    std::scoped_lock lock{state.mutex};
    ++state.present_count;
    DestroyRetired(state, false);
    // A rebuild in a burst of window resizes can fail while older contexts are still alive: try
    // again once they're gone (about once a second, a few times).
    if (!state.frame_gen_context && state.swapchain && state.owner && !state.prepare_failed &&
        state.context_retries < 5 && state.present_count % 60 == 0) {
        ++state.context_retries;
        DestroyRetired(state, false);
        if (CreateFrameGenContext(state, *state.owner, state.display, state.format)) {
            LOG_INFO(Render_Vulkan, "AMD frame generation: back on after a retry");
        } else if (state.context_retries == 5 &&
                   state.color_space != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            hdr_failed.store(true, std::memory_order_relaxed);
            LOG_ERROR(Render_Vulkan, "AMD frame generation: no generation context with HDR "
                                     "output after {} retries, presenting HDR without generation",
                      state.context_retries);
        }
    }
    if (state.frame_gen_context) {
        const bool generate = target.frame_id != 0;
        ffxConfigureDescFrameGeneration config{};
        config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        config.swapChain = state.swapchain;
        config.presentCallback = nullptr;
        config.presentCallbackUserContext = nullptr;
        config.frameGenerationCallback = DispatchFrameGeneration;
        config.frameGenerationCallbackUserContext = &state;
        config.frameGenerationEnabled = generate;
        config.allowAsyncWorkloads = false;
        config.HUDLessColor = {};
        if (generate && target.hudless) {
            config.HUDLessColor = ImageResource({target.hudless, target.hudless_format,
                                                 VK_IMAGE_ASPECT_COLOR_BIT},
                                                target.hudless_extent,
                                                FFX_API_RESOURCE_USAGE_READ_ONLY);
            static bool logged = false;
            if (!std::exchange(logged, true)) {
                LOG_INFO(Render_Vulkan, "AMD frame generation: the HUD is told apart with a "
                                        "hudless frame");
            }
        }
        const char debug = DebugMode();
        config.flags = debug == '1'   ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES
                       : debug == '2' ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW
                                      : 0u;
        config.onlyPresentGenerated = false;
        config.generationRect = {target.rect.offset.x, target.rect.offset.y,
                                 static_cast<int32_t>(target.rect.extent.width),
                                 static_cast<int32_t>(target.rect.extent.height)};
        config.frameID = generate ? target.frame_id : state.configured_frame_id;
        const ffxReturnCode_t result = state.api->configure(&state.frame_gen_context,
                                                            &config.header);
        if (result != FFX_API_RETURN_OK) {
            LOG_ERROR(Render_Vulkan, "AMD frame generation: configure failed ({}), generation "
                                     "off",
                      result);
            RetireFrameGenContext(state);
        } else {
            state.configured_frame_id = config.frameID;
            if (generate) {
                generated_frames.fetch_add(1, std::memory_order_relaxed);
            }
            if (generate != state.generating) {
                state.generating = generate;
                LOG_INFO(Render_Vulkan, "AMD frame generation: {} (FSR frame generation {})",
                         generate ? "generating frames" : "paused, no game motion data",
                         state.version);
            }
        }
    }
    return state.functions.pOutQueuePresentKHR(queue, &info);
}

void Prepare(Scheduler& scheduler, const PrepareInputs& in) {
    if (!Requested()) {
        return;
    }
    // Without the lock: AMD's present holds it while it paces frames.
    if (!prepare_ready.load(std::memory_order_acquire)) {
        return;
    }
    static std::optional<std::chrono::steady_clock::time_point> last;
    const auto now = std::chrono::steady_clock::now();
    const f32 frame_time_ms =
        last ? std::chrono::duration<f32, std::milli>(now - *last).count() : 1000.0f / 30.0f;
    last = now;
    const Camera camera = ExtractCamera(in.view_projection);
    scheduler.RequestOutsideRenderPassOperationContext();
    last_prepare_tick.store(scheduler.CurrentTick(), std::memory_order_release);
    scheduler.Record([in, camera, frame_time_ms](vk::CommandBuffer cmdbuf) {
        State& state = GetState();
        std::scoped_lock lock{state.mutex};
        if (!state.frame_gen_context || state.prepare_failed || state.suspended) {
            return;
        }
        constexpr VkAccessFlags WRITES =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array to_read{
            Barrier(in.depth, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            Barrier(in.motion_vectors, WRITES, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, to_read);

        const u64 frame_id = state.next_frame_id++;
        ffxDispatchDescFrameGenerationPrepareV2 prepare{};
        prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
        prepare.frameID = frame_id;
        prepare.flags = 0;
        prepare.commandList = reinterpret_cast<void*>(*cmdbuf);
        prepare.renderSize = {in.render_size.width, in.render_size.height};
        prepare.jitterOffset = {0.0f, 0.0f};
        prepare.motionVectorScale = {1.0f, 1.0f}; // already in render pixels
        prepare.frameTimeDelta = frame_time_ms;
        prepare.reset = in.reset;
        prepare.cameraNear = camera.near_plane;
        prepare.cameraFar = camera.far_plane;
        prepare.cameraFovAngleVertical = camera.fov;
        prepare.viewSpaceToMetersFactor = 1.0f;
        prepare.depth = ImageResource(in.depth, in.render_size,
                                      FFX_API_RESOURCE_USAGE_READ_ONLY |
                                          (in.depth.aspect & VK_IMAGE_ASPECT_DEPTH_BIT
                                               ? FFX_API_RESOURCE_USAGE_DEPTHTARGET
                                               : 0u));
        prepare.motionVectors =
            ImageResource(in.motion_vectors, in.render_size, FFX_API_RESOURCE_USAGE_READ_ONLY);
        std::copy(camera.position.begin(), camera.position.end(), prepare.cameraPosition);
        std::copy(camera.up.begin(), camera.up.end(), prepare.cameraUp);
        std::copy(camera.right.begin(), camera.right.end(), prepare.cameraRight);
        std::copy(camera.forward.begin(), camera.forward.end(), prepare.cameraForward);
        const ffxReturnCode_t result =
            state.api->dispatch(&state.frame_gen_context, &prepare.header);

        const std::array to_general{
            Barrier(in.depth, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | WRITES,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            Barrier(in.motion_vectors, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | WRITES, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_IMAGE_LAYOUT_GENERAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, to_general);
        if (result != FFX_API_RETURN_OK) {
            state.prepare_failed = true;
            PublishPrepareReady(state);
            LOG_ERROR(Render_Vulkan, "AMD frame generation: prepare failed ({}), generation off",
                      result);
            return;
        }
        static bool logged = false;
        if (!std::exchange(logged, true)) {
            LOG_INFO(Render_Vulkan,
                     "AMD frame generation: game motion {}x{}, camera near {} far {} fov {:.1f} deg",
                     in.render_size.width, in.render_size.height, camera.near_plane,
                     camera.far_plane, camera.fov * 57.2958f);
        }
        prepared_frame.store(frame_id);
    });
}

bool SwapchainActive() {
    return swapchain_active.load(std::memory_order_acquire);
}

u64 LastPrepareTick() {
    return last_prepare_tick.load(std::memory_order_acquire);
}

u64 TakePreparedFrame() {
    return prepared_frame.exchange(0);
}

#else // _WIN32

VkSwapchainKHR CreateSwapchain(const Device&, std::mutex&, const VkSwapchainCreateInfoKHR&) {
    return VK_NULL_HANDLE;
}
bool CanPresent(VkSurfaceFormatKHR) {
    return false;
}
OffReason LastOffReason() {
    return OffReason::None;
}
HdrMetadataResult SetHdrMetadata(VkSwapchainKHR, const VkHdrMetadataEXT&) {
    return HdrMetadataResult::NotSent;
}
void DestroySwapchain(const Device&, Scheduler*, VkSwapchainKHR) {}
void WaitForPresents() {}
void Shutdown() {}
std::vector<VkImage> GetImages(const Device&, VkSwapchainKHR) {
    return {};
}
VkResult AcquireNextImage(const Device&, VkSwapchainKHR, VkSemaphore, u32*) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VkResult Present(VkQueue, const VkPresentInfoKHR&, const PresentTarget&) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
void Prepare(Scheduler&, const PrepareInputs&) {}
u64 TakePreparedFrame() {
    return 0;
}
bool SwapchainActive() {
    return false;
}
u64 LastPrepareTick() {
    return 0;
}

#endif // _WIN32

} // namespace Vulkan::AmdFrameGen
