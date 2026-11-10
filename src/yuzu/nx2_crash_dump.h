// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// EDEN_NX2_CRASH_DUMP (1: %APPDATA%\lichii\crash_dumps, or a folder): on a crash, writes
/// lichii-crash-<pid>.txt (the exception, registers, call stack, code addresses on the stack,
/// loaded and recently unloaded modules) and lichii-crash-<pid>.dmp, without Windows' LocalDumps
/// registry key. Exceptions something handles (fastmem faults) never reach it; Windows' own
/// crash report follows as before. Called after the switch files are read and again once a
/// game launched, in case a library installed its own filter meanwhile. Windows only.
void InstallNx2CrashDump();
