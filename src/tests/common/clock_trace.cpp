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

    int start_lines = 0;
    int window_lines = 0;
    for (const Common::Log::RingLine& line : Common::Log::ReadRing(0)) {
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
}
