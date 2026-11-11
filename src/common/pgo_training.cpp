// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>

#include <fmt/format.h>

#include "common/common_types.h"
#include "common/logging.h"
#include "common/pgo_training.h"

#ifdef __LLVM_INSTR_PROFILE_GENERATE
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
#endif

namespace Common::PgoTraining {

#ifdef __LLVM_INSTR_PROFILE_GENERATE

namespace {

std::atomic<s64> g_next_write{0};

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Write() {
    static const bool named = [] {
        // Not the default name: it merges into the file on every write, which would count the
        // earlier part of the session again each time. Each write appends instead, and the
        // runtime truncates only the startup file, so the start time keeps a reused process id
        // from appending a second session to an old file.
        if (std::getenv("LLVM_PROFILE_FILE") == nullptr) {
            const std::string name =
                fmt::format("lichii-pgo-{}-%p.profraw", static_cast<s64>(std::time(nullptr)));
            __llvm_profile_set_filename(name.c_str());
        }
        return true;
    }();
    (void)named;
    const int result = __llvm_profile_write_file();
    LOG_INFO(Common, "PGO training profile written ({})", result == 0 ? "ok" : "failed");
}

} // Anonymous namespace

void Tick() {
    const s64 now = NowMs();
    s64 next = g_next_write.load(std::memory_order_relaxed);
    if (next != 0 && now < next) {
        return;
    }
    if (!g_next_write.compare_exchange_strong(next, now + 30'000, std::memory_order_relaxed)) {
        return;
    }
    if (next != 0) {
        Write();
    }
}

void Flush() {
    Write();
}

#else

void Tick() {}

void Flush() {}

#endif

} // namespace Common::PgoTraining
