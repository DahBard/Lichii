// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "common/common_types.h"

/// EDEN_NX2_CLOCK_TRACE=1 (Windows on x86-64): where a power-limited CPU's clock goes, per role
/// thread. The threads DkbCpuTrace::Registration names (guest cores, GPU front and back, Vulkan
/// worker) publish the logical processor they run on; a sampler thread ("ClockTrace") gives
/// their CPU time to those processors every 10 ms and reads each processor's clock from PDH every
/// second. Every 10 s one HW_GPU line ("Clock trace per second"): each role group's effective
/// clock, CPU time split into polls and work, share on P-, E- and low-power E-cores and
/// migrations, each core class's load, utility and clock, package power, the power mode and the
/// battery. Read on first use, so nx2_switches.txt
/// can set it too. With it unset each hook is a call that returns at once and nothing else of the
/// trace runs.
namespace Common::ClockTrace {

/// Whether EDEN_NX2_CLOCK_TRACE is set and this host supports the trace (read once).
[[nodiscard]] bool Enabled();

// The hooks are out of line and noexcept: a call adds no block to its caller, so the hot loops
// that call them keep their recorded PGO profile (an inlined check changes a function's control
// flow hash, and clang then drops its profile).

/// After a wait that may have slept: the thread may wake on another processor.
void NoteProcessor() noexcept;

/// In a role thread's busy loop, never per draw or method: every 32nd call publishes.
void NoteProcessorInLoop() noexcept;

/// Around a role thread's poll before it sleeps (a spin or UMWAIT wait for another thread): its
/// time is the thread's poll_ms, which Windows counts as running. 0 with the trace off.
[[nodiscard]] u64 PollStart() noexcept;
void PollEnd(u64 start) noexcept;

struct Slot;

/// DkbCpuTrace::Registration's part: the calling thread's slot for `role` (a DkbCpuTrace::Role)
/// until UnregisterThread, or nullptr (trace off, or the role has a thread already).
[[nodiscard]] Slot* RegisterThread(size_t role);
void UnregisterThread(Slot* slot);

/// Starts the sampler for an emulation session (Core::System::Load) when the trace is on.
void StartSession();
/// Stops and joins it (Core::System::ShutdownMainProcess): the next session starts clean.
void StopSession();

/// Windows' power settings as LogPowerMode reads them ("n/a", or nothing, where Windows doesn't
/// say).
struct PowerSettings {
    std::string mode;      ///< The power mode in effect (Settings > System > Power & battery)
    std::string user_mode; ///< The mode the user chose for the current power source
    std::string_view ac;   ///< AC power: "on", "off" (battery) or "unknown"
    std::string plan;      ///< The active power plan's name
    std::optional<u32> boost_mode;   ///< The plan's base AC processor boost mode
    std::optional<u32> preference;   ///< Its base AC energy performance preference
    std::optional<u32> preference_1; ///< The same for EfficiencyClass 1 (the P-cores)
};

/// LogPowerMode's line for `settings`.
[[nodiscard]] std::string PowerModeLine(const PowerSettings& settings);
/// Whether `settings` are "best power efficiency" on AC power, the mode in which Windows held the
/// laptop's E-cores at about a third of their clock (r55, sitting 3).
[[nodiscard]] bool PowerModeSlowsCores(const PowerSettings& settings);

/// Once per emulation start (VideoCore::CreateGPU), with or without the trace: one HW_GPU line
/// with the power mode, the user-configured one for the current power source, AC power or
/// battery, the plan's name and its base boost mode and energy performance preferences, read
/// with the trace's own power code, and a warning when PowerModeSlowsCores. Windows on x86-64,
/// as the trace; nothing elsewhere.
void LogPowerMode();

} // namespace Common::ClockTrace
