// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cmath>
#include <limits>

#include <catch2/catch_test_macros.hpp>
#include "video_core/ocarina_upscale.h"
#include "video_core/fsr.h"

TEST_CASE("OoT temporal backend selection is opt-in and vendor neutral", "[video_core][ocarina]") {
    using enum VideoCommon::OcarinaUpscaleBackend;
    using VideoCommon::SelectOcarinaUpscaler;
    CHECK(SelectOcarinaUpscaler(false, false, false, "") == Native);
    CHECK(SelectOcarinaUpscaler(false, false, true, "") == Native);
    CHECK(SelectOcarinaUpscaler(true, false, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(true, false, true, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, false, "") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, true, "") == Dlss);
    CHECK(SelectOcarinaUpscaler(true, true, true, "native") == Native);
    CHECK(SelectOcarinaUpscaler(false, true, true, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, "fsr") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, false, "dlss") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, false, true, "dlss") == Fsr);
    CHECK(SelectOcarinaUpscaler(false, true, true, "dlss") == Dlss);
    CHECK(SelectOcarinaUpscaler(false, false, false, "garbage") == Native);
}

TEST_CASE("FSR temporal sharpness uses the existing stops slider", "[video_core][fsr][ocarina]") {
    CHECK(FSR::RcasSharpnessFromStops(0.0f) == 1.0f);
    CHECK(FSR::RcasSharpnessFromStops(0.81f) == 0.595f);
    CHECK(FSR::RcasSharpnessFromStops(1.0f) == 0.5f);
    CHECK(FSR::RcasSharpnessFromStops(2.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(3.0f) == 0.0f);
    CHECK(FSR::RcasSharpnessFromStops(-1.0f) == 1.0f);
}

TEST_CASE("Ocarina FSR jitter follows FSR's phase count and Halton offsets", "[video_core][ocarina]") {
    using VideoCommon::FsrJitter;
    using VideoCommon::FsrJitterPhaseCount;
    CHECK(FsrJitterPhaseCount(2560, 3840) == 18);
    CHECK(FsrJitterPhaseCount(1920, 1920) == 8);
    CHECK(FsrJitterPhaseCount(1920, 2560) == 14);
    CHECK(FsrJitterPhaseCount(0, 3840) == 8);
    // Halton (2, 3) from index 1, centred on the pixel.
    const auto first = FsrJitter(0, 18);
    CHECK(first[0] == 0.0f);
    CHECK(std::abs(first[1] - (1.0f / 3.0f - 0.5f)) < 1e-6f);
    const auto second = FsrJitter(1, 18);
    CHECK(second[0] == -0.25f);
    CHECK(std::abs(second[1] - (2.0f / 3.0f - 0.5f)) < 1e-6f);
    for (u32 index = 0; index < 64; ++index) {
        const auto offset = FsrJitter(index, 18);
        CHECK(offset[0] >= -0.5f);
        CHECK(offset[0] < 0.5f);
        CHECK(offset[1] >= -0.5f);
        CHECK(offset[1] < 0.5f);
        CHECK(FsrJitter(index + 18, 18) == offset);
    }
}

TEST_CASE("Ocarina scene camera is told from flat passes by its projection", "[video_core][ocarina]") {
    // Rows of a GL perspective projection (1.0 vertical FOV, 16:9) and of an orthographic one.
    const std::array<f32, 16> perspective{1.0f, 0.0f, 0.0f, 0.0f,  0.0f, 1.78f, 0.0f, 0.0f,
                                          0.0f, 0.0f, -1.0f, -0.2f, 0.0f, 0.0f, -1.0f, 0.0f};
    std::array<f32, 16> orthographic{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                     0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    CHECK(VideoCommon::IsOcarinaPerspective(perspective));
    CHECK(!VideoCommon::IsOcarinaPerspective(orthographic));
    orthographic = perspective;
    orthographic[3] = std::numeric_limits<f32>::quiet_NaN();
    CHECK(!VideoCommon::IsOcarinaPerspective(orthographic));
}
