// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <numeric>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/logging.h"

namespace VideoCommon {

/// Present-to-present frame times, for the frame rate's lows. LICHII_FRAME_TIMES=1 (also on with
/// EDEN_DKB_FRAME_TRACE=1 or EDEN_NX2_GPU_PROFILE=1) logs each second's frame times
/// ("DKB frame_ms") and, every 10 s, their percentiles and lows. A 1% low is the frame rate of the
/// slowest 1% of frames: 1000 / their mean time in ms.
class FrameTimeStats {
public:
    static bool Enabled() {
        static const bool enabled = [] {
            for (const char* name :
                 {"LICHII_FRAME_TIMES", "EDEN_DKB_FRAME_TRACE", "EDEN_NX2_GPU_PROFILE"}) {
                const char* value = std::getenv(name);
                if (value != nullptr && value[0] == '1') {
                    return true;
                }
            }
            return false;
        }();
        return enabled;
    }

    void Note(std::chrono::steady_clock::time_point now) {
        if (last == std::chrono::steady_clock::time_point{}) {
            last = now;
            second_end = now + std::chrono::seconds{1};
            window_end = now + std::chrono::seconds{10};
            return;
        }
        const float ms = std::chrono::duration<float, std::milli>(now - last).count();
        last = now;
        second.push_back(ms);
        window.push_back(ms);
        if (now >= second_end) {
            std::string list;
            for (const float value : second) {
                fmt::format_to(std::back_inserter(list), " {:.1f}", value);
            }
            LOG_INFO(Render_Vulkan, "DKB frame_ms n={}{}", second.size(), list);
            second.clear();
            second_end = now + std::chrono::seconds{1};
        }
        if (now >= window_end) {
            LogWindow();
            window.clear();
            window_end = now + std::chrono::seconds{10};
        }
    }

private:
    void LogWindow() {
        std::vector<float> sorted = window;
        std::sort(sorted.begin(), sorted.end());
        const size_t count = sorted.size();
        const double total = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        const auto at = [&](double quantile) {
            return sorted[std::min(count - 1,
                                  static_cast<size_t>(quantile * static_cast<double>(count)))];
        };
        const auto low = [&](double fraction) {
            const size_t slowest =
                std::max<size_t>(1, static_cast<size_t>(fraction * static_cast<double>(count)));
            const double sum = std::accumulate(sorted.end() - slowest, sorted.end(), 0.0);
            return 1000.0 * static_cast<double>(slowest) / sum;
        };
        const auto over = [&](float ms) {
            const auto first = std::upper_bound(sorted.begin(), sorted.end(), ms);
            return 100.0 * static_cast<double>(sorted.end() - first) / static_cast<double>(count);
        };
        LOG_INFO(Render_Vulkan,
                 "Frame times: {} frames in {:.1f} s, {:.1f} fps | ms p50 {:.1f} p90 {:.1f} p99 "
                 "{:.1f} max {:.1f} | 1% low {:.1f} fps, 0.1% low {:.1f} fps | over 33.4 ms "
                 "{:.1f}%, over 50 ms {:.1f}%, over 100 ms {:.1f}%",
                 count, total / 1000.0, 1000.0 * static_cast<double>(count) / total, at(0.5),
                 at(0.9), at(0.99), sorted.back(), low(0.01), low(0.001), over(33.4f),
                 over(50.0f), over(100.0f));
    }

    std::chrono::steady_clock::time_point last{};
    std::chrono::steady_clock::time_point second_end{};
    std::chrono::steady_clock::time_point window_end{};
    std::vector<float> second;
    std::vector<float> window;
};

} // namespace VideoCommon
