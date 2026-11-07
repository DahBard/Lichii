// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <optional>
#include <utility>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/oot_network.h"

namespace VideoCommon {

/// Occlusion reports are written at once with this many samples passed and no host occlusion
/// query runs. Ocarina of Time reports ~1,400 a frame and reads them back on the CPU; on an
/// RX 9070 XT the queries, their copies and the read stalls cost ~35 ms of a 48 ms frame, and
/// answering them (4096 samples, everything visible) doubled its frame rate with no visible
/// change. On by default for Ocarina; LICHII_FAKE_OCCLUSION=<samples> sets it for any title and
/// LICHII_FAKE_OCCLUSION=0 turns it off.
inline u32 FakeOcclusionSamples() {
    static const std::optional<u32> forced = []() -> std::optional<u32> {
        const char* const value = std::getenv("LICHII_FAKE_OCCLUSION");
        if (value == nullptr) {
            return std::nullopt;
        }
        return static_cast<u32>(std::strtoul(value, nullptr, 10));
    }();
    const u32 samples = forced ? *forced
                               : (Settings::GetCurrentProgramID() == OotNetwork::ProgramId ? 4096U
                                                                                          : 0U);
    if (static bool logged = false; samples != 0 && !std::exchange(logged, true)) {
        LOG_INFO(HW_GPU, "Occlusion reports read {} samples without host queries", samples);
    }
    return samples;
}

} // namespace VideoCommon
