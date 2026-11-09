// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <iostream>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <streambuf>
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
#include "video_core/pipeline_cache_files.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/pipeline_record_file.h"
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
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_records.h"
#include "video_core/renderer_vulkan/vk_pipeline_translate_queue.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
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

std::array<std::atomic<std::chrono::nanoseconds::rep>, 2> runtime_spirv_ns{};
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

struct DeferredStages {
    std::unique_ptr<EmissionBudget::Reservation> reservation;
    std::shared_ptr<ShaderPools> pools;
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

namespace {
// NVIDIA's and AMD's drivers keep a disk cache of built pipelines of their own. Eden's copy on top
// only kept every pipeline the title ever built in host memory (title 1: a 245 MB file, ~0.35 GB
// of RAM for the session) and copied it whole at each save, which stalled frames; pipelines built
// no slower without it.
bool DriverCachesPipelines(const Device& device) {
    switch (device.GetDriverID()) {
    case VK_DRIVER_ID_NVIDIA_PROPRIETARY:
    case VK_DRIVER_ID_AMD_PROPRIETARY:
        return true;
    default:
        return false;
    }
}
} // Anonymous namespace

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
                                !DriverCachesPipelines(device_)},
      num_pipeline_workers{device.HasBrokenParallelShaderCompiling() ? 1ULL
                                                                     : GetTotalPipelineWorkers()},
      emission_budget{num_pipeline_workers},
      workers(num_pipeline_workers, "VkPipelineBuilder", {}, Common::ThreadPlacement::Background),
      deferred_workers(1, "VkDeferredPipelines", {}, Common::ThreadPlacement::Background),
      serialization_thread(1, "VkPipelineSerialization", {}, Common::ThreadPlacement::Background),
      background_pipelines{64, 16 * 1024 * 1024, device.HasBrokenParallelShaderCompiling()} {
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
    dynamic_features.driver_id = device.GetDriverID();
    dynamic_features.driver_version = device.GetDriverVersion();

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
// <title>/vulkan_used.bin: the graphics pipelines recent sessions used, as key hash + sessions
// since the last use.
constexpr u32 UsedPipelinesMagic = 0x31505545; // "EUP1"
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

std::unordered_map<u64, u8> LoadUsedPipelines(const std::filesystem::path& path, u8 max_age) {
    std::unordered_map<u64, u8> used;
    std::ifstream file{path, std::ios::binary};
    u32 magic{}, count{};
    if (!file.read(reinterpret_cast<char*>(&magic), sizeof(magic)) || magic != UsedPipelinesMagic ||
        !file.read(reinterpret_cast<char*>(&count), sizeof(count))) {
        return used;
    }
    used.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        u64 hash{};
        u8 age{};
        if (!file.read(reinterpret_cast<char*>(&hash), sizeof(hash)) ||
            !file.read(reinterpret_cast<char*>(&age), sizeof(age))) {
            break;
        }
        if (age < max_age) { // lists saved with a longer window
            used.emplace(hash, age);
        }
    }
    return used;
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

// EDEN_NX2_PREBUILD_STREAM (on by default, =0 off): the disk cache records of the boot builds
// are not kept in memory from boot until a worker builds them (over a gigabyte for a large
// cache, all of it with EDEN_NX2_PRELOAD_ALL_PIPELINES); each keeps its place in the file and is
// read again just before its build (RecordStream). What is built, in which order, and which
// record is tried first stay as without it.
bool PrebuildStreamEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_PREBUILD_STREAM");
        const bool on = value == nullptr || value[0] != '0';
        LOG_INFO(Render_Vulkan, "EDEN_NX2_PREBUILD_STREAM: disk cache records {}",
                 on ? "read again for each boot build" : "kept in memory until built");
        return on;
    }();
    return enabled;
}

/// An input stream over bytes in memory, for FileEnvironment::Deserialize.
class MemoryStreamBuf final : public std::streambuf {
public:
    MemoryStreamBuf(char* data, size_t size) {
        setg(data, data, data + size);
    }

protected:
    pos_type seekoff(off_type offset, std::ios_base::seekdir dir,
                     std::ios_base::openmode) override {
        char* const base = eback();
        const off_type from = dir == std::ios_base::beg   ? 0
                              : dir == std::ios_base::cur ? gptr() - base
                                                          : egptr() - base;
        const off_type target = from + offset;
        if (target < 0 || target > egptr() - base) {
            return pos_type(off_type(-1));
        }
        setg(base, base + target, egptr());
        return pos_type(target);
    }
    pos_type seekpos(pos_type position, std::ios_base::openmode which) override {
        return seekoff(off_type(position), std::ios_base::beg, which);
    }
};

/// Reads the environments of `record` again (`key_size`: the size of its key, after them), as
/// VideoCommon::LoadPipelines read them. False when it cannot be read or is no longer the record
/// that was there (its bytes hash differently).
bool ReadRecordEnvs(const VideoCommon::PipelineRecordFile& file,
                    const VideoCommon::PipelineRecord& record, size_t key_size,
                    std::vector<FileEnvironment>& envs, std::vector<char>& buffer) {
    const u64 size = record.end - record.begin;
    if (record.end <= record.begin || size < sizeof(u32) + key_size || size > (u64{1} << 31)) {
        return false;
    }
    buffer.resize(static_cast<size_t>(size));
    if (!file.ReadAt(record.begin, buffer) ||
        Common::CityHash64(buffer.data(), buffer.size()) != record.hash) {
        return false;
    }
    const size_t envs_size = buffer.size() - key_size;
    MemoryStreamBuf memory{buffer.data(), envs_size};
    std::istream stream{&memory};
    stream.exceptions(std::ios_base::failbit | std::ios_base::badbit);
    try {
        u32 num_envs{};
        stream.read(reinterpret_cast<char*>(&num_envs), sizeof(num_envs));
        if (num_envs == 0 || num_envs > Maxwell::MaxShaderProgram) {
            return false;
        }
        envs.clear();
        envs.resize(num_envs);
        const std::streampos end{static_cast<std::streamoff>(envs_size)};
        for (FileEnvironment& env : envs) {
            env.Deserialize(stream, end);
        }
        // The environments end where the key begins.
        return static_cast<u64>(std::streamoff{stream.tellg()}) == envs_size;
    } catch (const std::exception&) {
        return false;
    }
}

/// Reader-side totals of the records read again, for the boot build's log line.
struct StreamTotals {
    std::atomic<u64> records{};
    std::atomic<u64> failed{};
    std::atomic<u64> bytes{};
    std::atomic<u64> read_ns{};
    std::atomic<u64> wait_ns{}; ///< Time workers waited for their records
    std::atomic<u64> waits{};
};

/// EDEN_NX2_PREBUILD_STREAM: the records of queued builds, read again from the disk cache by one
/// thread in the order the builds were queued, up to `window` builds ahead of the workers. A
/// worker finds its records read already instead of waiting on the disk, and only the window is
/// held instead of every record from boot until its build. The workers take builds in queue
/// order (ThreadWorker is first in, first out), so at most one per worker is ahead of a waiting
/// one, and a window over the worker count always reaches it.
class RecordStream {
public:
    RecordStream(std::shared_ptr<const VideoCommon::PipelineRecordFile> file_, size_t window_)
        : file{std::move(file_)}, window{window_} {
        reader = std::jthread([this](std::stop_token stop) { Run(stop); });
    }
    ~RecordStream() {
        reader.request_stop();
        if (reader.joinable()) {
            reader.join();
        }
    }
    RecordStream(const RecordStream&) = delete;
    RecordStream& operator=(const RecordStream&) = delete;

    [[nodiscard]] const StreamTotals& Totals() const noexcept {
        return totals;
    }

    /// Adds a build's records (tried in this order); returns its index for Take. Builds are
    /// added in the order they are queued.
    size_t Add(std::vector<VideoCommon::PipelineRecord> refs, size_t key_size) {
        std::scoped_lock lock{mutex};
        entries.push_back(Entry{std::move(refs), key_size});
        changed.notify_all();
        return entries.size() - 1;
    }
    /// No more builds come: the reader ends (and closes the file) once it has read them all.
    void Seal() {
        std::scoped_lock lock{mutex};
        sealed = true;
        changed.notify_all();
    }

    /// The records of build `index` (one per record added, empty for one that could not be read
    /// again), waiting for them if the reader is behind (counted in wait_ns).
    std::vector<std::vector<FileEnvironment>> Take(size_t index) {
        std::unique_lock lock{mutex};
        Entry& entry = entries[index];
        if (entry.state != State::Ready && !finished) {
            const auto begin = std::chrono::steady_clock::now();
            changed.wait(lock, [&] { return entry.state == State::Ready || finished; });
            const auto waited = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin);
            totals.wait_ns.fetch_add(static_cast<u64>(waited.count()), std::memory_order_relaxed);
            totals.waits.fetch_add(1, std::memory_order_relaxed);
        }
        std::vector<std::vector<FileEnvironment>> records;
        if (entry.state == State::Ready) {
            records = std::move(entry.records);
            --ready;
        }
        entry.state = State::Taken;
        lock.unlock();
        changed.notify_all();
        return records;
    }

private:
    enum class State { Queued, Ready, Taken };
    struct Entry {
        std::vector<VideoCommon::PipelineRecord> refs;
        size_t key_size{};
        State state{State::Queued};
        std::vector<std::vector<FileEnvironment>> records;
    };

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("VkRecordReader");
        std::vector<char> buffer;
        size_t next = 0;
        while (!stop.stop_requested()) {
            std::vector<VideoCommon::PipelineRecord> refs;
            size_t key_size{};
            {
                std::unique_lock lock{mutex};
                changed.wait(lock, stop, [&] {
                    return (next < entries.size() && ready < window) ||
                           (sealed && next >= entries.size());
                });
                if (stop.stop_requested() || next >= entries.size()) {
                    break;
                }
                refs = entries[next].refs;
                key_size = entries[next].key_size;
            }
            const auto begin = std::chrono::steady_clock::now();
            std::vector<std::vector<FileEnvironment>> records(refs.size());
            u64 bytes = 0;
            for (size_t index = 0; index < refs.size(); ++index) {
                if (ReadRecordEnvs(*file, refs[index], key_size, records[index], buffer)) {
                    bytes += refs[index].end - refs[index].begin;
                } else {
                    records[index].clear();
                    totals.failed.fetch_add(1, std::memory_order_relaxed);
                }
            }
            totals.records.fetch_add(refs.size(), std::memory_order_relaxed);
            totals.bytes.fetch_add(bytes, std::memory_order_relaxed);
            totals.read_ns.fetch_add(
                static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count()),
                std::memory_order_relaxed);
            {
                std::scoped_lock lock{mutex};
                Entry& entry = entries[next];
                entry.records = std::move(records);
                entry.state = State::Ready;
                ++ready;
                ++next;
            }
            changed.notify_all();
        }
        {
            std::scoped_lock lock{mutex};
            finished = true;
        }
        changed.notify_all();
        // Nothing more to read: the file is not held open for the rest of the session.
        file.reset();
    }

    std::shared_ptr<const VideoCommon::PipelineRecordFile> file; ///< The reader thread's
    const size_t window;
    StreamTotals totals;
    std::mutex mutex;
    std::condition_variable_any changed;
    std::deque<Entry> entries;
    size_t ready{}; ///< Entries read and not taken
    bool sealed{};
    bool finished{};
    std::jthread reader; ///< Last: started once the members above exist
};

/// "workers waited 0.12 s on 3 reads, 812 records (41 MiB) read in 0.40 s, 0 failed"
std::string StreamTotalsText(const StreamTotals& totals) {
    return fmt::format("workers waited {:.2f} s on {} reads, {} records ({:.0f} MiB) read in "
                       "{:.2f} s, {} failed",
                       static_cast<double>(totals.wait_ns.load()) / 1e9, totals.waits.load(),
                       totals.records.load(),
                       static_cast<double>(totals.bytes.load()) / (1024.0 * 1024.0),
                       static_cast<double>(totals.read_ns.load()) / 1e9, totals.failed.load());
}

// EDEN_NX2_REKEY_FOREIGN_PIPELINES=0: cached graphics pipelines another GPU driver (or driver
// version) stored are skipped at boot instead of loaded under this driver (see load_graphics).
bool RekeyForeignPipelines() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_REKEY_FOREIGN_PIPELINES");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
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
    // The stop callback goes first: it uses background_pipelines.
    loading_stop_callback.reset();
    // Its threads build with the members below (a translation in progress finishes).
    translate_queue.reset();
    serialization_stop.request_stop();
    background_pipelines.RequestStop();
    if (const auto usage = background_pipelines.GetUsage(); usage.taken != 0 || usage.unused != 0) {
        // Prebuilt pipelines a draw never took cost driver memory and build time for nothing.
        LOG_INFO(Render_Vulkan,
                 "Background pipelines this session: {} taken by draws, {} never used",
                 usage.taken, usage.unused);
    }
    background_pipelines.Stop();
    Common::NoteShutdownProgress();
    workers.WaitForRequests();
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
    DestroyPipelinesWithProgress(graphics_cache);
    DestroyPipelinesWithProgress(evicted_pipelines);
    DestroyPipelinesWithProgress(retired_translations);
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
    if (used_pipelines_filename.empty() ||
        used_this_session.size() + used_and_evicted.size() < MinimumUsed) {
        return;
    }
    std::unordered_map<u64, u8> next;
    next.reserve(used_pipelines.size() + used_this_session.size());
    for (const auto& [key, pipeline] : graphics_cache) {
        if (used_this_session.contains(pipeline.get())) {
            next.emplace(key.Hash(), u8{0});
        }
    }
    for (const u64 hash : used_and_evicted) {
        next.emplace(hash, u8{0});
    }
    for (const auto& [hash, age] : used_pipelines) {
        if (age + 1 < MaxUsedPipelineAge(device)) {
            next.try_emplace(hash, static_cast<u8>(age + 1));
        }
    }
    std::ofstream file{used_pipelines_filename, std::ios::binary | std::ios::trunc};
    const u32 count = static_cast<u32>(next.size());
    file.write(reinterpret_cast<const char*>(&UsedPipelinesMagic), sizeof(UsedPipelinesMagic));
    file.write(reinterpret_cast<const char*>(&count), sizeof(count));
    for (const auto& [hash, age] : next) {
        file.write(reinterpret_cast<const char*>(&hash), sizeof(hash));
        file.write(reinterpret_cast<const char*>(&age), sizeof(age));
    }
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
    return entry.Get(kepler_compute->IsNativeMode(),
                     kepler_compute->regs.code_loc.Address() + qmd.program_start, key);
}

void PipelineCache::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                      const VideoCore::DiskResourceLoadCallback& callback) {
    if (title_id == 0) {
        return;
    }
    if (!pipeline_cache_filename.empty()) {
        background_pipelines.Stop();
        workers.WaitForRequests();
        serialization_thread.WaitForRequests();
        if (use_vulkan_pipeline_cache && !vulkan_pipeline_cache_filename.empty()) {
            SerializeVulkanPipelineCache(vulkan_pipeline_cache_filename, vulkan_pipeline_cache,
                                         CACHE_VERSION);
        }
    }

    const auto shader_dir{Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir)};
    const auto base_dir{shader_dir / fmt::format("{:016x}", title_id)};
    if (!Common::FS::CreateDir(shader_dir) || !Common::FS::CreateDir(base_dir)) {
        LOG_ERROR(Common_Filesystem, "Failed to create pipeline cache directories");
        return;
    }
    pipeline_cache_filename =
        VideoCommon::VersionedPipelineCachePath(base_dir, "vulkan", CACHE_VERSION);
    used_pipelines_filename =
        VideoCommon::VersionedPipelineCachePath(base_dir, "vulkan_used", CACHE_VERSION);
    const auto carried = VideoCommon::CarryOverPipelineCache(
        base_dir, "vulkan", VideoCommon::PipelineCacheMagic,
        VideoCommon::CompatibleVulkanPipelineCacheVersions, CACHE_VERSION);
    if (carried.version) {
        // The used list goes with the pipelines it was saved beside, never one this version kept
        // after its own cache was removed.
        const auto carried_used =
            VideoCommon::VersionedPipelineCachePath(base_dir, "vulkan_used", *carried.version);
        std::error_code migration_error;
        if (std::filesystem::exists(carried_used, migration_error)) {
            std::filesystem::copy_file(carried_used, used_pipelines_filename,
                                       std::filesystem::copy_options::overwrite_existing,
                                       migration_error);
        } else {
            std::filesystem::remove(used_pipelines_filename, migration_error);
        }
        LOG_INFO(Render_Vulkan, "Pipeline cache started from the version {} cache",
                 *carried.version);
    } else if (carried.failed) {
        LOG_WARNING(Render_Vulkan, "Couldn't copy the older pipeline cache; this one starts empty");
    }
    used_pipelines = LoadUsedPipelines(used_pipelines_filename, MaxUsedPipelineAge(device));
    used_pipelines_saved = std::chrono::steady_clock::now();
    size_t skipped_pipelines = 0;
    size_t deferred_pipelines = 0;
    size_t queued_graphics = 0;

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
    std::vector<PrecacheJob> precache_graphics;
    size_t precache_capture_bytes = 0;

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

    struct {
        std::mutex mutex;
        size_t total{};
        size_t built{};
        bool has_loaded{};
        std::unique_ptr<PipelineStatistics> statistics;
        std::chrono::steady_clock::time_point last_save{std::chrono::steady_clock::now()};
        /// Records the load never chooses: those lacking a value the translator reads (graphics
        /// ones with their key hash), and copies of a compute translation that stays
        std::vector<VideoCommon::PipelineRecord> incomplete;
        std::vector<std::pair<u64, VideoCommon::PipelineRecord>> incomplete_graphics;
        std::vector<VideoCommon::PipelineRecord> copies;
    } state;
    // A rebuilt translation is cached beside the stale one. Choose between them before building, so
    // loading only ever adds entries (see OrderComputeTranslations), and take the next when a
    // translation fails.
    ::Common::unordered_map<ComputePipelineCacheKey, std::vector<CachedComputeTranslation>>
        compute_envs;
    // Where each record read begins and ends, to remove those the load never chooses.
    u64 record_begin{VideoCommon::PipelineCacheMagic.size() + sizeof(CACHE_VERSION)};
    const auto read_record{[&](std::ifstream& file) {
        const auto record{VideoCommon::ReadPipelineRecord(file, record_begin)};
        record_begin = record.end;
        return record;
    }};
    /// For each graphics key hash read, where the first record's key is and its stages
    std::unordered_map<u64, std::pair<u64, u64>> first_graphics;
    std::vector<DuplicateGraphicsRecord> graphics_duplicates;

    if (device.IsKhrPipelineExecutablePropertiesEnabled()) {
        state.statistics = std::make_unique<PipelineStatistics>(device);
    }
    // EDEN_NX2_PREBUILD_STREAM: the boot builds' records are read again for each build, by one
    // thread a few builds ahead of the workers. The file is opened before the loader reads it
    // (see PipelineRecordFile); without one there is nothing to read.
    std::shared_ptr<RecordStream> boot_stream;
    if (PrebuildStreamEnabled()) {
        auto record_file =
            std::make_shared<VideoCommon::PipelineRecordFile>(pipeline_cache_filename);
        if (record_file->IsOpen()) {
            boot_stream = std::make_shared<RecordStream>(std::move(record_file),
                                                         2 * num_pipeline_workers + 8);
        }
    }
    const auto load_compute{[&](std::ifstream& file, FileEnvironment env) {
        ComputePipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        VideoCommon::CheckComputeRecord(env, key.shared_memory_size, key.workgroup_size);
        compute_envs[key].push_back({std::move(env), read_record(file)});
    }};
    const auto build_compute{[this, &state, &callback](
                                 const ComputePipelineCacheKey& key,
                                 std::vector<CachedComputeTranslation>& candidates) {
        ShaderPools pools;
        ComputePipelineCacheEntry entry;
        size_t chosen{};
        for (; chosen < candidates.size(); ++chosen) {
            pools.ReleaseContents();
            entry = CreateComputePipeline(pools, key, candidates[chosen].env,
                                          state.statistics.get(), false);
            if (entry.pipeline) {
                break;
            }
        }
        std::scoped_lock lock{state.mutex};
        if (entry.pipeline) {
            compute_cache.emplace(key, std::move(entry));
        }
        AddUnchosenComputeTranslations(candidates, chosen, host_info.EnvironmentReadProperties(),
                                       state.incomplete, state.copies);
        ++state.built;
        if (state.has_loaded) {
            callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
        }
    }};
    const auto queue_compute{[&](const ComputePipelineCacheKey& key,
                                 std::vector<CachedComputeTranslation> candidates) {
        if (boot_stream) {
            // In the order chosen above; the environments are read again for the build. One that
            // can't be read again is left out, as a record that doesn't load.
            std::vector<VideoCommon::PipelineRecord> refs;
            refs.reserve(candidates.size());
            for (const CachedComputeTranslation& candidate : candidates) {
                refs.push_back(candidate.record);
            }
            candidates.clear();
            const size_t index = boot_stream->Add(refs, sizeof(ComputePipelineCacheKey));
            workers.QueueWork([key, index, stream_ = boot_stream, refs_ = std::move(refs),
                               build_compute]() mutable {
                auto records = stream_->Take(index);
                std::vector<CachedComputeTranslation> candidates_;
                for (size_t i = 0; i < records.size() && i < refs_.size(); ++i) {
                    if (!records[i].empty()) {
                        candidates_.push_back({std::move(records[i].front()), refs_[i]});
                    }
                }
                records.clear();
                build_compute(key, candidates_);
            });
        } else {
            workers.QueueWork([key, candidates_ = std::move(candidates), build_compute]() mutable {
                build_compute(key, candidates_);
            });
        }
        ++state.total;
    }};
    // Graphics records left out, by reason: a cache recorded on another GPU or with another
    // dynamic state setting holds pipelines this device can't use (logged after loading).
    enum DroppedReason : size_t {
        DropMesh,
        DropEds1,
        DropEds2,
        DropEds2LogicOp,
        DropEds3Blend,
        DropEds3Enables,
        DropColorWriteEnable,
        DropVertexInput,
        DropProvokingVertex,
        DropDuplicate,
        DropCount,
    };
    std::array<size_t, DropCount> dropped{};
    std::array<size_t, DropCount> dropped_with_feature{}; ///< records that had the feature on
    size_t graphics_records = 0;
    size_t other_driver = 0;  ///< records from another driver or driver version
    size_t extra_records = 0; ///< records whose (masked) key an earlier record has
    const auto load_graphics{[&](std::ifstream& file, std::vector<FileEnvironment> envs) {
        GraphicsPipelineCacheKey key;
        file.read(reinterpret_cast<char*>(&key), sizeof(key));
        VideoCommon::CheckGraphicsRecord(key.unique_hashes, envs.size());
        const auto record{read_record(file)};
        const u64 stored_key_hash = key.Hash();
        u64 stages{};
        for (const FileEnvironment& env : envs) {
            stages = (stages << 8) | static_cast<u32>(env.ShaderStage());
        }
        // The checks below and disk_pipelines take the first record of a key: a later one with the
        // same key and stages is never built, whatever the device. Compared as stored, before
        // the conversions and masking below, so only records any build would key alike go.
        if (const auto [first, inserted] =
                first_graphics.try_emplace(key.Hash(), record.end - sizeof(key), stages);
            !inserted && first->second.second == stages) {
            graphics_duplicates.push_back({record, first->second.first});
        }
        ++graphics_records;

        // A key holds the driver that stored it, which never met a runtime key after a driver
        // update (or in a cache shared from another driver), so every such pipeline was built
        // again in game. Nothing builds from those fields: take this driver's. A record whose
        // dynamic states differ from this device's is left out below, as Fritchii leaves it.
        auto& fixed = key.state;
        if (fixed.driver_id != dynamic_features.driver_id ||
            fixed.driver_version != dynamic_features.driver_version) {
            ++other_driver;
            if (!RekeyForeignPipelines()) {
                return;
            }
            fixed.driver_id = dynamic_features.driver_id;
            fixed.driver_version = dynamic_features.driver_version;
        }
        // One key for every op while the logic op is off, as Refresh makes them.
        if (fixed.extended_dynamic_state_2_logic_op == 0 &&
            !dynamic_features.has_dynamic_state3_logic_op_enable &&
            fixed.dynamic_state.logic_op_enable == 0) {
            fixed.dynamic_state.logic_op.Assign(0);
        }

        const std::array<std::pair<bool, bool>, DropVertexInput - DropEds1 + 1> features{{
            {key.state.extended_dynamic_state != 0, dynamic_features.has_extended_dynamic_state},
            {key.state.extended_dynamic_state_2 != 0,
             dynamic_features.has_extended_dynamic_state_2},
            {key.state.extended_dynamic_state_2_logic_op != 0,
             dynamic_features.has_extended_dynamic_state_2_logic_op},
            {key.state.extended_dynamic_state_3_blend != 0,
             dynamic_features.has_extended_dynamic_state_3_blend},
            {key.state.extended_dynamic_state_3_enables != 0,
             dynamic_features.has_extended_dynamic_state_3_enables},
            {key.state.color_write_enable_dynamic != 0, dynamic_features.has_color_write_enable},
            {key.state.dynamic_vertex_input != 0, dynamic_features.has_dynamic_vertex_input},
        }};
        for (size_t index = 0; index < features.size(); ++index) {
            const auto [recorded, here] = features[index];
            if (recorded != here) {
                ++dropped[DropEds1 + index];
                dropped_with_feature[DropEds1 + index] += recorded ? 1 : 0;
                return;
            }
        }

        // Keys stored before they kept only the attribute types the programs read are looked up
        // masked. The pipeline is the same: a type is only dropped for an attribute no program
        // reads. Records that now share a key are built once, from the first; the file keeps them
        // all (other builds key them apart), and the used list is carried over to the masked
        // hashes.
        if (key.state.dynamic_vertex_input != 0) {
            std::array<u64, Maxwell::MaxShaderProgram> attribute_masks;
            attribute_masks.fill(~u64{0});
            size_t env_index = 0;
            for (size_t stage = 0; stage < Maxwell::MaxShaderProgram; ++stage) {
                if (key.unique_hashes[stage] != 0 && env_index < envs.size()) {
                    attribute_masks[stage] = VideoCommon::VertexAttributeMaskOf(envs[env_index++]);
                }
            }
            const u64 stored_hash = key.Hash();
            key.state.attribute_types = VideoCommon::KeyedAttributeTypes(
                key.state.attribute_types, key.unique_hashes, attribute_masks);
            if (const auto used = used_pipelines.find(stored_hash); used != used_pipelines.end()) {
                const u8 age = used->second;
                const auto [entry, inserted] = used_pipelines.try_emplace(key.Hash(), age);
                entry->second = (std::min)(entry->second, age);
            }
        }

        const bool key_requests_provoking_last = key.state.provoking_vertex_last != 0;
        if (key_requests_provoking_last && !dynamic_features.has_provoking_vertex_last_mode) {
            ++dropped[DropProvokingVertex];
            return;
        }

        const bool key_uses_transform_feedback = key.state.xfb_enabled != 0;
        if (key_uses_transform_feedback && key_requests_provoking_last &&
            !dynamic_features.has_provoking_vertex_tf_preserve) {
            ++dropped[DropProvokingVertex];
            return;
        }

        const u64 hash = key.Hash();
        // A key the conversions above changed (another driver's, another dynamic state form)
        // takes the used-list age of the key as stored (Fritchii v5 ececc0bbd).
        if (hash != stored_key_hash) {
            if (const auto used = used_pipelines.find(stored_key_hash);
                used != used_pipelines.end()) {
                const u8 age = used->second;
                const auto [entry, inserted] = used_pipelines.try_emplace(hash, age);
                entry->second = (std::min)(entry->second, age);
            }
        }
        if (!disk_pipelines.insert(hash).second) {
            ++extra_records;
            return;
        }
        // Pipelines the last sessions used are built at boot; with LICHII_KEEP_PIPELINES, the
        // others too while fewer than that many are queued.
        const bool used = used_pipelines.empty() ? queued_graphics < MaxPipelinesWithoutUsedList
                                                 : used_pipelines.contains(hash);
        if (!PreloadAllPipelines() && !used && queued_graphics >= KeptPipelines()) {
            ++skipped_pipelines;
            size_t capture_bytes = (envs.capacity() - envs.size()) * sizeof(FileEnvironment);
            for (const auto& env : envs) {
                capture_bytes += env.MemorySize();
            }
            if (precache && !precached_pipelines.contains(hash) &&
                capture_bytes <= PrecacheCaptureLimit - precache_capture_bytes) {
                precache_capture_bytes += capture_bytes;
                precache_graphics.push_back({hash, [this, key, envs = std::move(envs)]() mutable {
                    ShaderPools pools;
                    boost::container::static_vector<Shader::Environment*,
                                                    Maxwell::MaxShaderProgram>
                        env_ptrs;
                    for (auto& env : envs) {
                        env_ptrs.push_back(&env);
                    }
                    const auto pipeline =
                        CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), nullptr, false);
                    return pipeline != nullptr && !pipeline->Failed();
                }});
                return;
            }
            if (background_pipelines.Add(
                    key, capture_bytes, [this, key, envs = std::move(envs)]() mutable {
                        ShaderPools pools;
                        boost::container::static_vector<Shader::Environment*,
                                                        Maxwell::MaxShaderProgram>
                            env_ptrs;
                        for (auto& env : envs) {
                            env_ptrs.push_back(&env);
                        }
                        auto pipeline =
                            CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs), nullptr, false);
                        if (pipeline && pipeline->Failed()) {
                            return std::unique_ptr<GraphicsPipeline>{};
                        }
                        // These builds fill the driver cache too, but only GPU thread builds
                        // queued its writes: a session stopped before its exit save kept none.
                        if (pipeline) {
                            std::scoped_lock save_lock{background_save_mutex};
                            if (VulkanPipelineCacheSaveDue(background_last_save)) {
                                QueueVulkanPipelineCacheSave(false);
                            }
                        }
                        return pipeline;
                    })) {
                ++deferred_pipelines;
            }
            return;
        }
        ++queued_graphics;

        const auto build = [this, key, record, &state,
                            &callback](std::vector<FileEnvironment>& envs_) {
            std::unique_ptr<GraphicsPipeline> pipeline;
            bool incomplete{};
            // Empty when EDEN_NX2_PREBUILD_STREAM could not read the record again: left out as
            // a record that doesn't load (a draw builds the pipeline when it needs it).
            if (!envs_.empty()) {
                ShaderPools pools;
                boost::container::static_vector<Shader::Environment*, Maxwell::MaxShaderProgram>
                    env_ptrs;
                for (auto& env : envs_) {
                    env_ptrs.push_back(&env);
                }
                pipeline = CreateGraphicsPipeline(pools, key, MakeSpan(env_ptrs),
                                                  state.statistics.get(), false);
                incomplete = !pipeline && std::ranges::any_of(envs_, [this](const auto& env) {
                    return env.LacksRecordedValue(host_info.EnvironmentReadProperties());
                });
            }

            std::scoped_lock lock{state.mutex};
            if (pipeline) {
                graphics_cache.emplace(key, std::move(pipeline));
            }
            if (incomplete) {
                state.incomplete_graphics.emplace_back(key.Hash(), record);
            }
            // A boot that builds tens of thousands of pipelines takes minutes: what it built so
            // far is written now and then, so a boot that ends early (a crash, the app killed)
            // does not leave the next one to compile everything again.
            if (VulkanPipelineCacheSaveDue(state.last_save)) {
                QueueVulkanPipelineCacheSave(false);
            }
            ++state.built;
            if (state.has_loaded) {
                callback(VideoCore::LoadCallbackStage::Build, state.built, state.total);
            }
        };
        if (boot_stream) {
            const size_t index = boot_stream->Add({record}, sizeof(GraphicsPipelineCacheKey));
            workers.QueueWork([index, stream_ = boot_stream, build]() mutable {
                auto records = stream_->Take(index);
                std::vector<FileEnvironment> envs_;
                if (!records.empty()) {
                    envs_ = std::move(records.front());
                }
                records.clear();
                build(envs_);
            });
        } else {
            workers.QueueWork([envs_ = std::move(envs), build]() mutable { build(envs_); });
        }
        ++state.total;
    }};
    VideoCommon::LoadPipelines(stop_loading, pipeline_cache_filename, CACHE_VERSION, load_compute,
                               load_graphics);
    for (auto& [key, candidates] : compute_envs) {
        OrderComputeTranslations(candidates);
        queue_compute(key, std::move(candidates));
    }
    compute_envs.clear();
    if (boot_stream) {
        boot_stream->Seal();
    }

    LOG_INFO(Render_Vulkan, "Total Pipeline Count: {}", state.total);
    if (PreloadAllPipelines() && queued_graphics > 10000) {
        LOG_WARNING(Render_Vulkan,
                    "EDEN_NX2_PRELOAD_ALL_PIPELINES: building and keeping all {} cached graphics "
                    "pipelines holds ~{} GB of driver memory all session",
                    queued_graphics, queued_graphics * 460 / (1024 * 1024));
    }
    if (const size_t total_dropped = std::accumulate(dropped.begin(), dropped.end(), size_t{});
        total_dropped != 0) {
        static constexpr std::array<const char*, DropCount> reasons{
            "need mesh shaders",
            "extended dynamic state",
            "extended dynamic state 2",
            "extended dynamic state 2 logic op",
            "extended dynamic state 3 blending",
            "extended dynamic state 3 enables",
            "dynamic color write enable",
            "vertex input dynamic state",
            "need the last provoking vertex mode",
            "duplicates",
        };
        std::string detail;
        for (size_t index = 0; index < DropCount; ++index) {
            if (dropped[index] == 0) {
                continue;
            }
            detail += detail.empty() ? ": " : ", ";
            if (index >= DropEds1 && index <= DropVertexInput) {
                // A record is left out for the first feature it differs in, so all of these
                // differ the same way: on when recorded and off with this GPU and settings, or
                // the reverse.
                detail += fmt::format("{} recorded with {} {}", dropped[index], reasons[index],
                                      dropped_with_feature[index] != 0 ? "on (off here)"
                                                                       : "off (on here)");
            } else {
                detail += fmt::format("{} {}", dropped[index], reasons[index]);
            }
        }
        LOG_WARNING(Render_Vulkan, "Pipeline cache: {} of {} cached graphics pipelines left out{}",
                    total_dropped, graphics_records, detail);
    }
    if (other_driver != 0) {
        LOG_INFO(Render_Vulkan,
                 "Pipeline cache: {} of {} cached graphics pipelines from another driver or driver "
                 "version ({})",
                 other_driver, graphics_records,
                 RekeyForeignPipelines() ? "loaded under this one"
                                         : "skipped: EDEN_NX2_REKEY_FOREIGN_PIPELINES=0");
    }
    if (skipped_pipelines != 0) {
        LOG_INFO(
            Render_Vulkan,
            "{} cached graphics pipelines skipped at boot, {} admitted for background prebuild",
            skipped_pipelines, deferred_pipelines);
    }
    if (extra_records != 0) {
        LOG_INFO(Render_Vulkan,
                 "Pipeline cache: {} graphics records share their pipeline with an earlier one "
                 "(the same key, or one differing only in vertex attribute types the programs "
                 "don't read); each pipeline is built once, from its first record",
                 extra_records);
    }

    std::unique_lock lock{state.mutex};
    callback(VideoCore::LoadCallbackStage::Build, 0, state.total);
    state.has_loaded = true;
    lock.unlock();

    workers.WaitForRequests(stop_loading);
    ReportDiskRecordFailures();
    if (boot_stream) {
        LOG_INFO(Render_Vulkan, "Boot pipeline builds done, records read again: {}",
                 StreamTotalsText(boot_stream->Totals()));
        boot_stream.reset();
    }

    AddIncompleteGraphics(state.incomplete_graphics, graphics_duplicates, disk_pipelines,
                          state.incomplete);
    // Records the load never chooses are read again at every boot. The serialization thread
    // rewrites the file without them, before it appends the pipelines built from now on: at once
    // for incomplete ones, else once the superseded ones take up an eighth of the file.
    u64 superseded_bytes{};
    for (const auto& record : state.copies) {
        superseded_bytes += record.end - record.begin;
    }
    for (const auto& duplicate : graphics_duplicates) {
        superseded_bytes += duplicate.record.end - duplicate.record.begin;
    }
    if (!stop_loading.stop_requested() &&
        (!state.incomplete.empty() ||
         (superseded_bytes != 0 && superseded_bytes >= record_begin / 8))) {
        auto removed{std::move(state.incomplete)};
        removed.insert(removed.end(), state.copies.begin(), state.copies.end());
        serialization_thread.QueueWork([filename = pipeline_cache_filename,
                                        loaded_size = record_begin, records = std::move(removed),
                                        duplicates = std::move(graphics_duplicates), stop_loading,
                                        destroyed = serialization_stop.get_token()]() mutable {
            // Stopped by a cancelled load, or by the cache's destruction.
            std::stop_source stop;
            const std::stop_callback on_cancel{stop_loading, [&stop] { stop.request_stop(); }};
            const std::stop_callback on_destroy{destroyed, [&stop] { stop.request_stop(); }};
            if (stop.stop_requested()) {
                return;
            }
            AddEqualGraphicsKeys(filename, duplicates, records);
            VideoCommon::RemovePipelines(stop.get_token(), filename, CACHE_VERSION, loaded_size,
                                         std::move(records));
        });
    }

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
    if (precache && !stop_loading.stop_requested()) {
        PrecacheShaders(title_id, std::move(precache_graphics), std::move(precached_pipelines),
                        stop_loading, callback);
        ReportDiskRecordFailures();
    }
    if (stop_loading.stop_requested()) {
        background_pipelines.Stop();
    } else {
        // Stopping emulation also stops the background builds at once: they would otherwise
        // compile on until the cache is destroyed, competing with the teardown for the CPU.
        loading_stop_callback.emplace(
            stop_loading, std::function<void()>{[this] { background_pipelines.RequestStop(); }});
        {
            std::scoped_lock save_lock{background_save_mutex};
            background_last_save = std::chrono::steady_clock::now();
        }
        background_pipelines.Start(std::clamp<size_t>(num_pipeline_workers / 4, 1, 2));
    }
}

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
                jobs.push_back({0,
                                [this, key, compute_env = std::move(env)]() mutable {
                                    static const GuessMissing guess{};
                                    compute_env.SetGuessMissing(&guess);
                                    ShaderPools pools;
                                    // Built here, not queued: the pipeline is dropped at once.
                                    return CreateComputePipeline(pools, key, compute_env, nullptr,
                                                                 false, false)
                                               .pipeline != nullptr;
                                },
                                key.unique_hash});
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
    const size_t num_threads = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 64);
    // Builds a driver crash ended an earlier precache on are skipped (precache_journal.h). Cached
    // graphics pipelines and compute programs are named by their hashes, which don't depend on a
    // file: the journal belongs to the driver and the pipeline cache version.
    PrecacheJournal journal{
        ShaderWarm::ComputeWarmPath(title_id).parent_path() / "precache-jobs",
        fmt::format("{} {} {}", device.GetDriverVersion(), device.GetDeviceID(), CACHE_VERSION),
        num_threads, "pipeline"};
    std::vector<u8> built(jobs.size());
    std::atomic<size_t> next{};
    std::atomic<size_t> done{};
    std::atomic<bool> cancel{false};
    bool skipped = false;
    LOG_INFO(Render_Vulkan,
             "Shader precache: {} cached graphics pipelines, {} of {} compute programs and {} "
             "precompiled pipelines to build on {} threads",
             graphics_jobs, precompiled_first - graphics_jobs, file_entries,
             jobs.size() - precompiled_first, num_threads);
    if (!jobs.empty()) {
        std::vector<std::jthread> threads;
        for (size_t thread = 0; thread < num_threads; ++thread) {
            threads.emplace_back([&jobs, &built, &next, &done, &cancel, &journal, thread] {
                Common::SetCurrentThreadName("VkShaderPrecache");
                while (!cancel.load(std::memory_order_relaxed)) {
                    const size_t index = next.fetch_add(1);
                    if (index >= jobs.size()) {
                        break;
                    }
                    const u64 id = jobs[index].graphics_hash != 0 ? jobs[index].graphics_hash
                                                                  : jobs[index].compute_hash;
                    if (!journal.Poisoned(id)) {
                        journal.Begin(thread, id);
                        try {
                            // Built and destroyed here: only the driver's cache keeps it.
                            built[index] = jobs[index].build();
                        } catch (const std::exception& e) {
                            LOG_DEBUG(Render_Vulkan, "Shader precache: {}", e.what());
                        }
                        journal.End(thread);
                    }
                    jobs[index].build = {};
                    done.fetch_add(1);
                }
            });
        }
        while (done < jobs.size()) {
            if (stop_loading.stop_requested() || ShaderWarm::TakeSkipWarm()) {
                // Every thread stops after its current build; the scope end joins them.
                cancel = true;
                skipped = true;
                break;
            }
            if (callback) {
                callback(VideoCore::LoadCallbackStage::Warm, done, jobs.size());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
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
        LOG_INFO(Render_Vulkan,
                 "Precompiled pipelines: {} of {} prepared ({} already in the disk cache), {} not "
                 "buildable on this device{}{}{}, {} failed",
                 precompiled_built, total,
                 precompiled_counts ? precompiled_counts->known.load() : 0, unbuildable_total,
                 unbuildable.empty() ? "" : " (", unbuildable, unbuildable.empty() ? "" : ")",
                 total - precompiled_built - unbuildable_total);
        std::ofstream stamp_file(precompiled->first, std::ios::binary | std::ios::trunc);
        stamp_file << precompiled->second;
    }
    const size_t precached_before = precached.size();
    for (size_t index = 0; index < graphics_jobs; ++index) {
        if (built[index]) {
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
    LOG_INFO(Render_Vulkan, "Shader precache: {}, {} built, {} failed, {} not reached, {:.1f} s",
             stop_loading.stop_requested() ? "stopped" : (skipped ? "skipped" : "finished"),
             total_built, done - total_built, jobs.size() - std::min(jobs.size(), done.load()),
             std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count());
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
    const auto path =
        Precompile::EnsurePrecompiledFile(title_id, base_dir, CACHE_VERSION, stop_loading);
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
                    Common::g_build_version, device.GetModelName());
    if (!Precompile::Forced()) {
        std::ifstream stamp_file(stamp_path, std::ios::binary);
        std::string line;
        if (std::getline(stamp_file, line) && line + '\n' == stamp) {
            LOG_INFO(Render_Vulkan,
                     "Precompiled pipelines: {} was prepared for this driver and build already",
                     path_text);
            return std::nullopt;
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
    precompiled_counts = counts;
    for (size_t record = 0; record < record_offsets.size(); ++record) {
        const u64 offset = record_offsets[record];
        const auto [record_id, record_compute] = record_ids[record];
        jobs.push_back({record_compute ? 0 : record_id,
                        [this, path, env_index, known, counts, offset]() -> bool {
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
                                in.read(reinterpret_cast<char*>(&compute_key),
                                        sizeof(compute_key));
                            } else {
                                in.read(reinterpret_cast<char*>(&record_key),
                                        sizeof(record_key));
                            }
                            std::vector<FileEnvironment> envs(count);
                            for (size_t i = 0; i < count; ++i) {
                                const auto [env_offset, env_size] = (*env_index)[ids[i]];
                                in.clear();
                                in.seekg(static_cast<std::streamoff>(env_offset));
                                envs[i].Deserialize(
                                    in, static_cast<std::streamoff>(env_offset + env_size));
                            }
                            ShaderPools pools;
                            if (compute) {
                                // Built here, not queued: the pipeline is dropped at once.
                                return CreateComputePipeline(pools, compute_key, envs.front(),
                                                             nullptr, false, false)
                                           .pipeline != nullptr;
                            }
                            if (size_t reason = 0;
                                !RecordedKeyForThisDevice(record_key, envs, &reason)) {
                                ++counts->unbuildable[reason];
                                return false;
                            }
                            if (known->contains(record_key.Hash())) {
                                ++counts->known;
                                return true;
                            }
                            boost::container::static_vector<Shader::Environment*,
                                                            Maxwell::MaxShaderProgram>
                                env_ptrs;
                            for (auto& env : envs) {
                                env_ptrs.push_back(&env);
                            }
                            const auto pipeline = CreateGraphicsPipeline(
                                pools, record_key, MakeSpan(env_ptrs), nullptr, false);
                            return pipeline != nullptr && !pipeline->Failed();
                        },
                        record_compute ? record_id : 0});
    }
    LOG_INFO(Render_Vulkan, "Precompiled pipelines: {} records of {} to prepare",
             record_offsets.size(), path_text);
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
    const auto [pair, is_new]{graphics_cache.try_emplace(graphics_key)};
    auto& pipeline{pair->second};
    if (is_new) {
        ReportNewPipelineKey(graphics_key);
        pipeline = background_pipelines.Take(graphics_key);
        if (!pipeline) {
            // EDEN_NX2_ASYNC_TRANSLATE=1/2: a draw that may be skipped has its new pipeline
            // translated on a translation thread, and is skipped (as for an unbuilt pipeline)
            // until it is ready. The entry stays empty meanwhile.
            if (translate_queue && EnqueueTranslation()) {
                NoteTranslateSkip();
                return nullptr;
            }
            pipeline = CreateGraphicsPipeline();
        }
    } else if (!pipeline && translate_queue) {
        bool pending{};
        pipeline = AdoptTranslation(pending);
        if (pending) {
            return nullptr;
        }
    }
    if (!pipeline) {
        return nullptr;
    }
    constexpr auto save_interval = std::chrono::seconds{5};
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
    std::unique_ptr<EmissionBudget::Reservation> reservation, TranslationRecord* record) try {
    auto hash = key.Hash();
    LOG_DEBUG(Render_Vulkan, "{:#016x}", hash);
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
        const u32 cfg_offset{static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader))};
        Shader::Maxwell::Flow::CFG cfg(env, pools.flow_block, cfg_offset, index == 0);
        if (!uses_vertex_a || index != 1) {
            // Normal path
            programs[index] = TranslateProgram(pools.inst, pools.block, env, cfg, host_info);
        } else {
            // VertexB path when VertexA is present.
            auto& program_va{programs[0]};
            auto program_vb{TranslateProgram(pools.inst, pools.block, env, cfg, host_info)};
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
            if (emit_pools) {
                // The workers build the module from these words (DeferredStages).
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
        if (emit_pools) {
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
    if (emit_pools) {
        auto job = std::make_shared<DeferredStages>();
        job->reservation = std::move(reservation);
        job->pools = emit_pools;
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
            job.reset();
            completed_pools->ReleaseContents();
            {
                std::scoped_lock lock{spare_pools_mutex};
                if (spare_pools.size() < 4) {
                    spare_pools.push_back(std::move(completed_pools));
                }
            }
            AddRuntimeSpirvTime(1, begin);
            return stage_modules;
        };
    }
    Common::ThreadWorker* const thread_worker{build_in_parallel ? &workers : nullptr};
    return std::make_unique<GraphicsPipeline>(
        scheduler, buffer_cache, texture_cache, vulkan_pipeline_cache, &shader_notify, device,
        descriptor_pool, guest_descriptor_queue, descriptor_buffer_ring, thread_worker, statistics,
        render_pass_cache, key, std::move(modules), infos, std::move(emit_stages),
        std::move(mesh_emulation));

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
                           const std::string& shaders, const RuntimeBuildSplit& split = {}) {
    static std::chrono::steady_clock::duration total{};
    static RuntimeBuildSplit split_total{};
    static u32 count{}, logged{};
    static auto window = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - begin;
    total += elapsed;
    split_total.environments += split.environments;
    split_total.translation += split.translation;
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
        // The split goes at the end, so readers of the older line keep their fields.
        LOG_INFO(Render_Vulkan,
                 "Runtime pipelines in the last 10 s: {} built, {:.1f} ms on the GPU thread "
                 "({:.1f} ms of it graphics SPIR-V), {:.1f} ms graphics SPIR-V on workers, "
                 "{:.1f} ms of draws held for unbuilt pipelines; graphics on the GPU thread: "
                 "{:.1f} ms environments, {:.1f} ms translation",
                 count, ms(total), ms(take(runtime_spirv_ns[0])), ms(take(runtime_spirv_ns[1])),
                 ms(GraphicsPipeline::TakeHoldTime()), ms(split_total.environments),
                 ms(split_total.translation));
        total = {};
        split_total = {};
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

std::unique_ptr<GraphicsPipeline> PipelineCache::CreateGraphicsPipeline() {
    const auto begin = std::chrono::steady_clock::now();
    t_runtime_build = true;
    t_build_split = {};
    SCOPE_EXIT {
        t_runtime_build = false;
        ReportRuntimePipeline("Graphics", begin, PipelineShaders(graphics_key), t_build_split);
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

    std::shared_ptr<ShaderPools> emit_pools;
    auto reservation = emission_budget.TryAcquire(maxwell3d->GetNativeMacroEngine().native_turing);
    if (reservation) {
        {
            std::scoped_lock lock{spare_pools_mutex};
            if (!spare_pools.empty()) {
                emit_pools = std::move(spare_pools.back());
                spare_pools.pop_back();
            }
        }
        if (!emit_pools) {
            emit_pools = std::make_shared<ShaderPools>();
        }
    } else {
        main_pools.ReleaseContents();
    }
    ShaderPools& pools = emit_pools ? *emit_pools : main_pools;
    auto pipeline{CreateGraphicsPipeline(pools, graphics_key, environments.Span(), nullptr, true,
                                         std::move(emit_pools), std::move(reservation))};
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
        SerializePipeline(key, env_ptrs, pipeline_cache_filename, CACHE_VERSION,
                          host_info.EnvironmentReadProperties());
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
    // As the GPU thread's build: SPIR-V emission and the driver compile on the pipeline workers
    // when the emission budget has room, otherwise the SPIR-V here.
    std::shared_ptr<ShaderPools> emit_pools;
    auto reservation = emission_budget.TryAcquire(job.native);
    if (reservation) {
        {
            std::scoped_lock lock{spare_pools_mutex};
            if (!spare_pools.empty()) {
                emit_pools = std::move(spare_pools.back());
                spare_pools.pop_back();
            }
        }
        if (!emit_pools) {
            emit_pools = std::make_shared<ShaderPools>();
        }
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
                                          std::move(emit_pools), std::move(reservation),
                                          job.record.get());
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

    main_pools.ReleaseContents();
    auto entry{CreateComputePipeline(main_pools, key, env, nullptr, true)};
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
        SerializePipeline(key, std::array<const GenericEnvironment*, 1>{&env_},
                          pipeline_cache_filename, CACHE_VERSION,
                          host_info.EnvironmentReadProperties());
    });
    QueueVulkanPipelineCacheFlush();
    return entry;
}

ComputePipelineCacheEntry PipelineCache::CreateComputePipeline(
    ShaderPools& pools, const ComputePipelineCacheKey& key, Shader::Environment& env,
    PipelineStatistics* statistics, bool build_in_parallel, bool may_defer) try {
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
    std::vector<u32> code{EmitSPIRV(profile, program)};
    SelfCheckSpirv(code, key.unique_hash, "compute");
    device.SaveShader(code);
    vk::ShaderModule spv_module{BuildShader(device, code)};

    // Text log + .spv dump. Same split as the graphics path.
    const bool should_log = GPU::Logging::IsActive();
    const bool should_dump = Settings::values.gpu_log_shader_dumps.GetValue();
    if (should_log || should_dump) {
        const std::string shader_name = fmt::format("shader_{:016x}_compute", key.unique_hash);
        if (should_log) {
            const std::string shader_info = fmt::format("SPIR-V size: {} bytes, hash: {:016x}",
                code.size() * sizeof(u32), key.unique_hash);
            GPU::Logging::GPULogger::GetInstance().LogShaderCompilation(shader_name, shader_info);
        }
        if (should_dump) {
            GPU::Logging::DumpSpirvShader(key.unique_hash,
                                          std::span<const u32>(code.data(), code.size()));
        }
    }

    if (device.HasDebuggingToolAttached()) {
        const auto name{fmt::format("Shader {:016x}", key.unique_hash)};
        spv_module.SetObjectNameEXT(name.c_str());
    }
    const bool deferred = may_defer && program.info.uses_software_matrix &&
                          device.DefersSoftwareMatrixPipelines();
    Common::ThreadWorker* const thread_worker{deferred            ? &deferred_workers
                                              : build_in_parallel ? &workers
                                                                  : nullptr};
    const Shader::Info info{std::move(program.info)};
    if (!thread_worker) {
        // The driver compiles on this thread. Precache and boot workers would otherwise hold their
        // IR through the compile, up to hundreds of MiB for one large shader.
        program = {};
        code = std::vector<u32>{};
        pools.block.Free();
        pools.inst.Free();
    }
    return {.pipeline = std::make_unique<ComputePipeline>(
                device, scheduler, vulkan_pipeline_cache, descriptor_pool, guest_descriptor_queue,
                descriptor_buffer_ring, thread_worker, statistics, &shader_notify, info,
                std::move(spv_module), key.unique_hash, deferred),
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
