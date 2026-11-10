// SPDX-FileCopyrightText: Copyright 2026 CheezyEmu contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// What the emulator's own GPU work was doing, written to the log when the GPU crashes (device
// loss): the Ocarina 4K path, the FSR version, frame generation (from CheezyEmu).

#pragma once

#include <functional>
#include <string_view>
#include <map>
#include <mutex>
#include <string>

#include "common/logging.h"

namespace VideoCommon::GpuCrashContext {

struct State {
    std::mutex mutex;
    std::map<std::string, std::string, std::less<>> values;
};

inline State& Get() {
    static State state;
    return state;
}

/// Records a value (cheap when unchanged: called every frame by the Ocarina path).
inline void Set(std::string_view key, std::string_view value) {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    const auto it = state.values.find(key);
    if (it == state.values.end()) {
        state.values.emplace(std::string{key}, std::string{value});
    } else if (it->second != value) {
        it->second.assign(value);
    }
}

inline void Log() {
    State& state = Get();
    std::scoped_lock lock{state.mutex};
    if (state.values.empty()) {
        LOG_CRITICAL(Render_Vulkan, "GPU crash context: none of the emulator's own passes ran");
        return;
    }
    std::string text;
    for (const auto& [key, value] : state.values) {
        text += (text.empty() ? "" : ", ") + key + "=" + value;
    }
    LOG_CRITICAL(Render_Vulkan, "GPU crash context: {}", text);
}

} // namespace VideoCommon::GpuCrashContext
