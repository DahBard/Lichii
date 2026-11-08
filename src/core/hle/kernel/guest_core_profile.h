// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>
#include <cstddef>

#include "common/common_types.h"

namespace Kernel {

/// Where a guest core's host thread spends its time: running guest code (the JIT, with the slow
/// memory accesses and read traps it makes), each supervisor call by number, the kernel's own
/// work around them, spinning idle and waiting idle. On with LICHII_GUEST_PROFILE=1,
/// EDEN_DKB_FRAME_TRACE=1 or EDEN_NX2_GPU_PROFILE=1; each core logs a line every 10 s. Only the
/// core's own host thread touches its profile.
class GuestCoreProfile {
public:
    enum class Activity : u32 { Kernel, Guest, Svc, IdleSpin, IdleWait, Count };

    static const bool enabled;

    explicit GuestCoreProfile(size_t core_index_) : core_index{core_index_} {}

    /// Ends the current activity and starts `activity` (with its supervisor call number).
    void Switch(Activity activity, u32 svc = 0) {
        if (enabled) {
            SwitchSlow(activity, svc);
        }
    }

    /// Memory accesses the JIT made through callbacks instead of fastmem, on this thread.
    static void NoteSlowAccess() {
        if (enabled) {
            ++slow_accesses;
        }
    }

    /// Exclusive stores the JIT made through callbacks, on this thread.
    static void NoteSlowExclusive() {
        if (enabled) {
            ++slow_exclusives;
        }
    }

private:
    static constexpr size_t SvcCount = 256;
    static thread_local u64 slow_accesses;
    static thread_local u64 slow_exclusives;

    void SwitchSlow(Activity activity, u32 svc);
    void Log(u64 now, std::chrono::steady_clock::time_point wall);

    const size_t core_index;
    u64 window_start{};
    std::chrono::steady_clock::time_point window_wall{};
    u64 last{};
    u64 switches{};
    Activity current{Activity::Kernel};
    u32 current_svc{};
    u64 guest_entries{};
    std::array<u64, static_cast<size_t>(Activity::Count)> ticks{};
    std::array<u64, SvcCount> svc_ticks{};
    std::array<u32, SvcCount> svc_calls{};
};

} // namespace Kernel
