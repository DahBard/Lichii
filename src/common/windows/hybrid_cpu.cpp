// SPDX-License-Identifier: GPL-3.0-or-later

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <vector>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/windows/hybrid_cpu.h"

namespace Common::Windows {

namespace {

using GetSystemCpuSetInformationFn = BOOL(WINAPI*)(PSYSTEM_CPU_SET_INFORMATION, ULONG, PULONG,
                                                   HANDLE, ULONG);
using SetCpuSetsFn = BOOL(WINAPI*)(HANDLE, const ULONG*, ULONG);

// LICHII_CPU_POLICY:
//  off    - Windows places every thread.
//  soft   - keep every thread off a low-power island (Meteor/Arrow Lake-H SoC-tile E-cores).
//  auto   - soft, plus the GPU command threads on P-cores and core timing on E-cores (default).
//           Pipeline builds keep every other core: they run at low priority, and a draw can be
//           waiting for one.
//  strict - auto, plus the guest CPU cores on P-cores and pipeline builds on E-cores.
enum class Policy { Off, Soft, Auto, Strict };

Policy ReadPolicy() {
    const char* const text = std::getenv("LICHII_CPU_POLICY");
    const std::string_view value = text != nullptr ? text : "";
    if (value == "off" || value == "0") {
        return Policy::Off;
    }
    if (value == "soft") {
        return Policy::Soft;
    }
    if (value == "strict") {
        return Policy::Strict;
    }
    return Policy::Auto;
}

constexpr std::string_view PolicyName(Policy policy) {
    switch (policy) {
    case Policy::Off:
        return "off";
    case Policy::Soft:
        return "soft";
    case Policy::Auto:
        return "auto";
    case Policy::Strict:
        return "strict";
    }
    return "?";
}

struct Topology {
    Policy policy = Policy::Off;
    SetCpuSetsFn set_thread = nullptr;
    std::vector<ULONG> fastest;   ///< The highest efficiency class (P-cores).
    std::vector<ULONG> shared;    ///< Every core sharing a last-level cache with them.
    std::vector<ULONG> efficient; ///< The slower cores among those (E-cores).
    std::vector<ClassifiedProcessor> classes;
};

template <typename T>
T LoadKernel32(const char* name) {
    const HMODULE module = GetModuleHandleW(L"kernel32.dll");
    if (module == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(module, name)));
}

Topology Detect() {
    Topology topology;
    const auto get_info = LoadKernel32<GetSystemCpuSetInformationFn>("GetSystemCpuSetInformation");
    const auto set_thread = LoadKernel32<SetCpuSetsFn>("SetThreadSelectedCpuSets");
    const auto set_process = LoadKernel32<SetCpuSetsFn>("SetProcessDefaultCpuSets");
    if (get_info == nullptr || set_thread == nullptr) {
        return topology;
    }
    ULONG length = 0;
    get_info(nullptr, 0, &length, GetCurrentProcess(), 0);
    if (length == 0) {
        return topology;
    }
    std::vector<u64> storage((length + sizeof(u64) - 1) / sizeof(u64));
    auto* const bytes = reinterpret_cast<u8*>(storage.data());
    if (!get_info(reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(bytes), length, &length,
                  GetCurrentProcess(), 0)) {
        return topology;
    }

    struct Cpu {
        ULONG id;
        u32 cache; ///< Processor group and last-level cache index.
        BYTE efficiency;
        WORD group;
        BYTE number;
    };
    std::vector<Cpu> cpus;
    for (ULONG offset = 0; offset + sizeof(DWORD) * 2 <= length;) {
        const auto* const info = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(bytes + offset);
        if (info->Size == 0 || offset + info->Size > length) {
            break;
        }
        if (info->Type == CpuSetInformation &&
            (info->CpuSet.AllFlags & SYSTEM_CPU_SET_INFORMATION_REALTIME) == 0) {
            cpus.push_back(Cpu{
                .id = info->CpuSet.Id,
                .cache = (u32{info->CpuSet.Group} << 8) | info->CpuSet.LastLevelCacheIndex,
                .efficiency = info->CpuSet.EfficiencyClass,
                .group = info->CpuSet.Group,
                .number = info->CpuSet.LogicalProcessorIndex,
            });
        }
        offset += info->Size;
    }
    if (cpus.empty()) {
        return topology;
    }
    const auto by_class = [](const Cpu& a, const Cpu& b) { return a.efficiency < b.efficiency; };
    const BYTE top = std::ranges::max_element(cpus, by_class)->efficiency;
    const BYTE bottom = std::ranges::min_element(cpus, by_class)->efficiency;
    if (top == bottom) {
        for (const Cpu& cpu : cpus) {
            topology.classes.push_back(
                {cpu.group, cpu.number, CoreClass::Uniform, cpu.efficiency, cpu.id});
        }
        LOG_INFO(Common, "CPU: {} logical processors of one core type, no core placement",
                 cpus.size());
        return topology;
    }

    std::vector<u32> fast_caches;
    for (const Cpu& cpu : cpus) {
        if (cpu.efficiency == top && std::ranges::find(fast_caches, cpu.cache) == fast_caches.end()) {
            fast_caches.push_back(cpu.cache);
        }
    }
    const auto shares_cache = [&fast_caches](const Cpu& cpu) {
        return std::ranges::find(fast_caches, cpu.cache) != fast_caches.end();
    };
    // Three tiers (Meteor Lake, Arrow Lake-H): slower cores both on and off the P-cores' L3.
    // The ones off it (two SoC-tile E-cores at 2.5 GHz without L3) are left out entirely. With
    // two tiers (Alder/Raptor Lake, Lunar Lake) every slower core is an E-core.
    const bool island =
        std::ranges::any_of(cpus, [&](const Cpu& cpu) { return !shares_cache(cpu); }) &&
        std::ranges::any_of(cpus, [&](const Cpu& cpu) {
            return cpu.efficiency != top && shares_cache(cpu);
        });
    for (const Cpu& cpu : cpus) {
        const bool usable = !island || shares_cache(cpu);
        if (cpu.efficiency == top) {
            topology.fastest.push_back(cpu.id);
        }
        if (usable) {
            topology.shared.push_back(cpu.id);
            if (cpu.efficiency != top) {
                topology.efficient.push_back(cpu.id);
            }
        }
        topology.classes.push_back({cpu.group, cpu.number,
                                    cpu.efficiency == top ? CoreClass::Performance
                                    : usable              ? CoreClass::Efficient
                                                          : CoreClass::LowPower,
                                    cpu.efficiency, cpu.id});
    }

    const Policy policy = ReadPolicy();
    LOG_INFO(Common,
             "Hybrid CPU: {} P-cores, {} E-cores, {} low-power cores left out; core placement {} "
             "(LICHII_CPU_POLICY=off|soft|auto|strict)",
             topology.fastest.size(), topology.efficient.size(),
             cpus.size() - topology.shared.size(), PolicyName(policy));
    if (policy == Policy::Off) {
        return topology;
    }
    topology.policy = policy;
    topology.set_thread = set_thread;
    if (island && set_process != nullptr) {
        set_process(GetCurrentProcess(), topology.shared.data(),
                    static_cast<ULONG>(topology.shared.size()));
    }
    return topology;
}

const Topology& GetTopology() {
    static const Topology topology = Detect();
    return topology;
}

/// The CPU sets of a role, or nullptr for the process default.
const std::vector<ULONG>* RoleCpus(const Topology& topology, CoreRole role) {
    const bool strict = topology.policy == Policy::Strict;
    const bool placed = topology.policy == Policy::Auto || strict;
    switch (role) {
    case CoreRole::Any:
        return nullptr;
    case CoreRole::Fastest:
        return placed ? &topology.fastest : &topology.shared;
    case CoreRole::Performance:
        return strict ? &topology.fastest : &topology.shared;
    case CoreRole::Waiting:
        return placed ? &topology.efficient : &topology.shared;
    case CoreRole::Background:
        return strict ? &topology.efficient : &topology.shared;
    }
    return nullptr;
}

} // Anonymous namespace

void SetCurrentThreadCoreRole(CoreRole role, const char* thread_name) {
    const Topology& topology = GetTopology();
    if (topology.set_thread == nullptr) {
        return;
    }
    const std::vector<ULONG>* const cpus = RoleCpus(topology, role);
    const bool placed = cpus != nullptr && !cpus->empty();
    const BOOL result =
        placed ? topology.set_thread(GetCurrentThread(), cpus->data(),
                                     static_cast<ULONG>(cpus->size()))
               : topology.set_thread(GetCurrentThread(), nullptr, 0);
    // Pipeline builders and other pools would log one line per worker.
    if (placed && (role != CoreRole::Background || result == FALSE)) {
        const std::string_view set = cpus == &topology.fastest ? "P-cores"
                                     : cpus == &topology.efficient ? "E-cores"
                                                                   : "P- and E-cores";
        LOG_INFO(Common, "Core placement: {} on {} ({} logical processors){}",
                 thread_name != nullptr && thread_name[0] != '\0' ? thread_name : "thread", set,
                 cpus->size(), result == FALSE ? ", failed" : "");
    }
}

const std::vector<ClassifiedProcessor>& ClassifiedProcessors() {
    return GetTopology().classes;
}

} // namespace Common::Windows
