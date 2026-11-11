// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/host_memory.h"
#include "common/logging.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/heap_trace.h"
#include "core/hle/kernel/k_thread.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace Kernel::HeapTrace {
namespace {

constexpr size_t PhaseCount = static_cast<size_t>(Phase::Count);
constexpr u64 WindowNs = 10'000'000'000;

bool ReadSwitch() {
    const char* const value = std::getenv("EDEN_NX2_HEAP_TRACE");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

struct ProcessCounters {
    u32 faults{}; ///< Page faults of all its threads (Windows counts them in 32 bits)
    u64 working_set{};
    u64 commit{}; ///< Private commit
};

ProcessCounters ReadProcessCounters() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters))) {
        return {static_cast<u32>(counters.PageFaultCount), counters.WorkingSetSize,
                counters.PagefileUsage};
    }
#endif
    return {};
}

struct Totals {
    u64 calls{};
    u64 bytes{};
    u64 changed{};
    u64 total_ns{};
    u64 lock_ns{};
    std::array<u64, PhaseCount> phase_ns{};
    u64 host_regions{};
    u64 faults{};
    u64 zero_filled{};
    u64 committed{};
    u64 trace_ns{};
    std::vector<u64> call_ns;
};

struct Window {
    std::mutex mutex;
    u64 start_ns{};
    u32 start_faults{};
    std::array<Totals, 2> totals{};
};
Window g_window;

double MiB(u64 bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

/// The q quantile of `values`, which it reorders.
u64 Quantile(std::vector<u64>& values, double q) {
    const auto index = static_cast<std::ptrdiff_t>(q * static_cast<double>(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + index, values.end());
    return values[static_cast<size_t>(index)];
}

void LogTotals(Direction direction, Totals& totals, double seconds, const std::string& context) {
    const bool map = direction == Direction::Map;
    const auto ms = [seconds](u64 ns) { return static_cast<double>(ns) / 1e6 / seconds; };
    const auto us = [](u64 ns) { return static_cast<double>(ns) / 1e3; };
    const auto phase = [&totals](Phase which) {
        return totals.phase_ns[static_cast<size_t>(which)];
    };
    const auto per_call = [&totals](u64 count) {
        return totals.calls == 0 ? 0.0
                                 : static_cast<double>(count) / static_cast<double>(totals.calls);
    };
    u64 p50 = 0;
    u64 p99 = 0;
    u64 longest = 0;
    if (!totals.call_ns.empty()) {
        longest = *std::max_element(totals.call_ns.begin(), totals.call_ns.end());
        p99 = Quantile(totals.call_ns, 0.99);
        p50 = Quantile(totals.call_ns, 0.50);
    }
    u64 other = totals.total_ns - totals.lock_ns;
    for (const u64 ns : totals.phase_ns) {
        other -= (std::min)(other, ns);
    }
    std::string parts = fmt::format("lock wait {:.1f} ms", ms(totals.lock_ns));
    if (map) {
        const u64 zero_ns = phase(Phase::ZeroFill);
        parts += fmt::format(", allocation {:.1f} ms", ms(phase(Phase::Allocate)));
#ifdef _WIN32
        // EDEN_NX2_SEC_RESERVE commits guest memory the first time the heap hands it out.
        parts += fmt::format(" ({:.1f} MiB committed)", MiB(totals.committed) / seconds);
#endif
        parts += fmt::format(", zero fill {:.1f} ms ({:.1f} MiB at {:.2f} GB/s)", ms(zero_ns),
                             MiB(totals.zero_filled) / seconds,
                             zero_ns == 0 ? 0.0
                                          : static_cast<double>(totals.zero_filled) /
                                                static_cast<double>(zero_ns));
    }
    parts += fmt::format(", page table {:.1f} ms, host {} {:.1f} ms ({:.1f} regions per call)",
                         ms(phase(Phase::PageTable)), map ? "map" : "unmap",
                         ms(phase(Phase::Host)), per_call(totals.host_regions));
    if (!map || phase(Phase::Free) != 0) {
        parts += fmt::format(", free {:.1f} ms", ms(phase(Phase::Free)));
    }
    parts += fmt::format(", other {:.1f} ms; trace overhead {:.2f} ms", ms(other),
                         ms(totals.trace_ns));
#ifdef _WIN32
    parts += fmt::format(" | page faults during calls {:.1f} per call, {:.0f}/s",
                         per_call(totals.faults), static_cast<double>(totals.faults) / seconds);
#endif
    LOG_INFO(HW_Memory,
             "{} per second (EDEN_NX2_HEAP_TRACE, {:.1f} s): {:.0f} calls for {:.1f} MiB ({:.1f} "
             "MiB changed), {:.1f} ms; per call p50 {:.0f} us, p99 {:.0f} us, max {:.0f} us | "
             "{}{}",
             map ? "MapPhysicalMemory" : "UnmapPhysicalMemory", seconds,
             static_cast<double>(totals.calls) / seconds, MiB(totals.bytes) / seconds,
             MiB(totals.changed) / seconds, ms(totals.total_ns), us(p50), us(p99), us(longest), parts,
             context);
    std::vector<u64> call_ns = std::move(totals.call_ns);
    call_ns.clear();
    totals = Totals{};
    totals.call_ns = std::move(call_ns);
}

/// With g_window.mutex held.
void LogWindow(u64 now_ns, const ProcessCounters& counters) {
    const double seconds = static_cast<double>(now_ns - g_window.start_ns) / 1e9;
    std::string context;
#ifdef _WIN32
    context = fmt::format(
        " | process: page faults {:.0f}/s, working set {:.0f} MiB, commit {:.0f} MiB",
        static_cast<double>(static_cast<u32>(counters.faults - g_window.start_faults)) / seconds,
        MiB(counters.working_set), MiB(counters.commit));
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        context += fmt::format("; host: RAM available {:.0f} of {:.0f} MiB, commit {:.0f} of "
                               "{:.0f} MiB",
                               MiB(status.ullAvailPhys), MiB(status.ullTotalPhys),
                               MiB(status.ullTotalPageFile - status.ullAvailPageFile),
                               MiB(status.ullTotalPageFile));
    }
#endif
    LogTotals(Direction::Map, g_window.totals[static_cast<size_t>(Direction::Map)], seconds,
              context);
    LogTotals(Direction::Unmap, g_window.totals[static_cast<size_t>(Direction::Unmap)], seconds,
              context);
    g_window.start_ns = now_ns;
    g_window.start_faults = counters.faults;
}

} // Anonymous namespace

const bool enabled = ReadSwitch();

u64 NowNs() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return (std::max)(static_cast<u64>(ns), u64{1});
}

void Reset() {
    if (!enabled) {
        return;
    }
    std::scoped_lock lock{g_window.mutex};
    g_window.start_ns = 0;
    g_window.start_faults = 0;
    g_window.totals = {};
}

void Call::Begin(KernelCore& kernel, Direction direction, u64 bytes, const size_t& mapped,
                 u64 start) {
    locked_ns = NowNs();
    start_ns = start;
    mapped_size = &mapped;
    mapped_before = mapped;
    call_direction = direction;
    host_regions = 0;
    call_bytes = bytes;
    phase_ns = {};
    thread = GetCurrentThreadPointer(kernel);
    if (thread->m_nx2_heap_trace_call == nullptr) {
        thread->m_nx2_heap_trace_call = this;
    }
    host_memory = &kernel.System().DeviceMemory().buffer;
    faults_before = ReadProcessCounters().faults;
    zero_filled_before = Common::HostMemory::ZeroFilledBytes();
    committed_before = host_memory->GetCommitStats().committed_bytes;
    trace_ns = NowNs() - locked_ns;
}

void Call::End() {
    const u64 end_ns = NowNs();
    const ProcessCounters counters = ReadProcessCounters();
    const u64 zero_filled = Common::HostMemory::ZeroFilledBytes() - zero_filled_before;
    const u64 committed = host_memory->GetCommitStats().committed_bytes - committed_before;
    const u64 mapped_after = *mapped_size;
    if (thread->m_nx2_heap_trace_call == this) {
        thread->m_nx2_heap_trace_call = nullptr;
    }
    const u64 call_ns = end_ns - start_ns - trace_ns;
    std::scoped_lock lock{g_window.mutex};
    if (g_window.start_ns == 0) {
        g_window.start_ns = start_ns;
        g_window.start_faults = faults_before;
    }
    Totals& totals = g_window.totals[static_cast<size_t>(call_direction)];
    if (totals.call_ns.capacity() == 0) {
        totals.call_ns.reserve(8192);
    }
    ++totals.calls;
    totals.bytes += call_bytes;
    totals.changed += mapped_after > mapped_before ? mapped_after - mapped_before
                                                   : mapped_before - mapped_after;
    totals.total_ns += call_ns;
    totals.lock_ns += locked_ns - start_ns;
    for (size_t phase = 0; phase < PhaseCount; ++phase) {
        totals.phase_ns[phase] += phase_ns[phase];
    }
    totals.host_regions += host_regions;
    totals.faults += static_cast<u32>(counters.faults - faults_before);
    totals.zero_filled += zero_filled;
    totals.committed += committed;
    totals.call_ns.push_back(call_ns);
    // Begin's reads, then this function's and its wait for the lock.
    totals.trace_ns += trace_ns + (NowNs() - end_ns);
    if (end_ns >= g_window.start_ns + WindowNs) {
        LogWindow(end_ns, counters);
    }
}

void Phases::Begin(KernelCore& kernel) {
    call = GetCurrentThreadPointer(kernel)->m_nx2_heap_trace_call;
    if (call != nullptr) {
        since = NowNs();
    }
}

void Phases::EndSlow(Phase phase) {
    const u64 now = NowNs();
    call->phase_ns[static_cast<size_t>(phase)] += now - since;
    if (phase == Phase::Host) {
        ++call->host_regions;
    }
    since = now;
}

} // namespace Kernel::HeapTrace
