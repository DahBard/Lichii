// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace Kernel {

class KernelCore;
class KThread;

/// EDEN_NX2_HEAP_TRACE=1: what the guest's MapPhysicalMemory and UnmapPhysicalMemory (supervisor
/// calls 0x2c and 0x2d) cost the host. A call is timed from before it takes its page table's
/// physical memory lock to its end, its parts (Phase) where they run, and the process's page
/// faults are counted from the lock on. Every 10 s one HW_Memory line per direction. Read at
/// startup: set it in the environment, as the launchers do, not in nx2_switches.txt.
namespace HeapTrace {

enum class Direction : u8 { Map, Unmap };

/// Parts of a call, timed in the functions that do them; the line has the lock wait and the rest
/// (block bookkeeping, the trace's own counter reads) too.
enum class Phase : u8 {
    Allocate,  ///< KMemoryManager::AllocateForProcess: pages taken from the heap (and committed)
    ZeroFill,  ///< KMemoryManager::AllocateForProcess: pages that may hold data cleared
    PageTable, ///< Memory: the emulated page table's entries
    Host,      ///< Memory: the pages' fastmem view mapped or unmapped (HostMemory)
    Count,
};

extern const bool enabled;

[[nodiscard]] u64 NowNs();

/// Before a call takes its physical memory lock: when it started, 0 with the trace off.
[[nodiscard]] inline u64 Start() {
    return enabled ? NowNs() : 0;
}

/// Drops the unfinished 10 s window (KernelCore::Shutdown), so a later session's first lines
/// cover only that session.
void Reset();

/// A call, made once it holds its physical memory lock so that it ends before the next call on
/// that page table starts. `start` is Start(); `mapped` the page table's mapped physical memory
/// size, whose change is what the call mapped or unmapped. While it runs it is its guest
/// thread's (KThread::m_nx2_heap_trace_call), so a thread that stops in a call (emulation
/// stopped) leaves nothing behind for the next session.
class Call {
public:
    Call(KernelCore& kernel, Direction direction, u64 bytes, const size_t& mapped, u64 start) {
        if (start != 0) {
            Begin(kernel, direction, bytes, mapped, start);
        }
    }
    ~Call() {
        if (mapped_size != nullptr) {
            End();
        }
    }
    Call(const Call&) = delete;
    Call& operator=(const Call&) = delete;

private:
    friend class Phases;

    void Begin(KernelCore& kernel, Direction direction, u64 bytes, const size_t& mapped,
               u64 start);
    void End();

    const size_t* mapped_size{}; ///< Set while traced
    // Set by Begin, so that an untraced call stores nothing else.
    KThread* thread; ///< The guest thread making the call
    Direction call_direction;
    u64 call_bytes;
    u64 mapped_before;
    u64 start_ns;
    u64 locked_ns;
    u32 faults_before;
    u64 zero_filled_before;
    std::array<u64, static_cast<size_t>(Phase::Count)> phase_ns;
};

/// Splits what a function does for a traced call into phases: End(phase) gives `phase` the time
/// since the split began or since the previous End. Nothing unless the calling guest thread is
/// in a traced call.
class Phases {
public:
    explicit Phases(KernelCore& kernel) {
        if (enabled) {
            Begin(kernel);
        }
    }
    void End(Phase phase) {
        if (call != nullptr) {
            EndSlow(phase);
        }
    }

private:
    void Begin(KernelCore& kernel);
    void EndSlow(Phase phase);

    Call* call{};
    u64 since; ///< Set by Begin
};

} // namespace HeapTrace
} // namespace Kernel
