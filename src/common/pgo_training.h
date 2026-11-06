// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Common::PgoTraining {

/// Instrumented builds (clang -fprofile-generate): writes the profile every 30 s while a game
/// runs, so a training session survives a crash or a killed process. Does nothing otherwise.
void Tick();

/// Instrumented builds: writes the profile now. Does nothing otherwise.
void Flush();

} // namespace Common::PgoTraining
