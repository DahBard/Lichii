// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>

#include "common/common_types.h"

namespace Common {

namespace SpinWait {

/// Whether waits idle the core with UMONITOR/UMWAIT: the CPU has WAITPKG (Intel Tremont, Alder
/// Lake and later), its TSC rate is known, and LICHII_WAITPKG_SPIN is not 0.
bool UsesWaitpkg();

/// TSC ticks a PAUSE instruction takes on this CPU (measured once).
u64 PauseTicks();

/// TSC ticks per microsecond.
u64 TicksPerMicrosecond();

u64 Now();
void Pause();

/// Arms the address monitor on `address`'s cache line (UMONITOR). Requires UsesWaitpkg().
void Monitor(const void* address);

/// Idles the core in C0.1 until `deadline` (TSC), a write to the monitored line or an
/// interrupt (UMWAIT). Requires UsesWaitpkg().
void WaitUntil(u64 deadline);

template <typename Ready>
bool IdleUntil(const void* address, u64 deadline, Ready& ready) {
    // Conditions that are not on the monitored line are seen within a slice.
    const u64 slice = 4 * TicksPerMicrosecond();
    while (true) {
        if (ready()) {
            return true;
        }
        const u64 now = Now();
        if (now >= deadline) {
            return false;
        }
        Monitor(address);
        if (ready()) {
            return true;
        }
        WaitUntil(deadline - now > slice ? now + slice : deadline);
    }
}

} // namespace SpinWait

/// Waits until `ready()` holds, at most about as long as `pauses` PAUSE instructions take on
/// this CPU. The first polls spin with PAUSE; after them, on CPUs with WAITPKG, the core idles
/// in C0.1 until another core writes `address`'s cache line instead of executing PAUSE, which
/// wakes as fast and leaves more of a power-limited CPU's budget to the busy cores. Returns
/// whether `ready()` held.
template <typename Ready>
bool SpinUntil(const void* address, u32 pauses, Ready&& ready) {
    constexpr u32 FastPolls = 64;
    u32 spin = 0;
    for (; spin < pauses && spin < FastPolls; ++spin) {
        if (ready()) {
            return true;
        }
        SpinWait::Pause();
    }
    if (spin < pauses && SpinWait::UsesWaitpkg()) {
        const u64 deadline = SpinWait::Now() + u64{pauses - spin} * SpinWait::PauseTicks();
        return SpinWait::IdleUntil(address, deadline, ready);
    }
    for (; spin < pauses; ++spin) {
        if (ready()) {
            return true;
        }
        SpinWait::Pause();
    }
    return ready();
}

/// Waits like SpinUntil for up to `duration`. Requires SpinWait::UsesWaitpkg().
template <typename Ready>
bool SpinUntilFor(const void* address, std::chrono::microseconds duration, Ready&& ready) {
    const u64 deadline =
        SpinWait::Now() + static_cast<u64>(duration.count()) * SpinWait::TicksPerMicrosecond();
    return SpinWait::IdleUntil(address, deadline, ready);
}

} // namespace Common
