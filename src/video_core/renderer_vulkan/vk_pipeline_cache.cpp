// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <thread>
#include <unordered_set>
#include <vector>
#include <bit>
#include <numeric>
#include "common/cityhash.h"
#include "common/scope_exit.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/scm_rev.h"
#include "common/shutdown_progress.h"
#include "common/thread.h"
#include "common/thread_worker.h"
#include "core/core.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/ir_opt/passes.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/translate_profile.h"
#include "video_core/async_translate.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_precompile.h"
#include "video_core/nx2_profile.h"
#include "video_core/nx2_stutter_trace.h"
#include "video_core/pipeline_cache_files.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/async_draw_policy.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/mesh_fallback_limits.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/pipeline_shader_emission.h"
#include "video_core/renderer_vulkan/precache_journal.h"
#include "video_core/renderer_vulkan/pipeline_statistics.h"
#include "video_core/renderer_vulkan/vertex_repack.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_cooperative_hmma_test.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_binaries.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_records.h"
#include "video_core/renderer_vulkan/vk_pipeline_translate_queue.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_cache_v2.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_cache_v2/cache_file.h"
#include "video_core/shader_cache_v2/records.h"
#include "video_core/shader_environment.h"
#include "video_core/shader_notify.h"
#include "video_core/shader_warm/shader_warm_builder.h"
#include "video_core/surface.h"
#include "video_core/texture_cache/formatter.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/gpu_logging/gpu_logging.h"

#ifdef __ANDROID__
#include "../../android/app/src/main/jni/android_settings.h"
#endif

namespace Vulkan {

namespace {
using Shader::Backend::SPIRV::EmitSPIRV;
using Shader::Maxwell::ConvertLegacyToGeneric;
using Shader::Maxwell::GenerateGeometryPassthrough;
using Shader::Maxwell::MergeDualVertexPrograms;
using Shader::Maxwell::TranslateProgram;
using VideoCommon::ComputeEnvironment;
using VideoCommon::FileEnvironment;
using VideoCommon::GenericEnvironment;
using VideoCommon::GuessMissing;
using VideoCommon::GraphicsEnvironment;

constexpr u32 CACHE_VERSION = VideoCommon::VulkanPipelineCacheVersion;
// The driver's pipeline cache is saved every 32 new pipelines, at most every 10 s, and the used
// list every 5 s (from medicinauvsecretaria-lang's Lichii OoT AMD r1, 7d96f498f): an AMD driver
// crash lost up to 128 pipelines and 30 s of the list before.
constexpr size_t VULKAN_CACHE_FLUSH_PIPELINES = 32;
constexpr size_t VULKAN_CACHE_FLUSH_MIN_SECONDS = 10;
constexpr auto& VULKAN_CACHE_MAGIC_NUMBER = VideoCommon::VulkanDriverPipelineCacheMagic;

template <typename Container>
auto MakeSpan(Container& container) {
    return std::span(container.data(), container.size());
}

Shader::OutputTopology MaxwellToOutputTopology(Maxwell::PrimitiveTopology topology) {
    switch (topology) {
    case Maxwell::PrimitiveTopology::Points:
        return Shader::OutputTopology::PointList;
    case Maxwell::PrimitiveTopology::LineStrip:
        return Shader::OutputTopology::LineStrip;
    default:
        return Shader::OutputTopology::TriangleStrip;
    }
}

Shader::CompareFunction MaxwellToCompareFunction(Maxwell::ComparisonOp comparison) {
    switch (comparison) {
    case Maxwell::ComparisonOp::Never_D3D:
    case Maxwell::ComparisonOp::Never_GL:
        return Shader::CompareFunction::Never;
    case Maxwell::ComparisonOp::Less_D3D:
    case Maxwell::ComparisonOp::Less_GL:
        return Shader::CompareFunction::Less;
    case Maxwell::ComparisonOp::Equal_D3D:
    case Maxwell::ComparisonOp::Equal_GL:
        return Shader::CompareFunction::Equal;
    case Maxwell::ComparisonOp::LessEqual_D3D:
    case Maxwell::ComparisonOp::LessEqual_GL:
        return Shader::CompareFunction::LessThanEqual;
    case Maxwell::ComparisonOp::Greater_D3D:
    case Maxwell::ComparisonOp::Greater_GL:
        return Shader::CompareFunction::Greater;
    case Maxwell::ComparisonOp::NotEqual_D3D:
    case Maxwell::ComparisonOp::NotEqual_GL:
        return Shader::CompareFunction::NotEqual;
    case Maxwell::ComparisonOp::GreaterEqual_D3D:
    case Maxwell::ComparisonOp::GreaterEqual_GL:
        return Shader::CompareFunction::GreaterThanEqual;
    case Maxwell::ComparisonOp::Always_D3D:
    case Maxwell::ComparisonOp::Always_GL:
        return Shader::CompareFunction::Always;
    }
    UNIMPLEMENTED_MSG("Unimplemented comparison op={}", comparison);
    return {};
}

Shader::AttributeType CastAttributeType(const FixedPipelineState::VertexAttribute& attr) {
    if (attr.enabled == 0) {
        return Shader::AttributeType::Disabled;
    }
    switch (attr.Type()) {
    case Maxwell::VertexAttribute::Type::UnusedEnumDoNotUseBecauseItWillGoAway:
        ASSERT_MSG(false, "Invalid vertex attribute type!");
        return Shader::AttributeType::Disabled;
    case Maxwell::VertexAttribute::Type::SNorm:
    case Maxwell::VertexAttribute::Type::UNorm:
    case Maxwell::VertexAttribute::Type::Float:
        return Shader::AttributeType::Float;
    case Maxwell::VertexAttribute::Type::SInt:
        return Shader::AttributeType::SignedInt;
    case Maxwell::VertexAttribute::Type::UInt:
        return Shader::AttributeType::UnsignedInt;
    case Maxwell::VertexAttribute::Type::UScaled:
        return Shader::AttributeType::UnsignedScaled;
    case Maxwell::VertexAttribute::Type::SScaled:
        return Shader::AttributeType::SignedScaled;
    }
    return Shader::AttributeType::Float;
}

Shader::AttributeType AttributeType(const FixedPipelineState& state, size_t index) {
    switch (state.DynamicAttributeType(index)) {
    case 0:
        return Shader::AttributeType::Disabled;
    case 1:
        return Shader::AttributeType::Float;
    case 2:
        return Shader::AttributeType::SignedInt;
    case 3:
        return Shader::AttributeType::UnsignedInt;
    }
    return Shader::AttributeType::Disabled;
}

Shader::RuntimeInfo MakeRuntimeInfo(std::span<const Shader::IR::Program> programs,
                                    const GraphicsPipelineCacheKey& key,
                                    const Shader::IR::Program& program,
                                    const Shader::IR::Program* previous_program,
                                    const Vulkan::Device& device) {
    Shader::RuntimeInfo info;
    if (previous_program) {
        info.previous_stage_stores = previous_program->info.stores;
        info.previous_stage_legacy_stores_mapping = previous_program->info.legacy_stores_mapping;
        if (previous_program->is_geometry_passthrough) {
            info.previous_stage_stores.mask |= previous_program->info.passthrough.mask;
        }
    } else {
        info.previous_stage_stores.mask.set();
    }
    if (previous_program && previous_program->stage == Shader::Stage::Mesh) {
        for (const u32 line : previous_program->mesh.primitive_lines) {
            const auto attribute{static_cast<Shader::IR::Attribute>(line)};
            if (Shader::IR::IsGeneric(attribute)) {
                info.per_primitive_generics.set(Shader::IR::GenericAttributeIndex(attribute));
            }
        }
    }
    const Shader::Stage stage{program.stage};
    const bool has_geometry{key.unique_hashes[4] != 0 && !programs[4].is_geometry_passthrough};
    const bool gl_ndc{key.state.ndc_minus_one_to_one != 0};
    const float point_size{std::bit_cast<float>(key.state.point_size)};
    switch (stage) {
    case Shader::Stage::VertexB:
        if (!has_geometry) {
            if (key.state.topology == Maxwell::PrimitiveTopology::Points) {
                info.fixed_state_point_size = point_size;
            }
            if (key.state.xfb_enabled) {
                if (device.IsExtTransformFeedbackSupported()) {
                    auto [varyings, count] =
                        VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                    info.xfb_varyings = varyings;
                    info.xfb_count = count;
                } else {
                    LOG_WARNING(Render_Vulkan, "XFB requested in pipeline key but device lacks VK_EXT_transform_feedback; ignoring XFB decorations");
                }
            }
            info.convert_depth_mode = gl_ndc;
        }
        if (key.state.dynamic_vertex_input) {
            for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
                info.generic_input_types[index] = AttributeType(key.state, index);
            }
        } else {
            std::ranges::transform(key.state.attributes, info.generic_input_types.begin(),
                                   &CastAttributeType);
        }
        break;
    case Shader::Stage::TessellationEval:
        info.tess_clockwise = key.state.tessellation_clockwise != 0;
        info.tess_primitive = [&key] {
            const u32 raw{key.state.tessellation_primitive.Value()};
            switch (static_cast<Maxwell::Tessellation::DomainType>(raw)) {
            case Maxwell::Tessellation::DomainType::Isolines:
                return Shader::TessPrimitive::Isolines;
            case Maxwell::Tessellation::DomainType::Triangles:
                return Shader::TessPrimitive::Triangles;
            case Maxwell::Tessellation::DomainType::Quads:
                return Shader::TessPrimitive::Quads;
            }
            ASSERT(false);
            return Shader::TessPrimitive::Triangles;
        }();
        info.tess_spacing = [&] {
            const u32 raw{key.state.tessellation_spacing};
            switch (static_cast<Maxwell::Tessellation::Spacing>(raw)) {
            case Maxwell::Tessellation::Spacing::Integer:
                return Shader::TessSpacing::Equal;
            case Maxwell::Tessellation::Spacing::FractionalOdd:
                return Shader::TessSpacing::FractionalOdd;
            case Maxwell::Tessellation::Spacing::FractionalEven:
                return Shader::TessSpacing::FractionalEven;
            }
            ASSERT(false);
            return Shader::TessSpacing::Equal;
        }();
        break;
    case Shader::Stage::Mesh:
        info.convert_depth_mode = gl_ndc;
        if (program.mesh.topology == 0) {
            info.fixed_state_point_size = point_size;
        }
        break;
    case Shader::Stage::Geometry:
        if (program.output_topology == Shader::OutputTopology::PointList) {
            info.fixed_state_point_size = point_size;
        }
        if (key.state.xfb_enabled != 0) {
            if (device.IsExtTransformFeedbackSupported()) {
                auto [varyings, count] =
                    VideoCommon::MakeTransformFeedbackVaryings(key.state.xfb_state);
                info.xfb_varyings = varyings;
                info.xfb_count = count;
            } else {
                LOG_WARNING(Render_Vulkan, "XFB requested in pipeline key but device lacks VK_EXT_transform_feedback; ignoring XFB decorations");
            }
        }
        info.convert_depth_mode = gl_ndc;
        break;
    case Shader::Stage::Fragment: {
        info.alpha_test_func = MaxwellToCompareFunction(
            key.state.UnpackComparisonOp(key.state.alpha_test_func.Value()));
        info.alpha_test_reference = std::bit_cast<float>(key.state.alpha_test_ref);

        info.dual_source_blend = key.state.attachment0_dual_source_blend != 0;

        if (device.IsMoltenVK()) {
            for (size_t i = 0; i < 8; ++i) {
                const auto format = static_cast<Tegra::RenderTargetFormat>(key.state.color_formats[i]);
                if (format == Tegra::RenderTargetFormat::NONE) {
                    info.color_output_types[i] = Shader::AttributeType::Float;
                    continue;
                }
                const auto pixel_format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(format);
                if (VideoCore::Surface::IsPixelFormatInteger(pixel_format)) {
                    if (VideoCore::Surface::IsPixelFormatSignedInteger(pixel_format)) {
                        info.color_output_types[i] = Shader::AttributeType::SignedInt;
                    } else {
                        info.color_output_types[i] = Shader::AttributeType::UnsignedInt;
                    }
                } else {
                    info.color_output_types[i] = Shader::AttributeType::Float;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    switch (key.state.topology) {
    case Maxwell::PrimitiveTopology::Points:
        info.input_topology = Shader::InputTopology::Points;
        break;
    case Maxwell::PrimitiveTopology::Lines:
    case Maxwell::PrimitiveTopology::LineLoop:
    case Maxwell::PrimitiveTopology::LineStrip:
        info.input_topology = Shader::InputTopology::Lines;
        break;
    case Maxwell::PrimitiveTopology::Triangles:
    case Maxwell::PrimitiveTopology::TriangleStrip:
    case Maxwell::PrimitiveTopology::TriangleFan:
    case Maxwell::PrimitiveTopology::Quads:
    case Maxwell::PrimitiveTopology::QuadStrip:
    case Maxwell::PrimitiveTopology::Polygon:
    case Maxwell::PrimitiveTopology::Patches:
        info.input_topology = Shader::InputTopology::Triangles;
        break;
    case Maxwell::PrimitiveTopology::LinesAdjacency:
    case Maxwell::PrimitiveTopology::LineStripAdjacency:
        info.input_topology = Shader::InputTopology::LinesAdjacency;
        break;
    case Maxwell::PrimitiveTopology::TrianglesAdjacency:
    case Maxwell::PrimitiveTopology::TriangleStripAdjacency:
        info.input_topology = Shader::InputTopology::TrianglesAdjacency;
        break;
    }
    info.force_early_z = key.state.early_z != 0;
    info.y_negate = key.state.y_negate != 0;
    return info;
}

/// SPIR-V emission of run-time pipelines: graphics on the GPU thread, graphics on workers and
/// compute on workers (shader cache v2)
std::array<std::atomic<std::chrono::nanoseconds::rep>, 3> runtime_spirv_ns{};
/// Set while a translation thread builds (EDEN_NX2_ASYNC_TRANSLATE).
thread_local bool t_translate_worker{};

void AddRuntimeSpirvTime(size_t thread, std::chrono::steady_clock::time_point begin) {
    if (t_translate_worker) {
        // SPIR-V a translation thread emits itself is the workers' time, not the GPU thread's.
        thread = 1;
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    runtime_spirv_ns[thread].fetch_add(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count(),
        std::memory_order::relaxed);
}

// The GPU thread's split of its own runtime graphics builds: environments (GetGraphicsEnvironments,
// a native program's first lowering included) and translation (control flow, TranslateProgram and
// the IR passes, with the environment's lazy reads). Written and read on the GPU thread only.
struct RuntimeBuildSplit {
    std::chrono::steady_clock::duration environments{};
    std::chrono::steady_clock::duration translation{};
};
thread_local bool t_runtime_build{};
thread_local RuntimeBuildSplit t_build_split{};

// What the GPU thread spent on pipelines in the current frame, for the worst frame line
// (ReportWorstFrame). GPU thread only.
struct FrameTally {
    std::chrono::steady_clock::duration builds{};
    RuntimeBuildSplit split{};
    u32 count{};
    /// EDEN_NX2_ASYNC_TRANSLATE: translations queued (their environments and snapshots).
    std::chrono::steady_clock::duration queued_time{};
    u32 queued{};
};
FrameTally frame_tally;
/// Pipeline lookups skipped for unbuilt pipelines this session (BuiltPipeline), and for
/// pipelines still translating (EDEN_NX2_ASYNC_TRANSLATE).
std::atomic<u64> skipped_lookups{};

void NoteSkippedLookup() {
    if ((skipped_lookups.fetch_add(1, std::memory_order_relaxed) & 255) == 0) {
        // Counts lookups, not draws: a draw state can look its pipeline up more than once.
        LOG_INFO(Render_Vulkan, "Async shaders: {} pipeline lookups skipped for unbuilt pipelines",
                 skipped_lookups.load(std::memory_order_relaxed));
    }
}

/// EDEN_NX2_ASYNC_TRANSLATE: translations in flight at most (each holds its constant buffer
/// copies until translated), how long a draw that must not be skipped waits for one before
/// building its pipeline itself, unusable translations of a key before it is built on the GPU
/// thread for good, and the age at which a translation is taken for wedged.
constexpr size_t MaxTranslationsInFlight = 64;
constexpr std::chrono::milliseconds TranslateWaitCap{50};
constexpr u32 MaxTranslateFailures = 2;
constexpr std::chrono::seconds TranslateSweepAge{5};

/// EDEN_NX2_ASYNC_TRANSLATE_THREADS: translation threads (2 by default, 1 to 8).
size_t TranslateThreads() {
    const char* const value = std::getenv("EDEN_NX2_ASYNC_TRANSLATE_THREADS");
    const int threads = value != nullptr ? std::atoi(value) : 2;
    return static_cast<size_t>(std::clamp(threads, 1, 8));
}

/// "pipeline=<key hash> s<stage>:<shader hash> ...", as the runtime pipeline lines print them
std::string PipelineShaders(const GraphicsPipelineCacheKey& key) {
    std::string shaders = fmt::format("pipeline={:016x}", key.Hash());
    for (size_t stage = 0; stage < key.unique_hashes.size(); ++stage) {
        if (key.unique_hashes[stage] != 0) {
            shaders += fmt::format(" s{}:{:016x}", stage, key.unique_hashes[stage]);
        }
    }
    return shaders;
}

/// EDEN_NX2_SPIRV_SELFCHECK=1: throws (the pipeline is dropped, as one the translation rejects)
/// when the emitted SPIR-V uses an id it never defines or its own result, before the driver gets
/// it. Off: nothing.
void SelfCheckSpirv(std::span<const u32> code, u64 shader_hash, const char* stage) {
    if (!SpirvSelfCheck()) {
        return;
    }
    if (std::string problem = FindSpirvIdProblem(code); !problem.empty()) {
        LOG_ERROR(Render_Vulkan,
                  "SPIR-V self-check (EDEN_NX2_SPIRV_SELFCHECK=1): shader {:016x} {}: {}; its "
                  "pipeline is dropped instead of handed to the driver",
                  shader_hash, stage, problem);
        throw Shader::LogicError("SPIR-V self-check failed for shader {:016x} {}", shader_hash,
                                 stage);
    }
}

/// The shader module of a stage from its finished SPIR-V.
vk::ShaderModule BuildStageModule(const Device& device, std::span<const u32> code,
                                  u64 shader_hash, size_t stage_index) {
    static constexpr std::array stage_names{"vertex", "tess_control", "tess_eval", "geometry",
                                           "fragment"};
    SelfCheckSpirv(code, shader_hash, stage_names[stage_index]);
    device.SaveShader(code);
    vk::ShaderModule module{BuildShader(device, code)};

    // Text log + .spv dump. Text log is gated by gpu_log_level != Off; .spv dump
    // is independent and gated only by gpu_log_shader_dumps.
    const bool should_log = GPU::Logging::IsActive();
    const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
    if (should_log || should_dump) {
        const std::string shader_name =
            fmt::format("shader_{:016x}_{}", shader_hash, stage_names[stage_index]);
        if (should_log) {
            const std::string shader_info = fmt::format(
                "SPIR-V size: {} bytes, hash: {:016x}", code.size() * sizeof(u32), shader_hash);
            GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
        }
        if (should_dump) {
            GPU::Logging::DumpSpirvShader(shader_hash,
                                         std::span<const u32>(code.data(), code.size()));
        }
    }

    if (device.HasDebuggingToolAttached()) {
        const std::string name{fmt::format("Shader {:016x}", shader_hash)};
        module.SetObjectNameEXT(name.c_str());
    }
    return module;
}

vk::ShaderModule EmitShaderModule(const Device& device, const Shader::Profile& profile,
                                 const Shader::RuntimeInfo& runtime_info,
                                 Shader::IR::Program& program, Shader::Backend::Bindings& binding,
                                 u64 shader_hash, size_t stage_index,
                                 std::span<const u32> reference = {}) {
    const auto code =
        EmitPipelineShader(profile, runtime_info, program, binding, shader_hash, reference);
    return BuildStageModule(device, code, shader_hash, stage_index);
}

vk::ShaderModule EmitComputeModule(const Device& device, const Shader::Profile& profile,
                                   Shader::IR::Program& program, u64 shader_hash,
                                   std::span<const u32> reference = {}) {
    const std::vector<u32> code{EmitComputeShader(profile, program, shader_hash, reference)};
    SelfCheckSpirv(code, shader_hash, "compute");
    device.SaveShader(code);
    vk::ShaderModule spv_module{BuildShader(device, code)};

    // Text log + .spv dump. Same split as the graphics path.
    const bool should_log = GPU::Logging::IsActive();
    const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
    if (should_log || should_dump) {
        const std::string shader_name = fmt::format("shader_{:016x}_compute", shader_hash);
        if (should_log) {
            const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                code.size() * sizeof(u32), shader_hash);
            GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
        }
        if (should_dump) {
            GPU::Logging::DumpSpirvShader(shader_hash,
                                          std::span<const u32>(code.data(), code.size()));
        }
    }

    if (device.HasDebuggingToolAttached()) {
        const auto name{fmt::format("Shader {:016x}", shader_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    return spv_module;
}

/// A compute program whose SPIR-V the CPU stage of its pipeline's job emits (shader cache v2)
struct DeferredCompute {
    std::unique_ptr<QueuedIrBudget::Reservation> reservation;
    std::shared_ptr<ShaderPools> pools;
    Shader::IR::Program program;
    std::vector<u32> reference_spirv;
};

struct DeferredStages {
    std::unique_ptr<QueuedIrBudget::Reservation> reservation;
    std::shared_ptr<ShaderPools> pools;
    /// Pools of the stages translated on workers (ParallelTranslation)
    std::vector<std::shared_ptr<ShaderPools>> stage_pools;
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    std::array<std::optional<Shader::RuntimeInfo>, Maxwell::MaxShaderStage> runtime_infos;
    std::array<std::vector<u32>, Maxwell::MaxShaderStage> reference_spirv;
    /// SPIR-V emitted already (EDEN_NX2_ASYNC_TRANSLATE=2 records it on the translating thread):
    /// such a stage is built from these words, never emitted from its program a second time.
    std::array<std::vector<u32>, Maxwell::MaxShaderStage> emitted_spirv;
};

size_t GetTotalPipelineWorkers() {
    const size_t max_core_threads =
        std::max<size_t>(static_cast<size_t>(std::thread::hardware_concurrency()), 2ULL) - 1ULL;
#ifdef __ANDROID__
    const int configured = AndroidSettings::values.pipeline_worker_count.GetValue();
    const int clamped = std::clamp(configured, 2, 8);
    const size_t desired = static_cast<size_t>(clamped);
    if (desired == 0) {
        return 1ULL;
    }
    return std::min(max_core_threads, desired);
#else
    return max_core_threads;
#endif
}

} // Anonymous namespace

size_t ComputePipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), sizeof *this);
    return static_cast<size_t>(hash);
}

bool ComputePipelineCacheKey::operator==(const ComputePipelineCacheKey& rhs) const noexcept {
    return std::memcmp(&rhs, this, sizeof *this) == 0;
}

size_t GraphicsPipelineCacheKey::Hash() const noexcept {
    const u64 hash = Common::CityHash64(reinterpret_cast<const char*>(this), Size());
    return static_cast<size_t>(hash);
}

bool GraphicsPipelineCacheKey::operator==(const GraphicsPipelineCacheKey& rhs) const noexcept {
    if (unique_hashes != rhs.unique_hashes) {
        return false;
    }
    return std::memcmp(reinterpret_cast<const char*>(this) + sizeof(unique_hashes),
                       reinterpret_cast<const char*>(&rhs) + sizeof(unique_hashes),
                       state.Size()) == 0;
}

PipelineCache::PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_,
                             const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             GuestDescriptorQueue& guest_descriptor_queue_,
                             DescriptorBufferRing& descriptor_buffer_ring_,
                             RenderPassCache& render_pass_cache_, BufferCache& buffer_cache_,
                             TextureCache& texture_cache_, VideoCore::ShaderNotify& shader_notify_)
    : VideoCommon::ShaderCache{device_memory_}, device{device_}, scheduler{scheduler_},
      descriptor_pool{descriptor_pool_}, guest_descriptor_queue{guest_descriptor_queue_},
      descriptor_buffer_ring{descriptor_buffer_ring_}, render_pass_cache{render_pass_cache_},
      buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, shader_notify{shader_notify_},
      use_asynchronous_shaders{Settings::values.use_asynchronous_shaders.GetValue()},
      use_vulkan_pipeline_cache{Settings::values.use_vulkan_driver_pipeline_cache.GetValue() &&
                                !device_.DriverCachesPipelines()},
      serialization_thread(1, "VkPipelineSerialization", {}, Common::ThreadPlacement::Background) {
    const auto& float_control{device.FloatControlProperties()};
    const VkDriverId driver_id{device.GetDriverID()};
    const VkShaderStageFlags subgroup_stages{device.GetSubgroupSupportedStages()};
    const auto subgroup_stage_bit{[subgroup_stages](VkShaderStageFlags flag, Shader::Stage stage) {
        return (subgroup_stages & flag) != 0 ? (1u << static_cast<u32>(stage)) : 0u;
    }};
    u32 supported_subgroup_stages{
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexA) |
        subgroup_stage_bit(VK_SHADER_STAGE_VERTEX_BIT, Shader::Stage::VertexB) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                           Shader::Stage::TessellationControl) |
        subgroup_stage_bit(VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                           Shader::Stage::TessellationEval) |
        subgroup_stage_bit(VK_SHADER_STAGE_GEOMETRY_BIT, Shader::Stage::Geometry) |
        subgroup_stage_bit(VK_SHADER_STAGE_FRAGMENT_BIT, Shader::Stage::Fragment) |
        subgroup_stage_bit(VK_SHADER_STAGE_COMPUTE_BIT, Shader::Stage::Compute) |
        subgroup_stage_bit(VK_SHADER_STAGE_TASK_BIT_EXT, Shader::Stage::Task) |
        subgroup_stage_bit(VK_SHADER_STAGE_MESH_BIT_EXT, Shader::Stage::Mesh)};
    if (driver_id == VK_DRIVER_ID_MESA_TURNIP) {
        supported_subgroup_stages &= ~(1u << static_cast<u32>(Shader::Stage::Geometry));
    }
    profile = Shader::Profile{
        .supported_spirv = device.SupportedSpirvVersion(),
        .unified_descriptor_binding = true,
        .support_descriptor_aliasing = device.IsDescriptorAliasingSupported(),
        .support_int8 = device.IsInt8Supported(),
        .support_uniform_and_storage_buffer_8bit =
            device.IsUniformAndStorageBuffer8BitAccessSupported(),
        .support_storage_buffer_8bit = device.IsStorageBuffer8BitAccessSupported(),
        .support_int16 = device.IsShaderInt16Supported(),
        .support_uniform_and_storage_buffer_16bit =
            device.IsUniformAndStorageBuffer16BitAccessSupported(),
        .support_storage_buffer_16bit = device.IsStorageBuffer16BitAccessSupported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .support_vertex_instance_id = false,
        .support_float_controls = device.IsKhrShaderFloatControlsSupported(),
        .support_separate_denorm_behavior =
            float_control.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_separate_rounding_mode =
            float_control.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL,
        .support_fp16_denorm_preserve = float_control.shaderDenormPreserveFloat16 != VK_FALSE,
        .support_fp32_denorm_preserve = float_control.shaderDenormPreserveFloat32 != VK_FALSE,
        .support_fp16_denorm_flush = float_control.shaderDenormFlushToZeroFloat16 != VK_FALSE,
        .support_fp32_denorm_flush = float_control.shaderDenormFlushToZeroFloat32 != VK_FALSE,
        .support_fp16_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat16 != VK_FALSE,
        .support_fp32_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat32 != VK_FALSE,
        .support_fp64_signed_zero_nan_preserve =
            float_control.shaderSignedZeroInfNanPreserveFloat64 != VK_FALSE,
        .support_explicit_workgroup_layout = device.IsKhrWorkgroupMemoryExplicitLayoutSupported(),
        .support_workgroup_layout_8bit_access =
            device.IsWorkgroupMemoryExplicitLayout8BitAccessSupported(),
        .support_workgroup_layout_16bit_access =
            device.IsWorkgroupMemoryExplicitLayout16BitAccessSupported(),
        .support_shader_quad_control = device.IsKhrShaderQuadControlSupported(),
        .support_quad_shuffles = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_QUAD_BIT),
        .support_vote = device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_VOTE_BIT),
        .support_shuffle_relative =
            device.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT),
        .supported_subgroup_stages = supported_subgroup_stages,
        .support_viewport_index_layer_non_geometry =
            device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_typeless_image_loads = device.IsFormatlessImageLoadSupported(),
        .support_demote_to_helper_invocation =
            device.IsExtShaderDemoteToHelperInvocationSupported(),
        .support_int64_atomics = device.IsExtShaderAtomicInt64Supported(),
        .support_shared_int64_atomics = device.IsSharedInt64AtomicsSupported(),
        .support_derivative_control = true,
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_native_ndc = device.IsExtDepthClipControlSupported(),
        .support_scaled_attributes = !device.MustEmulateScaledFormats(),
        .support_multi_viewport = device.SupportsMultiViewport(),
        .support_geometry_streams = device.AreTransformFeedbackGeometryStreamsSupported(),
        .support_sampled_image_array_nonuniform_indexing =
            device.IsSampledImageArrayNonUniformIndexingSupported(),
        .support_storage_image_array_nonuniform_indexing =
            device.IsStorageImageArrayNonUniformIndexingSupported(),
        .support_uniform_texel_buffer_array_nonuniform_indexing =
            device.IsUniformTexelBufferArrayNonUniformIndexingSupported(),
        .support_storage_texel_buffer_array_nonuniform_indexing =
            device.IsStorageTexelBufferArrayNonUniformIndexingSupported(),

        .warp_size_potentially_larger_than_guest = device.IsWarpSizePotentiallyBiggerThanGuest(),

        .lower_left_origin_mode = false,
        .need_declared_frag_colors = false,
        .need_gather_subpixel_offset = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                       driver_id == VK_DRIVER_ID_MESA_RADV ||
                                       driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS ||
                                       driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA ||
                                       driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                       driver_id == VK_DRIVER_ID_MESA_TURNIP,

        .has_broken_spirv_clamp = driver_id == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS,
        .has_broken_spirv_position_input = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_unsigned_image_offsets = false,
        .has_broken_signed_operations = false,
        .has_broken_fp16_float_controls = driver_id == VK_DRIVER_ID_NVIDIA_PROPRIETARY,
        .max_compute_shared_memory_size = device.GetMaxComputeSharedMemorySize(),
        .has_broken_fp32_denorm_flush = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .ignore_nan_fp_comparisons = false,
        .has_broken_spirv_subgroup_mask_vector_extract_dynamic =
            driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY &&
            device.GetDriverVersion() < VK_MAKE_VERSION(512, 672, 0),
        .has_broken_spirv_subgroup_shuffle = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY,
        .has_broken_spirv_fragment_shuffle = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY,
        .max_subgroup_size = device.GetMaxSubgroupSize(),
        .has_broken_robust =
            device.IsNvidia() && device.GetNvidiaArch() <= NvidiaArchitecture::Arch_Pascal,
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_user_clip_distances = device.GetMaxUserClipDistances()
    };
    profile.support_cooperative_hmma =
        device.IsCooperativeHmmaSupported() && TestCooperativeHmma(device);
    profile.support_cooperative_hmma_16x16x16 =
        !profile.support_cooperative_hmma && device.IsCooperativeHmma16x16x16Supported();
    if (profile.support_cooperative_hmma_16x16x16) {
        LOG_INFO(Render_Vulkan, "Guest tensor operations run on 16x16x16 cooperative matrices");
    }

    host_info = Shader::HostTranslateInfo{
        .min_ssbo_alignment = device.GetStorageBufferAlignment(),
        .max_per_stage_descriptor_sampled_images = device.GetMaxPerStageDescriptorSampledImages(),
        .max_per_stage_resources = device.GetMaxPerStageResources(),
        .max_per_stage_descriptor_samplers = device.GetMaxPerStageDescriptorSamplers(),
        .max_per_stage_descriptor_storage_buffers = device.GetMaxPerStageDescriptorStorageBuffers(),
        .max_descriptor_set_samplers = device.GetMaxDescriptorSetSamplers(),
        .max_descriptor_set_uniform_buffers = device.GetMaxDescriptorSetUniformBuffers(),
        .max_descriptor_set_uniform_buffers_dynamic = device.GetMaxDescriptorSetUniformBuffersDynamic(),
        .max_descriptor_set_storage_buffers = device.GetMaxDescriptorSetStorageBuffers(),
        .max_descriptor_set_storage_buffers_dynamic = device.GetMaxDescriptorSetStorageBuffersDynamic(),
        .max_descriptor_set_sampled_images = device.GetMaxDescriptorSetSampledImages(),
        .max_descriptor_set_storage_images = device.GetMaxDescriptorSetStorageImages(),
        .max_descriptor_set_input_attachements = device.GetMaxDescriptorSetInputAttachments(),
        .support_float64 = device.IsFloat64Supported(),
        .support_float16 = device.IsFloat16Supported(),
        .support_int64 = device.IsShaderInt64Supported(),
        .needs_demote_reorder = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                                driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                                driver_id == VK_DRIVER_ID_SAMSUNG_PROPRIETARY,
        .support_snorm_render_buffer = true,
        .support_viewport_index_layer = device.IsExtShaderViewportIndexLayerSupported(),
        .support_viewport_mask = device.IsNvViewportArray2Supported(),
        .support_geometry_shader_passthrough = device.IsNvGeometryShaderPassthroughSupported(),
        .support_conditional_barrier = device.SupportsConditionalBarriers(),
        .single_lane_geometry_subgroups = !profile.SupportsSubgroupStage(Shader::Stage::Geometry),
        .support_byte_addressed_r16 = true,
        // The arrays are indexed per invocation, which needs nonuniform indexing to be defined.
        .support_handle_table_arrays = device.IsSampledImageArrayNonUniformIndexingSupported(),
    };
    host_info.ApplyDescriptorLimitPolicy();

    {
        // The descriptor arrays of bindless handle tables (HandleTableSize in texture_pass.cpp).
        const char* const table = std::getenv("LICHII_BINDLESS_TABLE");
        LOG_INFO(Render_Vulkan,
                 "Bindless handle tables: LICHII_BINDLESS_TABLE {}, sampled image arrays "
                 "indexed per invocation {}, up to {} sampled images per stage",
                 table != nullptr && table[0] != '\0' ? table : "default (64)",
                 device.IsSampledImageArrayNonUniformIndexingSupported() ? "supported"
                                                                         : "unsupported",
                 device.GetMaxPerStageDescriptorSampledImages());
    }

    if (device.GetMaxVertexInputAttributes() < Maxwell::NumVertexAttributes) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputAttributes is too low: {} < {}",
                    device.GetMaxVertexInputAttributes(), Maxwell::NumVertexAttributes);
    }
    if (device.GetMaxVertexInputBindings() < Maxwell::NumVertexArrays) {
        LOG_WARNING(Render_Vulkan, "maxVertexInputBindings is too low: {} < {}",
                    device.GetMaxVertexInputBindings(), Maxwell::NumVertexArrays);
    }

    LOG_INFO(Render_Vulkan, "DynamicState setting value: {}", u32(Settings::values.dyna_state.GetValue()));

    dynamic_features = {};
    // Keys without the driver, so that a driver update keeps the recorded pipelines
    dynamic_features.driver_id = 0;
    dynamic_features.driver_version = 0;

    // User granularity enforced in vulkan_device.cpp switch statement:
    //   Level 0: Core Dynamic States only
    //   Level 1: Core + EDS1
    //   Level 2: Core + EDS1 + EDS2 (accumulative)
    //   Level 3: Core + EDS1 + EDS2 + EDS3 (accumulative)
    // Here we only verify if extensions were successfully loaded by the device

    dynamic_features.has_extended_dynamic_state =
        device.IsExtExtendedDynamicStateSupported();

    dynamic_features.has_extended_dynamic_state_2 =
        device.IsExtExtendedDynamicState2Supported();
    dynamic_features.has_extended_dynamic_state_2_logic_op =
        device.IsExtExtendedDynamicState2ExtrasSupported();
    dynamic_features.has_extended_dynamic_state_2_patch_control_points = false;

    dynamic_features.has_extended_dynamic_state_3_blend =
        device.IsExtExtendedDynamicState3BlendingSupported();
    dynamic_features.has_extended_dynamic_state_3_enables =
        device.IsExtExtendedDynamicState3EnablesSupported();
    dynamic_features.has_color_write_enable =
        device.IsExtColorWriteEnableSupported();
    dynamic_features.has_dynamic_state3_depth_clamp_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3DepthClampEnable();
    dynamic_features.has_dynamic_state3_logic_op_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LogicOpEnable();
    dynamic_features.has_dynamic_state3_line_stipple_enable =
        dynamic_features.has_extended_dynamic_state_3_enables &&
        device.SupportsDynamicState3LineStippleEnable();

    // VIDS: Independent toggle (not affected by dyna_state levels)
    dynamic_features.has_dynamic_vertex_input =
        device.IsExtVertexInputDynamicStateSupported() &&
        Settings::values.vertex_input_dynamic_state.GetValue();
    dynamic_features.has_static_vertex_repack =
        StaticVertexRepackEnabled(device.GetMinVertexInputBindingStrideAlignment());

    dynamic_features.has_provoking_vertex = device.IsExtProvokingVertexSupported();
    dynamic_features.has_provoking_vertex_first_mode =
        device.SupportsProvokingVertexFirstMode();
    dynamic_features.has_provoking_vertex_last_mode =
        device.SupportsProvokingVertexLastMode();
    dynamic_features.has_provoking_vertex_tf_preserve =
        device.SupportsTransformFeedbackProvokingVertexPreservation();

    const size_t cpu_threads = GetTotalPipelineWorkers();
    const size_t driver_threads =
        device.HasBrokenParallelShaderCompiling()
            ? 1
            : std::min(cpu_threads, std::max<size_t>(std::thread::hardware_concurrency() / 2, 2));
    shader_jobs.emplace(VideoCommon::ShaderJobConfig{
        .cpu_threads = cpu_threads,
        .driver_threads = driver_threads,
        .name = "VkShader",
    });
    // EDEN_NX2_ASYNC_TRANSLATE=1/2: new pipelines of draws that may be skipped are translated on
    // threads of their own. It relies on the asynchronous shaders' draw skipping.
    if (VideoCommon::AsyncTranslateWorkers()) {
        if (use_asynchronous_shaders) {
            const size_t threads = TranslateThreads();
            translate_queue = std::make_unique<TranslateQueue>(
                threads, MaxTranslationsInFlight,
                [this](TranslationJob& job) { TranslateOnWorker(job); },
                [this](std::unique_ptr<GraphicsPipeline> pipeline) {
                    RetireTranslatedPipeline(std::move(pipeline));
                });
            LOG_INFO(Render_Vulkan,
                     "EDEN_NX2_ASYNC_TRANSLATE={}: new pipelines of draws that may be skipped are "
                     "translated on {} threads{}",
                     VideoCommon::AsyncTranslateMode(), threads,
                     VideoCommon::AsyncTranslateMode() == 2
                         ? ", and on the GPU thread too to compare (verify mode)"
                         : "");
        } else {
            LOG_WARNING(Render_Vulkan, "EDEN_NX2_ASYNC_TRANSLATE needs asynchronous shaders; "
                                       "translating on the GPU thread");
        }
    } else if (VideoCommon::AsyncTranslateMode() == 3) {
        LOG_INFO(Render_Vulkan, "EDEN_NX2_ASYNC_TRANSLATE=3: GPU-thread translations use "
                                "environment snapshots, checked against the live state");
    }
}

namespace {
// Pipelines the last two sessions used are built at boot. Each holds ~0.45 MB of driver memory:
// title 1 built 4,800 from eight sessions, 1.5 GB more than building them on first use (but that
// stalled frames 4x as often), while one session of its title screen uses ~1,500.
// MoltenVK keeps the previous eight sessions: a pipeline built while playing goes through
// SPIR-V -> MSL -> Metal, far more than the 2-3 ms the shorter window assumes, so each one
// built at boot is a stutter avoided.
// Ocarina still built pipelines for 1-2 s of every 10 s early in play with two sessions kept:
// keep sixteen (#161).
constexpr u8 UsedPipelinesMaxAge = 16;
constexpr u8 UsedPipelinesMaxAgeMoltenVK = 8;

u8 MaxUsedPipelineAge(const Device& device) {
    if (device.IsMoltenVK()) {
        return UsedPipelinesMaxAgeMoltenVK;
    }
    return UsedPipelinesMaxAge;
}

// Without a list of the pipelines recent sessions used (the first boot with this cache, or a session
// that ended before saving it), this many cached graphics pipelines at most are built at boot and the
// rest when first needed. A cache that grew over many builds held 48k pipelines, ~21 GB of driver
// memory built before the game started; the list is only saved at exit or after two minutes, so a
// boot that ran out of memory never saved one and the next boot did the same.
constexpr size_t MaxPipelinesWithoutUsedList = 3000;

// Captured environments the shader precache holds for skipped graphics pipelines; the rest wait
// for a later precache.
constexpr size_t PrecacheCaptureLimit = size_t{1} << 30;

// Shader cache v2: the records of the background prebuild the queue may hold, by payload bytes
constexpr u64 PrebuildCaptureLimit = u64{256} << 20;

// Shader cache v2: bytes a job's stage is taken to hold for the scheduler's budget. EGLA-80
// measured ~100 MiB of commit per driver compile in flight (24 at once held ~2.5 GB more than
// idle). A translation's IR pools grow with the program; first guesses until measured.
constexpr u64 DriverCompileBytes = u64{96} << 20;

u64 TranslationBytes(u64 record_bytes) {
    return std::clamp<u64>(record_bytes * 512, u64{8} << 20, u64{512} << 20);
}

u64 IrBytes(std::span<const Shader::IR::Program> programs) {
    u64 bytes{};
    for (const Shader::IR::Program& program : programs) {
        for (const Shader::IR::Block* const block : program.blocks) {
            bytes += sizeof(Shader::IR::Block) + block->Instructions().size() * sizeof(Shader::IR::Inst);
        }
    }
    return bytes;
}

/// A recorded graphics key as this build's draws key it (FixedPipelineState::Refresh): one key for
/// every logic op while the logic op is off (eb2b411de), and only the vertex attribute types the
/// programs read (#408, Fritchii v4 55bba8812). Neither changes the pipeline that is built, but a
/// record from a build without them would never meet a draw's key, and records differing only
/// there would each be built.
void KeyAsDraws(GraphicsPipelineCacheKey& key, std::span<const FileEnvironment> envs,
                const DynamicFeatures& features) {
    auto& fixed = key.state;
    if (fixed.extended_dynamic_state_2_logic_op == 0 &&
        !features.has_dynamic_state3_logic_op_enable && fixed.dynamic_state.logic_op_enable == 0) {
        fixed.dynamic_state.logic_op.Assign(0);
    }
    if (fixed.dynamic_vertex_input == 0) {
        return;
    }
    std::array<u64, Maxwell::MaxShaderProgram> attribute_masks;
    attribute_masks.fill(~u64{0});
    size_t env_index = 0;
    for (size_t stage = 0; stage < Maxwell::MaxShaderProgram; ++stage) {
        if (key.unique_hashes[stage] != 0 && env_index < envs.size()) {
            attribute_masks[stage] = VideoCommon::VertexAttributeMaskOf(envs[env_index++]);
        }
    }
    fixed.attribute_types = VideoCommon::KeyedAttributeTypes(fixed.attribute_types,
                                                             key.unique_hashes, attribute_masks);
}

// EDEN_NX2_PRELOAD_ALL_PIPELINES=1 builds every cached pipeline at boot, as before.
bool PreloadAllPipelines() {
    static const bool all = [] {
        const char* value = std::getenv("EDEN_NX2_PRELOAD_ALL_PIPELINES");
        return value != nullptr && value[0] == '1';
    }();
    return all;
}

// Built graphics pipelines kept however long they go unused, and cached pipelines built at boot
// whether or not the last sessions used them (LICHII_KEEP_PIPELINES=<n>; 0 restores upstream:
// boot builds only the used list, and a pipeline unused for EvictAfterFrames is destroyed). On
// AMD's driver a destroyed pipeline took ~6 ms to build again, and a draw that needs it waits:
// Ocarina of Time evicted 1,076 of its 1,236 pipelines during three minutes on the title screen,
// then held the first gameplay frames 2.3 s to rebuild them. ~0.45 MB of driver memory each.
size_t KeptPipelines() {
    static const size_t kept = [] {
        const char* value = std::getenv("LICHII_KEEP_PIPELINES");
        const size_t count = value != nullptr ? std::strtoull(value, nullptr, 10) : 4096;
        LOG_INFO(Render_Vulkan, "Pipelines: up to {} kept built and built at boot "
                                "(LICHII_KEEP_PIPELINES)", count);
        return count;
    }();
    return kept;
}
} // Anonymous namespace

namespace {
/// Destroys the pipelines of `pipelines` (a map or a deque of pairs holding unique_ptrs), telling
/// the frontend every few hundred that the teardown still moves: tens of thousands of them took
/// longer than the frontend waited for the emulation thread.
template <typename Container>
void DestroyPipelinesWithProgress(Container& pipelines) {
    constexpr size_t ProgressEvery = 256;
    size_t destroyed = 0;
    for (auto& entry : pipelines) {
        entry.second.reset();
        if (++destroyed % ProgressEvery == 0) {
            Common::NoteShutdownProgress();
        }
    }
    pipelines.clear();
    Common::NoteShutdownProgress();
}
} // Anonymous namespace

PipelineCache::~PipelineCache() {
    // The stop callback goes first: it uses shader_jobs.
    loading_stop_callback.reset();
    // Its threads build with the members below (a translation in progress finishes).
    translate_queue.reset();
    serialization_stop.request_stop();
    if (scheduled_prebuild) {
        if (const auto usage = scheduled_prebuild->GetUsage();
            usage.taken != 0 || usage.unused != 0) {
            // Prebuilt pipelines a draw never took cost driver memory and build time for nothing.
            LOG_INFO(Render_Vulkan,
                     "Background pipelines this session: {} taken by draws, {} never used",
                     usage.taken, usage.unused);
        }
    }
    StopBackgroundJobs();
    // The pipelines' builds end before the driver pipeline cache they build into goes.
    shader_jobs->Shutdown();
    Common::NoteShutdownProgress();
    serialization_thread.WaitForRequests();
    ReportDiskRecordFailures(); // the background prebuild's
    Common::NoteShutdownProgress();
    // The caches are written before anything slow is torn down, so a teardown cut short still
    // leaves them on disk.
    SaveUsedPipelines();
    if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
        SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                     CACHE_VERSION);
    }
    Common::NoteShutdownProgress();
    // After the records the serialization thread queued: its writer drains on destruction
    shader_cache_v2 = nullptr;
    cache_v2.reset();
    Common::NoteShutdownProgress();
    DestroyPipelinesWithProgress(graphics_cache);
    DestroyPipelinesWithProgress(evicted_pipelines);
    DestroyPipelinesWithProgress(retired_translations);
    if (pipeline_binaries) {
        pipeline_binaries->LogCounts("at the end of the session");
        pipeline_binaries.reset();
    }
}

void PipelineCache::StopTranslations() {
    if (!translate_queue) {
        return;
    }
    // The GPU threads stopped, so nothing enqueues or adopts anymore and the retired pipelines
    // can be handed over here.
    translate_queue->Shutdown();
    translate_queue.reset();
}

void PipelineCache::NoteDiskRecordFailure(std::string_view kind, u64 hash, const char* reason) {
    constexpr size_t Shown = 4;
    if (disk_record_failures.fetch_add(1, std::memory_order_relaxed) < Shown) {
        LOG_INFO(Render_Vulkan,
                 "{} pipeline {:016x}: a cached record this build cannot translate ({}), built "
                 "when first needed instead",
                 kind, hash, reason);
    } else {
        LOG_DEBUG(Render_Vulkan, "{} pipeline {:016x}: cached record not translated: {}", kind,
                  hash, reason);
    }
}

void PipelineCache::ReportDiskRecordFailures() {
    const size_t failures = disk_record_failures.exchange(0, std::memory_order_relaxed);
    if (failures != 0) {
        LOG_INFO(Render_Vulkan,
                 "{} cached pipeline records could not be translated by this build (stored by "
                 "another or an older build, or guessed by the shader precache); their pipelines "
                 "are built when first needed",
                 failures);
    }
}

void PipelineCache::SaveUsedPipelines() {
    used_pipelines_saved = std::chrono::steady_clock::now();
    // A session too short to say what the game uses (a boot to the menu) keeps the old list.
    constexpr size_t MinimumUsed = 64;
    if (!cache_v2 || used_this_session.size() + used_and_evicted.size() < MinimumUsed) {
        return;
    }
    // This session's record, written by the cache's thread; older sessions keep theirs.
    VideoCommon::ShaderCacheV2::UsedRecord record{.session_time = used_session_time};
    for (const auto& [key, pipeline] : graphics_cache) {
        if (used_this_session.contains(pipeline.get())) {
            record.pipelines.push_back(key.Hash());
        }
    }
    record.pipelines.insert(record.pipelines.end(), used_and_evicted.begin(),
                            used_and_evicted.end());
    std::ranges::sort(record.pipelines);
    record.pipelines.erase(std::ranges::unique(record.pipelines).begin(), record.pipelines.end());
    cache_v2->Writer().Append(VideoCommon::ShaderCacheV2::RecordKind::Used, used_session_key,
                              VideoCommon::ShaderCacheV2::EncodeUsed(record));
}

void PipelineCache::RefreshGraphicsKey() {
    if (!dynamic_features.has_dynamic_vertex_input) {
        graphics_key.state.Refresh(*maxwell3d, dynamic_features);
        return;
    }
    // From Fritchii v4 (Frizti/Lichii 55bba8812, a port of zortos293/Lichii#408): Refresh only
    // rewrites the types when the vertex input changes, so the masked copy in the key must not
    // carry over to the next programs. Types of attributes the programs don't read split their
    // pipelines by whatever the game left in those registers (Ocarina of Time: up to 20 keys for
    // the same shaders, each built again on the GPU thread).
    graphics_key.state.attribute_types = vertex_attribute_types;
    graphics_key.state.Refresh(*maxwell3d, dynamic_features);
    vertex_attribute_types = graphics_key.state.attribute_types;
    graphics_key.state.attribute_types = VideoCommon::KeyedAttributeTypes(
        vertex_attribute_types, graphics_key.unique_hashes, vertex_attribute_masks);
}

GraphicsPipeline* PipelineCache::CurrentGraphicsPipeline() {
    ++lookup_serial;
    if (!RefreshStages(graphics_key.unique_hashes)) {
        current_pipeline = nullptr;
        return nullptr;
    }
    RefreshGraphicsKey();

    if (current_pipeline) {
        GraphicsPipeline* const next{current_pipeline->Next(graphics_key)};
        if (next) {
            current_pipeline = next;
            current_pipeline->MarkUsed(frame_count);
            return BuiltPipeline(current_pipeline);
        }
    }
    return CurrentGraphicsPipelineSlowPath();
}

ComputePipeline* ComputePipelineCacheEntry::Get(bool native, GPUVAddr address,
                                                const ComputePipelineCacheKey& key) const {
    if (!pipeline && native) {
        throw Shader::RuntimeError(
            "Native compute pipeline unavailable: address={:#x} shader={:#016x} pipeline={:#016x}: {}",
            address, key.unique_hash, key.Hash(),
            failure.empty() ? "No compiler diagnostic was recorded" : failure);
    }
    return pipeline.get();
}

constexpr u32 MaxComputeRebuilds = 4;

namespace {
/// Which pointer slots a native program's arguments fill depends on the area (Ocarina of Time's
/// foliage culling fills different list slots in Hyrule Field and Castle Town), so a translation
/// rebuilt in one area may lack pointers the first cached one binds. Try first the translations
/// that bind every pointer the first one binds, those binding the most first and the newest of
/// those binding as many (an older one can be from a translator that asked about fewer slots, and
/// fail; Fritchii v4 9b8fef282), then the others in cache order.
void OrderComputeTranslations(std::vector<CachedComputeTranslation>& candidates) {
    if (candidates.size() < 2) {
        return;
    }
    std::vector<std::vector<u64>> keys;
    keys.reserve(candidates.size());
    for (const CachedComputeTranslation& candidate : candidates) {
        keys.push_back(candidate.env.MappedStoragePointerKeys());
    }
    std::vector<size_t> order(candidates.size());
    std::iota(order.begin(), order.end(), size_t{0});
    const auto keeps_first = [&](size_t index) {
        return std::ranges::includes(keys[index], keys.front());
    };
    std::ranges::stable_sort(order, [&](size_t lhs, size_t rhs) {
        const bool lhs_keeps{keeps_first(lhs)};
        if (lhs_keeps != keeps_first(rhs)) {
            return lhs_keeps;
        }
        if (!lhs_keeps) {
            return false;
        }
        if (keys[lhs].size() != keys[rhs].size()) {
            return keys[lhs].size() > keys[rhs].size();
        }
        return lhs > rhs;
    });
    std::vector<CachedComputeTranslation> ordered;
    ordered.reserve(candidates.size());
    for (const size_t index : order) {
        ordered.push_back(std::move(candidates[index]));
    }
    candidates = std::move(ordered);
}
} // Anonymous namespace

bool PipelineCache::StaleNullPointerSlot(const ComputePipelineCacheEntry& entry) const {
    const auto& qmd{kepler_compute->launch_description};
    for (const auto& [index, offset] : entry.null_pointer_slots) {
        if (index >= qmd.const_buffer_config.size() ||
            ((qmd.const_buffer_enable_mask >> index) & 1) == 0) {
            continue;
        }
        const auto& cbuf{qmd.const_buffer_config[index]};
        if (offset + sizeof(u64) > cbuf.size) {
            continue; // The translator reads past the bound size as zero, too
        }
        const GPUVAddr slot{cbuf.Address() + offset};
        if (!gpu_memory->IsFullyMappedRange(slot, sizeof(u64))) {
            continue;
        }
        const GPUVAddr address{gpu_memory->Read<u64>(slot)};
        if (address != 0 && gpu_memory->IsWithinGPUAddressRange(address) &&
            gpu_memory->IsFullyMappedRange(address, 1)) {
            LOG_INFO(Render_Vulkan,
                     "Rebuilding compute program {:#x}: c{}[{:#x}] was null at translation and "
                     "now points to {:#x}",
                     kepler_compute->regs.code_loc.Address() + qmd.program_start, index, offset, address);
            return true;
        }
    }
    return false;
}

ComputePipeline* PipelineCache::CurrentComputePipeline(u64& native_key) {

    const ShaderInfo* const shader{ComputeShader()};
    if (!shader) {
        return nullptr;
    }
    native_key = shader->lowered_native ? shader->lowered_native->native_program_key : 0;
    const auto& qmd{kepler_compute->launch_description};
    const ComputePipelineCacheKey key{
        .unique_hash = shader->unique_hash,
        .shared_memory_size = qmd.shared_alloc,
        .workgroup_size{qmd.block_dim_x, qmd.block_dim_y, qmd.block_dim_z},
    };
    const auto [pair, is_new]{compute_cache.try_emplace(key)};
    auto& entry{pair->second};
    if (is_new) {
        entry = CreateComputePipeline(key, shader);
    } else if (entry.pending) {
        if (entry.pending->pipeline->IsBuilt()) {
            const auto rebuilt{std::move(entry.pending)};
            if (rebuilt->pipeline->IsBound()) {
                LOG_INFO(Render_Vulkan, "Compute shader {:016x}: rebuilt pipeline in use",
                         key.unique_hash);
                retired_compute_pipelines.emplace_back(scheduler.CurrentTick(),
                                                       std::move(entry.pipeline));
                entry = std::move(*rebuilt);
            } else {
                LOG_WARNING(Render_Vulkan, "Keeping compute shader {:016x}: rebuild failed",
                            key.unique_hash);
                retired_compute_pipelines.emplace_back(scheduler.CurrentTick(),
                                                       std::move(rebuilt->pipeline));
                entry.rebuilds = MaxComputeRebuilds;
            }
        }
    } else if (!entry.null_pointer_slots.empty() && entry.rebuilds < MaxComputeRebuilds &&
               StaleNullPointerSlot(entry)) {
        // The translation dropped accesses through a pointer that was null back then. Which slots
        // the game fills depends on the area, so the new one keeps binding every pointer this one
        // binds, null now or not: a null pointer binds nothing.
        auto rebuilt{CreateComputePipeline(key, shader, entry.mapped_pointers)};
        if (rebuilt.pipeline) {
            rebuilt.rebuilds = entry.rebuilds + 1;
            rebuilt.used = entry.used;
            if (entry.pipeline) {
                entry.pending = std::make_unique<ComputePipelineCacheEntry>(std::move(rebuilt));
            } else {
                entry = std::move(rebuilt);
            }
        } else {
            // Keep the working translation when the new one cannot be built (for example a
            // narrow access that now stays global across two buffers), and stop retrying.
            LOG_WARNING(Render_Vulkan, "Keeping compute shader {:016x}: rebuild failed: {}",
                        key.unique_hash, rebuilt.failure);
            entry.rebuilds = MaxComputeRebuilds;
        }
    }
    if (VideoCommon::StutterTrace::Enabled() && !std::exchange(entry.used, true)) {
        VideoCommon::StutterTrace::CountLookup(
            is_new ? VideoCommon::StutterTrace::Lookup::ComputeNew
                   : VideoCommon::StutterTrace::Lookup::ComputePrebuilt);
    }
    return entry.Get(kepler_compute->IsNativeMode(),
                     kepler_compute->regs.code_loc.Address() + qmd.program_start, key);
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    if (!pipeline_cache_filename.empty()) {
        StopBackgroundJobs();
        serialization_thread.WaitForRequests();
        if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                         CACHE_VERSION);
        }
        shader_cache_v2 = nullptr;
        cache_v2.reset();
        pipeline_binaries.reset();
    }

    namespace StutterTrace = VideoCommon::StutterTrace;
    const auto load_begin = std::chrono::steady_clock::now();
    StutterTrace::BootMarker("shader_load_begin");
    const auto shader_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    namespace CacheV2 = VideoCommon::ShaderCacheV2;
    pipeline_cache_filename = base_dir / CacheV2::FileName;
    const auto migration =
        MigrateLegacyPipelineCache(base_dir, pipeline_cache_filename, MaxUsedPipelineAge(device));
    if (migration.version) {
        LOG_INFO(Render_Vulkan,
                 "Shader cache v2 started from the version {} cache: {} graphics and {} "
                 "compute pipelines ({} records collapsed), {} used-list entries",
                 *migration.version, migration.graphics, migration.compute, migration.collapsed,
                 migration.used);
    } else if (migration.failed) {
        LOG_WARNING(Render_Vulkan, "Couldn't start shader cache v2 from the older cache");
    }
    cache_v2 = std::make_unique<CacheV2::Cache>(pipeline_cache_filename);
    shader_cache_v2 = cache_v2.get();
    pipeline_binaries = PipelineBinaries::Open(device, base_dir);
    // Without the lock: another emulator's appends are complete records or a cut-off tail.
    // The payload hashes are checked on the shader job workers.
    auto log_v2 = std::make_shared<CacheV2::LogFile>(CacheV2::ReadLogFile(
        pipeline_cache_filename, [this](size_t count, const std::function<void(size_t)>& task) {
            VideoCommon::ParallelFor(*shader_jobs, count, task);
        }));
    cache_v2->LoadPrograms(log_v2->parsed.live);
    const CacheV2::LogStats& stats = log_v2->parsed.stats;
    LOG_INFO(Render_Vulkan,
             "Shader cache v2: {} bytes, {} live records, {} superseded, {} tombstones, {} "
             "damaged headers, {} damaged records, {} bytes skipped, {} bytes cut off",
             stats.file_size, stats.live, stats.superseded, stats.tombstones, stats.bad_headers,
             stats.bad_payloads, stats.skipped_bytes, stats.torn_tail_bytes);
    std::vector<u64> expired_sessions;
    used_pipelines =
        UsedPipelinesFromLog(log_v2->parsed.live, MaxUsedPipelineAge(device), expired_sessions);
    for (const u64 session : expired_sessions) {
        cache_v2->Writer().Append(CacheV2::RecordKind::Tombstone, session,
                                  CacheV2::TombstonePayload(CacheV2::RecordKind::Used));
    }
    used_session_time = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count());
    used_session_key = used_session_time ^ std::random_device{}();
    used_pipelines_saved = std::chrono::steady_clock::now();

    // With the shader precache this boot, the cached graphics pipelines skipped below are built
    // on the loading screen too, unless an earlier precache built them for this driver.
    namespace ShaderWarm = VideoCommon::ShaderWarm;
    const bool precache = ShaderWarm::PrecacheBeforeStart(
        Settings::values.precache_shaders.GetValue(), ShaderWarm::TakeWarmBeforeStart());
    std::unordered_set<u64> precached_pipelines;
    if (precache) {
        precached_pipelines = ShaderWarm::ReadPrecachedPipelines(
            ShaderWarm::PrecachedPipelinesPath(title_id),
            {device.GetDriverVersion(), device.GetDeviceID(), CACHE_VERSION});
    }
    if (use_vulkan_pipeline_cache) {
        vulkan_pipeline_cache_filename =
            VideoCommon::VersionedPipelineCachePath(base_dir, "vulkan_pipelines", CACHE_VERSION);
        if (VideoCommon::CarryOverPipelineCache(
                base_dir, "vulkan_pipelines", VULKAN_CACHE_MAGIC_NUMBER,
                VideoCommon::CompatibleVulkanPipelineCacheVersions, CACHE_VERSION)
                .failed) {
            LOG_WARNING(Render_Vulkan,
                        "Couldn't copy the older driver pipeline cache; this one starts empty");
        }
        vulkan_pipeline_cache =
            LoadVulkanPipelineCache(vulkan_pipeline_cache_filename, CACHE_VERSION);
    }
    LoadPipelinesV2(title_id, std::move(log_v2), stop_loading, callback, load_begin, precache,
                    std::move(precached_pipelines));
}

bool PipelineCache::KeyFitsDevice(const GraphicsPipelineCacheKey& key) const {
    if ((key.state.extended_dynamic_state != 0) != dynamic_features.has_extended_dynamic_state ||
        (key.state.extended_dynamic_state_2 != 0) !=
            dynamic_features.has_extended_dynamic_state_2 ||
        (key.state.extended_dynamic_state_2_logic_op != 0) !=
            dynamic_features.has_extended_dynamic_state_2_logic_op ||
        (key.state.extended_dynamic_state_3_blend != 0) !=
            dynamic_features.has_extended_dynamic_state_3_blend ||
        (key.state.extended_dynamic_state_3_enables != 0) !=
            dynamic_features.has_extended_dynamic_state_3_enables ||
        (key.state.color_write_enable_dynamic != 0) != dynamic_features.has_color_write_enable ||
        (key.state.dynamic_vertex_input != 0) != dynamic_features.has_dynamic_vertex_input) {
        return false;
    }
    const bool key_requests_provoking_last = key.state.provoking_vertex_last != 0;
    if (key_requests_provoking_last && !dynamic_features.has_provoking_vertex_last_mode) {
        return false;
    }
    const bool key_uses_transform_feedback = key.state.xfb_enabled != 0;
    return !key_uses_transform_feedback || !key_requests_provoking_last ||
           dynamic_features.has_provoking_vertex_tf_preserve;
}

PipelineBuildQueue PipelineCache::BuildQueue(bool build_in_parallel, u64 cpu_bytes) {
    if (!build_in_parallel) {
        return {.owner_builds = true};
    }
    return {.jobs = &*shader_jobs, .cpu_bytes = cpu_bytes, .driver_bytes = DriverCompileBytes};
}

std::shared_ptr<ShaderPools> PipelineCache::TakeSparePools() {
    {
        std::scoped_lock lock{spare_pools_mutex};
        if (!spare_pools.empty()) {
            auto pools = std::move(spare_pools.back());
            spare_pools.pop_back();
            return pools;
        }
    }
    return std::make_shared<ShaderPools>();
}

void PipelineCache::ReturnPools(std::shared_ptr<ShaderPools> pools) {
    pools->ReleaseContents();
    std::scoped_lock lock{spare_pools_mutex};
    if (spare_pools.size() < 4) {
        spare_pools.push_back(std::move(pools));
    }
}

void PipelineCache::StopBackgroundJobs() {
    scheduled_prebuild.reset();
}

std::unique_ptr<GraphicsPipeline> PipelineCache::TranslateCachedGraphics(
    std::span<const u8> payload, GraphicsPipelineCacheKey& key, PipelineStatistics* statistics,
    bool& incomplete, const std::function<bool(const GraphicsPipelineCacheKey&)>& admit) {
    incomplete = false;
    std::vector<FileEnvironment> envs;
    Common::UniqueFunction<void, std::istream&, FileEnvironment> read_compute{
        [](std::istream&, FileEnvironment) {
            throw std::ios_base::failure("Compute pipeline in another kind of record");
        }};
    Common::UniqueFunction<void, std::istream&, std::vector<FileEnvironment>> read_graphics{
        [&](std::istream& file, std::vector<FileEnvironment> read) {
            GraphicsPipelineCacheKey read_key;
            file.read(reinterpret_cast<char*>(&read_key), sizeof(read_key));
            VideoCommon::CheckGraphicsRecord(read_key.unique_hashes, read.size());
            envs = std::move(read);
        }};
    VideoCommon::ReadPipelinePayload(payload, read_compute, read_graphics);
    KeyAsDraws(key, envs, dynamic_features);
    if (admit && !admit(key)) {
        return nullptr;
    }
    ShaderPools pools;
    boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> env_ptrs;
    for (auto& env : envs) {
        env_ptrs.push_back(&env);
    }
    auto pipeline{CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), statistics, false)};
    incomplete = !pipeline && std::ranges::any_of(envs, [this](const FileEnvironment& env) {
        return env.LacksRecordedValue(host_info.EnvironmentReadProperties());
    });
    return pipeline;
}

void PipelineCache::LoadPipelinesV2(u64 title_id,
                                    std::shared_ptr<const VideoCommon::ShaderCacheV2::LogFile> log,
                                    std::stop_token stop_loading,
                                    const VideoCore::DiskResourceLoadCallback& callback,
                                    std::chrono::steady_clock::time_point load_begin,
                                    bool precache, std::unordered_set<u64> precached_pipelines) {
    namespace CacheV2 = VideoCommon::ShaderCacheV2;
    namespace StutterTrace = VideoCommon::StutterTrace;
    using CacheV2::RecordKind;
    using VideoCommon::ShaderJobPriority;
    auto& jobs = *shader_jobs;
    const u32 read_properties = host_info.EnvironmentReadProperties();

    // The boot builds (Soon), which Loading Shaders waits for, and the background prebuild. A
    // stopped load cancels both; the workers stay for the game.
    const VideoCommon::ShaderJobBatch boot = jobs.CreateBatch();
    scheduled_prebuild.emplace(jobs, PrebuildCaptureLimit);
    const std::stop_callback cancel_load{
        stop_loading, [&jobs, boot, prebuild = scheduled_prebuild->Batch()] {
            jobs.Cancel(boot);
            jobs.Cancel(prebuild);
        }};

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        bool has_loaded{};
        std::unique_ptr<PipelineStatistics> statistics;
        /// Records the load never chooses, as in the old cache; begin is the index in records
        std::vector<VideoCommon::PipelineRecord> incomplete;
        std::vector<std::pair<u64, VideoCommon::PipelineRecord>> incomplete_graphics;
        std::vector<VideoCommon::PipelineRecord> copies;
        std::chrono::steady_clock::time_point last_save{std::chrono::steady_clock::now()};
        /// Key hashes of the boot builds as draws key them (KeyAsDraws), and the records left
        /// out for sharing one with an earlier record
        std::unordered_set<u64> boot_keys;
        size_t shared_keys{};
    } state;
    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        state.statistics = std::make_unique<PipelineStatistics>(device);
    }
    // Under state.mutex
    const auto built_one = [&state, &callback] {
        ++state.built;
        if (state.has_loaded) {
            callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
        }
    };
    // The graphics and compute records read, by the index a PipelineRecord's begin stands for
    std::vector<const CacheV2::LogRecord*> records;
    const auto pipeline_record = [&records](size_t index) {
        return VideoCommon::PipelineRecord{.begin = index, .end = index + 1, .hash = records[index]->key};
    };
    // Intact, but not a record this build reads: it goes.
    const auto unreadable = [this](const CacheV2::LogRecord& record, const char* reason) {
        LOG_ERROR(Common_Filesystem, "Shader cache v2 record {:016x}: {}", record.key, reason);
        cache_v2->Writer().Append(RecordKind::Tombstone, record.key,
                                  CacheV2::TombstonePayload(record.kind));
    };

    // One pass over the records without reading them, which the jobs do in parallel: all
    // translations of a compute key go to one job, graphics records by their key's hash.
    ::Common::unordered_map<ComputePipelineCacheKey, std::vector<size_t>> compute_keys;
    std::vector<std::pair<size_t, GraphicsPipelineCacheKey>> boot_graphics;
    struct PrebuildRecord {
        GraphicsPipelineCacheKey key;
        std::vector<u8> payload;
    };
    std::vector<PrebuildRecord> prebuild;
    u64 prebuild_bytes = 0;
    std::vector<PrecacheJob> precache_graphics;
    u64 precache_capture_bytes = 0;
    std::vector<CacheV2::ProgramRecord> stale_programs;
    for (const CacheV2::LogRecord& record : log->parsed.live) {
        if (record.kind != RecordKind::Program) {
            continue;
        }
        const auto stamp = CacheV2::ProgramStamp(record.payload);
        if (!stamp) {
            continue;
        }
        cache_v2->AddProgram(record.key, *stamp);
        if (*stamp != CacheV2::LoweringStamp()) {
            if (auto program = CacheV2::DecodeProgram(record.payload)) {
                stale_programs.push_back(std::move(*program));
            }
        }
    }
    // Programs another build recorded are lowered again before any of their pipelines is built:
    // the records of those whose code or texture bound buffer changed move to this build's code
    // (a pipeline key holds the code's hash alone, so one built from the old record would be hit
    // with the new texture bound buffer). Their own records follow last, so that a load cut short
    // does this again at the next boot.
    std::vector<CacheV2::LogAppend> relinked_appends;
    std::vector<CacheV2::LogRecord> relinked_records;
    std::unordered_set<u64> replaced_records;
    if (!stale_programs.empty() && !stop_loading.stop_requested()) {
        const size_t stale_count = stale_programs.size();
        CacheV2::ForEach for_each = [&jobs](size_t count, const std::function<void(size_t)>& task) {
            VideoCommon::ParallelFor(jobs, count, task);
        };
        RevalidatedPrograms revalidated =
            RevalidateProgramRecords(std::move(stale_programs), for_each);
        relinked_appends =
            RelinkPipelineRecords(log->parsed.live, revalidated.relinked,
                                  [&stop_loading] { return stop_loading.stop_requested(); });
        if (!stop_loading.stop_requested()) {
            for (const CacheV2::LogAppend& append : relinked_appends) {
                cache_v2->Writer().Append(append.kind, append.key, append.payload);
                replaced_records.insert(append.key);
                if (append.kind == RecordKind::Tombstone) {
                    continue;
                }
                relinked_records.push_back({.kind = append.kind,
                                            .flags = 0,
                                            .key = append.key,
                                            .offset = 0,
                                            .payload = append.payload});
            }
            for (const CacheV2::LogAppend& append :
                 RelinkUsedPipelines(log->parsed.live, relinked_appends, MaxUsedPipelineAge(device),
                                     used_pipelines)) {
                cache_v2->Writer().Append(append.kind, append.key, append.payload);
            }
            for (const CacheV2::ProgramRecord& program : revalidated.unchanged) {
                cache_v2->RecordProgram(program);
            }
            for (const auto& [hash, relinked] : revalidated.relinked) {
                cache_v2->RecordProgram(relinked.program);
            }
            LOG_INFO(Render_Vulkan,
                     "Shader cache v2: {} programs of another build lowered again, {} to other "
                     "code or texture bound buffer, {} pipeline records moved to it",
                     stale_count, revalidated.relinked.size(), relinked_records.size());
        }
    }
    std::vector<const CacheV2::LogRecord*> plan;
    plan.reserve(log->parsed.live.size() + relinked_records.size());
    for (const CacheV2::LogRecord& record : log->parsed.live) {
        if (record.kind == RecordKind::Program ||
            ((record.kind == RecordKind::Graphics || record.kind == RecordKind::Compute) &&
             replaced_records.contains(record.key))) {
            continue;
        }
        plan.push_back(&record);
    }
    for (const CacheV2::LogRecord& record : relinked_records) {
        plan.push_back(&record);
    }

    size_t graphics_records = 0;
    size_t compute_records = 0;
    size_t skipped_pipelines = 0;
    size_t queued_graphics = 0;
    // Graphics records this device can't build (KeyFitsDevice), by the first state they differ
    // in: a cache recorded on another GPU or with other settings (logged after loading).
    static constexpr std::array<const char*, 8> unfit_reasons{
        "extended dynamic state",           "extended dynamic state 2",
        "extended dynamic state 2 logic op", "extended dynamic state 3 blending",
        "extended dynamic state 3 enables", "dynamic color write enable",
        "vertex input dynamic state",       "the last provoking vertex mode",
    };
    std::array<size_t, unfit_reasons.size()> unfit{};
    std::array<size_t, unfit_reasons.size()> unfit_recorded_on{}; ///< Recorded with it on
    const auto count_unfit = [&](const GraphicsPipelineCacheKey& unfit_key) {
        const auto& fixed = unfit_key.state;
        const std::array<std::pair<bool, bool>, 7> features{{
            {fixed.extended_dynamic_state != 0, dynamic_features.has_extended_dynamic_state},
            {fixed.extended_dynamic_state_2 != 0, dynamic_features.has_extended_dynamic_state_2},
            {fixed.extended_dynamic_state_2_logic_op != 0,
             dynamic_features.has_extended_dynamic_state_2_logic_op},
            {fixed.extended_dynamic_state_3_blend != 0,
             dynamic_features.has_extended_dynamic_state_3_blend},
            {fixed.extended_dynamic_state_3_enables != 0,
             dynamic_features.has_extended_dynamic_state_3_enables},
            {fixed.color_write_enable_dynamic != 0, dynamic_features.has_color_write_enable},
            {fixed.dynamic_vertex_input != 0, dynamic_features.has_dynamic_vertex_input},
        }};
        for (size_t reason = 0; reason < features.size(); ++reason) {
            if (features[reason].first != features[reason].second) {
                ++unfit[reason];
                unfit_recorded_on[reason] += features[reason].first ? 1 : 0;
                return;
            }
        }
        ++unfit[features.size()];
    };
    for (const CacheV2::LogRecord* const planned : plan) {
        const CacheV2::LogRecord& record = *planned;
        if (stop_loading.stop_requested()) {
            break;
        }
        if (record.kind == RecordKind::Compute) {
            ++compute_records;
            const auto key = PipelineRecordKey<ComputePipelineCacheKey>(record.payload);
            if (!key) {
                unreadable(record, "Compute pipeline record without a key");
                continue;
            }
            compute_keys[*key].push_back(records.size());
            records.push_back(&record);
            continue;
        }
        if (record.kind != RecordKind::Graphics) {
            continue;
        }
        ++graphics_records;
        const auto key = PipelineRecordKey<GraphicsPipelineCacheKey>(record.payload);
        if (!key) {
            unreadable(record, "Graphics pipeline record without a key");
            continue;
        }
        const size_t index = records.size();
        records.push_back(&record);
        if (!KeyFitsDevice(*key)) {
            count_unfit(*key);
            continue;
        }
        const u64 hash = key->Hash();
        if (!disk_pipelines.insert(hash).second) {
            continue;
        }
        // Pipelines the last sessions used are built at boot; with LICHII_KEEP_PIPELINES, the
        // others too while fewer than that many are queued.
        const bool used = used_pipelines.empty() ? queued_graphics < MaxPipelinesWithoutUsedList
                                                 : used_pipelines.contains(hash);
        if (PreloadAllPipelines() || used || queued_graphics < KeptPipelines()) {
            ++queued_graphics;
            boot_graphics.emplace_back(index, *key);
            continue;
        }
        ++skipped_pipelines;
        const u64 bytes = record.payload.size();
        std::vector<u8> payload(record.payload.begin(), record.payload.end());
        if (precache && !precached_pipelines.contains(hash) &&
            bytes <= PrecacheCaptureLimit - precache_capture_bytes) {
            precache_capture_bytes += bytes;
            precache_graphics.push_back({
                .graphics_hash = hash,
                .translate = [this, key = *key, payload = std::move(payload)]() mutable
                    -> Common::UniqueFunction<bool> {
                    bool incomplete{};
                    auto pipeline = TranslateCachedGraphics(payload, key, nullptr, incomplete);
                    if (!pipeline) {
                        return {};
                    }
                    return [pipeline = std::move(pipeline)] {
                        pipeline->Build();
                        return !pipeline->Failed();
                    };
                },
                .cpu_bytes = TranslationBytes(bytes),
            });
        } else if (bytes <= PrebuildCaptureLimit - prebuild_bytes) {
            prebuild_bytes += bytes;
            prebuild.push_back({*key, std::move(payload)});
        }
    }

    for (auto& [key, indices] : compute_keys) {
        ++state.total;
        u64 bytes = 0;
        for (const size_t index : indices) {
            bytes = std::max<u64>(bytes, records[index]->payload.size());
        }
        jobs.Submit(
            {
                .priority = ShaderJobPriority::Soon,
                .cpu_bytes = TranslationBytes(bytes),
                .driver_bytes = DriverCompileBytes,
                .cpu = [this, &state, &built_one, &records, &pipeline_record, &unreadable, key,
                        indices = std::move(indices),
                        read_properties](VideoCommon::ShaderJobContext& context)
                    -> VideoCommon::ShaderDriverStage {
                    std::vector<CachedComputeTranslation> candidates;
                    for (const size_t index : indices) {
                        const CacheV2::LogRecord& record = *records[index];
                        Common::UniqueFunction<void, std::istream&, FileEnvironment> read_compute{
                            [&](std::istream& file, FileEnvironment env) {
                                ComputePipelineCacheKey read_key;
                                file.read(reinterpret_cast<char*>(&read_key), sizeof(read_key));
                                VideoCommon::CheckComputeRecord(env, read_key.shared_memory_size,
                                                                read_key.workgroup_size);
                                candidates.push_back({std::move(env), pipeline_record(index)});
                            }};
                        Common::UniqueFunction<void, std::istream&, std::vector<FileEnvironment>>
                            read_graphics{[](std::istream&, std::vector<FileEnvironment>) {
                                throw std::ios_base::failure(
                                    "Graphics pipeline in another kind of record");
                            }};
                        try {
                            VideoCommon::ReadPipelinePayload(record.payload, read_compute,
                                                             read_graphics);
                        } catch (const std::ios_base::failure& e) {
                            unreadable(record, e.what());
                        }
                    }
                    OrderComputeTranslations(candidates);
                    ShaderPools pools;
                    ComputePipelineCacheEntry entry;
                    size_t chosen{};
                    for (; chosen < candidates.size() && !context.StopRequested(); ++chosen) {
                        pools.ReleaseContents();
                        entry = CreateComputePipeline(pools, key, candidates[chosen].env,
                                                      state.statistics.get(), false);
                        if (entry.pipeline) {
                            break;
                        }
                    }
                    std::scoped_lock lock{state.mutex};
                    AddUnchosenComputeTranslations(candidates, chosen, read_properties,
                                                   state.incomplete, state.copies);
                    if (!entry.pipeline) {
                        built_one();
                        return {};
                    }
                    return [this, &state, &built_one, key, entry = std::move(entry)]() mutable {
                        entry.pipeline->Build();
                        std::scoped_lock driver_lock{state.mutex};
                        compute_cache.emplace(key, std::move(entry));
                        built_one();
                    };
                },
            },
            &boot);
    }
    compute_keys.clear();
    for (const auto& [index, key] : boot_graphics) {
        ++state.total;
        const CacheV2::LogRecord& record = *records[index];
        jobs.Submit(
            {
                .priority = ShaderJobPriority::Soon,
                .cpu_bytes = TranslationBytes(record.payload.size()),
                .driver_bytes = DriverCompileBytes,
                .cpu = [this, &state, &built_one, &pipeline_record, &unreadable, &record, index,
                        key](VideoCommon::ShaderJobContext& context)
                    -> VideoCommon::ShaderDriverStage {
                    std::unique_ptr<GraphicsPipeline> pipeline;
                    bool incomplete{};
                    // Records that draws key alike are one pipeline, built from the first here
                    GraphicsPipelineCacheKey keyed = key;
                    const auto admit = [this, &state](const GraphicsPipelineCacheKey& draw_key) {
                        std::scoped_lock lock{state.mutex};
                        if (!state.boot_keys.insert(draw_key.Hash()).second) {
                            ++state.shared_keys;
                            return false;
                        }
                        disk_pipelines.insert(draw_key.Hash());
                        return true;
                    };
                    if (!context.StopRequested()) {
                        try {
                            pipeline = TranslateCachedGraphics(
                                record.payload, keyed, state.statistics.get(), incomplete, admit);
                        } catch (const std::ios_base::failure& e) {
                            unreadable(record, e.what());
                        }
                    }
                    if (!pipeline) {
                        std::scoped_lock lock{state.mutex};
                        if (incomplete) {
                            state.incomplete_graphics.emplace_back(key.Hash(),
                                                                   pipeline_record(index));
                        }
                        built_one();
                        return {};
                    }
                    return [this, &state, &built_one, keyed,
                            pipeline = std::move(pipeline)]() mutable {
                        pipeline->Build();
                        std::scoped_lock lock{state.mutex};
                        graphics_cache.emplace(keyed, std::move(pipeline));
                        // A boot of tens of thousands of pipelines takes minutes: what it built
                        // so far is written now and then, so a boot that ends early doesn't
                        // leave the next one to compile everything again.
                        if (VulkanPipelineCacheSaveDue(state.last_save)) {
                            QueueVulkanPipelineCacheSave(false);
                        }
                        built_one();
                    };
                },
            },
            &boot);
    }
    boot_graphics.clear();

    // With the precache, the prebuild starts after it.
    size_t deferred_pipelines = 0;
    const auto start_prebuild = [&] {
        {
            std::scoped_lock save_lock{background_save_mutex};
            background_last_save = std::chrono::steady_clock::now();
        }
        for (PrebuildRecord& record : prebuild) {
            const u64 bytes = record.payload.size();
            const bool added = scheduled_prebuild->Add(
                record.key, bytes, TranslationBytes(bytes), DriverCompileBytes,
                [this, key = record.key, payload = std::move(record.payload)]() mutable {
                    // These builds fill the driver cache too, but only GPU thread builds queue
                    // its timed writes: a session stopped before its exit save kept none.
                    {
                        std::scoped_lock save_lock{background_save_mutex};
                        if (VulkanPipelineCacheSaveDue(background_last_save)) {
                            QueueVulkanPipelineCacheSave(false);
                        }
                    }
                    bool incomplete{};
                    try {
                        return TranslateCachedGraphics(payload, key, nullptr, incomplete);
                    } catch (const std::ios_base::failure&) {
                        return std::unique_ptr<GraphicsPipeline>{};
                    }
                });
            deferred_pipelines += added ? 1 : 0;
        }
        prebuild.clear();
    };
    if (!precache) {
        start_prebuild();
    }

    LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", state.total);
    if (PreloadAllPipelines() && queued_graphics > 10000) {
        LOG_WARNING(Render_Vulkan,
                    "EDEN_NX2_PRELOAD_ALL_PIPELINES: building and keeping all {} cached graphics "
                    "pipelines holds ~{} GB of driver memory all session",
                    queued_graphics, queued_graphics * 460 / (1024 * 1024));
    }
    if (const size_t total_unfit = std::accumulate(unfit.begin(), unfit.end(), size_t{});
        total_unfit != 0) {
        std::string detail;
        for (size_t reason = 0; reason < unfit.size(); ++reason) {
            if (unfit[reason] == 0) {
                continue;
            }
            detail += detail.empty() ? ": " : ", ";
            if (reason + 1 == unfit.size()) {
                detail += fmt::format("{} need {}", unfit[reason], unfit_reasons[reason]);
                continue;
            }
            // A record is left out for the first state it differs in, so all of these differ
            // the same way: on when recorded and off with this GPU and settings, or the reverse.
            detail += fmt::format("{} recorded with {} {}", unfit[reason], unfit_reasons[reason],
                                  unfit_recorded_on[reason] != 0 ? "on (off here)"
                                                                 : "off (on here)");
        }
        LOG_WARNING(Render_Vulkan, "Pipeline cache: {} of {} cached graphics pipelines left out{}",
                    total_unfit, graphics_records, detail);
    }
    if (skipped_pipelines != 0) {
        LOG_INFO(Render_Vulkan,
                 "{} cached graphics pipelines skipped at boot, {} {} for background prebuild",
                 skipped_pipelines, precache ? prebuild.size() : deferred_pipelines,
                 precache ? "held" : "admitted");
    }
    {
        std::scoped_lock lock{state.mutex};
        callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
        state.has_loaded = true;
    }
    boot.Wait();
    ReportDiskRecordFailures();
    if (state.shared_keys != 0) {
        LOG_INFO(Render_Vulkan,
                 "Pipeline cache: {} graphics records share their pipeline with an earlier one "
                 "(a logic op or vertex attribute types the draws don't key); each pipeline is "
                 "built once",
                 state.shared_keys);
    }

    std::vector<DuplicateGraphicsRecord> no_duplicates;
    AddIncompleteGraphics(state.incomplete_graphics, no_duplicates, disk_pipelines,
                          state.incomplete);
    // The records the load never chooses go by tombstones, and the log is compacted once a
    // quarter of it is records no load reads.
    if (!stop_loading.stop_requested()) {
        for (const auto* removed : {&state.incomplete, &state.copies}) {
            for (const VideoCommon::PipelineRecord& record : *removed) {
                const CacheV2::LogRecord& read = *records[record.begin];
                cache_v2->Writer().Append(RecordKind::Tombstone, read.key,
                                          CacheV2::TombstonePayload(read.kind));
            }
        }
        const CacheV2::LogStats& stats = log->parsed.stats;
        if (stats.dead_bytes != 0 && stats.dead_bytes >= stats.file_size / 4) {
            cache_v2->Writer().Compact();
        }
    }
    records.clear();
    log.reset();

    if (use_vulkan_pipeline_cache) {
        // On the serialization thread: for a cache of tens of thousands of pipelines, reading
        // the driver data (gigabytes) and writing it can hold the boot at 100% long enough to
        // look frozen. It follows any write the boot build queued.
        QueueVulkanPipelineCacheSave(true);
        last_flush = std::chrono::steady_clock::now();
    }
    if (state.statistics) {
        state.statistics->Report();
    }
    if (StutterTrace::Enabled()) {
        const auto bytes = [](const std::filesystem::path& path) {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            return error ? std::uintmax_t{0} : size;
        };
        StutterTrace::SetCacheFile(Common::FS::PathToUTF8String(pipeline_cache_filename));
        StutterTrace::BootMarker(
            "shader_load_end",
            fmt::format("seconds={:.3f} graphics_records={} compute_records={} queued={} skipped={} "
                        "deferred={} cache_bytes={} driver_cache_bytes={}",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - load_begin)
                            .count(),
                        graphics_records, compute_records, state.total, skipped_pipelines,
                        precache ? prebuild.size() : deferred_pipelines,
                        bytes(pipeline_cache_filename), bytes(vulkan_pipeline_cache_filename)));
    }
    if (precache && !stop_loading.stop_requested()) {
        PrecacheShaders(title_id, std::move(precache_graphics), std::move(precached_pipelines),
                        stop_loading, callback);
        ReportDiskRecordFailures();
    }
    if (stop_loading.stop_requested()) {
        scheduled_prebuild.reset();
        return;
    }
    if (precache) {
        start_prebuild();
        LOG_INFO(Render_Vulkan, "{} cached graphics pipelines admitted for background prebuild",
                 deferred_pipelines);
    }
    // Stopping emulation also cancels the background builds at once: they would otherwise
    // compile on until the cache is destroyed, competing with the teardown for the CPU.
    loading_stop_callback.emplace(
        stop_loading, std::function<void()>{[prebuild_jobs = &jobs,
                                              prebuild = scheduled_prebuild->Batch()] {
            prebuild_jobs->Cancel(prebuild);
        }});
}

namespace {
/// The slots of a PrecacheJournal, one per build in flight
class JournalSlots {
public:
    static constexpr size_t None = ~size_t{0};

    explicit JournalSlots(size_t count) : free(count) {
        std::iota(free.rbegin(), free.rend(), size_t{0});
    }

    /// None when every slot is taken: that build goes unjournaled
    size_t Take() {
        std::scoped_lock lock{mutex};
        if (free.empty()) {
            return None;
        }
        const size_t slot = free.back();
        free.pop_back();
        return slot;
    }

    void Give(size_t slot) {
        std::scoped_lock lock{mutex};
        free.push_back(slot);
    }

private:
    std::mutex mutex;
    std::vector<size_t> free;
};

/// A prepared stamp ("size modified driver version model") without its second field, the
/// precompiled file's time; empty when it has no such field.
std::string StampWithoutTime(const std::string& stamp) {
    const size_t first = stamp.find(' ');
    const size_t second = first == std::string::npos ? first : stamp.find(' ', first + 1);
    if (second == std::string::npos) {
        return {};
    }
    return stamp.substr(0, first) + stamp.substr(second);
}
} // Anonymous namespace

void PipelineCache::PrecacheShaders(u64 title_id, std::vector<PrecacheJob> jobs,
                                    std::unordered_set<u64> precached, std::stop_token stop_loading,
                                    const VideoCore::DiskResourceLoadCallback& callback) {
    namespace ShaderWarm = VideoCommon::ShaderWarm;
    const size_t graphics_jobs = jobs.size();
    const std::filesystem::path warm_file = ShaderWarm::ComputeWarmPath(title_id);
    const std::string path = Common::FS::PathToUTF8String(warm_file);
    const auto stamp = [&](u64 entries) {
        return ShaderWarm::CurrentWarmStamp(warm_file, entries, device.GetDriverVersion(),
                                            device.GetDeviceID(), CACHE_VERSION);
    };
    // Compute programs of the precache file, unless a matching marker says they were built.
    size_t file_entries = 0;
    bool compute_pending = false;
    std::error_code ec;
    const auto current = stamp(0);
    const auto marker = ShaderWarm::ReadWarmMarker(warm_file);
    if (!std::filesystem::exists(warm_file, ec)) {
        LOG_INFO(Render_Vulkan, "Shader precache: no precache file at {}", path);
    } else if (current && marker && marker->Matches(*current)) {
        LOG_INFO(Render_Vulkan, "Shader precache: compute programs already done for this driver");
    } else {
        // Read the whole file here (its own reader: the disk cache loader deletes files it rejects).
        try {
            std::ifstream file(warm_file, std::ios::binary | std::ios::ate);
            file.exceptions(std::ifstream::failbit);
            const auto end{file.tellg()};
            file.seekg(0, std::ios::beg);
            std::array<char, 8> magic;
            u32 version{};
            file.read(magic.data(), magic.size())
                .read(reinterpret_cast<char*>(&version), sizeof(version));
            constexpr std::array<char, 8> expected_magic{'y', 'u', 'z', 'u', 'c', 'a', 'c', 'h'};
            if (magic != expected_magic || version != CACHE_VERSION) {
                LOG_WARNING(Render_Vulkan, "Shader precache: {} is not a version {} file", path,
                            CACHE_VERSION);
                throw std::runtime_error("wrong version");
            }
            while (file.tellg() != end) {
                u32 num_envs{};
                file.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
                FileEnvironment env;
                env.Deserialize(file, end);
                if (num_envs != 1 || env.ShaderStage() != Shader::Stage::Compute) {
                    LOG_WARNING(Render_Vulkan, "Shader precache: unexpected entry in {}", path);
                    throw std::runtime_error("unexpected entry");
                }
                ComputePipelineCacheKey key;
                file.read(reinterpret_cast<char*>(&key), sizeof(key));
                ++file_entries;
                // Programs already in the disk cache were built above; leave them out.
                if (compute_cache.contains(key)) {
                    continue;
                }
                const u64 cpu_bytes = TranslationBytes(env.MemorySize());
                jobs.push_back({
                    .translate = [this, key, compute_env = std::move(env)]() mutable
                    -> Common::UniqueFunction<bool> {
                        static const GuessMissing guess{};
                        compute_env.SetGuessMissing(&guess);
                        ShaderPools pools;
                        // Never deferred: the pipeline is dropped once built.
                        auto pipeline = CreateComputePipeline(pools, key, compute_env, nullptr,
                                                              false, nullptr, false)
                                            .pipeline;
                        if (!pipeline) {
                            return {};
                        }
                        return [pipeline = std::move(pipeline)] {
                            pipeline->Build();
                            return true;
                        };
                    },
                    .cpu_bytes = cpu_bytes,
                    .compute_hash = key.unique_hash,
                });
            }
            compute_pending = true;
        } catch (const std::exception& e) {
            LOG_WARNING(Render_Vulkan, "Shader precache: reading {} failed: {}", path, e.what());
            jobs.resize(graphics_jobs);
            file_entries = 0;
        }
    }
    // Fritchii's precompiled pipelines (a recipe for this title): built with the rest, here.
    const size_t precompiled_first = jobs.size();
    const auto precompiled = AddPrecompiledJobs(title_id, jobs, stop_loading);
    if (jobs.empty() && !compute_pending) {
        LOG_INFO(Render_Vulkan, "Shader precache: nothing to precache");
        return;
    }

    const auto begin = std::chrono::steady_clock::now();
    const size_t num_threads = GetTotalPipelineWorkers();
    // Builds a driver crash ended an earlier precache on are skipped (precache_journal.h). Cached
    // graphics pipelines and compute programs are named by their hashes, which don't depend on a
    // file: the journal belongs to the driver and the pipeline cache version. Its slots are
    // taken by the driver stages in flight, at most one per shader job thread.
    const size_t slot_count = 2 * num_threads;
    JournalSlots journal_slots{slot_count};
    PrecacheJournal journal{
        ShaderWarm::ComputeWarmPath(title_id).parent_path() / "precache-jobs",
        fmt::format("{} {} {}", device.GetDriverVersion(), device.GetDeviceID(), CACHE_VERSION),
        slot_count, "pipeline"};
    // 0: not done yet, 1: built, 2: done without a pipeline. Read while the workers run, to save
    // how far the precompiled records got.
    std::vector<std::atomic<u8>> built(jobs.size());
    std::atomic<size_t> done{};
    bool skipped = false;
    const auto save_progress = [&] {
        if (!precompiled || !precompiled_counts) {
            return;
        }
        size_t next = precompiled_first;
        while (next < jobs.size() && built[next].load() != 0) {
            ++next;
        }
        std::ofstream progress_file(precompiled_counts->progress_path,
                                    std::ios::binary | std::ios::trunc);
        progress_file << precompiled_counts->stamp
                      << precompiled_counts->resumed + (next - precompiled_first) << '\n';
    };
    auto next_save = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    const auto save_now_and_then = [&] {
        if (const auto now = std::chrono::steady_clock::now(); now >= next_save) {
            save_progress();
            next_save = now + std::chrono::seconds{30};
        }
    };
    LOG_INFO(Render_Vulkan,
             "Shader precache: {} cached graphics pipelines, {} of {} compute programs and {} "
             "precompiled pipelines to build on {} threads",
             graphics_jobs, precompiled_first - graphics_jobs, file_entries,
             jobs.size() - precompiled_first, num_threads);
    VideoCommon::StutterTrace::BootMarker("precache_begin", fmt::format("jobs={}", jobs.size()));
    if (!jobs.empty()) {
        // Background jobs of the shader workers, each built and destroyed: only the driver's
        // cache keeps it. A build a driver crash ended an earlier precache on is left out, and
        // each driver stage holds a journal slot while it runs.
        const VideoCommon::ShaderJobBatch batch = shader_jobs->CreateBatch();
        // At most this many jobs are submitted and not done. The scheduler's budget counts only
        // running stages, so translated pipelines waiting for a driver worker would pile up:
        // with the ~120k precompiled records, tens of GB.
        const size_t window = std::max<size_t>(4 * num_threads, 32);
        // Each CPU worker also runs its job's driver build, as before the merge: the driver
        // pool has half the threads, and the builds bound the precache (r42 took about twice
        // as long). Not where parallel builds are broken.
        const bool build_on_cpu_workers = !device.HasBrokenParallelShaderCompiling();
        for (size_t index = 0; index < jobs.size(); ++index) {
            while (index - done.load() >= window) {
                if (stop_loading.stop_requested() || ShaderWarm::TakeSkipWarm()) {
                    skipped = true;
                    break;
                }
                if (callback) {
                    callback(VideoCore::LoadCallbackStage::Warm, done, jobs.size());
                }
                save_now_and_then();
                std::this_thread::sleep_for(std::chrono::milliseconds{5});
            }
            if (skipped) {
                break;
            }
            const u64 id = jobs[index].graphics_hash != 0 ? jobs[index].graphics_hash
                                                          : jobs[index].compute_hash;
            if (journal.Poisoned(id)) {
                jobs[index].translate = {};
                built[index] = 2;
                done.fetch_add(1);
                continue;
            }
            shader_jobs->Submit(
                {
                    .priority = VideoCommon::ShaderJobPriority::Background,
                    .cpu_bytes = jobs[index].cpu_bytes +
                                 (build_on_cpu_workers ? DriverCompileBytes : 0),
                    .driver_bytes = DriverCompileBytes,
                    .cpu = [&built, &done, &journal, &journal_slots, build_on_cpu_workers, index,
                            id, translate = std::move(jobs[index].translate)](
                               VideoCommon::ShaderJobContext& context) mutable
                    -> VideoCommon::ShaderDriverStage {
                        Common::UniqueFunction<bool> build;
                        const bool stopped = context.StopRequested();
                        if (!stopped) {
                            try {
                                build = translate();
                            } catch (const std::exception& e) {
                                LOG_DEBUG(Render_Vulkan, "Shader precache: {}", e.what());
                            }
                        }
                        translate = {};
                        if (!build) {
                            // A stopped job stays not done, so the next launch builds it.
                            if (!stopped) {
                                built[index] = 2;
                            }
                            done.fetch_add(1);
                            return {};
                        }
                        auto driver_stage = [&built, &done, &journal, &journal_slots, index, id,
                                             pipeline_build = std::move(build)]() mutable {
                            const size_t slot = journal_slots.Take();
                            if (slot != JournalSlots::None) {
                                journal.Begin(slot, id);
                            }
                            u8 result = 2;
                            try {
                                if (pipeline_build()) {
                                    result = 1;
                                }
                            } catch (const std::exception& e) {
                                LOG_DEBUG(Render_Vulkan, "Shader precache: {}", e.what());
                            }
                            built[index] = result;
                            if (slot != JournalSlots::None) {
                                journal.End(slot);
                                journal_slots.Give(slot);
                            }
                            done.fetch_add(1);
                        };
                        if (build_on_cpu_workers) {
                            driver_stage();
                            return {};
                        }
                        return VideoCommon::ShaderDriverStage{std::move(driver_stage)};
                    },
                },
                &batch);
        }
        if (skipped) {
            // The builds not started are dropped; the running ones end first.
            shader_jobs->Cancel(batch);
        }
        while (!skipped && !batch.Idle()) {
            if (stop_loading.stop_requested() || ShaderWarm::TakeSkipWarm()) {
                // The builds not started are dropped; the running ones end first.
                shader_jobs->Cancel(batch);
                skipped = true;
                break;
            }
            if (callback) {
                callback(VideoCore::LoadCallbackStage::Warm, done, jobs.size());
            }
            save_now_and_then();
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        batch.Wait();
    }
    if (skipped || stop_loading.stop_requested()) {
        save_progress();
    }
    const size_t total_built = static_cast<size_t>(std::count(built.begin(), built.end(), u8{1}));
    if (compute_pending && !skipped) {
        if (const auto stamp_done = stamp(file_entries)) {
            ShaderWarm::WriteWarmMarker(warm_file, *stamp_done);
        }
    }
    if (precompiled && !skipped && !stop_loading.stop_requested()) {
        const size_t precompiled_built = static_cast<size_t>(
            std::count(built.begin() + static_cast<std::ptrdiff_t>(precompiled_first), built.end(),
                       u8{1}));
        std::string unbuildable;
        size_t unbuildable_total = 0;
        if (precompiled_counts) {
            for (size_t index = 0; index < PrecompiledCounts::reasons.size(); ++index) {
                if (const size_t count = precompiled_counts->unbuildable[index].load()) {
                    unbuildable += fmt::format("{}{} {}", unbuildable.empty() ? "" : ", ", count,
                                               PrecompiledCounts::reasons[index]);
                    unbuildable_total += count;
                }
            }
        }
        const size_t total = jobs.size() - precompiled_first;
        const size_t resumed = precompiled_counts ? precompiled_counts->resumed : 0;
        LOG_INFO(Render_Vulkan,
                 "Precompiled pipelines: {} of {} prepared ({} already in the disk cache), {} not "
                 "buildable on this device{}{}{}, {} failed{}",
                 precompiled_built, total,
                 precompiled_counts ? precompiled_counts->known.load() : 0, unbuildable_total,
                 unbuildable.empty() ? "" : " (", unbuildable, unbuildable.empty() ? "" : ")",
                 total - precompiled_built - unbuildable_total,
                 resumed != 0 ? fmt::format("; {} more in earlier runs", resumed) : "");
        std::ofstream stamp_file(precompiled->first, std::ios::binary | std::ios::trunc);
        stamp_file << precompiled->second;
        if (precompiled_counts) {
            std::error_code remove_error;
            std::filesystem::remove(precompiled_counts->progress_path, remove_error);
        }
    }
    const size_t precached_before = precached.size();
    for (size_t index = 0; index < graphics_jobs; ++index) {
        if (built[index] == 1) {
            precached.insert(jobs[index].graphics_hash);
        }
    }
    if (precached.size() != precached_before) {
        ShaderWarm::WritePrecachedPipelines(
            ShaderWarm::PrecachedPipelinesPath(title_id),
            {device.GetDriverVersion(), device.GetDeviceID(), CACHE_VERSION}, precached);
    }
    if (!stop_loading.stop_requested() && total_built != 0) {
        // Without this, the builds reach the file only at the next boot, game change or clean
        // close, and are lost if the app ends otherwise.
        QueueVulkanPipelineCacheSave(true);
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
    LOG_INFO(Render_Vulkan, "Shader precache: {}, {} built, {} failed, {} not reached, {:.1f} s",
             stop_loading.stop_requested() ? "stopped" : (skipped ? "skipped" : "finished"),
             total_built, done - total_built, jobs.size() - std::min(jobs.size(), done.load()),
             seconds);
    VideoCommon::StutterTrace::BootMarker(
        "precache_end", fmt::format("built={} seconds={:.3f}", total_built, seconds));
}

bool PipelineCache::RecordedKeyForThisDevice(GraphicsPipelineCacheKey& key,
                                             std::span<FileEnvironment> envs,
                                             size_t* reason) const {
    // As Fritchii v5.5's RecordedKeyForThisDevice. The driver fields only tell cache generations
    // apart; the pipeline does not use them, so the record is built for this driver.
    auto& fixed = key.state;
    fixed.driver_id = dynamic_features.driver_id;
    fixed.driver_version = dynamic_features.driver_version;
    // Recorded with a dynamic logic op (NVIDIA: the Ocarina recipe), built where it is static
    // (AMD's Windows driver; Fritchii 9ea66c827): with the logic op off the op does not count, so
    // the record is the key a draw gives here. With it on the op is unknown: left out below.
    if (fixed.extended_dynamic_state_2_logic_op != 0 &&
        !dynamic_features.has_extended_dynamic_state_2_logic_op &&
        !dynamic_features.has_dynamic_state3_logic_op_enable &&
        fixed.dynamic_state.logic_op_enable == 0) {
        fixed.extended_dynamic_state_2_logic_op.Assign(0);
    }
    // One key for every op while the logic op is off, as draws make them (Refresh).
    if (fixed.extended_dynamic_state_2_logic_op == 0 &&
        !dynamic_features.has_dynamic_state3_logic_op_enable &&
        fixed.dynamic_state.logic_op_enable == 0) {
        fixed.dynamic_state.logic_op.Assign(0);
    }
    // Made for a device with other dynamic states: left out, as Fritchii leaves it.
    const std::array<std::pair<bool, bool>, 7> features{{
        {fixed.extended_dynamic_state != 0, dynamic_features.has_extended_dynamic_state},
        {fixed.extended_dynamic_state_2 != 0, dynamic_features.has_extended_dynamic_state_2},
        {fixed.extended_dynamic_state_2_logic_op != 0,
         dynamic_features.has_extended_dynamic_state_2_logic_op},
        {fixed.extended_dynamic_state_3_blend != 0,
         dynamic_features.has_extended_dynamic_state_3_blend},
        {fixed.extended_dynamic_state_3_enables != 0,
         dynamic_features.has_extended_dynamic_state_3_enables},
        {fixed.color_write_enable_dynamic != 0, dynamic_features.has_color_write_enable},
        {fixed.dynamic_vertex_input != 0, dynamic_features.has_dynamic_vertex_input},
    }};
    for (size_t index = 0; index < features.size(); ++index) {
        if (features[index].first != features[index].second) {
            if (reason != nullptr) {
                *reason = index;
            }
            return false;
        }
    }
    if (fixed.dynamic_vertex_input != 0) {
        std::array<u64, Maxwell::MaxShaderProgram> attribute_masks;
        attribute_masks.fill(~u64{0});
        size_t env_index = 0;
        for (size_t stage = 0; stage < Maxwell::MaxShaderProgram; ++stage) {
            if (key.unique_hashes[stage] != 0 && env_index < envs.size()) {
                attribute_masks[stage] = VideoCommon::VertexAttributeMaskOf(envs[env_index++]);
            }
        }
        fixed.attribute_types = VideoCommon::KeyedAttributeTypes(
            fixed.attribute_types, key.unique_hashes, attribute_masks);
    }
    if ((fixed.provoking_vertex_last != 0 && !dynamic_features.has_provoking_vertex_last_mode) ||
        (fixed.xfb_enabled != 0 && fixed.provoking_vertex_last != 0 &&
         !dynamic_features.has_provoking_vertex_tf_preserve)) {
        if (reason != nullptr) {
            *reason = features.size();
        }
        return false;
    }
    return true;
}

std::optional<std::pair<std::filesystem::path, std::string>> PipelineCache::AddPrecompiledJobs(
    u64 title_id, std::vector<PrecacheJob>& jobs, std::stop_token stop_loading) {
    namespace Precompile = VideoCommon::Nx2Precompile;
    if (Precompile::Mode() == 0) {
        return std::nullopt;
    }
    const auto base_dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir) /
                          fmt::format("{:016x}", title_id);
    // Assembled from the recipe and the game's files when missing or older than the recipe
    // (tens of seconds, about the file's size in memory).
    bool noted = false;
    const auto path =
        Precompile::EnsurePrecompiledFile(title_id, base_dir, CACHE_VERSION, stop_loading, &noted);
    if (path.empty() || stop_loading.stop_requested()) {
        return std::nullopt;
    }
    const std::string path_text = Common::FS::PathToUTF8String(path);
    std::error_code error;
    const u64 file_size = std::filesystem::file_size(path, error);
    if (error) {
        return std::nullopt;
    }
    const auto modified = std::filesystem::last_write_time(path, error).time_since_epoch().count();
    // Prepared once per file, driver, GPU and release: the driver's cache keeps the pipelines.
    const auto stamp_path = Precompile::PreparedStampPath(base_dir, CACHE_VERSION);
    const std::string stamp =
        fmt::format("{} {} {:#x} {} {}\n", file_size, modified, device.GetDriverVersion(),
                    Precompile::PreparationVersion, device.GetModelName());
    // A copied profile (an import, a restored backup) can give the file another time. A stamp
    // that differs from this one only there still holds when the file's note names the recipe.
    const auto only_time_differs = [&stamp, noted](const std::string& line) {
        const std::string rest = StampWithoutTime(line + '\n');
        return noted && !rest.empty() && rest == StampWithoutTime(stamp);
    };
    std::string prepared_line;
    if (!Precompile::Forced()) {
        std::ifstream stamp_file(stamp_path, std::ios::binary);
        if (std::getline(stamp_file, prepared_line) && prepared_line + '\n' == stamp) {
            LOG_INFO(Render_Vulkan,
                     "Precompiled pipelines: {} was prepared for this driver already", path_text);
            return std::nullopt;
        }
    }
    // An unfinished preparation of the same file, driver and version resumes where it stopped.
    const auto progress_path = Precompile::PreparedProgressPath(base_dir, CACHE_VERSION);
    size_t resume = 0;
    if (!Precompile::Forced()) {
        std::ifstream progress_file(progress_path, std::ios::binary);
        std::string progress_key, progress_done;
        const bool counted =
            std::getline(progress_file, progress_key) && std::getline(progress_file, progress_done);
        // A copy of a prepared file, unless a preparation of the file as it is now was under way.
        if (progress_key + '\n' != stamp && only_time_differs(prepared_line)) {
            std::ofstream rewritten(stamp_path, std::ios::binary | std::ios::trunc);
            rewritten << stamp;
            LOG_INFO(Render_Vulkan,
                     "Precompiled pipelines: {} was prepared for this driver already; only its "
                     "time changed (a copied profile), so its stamp is updated",
                     path_text);
            return std::nullopt;
        }
        if (counted && (progress_key + '\n' == stamp || only_time_differs(progress_key))) {
            resume = static_cast<size_t>(std::strtoull(progress_done.c_str(), nullptr, 10));
            if (progress_key + '\n' != stamp) {
                LOG_INFO(Render_Vulkan,
                         "Precompiled pipelines: {} resumes its preparation; only its time "
                         "changed (a copied profile)",
                         path_text);
            }
        }
    }
    // Layout (Nx2Recipe): "NX2PREP1", u32 cache version, u32 environment count, u32 record count,
    // u32 reserved; per environment u64 offset and u64 size of a serialized environment; per
    // record u8 kind (0 graphics, 1 compute), u8 environment count, u16 reserved, u32 environment
    // indices in stage order, then the key. Records are read again when built.
    auto env_index = std::make_shared<std::vector<std::pair<u64, u64>>>();
    std::vector<u64> record_offsets;
    // Each record's key hash as recorded: its name in the precache journal, so a record a driver
    // crash ended is skipped at the next launch (precache_journal.h).
    std::vector<std::pair<u64, bool>> record_ids;
    std::vector<u64> record_bytes; ///< Its environments' bytes, for the job's memory estimate
    try {
        std::ifstream in(path, std::ios::binary);
        in.exceptions(std::ifstream::failbit);
        std::array<char, 8> magic{};
        u32 version{}, env_count{}, record_count{}, reserved{};
        in.read(magic.data(), magic.size())
            .read(reinterpret_cast<char*>(&version), sizeof(version))
            .read(reinterpret_cast<char*>(&env_count), sizeof(env_count))
            .read(reinterpret_cast<char*>(&record_count), sizeof(record_count))
            .read(reinterpret_cast<char*>(&reserved), sizeof(reserved));
        if (std::string_view{magic.data(), magic.size()} != "NX2PREP1" ||
            version != CACHE_VERSION) {
            LOG_WARNING(Render_Vulkan, "Precompiled pipeline file {} has another format or version",
                        path_text);
            return std::nullopt;
        }
        if (u64{env_count} * 16 > file_size || u64{record_count} * 8 > file_size) {
            LOG_ERROR(Render_Vulkan, "Precompiled pipeline file {} is damaged", path_text);
            return std::nullopt;
        }
        env_index->resize(env_count);
        in.read(reinterpret_cast<char*>(env_index->data()),
                static_cast<std::streamsize>(env_count * sizeof((*env_index)[0])));
        if (std::ranges::any_of(*env_index, [&](const auto& entry) {
                return entry.first > file_size || entry.second > file_size - entry.first;
            })) {
            LOG_ERROR(Render_Vulkan, "Precompiled pipeline file {} is damaged", path_text);
            return std::nullopt;
        }
        record_offsets.reserve(record_count);
        record_ids.reserve(record_count);
        record_bytes.reserve(record_count);
        std::array<u32, Maxwell::MaxShaderProgram> ids{};
        for (u32 i = 0; i < record_count; ++i) {
            record_offsets.push_back(static_cast<u64>(in.tellg()));
            u8 kind{}, count{};
            u16 pad{};
            in.read(reinterpret_cast<char*>(&kind), 1)
                .read(reinterpret_cast<char*>(&count), 1)
                .read(reinterpret_cast<char*>(&pad), 2);
            const bool compute = kind == 1;
            if (count == 0 || count > ids.size() || kind > 1 || (compute && count != 1)) {
                LOG_ERROR(Render_Vulkan, "Precompiled pipeline file {} is damaged", path_text);
                return std::nullopt;
            }
            in.read(reinterpret_cast<char*>(ids.data()), count * sizeof(u32));
            if (std::any_of(ids.begin(), ids.begin() + count,
                            [&](u32 id) { return id >= env_count; })) {
                LOG_ERROR(Render_Vulkan, "Precompiled pipeline file {} is damaged", path_text);
                return std::nullopt;
            }
            u64 bytes = 0;
            for (size_t stage = 0; stage < count; ++stage) {
                bytes += (*env_index)[ids[stage]].second;
            }
            record_bytes.push_back(bytes);
            if (compute) {
                ComputePipelineCacheKey key{};
                in.read(reinterpret_cast<char*>(&key), sizeof(key));
                record_ids.emplace_back(key.unique_hash, true);
            } else {
                GraphicsPipelineCacheKey key{};
                in.read(reinterpret_cast<char*>(&key), sizeof(key));
                record_ids.emplace_back(key.Hash(), false);
            }
        }
    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Render_Vulkan, "Cannot read precompiled pipelines from {}: {}", path_text,
                  e.what());
        return std::nullopt;
    }
    // Graphics pipelines the disk cache holds were built at boot already.
    const auto known = std::make_shared<const std::unordered_set<u64>>(disk_pipelines);
    const auto counts = std::make_shared<PrecompiledCounts>();
    resume = std::min(resume, record_offsets.size());
    counts->resumed = resume;
    counts->progress_path = progress_path;
    counts->stamp = stamp;
    precompiled_counts = counts;
    for (size_t record = resume; record < record_offsets.size(); ++record) {
        const u64 offset = record_offsets[record];
        const auto [record_id, record_compute] = record_ids[record];
        // The record is read again and translated in the job's CPU stage; its driver stage
        // builds the pipeline, which is then dropped: only the driver's cache keeps it.
        jobs.push_back({
            .graphics_hash = record_compute ? 0 : record_id,
            .translate = [this, path, env_index, known, counts,
                          offset]() -> Common::UniqueFunction<bool> {
                std::ifstream in(path, std::ios::binary);
                in.exceptions(std::ifstream::failbit);
                in.seekg(static_cast<std::streamoff>(offset));
                u8 kind{}, count{};
                u16 pad{};
                std::array<u32, Maxwell::MaxShaderProgram> ids{};
                in.read(reinterpret_cast<char*>(&kind), 1)
                    .read(reinterpret_cast<char*>(&count), 1)
                    .read(reinterpret_cast<char*>(&pad), 2)
                    .read(reinterpret_cast<char*>(ids.data()), count * sizeof(u32));
                const bool compute = kind == 1;
                ComputePipelineCacheKey compute_key{};
                GraphicsPipelineCacheKey record_key{};
                if (compute) {
                    in.read(reinterpret_cast<char*>(&compute_key), sizeof(compute_key));
                } else {
                    in.read(reinterpret_cast<char*>(&record_key), sizeof(record_key));
                }
                std::vector<FileEnvironment> envs(count);
                for (size_t i = 0; i < count; ++i) {
                    const auto [env_offset, env_size] = (*env_index)[ids[i]];
                    in.clear();
                    in.seekg(static_cast<std::streamoff>(env_offset));
                    envs[i].Deserialize(in, static_cast<std::streamoff>(env_offset + env_size));
                }
                ShaderPools pools;
                if (compute) {
                    // Never deferred: the pipeline is dropped once built.
                    auto compute_pipeline = CreateComputePipeline(pools, compute_key, envs.front(),
                                                                  nullptr, false, nullptr, false)
                                                .pipeline;
                    if (!compute_pipeline) {
                        return {};
                    }
                    return [compute_pipeline = std::move(compute_pipeline)] {
                        compute_pipeline->Build();
                        return compute_pipeline->IsBound();
                    };
                }
                if (size_t reason = 0; !RecordedKeyForThisDevice(record_key, envs, &reason)) {
                    ++counts->unbuildable[reason];
                    return {};
                }
                if (known->contains(record_key.Hash())) {
                    ++counts->known;
                    return [] { return true; };
                }
                boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram>
                    env_ptrs;
                for (auto& env : envs) {
                    env_ptrs.push_back(&env);
                }
                auto pipeline = CreateGraphicsPipeline(pools, record_key, MakeSpan(env_ptrs),
                                                       nullptr, false);
                if (!pipeline) {
                    return {};
                }
                return [pipeline = std::move(pipeline)] {
                    pipeline->Build();
                    return !pipeline->Failed();
                };
            },
            .cpu_bytes = TranslationBytes(record_bytes[record]),
            .compute_hash = record_compute ? record_id : 0,
        });
    }
    LOG_INFO(Render_Vulkan, "Precompiled pipelines: {} records of {} to prepare{}",
             record_offsets.size() - resume, path_text,
             resume != 0 ? fmt::format(", resuming after the first {}", resume) : "");
    return std::make_pair(stamp_path, stamp);
}

void PipelineCache::QueueVulkanPipelineCacheFlush() {
    if (!use_vulkan_pipeline_cache || vulkan_pipeline_cache_filename.empty()) {
        return;
    }
    if (++pipelines_since_flush < VULKAN_CACHE_FLUSH_PIPELINES) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto megabytes = last_cache_size.load(std::memory_order_relaxed) / (1024 * 1024);
    const std::chrono::seconds interval{
        std::max<size_t>(VULKAN_CACHE_FLUSH_MIN_SECONDS, megabytes)};
    if (last_flush.time_since_epoch().count() != 0 && now - last_flush < interval) {
        return;
    }
    if (flush_in_flight.load(std::memory_order_acquire)) {
        return;
    }
    pipelines_since_flush = 0;
    last_flush = now;
    QueueVulkanPipelineCacheSave(false);
}

bool PipelineCache::VulkanPipelineCacheSaveDue(std::chrono::steady_clock::time_point& last) const {
    // As QueueVulkanPipelineCacheFlush: at most once a second per MiB the cache holds, as each
    // write copies all of it.
    const auto now = std::chrono::steady_clock::now();
    const auto megabytes = last_cache_size.load(std::memory_order_relaxed) / (1024 * 1024);
    const std::chrono::seconds interval{
        std::max<size_t>(VULKAN_CACHE_FLUSH_MIN_SECONDS, megabytes)};
    if (now - last < interval) {
        return false;
    }
    last = now;
    return true;
}

void PipelineCache::QueueVulkanPipelineCacheSave(bool again_if_busy) {
    if (!use_vulkan_pipeline_cache || vulkan_pipeline_cache_filename.empty()) {
        return;
    }
    if (flush_in_flight.exchange(true, std::memory_order_acq_rel)) {
        // The write under way may have read the driver data already: one more follows it.
        if (again_if_busy) {
            save_again.store(true, std::memory_order_release);
        }
        return;
    }
    serialization_thread.QueueWork([this] {
        do {
            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                         CACHE_VERSION);
            size_t size = 0;
            vulkan_pipeline_cache.Read(&size, nullptr);
            last_cache_size.store(size, std::memory_order_relaxed);
        } while (save_again.exchange(false, std::memory_order_acq_rel));
        flush_in_flight.store(false, std::memory_order_release);
    });
}

namespace {
// EDEN_NX2_PIPELINE_DIFF=1: for a new graphics pipeline whose shaders an earlier one uses too,
// log which parts of the fixed state differ from that one (finds state that splits the same
// draws into many pipelines, each translated on the GPU thread).
void ReportNewPipelineKey(const GraphicsPipelineCacheKey& key) {
    static const bool enabled = std::getenv("EDEN_NX2_PIPELINE_DIFF") != nullptr;
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    static std::unordered_map<u64, GraphicsPipelineCacheKey> last_by_shaders;
    static u32 logged{};
    const u64 shaders = Common::CityHash64(reinterpret_cast<const char*>(key.unique_hashes.data()),
                                           sizeof(key.unique_hashes));
    const auto [it, first] = last_by_shaders.try_emplace(shaders, key);
    if (first) {
        return;
    }
    const FixedPipelineState& now = key.state;
    const FixedPipelineState& before = it->second.state;
    std::string diffs;
    if (now.raw1 != before.raw1) diffs += fmt::format(" raw1^{:#x}", now.raw1 ^ before.raw1);
    if (now.raw2 != before.raw2) diffs += fmt::format(" raw2^{:#x}", now.raw2 ^ before.raw2);
    if (now.color_formats != before.color_formats) {
        diffs += fmt::format(" color_formats {:02x} -> {:02x}", fmt::join(before.color_formats, ""),
                             fmt::join(now.color_formats, ""));
    }
    if (now.alpha_test_ref != before.alpha_test_ref) {
        diffs += fmt::format(" alpha_test_ref {:#x} -> {:#x} (func {})", before.alpha_test_ref,
                             now.alpha_test_ref, now.alpha_test_func.Value());
    }
    if (now.point_size != before.point_size) {
        diffs += fmt::format(" point_size {:#x} -> {:#x}", before.point_size, now.point_size);
    }
    if (now.viewport_swizzles != before.viewport_swizzles) diffs += " viewport_swizzles";
    if (now.attribute_types != before.attribute_types) {
        diffs += fmt::format(" attribute_types^{:#x}", now.attribute_types ^ before.attribute_types);
    }
    if (std::memcmp(&now.dynamic_state, &before.dynamic_state, sizeof(now.dynamic_state)) != 0) {
        diffs += " dynamic_state";
    }
    if (now.driver_id != before.driver_id || now.driver_version != before.driver_version) {
        diffs += " driver";
    }
    const size_t size = now.Size();
    if (diffs.empty() && size > offsetof(FixedPipelineState, attachments)) {
        diffs = fmt::format(" past attachments (key size {})", size);
    }
    it->second = key;
    if (logged++ < 400) {
        LOG_INFO(Render_Vulkan, "New pipeline for known shaders {:016x} differs in:{}", shaders,
                 diffs.empty() ? std::string{" nothing in the key prefix"} : diffs);
    }
}

// Without VK_EXT_mesh_shader: logs, once per shader set, what a skipped task/mesh pipeline would
// need from an emulation (task stage, per-primitive outputs, topology, output sizes).
template <typename Programs>
void ReportUnsupportedMeshPipeline(const GraphicsPipelineCacheKey& key, const Programs& programs) {
    static std::mutex mutex;
    static std::unordered_set<u64> seen;
    static u32 with_task{}, per_primitive{}, non_triangle{}, total{};
    u64 shaders{};
    for (const u64 hash : key.unique_hashes) {
        shaders = shaders * 31 + hash;
    }
    std::scoped_lock lock{mutex};
    if (!seen.insert(shaders).second) {
        return;
    }
    const Shader::IR::Program* task{};
    const Shader::IR::Program* mesh{};
    bool has_pixel{};
    for (size_t index = 0; index < programs.size(); ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        const auto& program{programs[index]};
        if (program.stage == Shader::Stage::Task) {
            task = &program;
        } else if (program.stage == Shader::Stage::Mesh) {
            mesh = &program;
        } else if (program.stage == Shader::Stage::Fragment) {
            has_pixel = true;
        }
    }
    ++total;
    if (mesh == nullptr) {
        LOG_WARNING(Render_Vulkan, "Mesh emulation survey: pipeline {:016x} without a mesh stage",
                    shaders);
        return;
    }
    const auto& info{mesh->mesh};
    u32 position{}, generic{}, other{};
    std::string others;
    for (const u32 line : info.vertex_lines) {
        const auto attribute{static_cast<Shader::IR::Attribute>(line)};
        if (attribute >= Shader::IR::Attribute::PositionX &&
            attribute <= Shader::IR::Attribute::PositionW) {
            ++position;
        } else if (Shader::IR::IsGeneric(attribute)) {
            ++generic;
        } else {
            ++other;
            others += fmt::format(" {}", line);
        }
    }
    with_task += task != nullptr;
    per_primitive += !info.primitive_lines.empty();
    non_triangle += info.topology != 4;
    LOG_WARNING(Render_Vulkan,
                "Mesh emulation survey #{}: task={} (payload {} words, wg {}x{}x{}) mesh wg "
                "{}x{}x{} max_vertices={} max_primitives={} topology={} vertex_lines={} "
                "(position {} generic {} other {}:{}) primitive_lines={} pixel={} | totals: "
                "task {} per-primitive {} non-triangle {}",
                total, task != nullptr, info.payload_words, task ? task->workgroup_size[0] : 0,
                task ? task->workgroup_size[1] : 0, task ? task->workgroup_size[2] : 0,
                mesh->workgroup_size[0], mesh->workgroup_size[1], mesh->workgroup_size[2],
                info.max_vertices, info.max_primitives, info.topology, info.vertex_lines.size(),
                position, generic, other, others, info.primitive_lines.size(), has_pixel,
                with_task, per_primitive, non_triangle);
}
} // Anonymous namespace

GraphicsPipeline* PipelineCache::CurrentGraphicsPipelineSlowPath() {
    namespace StutterTrace = VideoCommon::StutterTrace;
    const auto [pair, is_new]{graphics_cache.try_emplace(graphics_key)};
    auto& pipeline{pair->second};
    if (is_new) {
        ReportNewPipelineKey(graphics_key);
        if (scheduled_prebuild) {
            pipeline = scheduled_prebuild->Take(graphics_key);
        }
        if (StutterTrace::Enabled()) {
            StutterTrace::CountLookup(pipeline ? StutterTrace::Lookup::GraphicsBackground
                                      : disk_pipelines.contains(graphics_key.Hash())
                                          ? StutterTrace::Lookup::GraphicsRecorded
                                          : StutterTrace::Lookup::GraphicsNew);
        }
        if (!pipeline) {
            const auto keep = evicted_pointer_slots.Take(graphics_key.Hash());
            // EDEN_NX2_ASYNC_TRANSLATE=1/2: a draw that may be skipped has its new pipeline
            // translated on a translation thread, and is skipped (as for an unbuilt pipeline)
            // until it is ready. The entry stays empty meanwhile. A key evicted with storage
            // pointer slots to keep (#1143) is translated here.
            if (!keep && translate_queue && EnqueueTranslation()) {
                NoteTranslateSkip();
                return nullptr;
            }
            pipeline = CreateGraphicsPipeline(keep ? &*keep : nullptr);
        }
    } else if (!pipeline && translate_queue) {
        bool pending{};
        pipeline = AdoptTranslation(pending);
        if (pending) {
            return nullptr;
        }
    } else if (StutterTrace::Enabled() && pipeline && !pipeline->WasUsed()) {
        StutterTrace::CountLookup(StutterTrace::Lookup::GraphicsPrebuilt);
    }
    if (!pipeline) {
        return nullptr;
    }
    // Every 15 s as upstream, not round 17's 5 s: each save appends the session's whole Used record
    // to the v2 log.
    constexpr auto save_interval = std::chrono::seconds{15};
    if (used_this_session.insert(pipeline.get()).second &&
        std::chrono::steady_clock::now() - used_pipelines_saved > save_interval) {
        SaveUsedPipelines();
    }
    if (current_pipeline) {
        current_pipeline->AddTransition(pipeline.get());
    }
    current_pipeline = pipeline.get();
    current_pipeline->MarkUsed(frame_count);
    return BuiltPipeline(current_pipeline);
}

namespace {
// The worst frame of the last 10 s (TickFrame runs once per presented frame, on the GPU thread)
// and what the GPU thread spent on pipelines in it, so a hitch reads as "2 builds, 14 ms
// translation, 0 held" without a profiler. Held draws are counted when the worker waited for them
// (they belong to the frame being recorded, close enough). EDEN_NX2_PIPELINE_TIMING=1 also logs
// every frame over 33 ms.
void ReportWorstFrame() {
    struct Frame {
        std::chrono::steady_clock::duration time{};
        FrameTally tally{};
        std::chrono::nanoseconds held{};
        u64 skipped{};
    };
    static const bool every_slow_frame = [] {
        const char* value = std::getenv("EDEN_NX2_PIPELINE_TIMING");
        return value != nullptr && value[0] == '1';
    }();
    static std::chrono::steady_clock::time_point last_tick{};
    static std::chrono::steady_clock::time_point window{};
    static std::chrono::nanoseconds last_held{};
    static u64 last_skipped{};
    static Frame worst{};
    static u32 frames{};
    static u32 slow_frames{};
    static u32 logged_slow{};
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const auto describe = [&](const Frame& frame) {
        return fmt::format("{} pipelines built on the GPU thread in {:.1f} ms ({:.1f} ms "
                           "environments, {:.1f} ms translation), {:.1f} ms of draws held, {} "
                           "lookups skipped, {} translations queued in {:.1f} ms",
                           frame.tally.count, ms(frame.tally.builds),
                           ms(frame.tally.split.environments), ms(frame.tally.split.translation),
                           ms(frame.held), frame.skipped, frame.tally.queued,
                           ms(frame.tally.queued_time));
    };
    const auto now = std::chrono::steady_clock::now();
    const auto held_total = GraphicsPipeline::HoldTimeTotal();
    const u64 skipped_total = skipped_lookups.load(std::memory_order_relaxed);
    if (last_tick == std::chrono::steady_clock::time_point{}) {
        window = now;
    } else {
        const Frame frame{now - last_tick, frame_tally, held_total - last_held,
                          skipped_total - last_skipped};
        ++frames;
        if (frame.time > std::chrono::milliseconds{33}) {
            ++slow_frames;
            if (every_slow_frame && logged_slow < 2000) {
                ++logged_slow;
                LOG_INFO(Render_Vulkan, "Slow frame: {:.1f} ms; {}", ms(frame.time),
                         describe(frame));
            }
        }
        if (frame.time > worst.time) {
            worst = frame;
        }
    }
    last_tick = now;
    last_held = held_total;
    last_skipped = skipped_total;
    frame_tally = {};
    if (now - window >= std::chrono::seconds{10}) {
        LOG_INFO(Render_Vulkan,
                 "Worst frame in the last 10 s: {:.1f} ms ({} of {} frames over 33 ms); in it {}",
                 ms(worst.time), slow_frames, frames, describe(worst));
        worst = {};
        frames = 0;
        slow_frames = 0;
        window = now;
    }
}
} // Anonymous namespace

namespace {
/// EDEN_NX2_TRANSLATE_PROFILE=1 (zortos293/Lichii#896's profiler, off by default): every 10 s the
/// time each translation step took on all threads, the largest first.
void ReportTranslateProfile() {
    if (!Shader::TranslateProfile::Enabled()) {
        return;
    }
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - window < std::chrono::seconds{10}) {
        return;
    }
    window = now;
    namespace Profile = Shader::TranslateProfile;
    const Profile::Totals totals = Profile::Take();
    std::array<size_t, Profile::NumSteps> order{};
    std::iota(order.begin(), order.end(), size_t{0});
    std::ranges::sort(order, [&](size_t lhs, size_t rhs) {
        return totals.nanoseconds[lhs] > totals.nanoseconds[rhs];
    });
    std::string steps;
    for (const size_t step : order) {
        if (totals.counts[step] == 0) {
            continue;
        }
        steps += fmt::format("{}{} {:.1f} ms ({})", steps.empty() ? "" : ", ",
                             Profile::Name(static_cast<Profile::Step>(step)),
                             static_cast<double>(totals.nanoseconds[step]) / 1e6,
                             totals.counts[step]);
    }
    if (!steps.empty()) {
        LOG_INFO(Render_Vulkan, "Translation profile in the last 10 s (all threads): {}", steps);
    }
}
} // Anonymous namespace

void PipelineCache::TickFrame() {
    ++frame_count;
    ReportWorstFrame();
    ReportAsyncDiscard();
    ReportTranslateProfile();
    if (translate_queue) {
        TickTranslations();
    }
    // A retired translation's build may still be queued on the pipeline workers: destroying it
    // now would wait for that build, or run it here on the GPU thread.
    std::erase_if(retired_translations, [this](const auto& entry) {
        return scheduler.IsFree(entry.first) && entry.second->IsBuilt();
    });
    native_wait_budget_us =
        static_cast<s64>(Settings::values.native_pipeline_wait_ms.GetValue()) * 1000;
    while (!evicted_pipelines.empty() && scheduler.IsFree(evicted_pipelines.front().first)) {
        evicted_pipelines.pop_front();
    }
    while (!retired_compute_pipelines.empty() &&
           scheduler.IsFree(retired_compute_pipelines.front().first) &&
           retired_compute_pipelines.front().second->IsBuildTaskDone()) {
        retired_compute_pipelines.pop_front();
    }
    constexpr u64 EvictCheckFrames = 600;
    if (frame_count % EvictCheckFrames == 0) {
        EvictUnusedPipelines();
    }
}

void PipelineCache::EvictUnusedPipelines() {
    // Building a pipeline again on MoltenVK means a Metal compile mid-game, far dearer than the
    // rebuild cost below, so none are evicted there.
    if (device.IsMoltenVK()) {
        return;
    }
    // EDEN_NX2_PRELOAD_ALL_PIPELINES asked for every cached pipeline built: evicting the ones not
    // used yet threw the preload away after 9000 frames (Ocarina: 22,510 of 24,285, then rebuilt
    // one by one in new places).
    if (PreloadAllPipelines()) {
        return;
    }
    // Each built pipeline holds ~0.45 MB of driver memory and none was ever destroyed, so it grew
    // with every place a session went (title 1: 1,500 on its title screen alone). One unused for
    // this long is destroyed; rebuilding it later requires a full GPU-thread shader translation
    // (~8.5 ms each) plus a driver pipeline cache hit.
    constexpr u64 EvictAfterFrames = 9000;
    if (frame_count <= EvictAfterFrames || graphics_cache.size() <= KeptPipelines()) {
        return;
    }
    const u64 cutoff = frame_count - EvictAfterFrames;
    const u64 tick = scheduler.CurrentTick();
    // ~0.9 GB of driver memory at ~0.45 MB each.
    constexpr size_t MaxUnusedPrebuiltPipelines = 2000;
    size_t kept_prebuilt = 0;
    size_t evicted = 0;
    for (auto it = graphics_cache.begin(); it != graphics_cache.end();) {
        GraphicsPipeline* const pipeline = it->second.get();
        if (pipeline == nullptr || pipeline == current_pipeline || !pipeline->IsBuilt() ||
            pipeline->LastUse() >= cutoff) {
            ++it;
            continue;
        }
        // A pipeline built from the disk cache at boot that no draw has used yet stays, up to a
        // budget: evicting it throws the prebuild away, and the game translates it again on the
        // GPU thread when it reaches that content (OoT: 1174 of 1346 evicted on the title screen,
        // 684 rebuilt in 10 s on entering the field). Past the budget they go like the rest, so a
        // large cache can't hold their driver memory for the whole session.
        if (!pipeline->WasUsed() && kept_prebuilt < MaxUnusedPrebuiltPipelines) {
            ++kept_prebuilt;
            ++it;
            continue;
        }
        if (used_this_session.erase(pipeline) != 0) {
            used_and_evicted.insert(it->first.Hash());
        }
        evicted_pointer_slots.Remember(it->first.Hash(), pipeline->MappedPointers());
        evicted_pipelines.emplace_back(tick, std::move(it->second));
        it = graphics_cache.erase(it);
        ++evicted;
    }
    if (evicted == 0) {
        return;
    }
    for (auto& [key, pipeline] : graphics_cache) {
        if (pipeline) {
            pipeline->ClearTransitions();
        }
    }
    LOG_INFO(Render_Vulkan, "Evicted {} graphics pipelines unused for {} frames, {} left", evicted,
             EvictAfterFrames, graphics_cache.size());
}

namespace {
/// EDEN_NX2_ASYNC_DISCARD as given: -1 unset, 0, 1 or 2. Read on the first unbuilt pipeline,
/// after nx2_switches.txt was applied.
int AsyncDiscardSwitch() {
    static const int value = [] {
        const char* text = std::getenv("EDEN_NX2_ASYNC_DISCARD");
        const int parsed = text == nullptr  ? -1
                           : text[0] == '0' ? 0
                           : text[0] == '1' ? 1
                                            : 2;
        static constexpr std::array<const char*, 4> what{
            "unset: 2 while the native_async_draws setting is Wait (its default), else the setting",
            "0: native draws by the native_async_draws setting",
            "1: native draws skip unbuilt pipelines by the legacy heuristic",
            "2: native draws skip unbuilt pipelines only into render targets cleared every frame",
        };
        LOG_INFO(Render_Vulkan, "EDEN_NX2_ASYNC_DISCARD {}", what[parsed + 1]);
        return parsed;
    }();
    return value;
}
} // Anonymous namespace

int AsyncDiscardMode() {
    if (const int value = AsyncDiscardSwitch(); value >= 0) {
        return value;
    }
    // Fritchii v5's default (b2a243415), unless the setting was changed from Wait: with an empty
    // shader cache, skipping by the legacy heuristic dropped draws into render-once layers for
    // good (no "Press Start", empty chat bubbles, wrong lighting), while waiting for every
    // pipeline stalls each new one.
    return Settings::values.native_async_draws.GetValue() == Settings::NativeAsyncDraws::Wait
               ? 2
               : 0;
}

NativeDrawAction PipelineCache::PendingDrawAction() {
    // Native draws whose pipeline is still building are skipped instead of waited for when the
    // mode allows (the object is missing for a frame or more rather than the game holding while
    // the driver compiles). Full-screen passes (no depth, few vertices) still wait, so a lighting
    // or post pass is never dropped.
    //
    // The legacy heuristic ignores the target, so a draw into a layer the game renders once
    // (title screen text, a chat bubble, a probe) loses its only draw for good when its pipeline
    // is missing. EDEN_NX2_ASYNC_DISCARD=2 skips only into targets fully cleared in this frame
    // and in a recent earlier one; first-time and persistent targets wait. The frame is the
    // texture cache's render frame, counted at the guest's flips (ClassifyClearedTarget). While
    // the render-frame clock is off or not live it is the present tick, which can land between
    // two clears of one target in the same frame and make the target look recurring.
    if (discard_decision.serial == lookup_serial) {
        return discard_decision.action;
    }
    const int mode = AsyncDiscardMode();
    const bool native = maxwell3d->GetNativeMacroEngine().native_turing;
    // The environment variable wins over the setting; the setting is read per call.
    NativePendingMode native_mode = NativePendingMode::Wait;
    if (native && mode != 0) {
        native_mode = NativePendingMode::Skip;
    } else if (native) {
        switch (Settings::values.native_async_draws.GetValue()) {
        case Settings::NativeAsyncDraws::Bounded:
            native_mode = NativePendingMode::Bounded;
            break;
        case Settings::NativeAsyncDraws::Skip:
            native_mode = NativePendingMode::Skip;
            break;
        default:
            break;
        }
    }
    // Classified in every mode, for the counts (with 1, the persistent column is what that rule
    // drops for good); it runs per missing pipeline, not per draw.
    const VideoCommon::DrawTargets targets = texture_cache.ClassifyDrawTargets();
    const bool recurring =
        mode != 2 || !native || targets.kind == VideoCommon::DrawTargetClass::Recurring;
    const auto& draw_state = maxwell3d->draw_manager.draw_state;
    // Native draws are judged by the heuristic only when a mode other than Wait is chosen.
    const bool can_discard = CanDiscardPendingDraw(
        native, maxwell3d->regs.zeta_enable != 0, draw_state.draw_element_count,
        draw_state.draw_element_count, native && native_mode != NativePendingMode::Wait,
        recurring);
    const NativeDrawAction action =
        native ? DecideNativePending(native_mode, can_discard, native_wait_budget_us)
               : (can_discard ? NativeDrawAction::Skip : NativeDrawAction::Wait);
    discard_decision = DiscardDecision{
        .serial = lookup_serial, .action = action, .skip = false, .targets = targets};
    return action;
}

/// Returns `pipeline` when it is built or the current draw must wait for it, and nullptr when the
/// draw may be skipped while it builds (asynchronous shaders, PendingDrawAction). Each nullptr
/// returned for an unbuilt pipeline is counted for the log.
GraphicsPipeline* PipelineCache::BuiltPipeline(GraphicsPipeline* pipeline) noexcept {
    if (pipeline->IsBuilt()) {
        return pipeline;
    }
    if (!use_asynchronous_shaders) {
        return pipeline;
    }
    bool discard = false;
    switch (PendingDrawAction()) {
    case NativeDrawAction::Wait:
        break;
    case NativeDrawAction::Skip:
        discard = true;
        break;
    case NativeDrawAction::WaitTimed: {
        const auto begin = std::chrono::steady_clock::now();
        const bool built = pipeline->WaitBuilt(std::chrono::microseconds{native_wait_budget_us});
        const auto spent = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - begin)
                               .count();
        native_wait_budget_us -= spent;
        native_wait_used_us += spent;
        static auto window = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - window > std::chrono::seconds{10}) {
            LOG_INFO(Render_Vulkan,
                     "Bounded pipeline wait: {:.1f} ms spent in the last 10 s (budget {} ms "
                     "per frame)",
                     static_cast<double>(native_wait_used_us) / 1000.0,
                     Settings::values.native_pipeline_wait_ms.GetValue());
            native_wait_used_us = 0;
            window = std::chrono::steady_clock::now();
        }
        discard = !built;
        break;
    }
    }
    discard_decision.skip = discard;
    NoteDiscardDecision(discard, discard_decision.targets);
    if (discard) {
        NoteSkippedLookup();
    }
    return discard ? nullptr : pipeline;
}

bool PipelineCache::DrawMayBeSkipped() {
    // A bounded wait (native_async_draws = Bounded) may end in a skip too: the translation runs
    // off the GPU thread, and the budget is spent on the driver compile once it is adopted.
    return PendingDrawAction() != NativeDrawAction::Wait;
}

void PipelineCache::NoteTranslateSkip() {
    discard_decision.skip = true;
    NoteDiscardDecision(true, discard_decision.targets);
    NoteSkippedLookup();
    ++translate_stats.skipped;
}

void PipelineCache::NoteDiscardDecision(bool skip, const VideoCommon::DrawTargets& targets) {
    auto& stats = discard_stats;
    const bool repeat = stats.last_frame == frame_count && stats.last_skip == skip &&
                        stats.last_kind == targets.kind && stats.last_target == targets.gpu_addr &&
                        stats.last_key == graphics_key;
    if (repeat) {
        return;
    }
    stats.last_frame = frame_count;
    stats.last_skip = skip;
    stats.last_kind = targets.kind;
    stats.last_target = targets.gpu_addr;
    stats.last_key = graphics_key;
    const size_t index = static_cast<size_t>(targets.kind);
    ++(skip ? stats.skipped : stats.waited)[index];
    constexpr size_t MaxListedTargets = 32;
    if (targets.kind != VideoCommon::DrawTargetClass::Persistent || targets.gpu_addr == 0 ||
        listed_persistent_targets.size() >= MaxListedTargets ||
        !listed_persistent_targets.insert(targets.gpu_addr).second) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "Async discard: draw into persistent target {:#x} {}x{} {} {} for its pipeline "
             "{:016x} (EDEN_NX2_ASYNC_DISCARD mode {}; each target listed once, {} of {})",
             targets.gpu_addr, targets.width, targets.height, targets.format,
             skip ? "skipped" : "waits", graphics_key.Hash(), AsyncDiscardMode(),
             listed_persistent_targets.size(), MaxListedTargets);
}

void PipelineCache::ReportAsyncDiscard() {
    auto& stats = discard_stats;
    const auto now = std::chrono::steady_clock::now();
    if (stats.window == std::chrono::steady_clock::time_point{}) {
        stats.window = now;
        stats.held_at_window = GraphicsPipeline::HoldTimeTotal();
        return;
    }
    if (now - stats.window < std::chrono::seconds{10}) {
        return;
    }
    const auto held_total = GraphicsPipeline::HoldTimeTotal();
    const auto held = held_total - stats.held_at_window;
    const auto& skipped = stats.skipped;
    const auto& waited = stats.waited;
    if (skipped[0] + skipped[1] + skipped[2] + waited[0] + waited[1] + waited[2] != 0) {
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        LOG_INFO(Render_Vulkan,
                 "Async discard in the last 10 s (EDEN_NX2_ASYNC_DISCARD mode {}): lookups of "
                 "unbuilt pipelines skipped {} recurring, {} first-time, {} persistent; waited "
                 "{} recurring, {} first-time, {} persistent; {:.1f} ms of draws held for "
                 "unbuilt pipelines",
                 AsyncDiscardMode(), skipped[0], skipped[1], skipped[2], waited[0], waited[1],
                 waited[2], ms(held));
    }
    stats.skipped = {};
    stats.waited = {};
    stats.held_at_window = held_total;
    stats.window = now;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    ShaderPools& pools, const GraphicsPipelineCacheKey& key,
    std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
    bool build_in_parallel, std::shared_ptr<ShaderPools> emit_pools,
    ParallelTranslation* parallel, TranslationRecord* record) try {
    auto hash = key.Hash();
    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);
    // Declared before the programs, whose IR they hold
    std::vector<std::shared_ptr<ShaderPools>> stage_pools;
    SCOPE_EXIT {
        for (std::shared_ptr<ShaderPools>& stage : stage_pools) {
            ReturnPools(std::move(stage));
        }
    };
    std::vector<VideoCommon::StageTranslation> translations;
    std::array<VideoCommon::StageTranslation*, Maxwell::MaxShaderProgram> translated{};
    if (parallel) {
        translations.reserve(envs.size());
        size_t stage_env{};
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            Shader::Environment& env{*envs[stage_env++]};
            if (index == static_cast<size_t>(Maxwell::ShaderType::Geometry) &&
                !device.IsGeometryShaderSupported() && env.IsNativeProgram()) {
                continue; // The pipeline is skipped below before this stage is needed
            }
            translations.push_back({
                .env = &env,
                .pools = stage_pools.emplace_back(TakeSparePools()).get(),
                .cfg_offset = static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader)),
                .exits_to_dispatcher = index == 0,
            });
            translated[index] = &translations.back();
        }
        const auto begin = std::chrono::steady_clock::now();
        VideoCommon::TranslateStages(*shader_jobs, translations, host_info);
        parallel->wait += std::chrono::steady_clock::now() - begin;
        for (const VideoCommon::StageTranslation& translation : translations) {
            parallel->stages += translation.time;
        }
        parallel->count = translations.size();
    }
    size_t env_index{0};
    std::array<Shader::IR::Program, Maxwell::MaxShaderProgram> programs;
    const bool uses_vertex_a{key.unique_hashes[0] != 0};
    const bool uses_vertex_b{key.unique_hashes[1] != 0};

    // Layer passthrough generation for devices without VK_EXT_shader_viewport_index_layer
    Shader::IR::Program* layer_source_program{};
    bool mesh_unsupported{};

    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && is_emulated_stage) {
            auto topology = MaxwellToOutputTopology(key.state.topology);
            programs[index] = GenerateGeometryPassthrough(pools.inst, pools.block, host_info,
                                                          *layer_source_program, topology);
            continue;
        }
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;
        if (index == static_cast<size_t>(Maxwell::ShaderType::Geometry) &&
            !device.IsGeometryShaderSupported() && env.IsNativeProgram()) {
            // A native (Switch 2) geometry program on a device without geometry shaders: the
            // pipeline cannot run it, so its draws are skipped.
            static std::atomic<bool> logged{};
            if (!logged.exchange(true)) {
                LOG_WARNING(Render_Vulkan, "Skipping native geometry shader pipelines: the device has no geometry shaders");
            }
            return nullptr;
        }
        // Without VK_EXT_mesh_shader the driver cannot build a task/mesh pipeline; the draw is
        // skipped (see the pipeline check in the rasterizer). The stages are still translated
        // so the skipped pipeline's needs can be reported below.
        if ((env.ShaderStage() == Shader::Stage::Task ||
             env.ShaderStage() == Shader::Stage::Mesh) &&
            !device.IsExtMeshShaderSupported()) {
            static std::atomic<bool> logged{};
            if (!logged.exchange(true, std::memory_order_relaxed)) {
                LOG_WARNING(Render_Vulkan,
                            "{} has no VK_EXT_mesh_shader: pipelines with task/mesh shaders are "
                            "skipped, so the geometry they draw (such as foliage and props) will "
                            "be missing",
                            device.GetModelName());
            }
            mesh_unsupported = true;
        }

        const auto translate_begin = t_runtime_build ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
        const auto translate = [&] {
            if (translated[index]) {
                return translated[index]->Take();
            }
            const u32 cfg_offset{
                static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
            return TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        };
        if (!uses_vertex_a || index != 1) {
            // Normal path
            programs[index] = translate();
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{translate()};
            programs[index] = MergeDualVertexPrograms(program_va, program_vb, env, host_info);
            // Each half fit its texture arrays to the stage limits on its own; joined, they are
            // one stage.
            Shader::Optimization::LimitSampledTextureArrays(programs[index], host_info);
        }
        if (t_runtime_build) {
            t_build_split.translation += std::chrono::steady_clock::now() - translate_begin;
        }

        if (device.GetDriverID() != VK_DRIVER_ID_NVIDIA_PROPRIETARY && Shader::Optimization::HasBrokenPattern(programs[index])) {
            LOG_WARNING(Render_Vulkan, "0x{:016x}: Skipping shader with known issues", key.unique_hashes[index]);
            return nullptr;
        }

        if (Settings::values.dump_guest_shaders) {
            env.Dump(hash, key.unique_hashes[index]);
        }

        if (programs[index].info.requires_layer_emulation) {
            layer_source_program = &programs[index];
        }
    }
    // Mesh emulation without VK_EXT_mesh_shader (prototype): a mesh program, after an optional
    // task program, drawing triangles without per-primitive outputs, before a pixel program only.
    std::optional<MeshEmulationSetup> mesh_emulation;
    size_t mesh_index{};
    if (mesh_unsupported) {
        ReportUnsupportedMeshPipeline(key, programs);
        const bool has_task{key.unique_hashes[1] != 0 &&
                            programs[1].stage == Shader::Stage::Task};
        for (size_t index = 1; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0 && programs[index].stage == Shader::Stage::Mesh) {
                mesh_index = index;
                break;
            }
        }
        bool eligible{mesh_index != 0 && (mesh_index == 1 || has_task) &&
                      layer_source_program == nullptr};
        const Shader::IR::Program& mesh{programs[mesh_index]};
        eligible = eligible && mesh.mesh.topology == 4 && mesh.mesh.primitive_lines.empty() &&
                   !mesh.mesh.vertex_lines.empty() && mesh.mesh.max_vertices != 0 &&
                   mesh.mesh.max_primitives != 0;
        for (size_t index = 1; index < Maxwell::MaxShaderProgram && eligible; ++index) {
            const bool emulated{index == mesh_index || (has_task && index == 1)};
            eligible = emulated || key.unique_hashes[index] == 0 ||
                       programs[index].stage == Shader::Stage::Fragment;
        }
        if (!eligible) {
            return nullptr;
        }
        // The output buffer's binding follows every program's (unified bindings, one per
        // descriptor element, as the SPIR-V backend numbers them). The task and mesh programs
        // share the compute stage, so their descriptors and the emulation's own buffers count
        // against one set of per-stage limits.
        u32 output_binding{};
        u32 compute_storage_buffers{has_task ? 4U : 1U};
        u32 compute_sampled_images{};
        u32 compute_resources{compute_storage_buffers};
        for (size_t index = 1; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            const Shader::Info& info{programs[index].info};
            const bool emulated{index == mesh_index || (has_task && index == 1)};
            const auto add = [&](const auto& descriptors, u32* category) {
                const u32 count{Shader::NumDescriptors(descriptors)};
                output_binding += count;
                if (emulated) {
                    compute_resources += count;
                    if (category) {
                        *category += count;
                    }
                }
            };
            add(info.constant_buffer_descriptors, nullptr);
            add(info.storage_buffers_descriptors, &compute_storage_buffers);
            add(info.texture_buffer_descriptors, &compute_sampled_images);
            add(info.image_buffer_descriptors, nullptr);
            add(info.texture_descriptors, &compute_sampled_images);
            add(info.image_descriptors, nullptr);
        }
        if (compute_storage_buffers > device.GetMaxPerStageDescriptorStorageBuffers() ||
            compute_sampled_images > device.GetMaxPerStageDescriptorSampledImages() ||
            compute_resources > device.GetMaxPerStageResources()) {
            LOG_WARNING(Render_Vulkan,
                        "Mesh emulation skipped: {} storage buffers, {} sampled images and {} "
                        "resources exceed the compute stage limits",
                        compute_storage_buffers, compute_sampled_images, compute_resources);
            return nullptr;
        }
        mesh_emulation.emplace();
        mesh_emulation->output_binding = output_binding;
        if (has_task) {
            // After the output: the task payload, offset and argument buffers.
            mesh_emulation->has_task = true;
            mesh_emulation->mesh_stage = static_cast<u32>(mesh_index - 1);
            mesh_emulation->payload_stride = (std::max)(programs[1].mesh.payload_words, 2U);
            mesh_emulation->payload_binding = output_binding + 1;
            mesh_emulation->offsets_binding = output_binding + 2;
            mesh_emulation->args_binding = output_binding + 3;
            mesh_emulation->distribution_spirv =
                Shader::Backend::SPIRV::EmitMeshEmulationDistribution(profile);
        }
        mesh_emulation->block_words = Shader::Backend::SPIRV::MeshEmulationBlockWords(mesh.mesh);
        mesh_emulation->max_primitives = mesh.mesh.max_primitives;
        mesh_emulation->max_vertices = mesh.mesh.max_vertices;
        mesh_emulation->vertex_lines = static_cast<u32>(mesh.mesh.vertex_lines.size());
        for (u32 line = 0; line < mesh.mesh.vertex_lines.size(); ++line) {
            const u32 attribute{mesh.mesh.vertex_lines[line]};
            const u32 position_x{static_cast<u32>(Shader::IR::Attribute::PositionX)};
            if (attribute >= position_x && attribute < position_x + 4) {
                mesh_emulation->position_lines[attribute - position_x] = line;
            }
        }
        static std::atomic<u32> emulated{};
        if (const u32 count{++emulated}; count <= 64) {
            // The input region addresses the mesh program reads (0 is its workgroup index).
            std::string reads;
            for (const Shader::IR::Block* block : mesh.blocks) {
                for (const Shader::IR::Inst& inst : block->Instructions()) {
                    if (inst.GetOpcode() != Shader::IR::Opcode::IsbeRead) {
                        continue;
                    }
                    const Shader::IR::Value address{inst.Arg(0)};
                    reads += address.IsImmediate() ? fmt::format(" {:#x}", address.U32())
                                                   : std::string{" dynamic"};
                }
            }
            LOG_INFO(Render_Vulkan,
                     "Mesh emulation #{}: pipeline {:016x}, {} vertices x {} lines, {} triangles "
                     "per workgroup, output binding {}, task {} (payload {} words), input "
                     "reads:{}",
                     count, hash, mesh.mesh.max_vertices, mesh.mesh.vertex_lines.size(),
                     mesh.mesh.max_primitives, output_binding, has_task,
                     mesh_emulation->payload_stride, reads.empty() ? " none" : reads);
        }
    }
    // A tessellation pipeline the driver would refuse is skipped like a missing mesh pipeline:
    // the draw is dropped (see the pipeline check in the rasterizer) instead of failing the build.
    {
        const Shader::IR::Program* control{};
        const Shader::IR::Program* evaluation{};
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] == 0) {
                continue;
            }
            if (programs[index].stage == Shader::Stage::TessellationControl) {
                control = &programs[index];
            } else if (programs[index].stage == Shader::Stage::TessellationEval) {
                evaluation = &programs[index];
            }
        }
        if (control != nullptr || evaluation != nullptr) {
            const u32 max_patch_size = device.GetMaxTessellationPatchSize();
            const char* reason = nullptr;
            if (!device.IsTessellationSupported()) {
                reason = "the device has no tessellation shaders";
            } else if (control == nullptr || evaluation == nullptr) {
                reason = "it has only one of the control and evaluation stages";
            } else if (control->invocations == 0 || control->invocations > max_patch_size) {
                reason = "the control shader's output vertex count is out of range";
            }
            if (reason != nullptr) {
                LOG_WARNING(Render_Vulkan,
                            "Skipping tessellation pipeline: {} (tcs={} tes={}, tcs output "
                            "vertices {}, patch control points {}, device max patch size {})",
                            reason, control != nullptr, evaluation != nullptr,
                            control != nullptr ? control->invocations : 0,
                            key.state.patch_control_points_minus_one.Value() + 1, max_patch_size);
                return nullptr;
            }
        }
    }
    {
        // The descriptor set limits count the textures of every stage the pipeline emits.
        std::array<Shader::IR::Program*, Maxwell::MaxShaderProgram> stages{};
        size_t num_stages{};
        for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0;
             index < Maxwell::MaxShaderProgram; ++index) {
            const bool is_emulated_stage = layer_source_program != nullptr &&
                                           index == static_cast<u32>(Maxwell::ShaderType::Geometry);
            if (key.unique_hashes[index] != 0 || is_emulated_stage) {
                stages[num_stages++] = &programs[index];
            }
        }
        Shader::Optimization::LimitPipelineSampledTextureArrays(
            std::span{stages.data(), num_stages}, host_info);
    }
    // Each stage stays within the limit on its own; together they share one descriptor set.
    if (const u32 set_limit = host_info.max_descriptor_set_storage_buffers; set_limit != 0) {
        size_t storage_buffers{};
        for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0;
             index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                storage_buffers +=
                    Shader::NumDescriptors(programs[index].info.storage_buffers_descriptors);
            }
        }
        // The fallback binds its output buffer (and, for task draws, payload,
        // offsets and indirect arguments) into the same descriptor set.
        const size_t auxiliary_buffers{
            mesh_emulation ? (mesh_emulation->has_task ? 4U : 1U) : 0U};
        if (!FitsStorageBufferSetLimit(storage_buffers, auxiliary_buffers, set_limit)) {
            throw Shader::NotImplementedException(
                "Binding {} storage buffers ({} for mesh fallback) to a descriptor set limited to {}",
                storage_buffers + auxiliary_buffers, auxiliary_buffers, set_limit);
        }
    }
    u64 deferred_ir_bytes = 0;
    std::unique_ptr<QueuedIrBudget::Reservation> ir_reservation;
    if (emit_pools) {
        deferred_ir_bytes = IrBytes(programs);
        ir_reservation = queued_ir.TryReserve(deferred_ir_bytes);
    }
    const bool defer_emission = ir_reservation != nullptr;
    std::array<const Shader::Info*, Maxwell::MaxShaderStage> infos{};
    std::array<vk::ShaderModule, Maxwell::MaxShaderStage> modules;
    std::array<std::optional<Shader::RuntimeInfo>, Maxwell::MaxShaderStage> deferred_runtime_infos;
    std::array<std::vector<u32>, Maxwell::MaxShaderStage> reference_spirv;
    std::array<std::vector<u32>, Maxwell::MaxShaderStage> deferred_spirv;

    const Shader::IR::Program* previous_stage{};
    Shader::Backend::Bindings binding;
    for (size_t index = uses_vertex_a && uses_vertex_b ? 1 : 0; index < Maxwell::MaxShaderProgram;
         ++index) {
        const bool is_emulated_stage = layer_source_program != nullptr &&
                                       index == static_cast<u32>(Maxwell::ShaderType::Geometry);
        if (key.unique_hashes[index] == 0 && !is_emulated_stage) {
            continue;
        }
        UNIMPLEMENTED_IF(index == 0);

        Shader::IR::Program& program{programs[index]};
        const size_t stage_index{index - 1};
        infos[stage_index] = &program.info;

        auto runtime_info{MakeRuntimeInfo(programs, key, program, previous_stage, device)};
        if (mesh_emulation && (program.stage == Shader::Stage::Mesh ||
                               program.stage == Shader::Stage::Task)) {
            runtime_info.mesh_as_compute = true;
            runtime_info.mesh_output_binding = mesh_emulation->output_binding;
            if (mesh_emulation->has_task) {
                runtime_info.mesh_task_payload_binding = mesh_emulation->payload_binding;
                runtime_info.mesh_task_offsets_binding = mesh_emulation->offsets_binding;
                runtime_info.mesh_task_args_binding = mesh_emulation->args_binding;
                runtime_info.mesh_task_payload_stride = mesh_emulation->payload_stride;
            }
            if (program.stage == Shader::Stage::Mesh) {
                mesh_emulation->vertex_spirv = Shader::Backend::SPIRV::EmitMeshEmulationVertex(
                    profile, program.mesh, mesh_emulation->output_binding,
                    mesh_emulation->has_task ? mesh_emulation->args_binding : ~0U,
                    runtime_info.convert_depth_mode && !profile.support_native_ndc);
            }
        }
        ConvertLegacyToGeneric(program, runtime_info);
        // EDEN_NX2_MESH_EMULATION_DUMP=<dir>: write the emulation's vertex shader for spirv-val.
        // (Emitting the mesh program a second time here to dump its compute shader crashed the
        // build that followed, so only the vertex shader is written.)
        if (mesh_emulation && program.stage == Shader::Stage::Mesh) {
            if (const char* const dump{std::getenv("EDEN_NX2_MESH_EMULATION_DUMP")}) {
                const auto& code{mesh_emulation->vertex_spirv};
                std::ofstream file{fmt::format("{}/{:016x}.vert.spv", dump, hash),
                                   std::ios::binary};
                file.write(reinterpret_cast<const char*>(code.data()),
                           static_cast<std::streamsize>(code.size() * sizeof(u32)));
            }
        }
        if (record) {
            // EDEN_NX2_ASYNC_TRANSLATE=2: this stage's SPIR-V, emitted once and used for the
            // record and for the pipeline alike. EmitSPIRV changes the program it emits: the flag
            // of an operation (GetZeroFromOp, GetSignFromOp, GetCarryFromOp, GetOverflowFromOp,
            // GetSparseFromOp, GetInBoundsFromOp) gets its id from the operation's emitter and is
            // turned into a Void, cut from the operation. A second emission of the same program
            // never defines those flags again, and their users read the ids of the first one:
            // ids the module never defines, and a driver crash.
            const auto begin = std::chrono::steady_clock::now();
            record->spirv[stage_index] = EmitSPIRV(profile, runtime_info, program, binding);
            if (record->translate_only) {
                previous_stage = &program;
                continue;
            }
            if (defer_emission) {
                // The job builds the module from these words (DeferredStages).
                deferred_runtime_infos[stage_index] = runtime_info;
                deferred_spirv[stage_index] = record->spirv[stage_index];
            } else {
                modules[stage_index] = BuildStageModule(device, record->spirv[stage_index],
                                                        key.unique_hashes[index], stage_index);
            }
            if (build_in_parallel) {
                AddRuntimeSpirvTime(0, begin);
            }
            previous_stage = &program;
            continue;
        }
        if (defer_emission) {
            deferred_runtime_infos[stage_index] = runtime_info;
            if (VideoCommon::Nx2Perf::Verify()) {
                reference_spirv[stage_index] = EmitSPIRV(profile, runtime_info, program, binding);
            }
        } else {
            const auto begin = std::chrono::steady_clock::now();
            modules[stage_index] = EmitShaderModule(device, profile, runtime_info, program, binding,
                                                   key.unique_hashes[index], stage_index);
            if (build_in_parallel) {
                AddRuntimeSpirvTime(0, begin);
            }
        }
        previous_stage = &program;
    }
    if (record) {
        record->complete = true;
        if (record->translate_only) {
            return nullptr;
        }
    }
    std::function<std::array<vk::ShaderModule, Maxwell::MaxShaderStage>()> emit_stages;
    if (defer_emission) {
        auto job = std::make_shared<DeferredStages>();
        job->reservation = std::move(ir_reservation);
        job->pools = emit_pools;
        job->stage_pools = std::move(stage_pools);
        stage_pools.clear();
        job->programs = std::move(programs);
        job->runtime_infos = std::move(deferred_runtime_infos);
        job->reference_spirv = std::move(reference_spirv);
        job->emitted_spirv = std::move(deferred_spirv);
        for (size_t stage = 0; stage < infos.size(); ++stage) {
            if (infos[stage]) {
                infos[stage] = &job->programs[stage + 1].info;
            }
        }
        emit_stages = [this, job = std::move(job), hashes = key.unique_hashes]() mutable {
            const auto begin = std::chrono::steady_clock::now();
            std::array<vk::ShaderModule, Maxwell::MaxShaderStage> stage_modules;
            Shader::Backend::Bindings stage_binding;
            for (size_t stage = 0; stage < stage_modules.size(); ++stage) {
                if (!job->emitted_spirv[stage].empty()) {
                    // Emitted already: a program is emitted once (see the record in the loop).
                    stage_modules[stage] = BuildStageModule(device, job->emitted_spirv[stage],
                                                            hashes[stage + 1], stage);
                } else if (job->runtime_infos[stage]) {
                    stage_modules[stage] =
                        EmitShaderModule(device, profile, *job->runtime_infos[stage],
                                         job->programs[stage + 1], stage_binding,
                                         hashes[stage + 1], stage, job->reference_spirv[stage]);
                }
            }
            auto completed_pools = std::move(job->pools);
            auto completed_stages = std::move(job->stage_pools);
            job.reset();
            ReturnPools(std::move(completed_pools));
            for (std::shared_ptr<ShaderPools>& stage : completed_stages) {
                ReturnPools(std::move(stage));
            }
            AddRuntimeSpirvTime(1, begin);
            return stage_modules;
        };
    }
    static_assert(VideoCommon::EvictedPointerSlots::NumPrograms == Maxwell::MaxShaderProgram);
    VideoCommon::EvictedPointerSlots::Slots mapped_pointers;
    for (size_t index = 0, mapped_env = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] != 0) {
            mapped_pointers[index] = envs[mapped_env++]->MappedStoragePointerKeys();
        }
    }
    auto pipeline = std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, pipeline_binaries.get(),
        &shader_notify, device, descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring,
        BuildQueue(build_in_parallel, defer_emission ? deferred_ir_bytes : 0), statistics,
        render_pass_cache, key, std::move(modules), infos, std::move(emit_stages),
        std::move(mesh_emulation));
    pipeline->SetMappedPointers(std::move(mapped_pointers));
    return pipeline;

} catch (const Shader::Exception& exception) {
    auto hash = key.Hash();
    if (record != nullptr && record->translate_only) {
        // The verify mode's GPU-thread translation: the worker logs and dumps this failure.
        LOG_DEBUG(Render_Vulkan, "Verify translation of {}: {}", PipelineShaders(key),
                  exception.what());
        return nullptr;
    }
    if (!build_in_parallel) {
        // A disk cache record: one another (or an older) build stored can lack what this
        // translator asks for (merged packs: "Uncached storage pointer classification", 144
        // errors and dumps for 108 pipelines). Not an error: the GPU thread builds the pipeline
        // from the game when a draw needs it, and stores that record.
        NoteDiskRecordFailure("Graphics", hash, exception.what());
        return nullptr;
    }
    size_t env_index{0};
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (key.unique_hashes[index] == 0) {
            continue;
        }
        Shader::Environment& env{*envs[env_index]};
        ++env_index;

        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        try {
            // Walks the program for the dump; the one that failed may fail here again, and an
            // exception leaving this handler would end the process.
            Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        } catch (const Shader::Exception&) {
        }
        env.Dump(hash, key.unique_hashes[index]);
    }
    LOG_ERROR(Render_Vulkan, "{}", exception.what());
    return nullptr;
}

namespace {
// Pipelines built on the GPU thread at run time (shader translation, and the SPIR-V unless a worker
// emits it; the driver compile is on workers): the first 400 over 2 ms and every one over 100 ms
// are logged, and the totals every 10 s, to tell how much of the stutter on new content is
// translation.
void ReportRuntimePipeline(const char* kind, std::chrono::steady_clock::time_point begin,
                           const std::string& shaders, const RuntimeBuildSplit& split = {},
                           std::chrono::nanoseconds translation_wait = {},
                           std::chrono::nanoseconds stage_translation = {}) {
    static std::mutex mutex;
    std::scoped_lock lock{mutex};
    static std::chrono::steady_clock::duration total{};
    static RuntimeBuildSplit split_total{};
    static std::chrono::nanoseconds total_translation_wait{}, total_stage_translation{};
    static u32 count{}, logged{};
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - begin;
    total += elapsed;
    split_total.environments += split.environments;
    split_total.translation += split.translation;
    total_translation_wait += translation_wait;
    total_stage_translation += stage_translation;
    ++count;
    frame_tally.builds += elapsed;
    frame_tally.split.environments += split.environments;
    frame_tally.split.translation += split.translation;
    ++frame_tally.count;
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    // Slow builds are always logged with their shaders, to find the programs that are slow to
    // translate.
    if (elapsed > std::chrono::milliseconds{100} ||
        (elapsed > std::chrono::milliseconds{2} && logged < 400)) {
        ++logged;
        // Graphics builds name their environments and translation time too, after the shaders
        // so readers of the older line keep their fields.
        const std::string parts =
            split.environments.count() != 0 || split.translation.count() != 0
                ? fmt::format(" ({:.1f} ms environments, {:.1f} ms translation)",
                              ms(split.environments), ms(split.translation))
                : std::string{};
        LOG_INFO(Render_Vulkan, "{} pipeline built on the GPU thread in {:.1f} ms: {}{}", kind,
                 ms(elapsed), shaders, parts);
    }
    if (now - window > std::chrono::seconds{10}) {
        const auto take = [](auto& counter) {
            return std::chrono::nanoseconds{counter.exchange(0, std::memory_order::relaxed)};
        };
        // Shader cache v2: the GPU thread's wait for graphics stages translated at once on
        // workers, the translation time they took together, and compute SPIR-V on workers
        const std::string v2 = fmt::format(
            ", {:.1f} ms on the GPU thread waiting for {:.1f} ms of graphics "
            "stage translation on workers, {:.1f} ms compute SPIR-V on workers",
            ms(total_translation_wait), ms(total_stage_translation), ms(take(runtime_spirv_ns[2])));
        // The split goes at the end, so readers of the older line keep their fields.
        LOG_INFO(Render_Vulkan,
                 "Runtime pipelines in the last 10 s: {} built, {:.1f} ms on the GPU thread "
                 "({:.1f} ms of it graphics SPIR-V), {:.1f} ms graphics SPIR-V on workers, "
                 "{:.1f} ms of draws held for unbuilt pipelines{}; graphics on the GPU thread: "
                 "{:.1f} ms environments, {:.1f} ms translation",
                 count, ms(total), ms(take(runtime_spirv_ns[0])), ms(take(runtime_spirv_ns[1])),
                 ms(GraphicsPipeline::TakeHoldTime()), v2, ms(split_total.environments),
                 ms(split_total.translation));
        total = {};
        split_total = {};
        total_translation_wait = {};
        total_stage_translation = {};
        count = 0;
        window = now;
    }
}


// EDEN_NX2_ASYNC_TRANSLATE=3: GPU-thread builds translate from environment snapshots, and every
// read they recorded is answered again from the live state right after. Any difference is a
// snapshot gap: the first 16 are logged with their pipeline, and the counts every 10 s. GPU
// thread only.
void NoteSnapshotCheck(const GraphicsPipelineCacheKey& key, size_t bytes,
                       std::span<Shader::Environment* const> envs) {
    static u32 pipelines{}, logged{};
    static size_t copied{}, reads{}, differing{}, gave_up{};
    static auto window = std::chrono::steady_clock::now();
    ++pipelines;
    copied += bytes;
    for (Shader::Environment* const env : envs) {
        auto& graphics_env = static_cast<GraphicsEnvironment&>(*env);
        const auto check = graphics_env.CheckAgainstLive();
        reads += check.reads;
        differing += check.differing;
        if (graphics_env.GuardedReadFailed()) {
            ++gave_up;
        }
        if (check.differing != 0 && logged < 16) {
            ++logged;
            LOG_WARNING(Render_Vulkan,
                        "Environment snapshot differs from the live state: {} reads of {} in {} "
                        "stage {}, first: {}",
                        check.differing, check.reads, PipelineShaders(key),
                        static_cast<u32>(env->ShaderStage()), check.first);
        }
        graphics_env.ReleaseSnapshotBytes();
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - window > std::chrono::seconds{10}) {
        LOG_INFO(Render_Vulkan,
                 "Environment snapshots in the last 10 s: {} pipelines, {} KB copied, {} reads "
                 "checked, {} differ, {} guarded reads gave up",
                 pipelines, copied / 1024, reads, differing, gave_up);
        pipelines = 0;
        copied = reads = differing = gave_up = 0;
        window = now;
    }
}
} // Anonymous namespace

void PipelineCache::WarnKeptPointer(size_t program, u64 slot) const {
    if ((slot >> 63) != 0) {
        return;
    }
    const u32 index = static_cast<u32>(slot >> 32);
    const u32 offset = static_cast<u32>(slot);
    const size_t stage = program == 0 ? 0 : program - 1;
    const auto& cbufs = maxwell3d->state.shader_stages[stage].const_buffers;
    GPUVAddr value = 0;
    const char* reason = nullptr;
    if (index >= cbufs.size() || !cbufs[index].enabled ||
        offset + sizeof(u64) > cbufs[index].size ||
        !gpu_memory->IsFullyMappedRange(cbufs[index].address + offset, sizeof(u64))) {
        reason = "constant buffer not bound";
    } else {
        value = gpu_memory->Read<u64>(cbufs[index].address + offset);
        if (value == 0) {
            reason = "null";
        } else if (!gpu_memory->IsWithinGPUAddressRange(value)) {
            reason = "outside the GPU address range";
        } else if (!gpu_memory->IsFullyMappedRange(value, 1)) {
            reason = "unmapped";
        }
    }
    if (!reason) {
        return;
    }
    static std::atomic<u64> kept{};
    const u64 count = kept.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 32 || count % 1000 == 0) {
        LOG_WARNING(Render_Vulkan,
                    "Graphics pipeline {:016x} translated again after eviction: s{}:c{}[{:#x}] "
                    "holds {:#x} ({}), kept as the storage buffer pointer it was ({} so far)",
                    graphics_key.Hash(), program, index, offset, value, reason, count);
    }
}

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline(
    const VideoCommon::EvictedPointerSlots::Slots* keep_pointers) {
    const VideoCommon::Nx2Profile::Scope profile_scope{
        VideoCommon::Nx2Profile::Zone::GraphicsPipelineMiss};
    const auto begin = std::chrono::steady_clock::now();
    t_runtime_build = true;
    t_build_split = {};
    std::optional<ParallelTranslation> parallel;
    SCOPE_EXIT {
        t_runtime_build = false;
        std::string shaders = PipelineShaders(graphics_key);
        if (!parallel) {
            ReportRuntimePipeline("Graphics", begin, shaders, t_build_split);
            return;
        }
        const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
        shaders += fmt::format(" translate_wait_ms={:.1f} stage_translate_ms={:.1f} stages={}",
                               ms(parallel->wait), ms(parallel->stages), parallel->count);
        ReportRuntimePipeline("Graphics", begin, shaders, t_build_split, parallel->wait,
                              parallel->stages);
    };
    GraphicsEnvironments environments;
    const bool snapshot_check = VideoCommon::AsyncTranslateMode() == 3;
    const size_t snapshot_bytes =
        GetGraphicsEnvironments(environments, graphics_key.unique_hashes, snapshot_check);
    t_build_split.environments = std::chrono::steady_clock::now() - begin;
    SCOPE_EXIT {
        // Right after the translation (the SPIR-V and the driver compile may still be queued),
        // before the GPU thread changes any state.
        if (snapshot_check) {
            NoteSnapshotCheck(graphics_key, snapshot_bytes, environments.Span());
        }
    };
    if (keep_pointers) {
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (graphics_key.unique_hashes[index] == 0) {
                continue;
            }
            environments.envs[index].KeepStoragePointers((*keep_pointers)[index]);
            for (const u64 slot : (*keep_pointers)[index]) {
                WarnKeptPointer(index, slot);
            }
        }
    }

    // Shader cache v2: two or more stages translate at once on the shader jobs' CPU workers while
    // this thread waits. The texture descriptor pool isn't flushed first (upstream's #1036 does):
    // no translation flushes it here (GenericEnvironment::ReadTextureInfo, Fritchii 591ceee8b),
    // and with this thread waiting nothing else touches guest memory.
    if (std::ranges::count_if(graphics_key.unique_hashes, [](u64 hash) { return hash != 0; }) >=
        2) {
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (graphics_key.unique_hashes[index] != 0) {
                environments.envs[index].SetTexturePoolFlushed();
            }
        }
        parallel.emplace();
    }

    // Native programs emit their SPIR-V in the pipeline's job instead of here
    std::shared_ptr<ShaderPools> emit_pools;
    if (maxwell3d->GetNativeMacroEngine().native_turing) {
        emit_pools = TakeSparePools();
    } else {
        main_pools.ReleaseContents();
    }
    ShaderPools& pools = emit_pools ? *emit_pools : main_pools;
    auto pipeline{CreateGraphicsPipeline(pools, graphics_key, environments.Span(), nullptr, true,
                                         std::move(emit_pools), parallel ? &*parallel : nullptr)};
    if (!pipeline || pipeline_cache_filename.empty()) {
        return pipeline;
    }
    StoreRuntimeGraphicsPipeline(graphics_key, environments.envs);
    return pipeline;
}

void PipelineCache::StoreRuntimeGraphicsPipeline(
    const GraphicsPipelineCacheKey& key,
    const std::array<GraphicsEnvironment, Maxwell::MaxShaderProgram>& environments) {
    if (disk_pipelines.contains(key.Hash())) {
        // In the disk cache already, only not built at boot.
        QueueVulkanPipelineCacheFlush();
        return;
    }
    disk_pipelines.insert(key.Hash());
    serialization_thread.QueueWork([this, key, envs = environments] {
        boost::container::static_vector<const GenericEnvironment*, Maxwell::MaxShaderProgram>
            env_ptrs;
        for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
            if (key.unique_hashes[index] != 0) {
                env_ptrs.push_back(&envs[index]);
            }
        }
        const std::vector<u8> payload{VideoCommon::SerializePipelinePayload(
            std::span(reinterpret_cast<const char*>(&key), sizeof(key)), env_ptrs,
            host_info.EnvironmentReadProperties())};
        if (!payload.empty()) {
            cache_v2->Writer().Append(VideoCommon::ShaderCacheV2::RecordKind::Graphics,
                                      GraphicsRecordKey(key), payload);
        }
    });
    QueueVulkanPipelineCacheFlush();
}

namespace {
// EDEN_NX2_ASYNC_TRANSLATE=2: translations compared and how many differed, logged every 10 s by
// TickTranslations; the first 8 differences are logged by the translation thread that found them.
std::atomic<u64> verify_compared{};
std::atomic<u64> verify_differing{};
std::atomic<u32> verify_logged{};

/// The worker's translation against the GPU thread's: the SPIR-V of each stage word for word,
/// then the reads each environment recorded. Empty when they match.
std::string CompareTranslations(const TranslationJob& job) {
    const TranslationRecord& reference = *job.reference;
    const TranslationRecord& record = *job.record;
    if (reference.complete != record.complete) {
        return fmt::format("the translation {} on the GPU thread and {} on its thread",
                           reference.complete ? "completed" : "failed",
                           record.complete ? "completed" : "failed");
    }
    for (size_t stage = 0; stage < record.spirv.size(); ++stage) {
        const auto& expected = reference.spirv[stage];
        const auto& got = record.spirv[stage];
        if (expected == got) {
            continue;
        }
        const auto [mismatch, unused] = std::ranges::mismatch(expected, got);
        return fmt::format("stage {} SPIR-V: {} words on the GPU thread, {} on its thread, first "
                           "difference at word {}",
                           stage, expected.size(), got.size(),
                           std::distance(expected.begin(), mismatch));
    }
    for (size_t index = 0; index < job.envs.size(); ++index) {
        if (job.key.unique_hashes[index] == 0) {
            continue;
        }
        if (std::string difference = job.envs[index].FirstReadDifference(job.reference_envs[index]);
            !difference.empty()) {
            return fmt::format("program {} reads: {}", index, difference);
        }
    }
    return {};
}
} // Anonymous namespace

bool PipelineCache::EnqueueTranslation() {
    if (!DrawMayBeSkipped() || translate_sync_only.contains(graphics_key)) {
        return false;
    }
    if (translate_queue->InFlight() >= MaxTranslationsInFlight) {
        ++translate_stats.full;
        return false;
    }
    const auto begin = std::chrono::steady_clock::now();
    auto job = std::make_unique<TranslationJob>();
    job->key = graphics_key;
    job->native = maxwell3d->GetNativeMacroEngine().native_turing;
    job->enqueued = begin;
    GraphicsEnvironments environments;
    translate_stats.snapshot_bytes +=
        GetGraphicsEnvironments(environments, graphics_key.unique_hashes, true);
    // A copy (environments declare a destructor, so they have no moves); the snapshot is shared.
    job->envs = environments.envs;
    if (VideoCommon::AsyncTranslateMode() == 2) {
        // Verify mode: the same draw translated here from the live state too, as the GPU thread
        // would have, for the translation thread to compare with its own (SPIR-V and reads).
        // Pools of its own: nothing else on the GPU thread holds IR in them.
        GraphicsEnvironments live;
        GetGraphicsEnvironments(live, graphics_key.unique_hashes);
        job->reference = std::make_unique<TranslationRecord>();
        job->reference->translate_only = true;
        if (!verify_pools) {
            verify_pools = std::make_unique<ShaderPools>();
        }
        verify_pools->ReleaseContents();
        [[maybe_unused]] const auto none =
            CreateGraphicsPipeline(*verify_pools, graphics_key, live.Span(), nullptr, true,
                                   nullptr, nullptr, job->reference.get());
        job->reference_envs = live.envs;
        job->record = std::make_unique<TranslationRecord>();
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    translate_stats.enqueue_time += elapsed;
    frame_tally.queued_time += elapsed;
    ++frame_tally.queued;
    if (!translate_queue->Enqueue(std::move(job))) {
        ++translate_stats.full;
        return false;
    }
    ++translate_stats.queued;
    return true;
}

std::unique_ptr<GraphicsPipeline> PipelineCache::AdoptTranslation(bool& pending) {
    pending = false;
    if (!translate_queue->Contains(graphics_key)) {
        // Not translating: a key that made no pipeline, which stays empty as before.
        return nullptr;
    }
    auto job = translate_queue->Take(graphics_key);
    if (!job) {
        if (DrawMayBeSkipped()) {
            NoteTranslateSkip();
            pending = true;
            return nullptr;
        }
        // A draw that must not be skipped (a full-screen pass) needs a pipeline a skippable draw
        // queued: wait a little for it, then build it here. Never a near match.
        NoteDiscardDecision(false, discard_decision.targets);
        const auto begin = std::chrono::steady_clock::now();
        job = translate_queue->Wait(graphics_key, TranslateWaitCap);
        ++translate_stats.waits;
        translate_stats.wait_time += std::chrono::steady_clock::now() - begin;
        if (!job) {
            translate_queue->Abandon(graphics_key);
            ++translate_stats.fallbacks;
            return CreateGraphicsPipeline();
        }
    }
    bool retry{};
    auto pipeline = AdoptJob(std::move(job), retry);
    if (!retry) {
        return pipeline;
    }
    // Not usable: translate again from a fresh snapshot of this draw, or build it here.
    if (!translate_sync_only.contains(graphics_key) && DrawMayBeSkipped() &&
        EnqueueTranslation()) {
        ++translate_stats.requeued;
        NoteTranslateSkip();
        pending = true;
        return nullptr;
    }
    ++translate_stats.fallbacks;
    return CreateGraphicsPipeline();
}

std::unique_ptr<GraphicsPipeline> PipelineCache::AdoptJob(std::unique_ptr<TranslationJob> job,
                                                          bool& retry) {
    retry = false;
    switch (job->outcome) {
    case TranslationJob::Outcome::Built:
        if (std::ranges::all_of(job->envs, [](GraphicsEnvironment& env) {
                return env.TextureReadsStillMatch();
            })) {
            const auto ready = std::chrono::steady_clock::now() - job->enqueued;
            ++translate_stats.adopted;
            translate_stats.translation_time += job->translation;
            translate_stats.ready_total += ready;
            translate_stats.ready_max = (std::max)(translate_stats.ready_max, ready);
            translate_failures.erase(job->key);
            auto pipeline = std::move(job->pipeline);
            if (!pipeline_cache_filename.empty()) {
                StoreRuntimeGraphicsPipeline(job->key, job->envs);
            }
            return pipeline;
        }
        ++translate_stats.stale;
        break;
    case TranslationJob::Outcome::Retry:
        ++translate_stats.gave_up;
        break;
    case TranslationJob::Outcome::Differs:
        // Verify mode: a snapshot gap (logged by the translation thread). Built on the GPU
        // thread from the live state, now and from then on.
        ++translate_stats.differs;
        translate_sync_only.insert(job->key);
        break;
    case TranslationJob::Outcome::NoPipeline:
    case TranslationJob::Outcome::Pending:
        ++translate_stats.no_pipeline;
        // A pipeline made before a failure on its thread is retired, not destroyed here.
        translate_queue->Discard(std::move(job));
        return nullptr;
    }
    retry = true;
    if (++translate_failures[job->key] >= MaxTranslateFailures) {
        translate_sync_only.insert(job->key);
    }
    translate_queue->Discard(std::move(job));
    return nullptr;
}

void PipelineCache::TranslateOnWorker(TranslationJob& job) {
    boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram> env_ptrs;
    for (size_t index = 0; index < Maxwell::MaxShaderProgram; ++index) {
        if (job.key.unique_hashes[index] != 0) {
            env_ptrs.push_back(&job.envs[index]);
        }
    }
    // As the GPU thread's build: a native program's SPIR-V is emitted by the pipeline's job when
    // the queued IR budget has room (CreateGraphicsPipeline), otherwise here; the driver compile
    // is a job of shader_jobs.
    std::shared_ptr<ShaderPools> emit_pools;
    if (job.native) {
        emit_pools = TakeSparePools();
    }
    thread_local ShaderPools thread_pools;
    if (!emit_pools) {
        thread_pools.ReleaseContents();
    }
    ShaderPools& pools = emit_pools ? *emit_pools : thread_pools;
    t_translate_worker = true;
    SCOPE_EXIT {
        t_translate_worker = false;
    };
    job.pipeline = CreateGraphicsPipeline(pools, job.key, MakeSpan(env_ptrs), nullptr, true,
                                          std::move(emit_pools), nullptr, job.record.get());
    if (std::ranges::any_of(job.envs, [](const GraphicsEnvironment& env) {
            return env.GuardedReadFailed();
        })) {
        // Translated with zeros where a read gave up: never used, translated again from a fresh
        // snapshot. The pipeline stays in the job: the GPU thread retires it (AdoptJob, Discard)
        // rather than destroying it on this thread.
        job.outcome = TranslationJob::Outcome::Retry;
    } else if (job.reference && job.record) {
        verify_compared.fetch_add(1, std::memory_order_relaxed);
        if (const std::string difference = CompareTranslations(job); !difference.empty()) {
            verify_differing.fetch_add(1, std::memory_order_relaxed);
            if (verify_logged.fetch_add(1, std::memory_order_relaxed) < 8) {
                LOG_WARNING(Render_Vulkan, "Async translate verify: {} differs: {}",
                            PipelineShaders(job.key), difference);
            }
            // Never used: retired by the GPU thread (AdoptJob, Discard), which builds the key
            // itself from the live state.
            job.outcome = TranslationJob::Outcome::Differs;
        }
    }
    // The comparison is done; the copies go with the job's snapshot bytes.
    job.reference.reset();
    job.record.reset();
    for (GraphicsEnvironment& env : job.reference_envs) {
        env = GraphicsEnvironment{};
    }
    for (GraphicsEnvironment& env : job.envs) {
        env.ReleaseSnapshotBytes();
    }
}

void PipelineCache::RetireTranslatedPipeline(std::unique_ptr<GraphicsPipeline> pipeline) {
    if (pipeline) {
        retired_translations.emplace_back(scheduler.CurrentTick(), std::move(pipeline));
    }
}

void PipelineCache::TickTranslations() {
    translate_queue->RetireAbandoned();
    // Translations whose draws did not come back yet: adopted now, so the next draw finds them.
    for (auto& job : translate_queue->TakeFinished()) {
        const auto it = graphics_cache.find(job->key);
        if (it == graphics_cache.end() || it->second) {
            translate_queue->Discard(std::move(job));
            continue;
        }
        bool retry{};
        if (auto pipeline = AdoptJob(std::move(job), retry)) {
            it->second = std::move(pipeline);
            ++translate_stats.adopted_between_frames;
        } else if (retry) {
            // Not usable, and no draw to take a fresh snapshot from: the next lookup of the key
            // is a new one (queued again, or built on the GPU thread once it failed too often).
            graphics_cache.erase(it);
        }
    }
    if (frame_count % 60 == 0) {
        for (const auto& key : translate_queue->Sweep(TranslateSweepAge)) {
            ++translate_stats.swept;
            translate_sync_only.insert(key);
            if (const auto it = graphics_cache.find(key);
                it != graphics_cache.end() && !it->second) {
                graphics_cache.erase(it);
            }
            LOG_WARNING(Render_Vulkan,
                        "Async translate: {} still translating after {} s; built on the GPU "
                        "thread at its next draw instead",
                        PipelineShaders(key), TranslateSweepAge.count());
        }
    }
    auto& stats = translate_stats;
    const auto now = std::chrono::steady_clock::now();
    if (stats.window == std::chrono::steady_clock::time_point{}) {
        stats.window = now;
        return;
    }
    if (now - stats.window < std::chrono::seconds{10}) {
        return;
    }
    const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
    const u32 adopted = stats.adopted;
    LOG_INFO(Render_Vulkan,
             "Async translate in the last 10 s: {} queued ({:.1f} ms of GPU thread for "
             "environments and snapshots, {} KB copied, {} not queued: full), {} adopted ({} "
             "between frames; first lookup to ready {:.1f} ms average, {:.1f} ms worst; {:.1f} ms "
             "translating on its threads), {} lookups skipped while translating, {} waits "
             "({:.1f} ms), {} built on the GPU thread after all, {} queued again ({} stale "
             "texture descriptors, {} guarded reads gave up), {} made no pipeline, {} swept, {} "
             "in flight",
             stats.queued, ms(stats.enqueue_time), stats.snapshot_bytes / 1024, stats.full,
             adopted, stats.adopted_between_frames,
             adopted != 0 ? ms(stats.ready_total) / adopted : 0.0, ms(stats.ready_max),
             ms(stats.translation_time), stats.skipped, stats.waits, ms(stats.wait_time),
             stats.fallbacks, stats.requeued, stats.stale, stats.gave_up, stats.no_pipeline,
             stats.swept, translate_queue->InFlight());
    if (VideoCommon::AsyncTranslateMode() == 2) {
        LOG_INFO(Render_Vulkan,
                 "Async translate verify: {} compared, {} differ ({} rejected at adoption)",
                 verify_compared.exchange(0, std::memory_order_relaxed),
                 verify_differing.exchange(0, std::memory_order_relaxed), stats.differs);
    }
    stats = {};
    stats.window = now;
}

ComputePipelineCacheEntry PipelineCache::CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                               const ShaderInfo* shader,
                                                               std::span<const u64> keep_pointers) {
    const VideoCommon::Nx2Profile::Scope profile_scope{
        VideoCommon::Nx2Profile::Zone::ComputePipelineMiss};
    const auto begin = std::chrono::steady_clock::now();
    SCOPE_EXIT {
        ReportRuntimePipeline("Compute", begin,
                              fmt::format("pipeline={:016x} s6:{:016x} address={:#x}", key.Hash(),
                                          key.unique_hash,
                                          kepler_compute->regs.code_loc.Address() +
                                              kepler_compute->launch_description.program_start));
    };
    const GPUVAddr program_base{kepler_compute->regs.code_loc.Address()};
    const auto& qmd{kepler_compute->launch_description};
    ComputeEnvironment env{*kepler_compute, *gpu_memory, program_base, qmd.program_start,
                           shader->lowered_native};
    env.SetCachedSize(shader->size_bytes);
    env.KeepStoragePointers(keep_pointers);

    // The SPIR-V is emitted by the pipeline's job instead of here
    std::shared_ptr<ShaderPools> emit_pools = TakeSparePools();
    ShaderPools& pools = *emit_pools;
    auto entry{CreateComputePipeline(pools, key, env, nullptr, true, std::move(emit_pools))};
    if (const char* directory = std::getenv("EDEN_NX2_SHADER_DUMP");
        directory && !entry.pipeline && env.CanBeSerialized()) {
        const auto path = std::filesystem::path{directory} /
                          fmt::format("failed-compute-{:016x}-v{}.env", key.unique_hash, CACHE_VERSION);
        std::ofstream file{path, std::ios::binary};
        if (file) {
            env.Serialize(file, host_info.EnvironmentReadProperties());
            LOG_INFO(Render_Vulkan, "Captured failed compute environment: {} written={}",
                     path.string(), file.good());
        }
    }
    if (!entry.pipeline || pipeline_cache_filename.empty()) {
        return entry;
    }
    serialization_thread.QueueWork([this, key, env_ = std::move(env)] {
        const std::array<const GenericEnvironment*, 1> envs{&env_};
        const std::vector<u8> payload{VideoCommon::SerializePipelinePayload(
            std::span(reinterpret_cast<const char*>(&key), sizeof(key)), envs,
            host_info.EnvironmentReadProperties())};
        if (!payload.empty()) {
            cache_v2->Writer().Append(VideoCommon::ShaderCacheV2::RecordKind::Compute,
                                      ComputeRecordKey(payload), payload);
        }
    });
    QueueVulkanPipelineCacheFlush();
    return entry;
}

ComputePipelineCacheEntry PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    PipelineStatistics* statistics, bool build_in_parallel,
    std::shared_ptr<ShaderPools> emit_pools, bool may_defer) try {
    SCOPE_EXIT {
        if (emit_pools) {
            ReturnPools(std::move(emit_pools));
        }
    };
    auto hash = key.Hash();
    if (device.HasBrokenCompute()) {
        LOG_ERROR(Render_Vulkan, "Skipping {:#016x}", hash);
        return {.failure = "Device reports broken compute support"};
    }

    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);

    Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};

    // Dump it before error.
    if (Settings::values.dump_guest_shaders) {
        env.Dump(hash, key.unique_hash);
    }

    auto program{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
    const VkDriverIdKHR driver_id = device.GetDriverID();
    const bool needs_shared_mem_clamp =
        driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
        driver_id == VK_DRIVER_ID_ARM_PROPRIETARY || driver_id == VK_DRIVER_ID_MOLTENVK;
    const u32 max_shared_memory = device.GetMaxComputeSharedMemorySize();
    if (needs_shared_mem_clamp && program.shared_memory_size > max_shared_memory) {
        LOG_WARNING(Render_Vulkan,
                    "Compute shader {:#016x} requests {}KB shared memory but device max is {}KB - clamping",
                    key.unique_hash,
                    program.shared_memory_size / 1024,
                    max_shared_memory / 1024);
        program.shared_memory_size = max_shared_memory;
    }
    // A software HMMA program on a driver that takes minutes on it is built by a Background job
    // nothing waits for, its dispatches skipped meanwhile (DefersSoftwareMatrixPipelines)
    const bool deferred = may_defer && program.info.uses_software_matrix &&
                          device.DefersSoftwareMatrixPipelines();
    std::unique_ptr<ComputePipeline> pipeline;
    const u64 ir_bytes = emit_pools ? IrBytes(std::span(&program, 1)) : 0;
    if (auto ir_reservation = emit_pools ? queued_ir.TryReserve(ir_bytes) : nullptr) {
        auto job = std::make_shared<DeferredCompute>();
        if (VideoCommon::Nx2Perf::Verify()) {
            job->reference_spirv = EmitComputeShader(profile, program, key.unique_hash);
        }
        job->reservation = std::move(ir_reservation);
        job->pools = std::move(emit_pools);
        job->program = std::move(program);
        // Copied by the pipeline before its job can run
        const Shader::Info& info = job->program.info;
        auto emit = [this, job, shader_hash = key.unique_hash]() mutable {
            const auto begin = std::chrono::steady_clock::now();
            vk::ShaderModule module{EmitComputeModule(device, profile, job->program, shader_hash,
                                                      job->reference_spirv)};
            auto completed_pools = std::move(job->pools);
            job.reset();
            ReturnPools(std::move(completed_pools));
            AddRuntimeSpirvTime(2, begin);
            return module;
        };
        pipeline = std::make_unique<ComputePipeline>(
            device, scheduler, vulkan_pipeline_cache, pipeline_binaries.get(), hash,
            descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring,
            BuildQueue(build_in_parallel || deferred, ir_bytes), statistics,
            &shader_notify, info, vk::ShaderModule{}, key.unique_hash, std::move(emit), deferred);
    } else {
        vk::ShaderModule module{EmitComputeModule(device, profile, program, key.unique_hash)};
        const Shader::Info info{std::move(program.info)};
        if (!build_in_parallel) {
            // The driver compiles on this thread. Precache and boot workers would otherwise hold their
            // IR through the compile, up to hundreds of MiB for one large shader.
            program = {};
            pools.block.Free();
            pools.inst.Free();
        }
        pipeline = std::make_unique<ComputePipeline>(
            device, scheduler, vulkan_pipeline_cache, pipeline_binaries.get(), hash,
            descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring,
            BuildQueue(build_in_parallel || deferred), statistics, &shader_notify, info,
            std::move(module), key.unique_hash, std::function<vk::ShaderModule()>{}, deferred);
    }
    return {.pipeline = std::move(pipeline),
            .null_pointer_slots = env.IsNativeProgram() ? env.NullStoragePointerSlots()
                                                        : std::vector<std::pair<u32, u32>>{},
            .mapped_pointers =
                env.IsNativeProgram() ? env.MappedStoragePointerKeys() : std::vector<u64>{}};

} catch (const Shader::Exception& exception) {
    if (!build_in_parallel) {
        // A disk cache record this build cannot translate, as for graphics: built from the game
        // when first dispatched.
        NoteDiskRecordFailure("Compute", key.Hash(), exception.what());
    } else {
        LOG_ERROR(Render_Vulkan,
                  "Compute pipeline compilation failed shader={:#016x} pipeline={:#016x}: {}",
                  key.unique_hash, key.Hash(), exception.what());
    }
    return {.failure = exception.what()};
}

void PipelineCache::SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                                 const vk::PipelineCache& pipeline_cache,
                                                 u32 cache_version) {
    // Written beside the cache and renamed over it: the emulator stopped mid-write left the cache
    // empty, and every later boot failed to read it.
    auto temporary = filename;
    temporary += ".tmp";
    std::ofstream file(temporary, std::ios::binary);
    if (!file.is_open()) {
        LOG_ERROR(Common_Filesystem, "Failed to open Vulkan driver pipeline cache file {}",
                  Common::FS::PathToUTF8String(temporary));
        return;
    }
    try {
        file.exceptions(std::ifstream::failbit);
        file.write(VULKAN_CACHE_MAGIC_NUMBER.data(), VULKAN_CACHE_MAGIC_NUMBER.size())
            .write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));

        size_t cache_size = 0;
        std::vector<char> cache_data;
        if (pipeline_cache) {
            pipeline_cache.Read(&cache_size, nullptr);
            cache_data.resize(cache_size);
            pipeline_cache.Read(&cache_size, cache_data.data());
        }
        Common::NoteShutdownProgress();
        // In pieces: a cache of tens of thousands of pipelines is gigabytes, and a shutdown
        // waiting on it is told the write still moves.
        constexpr size_t Chunk = size_t{64} << 20;
        for (size_t offset = 0; offset < cache_size; offset += Chunk) {
            file.write(cache_data.data() + offset,
                       static_cast<std::streamsize>((std::min)(Chunk, cache_size - offset)));
            Common::NoteShutdownProgress();
        }
        file.close();
    } catch (const std::ios_base::failure& e) {
        // The cache stays as it was: only the file being written goes.
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        file.exceptions(std::ios::goodbit);
        file.close();
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return;
    }
    std::error_code error;
    std::filesystem::rename(temporary, filename, error);
    if (error) {
        LOG_ERROR(Common_Filesystem, "Failed to replace Vulkan driver pipeline cache file {}: {}",
                  Common::FS::PathToUTF8String(filename), error.message());
        std::filesystem::remove(temporary, error);
        return;
    }

    LOG_INFO(Render_Vulkan, "Vulkan driver pipelines cached at: {}",
             Common::FS::PathToUTF8String(filename));
}

vk::PipelineCache PipelineCache::LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                                         u32 expected_cache_version) {
    const auto create_pipeline_cache = [this](size_t data_size, const void* data) {
        VkPipelineCacheCreateInfo pipeline_cache_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .initialDataSize = data_size,
            .pInitialData = data};
        return device.GetLogical().CreatePipelineCache(pipeline_cache_ci);
    };
    try {
        std::ifstream file(filename, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            return create_pipeline_cache(0, nullptr);
        }
        file.exceptions(std::ifstream::failbit);
        const auto end{file.tellg()};
        file.seekg(0, std::ios::beg);

        std::array<char, 8> magic_number;
        u32 cache_version;
        // A file cut short (the emulator stopped while writing it) would throw from the reads
        // below, and macOS's libc++ throws an ios_base::failure the handler doesn't match.
        if (static_cast<size_t>(end) < magic_number.size() + sizeof(cache_version)) {
            file.close();
            LOG_ERROR(Common_Filesystem, "Truncated Vulkan driver pipeline cache file, deleting it");
            Common::FS::RemoveFile(filename);
            return create_pipeline_cache(0, nullptr);
        }
        file.read(magic_number.data(), magic_number.size())
            .read(reinterpret_cast<char*>(&cache_version), sizeof(cache_version));
        if (magic_number != VULKAN_CACHE_MAGIC_NUMBER || cache_version != expected_cache_version) {
            file.close();
            if (Common::FS::RemoveFile(filename)) {
                if (magic_number != VULKAN_CACHE_MAGIC_NUMBER) {
                    LOG_ERROR(Common_Filesystem, "Invalid Vulkan driver pipeline cache file");
                }
                if (cache_version != expected_cache_version) {
                    LOG_INFO(Common_Filesystem, "Deleting old Vulkan driver pipeline cache");
                }
            } else {
                LOG_ERROR(Common_Filesystem,
                          "Invalid Vulkan pipeline cache file and failed to delete it in \"{}\"",
                          Common::FS::PathToUTF8String(filename));
            }
            return create_pipeline_cache(0, nullptr);
        }

        static constexpr size_t header_size = magic_number.size() + sizeof(cache_version);
        const size_t cache_size = static_cast<size_t>(end) - header_size;
        std::vector<char> cache_data(cache_size);
        file.read(cache_data.data(), cache_size);

        LOG_INFO(Render_Vulkan,
                 "Loaded Vulkan driver pipeline cache: {}", Common::FS::PathToUTF8String(filename));

        try {
            return create_pipeline_cache(cache_size, cache_data.data());
        } catch (const vk::Exception& e) {
            // The blob came from another driver or driver version (e.g. switching between
            // MoltenVK and KosmicKrisp on macOS): the driver rejects it. Start empty instead of
            // aborting, and drop the stale file so the next run doesn't retry it.
            LOG_ERROR(Render_Vulkan,
                      "Driver rejected the Vulkan pipeline cache file ({}), starting with an empty "
                      "cache and deleting it",
                      e.what());
            Common::FS::RemoveFile(filename);
            return create_pipeline_cache(0, nullptr);
        }

    } catch (const std::ios_base::failure& e) {
        LOG_ERROR(Common_Filesystem, "{}", e.what());
        if (!Common::FS::RemoveFile(filename)) {
            LOG_ERROR(Common_Filesystem, "Failed to delete Vulkan driver pipeline cache file {}",
                      Common::FS::PathToUTF8String(filename));
        }

        return create_pipeline_cache(0, nullptr);
    }
}

} // namespace Vulkan
