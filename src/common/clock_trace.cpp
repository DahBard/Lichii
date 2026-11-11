// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <mutex>

#include "common/clock_trace.h"
#include "common/common_types.h"
#include "common/dkb_cpu_trace.h"
#include "common/logging.h"

#if defined(_WIN32) && defined(ARCHITECTURE_x86_64)
#define LICHII_CLOCK_TRACE 1
#else
#define LICHII_CLOCK_TRACE 0
#endif

#if LICHII_CLOCK_TRACE
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include "common/cpu_features.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "common/windows/hybrid_cpu.h"
#include "common/x64/rdtsc.h"
#endif

namespace Common::ClockTrace {

/// A role thread's slot. The thread writes processor, migrations, poll_ticks and loop_calls; the
/// rest changes under the registry mutex, which the sampler holds while it reads a slot. One
/// cache line each, so that role threads never write each other's.
struct alignas(64) Slot {
    std::atomic<u32> processor{};  ///< (group << 8) | number, as GetCurrentProcessorNumberEx gives
    std::atomic<u32> migrations{}; ///< Changes of `processor`
    std::atomic<u64> poll_ticks{}; ///< TSC ticks between PollStart and PollEnd
    u32 loop_calls{};
    bool active{};
    u64 generation{};
    void* handle{}; ///< The thread, for QueryThreadCycleTime
};

namespace {

/// Set once when the trace is enabled; every hook reads it. A cache line of its own, so that no
/// often-written variable that the linker places next to it makes the hooks miss.
struct alignas(64) PublishFlag {
    std::atomic<bool> on{false};
};
PublishFlag g_publish;

constexpr u32 LoopPublishInterval = 32;

thread_local Slot* t_slot = nullptr;

bool Requested() {
    static const bool requested = [] {
        const char* const value = std::getenv("EDEN_NX2_CLOCK_TRACE");
        return value != nullptr && value[0] != '\0' && value[0] != '0';
    }();
    return requested;
}

u32 CurrentProcessor() {
#if LICHII_CLOCK_TRACE
    PROCESSOR_NUMBER number{};
    GetCurrentProcessorNumberEx(&number);
    return (u32{number.Group} << 8) | number.Number;
#else
    return 0;
#endif
}

void PublishTo(Slot& slot) {
    const u32 processor = CurrentProcessor();
    if (processor != slot.processor.load(std::memory_order_relaxed)) {
        slot.processor.store(processor, std::memory_order_relaxed);
        slot.migrations.store(slot.migrations.load(std::memory_order_relaxed) + 1,
                              std::memory_order_relaxed);
    }
}

#if LICHII_CLOCK_TRACE

struct Registry {
    std::mutex mutex;
    std::array<Slot, DkbCpuTrace::RoleCount> slots;
};

Registry& GetRegistry() {
    // Never destroyed: a role thread may still unregister while statics are destroyed at exit.
    static Registry* const registry = new Registry;
    return *registry;
}

using CoreClass = Windows::CoreClass;
using SteadyClock = std::chrono::steady_clock;

constexpr auto SampleInterval = std::chrono::milliseconds{10};
constexpr auto CollectInterval = std::chrono::seconds{1};
constexpr auto LogInterval = std::chrono::seconds{10};
constexpr size_t ClassCount = 4;
constexpr double NoReading = std::numeric_limits<double>::quiet_NaN();

constexpr size_t ClassIndex(CoreClass core_class) {
    return static_cast<size_t>(core_class);
}

constexpr std::array<std::string_view, ClassCount> ClassNames{"cores", "P", "E", "LP-E"};
constexpr std::array<std::string_view, ClassCount> ShareNames{"cores", "p", "e", "lpe"};

// Power mode overlays (the Settings > Power slider), as PowerGetEffectiveOverlayScheme gives them.
constexpr GUID OverlayBestEfficiency{
    0x961cc777, 0x2547, 0x4f9d, {0x81, 0x74, 0x7d, 0x86, 0x18, 0x1b, 0x8a, 0x7a}};
constexpr GUID OverlayBalanced{};
constexpr GUID OverlayBetterPerformance{
    0x3af9b8d9, 0x7c97, 0x431d, {0xad, 0x78, 0x34, 0xa8, 0xbf, 0xea, 0x43, 0x9f}};
constexpr GUID OverlayBestPerformance{
    0xded574b5, 0x45a0, 0x4f42, {0x87, 0x37, 0x46, 0x34, 0x5c, 0x09, 0xc2, 0x38}};

// Processor power management settings (winnt.h; the "_1" ones are for processors of
// EfficiencyClass 1 on hybrid CPUs).
constexpr GUID ProcessorSubgroup{
    0x54533251, 0x82be, 0x4824, {0x96, 0xc1, 0x47, 0xb6, 0x0b, 0x74, 0x0d, 0x00}};
constexpr GUID BoostModeSetting{
    0xbe337238, 0x0d82, 0x4146, {0xa9, 0x60, 0x4f, 0x37, 0x49, 0xd4, 0x70, 0xc7}};
constexpr GUID MaximumStateSetting{
    0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xec}};
constexpr GUID MaximumStateSetting1{
    0xbc5038f7, 0x23e0, 0x4960, {0x96, 0xda, 0x33, 0xab, 0xaf, 0x59, 0x35, 0xed}};
constexpr GUID MinimumStateSetting{
    0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4c}};
constexpr GUID MinimumStateSetting1{
    0x893dee8e, 0x2bef, 0x41e0, {0x89, 0xc6, 0xb5, 0x5d, 0x09, 0x29, 0x96, 0x4d}};
constexpr GUID PreferenceSetting{
    0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x63}};
constexpr GUID PreferenceSetting1{
    0x36687f9e, 0xe3a5, 0x4dbf, {0xb1, 0xdc, 0x15, 0xeb, 0x38, 0x1c, 0x68, 0x64}};

bool SameGuid(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

std::string GuidText(const GUID& guid) {
    return fmt::format("{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
                       guid.Data1, guid.Data2, guid.Data3, guid.Data4[0], guid.Data4[1],
                       guid.Data4[2], guid.Data4[3], guid.Data4[4], guid.Data4[5], guid.Data4[6],
                       guid.Data4[7]);
}

std::string OverlayName(const GUID& overlay) {
    if (SameGuid(overlay, OverlayBestEfficiency)) {
        return "best power efficiency";
    }
    if (SameGuid(overlay, OverlayBalanced)) {
        return "balanced";
    }
    if (SameGuid(overlay, OverlayBetterPerformance)) {
        return "better performance";
    }
    if (SameGuid(overlay, OverlayBestPerformance)) {
        return "best performance";
    }
    return GuidText(overlay);
}

std::string_view BoostModeName(DWORD mode) {
    static constexpr std::array<std::string_view, 7> names{
        "disabled",           "enabled",
        "aggressive",         "efficient enabled",
        "efficient aggressive", "aggressive at guaranteed",
        "efficient aggressive at guaranteed"};
    return mode < names.size() ? names[mode] : "unknown";
}

/// powrprof.dll, loaded rather than linked: PowerGetEffectiveOverlayScheme is not in every SDK.
class PowerApi {
public:
    using GetActiveSchemeFn = DWORD(WINAPI*)(HKEY, GUID**);
    using ReadFriendlyNameFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, UCHAR*,
                                              DWORD*);
    using ReadACValueIndexFn = DWORD(WINAPI*)(HKEY, const GUID*, const GUID*, const GUID*, DWORD*);
    using GetEffectiveOverlaySchemeFn = DWORD(WINAPI*)(GUID*);

    PowerApi() {
        module = LoadLibraryExW(L"powrprof.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            return;
        }
        // Through void*, as elsewhere: a direct cast from FARPROC trips -Wcast-function-type.
        const auto proc = [this](const char* name) {
            return reinterpret_cast<void*>(GetProcAddress(module, name));
        };
        get_active_scheme = reinterpret_cast<GetActiveSchemeFn>(proc("PowerGetActiveScheme"));
        read_friendly_name = reinterpret_cast<ReadFriendlyNameFn>(proc("PowerReadFriendlyName"));
        read_ac_value_index =
            reinterpret_cast<ReadACValueIndexFn>(proc("PowerReadACValueIndex"));
        get_effective_overlay_scheme = reinterpret_cast<GetEffectiveOverlaySchemeFn>(
            proc("PowerGetEffectiveOverlayScheme"));
    }
    ~PowerApi() {
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }
    PowerApi(const PowerApi&) = delete;
    PowerApi& operator=(const PowerApi&) = delete;

    std::optional<GUID> ActiveScheme() const {
        GUID* scheme = nullptr;
        if (get_active_scheme == nullptr || get_active_scheme(nullptr, &scheme) != ERROR_SUCCESS ||
            scheme == nullptr) {
            return std::nullopt;
        }
        const GUID result = *scheme;
        LocalFree(scheme);
        return result;
    }

    std::string SchemeName(const GUID& scheme) const {
        DWORD size = 0;
        if (read_friendly_name == nullptr ||
            read_friendly_name(nullptr, &scheme, nullptr, nullptr, nullptr, &size) !=
                ERROR_SUCCESS ||
            size < sizeof(wchar_t)) {
            return "n/a";
        }
        std::wstring name(size / sizeof(wchar_t), L'\0');
        if (read_friendly_name(nullptr, &scheme, nullptr, nullptr,
                               reinterpret_cast<UCHAR*>(name.data()), &size) != ERROR_SUCCESS) {
            return "n/a";
        }
        name.resize(std::wcslen(name.c_str()));
        return UTF16ToUTF8(name);
    }

    std::optional<DWORD> AcValue(const GUID& scheme, const GUID& setting) const {
        DWORD value = 0;
        if (read_ac_value_index == nullptr ||
            read_ac_value_index(nullptr, &scheme, &ProcessorSubgroup, &setting, &value) !=
                ERROR_SUCCESS) {
            return std::nullopt;
        }
        return value;
    }

    std::string PowerMode() const {
        GUID overlay{};
        if (get_effective_overlay_scheme == nullptr ||
            get_effective_overlay_scheme(&overlay) != ERROR_SUCCESS) {
            return "n/a";
        }
        return OverlayName(overlay);
    }

private:
    HMODULE module{};
    GetActiveSchemeFn get_active_scheme{};
    ReadFriendlyNameFn read_friendly_name{};
    ReadACValueIndexFn read_ac_value_index{};
    GetEffectiveOverlaySchemeFn get_effective_overlay_scheme{};
};

struct PowerState {
    std::string mode;
    std::string_view ac;
    std::string_view battery_saver;

    bool operator==(const PowerState&) const = default;
};

PowerState ReadPowerState(const PowerApi& api) {
    PowerState state{.mode = api.PowerMode(), .ac = "unknown", .battery_saver = "unknown"};
    SYSTEM_POWER_STATUS status{};
    if (GetSystemPowerStatus(&status)) {
        state.ac = status.ACLineStatus == 1 ? "on" : status.ACLineStatus == 0 ? "off" : "unknown";
        state.battery_saver = (status.SystemStatusFlag & 1) != 0 ? "on" : "off";
    }
    return state;
}

/// pdh.dll, loaded by the first session that reads a counter rather than linked, so that the
/// process does not load it with the trace off. Kept loaded once it is.
class PdhApi {
public:
    using OpenQueryFn = PDH_STATUS(WINAPI*)(LPCWSTR, DWORD_PTR, PDH_HQUERY*);
    using AddEnglishCounterFn = PDH_STATUS(WINAPI*)(PDH_HQUERY, LPCWSTR, DWORD_PTR,
                                                    PDH_HCOUNTER*);
    using CollectQueryDataFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);
    using GetFormattedCounterArrayFn = PDH_STATUS(WINAPI*)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD,
                                                           PDH_FMT_COUNTERVALUE_ITEM_W*);
    using CloseQueryFn = PDH_STATUS(WINAPI*)(PDH_HQUERY);

    static const PdhApi& Get() {
        static const PdhApi* const api = new PdhApi;
        return *api;
    }

    bool Loaded() const {
        return open_query != nullptr && add_english_counter != nullptr &&
               collect_query_data != nullptr && get_formatted_counter_array != nullptr &&
               close_query != nullptr;
    }

    OpenQueryFn open_query{};
    AddEnglishCounterFn add_english_counter{};
    CollectQueryDataFn collect_query_data{};
    GetFormattedCounterArrayFn get_formatted_counter_array{};
    CloseQueryFn close_query{};

private:
    PdhApi() {
        const HMODULE module = LoadLibraryExW(L"pdh.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            return;
        }
        const auto proc = [module](const char* name) {
            return reinterpret_cast<void*>(GetProcAddress(module, name));
        };
        open_query = reinterpret_cast<OpenQueryFn>(proc("PdhOpenQueryW"));
        add_english_counter = reinterpret_cast<AddEnglishCounterFn>(proc("PdhAddEnglishCounterW"));
        collect_query_data = reinterpret_cast<CollectQueryDataFn>(proc("PdhCollectQueryData"));
        get_formatted_counter_array =
            reinterpret_cast<GetFormattedCounterArrayFn>(proc("PdhGetFormattedCounterArrayW"));
        close_query = reinterpret_cast<CloseQueryFn>(proc("PdhCloseQuery"));
    }
};

/// A PDH query of wildcard counters, read per instance.
class PdhQuery {
public:
    PdhQuery() : api{PdhApi::Get()} {
        if (!api.Loaded() || api.open_query(nullptr, 0, &query) != ERROR_SUCCESS) {
            query = nullptr;
        }
    }
    ~PdhQuery() {
        if (query != nullptr) {
            api.close_query(query);
        }
    }
    PdhQuery(const PdhQuery&) = delete;
    PdhQuery& operator=(const PdhQuery&) = delete;

    /// The counter, or nullptr where this Windows has no such object or counter. English names,
    /// so that localized Windows finds them too.
    PDH_HCOUNTER Add(const wchar_t* path) {
        PDH_HCOUNTER counter{};
        if (query == nullptr ||
            api.add_english_counter(query, path, 0, &counter) != ERROR_SUCCESS) {
            return nullptr;
        }
        return counter;
    }

    bool Collect() {
        return query != nullptr && api.collect_query_data(query) == ERROR_SUCCESS;
    }

    /// Calls visit(instance name, value) for each instance with a valid value. Rate counters
    /// have one from the second collection on.
    template <typename Visit>
    bool Read(PDH_HCOUNTER counter, Visit&& visit) {
        // Percentages above 100 (turbo, utility) must not be capped.
        constexpr DWORD Format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
        DWORD bytes = 0;
        DWORD count = 0;
        PDH_STATUS status =
            api.get_formatted_counter_array(counter, Format, &bytes, &count, nullptr);
        for (int attempt = 0; attempt < 3 && static_cast<DWORD>(status) == PDH_MORE_DATA;
             ++attempt) {
            buffer.resize(bytes / sizeof(PDH_FMT_COUNTERVALUE_ITEM_W) + 1);
            bytes = static_cast<DWORD>(buffer.size() * sizeof(PDH_FMT_COUNTERVALUE_ITEM_W));
            status =
                api.get_formatted_counter_array(counter, Format, &bytes, &count, buffer.data());
        }
        if (status != ERROR_SUCCESS) {
            return false;
        }
        for (DWORD i = 0; i < count; ++i) {
            const PDH_FMT_COUNTERVALUE_ITEM_W& item = buffer[i];
            if ((item.FmtValue.CStatus == PDH_CSTATUS_VALID_DATA ||
                 item.FmtValue.CStatus == PDH_CSTATUS_NEW_DATA) &&
                item.szName != nullptr && std::isfinite(item.FmtValue.doubleValue)) {
                visit(item.szName, item.FmtValue.doubleValue);
            }
        }
        return true;
    }

private:
    const PdhApi& api;
    PDH_HQUERY query{};
    std::vector<PDH_FMT_COUNTERVALUE_ITEM_W> buffer;
};

/// A Processor Information instance's processor, (group << 8) | number, from "group,number";
/// nothing for the "_Total" ones.
std::optional<u32> ProcessorKey(const wchar_t* name) {
    const auto number = [&name](u32 limit) -> std::optional<u32> {
        if (*name < L'0' || *name > L'9') {
            return std::nullopt;
        }
        u32 value = 0;
        while (*name >= L'0' && *name <= L'9') {
            value = value * 10 + static_cast<u32>(*name - L'0');
            if (value > limit) {
                return std::nullopt;
            }
            ++name;
        }
        return value;
    };
    const auto group = number(0xffff);
    if (!group || *name != L',') {
        return std::nullopt;
    }
    ++name;
    const auto index = number(0xff);
    if (!index || *name != L'\0') {
        return std::nullopt;
    }
    return (*group << 8) | *index;
}

std::string ProcessorList(const std::vector<std::pair<u16, u8>>& processors) {
    const bool one_group = std::ranges::all_of(
        processors, [&processors](const auto& processor) {
            return processor.first == processors.front().first;
        });
    std::string text;
    for (size_t i = 0; i < processors.size();) {
        size_t last = i;
        while (last + 1 < processors.size() &&
               processors[last + 1].first == processors[i].first &&
               processors[last + 1].second == processors[last].second + 1) {
            ++last;
        }
        const std::string group =
            one_group ? std::string{} : fmt::format("{}:", processors[i].first);
        fmt::format_to(std::back_inserter(text), "{}{}{}", text.empty() ? "" : ",", group,
                       processors[i].second);
        if (last != i) {
            fmt::format_to(std::back_inserter(text), "-{}", processors[last].second);
        }
        i = last + 1;
    }
    return text;
}

void UseEcoQoS() {
    // THREAD_POWER_THROTTLING_STATE, which older SDKs and MinGW lack.
    struct PowerThrottlingState {
        ULONG version;
        ULONG control_mask;
        ULONG state_mask;
    };
    using SetThreadInformationFn = BOOL(WINAPI*)(HANDLE, THREAD_INFORMATION_CLASS, LPVOID, DWORD);
    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto set = reinterpret_cast<SetThreadInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel32, "SetThreadInformation")));
    if (set == nullptr) {
        return;
    }
    // Version 1, execution speed throttled: Windows prefers efficient cores and clocks.
    PowerThrottlingState state{1, 1, 1};
    set(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
}

/// The clock trace of one emulation session.
class Sampler {
public:
    Sampler() : thread{[this](std::stop_token stop) { Run(stop); }} {}
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

private:
    struct Processor {
        u16 group;
        u8 number;
        CoreClass core_class;
        u8 efficiency_class;
        double nominal_mhz = NoReading;
        // This second's readings.
        double frequency = NoReading;   ///< Processor Frequency, MHz (the nominal clock)
        double performance = NoReading; ///< % Processor Performance (above 100 in turbo)
        double utility = NoReading;     ///< % Processor Utility
        double busy = NoReading;        ///< % Processor Time
        // The window's.
        double utility_sum{};
        u32 utility_seconds{};
        double busy_sum{};
        double busy_mhz_sum{}; ///< Busy % times effective MHz
    };

    struct RoleState {
        u64 generation{};
        bool has_last{};
        u64 last_cycles{};
        u32 last_migrations{};
        u64 last_poll{};
        std::vector<u64> second_cycles; ///< Per processor, then processor unknown
        // The window's.
        u64 cycles{};
        std::array<u64, ClassCount> class_cycles{};
        double mhz_cycles{};   ///< Cycles times the effective MHz they ran at
        u64 clocked_cycles{};  ///< Cycles with a known clock
        u64 migrations{};
        u64 poll_cycles{}; ///< TSC ticks in polls
    };

    struct GroupTotals {
        u64 cycles{};
        std::array<u64, ClassCount> class_cycles{};
        double mhz_cycles{};
        u64 clocked_cycles{};
        u64 migrations{};
        u64 poll_cycles{};
    };

    struct PowerCounter {
        std::string_view prefix; ///< Before its instance names in the lines
        PDH_HCOUNTER counter;
    };

    void Run(std::stop_token stop);
    bool WaitUntil(SteadyClock::time_point deadline, const std::stop_token& stop);
    void Setup();
    size_t IndexOf(u32 key) const;
    void SampleThreads();
    void CollectSecond();
    void ReadProcessors(PDH_HCOUNTER counter, double Processor::*reading);
    void LogStart();
    void LogWindow(SteadyClock::time_point now);
    GroupTotals Totals(std::initializer_list<size_t> role_indices) const;
    std::string GroupFields(const GroupTotals& totals, double seconds) const;
    u64 OwnCycles() const;

    std::mutex wait_mutex;
    std::condition_variable_any wait_cv;

    // The sampler thread's own.
    double tsc_hz{};
    std::vector<Processor> processors;
    std::vector<size_t> processor_index; ///< By key; processors.size() for an unknown processor
    std::array<bool, ClassCount> class_present{};
    std::array<RoleState, DkbCpuTrace::RoleCount> roles{};
    std::unique_ptr<PdhQuery> processor_query;
    std::unique_ptr<PdhQuery> power_query;
    PDH_HCOUNTER frequency_counter{};
    PDH_HCOUNTER performance_counter{};
    PDH_HCOUNTER utility_counter{};
    PDH_HCOUNTER time_counter{};
    std::vector<PowerCounter> power_counters;
    std::map<std::string, std::pair<double, u32>> power_window; ///< mW sum and seconds
    std::unique_ptr<PowerApi> power_api;
    PowerState last_power;
    double collect_ms = NoReading; ///< The CPU time of the last processor counter collection
    u32 collect_failures{};
    bool started{};
    SteadyClock::time_point window_start;
    u64 window_own_cycles{};

    std::jthread thread; ///< Last: starts once the members above exist, stops before they go
};

void Sampler::Run(std::stop_token stop) {
    SetCurrentThreadName("ClockTrace");
    SetCurrentThreadPriority(ThreadPriority::Low);
    // Off the P-cores (E-cores with the default core placement), and EcoQoS.
    SetCurrentThreadToWaitingCores();
    UseEcoQoS();
    Setup();
    window_start = SteadyClock::now();
    window_own_cycles = OwnCycles();
    auto next_sample = window_start + SampleInterval;
    auto next_collect = window_start + CollectInterval;
    const auto advance = [](SteadyClock::time_point& next, SteadyClock::duration interval,
                            SteadyClock::time_point now) {
        next += interval;
        if (next <= now) {
            next = now + interval;
        }
    };
    while (WaitUntil((std::min)(next_sample, next_collect), stop)) {
        const auto now = SteadyClock::now();
        if (now < next_sample && now < next_collect) {
            continue;
        }
        SampleThreads();
        advance(next_sample, SampleInterval, now);
        if (now < next_collect) {
            continue;
        }
        advance(next_collect, CollectInterval, now);
        CollectSecond();
        if (!started) {
            // After the second collection: rate counters and power instances are known.
            LogStart();
            started = true;
        }
        // Ten collections a line, however late this one woke.
        if (now - window_start >= LogInterval - std::chrono::milliseconds{500}) {
            LogWindow(now);
        }
    }
}

bool Sampler::WaitUntil(SteadyClock::time_point deadline, const std::stop_token& stop) {
    std::unique_lock lock{wait_mutex};
    wait_cv.wait_until(lock, stop, deadline, [] { return false; });
    return !stop.stop_requested();
}

u64 Sampler::OwnCycles() const {
    ULONG64 cycles = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles);
    return cycles;
}

void Sampler::Setup() {
    // QueryThreadCycleTime counts TSC ticks on CPUs with an invariant TSC, which run at a
    // fixed rate whatever the cores' clocks: CPU time is cycles over the TSC rate.
    tsc_hz = static_cast<double>(g_cpu_caps.tsc_frequency);
    if (tsc_hz <= 0.0) {
        tsc_hz = static_cast<double>(X64::EstimateRDTSCFrequency());
    }

    for (const Windows::ClassifiedProcessor& processor : Windows::ClassifiedProcessors()) {
        processors.push_back({.group = processor.group,
                              .number = processor.number,
                              .core_class = processor.core_class,
                              .efficiency_class = processor.efficiency_class});
    }
    if (processors.empty()) {
        const WORD groups = GetActiveProcessorGroupCount();
        for (WORD group = 0; group < groups; ++group) {
            const DWORD count = (std::min)(GetActiveProcessorCount(group), DWORD{64});
            for (DWORD number = 0; number < count; ++number) {
                processors.push_back({.group = group,
                                      .number = static_cast<u8>(number),
                                      .core_class = CoreClass::Uniform,
                                      .efficiency_class = 0});
            }
        }
    }
    std::ranges::sort(processors, [](const Processor& a, const Processor& b) {
        return std::pair{a.group, a.number} < std::pair{b.group, b.number};
    });
    u32 last_key = 0;
    for (const Processor& processor : processors) {
        last_key = (std::max)(last_key, (u32{processor.group} << 8) | processor.number);
        class_present[ClassIndex(processor.core_class)] = true;
    }
    processor_index.assign(size_t{last_key} + 1, processors.size());
    for (size_t i = 0; i < processors.size(); ++i) {
        processor_index[(size_t{processors[i].group} << 8) | processors[i].number] = i;
    }
    for (RoleState& role : roles) {
        role.second_cycles.assign(processors.size() + 1, 0);
    }

    processor_query = std::make_unique<PdhQuery>();
    frequency_counter = processor_query->Add(L"\\Processor Information(*)\\Processor Frequency");
    performance_counter =
        processor_query->Add(L"\\Processor Information(*)\\% Processor Performance");
    utility_counter = processor_query->Add(L"\\Processor Information(*)\\% Processor Utility");
    time_counter = processor_query->Add(L"\\Processor Information(*)\\% Processor Time");
    // Power: the Energy Meter counters are the CPU's RAPL domains (RAPL_Package0_PKG is the
    // package; Windows 11 has them without a driver), the Power Meter ones ACPI or platform
    // power meters where a machine has them. Both "Power" counters are in milliwatts: the power
    // meter interface's unit, and what RAPL_Package0_PKG reads as in tools that show it.
    power_query = std::make_unique<PdhQuery>();
    for (const auto& [prefix, path] :
         {std::pair{std::string_view{}, L"\\Energy Meter(*)\\Power"},
          std::pair{std::string_view{"power_meter:"}, L"\\Power Meter(*)\\Power"}}) {
        if (const PDH_HCOUNTER counter = power_query->Add(path); counter != nullptr) {
            power_counters.push_back({prefix, counter});
        }
    }
    power_query->Collect();

    if (processor_query->Collect()) {
        // The nominal clock is a plain value, there after the first collection.
        ReadProcessors(frequency_counter, &Processor::frequency);
        for (Processor& processor : processors) {
            processor.nominal_mhz = processor.frequency;
            processor.frequency = NoReading;
        }
    }

    power_api = std::make_unique<PowerApi>();
    last_power = ReadPowerState(*power_api);
}

size_t Sampler::IndexOf(u32 key) const {
    return key < processor_index.size() ? processor_index[key] : processors.size();
}

void Sampler::SampleThreads() {
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    for (size_t role = 0; role < roles.size(); ++role) {
        const Slot& slot = registry.slots[role];
        RoleState& state = roles[role];
        if (!slot.active) {
            state.has_last = false;
            continue;
        }
        if (state.generation != slot.generation) {
            // Another thread for the role (a new session's): it starts from its first sample.
            state.generation = slot.generation;
            state.has_last = false;
        }
        ULONG64 cycles = 0;
        if (!QueryThreadCycleTime(static_cast<HANDLE>(slot.handle), &cycles)) {
            state.has_last = false;
            continue;
        }
        const u32 migrations = slot.migrations.load(std::memory_order_relaxed);
        const u64 poll = slot.poll_ticks.load(std::memory_order_relaxed);
        if (state.has_last && cycles >= state.last_cycles) {
            // The time since the last sample goes to the processor the thread published last.
            state.second_cycles[IndexOf(slot.processor.load(std::memory_order_relaxed))] +=
                cycles - state.last_cycles;
            state.migrations += migrations - state.last_migrations;
            state.poll_cycles += poll - state.last_poll;
        }
        state.has_last = true;
        state.last_cycles = cycles;
        state.last_migrations = migrations;
        state.last_poll = poll;
    }
}

void Sampler::ReadProcessors(PDH_HCOUNTER counter, double Processor::*reading) {
    if (counter == nullptr) {
        return;
    }
    processor_query->Read(counter, [this, reading](const wchar_t* name, double value) {
        if (const auto key = ProcessorKey(name)) {
            if (const size_t index = IndexOf(*key); index < processors.size()) {
                processors[index].*reading = value;
            }
        }
    });
}

void Sampler::CollectSecond() {
    const u64 collect_start = OwnCycles();
    if (processor_query->Collect()) {
        ReadProcessors(frequency_counter, &Processor::frequency);
        ReadProcessors(performance_counter, &Processor::performance);
        ReadProcessors(utility_counter, &Processor::utility);
        ReadProcessors(time_counter, &Processor::busy);
        collect_ms = static_cast<double>(OwnCycles() - collect_start) * 1e3 / tsc_hz;
    } else {
        ++collect_failures;
    }
    // A processor's effective clock for the second: its nominal clock times its performance
    // (the clock while it ran, from the CPU's actual and reference cycle counts).
    std::vector<double> effective_mhz(processors.size(), NoReading);
    for (size_t i = 0; i < processors.size(); ++i) {
        Processor& processor = processors[i];
        const double mhz = processor.frequency * processor.performance / 100.0;
        if (std::isfinite(mhz) && mhz > 0.0) {
            effective_mhz[i] = mhz;
        }
        if (std::isfinite(processor.utility)) {
            processor.utility_sum += processor.utility;
            ++processor.utility_seconds;
        }
        if (std::isfinite(processor.busy) && std::isfinite(effective_mhz[i])) {
            processor.busy_sum += processor.busy;
            processor.busy_mhz_sum += processor.busy * effective_mhz[i];
        }
        processor.frequency = processor.performance = processor.utility = processor.busy =
            NoReading;
    }
    for (RoleState& role : roles) {
        for (size_t i = 0; i < role.second_cycles.size(); ++i) {
            const u64 cycles = role.second_cycles[i];
            if (cycles == 0) {
                continue;
            }
            role.second_cycles[i] = 0;
            role.cycles += cycles;
            if (i == processors.size()) {
                continue;
            }
            role.class_cycles[ClassIndex(processors[i].core_class)] += cycles;
            if (std::isfinite(effective_mhz[i])) {
                role.mhz_cycles += static_cast<double>(cycles) * effective_mhz[i];
                role.clocked_cycles += cycles;
            }
        }
    }
    if (!power_counters.empty() && power_query->Collect()) {
        for (const PowerCounter& power : power_counters) {
            power_query->Read(power.counter, [this, &power](const wchar_t* name, double value) {
                std::string instance = UTF16ToUTF8(std::wstring_view{name});
                if (instance.find("_Total") != std::string::npos) {
                    return;
                }
                std::ranges::replace(instance, ' ', '_');
                auto& [sum, seconds] = power_window[fmt::format("{}{}", power.prefix, instance)];
                sum += value;
                ++seconds;
            });
        }
    }
}

void Sampler::LogStart() {
    std::string classes;
    for (const CoreClass core_class : {CoreClass::Performance, CoreClass::Efficient,
                                       CoreClass::LowPower, CoreClass::Uniform}) {
        std::vector<std::pair<u16, u8>> members;
        double low = NoReading;
        double high = NoReading;
        u8 efficiency_class = 0;
        for (const Processor& processor : processors) {
            if (processor.core_class != core_class) {
                continue;
            }
            members.emplace_back(processor.group, processor.number);
            efficiency_class = processor.efficiency_class;
            if (std::isfinite(processor.nominal_mhz)) {
                low = std::isfinite(low) ? (std::min)(low, processor.nominal_mhz)
                                         : processor.nominal_mhz;
                high = std::isfinite(high) ? (std::max)(high, processor.nominal_mhz)
                                           : processor.nominal_mhz;
            }
        }
        if (members.empty()) {
            continue;
        }
        const std::string nominal =
            !std::isfinite(low) ? std::string{"n/a"}
            : low == high       ? fmt::format("{:.0f} MHz", low)
                                : fmt::format("{:.0f}-{:.0f} MHz", low, high);
        const std::string_view name = core_class == CoreClass::Performance ? "P-cores"
                                      : core_class == CoreClass::Efficient ? "E-cores"
                                      : core_class == CoreClass::LowPower  ? "LP-E cores"
                                                                           : "cores";
        fmt::format_to(std::back_inserter(classes), "{}{} {} (EfficiencyClass {}, {} nominal)",
                       classes.empty() ? "" : ", ", name, ProcessorList(members),
                       efficiency_class, nominal);
    }

    std::string plan = "power plan n/a";
    std::string values = "n/a";
    if (const auto scheme = power_api->ActiveScheme()) {
        plan = fmt::format("power plan \"{}\" ({})", power_api->SchemeName(*scheme),
                           GuidText(*scheme));
        const auto value = [&](const GUID& setting, const GUID& setting_1,
                               std::string_view unit) {
            const auto base = power_api->AcValue(*scheme, setting);
            std::string text = base ? fmt::format("{}{}", *base, unit) : std::string{"n/a"};
            if (const auto class_1 = power_api->AcValue(*scheme, setting_1)) {
                fmt::format_to(std::back_inserter(text), " (EfficiencyClass 1: {}{})", *class_1,
                               unit);
            }
            return text;
        };
        const auto boost = power_api->AcValue(*scheme, BoostModeSetting);
        values = fmt::format(
            "boost mode {}, maximum processor state {}, minimum processor state {}, energy "
            "performance preference {}",
            boost ? fmt::format("{} ({})", BoostModeName(*boost), *boost) : std::string{"n/a"},
            value(MaximumStateSetting, MaximumStateSetting1, "%"),
            value(MinimumStateSetting, MinimumStateSetting1, "%"),
            value(PreferenceSetting, PreferenceSetting1, ""));
    }

    std::string power = "package power n/a";
    if (!power_counters.empty()) {
        std::string instances;
        for (const auto& [instance, reading] : power_window) {
            fmt::format_to(std::back_inserter(instances), "{}{}", instances.empty() ? "" : ", ",
                           instance);
        }
        power = fmt::format("package power counters: {}", instances.empty() ? "none" : instances);
    }

    LOG_INFO(HW_GPU,
             "Clock trace (EDEN_NX2_CLOCK_TRACE): {}; {}, power mode \"{}\", AC power {}, battery "
             "saver {}; the plan's base AC values (the power mode may change them): {}; {}; TSC "
             "{:.1f} MHz; reading the processor counters took {:.2f} ms",
             classes.empty() ? "no processors" : classes, plan, last_power.mode, last_power.ac,
             last_power.battery_saver, values, power, tsc_hz / 1e6, collect_ms);
}

Sampler::GroupTotals Sampler::Totals(std::initializer_list<size_t> role_indices) const {
    GroupTotals totals;
    for (const size_t index : role_indices) {
        const RoleState& role = roles[index];
        totals.cycles += role.cycles;
        for (size_t i = 0; i < ClassCount; ++i) {
            totals.class_cycles[i] += role.class_cycles[i];
        }
        totals.mhz_cycles += role.mhz_cycles;
        totals.clocked_cycles += role.clocked_cycles;
        totals.migrations += role.migrations;
        totals.poll_cycles += role.poll_cycles;
    }
    return totals;
}

std::string Sampler::GroupFields(const GroupTotals& totals, double seconds) const {
    std::string text =
        totals.clocked_cycles == 0
            ? std::string{"ghz=n/a"}
            : fmt::format("ghz={:.2f}", totals.mhz_cycles /
                                            static_cast<double>(totals.clocked_cycles) / 1e3);
    const auto ms = [this, seconds](u64 ticks) {
        return static_cast<double>(ticks) * 1e3 / tsc_hz / seconds;
    };
    // Polls are timed by the TSC while the thread may also be preempted: work never goes below 0.
    const u64 work = totals.cycles > totals.poll_cycles ? totals.cycles - totals.poll_cycles : 0;
    fmt::format_to(std::back_inserter(text), " cpu_ms={:.1f} poll_ms={:.1f} work_ms={:.1f}",
                   ms(totals.cycles), ms(totals.poll_cycles), ms(work));
    // Shares only where there is more than one class.
    if (!class_present[ClassIndex(CoreClass::Uniform)]) {
        u64 classified = 0;
        for (const u64 cycles : totals.class_cycles) {
            classified += cycles;
        }
        for (size_t i = 0; i < ClassCount; ++i) {
            if (!class_present[i]) {
                continue;
            }
            fmt::format_to(std::back_inserter(text), " {}={:.0f}%", ShareNames[i],
                           classified == 0 ? 0.0
                                           : static_cast<double>(totals.class_cycles[i]) * 100.0 /
                                                 static_cast<double>(classified));
        }
    }
    fmt::format_to(std::back_inserter(text), " migrations={:.1f}",
                   static_cast<double>(totals.migrations) / seconds);
    return text;
}

void Sampler::LogWindow(SteadyClock::time_point now) {
    using DkbCpuTrace::Role;
    const auto index = [](Role role) { return static_cast<size_t>(role); };
    const double seconds = std::chrono::duration<double>(now - window_start).count();

    std::string line = fmt::format(
        "back {} | front {} | vulkan_worker {}",
        GroupFields(Totals({index(Role::GpuBack)}), seconds),
        GroupFields(Totals({index(Role::GpuCommand)}), seconds),
        GroupFields(Totals({index(Role::VulkanWorker)}), seconds));
    const GroupTotals guest =
        Totals({index(Role::GuestCore0), index(Role::GuestCore1), index(Role::GuestCore2),
                index(Role::GuestCore3), index(Role::GuestCore4), index(Role::GuestCore5),
                index(Role::GuestSingleCore)});
    fmt::format_to(std::back_inserter(line), " | guest_cores {}", GroupFields(guest, seconds));
    // By work: a core that idles in its polls all the time shows close to 1000 cpu_ms too.
    const auto work = [this](size_t role) {
        const RoleState& state = roles[role];
        return state.cycles > state.poll_cycles ? state.cycles - state.poll_cycles : u64{0};
    };
    size_t busiest = index(Role::GuestCore0);
    for (size_t core = index(Role::GuestCore0); core <= index(Role::GuestSingleCore); ++core) {
        if (work(core) > work(busiest)) {
            busiest = core;
        }
    }
    if (roles[busiest].cycles == 0) {
        line += " | busiest_guest_core core=none";
    } else {
        fmt::format_to(std::back_inserter(line), " | busiest_guest_core core={} {}",
                       busiest == index(Role::GuestSingleCore) ? std::string{"single"}
                                                               : fmt::format("{}", busiest),
                       GroupFields(Totals({busiest}), seconds));
    }

    for (size_t class_index = 0; class_index < ClassCount; ++class_index) {
        if (!class_present[class_index]) {
            continue;
        }
        double utility = 0.0;
        u32 utility_count = 0;
        double busy = 0.0;
        double busy_mhz = 0.0;
        // Each top-class processor: its clock while busy and its utility.
        std::vector<std::pair<double, double>> each;
        for (const Processor& processor : processors) {
            if (ClassIndex(processor.core_class) != class_index) {
                continue;
            }
            const double mean_utility =
                processor.utility_seconds == 0
                    ? NoReading
                    : processor.utility_sum / static_cast<double>(processor.utility_seconds);
            if (std::isfinite(mean_utility)) {
                utility += mean_utility;
                ++utility_count;
            }
            busy += processor.busy_sum;
            busy_mhz += processor.busy_mhz_sum;
            each.emplace_back(processor.busy_sum > 0.0
                                  ? processor.busy_mhz_sum / processor.busy_sum / 1e3
                                  : 0.0,
                              std::isfinite(mean_utility) ? mean_utility : 0.0);
        }
        fmt::format_to(std::back_inserter(line), " | {} util={} ghz={}", ClassNames[class_index],
                       utility_count == 0
                           ? std::string{"n/a"}
                           : fmt::format("{:.0f}%", utility / static_cast<double>(utility_count)),
                       busy > 0.0 ? fmt::format("{:.2f}", busy_mhz / busy / 1e3)
                                  : std::string{"n/a"});
        const bool top_class = class_index == ClassIndex(CoreClass::Performance) ||
                               class_index == ClassIndex(CoreClass::Uniform);
        if (top_class && busy > 0.0) {
            std::ranges::sort(each, std::greater{});
            std::string cores;
            for (const auto& [ghz, core_utility] : each) {
                fmt::format_to(std::back_inserter(cores), "{}{:.2f}@{:.0f}%",
                               cores.empty() ? "" : ",", ghz, core_utility);
            }
            fmt::format_to(std::back_inserter(line), " each={}", cores);
        }
    }

    if (power_counters.empty()) {
        line += " | power n/a";
    } else {
        line += " | power";
        if (power_window.empty()) {
            line += " none";
        }
        for (const auto& [instance, reading] : power_window) {
            // The counters' milliwatts as watts.
            fmt::format_to(std::back_inserter(line), " {}={:.1f}W", instance,
                           reading.first / static_cast<double>(reading.second) / 1e3);
        }
    }

    const PowerState power = ReadPowerState(*power_api);
    fmt::format_to(std::back_inserter(line),
                   " | power_mode=\"{}\" ac={} battery_saver={} changed={}", power.mode,
                   power.ac, power.battery_saver, power == last_power ? "no" : "yes");
    last_power = power;
    if (collect_failures != 0) {
        fmt::format_to(std::back_inserter(line), " | pdh_failures={}", collect_failures);
    }

    const u64 own_cycles = OwnCycles();
    LOG_INFO(HW_GPU,
             "Clock trace per second (EDEN_NX2_CLOCK_TRACE, {:.1f} s): {} | trace overhead {:.2f} "
             "ms/s",
             seconds, line,
             static_cast<double>(own_cycles - window_own_cycles) * 1e3 / tsc_hz / seconds);

    // This line's own formatting counts in the next window.
    window_start = now;
    window_own_cycles = own_cycles;
    collect_failures = 0;
    power_window.clear();
    for (RoleState& role : roles) {
        role.cycles = 0;
        role.class_cycles = {};
        role.mhz_cycles = 0.0;
        role.clocked_cycles = 0;
        role.migrations = 0;
        role.poll_cycles = 0;
    }
    for (Processor& processor : processors) {
        processor.utility_sum = 0.0;
        processor.utility_seconds = 0;
        processor.busy_sum = 0.0;
        processor.busy_mhz_sum = 0.0;
    }
}

std::mutex g_session_mutex;
std::unique_ptr<Sampler> g_sampler;

#endif // LICHII_CLOCK_TRACE

} // Anonymous namespace

bool Enabled() {
    static const bool enabled = [] {
        const bool on = LICHII_CLOCK_TRACE != 0 && Requested();
        if (on) {
            g_publish.on.store(true, std::memory_order_relaxed);
        }
        return on;
    }();
    return enabled;
}

void NoteProcessor() noexcept {
    if (g_publish.on.load(std::memory_order_relaxed)) [[unlikely]] {
        if (Slot* const slot = t_slot) {
            PublishTo(*slot);
        }
    }
}

void NoteProcessorInLoop() noexcept {
    if (g_publish.on.load(std::memory_order_relaxed)) [[unlikely]] {
        Slot* const slot = t_slot;
        if (slot != nullptr && ++slot->loop_calls % LoopPublishInterval == 0) {
            PublishTo(*slot);
        }
    }
}

u64 PollStart() noexcept {
#if LICHII_CLOCK_TRACE
    if (g_publish.on.load(std::memory_order_relaxed)) [[unlikely]] {
        if (t_slot != nullptr) {
            return X64::FencedRDTSC();
        }
    }
#endif
    return 0;
}

void PollEnd([[maybe_unused]] u64 start) noexcept {
#if LICHII_CLOCK_TRACE
    if (start == 0) [[likely]] {
        return;
    }
    if (Slot* const slot = t_slot) {
        const u64 ticks = X64::FencedRDTSC() - start;
        slot->poll_ticks.store(slot->poll_ticks.load(std::memory_order_relaxed) + ticks,
                               std::memory_order_relaxed);
    }
#endif
}

Slot* RegisterThread(size_t role) {
    if (!Enabled() || role >= DkbCpuTrace::RoleCount) {
        return nullptr;
    }
#if LICHII_CLOCK_TRACE
    HANDLE handle{};
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                         THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        return nullptr;
    }
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    Slot& slot = registry.slots[role];
    if (slot.active) {
        CloseHandle(handle);
        return nullptr;
    }
    slot.active = true;
    ++slot.generation;
    slot.handle = handle;
    slot.processor.store(CurrentProcessor(), std::memory_order_relaxed);
    slot.migrations.store(0, std::memory_order_relaxed);
    slot.poll_ticks.store(0, std::memory_order_relaxed);
    slot.loop_calls = 0;
    t_slot = &slot;
    return &slot;
#else
    return nullptr;
#endif
}

void UnregisterThread(Slot* slot) {
    if (slot == nullptr) {
        return;
    }
    if (t_slot == slot) {
        t_slot = nullptr;
    }
#if LICHII_CLOCK_TRACE
    auto& registry = GetRegistry();
    std::scoped_lock lock{registry.mutex};
    slot->active = false;
    CloseHandle(static_cast<HANDLE>(slot->handle));
    slot->handle = nullptr;
#endif
}

void StartSession() {
    if (!Enabled()) {
        if (Requested()) {
            LOG_INFO(HW_GPU, "Clock trace: not supported on this host");
        }
        return;
    }
#if LICHII_CLOCK_TRACE
    // A process that exits with a session running (no ShutdownMainProcess) stops the sampler
    // here rather than in g_sampler's destructor, which may run after the logger's: exit runs
    // what is registered later first.
    [[maybe_unused]] static const bool stop_at_exit = [] {
        std::atexit([] { StopSession(); });
        return true;
    }();
    std::scoped_lock lock{g_session_mutex};
    g_sampler.reset();
    g_sampler = std::make_unique<Sampler>();
#endif
}

void StopSession() {
#if LICHII_CLOCK_TRACE
    std::scoped_lock lock{g_session_mutex};
    g_sampler.reset();
#endif
}

} // namespace Common::ClockTrace
