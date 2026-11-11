// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

#include "common/spin_wait.h"

#ifdef ARCHITECTURE_x86_64
#ifdef _MSC_VER
#include <intrin.h>
#endif
#include <immintrin.h>
#include "common/cpu_features.h"
#endif

namespace Common::SpinWait {

bool UsesC02() {
    static const bool enabled = [] {
        const char* const value = std::getenv("LICHII_UMWAIT_C02");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return enabled;
}

u64 MonitoredLineSliceTicks() {
    return UsesC02() ? ~u64{0} : SliceTicks();
}

#ifdef ARCHITECTURE_x86_64

u64 Now() {
    return static_cast<u64>(__rdtsc());
}

void Pause() {
    _mm_pause();
}

namespace {

u64 MeasurePauseTicks() {
    // Skylake-derived Intel cores take ~140 cycles per PAUSE, AMD's far fewer: the spin budgets
    // in pauses were tuned on one of them, so they are kept as time on this CPU.
    u64 best = ~u64{0};
    for (int trial = 0; trial < 5; ++trial) {
        const u64 start = Now();
        for (int i = 0; i < 256; ++i) {
            _mm_pause();
        }
        best = (std::min)(best, Now() - start);
    }
    return std::clamp<u64>(best / 256, 1, 1000);
}

} // Anonymous namespace

u64 PauseTicks() {
    static const u64 ticks = MeasurePauseTicks();
    return ticks;
}

u64 TicksPerMicrosecond() {
    static const u64 ticks = (std::max)(g_cpu_caps.tsc_frequency / 1'000'000, u64{1});
    return ticks;
}

// clang and GCC compile the WAITPKG intrinsics into a function marked for it. MSVC builds (the
// comparison build) spin with PAUSE as before.
#if defined(__clang__) || defined(__GNUC__)
#define LICHII_WAITPKG 1
#else
#define LICHII_WAITPKG 0
#endif

bool UsesWaitpkg() {
    static const bool enabled = [] {
        const char* const value = std::getenv("LICHII_WAITPKG_SPIN");
        if (!LICHII_WAITPKG || (value != nullptr && value[0] == '0')) {
            return false;
        }
        return g_cpu_caps.waitpkg && g_cpu_caps.invariant_tsc &&
               g_cpu_caps.tsc_frequency >= 100'000'000;
    }();
    return enabled;
}

#if LICHII_WAITPKG
__attribute__((target("waitpkg"))) void Monitor(const void* address) {
    _umonitor(const_cast<void*>(address));
}

__attribute__((target("waitpkg"))) void WaitUntil(u64 deadline) {
    if (UsesC02()) [[unlikely]] {
        // TEST: control bit 0 clear, C0.2, which saves more and wakes slower. Windows may
        // disallow C0.2 (IA32_UMWAIT_CONTROL bit 0); the core then waits in C0.1 all the same.
        _umwait(0, deadline);
        return;
    }
    // Control bit 0 set: C0.1, the state with the faster wakeup.
    _umwait(1, deadline);
}
#else
void Monitor(const void*) {}

void WaitUntil(u64) {}
#endif

#else

u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

void Pause() {
#if defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

u64 PauseTicks() {
    return 1;
}

u64 TicksPerMicrosecond() {
    return 1000;
}

bool UsesWaitpkg() {
    return false;
}

void Monitor(const void*) {}

void WaitUntil(u64) {}

#endif

} // namespace Common::SpinWait
