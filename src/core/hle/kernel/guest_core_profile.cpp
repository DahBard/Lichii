// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <numeric>
#include <string>
#include <string_view>

#include <fmt/format.h>

#include "common/logging.h"
#include "common/spin_wait.h"
#include "core/hle/kernel/guest_core_profile.h"

namespace Kernel {
namespace {

bool ProfileEnabled() {
    for (const char* name :
         {"LICHII_GUEST_PROFILE", "EDEN_DKB_FRAME_TRACE", "EDEN_NX2_GPU_PROFILE"}) {
        const char* value = std::getenv(name);
        if (value != nullptr && value[0] == '1') {
            return true;
        }
    }
    return false;
}

/// The calls a game makes often; the others print as numbers.
std::string SvcName(u32 id) {
    switch (id) {
    case 0x01:
        return "SetHeapSize";
    case 0x06:
        return "QueryMemory";
    case 0x08:
        return "CreateThread";
    case 0x0b:
        return "SleepThread";
    case 0x0c:
        return "GetThreadPriority";
    case 0x0d:
        return "SetThreadPriority";
    case 0x0f:
        return "SetThreadCoreMask";
    case 0x10:
        return "GetCurrentProcessorNumber";
    case 0x11:
        return "SignalEvent";
    case 0x12:
        return "ClearEvent";
    case 0x16:
        return "CloseHandle";
    case 0x17:
        return "ResetSignal";
    case 0x18:
        return "WaitSynchronization";
    case 0x19:
        return "CancelSynchronization";
    case 0x1a:
        return "ArbitrateLock";
    case 0x1b:
        return "ArbitrateUnlock";
    case 0x1c:
        return "WaitProcessWideKeyAtomic";
    case 0x1d:
        return "SignalProcessWideKey";
    case 0x1e:
        return "GetSystemTick";
    case 0x21:
        return "SendSyncRequest";
    case 0x22:
        return "SendSyncRequestWithUserBuffer";
    case 0x25:
        return "GetThreadId";
    case 0x27:
        return "OutputDebugString";
    case 0x29:
        return "GetInfo";
    case 0x2c:
        return "MapPhysicalMemory";
    case 0x34:
        return "WaitForAddress";
    case 0x35:
        return "SignalToAddress";
    case 0x36:
        return "SynchronizePreemptionState";
    case 0x43:
        return "ReplyAndReceive";
    default:
        return fmt::format("{:#04x}", id);
    }
}

} // Anonymous namespace

const bool GuestCoreProfile::enabled = ProfileEnabled();
thread_local u64 GuestCoreProfile::slow_accesses = 0;
thread_local u64 GuestCoreProfile::slow_exclusives = 0;

void GuestCoreProfile::SwitchSlow(Activity activity, u32 svc) {
    const u64 now = Common::SpinWait::Now();
    if (window_start == 0) {
        window_start = now;
        window_wall = std::chrono::steady_clock::now();
        last = now;
        slow_accesses = 0;
        slow_exclusives = 0;
    }
    const u64 elapsed = now - last;
    ticks[static_cast<size_t>(current)] += elapsed;
    if (current == Activity::Svc && current_svc < SvcCount) {
        svc_ticks[current_svc] += elapsed;
    }
    last = now;
    current = activity;
    current_svc = svc;
    if (activity == Activity::Svc && svc < SvcCount) {
        ++svc_calls[svc];
    } else if (activity == Activity::Guest) {
        ++guest_entries;
    }
    // The wall clock is read now and then; it also calibrates the ticks.
    if ((++switches & 1023) == 0) {
        const auto wall = std::chrono::steady_clock::now();
        if (wall - window_wall >= std::chrono::seconds{10}) {
            Log(now, wall);
        }
    }
}

void GuestCoreProfile::Log(u64 now, std::chrono::steady_clock::time_point wall) {
    const double seconds = std::chrono::duration<double>(wall - window_wall).count();
    const double ticks_per_ms = static_cast<double>(now - window_start) / (seconds * 1000.0);
    const auto ms_per_second = [&](u64 value) {
        return static_cast<double>(value) / ticks_per_ms / seconds;
    };
    const auto per_second = [&](u64 value) { return static_cast<double>(value) / seconds; };
    const auto at = [&](Activity activity) {
        return ms_per_second(ticks[static_cast<size_t>(activity)]);
    };

    std::array<u32, SvcCount> order{};
    std::iota(order.begin(), order.end(), 0U);
    std::ranges::sort(order, [&](u32 a, u32 b) { return svc_ticks[a] > svc_ticks[b]; });
    std::string calls;
    u64 total_calls = 0;
    for (const u32 count : svc_calls) {
        total_calls += count;
    }
    for (size_t i = 0; i < 8 && svc_ticks[order[i]] != 0; ++i) {
        const u32 id = order[i];
        fmt::format_to(std::back_inserter(calls), "{}{} {:.1f} ms ({:.0f}/s)",
                       calls.empty() ? "" : ", ", SvcName(id), ms_per_second(svc_ticks[id]),
                       per_second(svc_calls[id]));
    }
    LOG_INFO(Kernel,
             "Guest core {} per second: guest code {:.1f} ms ({:.0f} entries, {:.0f} slow memory "
             "accesses, {:.0f} slow exclusive stores), supervisor calls {:.1f} ms ({:.0f}), "
             "kernel {:.1f} ms, idle spin {:.1f} ms, idle wait {:.1f} ms | calls: {}",
             core_index, at(Activity::Guest), per_second(guest_entries),
             per_second(slow_accesses), per_second(slow_exclusives), at(Activity::Svc),
             per_second(total_calls), at(Activity::Kernel), at(Activity::IdleSpin),
             at(Activity::IdleWait), calls.empty() ? "none" : calls);

    window_start = now;
    window_wall = wall;
    guest_entries = 0;
    slow_accesses = 0;
    slow_exclusives = 0;
    ticks.fill(0);
    svc_ticks.fill(0);
    svc_calls.fill(0);
}

} // namespace Kernel
