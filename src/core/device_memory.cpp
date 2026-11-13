// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef _WIN32
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>

#include "common/logging.h"
#include "common/memory_detect.h"
#include "common/thread.h"
#endif

#include "core/device_memory.h"
#include "hle/kernel/board/nintendo/nx/k_system_control.h"

namespace Core {

#ifdef HAS_NCE
constexpr size_t VirtualReserveSize = 1ULL << 38;
#else
constexpr size_t VirtualReserveSize = 1ULL << 39;
#endif

DeviceMemory::DeviceMemory()
    : buffer{Kernel::Board::Nintendo::Nx::KSystemControl::Init::GetIntendedMemorySize(),
             VirtualReserveSize} {
#ifdef _WIN32
    // EDEN_NX2_ZERO_TRIM=0: no zero-page passes (A/B: ab-25-no-zero-trim.bat).
    if (const char* value = std::getenv("EDEN_NX2_ZERO_TRIM");
        value == nullptr || value[0] != '0') {
        zero_trim_thread = std::jthread{[this](std::stop_token stop) { TrimZeroPages(stop); }};
    }
#endif
}

DeviceMemory::~DeviceMemory() = default;

#ifdef _WIN32
// Windows can't punch holes in the guest memory section, so pages holding only zeros (games clear
// their heaps while booting: Ocarina leaves ~7 GB) stay resident until they leave the working set.
// A pass comes every 5 s while they keep turning up, backing off to once every 5 minutes.
void DeviceMemory::TrimZeroPages(std::stop_token stop) {
    using namespace std::chrono_literals;
    Common::SetCurrentThreadName("ZeroTrim");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
    constexpr size_t Busy = size_t{64} << 20;
    std::mutex mutex;
    std::condition_variable_any wake;
    std::chrono::seconds interval = 5s;
    Common::HostMemory::CommitStats logged_commit{};
    while (true) {
        {
            std::unique_lock lock{mutex};
            wake.wait_for(lock, stop, interval, [] { return false; });
        }
        if (stop.stop_requested()) {
            return;
        }
        // EDEN_NX2_SEC_RESERVE: the guest memory's commit, when it grew by 512 MiB or the fault
        // handler had to commit pages (each a missed commit: that count must stay 0).
        if (const auto commit = buffer.GetCommitStats();
            commit.reserved &&
            (commit.committed_bytes >= logged_commit.committed_bytes + (u64{512} << 20) ||
             commit.fault_commits != logged_commit.fault_commits)) {
            const bool in_virtual = (commit.first_fault_offset >> 63) != 0;
            LOG_INFO(HW_Memory,
                     "Guest memory: {} of {} MiB committed (EDEN_NX2_SEC_RESERVE), {} fault "
                     "commits (first at {} offset {:#x})",
                     commit.committed_bytes >> 20, commit.section_bytes >> 20,
                     commit.fault_commits, in_virtual ? "virtual" : "backing",
                     commit.first_fault_offset & ~(u64{1} << 63));
            logged_commit = commit;
        }
        const auto start = std::chrono::steady_clock::now();
        Common::HostMemory::ZeroTrimPhases phases{};
        std::pair<size_t, size_t> result;
        try {
            result = buffer.TrimZeroPages(&phases);
        } catch (const std::bad_alloc&) {
            // A pass needs 32 MiB and more of scratch. When Windows runs out of commit, the
            // exception leaving this thread ended the emulator (#1346); the pass is only a saving.
            interval = std::chrono::seconds{60};
            const Common::ProcessMemoryUsage usage = Common::GetProcessMemoryUsage();
            LOG_WARNING(HW_Memory,
                        "Skipped trimming all-zero guest pages: out of memory. Windows commit is "
                        "{} of {} MiB (this process {} MiB); the limit is RAM plus the page file",
                        usage.system_commit >> 20, usage.system_commit_limit >> 20,
                        usage.commit >> 20);
            continue;
        }
        const auto [resident, trimmed] = result;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        if (trimmed < Busy) {
            // A pass that trims little still queries the working set and checks every resident
            // page. A slow one is logged, to line it up with slow frames.
            interval = (std::min)(interval * 2, std::chrono::seconds{300});
            if (ms >= 100) {
                LOG_INFO(HW_Memory,
                         "Zero-page pass: {} MiB trimmed of {} MiB resident in {} ms "
                         "(next in {} s); working set query {} ms, page reads {} ms, {} unlock "
                         "runs {} ms (longest {:.1f} ms)",
                         trimmed >> 20, resident >> 20, ms, interval.count(),
                         phases.query_us / 1000, phases.scan_us / 1000, phases.unlock_runs,
                         phases.unlock_us / 1000,
                         static_cast<double>(phases.longest_unlock_us) / 1e3);
            }
            continue;
        }
        interval = 5s;
        LOG_INFO(HW_Memory,
                 "Trimmed {} MiB of all-zero guest pages out of {} MiB resident in {} ms; the kernel "
                 "zero-filled {} MiB so far; working set query {} ms, page reads {} ms, {} unlock "
                 "runs {} ms (longest {:.1f} ms)",
                 trimmed >> 20, resident >> 20, ms, Common::HostMemory::ZeroFilledBytes() >> 20,
                 phases.query_us / 1000, phases.scan_us / 1000, phases.unlock_runs,
                 phases.unlock_us / 1000, static_cast<double>(phases.longest_unlock_us) / 1e3);
    }
}
#endif

} // namespace Core
