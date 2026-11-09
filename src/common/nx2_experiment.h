// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <string>

namespace Common {

/// Fritchii's numbered experiment switches (Frizti/Lichii, nx2_debug_control.h LoadExperiments):
/// the value of EDEN_NX2_EXP<n>, or `fallback` (Fritchii's default) when it is unset or empty.
/// Call it on first use and keep the value (a function-local static): static initialization
/// runs before the frontend applies nx2_switches.txt.
[[nodiscard]] inline int ReadNx2Experiment(int n, int fallback) {
    const std::string name = "EDEN_NX2_EXP" + std::to_string(n);
    const char* value = std::getenv(name.c_str());
    return value != nullptr && value[0] != '\0' ? std::atoi(value) : fallback;
}

} // namespace Common
