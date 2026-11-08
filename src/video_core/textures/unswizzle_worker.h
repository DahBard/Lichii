// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>

#include "common/common_types.h"
#include "common/scratch_buffer.h"
#include "common/thread_worker.h"

namespace Tegra::Texture {

inline constexpr size_t ParallelUnswizzleMinBytes = 2 * 1024 * 1024;

class UnswizzleWorker {
public:
    bool TryUnswizzle(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                      u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                      u32 stride_alignment, bool native_tiling, bool verify,
                      Common::ThreadWorker* workers = nullptr);

private:
    Common::ScratchBuffer<u8> owned;
};

/// Untiles a 2D level of ParallelUnswizzleMinBytes or more in bands of block rows shared by idle
/// workers and the caller (synchronous: done when it returns true). From Fritchii v4 (Frizti/
/// Lichii 7d75f9133): BC5 4096x4096 1.07 -> 0.28 ms there. False when the level doesn't qualify.
bool TryUnswizzleBands(std::span<u8> output, std::span<const u8> input, u32 bytes_per_pixel,
                       u32 width, u32 height, u32 depth, u32 block_height, u32 block_depth,
                       u32 stride_alignment, bool native_tiling, bool verify,
                       Common::ThreadWorker* workers = nullptr);

} // namespace Tegra::Texture
