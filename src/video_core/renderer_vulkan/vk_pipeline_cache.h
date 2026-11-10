// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include "common/container/unordered_map.h"
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/async_draw_policy.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_prebuild.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/shader_cache.h"
#include "video_core/shader_cache_v2/cache.h"
#include "video_core/shader_jobs.h"
#include "video_core/shader_stage_translation.h"

namespace Core {
class System;
}

namespace Shader::IR {
struct Program;
}

namespace VideoCore {
class ShaderNotify;
}

namespace VideoCommon::ShaderCacheV2 {
struct LogFile;
struct ProgramRecord;
} // namespace VideoCommon::ShaderCacheV2

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;

struct ComputePipelineCacheKey {
    u64 unique_hash;
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;

    size_t Hash() const noexcept;

    bool operator==(const ComputePipelineCacheKey& rhs) const noexcept;

    bool operator!=(const ComputePipelineCacheKey& rhs) const noexcept {
        return !operator==(rhs);
    }
};
static_assert(std::has_unique_object_representations_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_copyable_v<ComputePipelineCacheKey>);
static_assert(std::is_trivially_constructible_v<ComputePipelineCacheKey>);

} // namespace Vulkan

namespace std {

template <>
struct hash<Vulkan::ComputePipelineCacheKey> {
    size_t operator()(const Vulkan::ComputePipelineCacheKey& k) const noexcept {
        return k.Hash();
    }
};

} // namespace std

namespace Vulkan {

class ComputePipeline;
class DescriptorPool;
class Device;
class PipelineStatistics;
class RenderPassCache;
class Scheduler;
class TranslateQueue;
struct TranslationJob;
struct TranslationRecord;

using VideoCommon::ShaderInfo;

using VideoCommon::ShaderPools;

struct ComputePipelineCacheEntry {
    std::unique_ptr<ComputePipeline> pipeline;
    std::string failure;
    /// Constant buffer slots that were null pointers at translation (see NullStoragePointerSlots)
    std::vector<std::pair<u32, u32>> null_pointer_slots;
    /// Constant buffer slots bound as storage buffer pointers (see MappedStoragePointerKeys)
    std::vector<u64> mapped_pointers;
    u32 rebuilds{};
    /// Rebuild still compiling: dispatches keep using this entry's pipeline until it is ready
    std::unique_ptr<ComputePipelineCacheEntry> pending;
    /// Whether a dispatch looked it up yet (kept only with EDEN_NX2_STUTTER_TRACE)
    bool used{};

    [[nodiscard]] ComputePipeline* Get(bool native, GPUVAddr address,
                                       const ComputePipelineCacheKey& key) const;
};

/// EDEN_NX2_ASYNC_DISCARD, read on first use: what a native draw does while its pipeline is
/// missing. 0: the native_async_draws setting decides (Wait by default). 1: skipped by the legacy
/// heuristic. 2: skipped only into render targets the frame clears and redraws, the rest wait.
/// Unset: 2 while the setting is Wait, its default; 0 when the setting was changed. Non-native
/// draws keep the legacy heuristic in every mode.
[[nodiscard]] int AsyncDiscardMode();

class PipelineCache : public VideoCommon::ShaderCache {
public:
    explicit PipelineCache(Tegra::MaxwellDeviceMemoryManager& device_memory_, const Device& device,
                           Scheduler& scheduler, DescriptorPool& descriptor_pool,
                           GuestDescriptorQueue& guest_descriptor_queue,
                           DescriptorBufferRing& descriptor_buffer_ring,
                           RenderPassCache& render_pass_cache, BufferCache& buffer_cache,
                           TextureCache& texture_cache, VideoCore::ShaderNotify& shader_notify_);
    ~PipelineCache();

    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipeline();

    /// EDEN_NX2_ASYNC_TRANSLATE: joins the translation threads and retires what they made. For
    /// the emulation's shutdown once the GPU threads stopped (GPU::NotifyShutdown), before the
    /// guest's GPU address spaces the translations read are destroyed.
    void StopTranslations();

    /// Whether the last CurrentGraphicsPipeline returned null because its draw was skipped for an
    /// unbuilt pipeline (asynchronous shaders), not because there is no pipeline to build.
    [[nodiscard]] bool LastLookupSkipped() const noexcept {
        return discard_decision.serial == lookup_serial && discard_decision.skip;
    }

    void TickFrame();

    [[nodiscard]] ComputePipeline* CurrentComputePipeline(u64& native_key);

    void LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                           const VideoCore::DiskResourceLoadCallback& callback);

private:
    [[nodiscard]] GraphicsPipeline* CurrentGraphicsPipelineSlowPath();

    /// Returns `pipeline` when it is built or its draw must wait for it, nullptr when the draw
    /// may be skipped while it builds (asynchronous shaders, CanDiscardPendingDraw).
    [[nodiscard]] GraphicsPipeline* BuiltPipeline(GraphicsPipeline* pipeline) noexcept;

    /// What the current draw does while its pipeline is not ready (EDEN_NX2_ASYNC_DISCARD, the
    /// native_async_draws setting, the legacy heuristic; full-screen passes always wait),
    /// decided once per lookup however often the lookup asks.
    [[nodiscard]] NativeDrawAction PendingDrawAction();
    /// Counts CurrentGraphicsPipeline calls, for the once-per-lookup decision.
    u64 lookup_serial{};
    struct DiscardDecision {
        u64 serial{~u64{0}};
        NativeDrawAction action{};
        bool skip{}; ///< The lookup's draw was skipped
        VideoCommon::DrawTargets targets{};
    };
    DiscardDecision discard_decision;
    /// Counts one decision for the 10 s line, and lists a persistent target the first time a
    /// draw into it misses its pipeline. GPU thread.
    void NoteDiscardDecision(bool skip, const VideoCommon::DrawTargets& targets);
    /// TickFrame: the EDEN_NX2_ASYNC_DISCARD counts of the last 10 s.
    void ReportAsyncDiscard();
    /// EDEN_NX2_ASYNC_DISCARD counts, GPU thread only. Lookups by class of their targets (index:
    /// VideoCommon::DrawTargetClass), a lookup that repeats the one before (same pipeline key and
    /// target, same frame: the rasterizer's second lookups) counted once.
    struct DiscardStats {
        std::array<u32, 3> skipped{};
        std::array<u32, 3> waited{};
        std::chrono::nanoseconds held_at_window{}; ///< GraphicsPipeline::HoldTimeTotal then
        std::chrono::steady_clock::time_point window{};
        // The last lookup counted.
        GraphicsPipelineCacheKey last_key{};
        u64 last_frame{~u64{0}};
        GPUVAddr last_target{};
        VideoCommon::DrawTargetClass last_kind{};
        bool last_skip{};
    };
    DiscardStats discard_stats;
    /// Persistent targets listed so far (32 at most per session).
    std::unordered_set<GPUVAddr> listed_persistent_targets;

    /// With `keep_pointers`, the slots an evicted pipeline of this key bound stay storage buffer
    /// pointers whatever they hold now.
    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        const VideoCommon::EvictedPointerSlots::Slots* keep_pointers = nullptr);

    /// Whether the current draw may be skipped while its pipeline is not ready (PendingDrawAction
    /// does not wait for it).
    [[nodiscard]] bool DrawMayBeSkipped();

    /// Stores a graphics pipeline built at run time in the disk cache (unless it is there), with
    /// the environments it was translated from. GPU thread.
    void StoreRuntimeGraphicsPipeline(
        const GraphicsPipelineCacheKey& key,
        const std::array<VideoCommon::GraphicsEnvironment, Maxwell::MaxShaderProgram>& envs);

    /// EDEN_NX2_ASYNC_TRANSLATE=1/2 (translate_queue set). Queues the current key's translation
    /// from snapshot environments when the draw may be skipped; false when it is to be built on
    /// the GPU thread instead.
    bool EnqueueTranslation();
    /// A later lookup of a key whose entry is empty: its translated pipeline when ready; `pending`
    /// when the draw is to be skipped meanwhile. Waits (capped) or builds here for a draw that
    /// must not be skipped.
    std::unique_ptr<GraphicsPipeline> AdoptTranslation(bool& pending);
    /// What a finished job gives: its pipeline once its texture reads check out on the GPU
    /// thread. `retry` when it was not usable (to be translated again, or built here).
    std::unique_ptr<GraphicsPipeline> AdoptJob(std::unique_ptr<TranslationJob> job, bool& retry);
    /// The translation itself, on a translation thread.
    void TranslateOnWorker(TranslationJob& job);
    /// Between frames: adopts finished jobs whose draws did not come back, sweeps wedged ones
    /// and logs the counts every 10 s.
    void TickTranslations();
    /// A lookup skipped while its pipeline translates: counted as a skipped lookup.
    void NoteTranslateSkip();

    /// FixedPipelineState::Refresh of graphics_key, keeping only the vertex attribute types the
    /// bound programs read (VideoCommon::KeyedAttributeTypes).
    void RefreshGraphicsKey();

    /// Logs a kept pointer slot (an index of keep_pointers) that this translation would otherwise
    /// have classified as no pointer: null, unmapped, or out of range right now.
    void WarnKeptPointer(size_t program, u64 slot) const;

    /// Shader cache v2, run-time misses: the stages translated at once on shader_jobs' CPU workers
    /// while the GPU thread waited (VideoCommon::TranslateStages)
    struct ParallelTranslation {
        std::chrono::nanoseconds wait{};   ///< On the GPU thread
        std::chrono::nanoseconds stages{}; ///< The stages' translations together
        size_t count{};
    };

    /// Builds the pipeline as a job of shader_jobs with build_in_parallel, else leaves it for the
    /// caller's Build() (see BuildQueue). With `parallel` the stages translate at once on
    /// shader_jobs, each in IR pools of its own; their environments must be safe to read from those
    /// threads.
    std::unique_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        ShaderPools& pools, const GraphicsPipelineCacheKey& key,
        std::span<Shader::Environment* const> envs, PipelineStatistics* statistics,
        bool build_in_parallel, std::shared_ptr<ShaderPools> emit_pools = nullptr,
        ParallelTranslation* parallel = nullptr, TranslationRecord* record = nullptr);

    /// Released IR pools for a build, or new ones
    [[nodiscard]] std::shared_ptr<ShaderPools> TakeSparePools();
    /// Releases the pools' contents and keeps a few for later builds
    void ReturnPools(std::shared_ptr<ShaderPools> pools);

    [[nodiscard]] bool StaleNullPointerSlot(const ComputePipelineCacheEntry& entry) const;

    /// Translates the current compute program, binding the pointers in keep_pointers whatever
    /// their slots hold now (see GenericEnvironment::KeepStoragePointers)
    ComputePipelineCacheEntry CreateComputePipeline(const ComputePipelineCacheKey& key,
                                                    const ShaderInfo* shader,
                                                    std::span<const u64> keep_pointers = {});

    /// Builds the pipeline as CreateGraphicsPipeline does. With `emit_pools`, which `pools` must
    /// be, the SPIR-V is emitted by the CPU stage of the pipeline's job, which takes the pools and
    /// the program.
    /// `may_defer`: a software HMMA program is built by a Background job nothing waits for on
    /// drivers that take minutes on it (Device::DefersSoftwareMatrixPipelines), its dispatches
    /// skipped meanwhile. False builds it as any other, for the precache jobs.
    ComputePipelineCacheEntry CreateComputePipeline(ShaderPools& pools,
                                                     const ComputePipelineCacheKey& key,
                                                     Shader::Environment& env,
                                                     PipelineStatistics* statistics,
                                                     bool build_in_parallel,
                                                     std::shared_ptr<ShaderPools> emit_pools = nullptr,
                                                     bool may_defer = true);

    /// Where CreateGraphicsPipeline and CreateComputePipeline build; `cpu_bytes` estimates the
    /// IR a deferred emission holds
    [[nodiscard]] PipelineBuildQueue BuildQueue(bool build_in_parallel, u64 cpu_bytes = 0);

    /// Whether the device can build a cached graphics pipeline of this key: its dynamic state
    /// and provoking vertex must match the device's
    [[nodiscard]] bool KeyFitsDevice(const GraphicsPipelineCacheKey& key) const;

    /// The boot load of the v2 cache (LoadDiskResources): `log` read, its records built as jobs
    /// of shader_jobs
    void LoadPipelinesV2(u64 title_id, std::shared_ptr<const VideoCommon::ShaderCacheV2::LogFile> log,
                         std::stop_token stop_loading,
                         const VideoCore::DiskResourceLoadCallback& callback,
                         std::chrono::steady_clock::time_point load_begin, bool precache,
                         std::unordered_set<u64> precached_pipelines);

    /// Reads a v2 graphics record and translates its pipeline, left for Build(). Null when it
    /// doesn't translate, `incomplete` when the record lacks a value the translation reads.
    /// `key` comes back keyed as draws key the record (KeyAsDraws); a key `admit` refuses is not
    /// translated. Throws std::ios_base::failure for a record it can't read.
    std::unique_ptr<GraphicsPipeline> TranslateCachedGraphics(
        std::span<const u8> payload, GraphicsPipelineCacheKey& key,
        PipelineStatistics* statistics, bool& incomplete,
        const std::function<bool(const GraphicsPipelineCacheKey&)>& admit = {});

    /// Cancels the v2 prebuild and waits for its running stages
    void StopBackgroundJobs();

    void SerializeVulkanPipelineCache(const std::filesystem::path& filename,
                                      const vk::PipelineCache& pipeline_cache, u32 cache_version);

    vk::PipelineCache LoadVulkanPipelineCache(const std::filesystem::path& filename,
                                              u32 expected_cache_version);

    void QueueVulkanPipelineCacheFlush();

    /// Writes the driver pipeline cache on the serialization thread, unless a write is queued
    /// already (`again_if_busy`: then one more follows it, for a save that must hold the latest
    /// builds, such as the end of a precache). Callable from any thread.
    void QueueVulkanPipelineCacheSave(bool again_if_busy);
    /// True (and `last` set to now) when a write of the driver cache is due since `last`.
    [[nodiscard]] bool VulkanPipelineCacheSaveDue(
        std::chrono::steady_clock::time_point& last) const;

    /// One build of the shader precache: a pipeline built and destroyed
    struct PrecacheJob {
        u64 graphics_hash{}; ///< The cached graphics pipeline it builds, 0 for a compute program
        /// Translates the pipeline and returns its build, which is true when it built (the job's
        /// CPU and driver stages)
        Common::UniqueFunction<Common::UniqueFunction<bool>> translate;
        u64 cpu_bytes{};
        u64 compute_hash{}; ///< The compute program it builds (its unique hash), else 0
    };

    /// Shader precache before the game starts (precache_shaders), on every CPU thread: the cached
    /// graphics pipelines this boot skipped (`jobs`; `precached` holds those built by earlier
    /// precaches) and the compute programs of the title's precache file (made from the game files,
    /// missing reads guessed) are built, then thrown away. They only fill the driver's shader
    /// cache, so a later build of the same pipeline finds the driver's work done. Reports progress
    /// through `callback` and returns when it is done, skipped from the loading screen or the load
    /// is stopped.
    void PrecacheShaders(u64 title_id, std::vector<PrecacheJob> jobs,
                         std::unordered_set<u64> precached, std::stop_token stop_loading,
                         const VideoCore::DiskResourceLoadCallback& callback);

    /// Fritchii's precompiled pipelines (zortos293/Lichii#895, VideoCommon::Nx2Precompile): the
    /// records of the title's precompiled file, assembled from its recipe and the game's own files,
    /// as precache jobs. Returns the stamp file and the stamp PrecacheShaders writes once every
    /// one of them ran; nothing when there is nothing to prepare.
    std::optional<std::pair<std::filesystem::path, std::string>> AddPrecompiledJobs(
        u64 title_id, std::vector<PrecacheJob>& jobs, std::stop_token stop_loading);
    /// A precompiled record's key as this device keys it (Fritchii's RecordedKeyForThisDevice:
    /// this driver, and a dynamic logic op recorded with the logic op off made static where it
    /// is). False when it was made for a device with other dynamic states; `reason` is then the
    /// index of the first one in PrecompiledCounts::unbuildable.
    [[nodiscard]] bool RecordedKeyForThisDevice(GraphicsPipelineCacheKey& key,
                                                std::span<VideoCommon::FileEnvironment> envs,
                                                size_t* reason = nullptr) const;
    /// What the last precompiled preparation's records came to, for its log line (as Fritchii
    /// v5.3 logs its skips, 1fa49c5c3).
    struct PrecompiledCounts {
        static constexpr std::array<const char*, 8> reasons{
            "EDS1",          "EDS2",         "EDS2 logic op",        "EDS3 blend",
            "EDS3 enables",  "color write enable", "dynamic vertex input", "provoking vertex",
        };
        std::atomic<size_t> known{}; ///< Graphics pipelines the disk cache held (built at boot)
        std::array<std::atomic<size_t>, reasons.size()> unbuildable{};
        size_t resumed{}; ///< Records an earlier, unfinished preparation got through
        std::filesystem::path progress_path;
        std::string stamp;
    };
    std::shared_ptr<PrecompiledCounts> precompiled_counts;

    const Device& device;
    Scheduler& scheduler;
    DescriptorPool& descriptor_pool;
    GuestDescriptorQueue& guest_descriptor_queue;
    DescriptorBufferRing& descriptor_buffer_ring;
    RenderPassCache& render_pass_cache;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    VideoCore::ShaderNotify& shader_notify;
    bool use_asynchronous_shaders{};
    bool use_vulkan_pipeline_cache{};
    /// The pipelines' builds, the prebuild and the precache. Declared before the pipelines, whose
    /// builds it runs.
    std::optional<VideoCommon::ShaderJobScheduler> shader_jobs;
    /// IR of the pipelines whose job emits their SPIR-V
    QueuedIrBudget queued_ir{VideoCommon::DefaultShaderJobBudget()};
    std::optional<ScheduledPrebuild<GraphicsPipelineCacheKey, GraphicsPipeline>> scheduled_prebuild;

    GraphicsPipelineCacheKey graphics_key{};
    u64 vertex_attribute_types{}; ///< graphics_key's attribute types before the programs' mask
    GraphicsPipeline* current_pipeline{};

    /// Destroys graphics pipelines unused for a while, once the GPU is done with them.
    void EvictUnusedPipelines();
    u64 frame_count{};
    /// Wait budget of native draws for unbuilt pipelines left in this frame, in microseconds
    /// (native_async_draws = Bounded). Only touched on the GPU thread; reset in TickFrame.
    s64 native_wait_budget_us{8000};
    s64 native_wait_used_us{}; ///< Budget spent since the last 10 s log line.
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> evicted_pipelines; ///< (tick, pipeline)
    /// Pipelines the translate queue did not use (discarded, stale, abandoned, rejected by the
    /// verify mode), handed over on the GPU thread: destroyed by TickFrame once the GPU and their
    /// queued build are done with them, never on a translation thread (where the destructor
    /// would wait for the driver compile, or run it).
    std::deque<std::pair<u64, std::unique_ptr<GraphicsPipeline>>> retired_translations; ///< (tick, pipeline)
    void RetireTranslatedPipeline(std::unique_ptr<GraphicsPipeline> pipeline);
    std::unordered_set<u64> used_and_evicted; ///< Key hashes of pipelines used this session, evicted since
    VideoCommon::EvictedPointerSlots evicted_pointer_slots;

    ::Common::unordered_map<ComputePipelineCacheKey, ComputePipelineCacheEntry> compute_cache;
    /// Replaced compute pipelines, destroyed once the GPU and their queued build are done with
    /// them (tick, pipeline)
    std::deque<std::pair<u64, std::unique_ptr<ComputePipeline>>> retired_compute_pipelines;
    ::Common::unordered_map<GraphicsPipelineCacheKey, std::unique_ptr<GraphicsPipeline>> graphics_cache;

    ShaderPools main_pools;
    /// EDEN_NX2_ASYNC_TRANSLATE=2: the GPU thread's reference translation at enqueue uses these
    /// rather than releasing main_pools. Made on first use, only in that mode.
    std::unique_ptr<ShaderPools> verify_pools;
    std::mutex spare_pools_mutex;
    std::vector<std::shared_ptr<ShaderPools>> spare_pools;

    Shader::Profile profile;
    Shader::HostTranslateInfo host_info;

    std::filesystem::path pipeline_cache_filename;

    /// Key hashes of the graphics pipelines in the disk cache. Only those a recent session used
    /// (used_pipelines) are built at boot; the rest are built when first needed, and not stored
    /// again. Each built pipeline holds driver memory (title 1: 6.5k cached, a quarter used).
    std::unordered_set<u64> disk_pipelines;
    /// Disk cache records the translation rejected since the last report: the first few are
    /// logged, the rest counted (ReportDiskRecordFailures, after the boot build, the precache
    /// and, for the background prebuild, at exit).
    std::atomic<size_t> disk_record_failures{};
    void NoteDiskRecordFailure(std::string_view kind, u64 hash, const char* reason);
    void ReportDiskRecordFailures();
    std::unordered_map<u64, u8> used_pipelines; ///< Key hash -> sessions since it was last used
    std::unordered_set<const GraphicsPipeline*> used_this_session;
    std::chrono::steady_clock::time_point used_pipelines_saved{};
    void SaveUsedPipelines();

    /// The cache pipeline_cache_filename names, and this session's Used record
    std::unique_ptr<VideoCommon::ShaderCacheV2::Cache> cache_v2;
    /// Its driver layer, where the device has VK_KHR_pipeline_binary
    std::unique_ptr<PipelineBinaries> pipeline_binaries;
    u64 used_session_key{};
    u64 used_session_time{};

    std::filesystem::path vulkan_pipeline_cache_filename;
    vk::PipelineCache vulkan_pipeline_cache;
    size_t pipelines_since_flush{};
    std::chrono::steady_clock::time_point last_flush{};
    std::atomic<size_t> last_cache_size{};
    std::atomic_bool flush_in_flight{};
    std::atomic_bool save_again{};
    /// When the background prebuild last queued a driver cache write.
    std::mutex background_save_mutex;
    std::chrono::steady_clock::time_point background_last_save{};

    /// Requested first when the cache is destroyed, to stop a rewrite of the disk cache: Android
    /// and yuzu_cmd load with a stop token that never stops
    std::stop_source serialization_stop;
    Common::ThreadWorker serialization_thread;
    DynamicFeatures dynamic_features;
    /// Cancels the background prebuild when the load's stop token is requested (emulation stops).
    /// Reset first in the destructor: it uses shader_jobs.
    std::optional<std::stop_callback<std::function<void()>>> loading_stop_callback;

    /// EDEN_NX2_ASYNC_TRANSLATE=1/2: graphics pipelines translated off the GPU thread. Null with
    /// the switch off. Reset first in the destructor (its threads build with the members above).
    std::unique_ptr<TranslateQueue> translate_queue;
    /// Keys whose translations kept failing their checks or were swept: built on the GPU thread.
    std::unordered_set<GraphicsPipelineCacheKey> translate_sync_only;
    /// Unusable translations per key (a stale texture descriptor, a guarded read that gave up).
    std::unordered_map<GraphicsPipelineCacheKey, u32> translate_failures;
    /// Counts for the 10 s line, GPU thread only.
    struct TranslateStats {
        u32 queued{};
        u32 full{}; ///< Not queued: too many in flight
        u32 adopted{};
        u32 adopted_between_frames{};
        u32 no_pipeline{};
        u32 stale{};   ///< A texture descriptor changed before adoption
        u32 gave_up{}; ///< A guarded read gave up
        u32 differs{}; ///< Verify mode: not what the GPU thread made
        u32 requeued{};
        u32 waits{};
        u32 fallbacks{}; ///< Built on the GPU thread after all
        u32 swept{};
        u64 skipped{}; ///< Lookups skipped while translating
        size_t snapshot_bytes{};
        std::chrono::steady_clock::duration enqueue_time{};
        std::chrono::steady_clock::duration wait_time{};
        std::chrono::steady_clock::duration translation_time{};
        std::chrono::steady_clock::duration ready_total{};
        std::chrono::steady_clock::duration ready_max{};
        std::chrono::steady_clock::time_point window{};
    } translate_stats;
};

} // namespace Vulkan
