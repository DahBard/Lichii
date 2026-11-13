// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdlib>
#include <utility>
#include <vector>

#include "common/common_types.h"

namespace VideoCommon {

class InvalidationAccumulator {
public:
    /// `whole_blocks_`: a range runs from the start of the first 32-byte block a write touches
    /// to the end of the last (upstream #700), otherwise it is the written bytes (see Add).
    explicit InvalidationAccumulator(bool whole_blocks_ = WholeBlocksSwitch()) noexcept
        : whole_blocks{whole_blocks_} {}
    ~InvalidationAccumulator() = default;

    void Add(GPUVAddr address, size_t size) noexcept {
        auto const end_address = start_address + accumulated_size;
        if (!(address >= start_address && address + size <= end_address)) {
            // A range is the bytes the write wrote. #700's whole blocks reach bytes the guest
            // didn't write, and where the GPU wrote those and guest memory doesn't hold them yet
            // (preflush, EDEN_NX2_EXP35's collected writes), the invalidation loaded stale guest
            // bytes over them (Fritchii v5.1 c2d2f50d1: stale vertices in the graveyard). That
            // revert took the size before aligning the address down again, as before #700, which
            // leaves out the written bytes of a write starting past its block's start (a lone word
            // at 0x54 gave [0x40, 0x4c)). Nothing that takes these ranges needs them aligned, and
            // consecutive writes still merge (each starts at the range's end).
            if (whole_blocks) {
                const GPUVAddr first_block = address & atomicity_mask;
                size = ((address + size + atomicity_size_mask) & atomicity_mask) - first_block;
                address = first_block;
            }
            if (start_address == 0) {
                start_address = address;
                accumulated_size = size;
                // will now have collected as this point
            } else if (address != end_address) {
                buffer.emplace_back(start_address, accumulated_size);
                start_address = address;
                accumulated_size = size;
            } else {
                accumulated_size += size;
            }
        }
    }

    template <typename F>
    [[nodiscard]] bool InvalidateAll(F&& f) noexcept {
        if (start_address > 0) {
            for (auto [address, size] : buffer)
                f(address, size);
            f(start_address, accumulated_size);
            buffer.clear();
            start_address = 0;
            accumulated_size = 0;
            return true;
        }
        return false;
    }

private:
    /// EDEN_NX2_INVALIDATE_BLOCKS=1: upstream #700's whole blocks.
    [[nodiscard]] static bool WholeBlocksSwitch() noexcept {
        const char* value = std::getenv("EDEN_NX2_INVALIDATE_BLOCKS");
        return value != nullptr && value[0] == '1';
    }

    static constexpr size_t atomicity_bits = 5;
    static constexpr size_t atomicity_size = 1ULL << atomicity_bits;
    static constexpr size_t atomicity_size_mask = atomicity_size - 1;
    static constexpr size_t atomicity_mask = ~atomicity_size_mask;
    std::vector<std::pair<VAddr, size_t>> buffer;
    GPUVAddr start_address = 0;
    size_t accumulated_size = 0;
    bool whole_blocks;
};

} // namespace VideoCommon
