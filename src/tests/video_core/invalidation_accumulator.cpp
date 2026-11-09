// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <utility>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "video_core/invalidation_accumulator.h"

namespace {

std::vector<std::pair<GPUVAddr, size_t>> Ranges(VideoCommon::InvalidationAccumulator& accumulator) {
    std::vector<std::pair<GPUVAddr, size_t>> ranges;
    (void)accumulator.InvalidateAll(
        [&ranges](GPUVAddr address, size_t size) { ranges.emplace_back(address, size); });
    return ranges;
}

} // namespace

TEST_CASE("A word past its block's start invalidates the whole block", "[invalidation_accumulator]") {
    VideoCommon::InvalidationAccumulator accumulator{true};
    accumulator.Add(0x1054, 4);
    const auto ranges = Ranges(accumulator);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].first == 0x1040);
    CHECK(ranges[0].second == 0x20);
}

TEST_CASE("Words written from mid-block on are all invalidated", "[invalidation_accumulator]") {
    VideoCommon::InvalidationAccumulator accumulator{true};
    for (GPUVAddr address = 0x2054; address < 0x2084; address += 4) {
        accumulator.Add(address, 4);
    }
    const auto ranges = Ranges(accumulator);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].first == 0x2040);
    CHECK(ranges[0].second == 0x60);
}

TEST_CASE("A write spanning blocks covers each block it touches", "[invalidation_accumulator]") {
    VideoCommon::InvalidationAccumulator accumulator{true};
    accumulator.Add(0x3018, 0x30);
    const auto ranges = Ranges(accumulator);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].first == 0x3000);
    CHECK(ranges[0].second == 0x60);
}

TEST_CASE("A range is the bytes written", "[invalidation_accumulator]") {
    VideoCommon::InvalidationAccumulator accumulator{false};
    accumulator.Add(0x1054, 4);
    // A constant buffer vec4 written word by word, as WriteWordsCached adds it.
    for (GPUVAddr address = 0x3030; address < 0x3040; address += 4) {
        accumulator.Add(address, 4);
    }
    // Inside the range already: nothing new.
    accumulator.Add(0x3034, 4);
    const auto ranges = Ranges(accumulator);
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].first == 0x1054);
    CHECK(ranges[0].second == 4);
    CHECK(ranges[1].first == 0x3030);
    CHECK(ranges[1].second == 0x10);
}

TEST_CASE("Words written from mid-block on merge into one range", "[invalidation_accumulator]") {
    VideoCommon::InvalidationAccumulator accumulator{false};
    for (GPUVAddr address = 0x2054; address < 0x2084; address += 4) {
        accumulator.Add(address, 4);
    }
    const auto ranges = Ranges(accumulator);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].first == 0x2054);
    CHECK(ranges[0].second == 0x30);
}
