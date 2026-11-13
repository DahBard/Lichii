// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <limits>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/present/hdr_convert.h"
#include "video_core/renderer_vulkan/vk_amd_frame_gen.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

using Settings::HdrSwapchain;
using Vulkan::ChooseHdrSurfaceFormat;

namespace {

constexpr VkSurfaceFormatKHR Sdr{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
constexpr VkSurfaceFormatKHR Hdr10{VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                   VK_COLOR_SPACE_HDR10_ST2084_EXT};
constexpr VkSurfaceFormatKHR Scrgb{VK_FORMAT_R16G16B16A16_SFLOAT,
                                   VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};

bool Same(const std::optional<VkSurfaceFormatKHR>& a, const VkSurfaceFormatKHR& b) {
    return a && a->format == b.format && a->colorSpace == b.colorSpace;
}

} // Anonymous namespace

TEST_CASE("HDR output takes the preferred pair the surface lists", "[hdr_output]") {
    const std::array both{Sdr, Scrgb, Hdr10};
    REQUIRE(Same(ChooseHdrSurfaceFormat(both, HdrSwapchain::Hdr10), Hdr10));
    REQUIRE(Same(ChooseHdrSurfaceFormat(both, HdrSwapchain::Scrgb), Scrgb));
}

TEST_CASE("HDR output falls back to the other listed pair", "[hdr_output]") {
    const std::array scrgb_only{Sdr, Scrgb};
    REQUIRE(Same(ChooseHdrSurfaceFormat(scrgb_only, HdrSwapchain::Hdr10), Scrgb));
    const std::array hdr10_only{Sdr, Hdr10};
    REQUIRE(Same(ChooseHdrSurfaceFormat(hdr10_only, HdrSwapchain::Scrgb), Hdr10));
}

TEST_CASE("HDR output needs the exact format and colour space pair", "[hdr_output]") {
    // A 10-bit SDR format or an HDR10 colour space on another format is no HDR10 pair.
    const std::array mismatched{
        Sdr,
        VkSurfaceFormatKHR{VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_HDR10_ST2084_EXT},
    };
    REQUIRE_FALSE(ChooseHdrSurfaceFormat(mismatched, HdrSwapchain::Hdr10));
    REQUIRE_FALSE(ChooseHdrSurfaceFormat(std::array{Sdr}, HdrSwapchain::Scrgb));
}

TEST_CASE("Layers are converted only where their encoding differs from the frame's",
          "[hdr_output]") {
    using Vulkan::HdrEncoding;
    using Vulkan::HdrPresent;
    Tegra::FramebufferConfig layer{};
    const HdrPresent sdr{};
    const HdrPresent hdr10{.color_space = VK_COLOR_SPACE_HDR10_ST2084_EXT};
    const HdrPresent scrgb{.color_space = VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT};

    layer.color_space = Tegra::ColorSpace::RGB709;
    REQUIRE_FALSE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, sdr)));
    const auto sdr_on_hdr10 = Vulkan::MakeHdrConvertParams(layer, hdr10);
    REQUIRE(Vulkan::NeedsHdrConvert(sdr_on_hdr10));
    REQUIRE(sdr_on_hdr10.input == HdrEncoding::Srgb);
    REQUIRE(sdr_on_hdr10.output == HdrEncoding::Pq);

    layer.color_space = Tegra::ColorSpace::RGB2020_PQ;
    REQUIRE_FALSE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, hdr10)));
    REQUIRE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, sdr)));
    REQUIRE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, scrgb)));

    layer.color_space = Tegra::ColorSpace::RGB709_Linear;
    layer.linear_unit_luminance = 0; // unknown: the scRGB 80 nits
    REQUIRE_FALSE(Vulkan::NeedsHdrConvert(Vulkan::MakeHdrConvertParams(layer, scrgb)));
    layer.linear_unit_luminance = 100;
    const auto rescaled = Vulkan::MakeHdrConvertParams(layer, scrgb);
    REQUIRE(Vulkan::NeedsHdrConvert(rescaled));
    REQUIRE(rescaled.nits_per_unit == 100.0f);
}

TEST_CASE("AMD frame generation presents SDR and HDR10 only", "[hdr_output]") {
    using Vulkan::AmdFrameGen::Output;
    using Vulkan::AmdFrameGen::OutputOf;
    // Every SDR swapchain, whatever its format, as before HDR output.
    REQUIRE(OutputOf(Sdr) == Output::Sdr);
    REQUIRE(OutputOf({VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}) ==
            Output::Sdr);
    REQUIRE(OutputOf(Hdr10) == Output::Hdr10);
    REQUIRE(OutputOf(Scrgb) == Output::Unsupported);
    // HDR10 only as A2B10G10R10: AMD's runtime has no format for A2R10G10B10.
    REQUIRE(OutputOf({VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_HDR10_ST2084_EXT}) ==
            Output::Unsupported);
    REQUIRE(OutputOf({VK_FORMAT_R16G16B16A16_SFLOAT, VK_COLOR_SPACE_HDR10_ST2084_EXT}) ==
            Output::Unsupported);
    REQUIRE(OutputOf({VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_DISPLAY_NATIVE_AMD}) ==
            Output::Unsupported);
}

TEST_CASE("AMD frame generation decodes HDR10 frames as PQ up to a peak", "[hdr_output]") {
    using Vulkan::AmdFrameGen::CompleteHdr10Decode;
    using Vulkan::AmdFrameGen::HdrDecode;
    constexpr u32 srgb = 0;
    constexpr u32 pq = 1;
    constexpr f32 told_peak = 1400.0f;
    // What AMD's swapchain passes once the HDR10 metadata reached it: kept, its peak included.
    const HdrDecode complete{pq, 0.0f, 1400.0f};
    REQUIRE(CompleteHdr10Decode(complete, told_peak) == complete);
    const HdrDecode other_peak{pq, 0.0f, 1000.0f};
    REQUIRE(CompleteHdr10Decode(other_peak, told_peak) == other_peak);
    // Before any metadata the range is 0 nits, which generation divides by.
    REQUIRE(CompleteHdr10Decode({pq, 0.0f, 0.0f}, told_peak) == HdrDecode{pq, 0.0f, told_peak});
    const HdrDecode nan{pq, 0.0f, std::numeric_limits<f32>::quiet_NaN()};
    REQUIRE(CompleteHdr10Decode(nan, told_peak) == HdrDecode{pq, 0.0f, told_peak});
    // A swapchain that took the colour space for SDR.
    REQUIRE(CompleteHdr10Decode({srgb, 0.0f, 0.0f}, told_peak) == HdrDecode{pq, 0.0f, told_peak});
    REQUIRE(CompleteHdr10Decode({srgb, 0.0f, 600.0f}, told_peak) == HdrDecode{pq, 0.0f, 600.0f});
}
