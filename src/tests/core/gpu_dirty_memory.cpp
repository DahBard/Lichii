// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <functional>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "core/gpu_dirty_memory_manager.h"

TEST_CASE("GPUDirtyMemoryManager reports collected writes until they are gathered",
          "[core][gpu_dirty_memory]") {
    Core::GPUDirtyMemoryManager manager;
    manager.Collect(0x10040, 0x10);
    manager.Collect(0x20000, 0x40);

    REQUIRE(manager.Overlaps(0x10040, 1));
    REQUIRE(manager.Overlaps(0x10000, 0x1000));
    REQUIRE(manager.Overlaps(0x20000, 4));
    REQUIRE_FALSE(manager.Overlaps(0x10000, 0x40));
    REQUIRE_FALSE(manager.Overlaps(0x10080, 0x40));
    REQUIRE_FALSE(manager.Overlaps(0x30000, 0x1000));

    std::vector<std::pair<PAddr, size_t>> gathered;
    std::function<void(PAddr, size_t)> callback = [&](PAddr address, size_t size) {
        gathered.emplace_back(address, size);
    };
    manager.Gather(callback);

    REQUIRE(gathered.size() == 2);
    REQUIRE_FALSE(manager.Overlaps(0x10000, 0x1000));
    REQUIRE_FALSE(manager.Overlaps(0x20000, 4));
}

TEST_CASE("GPUDirtyMemoryManager collects writes across its 2 KiB pages whole",
          "[core][gpu_dirty_memory]") {
    Core::GPUDirtyMemoryManager manager;
    manager.Collect(0x107f0, 0x20);   // 16 bytes on each side of 0x10800
    manager.Collect(0x20000, 0x1000); // a whole 4 KiB page
    manager.Collect(0x30100, 0x900);

    std::vector<std::pair<PAddr, size_t>> gathered;
    std::function<void(PAddr, size_t)> callback = [&](PAddr address, size_t size) {
        gathered.emplace_back(address, size);
    };
    manager.Gather(callback);

    const auto covered = [&](PAddr begin, PAddr end) {
        for (PAddr address = begin; address < end; ++address) {
            bool found = false;
            for (const auto& [start, size] : gathered) {
                found = found || (address >= start && address < start + size);
            }
            if (!found) {
                return false;
            }
        }
        return true;
    };
    REQUIRE(covered(0x107f0, 0x10810));
    REQUIRE(covered(0x20000, 0x21000));
    REQUIRE(covered(0x30100, 0x30a00));
}
