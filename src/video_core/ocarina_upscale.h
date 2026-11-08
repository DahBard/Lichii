// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

#include "common/common_types.h"

namespace VideoCommon {

enum class OcarinaUpscaleBackend { Native, Fsr, Dlss };

constexpr OcarinaUpscaleBackend SelectOcarinaUpscaler(bool fsr_enabled, bool dlss_enabled,
                                                    bool dlss_available,
                                                    std::string_view override) {
    if (override == "native") {
        return OcarinaUpscaleBackend::Native;
    }
    if (override == "fsr") {
        return OcarinaUpscaleBackend::Fsr;
    }
    if (!fsr_enabled && !dlss_enabled && override != "dlss") {
        return OcarinaUpscaleBackend::Native;
    }
    return dlss_enabled && dlss_available ? OcarinaUpscaleBackend::Dlss
                                          : OcarinaUpscaleBackend::Fsr;
}

/// Ocarina's scene camera: vertex (or mesh) constant buffer 3, its projection's rows at 0x70.
constexpr u32 OcarinaCameraBuffer = 3;
constexpr u32 OcarinaCameraProjectionOffset = 0x70;
constexpr u32 OcarinaCameraBytes = 0xb0;

/// Whether a projection (rows) is a GL-style perspective one, as the scene camera's
/// (zortos293/Lichii#253).
[[nodiscard]] inline bool IsOcarinaPerspective(const std::array<f32, 16>& projection) {
    for (const f32 value : projection) {
        if (!std::isfinite(value)) {
            return false;
        }
    }
    return projection[0] > 0.0f && projection[5] > 0.0f && projection[14] == -1.0f &&
           projection[15] == 0.0f && projection[12] == 0.0f && projection[13] == 0.0f;
}

[[nodiscard]] constexpr f32 Halton(u32 index, u32 base) {
    f32 result = 0.0f;
    f32 fraction = 1.0f;
    while (index > 0) {
        fraction /= static_cast<f32>(base);
        result += fraction * static_cast<f32>(index % base);
        index /= base;
    }
    return result;
}

/// FSR's jitter cycle for a render to output width (ffxFsr3UpscalerGetJitterPhaseCount): 8 at
/// the same size, 18 for 1440p to 4K.
[[nodiscard]] inline u32 FsrJitterPhaseCount(u32 render_width, u32 output_width) {
    if (render_width == 0) {
        return 8;
    }
    const f32 ratio = static_cast<f32>(output_width) / static_cast<f32>(render_width);
    return static_cast<u32>((std::max)(1.0f, 8.0f * ratio * ratio));
}

/// The sample offset of jittered frame `index` in render pixels, -0.5 to 0.5, Halton bases 2 and
/// 3 (ffxFsr3UpscalerGetJitterOffset). Moving the viewport by it moves the image by it, which is
/// FSR's convention: input texel k samples k + 0.5 - jitter.
[[nodiscard]] inline std::array<f32, 2> FsrJitter(u32 index, u32 phases) {
    const u32 position = index % (std::max)(phases, 1U) + 1;
    return {Halton(position, 2) - 0.5f, Halton(position, 3) - 0.5f};
}

} // namespace VideoCommon
