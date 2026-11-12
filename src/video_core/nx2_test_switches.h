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

/// TEST: EDEN_NX2_TEST_1161_DRAWS=0 restores tuned4's delta overflow break, handle checks over
/// whole constant buffers and attribute format breaks (upstream #1161's fast-draw half; the
/// renderer copies it into Nx2FastDraw::g_test_1161_draws).
[[nodiscard]] inline bool Draws1161() {
    static const bool on = SwitchOn("EDEN_NX2_TEST_1161_DRAWS");
    return on;
}

/// TEST: EDEN_NX2_TEST_1161_HANDLES=0 keeps pipelines with texture handles computed at draw time
/// out of delta draws (DeltaBlocker::ComputedHandles), as before upstream #1161's e05a72847.
[[nodiscard]] inline bool Handles1161() {
    static const bool on = SwitchOn("EDEN_NX2_TEST_1161_HANDLES");
    return on;
}

/// TEST: EDEN_NX2_TEST_1161_REPORTS=0 restores, of upstream #1161's report and fence half, a
/// new fence operation list per made fence, the buffer cache lock for every pending report and
/// the host query pool reset on the recording thread.
[[nodiscard]] inline bool Reports1161() {
    static const bool on = SwitchOn("EDEN_NX2_TEST_1161_REPORTS");
    return on;
}

} // namespace VideoCommon::Nx2Test
