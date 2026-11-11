// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>

/// EDEN_NX2_CLOCK_TRACE=1 (Windows on x86-64): where a power-limited CPU's clock goes, per role
/// thread. The threads DkbCpuTrace::Registration names (guest cores, GPU front and back, Vulkan
/// worker) publish the logical processor they run on; a sampler thread ("ClockTrace") gives
/// their CPU time to those processors every 10 ms and reads each processor's clock from PDH every
/// second. Every 10 s one HW_GPU line ("Clock trace per second"): each role group's effective
/// clock, CPU time, share on P-, E- and low-power E-cores and migrations, each core class's
/// utility and clock, package power and the power mode. Read on first use, so nx2_switches.txt
/// can set it too. With it unset the hooks are one predictable branch each and nothing else runs.
namespace Common::ClockTrace {

/// Whether EDEN_NX2_CLOCK_TRACE is set and this host supports the trace (read once).
[[nodiscard]] bool Enabled();

namespace Detail {
/// Set once when the trace is enabled.
extern std::atomic<bool> g_publish;
void Publish();
void PublishInLoop();
} // namespace Detail

/// After a wait that may have slept: the thread may wake on another processor.
inline void NoteProcessor() {
    if (Detail::g_publish.load(std::memory_order_relaxed)) [[unlikely]] {
        Detail::Publish();
    }
}

/// In a role thread's busy loop, never per draw or method: every 32nd call publishes.
inline void NoteProcessorInLoop() {
    if (Detail::g_publish.load(std::memory_order_relaxed)) [[unlikely]] {
        Detail::PublishInLoop();
    }
}

struct Slot;

/// DkbCpuTrace::Registration's part: the calling thread's slot for `role` (a DkbCpuTrace::Role)
/// until UnregisterThread, or nullptr (trace off, or the role has a thread already).
[[nodiscard]] Slot* RegisterThread(size_t role);
void UnregisterThread(Slot* slot);

/// Starts the sampler for an emulation session (Core::System::Load) when the trace is on.
void StartSession();
/// Stops and joins it (Core::System::ShutdownMainProcess): the next session starts clean.
void StopSession();

} // namespace Common::ClockTrace
