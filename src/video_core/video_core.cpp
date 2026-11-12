// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: 2014 Citra Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <iterator>
#include <memory>
#include <string>
#include <utility>

#include "common/clock_trace.h"
#include "common/logging.h"
#include "common/settings.h"
#include "common/spin_wait.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/kernel/k_address_arbiter.h"
#include "core/hle/kernel/k_spin_lock.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/physical_core.h"
#include "video_core/buffer_cache/uniform_stream_reuse.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/host1x/host1x.h"
#include "video_core/nx2_async_readback.h"
#include "video_core/nx2_cmdpage_diag.h"
#include "video_core/nx2_download_trace.h"
#include "video_core/nx2_drain_check.h"
#include "video_core/nx2_entry_check.h"
#include "video_core/nx2_fast_draw.h"
#include "video_core/nx2_list_indirect.h"
#include "video_core/nx2_list_indirect_stats.h"
#include "video_core/nx2_list_stats.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stutter_trace.h"
#include "video_core/nx2_test_switches.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_null/renderer_null.h"
#ifdef HAS_OPENGL
#include "video_core/renderer_opengl/renderer_opengl.h"
#endif
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/video_core.h"

namespace {

std::unique_ptr<VideoCore::RendererBase> CreateRenderer(Core::System& system, Core::Frontend::EmuWindow& emu_window, Tegra::GPU& gpu, std::unique_ptr<Core::Frontend::GraphicsContext> context) {
    [[maybe_unused]] auto& device_memory = system.Host1x().MemoryManager();
    switch (Settings::values.renderer_backend.GetValue()) {
#ifdef HAS_OPENGL
    case Settings::RendererBackend::OpenGL_GLSL:
    case Settings::RendererBackend::OpenGL_GLASM:
    case Settings::RendererBackend::OpenGL_SPIRV:
        return std::make_unique<OpenGL::RendererOpenGL>(emu_window, device_memory, gpu, std::move(context));
#endif
    case Settings::RendererBackend::Vulkan:
        return std::make_unique<Vulkan::RendererVulkan>(emu_window, device_memory, gpu, std::move(context));
    case Settings::RendererBackend::Null:
        return std::make_unique<Null::RendererNull>(emu_window, gpu, std::move(context));
    default:
        return nullptr;
    }
}

/// Every TEST switch, and the other switches the A/B launchers set, as the code applies them, once
/// the renderer latched its own: a launcher whose switch did not take effect shows here, not as a
/// null A/B result.
void LogTestSwitches(const Core::System& system) {
    namespace Test = VideoCommon::Nx2Test;
    namespace FastDraw = VideoCommon::Nx2FastDraw;
    const std::array<std::pair<const char*, bool>, 14> switches{{
        {"EDEN_NX2_TEST_QUICK", Test::QuickWins()},
        {"EDEN_NX2_TEST_CONTIG", Test::ContiguousResolve()},
        {"EDEN_NX2_TEST_1161_DRAWS", FastDraw::g_test_1161_draws.load(std::memory_order_relaxed)},
        {"EDEN_NX2_TEST_1161_HANDLES", Test::Handles1161()},
        {"EDEN_NX2_TEST_BANK_BITS", Test::BankBits()},
        {"EDEN_NX2_TEST_1161_REPORTS", Test::Reports1161()},
        {"EDEN_NX2_TEST_DRAW_TRIMS", Test::DrawTrims()},
        {"EDEN_NX2_TEST_XLATE_CACHE", Test::TranslationCache()},
        {"EDEN_NX2_TEST_1306", Test::Collector1306()},
        {"EDEN_NX2_TEST_1216", FastDraw::g_test_1216.load(std::memory_order_relaxed)},
        {"EDEN_NX2_FOLD_INSTANCES", Tegra::Engines::Maxwell3D::FoldInstancesEnabled()},
        {"EDEN_NX2_ZERO_TRIM", system.DeviceMemory().ZeroTrimRunning()},
        {"EDEN_NX2_KSPIN", Kernel::Nx2Wait::Enabled()},
        {"EDEN_NX2_ARB_SPIN",
         Kernel::ArbSpinMicroseconds() != 0 && system.Kernel().IsMulticore()},
    }};
    std::string line;
    for (const auto& [name, on] : switches) {
        fmt::format_to(std::back_inserter(line), "{}{} {}", line.empty() ? "" : ", ", name,
                       on ? "on" : "off");
    }
    // Option 3's arms (the power-limited CPU), as the code reads them. Whether the pinning plan
    // applies (enough P-cores) is worked out here only when pinning is on.
    const s64 idle_spin = Kernel::IdleSpinSetting();
    const bool pin = Common::PinGpuThreads();
    fmt::format_to(std::back_inserter(line),
                   ", pin_gpu_threads {} ({}{}), LICHII_UMWAIT_C02 {}, UMWAIT {}, "
                   "EDEN_NX2_IDLE_SPIN_US {} ({}), EDEN_NX2_CLOCK_TRACE {}",
                   !pin                              ? "off"
                   : Common::PinGpuThreadsOnly()     ? "GPU threads only"
                                                     : "on",
                   Common::PinGpuThreadsFromEnvironment() ? "EDEN_NX2_PIN_GPU_THREADS" : "setting",
                   !pin                                  ? ""
                   : Common::GpuThreadPinningApplies()   ? ", applied"
                                                         : ", skipped: too few P-cores",
                   // LICHII_UMWAIT_C02 acts only through UMWAIT.
                   !Common::SpinWait::UsesC02()      ? "off"
                   : Common::SpinWait::UsesWaitpkg() ? "on (idle polls in one UMWAIT)"
                                                     : "set, no effect (UMWAIT off)",
                   Common::SpinWait::UsesWaitpkg() ? "on" : "off",
                   idle_spin < 0 ? std::string{"default"} : std::to_string(idle_spin),
                   // Only the multicore idle thread polls (PhysicalCore::Idle).
                   system.Kernel().IsMulticore()
                       ? fmt::format("{} us", Kernel::IdleSpinMicroseconds(system.Kernel()))
                       : std::string{"single core, no idle poll"},
                   Common::ClockTrace::Enabled() ? "on" : "off");
    LOG_INFO(HW_GPU, "TEST switches in effect: {}", line);
}

} // Anonymous namespace

namespace VideoCore {

/// @brief Creates an emulated GPU instance using the given system context.
void CreateGPU(std::optional<Tegra::GPU>& gpu, Core::Frontend::EmuWindow& emu_window, Core::System& system) {
    Settings::UpdateRescalingInfo();
    VideoCommon::Nx2Profile::ReadSwitch();
    VideoCommon::StutterTrace::StartSession();
    Tegra::Nx2CmdPageDiag::ReadSwitch();
    VideoCommon::Nx2DownloadTrace::ReadSwitch();
    VideoCommon::Nx2DrainCheck::ReadSwitch();
    Tegra::Nx2ListStats::ReadSwitches();
    VideoCommon::Nx2AcquireStats::ReadSwitch();
    Tegra::Nx2EntryCheck::ReadSwitch();
    Tegra::Nx2ListIndirect::ReadSwitch();
    Tegra::Nx2ListIndirect::Stats::ReadSwitch();
    VideoCommon::ReadUniformStreamReuseSwitch();
    VideoCore::AsyncReadback::ReadSwitch();
    VideoCommon::Nx2Perf::ReadSwitch();

    const auto nvdec_value = Settings::values.nvdec_emulation.GetValue();
    const bool use_nvdec = nvdec_value != Settings::NvdecEmulation::Off;
    const bool use_async = Settings::values.use_asynchronous_gpu_emulation.GetValue();
    gpu.emplace(system, use_async, use_nvdec);
    auto context = emu_window.CreateSharedContext();
    auto scope = context->Acquire();
    try {
        auto renderer = CreateRenderer(system, emu_window, *gpu, std::move(context));
        gpu->BindRenderer(std::move(renderer));
        LogTestSwitches(system);
    } catch (const std::runtime_error& exception) {
        scope.Cancel();
        LOG_ERROR(HW_GPU, "Failed to initialize GPU: {}", exception.what());
        gpu.reset();
    }
}

} // namespace VideoCore
