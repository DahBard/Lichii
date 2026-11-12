// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "common/clock_trace.h"
#include "common/dkb_cpu_trace.h"
#include "common/logging.h"

namespace {
using namespace std::chrono_literals;
namespace ClockTrace = Common::ClockTrace;
namespace DkbCpuTrace = Common::DkbCpuTrace;

// A role thread as the emulator has them: it registers, then loops until told to stop.
class RoleThread {
public:
    RoleThread(DkbCpuTrace::Role role, bool busy) {
        thread = std::thread([this, role, busy] {
            const DkbCpuTrace::Registration registration{role};
            registered.store(true);
            while (!stop.load()) {
                if (busy) {
                    ClockTrace::NoteProcessorInLoop();
                    const u64 poll_start = ClockTrace::PollStart();
                    const auto until = std::chrono::steady_clock::now() + 20us;
                    while (std::chrono::steady_clock::now() < until) {
                    }
                    ClockTrace::PollEnd(poll_start);
                } else {
                    std::this_thread::sleep_for(1ms);
                    ClockTrace::NoteProcessor();
                }
            }
        });
        while (!registered.load()) {
            std::this_thread::yield();
        }
    }
    ~RoleThread() {
        stop.store(true);
        thread.join();
    }

private:
    std::atomic<bool> registered{false};
    std::atomic<bool> stop{false};
    std::thread thread;
};
} // namespace

// Runs only with EDEN_NX2_CLOCK_TRACE=1 (the tests workflow runs it on its Windows runner):
// the PDH, power and thread-time calls the trace makes on the owner's laptop, on a real host.
TEST_CASE("Clock trace logs a start line and a 10 s window, and stops", "[common][clock_trace]") {
    if (!ClockTrace::Enabled()) {
        SKIP("EDEN_NX2_CLOCK_TRACE is unset, or this host has no clock trace");
    }
    Common::Log::Initialize();
    Common::Log::SetColorConsoleBackendEnabled(false);
    Common::Log::SetRingCapacity(64);
    {
        const RoleThread back{DkbCpuTrace::Role::GpuBack, true};
        const RoleThread guest{DkbCpuTrace::Role::GuestCore0, true};
        const RoleThread front{DkbCpuTrace::Role::GpuCommand, false};
        // A second thread for a role that has one gets no slot.
        const RoleThread second_back{DkbCpuTrace::Role::GpuBack, false};

        // The window line comes 10 s after the sampler's setup, which can take a while on a
        // host whose performance counters were never read.
        ClockTrace::StartSession();
        std::this_thread::sleep_for(14s);
        const auto stop_start = std::chrono::steady_clock::now();
        ClockTrace::StopSession();
        CHECK(std::chrono::steady_clock::now() - stop_start < 1s);

        // The next session starts afresh.
        ClockTrace::StartSession();
        std::this_thread::sleep_for(3s);
        ClockTrace::StopSession();
    }
    // The power mode boot line, which every emulation start logs with or without the trace.
    ClockTrace::LogPowerMode();

    int start_lines = 0;
    int window_lines = 0;
    int power_lines = 0;
    for (const Common::Log::RingLine& line : Common::Log::ReadRing(0)) {
        if (line.text.find("Windows power mode \"") != std::string::npos) {
            std::printf("%s\n", line.text.c_str());
            ++power_lines;
            continue;
        }
        if (line.text.find("Clock trace") == std::string::npos) {
            continue;
        }
        std::printf("%s\n", line.text.c_str());
        if (line.text.find("Clock trace per second") != std::string::npos) {
            ++window_lines;
        } else if (line.text.find("Clock trace (EDEN_NX2_CLOCK_TRACE)") != std::string::npos) {
            ++start_lines;
        }
    }
    std::fflush(stdout);
    Common::Log::SetRingCapacity(0);
    CHECK(start_lines == 2);
    CHECK(window_lines == 1);
    CHECK(power_lines == 1);
}

TEST_CASE("Power mode line and its warning", "[common][clock_trace]") {
    ClockTrace::PowerSettings settings{.mode = "best power efficiency",
                                       .user_mode = "best power efficiency",
                                       .ac = "on",
                                       .plan = "Balanced",
                                       .boost_mode = 3,
                                       .preference = 75,
                                       .preference_1 = 33};
    CHECK(ClockTrace::PowerModeLine(settings) ==
          "Windows power mode \"best power efficiency\" (user-configured \"best power "
          "efficiency\"), on AC power, power plan \"Balanced\"; the plan's base AC values (the "
          "power mode may change them): boost mode efficient enabled (3), energy performance "
          "preference 75 (EfficiencyClass 1: 33)");
    CHECK(ClockTrace::PowerModeSlowsCores(settings));

    // No warning on battery, with the power source unknown or in another mode.
    settings.ac = "off";
    CHECK(ClockTrace::PowerModeLine(settings).find("), on battery, power plan") !=
          std::string::npos);
    CHECK_FALSE(ClockTrace::PowerModeSlowsCores(settings));
    settings.ac = "unknown";
    CHECK_FALSE(ClockTrace::PowerModeSlowsCores(settings));
    settings.ac = "on";
    settings.mode = "best performance";
    CHECK_FALSE(ClockTrace::PowerModeSlowsCores(settings));

    // The user-configured mode decides only where Windows doesn't give the one in effect.
    settings.mode = "n/a";
    CHECK(ClockTrace::PowerModeSlowsCores(settings));
    settings.user_mode = "best performance";
    CHECK_FALSE(ClockTrace::PowerModeSlowsCores(settings));

    // What Windows doesn't say reads n/a; a boost mode past the known ones is named unknown.
    ClockTrace::PowerSettings unknown{
        .mode = "n/a", .user_mode = "n/a", .ac = "unknown", .plan = "n/a"};
    CHECK(ClockTrace::PowerModeLine(unknown) ==
          "Windows power mode \"n/a\" (user-configured \"n/a\"), power source unknown, power "
          "plan \"n/a\"; the plan's base AC values (the power mode may change them): boost mode "
          "n/a, energy performance preference n/a");
    CHECK_FALSE(ClockTrace::PowerModeSlowsCores(unknown));
    unknown.boost_mode = 9;
    unknown.preference_1 = 0;
    CHECK(ClockTrace::PowerModeLine(unknown).ends_with(
        "boost mode unknown (9), energy performance preference n/a (EfficiencyClass 1: 0)"));
}
