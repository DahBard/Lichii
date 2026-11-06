// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Common::Windows {

/// What a thread does, for placing it on a hybrid CPU (Intel Alder Lake and later).
enum class CoreRole {
    Any,         ///< The process default: every core but a low-power island.
    Fastest,     ///< Threads that bound the frame time (GPU command threads): P-cores.
    Performance, ///< Busy, latency-sensitive threads (guest cores): P- and E-cores.
    Waiting,     ///< Threads that wait without sleeping (core timing): E-cores.
    Background,  ///< Throughput work (pipeline builds): E-cores.
};

/// Restricts the calling thread to the CPU sets of its role. Does nothing on CPUs with a single
/// core type or with LICHII_CPU_POLICY=off.
void SetCurrentThreadCoreRole(CoreRole role, const char* thread_name);

} // namespace Common::Windows
