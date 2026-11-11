// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <vector>

#include "common/common_types.h"

namespace Common::Windows {

/// What a thread does, for placing it on a hybrid CPU (Intel Alder Lake and later).
enum class CoreRole {
    Any,         ///< The process default: every core but a low-power island.
    Fastest,     ///< Threads that bound the frame time (GPU command threads): P-cores.
    Performance, ///< Busy, latency-sensitive threads (guest cores): P- and E-cores.
    Waiting,     ///< Threads that wait without sleeping (core timing): E-cores.
    Background,  ///< Low-priority throughput work (pipeline builds): any core but the island.
};

/// Restricts the calling thread to the CPU sets of its role. Does nothing on CPUs with a single
/// core type or with LICHII_CPU_POLICY=off.
void SetCurrentThreadCoreRole(CoreRole role, const char* thread_name);

/// The core classes core placement tells apart (whatever LICHII_CPU_POLICY says).
enum class CoreClass : u8 {
    Uniform,     ///< A CPU with one core type
    Performance, ///< The highest efficiency class: P-cores
    Efficient,   ///< Slower cores on the P-cores' last-level cache: E-cores
    LowPower,    ///< Slower cores off it: a low-power island (Meteor/Arrow Lake-H SoC-tile E-cores)
};

struct ClassifiedProcessor {
    u16 group;
    u8 number; ///< In the group, as GetCurrentProcessorNumberEx and PDH ("group,number") give it
    CoreClass core_class;
    u8 efficiency_class; ///< The CPU set's EfficiencyClass (higher is faster)
    u32 cpu_set_id;      ///< For SetThreadSelectedCpuSets
};

/// Every logical processor with its class; empty without CPU set information.
const std::vector<ClassifiedProcessor>& ClassifiedProcessors();

} // namespace Common::Windows
