// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_fsr4_upscaler.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

#include <ffx_api.h>
#include <ffx_upscale.h>
#include <vk/ffx_api_vk.h>

namespace Vulkan {

struct Fsr4Upscaler::Api {
    PfnFfxCreateContext create{};
    PfnFfxDestroyContext destroy{};
    PfnFfxQuery query{};
    PfnFfxDispatch dispatch{};
};

namespace {

constexpr VkImageSubresourceRange COLOR_RANGE{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

std::string_view ReturnCodeName(ffxReturnCode_t code) {
    switch (code) {
    case FFX_API_RETURN_OK:
        return "ok";
    case FFX_API_RETURN_ERROR:
        return "error";
    case FFX_API_RETURN_ERROR_UNKNOWN_DESCTYPE:
        return "unknown description type";
    case FFX_API_RETURN_ERROR_RUNTIME_ERROR:
        return "runtime error";
    case FFX_API_RETURN_NO_PROVIDER:
        return "no provider";
    case FFX_API_RETURN_ERROR_MEMORY:
        return "out of memory";
    case FFX_API_RETURN_ERROR_PARAMETER:
        return "invalid parameter";
    case FFX_API_RETURN_PROVIDER_NO_SUPPORT_NEW_DESCTYPE:
        return "description type too new for the provider";
    default:
        return "unknown error";
    }
}

/// The API's messages, at most 50 and then every 500th, with their wide characters narrowed.
void LogMessage(uint32_t type, const wchar_t* message) {
    static std::atomic<u32> count{};
    const u32 index = count.fetch_add(1, std::memory_order_relaxed);
    if (message == nullptr || (index >= 50 && index % 500 != 0)) {
        return;
    }
    std::string text;
    for (const wchar_t* c = message; *c != 0 && text.size() < 1024; ++c) {
        text.push_back(*c >= 0x20 && *c < 0x7f ? static_cast<char>(*c) : '?');
    }
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        LOG_ERROR(Render_Vulkan, "AMD FSR API: {}", text);
    } else {
        LOG_WARNING(Render_Vulkan, "AMD FSR API: {}", text);
    }
}

const char* EnvValue(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : nullptr;
}

/// The API's loader from the executable's folder, loaded once; null when it is missing.
const Fsr4Upscaler::Api* LoadApi() {
#ifdef _WIN32
    static const std::optional<Fsr4Upscaler::Api> api = []() -> std::optional<Fsr4Upscaler::Api> {
        const auto path = Common::FS::GetExeDirectory() / "amd_fidelityfx_loader_vk.dll";
        // The loader then loads amd_fidelityfx_upscaler_vk.dll from the same folder.
        HMODULE module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (module == nullptr) {
            LOG_WARNING(Render_Vulkan,
                        "AMD FSR 4: could not load {} (error {}); using FSR 3.1",
                        Common::FS::PathToUTF8String(path), GetLastError());
            return std::nullopt;
        }
        Fsr4Upscaler::Api result{};
        const auto resolve = [module](const char* name, auto& function) {
            function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(
                reinterpret_cast<void*>(GetProcAddress(module, name)));
            return function != nullptr;
        };
        if (!resolve("ffxCreateContext", result.create) ||
            !resolve("ffxDestroyContext", result.destroy) || !resolve("ffxQuery", result.query) ||
            !resolve("ffxDispatch", result.dispatch)) {
            LOG_WARNING(Render_Vulkan, "AMD FSR 4: {} lacks the API's functions; using FSR 3.1",
                        Common::FS::PathToUTF8String(path));
            FreeLibrary(module);
            return std::nullopt;
        }
        // Kept loaded for the rest of the run.
        return result;
    }();
    return api ? &*api : nullptr;
#else
    return nullptr;
#endif
}

/// "4.1.2" -> {4, 1, 2}; missing parts read as 0.
std::array<u32, 3> ParseVersion(std::string_view name) {
    std::array<u32, 3> parts{};
    size_t part = 0;
    for (const char c : name) {
        if (c == '.') {
            if (++part == parts.size()) {
                break;
            }
        } else if (c >= '0' && c <= '9') {
            parts[part] = parts[part] * 10 + static_cast<u32>(c - '0');
        } else {
            break;
        }
    }
    return parts;
}

FfxApiResource Resource(VkImage image, VkFormat format, VkExtent2D extent, uint32_t usage) {
    FfxApiResource resource{};
    if (image == VK_NULL_HANDLE) {
        return resource;
    }
    resource.resource = reinterpret_cast<void*>(image);
    resource.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    resource.description.format = ffxApiGetSurfaceFormatVK(format);
    resource.description.width = extent.width;
    resource.description.height = extent.height;
    resource.description.depth = 1;
    resource.description.mipCount = 1;
    resource.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    resource.description.usage = usage;
    // Every image here stays in the GENERAL layout; the API returns them to it after a dispatch.
    resource.state = FFX_API_RESOURCE_STATE_UNORDERED_ACCESS;
    return resource;
}

} // Anonymous namespace

bool Fsr4Upscaler::Wanted() {
#ifdef _WIN32
    const char* value = EnvValue("LICHII_FSR4");
    return value == nullptr || value[0] != '0';
#else
    return false;
#endif
}

std::unique_ptr<Fsr4Upscaler> Fsr4Upscaler::Create(const Device& device, Scheduler& scheduler,
                                                    MemoryAllocator& allocator,
                                                    VkExtent2D render_size,
                                                    VkExtent2D upscale_size) {
    const Api* api = LoadApi();
    if (api == nullptr) {
        return nullptr;
    }
    VkDeviceContext device_context{};
    device_context.vkDevice = *device.GetLogical();
    device_context.vkPhysicalDevice = device.GetPhysical();
    device_context.vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr;

    // The upscaler versions the API offers for this device, e.g. "4.1.2" and "3.1.5".
    ffxQueryDescGetVersions versions{};
    versions.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    versions.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    versions.device = &device_context;
    uint64_t count = 0;
    versions.outputCount = &count;
    ffxReturnCode_t result = api->query(nullptr, &versions.header);
    if (result != FFX_API_RETURN_OK || count == 0) {
        LOG_WARNING(Render_Vulkan, "AMD FSR 4: the API offers no upscaler here ({}); using FSR 3.1",
                    ReturnCodeName(result));
        return nullptr;
    }
    std::vector<uint64_t> ids(count);
    std::vector<const char*> names(count);
    versions.versionIds = ids.data();
    versions.versionNames = names.data();
    result = api->query(nullptr, &versions.header);
    if (result != FFX_API_RETURN_OK) {
        LOG_WARNING(Render_Vulkan, "AMD FSR 4: listing the upscaler versions failed ({}); using "
                                   "FSR 3.1",
                    ReturnCodeName(result));
        return nullptr;
    }
    ids.resize(count);
    names.resize(count);
    const char* const wanted = EnvValue("LICHII_FSR4_VERSION");
    std::optional<size_t> chosen;
    std::string offered;
    for (size_t i = 0; i < ids.size(); ++i) {
        const std::string_view name = names[i] != nullptr ? names[i] : "";
        offered += fmt::format("{}{}", offered.empty() ? "" : ", ", name);
        if (wanted != nullptr) {
            if (name == wanted) {
                chosen = i;
            }
        } else if (!name.empty() && name[0] == '4' &&
                   (!chosen || ParseVersion(name) > ParseVersion(names[*chosen]))) {
            chosen = i;
        }
    }
    if (!chosen) {
        LOG_WARNING(Render_Vulkan, "AMD FSR 4: no {} among the offered upscalers ({}); using "
                                   "FSR 3.1",
                    wanted != nullptr ? wanted : "FSR 4", offered);
        return nullptr;
    }
    LOG_INFO(Render_Vulkan, "AMD FSR 4: the API offers {}; using {}", offered, names[*chosen]);

    std::unique_ptr<Fsr4Upscaler> upscaler{
        new Fsr4Upscaler(device, scheduler, allocator, *api, ids[*chosen])};
    if (!upscaler->CreateContext(render_size, upscale_size)) {
        return nullptr;
    }
    return upscaler;
}

Fsr4Upscaler::Fsr4Upscaler(const Device& device_, Scheduler& scheduler_,
                           MemoryAllocator& allocator_, const Api& api_, u64 version_id_)
    : device{device_}, scheduler{scheduler_}, allocator{allocator_}, api{api_},
      version_id{version_id_} {
    point_sampler = device.GetLogical().CreateSampler(VkSamplerCreateInfo{
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_NEVER,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    });
}

Fsr4Upscaler::~Fsr4Upscaler() {
    DestroyContext();
}

bool Fsr4Upscaler::CreateContext(VkExtent2D render_size, VkExtent2D new_upscale_size) {
    const auto start = std::chrono::steady_clock::now();
    VkDeviceContext device_context{};
    device_context.vkDevice = *device.GetLogical();
    device_context.vkPhysicalDevice = device.GetPhysical();
    device_context.vkDeviceProcAddr = device.GetDispatchLoader().vkGetDeviceProcAddr;

    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.deviceContext = device_context;

    ffxOverrideVersion version_override{};
    version_override.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    version_override.versionId = version_id;

    ffxCreateContextDescUpscaleVersion api_version{};
    api_version.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    api_version.version = FFX_UPSCALER_VERSION;

    const char* const debug = EnvValue("LICHII_FSR4_DEBUG");
    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    // Ocarina's colour is tonemapped LDR with a square-root encoding, its depth finite and not
    // inverted; motion is in render pixels without jitter.
    create.flags = FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE;
    if (debug == nullptr || debug[0] != '0') {
        create.flags |= FFX_UPSCALE_ENABLE_DEBUG_CHECKING;
    }
    create.maxRenderSize = {render_size.width, render_size.height};
    create.maxUpscaleSize = {new_upscale_size.width, new_upscale_size.height};
    create.fpMessage = &LogMessage;
    create.header.pNext = &api_version.header;
    api_version.header.pNext = &backend.header;
    backend.header.pNext = &version_override.header;

    ffxContext new_context = nullptr;
    const ffxReturnCode_t result = api.create(&new_context, &create.header, nullptr);
    if (result != FFX_API_RETURN_OK || new_context == nullptr) {
        LOG_WARNING(Render_Vulkan, "AMD FSR 4: creating the upscaler failed ({}); using FSR 3.1",
                    ReturnCodeName(result));
        return false;
    }
    context = new_context;
    max_render_size = render_size;
    upscale_size = new_upscale_size;

    output = allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .extent = {upscale_size.width, upscale_size.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    });
    output_view = device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *output,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R16G16B16A16_SFLOAT,
        .subresourceRange = COLOR_RANGE,
    });
    // Unit exposure, as FSR 3.1 takes Ocarina's LDR colour.
    exposure = allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R32_SFLOAT,
        .extent = {1, 1, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    });
    images_initialized = false;

    std::string provider = "?";
    ffxQueryGetProviderVersion provider_query{};
    provider_query.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    if (api.query(&context, &provider_query.header) == FFX_API_RETURN_OK &&
        provider_query.versionName != nullptr) {
        provider = provider_query.versionName;
    }
    FfxApiEffectMemoryUsage memory{};
    ffxQueryDescUpscaleGetGPUMemoryUsage memory_query{};
    memory_query.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    memory_query.gpuMemoryUsageUpscaler = &memory;
    const bool has_memory = api.query(&context, &memory_query.header) == FFX_API_RETURN_OK;
    LOG_INFO(Render_Vulkan,
             "AMD FSR 4: upscaler {} ready for {}x{} -> {}x{} in {} ms ({} MiB of GPU memory)",
             provider, render_size.width, render_size.height, upscale_size.width,
             upscale_size.height,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count(),
             has_memory ? fmt::format("{}", memory.totalUsageInBytes >> 20) : std::string{"?"});
    return true;
}

void Fsr4Upscaler::DestroyContext() {
    if (context == nullptr) {
        return;
    }
    // The context's images and pipelines may still be in use by submitted work.
    void(device.GetLogical().WaitIdle());
    api.destroy(&context, nullptr);
    context = nullptr;
}

bool Fsr4Upscaler::Upscale(const Inputs& in) {
    if (Failed()) {
        return false;
    }
    if (in.render_size.width > max_render_size.width ||
        in.render_size.height > max_render_size.height ||
        in.upscale_size.width != upscale_size.width ||
        in.upscale_size.height != upscale_size.height) {
        // Docked and handheld render and present at other sizes.
        scheduler.Finish();
        DestroyContext();
        if (!CreateContext(in.render_size, in.upscale_size)) {
            failed.store(true, std::memory_order_relaxed);
            return false;
        }
    }
    const bool initialize = !images_initialized;
    images_initialized = true;
    scheduler.Record([this, in, initialize, out = *output,
                      exposure_image = *exposure](vk::CommandBuffer cmdbuf) {
        if (initialize) {
            const std::array barriers{
                VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = out,
                    .subresourceRange = COLOR_RANGE,
                },
                VkImageMemoryBarrier{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = exposure_image,
                    .subresourceRange = COLOR_RANGE,
                },
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT |
                                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   0, {}, {}, barriers);
            cmdbuf.ClearColorImage(exposure_image, VK_IMAGE_LAYOUT_GENERAL,
                                   VkClearColorValue{.float32 = {1.0f, 1.0f, 1.0f, 1.0f}},
                                   COLOR_RANGE);
        }
        // The inputs come from the game's draws and our compute passes.
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            VkMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                 VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                 VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            });

        const VkExtent2D render = in.render_size;
        ffxDispatchDescUpscale dispatch{};
        dispatch.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
        dispatch.commandList = reinterpret_cast<void*>(*cmdbuf);
        dispatch.color = Resource(in.color.image, in.color.format, render,
                                  FFX_API_RESOURCE_USAGE_READ_ONLY);
        dispatch.depth = Resource(in.depth.image, in.depth.format, render,
                                  FFX_API_RESOURCE_USAGE_READ_ONLY |
                                      FFX_API_RESOURCE_USAGE_DEPTHTARGET);
        dispatch.motionVectors = Resource(in.motion_vectors.image, in.motion_vectors.format,
                                          render, FFX_API_RESOURCE_USAGE_READ_ONLY);
        dispatch.exposure = Resource(exposure_image, VK_FORMAT_R32_SFLOAT, VkExtent2D{1, 1},
                                     FFX_API_RESOURCE_USAGE_READ_ONLY);
        dispatch.reactive = Resource(in.reactive.image, in.reactive.format, render,
                                     FFX_API_RESOURCE_USAGE_READ_ONLY);
        dispatch.transparencyAndComposition =
            Resource(in.transparency.image, in.transparency.format, render,
                     FFX_API_RESOURCE_USAGE_READ_ONLY);
        dispatch.output = Resource(out, VK_FORMAT_R16G16B16A16_SFLOAT, in.upscale_size,
                                   FFX_API_RESOURCE_USAGE_UAV);
        dispatch.jitterOffset = {in.jitter[0], in.jitter[1]};
        dispatch.motionVectorScale = {in.motion_vector_scale[0], in.motion_vector_scale[1]};
        dispatch.renderSize = {render.width, render.height};
        dispatch.upscaleSize = {in.upscale_size.width, in.upscale_size.height};
        dispatch.enableSharpening = in.sharpen;
        dispatch.sharpness = in.sharpness;
        dispatch.frameTimeDelta = in.frame_time_ms;
        dispatch.preExposure = 1.0f;
        dispatch.reset = in.reset;
        dispatch.cameraNear = in.camera_near;
        dispatch.cameraFar = in.camera_far;
        dispatch.cameraFovAngleVertical = in.vertical_fov;
        dispatch.viewSpaceToMetersFactor = 1.0f;
        dispatch.flags = FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
        const ffxReturnCode_t result = api.dispatch(&context, &dispatch.header);
        if (result != FFX_API_RETURN_OK && !failed.exchange(true, std::memory_order_relaxed)) {
            LOG_ERROR(Render_Vulkan, "AMD FSR 4: the upscale dispatch failed ({}); moving to FSR "
                                     "3.1",
                      ReturnCodeName(result));
        }
        // The output pass samples the result.
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
            VkMemoryBarrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            });
    });
    return true;
}

} // namespace Vulkan
