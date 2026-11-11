// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon::Nx2Test {

/// TEST: on unless the variable is "0". Read on first use and kept: the frontend has applied
/// nx2_switches.txt by then.
[[nodiscard]] inline bool SwitchOn(const char* name) {
    const char* const value = std::getenv(name);
    return value == nullptr || value[0] != '0';
}

/// TEST: EDEN_NX2_TEST_QUICK=0 restores tuned4's flush after queueing a fence, its device
/// memory usage query on every call and InvalidateGPUCache's gather with nothing collected.
[[nodiscard]] inline bool QuickWins() {
    static const bool on = SwitchOn("EDEN_NX2_TEST_QUICK");
    return on;
}

/// TEST: EDEN_NX2_TEST_CONTIG=0 restores tuned4's VirtualRangeCache::Query for storage bindings
/// that lie in one device range (BufferCache::ResolveMultiRangeStorage).
[[nodiscard]] inline bool ContiguousResolve() {
    static const bool on = SwitchOn("EDEN_NX2_TEST_CONTIG");
    return on;
}

/// TEST: EDEN_NX2_TEST_1161_DRAWS=0 restores tuned4's delta overflow break, handle checks over
/// whole constant buffers and attribute format breaks (upstream #1161's fast-draw half; the
/// renderer copies it into Nx2FastDraw::g_test_1161_draws).
[[nodiscard]] inline bool Draws1161() {
    static const bool on = SwitchOn("EDEN_NX2_TEST_1161_DRAWS");
    return on;
}

/// TEST: EDEN_NX2_TEST_BANK_BITS=0 restores the native bank check's walk of every page with a
/// snapshot lookup each (BufferCache::CheckNativeBankPagesTracked).
[[nodiscard]] inline bool BankBits() {
    static const bool on = SwitchOn("EDEN_NX2_TEST_BANK_BITS");
    return on;
}

} // namespace VideoCommon::Nx2Test
