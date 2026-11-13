# Upstream merge plan: nx2 `65745c243f` into tuned4 (merge 3, step 1)

Planned 2026-11-13 (round 22). Step 1 of the two the owner set on 2026-11-13: `65745c243f` now,
build 171 (`c169856090`) on top in step 2. Workflow `wf_13890ad5-04e`: three surveys
(`scratchpad/merge3/survey-gpu.md`, `survey-shaders.md`, `survey-present.md`) and this plan; the
main thread checked the coverage, the checkpoint greps and the corrections below against the trees.
Line numbers: `wt:` = the trial merge in `scratchpad/wt-merge3` with its conflict markers,
`ours:` = tuned4 `5fc428bdc`, `up:` = `65745c243f`. If tuned4 moves before the merge (r59), re-run
the trial and re-map the `wt:` lines.

## 1. Target and scope

- Base `2eb5f081c` (upstream build 160, merge 2). Target `65745c243f`
  (`refs/remotes/zortos/merge3-target`), upstream's tip when the owner said build 169 was out, so
  build 169 is it or an older commit: 60 commits, 289 files, +18597/-1371.
- Ours: tuned4 `5fc428bdc` (r58): 468 commits (15 merges) past the base, 384 files, +40548/-4615.
- Trial (branch `r22-merge-nx2c` at `5fc428bdc`, `git merge --no-ff --no-commit 65745c243f`,
  MERGE_HEAD checked): 83 conflicted files, 199 hunks:
  - 75 content conflicts, 1 add/add (`texture_cache/eviction_age.h`), 7 modify/delete (upstream's
    FSR 4 files that our CheezyEmu port deleted: `video_core/fsr4_support.h`,
    `vulkan_common/ffx_files.h`, `ffx_runtime.cpp/.h`, `yuzu/configuration/amd_fsr4_panel.cpp/.h`,
    `tests/video_core/fsr4_support.cpp`);
  - most hunks: texture_pass.cpp 14, vk_graphics_pipeline.cpp 11, texture_handles.cpp 11,
    dma_pusher.cpp 7, vk_rasterizer.cpp 6, vk_present_manager.cpp 6, gpu_thread.cpp 6.
- 118 files changed on both sides: the 83 plus 35 that merged cleanly. Each of the 35 is assigned
  to an area in section 3 (several break the build as merged).
- Verdict: take all 60 commits. 22 don't meet our code (#1352, #1413, #1452, #1456, #1457, #1458,
  #1459, #1460, #1464, #1465, #1466, #1469, #1491, #1492, #1497, #1499, #1500, #1503, #1511,
  #1518, #1528, #1532; #1413 only shares nx2_gsv.h's include lines and sits next to our GetSpan
  fix in memory_manager.cpp, #1459 only shares a test helper); the other 38 do (sections 2 and 3).
- None changes CACHE_VERSION (`VulkanPipelineCacheVersion = 54`, pipeline_cache_files.h:30 on both
  sides), the GraphicsPipelineCacheKey (fixed_pipeline_state.* merge identical) or anything the
  prepared stamp holds (vk_pipeline_cache.cpp ours:2494-2496: file size and time, driver version,
  `PreparationVersion = "tuned 1"` from nx2_precompile.h:70, GPU name): the shader preparation
  doesn't run again. The lowering stamp changes (#1462's lower_to_maxwell.h): one re-lowering of
  the v2 program records at the first boot.
- Git state to know:
  - `rerere.enabled=true`. trial.out records 76 preimages and reuses none. Step 2 will reuse
    step 1's resolutions where hunks repeat: review every "Resolved ... using previous resolution".
  - The trial's MERGE_MSG names `refs/remotes/zortos/nx2-latest`, which now points at
    `c169856090`. The commit message must name `65745c243f` and list the 60 commits.

### Coverage check (main thread, against both lists)

- Commits: every one of the 60 hashes in upstream-commits.txt appears in at least one survey (GPU
  30 plus 3 pipeline-cache pointers, shaders 12, presentation/CPU 27; shared: #1437, #1462, #1463,
  #1466, #1472, #1485, #1493, #1502, #1505, #1519, #1523). None missing.
- Files: all 83 in conflicts.txt are covered (GPU 32, shaders 10, presentation/CPU 41) and all 35
  clean ones (both-files.txt minus conflicts.txt) are named in a survey. None missing.
- What the surveys left unassigned or got wrong, covered here:
  1. EXP60's code outside vk_rasterizer.cpp: `gpu.cpp:341` (handed path in GPU::OnCPURead),
     `rasterizer_download_area.h:23` (`DownloadAreaSource::Handed`), `rasterizer_interface.h:216`
     (`GetFlushAreaLocked`), and `gpu_wait_stats.h:90` (`source_names` is indexed by that enum,
     with "handed" at 3). Area 3. `.source` itself stays: GpuWaitStats and gpu.cpp:334's LockFree
     branch read it.
  2. `Nx2Perf::g_guest_frame_interval_ns` readers: `vk_host_pacer.cpp:234` and
     `vk_present_manager.cpp` wt:678 (in PaceFrame, the host pacer, which stays; not in
     PaceGeneratedFrame), declared at `nx2_perf.h:33`. Area 5.
  3. Our-only files tied to removed features: `nx2_test_switches.h`, the `nx2_profile.h:320-324`
     counters with their 'reports and fences per second' line (:809-816), and
     `vk_rasterizer.h` wt:539-541 (the '#1161 per second' stats) (area 3); `tests/video_core/texture_gc_age.cpp` (area 2); `common/spin_wait.cpp:28`
     and `.h:46` MonitoredLineSliceTicks (area 7); `yuzu/main.cpp:11,90`, `yuzu/CMakeLists.txt:182-183`,
     `yuzu/nx2_crash_dump.*` (area 6); `nx2_gsv.h:36`'s `nx2_experiment.h` include, used only by
     EXP37 (wt:4437) (area 5).
  4. Checkpoint greps corrected after checking the trees:
     - `emu_thread->disconnect()` can't be empty: upstream keeps two (up main_window.cpp:2497,
       2568); only ours in TerminateEmulationThread (8755121655) goes, so main_window.cpp keeps 2.
     - `HandleTableSize` only in src/shader_recompiler and src/video_core: upstream's kernel has
       `GetHandleTableSize` (k_capabilities.h:42, k_process.cpp:967).
     - `EDEN_NX2_CRASH_DUMP` stays upstream's in yuzu_cmd (yuzu.cpp:518-522) and NX2_NOTES.md:218;
       only our GUI writer and `dist/nx2_switches.txt:30` go.
     - `StreamlineDlssUpscaler::Inputs::Camera` stays in upstream's vk_streamline_dlss.cpp; the
       check is for vk_ocarina_camera_motion.cpp only.
     - `fsr4_capable` is no check: vk_amd_frame_gen.cpp:184 has a local of that name.
     - `noperspective_outputs`/`ScaleNoPerspectiveOutputs` exist upstream (emit_spirv_special.cpp:28ff)
       but are absent from the trial tree, so "stays absent" holds (the #820 revert survives).
  5. `common/recommended_settings.h` is a conflicted file (our tuned table), not upstream-only as
     the presentation survey says; #1569's one-line change to its declaration applies on top.
  6. The shader survey's "`--theirs` equals upstream" was rechecked: merging base -> ours into
     upstream's file with conflicts favouring upstream gives `65745c243f`'s file exactly for
     texture_pass.cpp, passes.h, global_memory_to_storage_buffer_pass.cpp, shader_handle_program.h
     and texture_handles.cpp; host_translate_info.h differs only by the duplicate member and
     fallback call the clean merge added.
  7. Parallel resolvers in one worktree would fight over `.git/index.lock`: no index-changing git
     commands in the areas (section 3 rules).

### Upstream after the target (step 2, not in this merge)

- `45d0fb6807..21f024e57a` is nine commits counting #1544's merge `21d62e43ca`; `c169856090` (#1569,
  build 171) came after. A trial of build 171 onto `5fc428bdc` (scratchpad/wt-trial171,
  conflicts171.txt) has 85 conflicted files: the same frontend files at their src/lichii paths.
- Change what step 1 resolves:
  - **#1521 `45d0fb6807`** (keep gaps): buffer_cache.h (keep verify KeptUniformMatches,
    NoteUniformWrite for DMA copies, clears, inline uploads), buffer_cache_base.h,
    uniform_stream_reuse.h, nx2_fast_draw.h (TakeDelta copies only the ranges in use),
    vk_rasterizer.cpp (TakeReusableDraw, next to our KeepsAttributeTypes line), puller.cpp
    (NoteSync after a met acquire and after drains: its lines are in upstream's acquire path, so
    they must be placed in ours), gpu_thread.cpp (NoteSync per submitted list), both tests. Our
    verify-by-kind's slot-range and older-buffer kinds become refusals before the verify.
  - **#1544 (`9b50adb7b4`, merge `21d62e43ca`)**: drops the yuzu SPDX line in 2512 files, 49 of
    step 1's conflicted files: top-of-file hunks re-conflict next to our include changes, and the
    7 FSR 4 files are modify/delete again (`git rm`). Our own files drop their yuzu SPDX lines
    (owner, CLAUDE.md 2026-11-13).
  - **#1541 `946f463618`**: src/yuzu -> src/lichii (287 renames), target yuzu -> lichii,
    YUZU_LEGACY -> LICHII_LEGACY (buffer_cache.h, buffer_cache_base.h, texture_cache_base.h),
    YUZU_TESTS -> LICHII_TESTS (tools/shader-sweep/CMakeLists.txt), both CMakeLists of video_core
    and tests. Our FFX DLL copy in src/yuzu/CMakeLists.txt (`TARGET yuzu`) conflicts at
    src/lichii/CMakeLists.txt:270-299; lichii-tuned.yml's `--target yuzu` and `-DYUZU_*` options
    follow (LegacyOptions.cmake keeps the old ones working, with deprecation messages).
  - **#1551 `99b9064bbf`** (Qt names): step 1's main_window.cpp/.h hunks come back at src/lichii;
    src/lichii/main.cpp's conflict on our `InstallNx2CrashDump();` disappears because step 1
    drops our crash writer.
  - **#1550 `21f024e57a`** (library names): YUZU_NON_COPYABLE/NON_MOVEABLE -> LICHII_* without an
    alias (our ScopedCoreGuard in physical_core.cpp, 3fa3e9c890), YUZU_PAGEBITS/PAGESIZE/PAGEMASK
    -> LICHII_*; it edits arm_dynarmic_64.cpp, nx2_gsv.h, memory.cpp, texture_cache.h and
    texture_cache_base.h next to step 1's resolutions. Our added lines use YUZU_PAGEBITS 6 times,
    YUZU_PAGEMASK, YUZU_NON_COPYABLE and YUZU_NON_MOVEABLE once each (`git diff 2eb5f081c
    5fc428bdc -- src`).
  - **#1569 `c169856090`**: renames #1536's `values` parameter (recommended_settings.cpp/.h).
    Step 1 cherry-picks it (follow-up a), so step 2 merges it as a no-op.
- Don't change step 1's resolutions: #1539 `ab42e09028` reverts #1462's spirv_emit_context.cpp
  helpers (clean in step 1; Ocarina's far forest unculled and at wrong heights on an RTX 3080) and
  adds a global_memory_coherence.cpp test; #1552 (Android), #1553 (docs).
- Consequence: no owner build of step 1. #1441's keep gaps (#1521) and #1462's SPIR-V routing
  (#1539) are only closed in step 2.

## 2. Decisions

### Upstream's code replaces ours

| Upstream | Ours that goes | Also removed |
|---|---|---|
| #1485 `bd0171f7b9` (#354 in full) | round-11 port `259c46e592`, `9fdc71f68f`, `04d0f5bdb3`, `fb2b3ccdaa`, `7ad253a111`, `680024d782`, `4ebcf1cad2`, `d456eb6027`, `9f4068f02a`, `77cc43f8b8` (patch-identical to PR #354's first ten; texture_pass.cpp byte-identical to `2741f21d6f`) | LICHII_BINDLESS_TABLE (`HandleTableSize`, boot log ours vk_pipeline_cache.cpp:766-776; no launcher or README). 'Handle table:' lines need EDEN_NX2_HANDLE_TABLE_NOTES=1 |
| #1519 `65745c243f` | '#16' `9659b7c283`, `0b00d88226`; #1161 draws `d854104fc1` | EDEN_NX2_TEST_1161_HANDLES/_DRAWS, AppendRangeOrOverflow, NoteRange, g_test_1161_draws, g_vertex_streams_dynamic, our IsFastDrawVertexFormat, BreakClass::DeltaOverflow, computed_delta, record_computed, the boot line's 1161 parts, 'Fast draw #1161 per second'; ab-29, ab-36 |
| #1515 `5b9012f4b0`, #1516 `342b839578` | #1161 reports `1fa407559e` (counters `86fc2def53`) | EDEN_NX2_TEST_1161_REPORTS, the vector FIFO (query_stream.h), our fence-list recycling (fence_manager.h wt:383-392), counters ReportLockSkips, ReportLocks, FenceListsRecycled, FenceListsNew (nx2_profile.h:320-324) and their 'reports and fences per second' line (nx2_profile.h:809-816); ab-32. With all three switches gone: `nx2_test_switches.h` and its six references |
| #1479 `31b7d67ede` | EXP36 lock-free reads `3f0ce586fa` (Fritchii 99567589b): upstream skips each cache's lock on its own, a superset | EDEN_NX2_EXP36 (ab-23 sets it); EXP60 in the same function (Fritchii e423d1bd1, off, no launcher): GetFlushAreaLocked, DownloadAreaSource::Handed, gpu.cpp's handed path |
| #1439 `562f6ceb19` | #1311 port `7f85b0007e` (= upstream PR #1311 `5d9a8f5a12`) | TextureGcEffectiveMinAge, src/tests/video_core/texture_gc_age.cpp and its tests/CMakeLists line |
| #1376 `9f932fe3c5` | latch wait `7c58293328` (Celmax via #805) | LatchEpoch, WaitForLatch, latch_cv, latch_epoch; our OnWrite(..., Source) and OnDelayedRelease call Nx2PendingWake::Get().Notify() |
| #1373 `cedfc26f47` | WAITPKG idle poll `d5bc85b117` (C0.1 slices, SpinUntilFor), LICHII_UMWAIT_C02's idle-poll part `8a50b21adb`/`a0329ba2f4` | SpinWait::MonitoredLineSliceTicks, EDEN_NX2_IDLE_PAUSE (`3fa3e9c890`; ab-15 sets it) |
| #1493 `faa4bd13a8` | LICHII_FG_PACING `c1f7068cf2`, its 10 s log `c3fe6e601f`, our FG warn-once and 10 s log (`9073c87184`), `Nx2Perf::g_guest_frame_interval_ns` (`fe9a0f3f4b`) | PaceGeneratedFrame, Frame::generated_count/index, four pacing members; README-tuned:78 |
| #1449 `5afdffd134` | `8755121655` (`emu_thread->disconnect()` before terminate) | none |
| #1453 `4607cba98a` | GUI crash writer `da111dfcf5`: with both, upstream's filter chains to ours and ours (re-installed after each launch) to upstream's, so a crash recurses until the stack overflows | nx2_crash_dump.cpp/.h, both InstallNx2CrashDump calls, yuzu/CMakeLists.txt:182-183, `EDEN_NX2_CRASH_DUMP=1` (nx2_switches.txt:30), README-tuned:199-201 |
| #1512 `6bf9b752bb` | EXP37 `80515e0677` (Fritchii f85bef3a6, off, no launcher): the mirror refreshes the page under the syncpoint lock, which EXP37's poll was for | SetGuestIncrementHook/RemoveGuestIncrementHook, gsv's OnGuestIncrement poll, nx2_gsv.h's nx2_experiment.h include |
| #1441 `31ae51bd67` | #949's keep model (base) under `d854104fc1` | none; verify-by-kind `2ea7932b5b` stays on top (its test rewritten) |
| #1462 `bf9527fde3` | none | its Intel guard joins our QueueVulkanPipelineCacheSave(bool) (`7306296431`) |

- Not built or handed out on its own: step 1 (see section 1). The owner's next build is step 2.
- #1161's profile counters go with the A/B they served (no switch left to compare).

### Ours stays over upstream's

- **CheezyEmu's FSR 4 upscaler, AMD frame generation and Radeon work** (`a205b1f07e`,
  `499960272e`, `d399d2719c`) over #1496 `8876452393` (fsr4-vk on Linux inside upstream's FSR 4
  stack; "Windows is unchanged"): `git rm` the 7 files; ours for vk_fsr4_upscaler.cpp/.h,
  qt_common/util/vk.*, configure_graphics.cpp's FSR block, shared_translation.cpp hunk 1 and
  docs/native-dlss-replacement.md; Fsr4PortDevice and the fsr4_support.h include go;
  `WantsFsr4Port` returns false. Upstream's three new device FEATUREs stay (removed off Linux).
- **Next to #1508 `80fb6db2fb`** (NVIDIA DLSS-G as a frame generation type): AMD FSR keeps its
  place in FrameGenType (owner decision 1). #1508's Streamline code needs ENABLE_NVIDIA_DLSS, MSVC
  only (up video_core/CMakeLists.txt:633), so our MinGW builds have none of it; Qt hides the type
  (configure_graphics.cpp wt:480-486); its `.camera` input is deleted from our Fsr4Upscaler call.
- **Recommended settings and defaults**: our tuned table (`c7c78bd3bb`, `ff550600e8`) and code
  defaults (`36948bc369`: precache Always, use_reactive_flushing false, async draws Bounded, VRAM
  Aggressive) stay, with #1513's two AoI entries added. #1536's per-game reset is taken.
  TunedDefaultsVersion stays 4 (config.cpp:157).
- **The MinGW build**: lichii-tuned.yml builds pgo, pgo-lto and pgo-train with MinGW clang (msvc
  variant off). `d34bcee344` (no NX converter in MinGW builds; CMakeModules untouched upstream) and
  `0c6d670797` (memory_detect.cpp, merged cleanly next to #1478) stay. #1453's handler loads
  dbghelp at run time (crash_handler.cpp:154) and builds with MinGW headers; on MinGW it also
  reports an uncaught C++ exception (code 0x20474343) before chaining on, which is fine.
  #1536's `values` parameter breaks our `-Werror=shadow` (ENABLE_WERROR defaults ON,
  CMakeLists.txt:85; src/CMakeLists.txt:184): follow-up (a).
- **GPU front**: our acquire path (#805's design, jpkto `2643a28b16` and on) with #1376's word
  pointer and GPU-tick wait ported into it; our dma_pusher layout (`aa772444ca`, `2ea9ee5b8c`,
  `410a46fdfb`, `4728e4d7df`, `5930b88867`) with #1520's copier inserted; publish before the
  wait (`c4463eca08`), QMD take-over, ThreadStats/WaitStats, clock trace. Upstream's AcquireStats
  come back (merged upstream code uses them).
- **Caches**: our report op (query_cache.h: delayed release, payload, split entry for #805's
  NoteFencedWrite, which upstream's lacks); LICHII_BANK_CHECK (`7864fd62e2`, `5acb2d398e`,
  `78b9e52d85`, `916a0ccab4`; mode 3 default, buffer_cache.h ours:2727-2738); #1306 `9940937f4e`
  (open PR) and the batched collector downloads `2fe8d78d35` inside upstream's EvictionAge;
  #1216 `c899e82787` (EDEN_NX2_TEST_1216, open PR); #974's neutral methods and report operands
  (`974469aa59`, `cf811c16aa`, open PR); `419af3688`'s KeepsAttributeTypes (our #408 keyed
  attribute types, which upstream lacks; its plain compare would bring back build B's stale
  vertex_attribute_types defect).
- **Shaders and pipelines**: the preparation (recipe, stamp, progress, journal `4218f2e086` =
  upstream #999), VulkanPipelineCacheSaveDue (`7306296431`), HoldTimeTotal and t_build_split
  (`d5d184e35a`), #896's profiler scopes (`ca992b6b3e`), the #820 revert (`b5e399c6a7`),
  `3880800d0` (a failed pipeline keeps no stages; upstream still lacks it at `c169856090`), the
  HMMA deferral (`bace6509e0`), `a202ae1d52` (open #1008).
- **CPU**: #717's JIT switches (`06d6d329b7`) with #1472's counters and #1517's Reset, #717's
  trap fast paths in memory.cpp (`98843dfae2`) under #1472's timing wrappers, the zero-page trim
  phases (`f5f9e135bb`) with #1451's catch, the front macro replay (`9571d126cb`) with #1482's
  trace path, inline CNTPCT (`25ffc03932`), option 3 (KSPIN, ARB_SPIN, IDLE_SPIN_US as the poll's
  limit, clock trace, power-mode line `96e1e4e884`), hybrid placement (`d5bc85b117`), the core
  lock (EDEN_NX2_CORE_LOCK).

### Owner decisions (the merge applies the default unless told otherwise)

1. **FrameGenType numbering.** Default: `ENUM(FrameGenType, Lsfg, Fsr3, AmdFsr, NvidiaDlssG)`:
   AMD FSR stays 2, DLSS-G becomes 3. Configs store the integer (settings_setting.h:138-139) and
   the owner's profile holds 2 = AMD FSR, so nothing changes in the tuned build. Cost: official
   Lichii, which shares %APPDATA%\lichii, reads that 2 as DLSS-G and on AMD falls back to Optical
   Flow (frame_gen_choice.h:51-54). Alternative: upstream's numbering (DLSS-G 2, AMD FSR 3) plus a
   one-time rewrite of the stored 2 (TunedDefaultsVersion 5); an official build would then clamp
   3 to its last value (DLSS-G) anyway, so it gains nothing there.
2. **#1505's boot marker.** Default: as upstream. The marker spans our whole LoadPipelinesV2,
   PrecacheShaders and the recipe preparation included (wt vk_pipeline_cache.cpp:1463 created,
   2118 PrecacheShaders, 1470 removed). After a crash or kill during the loading screen, the next
   launch builds no cached pipelines and doesn't prepare; the one after resumes the preparation,
   skipping the crashed builds (our journal). Alternative: end the marker before PrecacheShaders,
   since the journal covers the preparation: one launch saved after a preparation crash, at the
   price of a divergence from upstream.

## 3. Resolution by area

| Area | Files (hunks) | Hard | Clean files to check |
|---|---|---|---|
| 1 front: GPU thread, command lists, acquires | 8 (29) | nx2.h, gpu_thread.cpp, dma_pusher.cpp | 3 |
| 2 engines, fast draws, textures | 10 (24) | texture_cache.h | 7 |
| 3 buffers, queries, rasterizer | 10 (24) | buffer_cache.h | 2 |
| 4 shaders and pipelines | 10 (53) | none (2 moderate) | 11 |
| 5 presentation, frame generation, FSR 4, device, gsv | 22 (35) | none (7 moderate) | 2 |
| 6 settings and frontend | 11 (13) | none | 0 |
| 7 CPU, JIT, core memory, build lists | 12 (21) | physical_core.cpp | 10 |

Rules for the parallel resolvers:
- Each resolver edits only its area's files (conflicted, clean-to-check and the our-only files
  named for it). One resolver per file.
- No git command that writes the index (`add`, `rm`, `checkout --ours/--theirs`): parallel ones
  collide on `.git/index.lock`. Take a whole side with `git show :2:<path> > <path>` (ours) or
  `git show :3:<path> > <path>` (theirs); each resolver lists the files it resolved and the ones
  to delete. The main thread then runs one `git add` and the `git rm`s.
- Compile errors the clean merge made count as part of the area (listed per file).

### Area 1: front (GPU thread, command lists, acquires)

- `video_core/nx2.h` (3, hard): h1 (wt:10-13) drop `<condition_variable>` with latch_cv. h2
  (wt:644-658) upstream's word/seen slots plus our source/delayed latch fields. h3 (wt:690-797)
  our LatchSource, OnWrite(..., Source), OnDelayedRelease, OnDelayedReleaseDone,
  ClearDelayedReleases plus upstream's WordStored; `Nx2PendingWake::Get().Notify()` where we
  notified latch_cv; drop LatchEpoch, WaitForLatch, latch_epoch. Keep upstream's Nx2PendingWake
  and `g_nx2_guest_frame_interval_ns` (wt:36, clean) as they are.
- `video_core/gpu_thread.cpp` (6, hard): h1 our includes plus nx2_back_idle_trace.h. h2 both. h3
  `wake.Arm()`, no seen_latch. h4 `if (!popped)`, our MaybeTakeOver after ResumePending, our
  `split.Publish()` before the wait (`c4463eca08`), BackIdleTrace states. h5 upstream's
  Poll/Sleep with `Common::ClockTrace::NoteProcessor()` after each wait. h6 ThreadStats timer,
  WaitStats idle scope, BackIdleTrace states, PopWait, NoteProcessor. Add
  `if (VideoCommon::g_acquire_counting) split.ReportAcquire();` (up gpu_thread.cpp:97-99).
- `video_core/dma_pusher.cpp` (7, hard): h1/h2 both includes. h3 (wt:64-337) both blocks, each
  closed: our anonymous namespace (copy writers, NoteListRead, static_asserts, EngineWrites),
  then upstream's CommandWordsPool and its namespace (CopierPause, SnapshotCopier,
  WaitForCopyJob). h4 (wt:343-363) upstream's WaitForSnapshotCopies/QueueSnapshotCopy. h5
  (wt:374-423) HEAD (empty): upstream's ctor/dtor/CanResumeAcquire/DispatchCalls duplicate ours
  (wt:500-560); move upstream's dtor body (WaitForSnapshotCopies, clear, CommandWordsPool::Trim)
  into ours (wt:508) and `Nx2PendingWake::Get().WaitForGpuTick(*pending_list_tick)` into our
  CanResumeAcquire (wt:510), keeping ApplyCollectedWrites and `Defer(..., Writes::Caches)`. h6
  (wt:589-617) theirs (publish before copying, copier for the later lists). h7 (wt:1274-1323) our
  NoteEngineWrites, then RefreshSnapshot with our Resnapshot zone and upstream's WaitForCopyJob.
- `video_core/dma_pusher.h` (1): ours plus QueueSnapshotCopy and WaitForSnapshotCopies; drop
  NoteDmaSemaphore (wt:281; no definition left); RefreshSnapshot's comment gets "Waits for the
  list's copier job first."
- `video_core/gpu_split.cpp` (5, moderate): h1 both (EngineName, WaitState, CountedWait). h2 both
  settings blocks. h3 our InMemory predicate with `boost::unordered::erase_if(semaphore_writes,
  [this](const auto& e) { return InMemory(e.second); })` (semaphore_writes is a boost flat map
  now; std::erase_if doesn't take it). h4 our comment plus upstream's idle_since lines without its
  SplitWait scope (ours at wt:960). h5 theirs (ReportAcquire).
- `video_core/gpu_split.h` (1): theirs (AcquireStats, g_acquire_stats, g_acquire_counting,
  g_acquire_verify, Count; merged code in gpu_thread, dma_pusher, CountedWait and SettleIdleBack
  calls them) plus `void ReportAcquire();`.
- `video_core/engines/puller.cpp` (4, moderate): h1-h3 ours (CheckWithoutDrain isn't on our path;
  upstream's counters there would count acquires twice). h4 our AddWaiter with upstream's comment,
  and give it the 4-byte-aligned word pointer (GetPointer) so WordStored watches our waiters.
- `video_core/control/scheduler.cpp` (2): both includes; our AcquirePoll scope and PublishSoon,
  then upstream's `BackIdleTrace::SetFrontState(Parse)`.
- Clean, check: `renderer_vulkan/vk_master_semaphore.cpp/.h` (identical to upstream; OnGpuTick
  needs Nx2PendingWake intact), `core/hle/service/nvdrv/nvdrv.cpp` (both resets present, wt:137
  ClearDelayedReleases and wt:140 the frame interval).

### Area 2: engines, fast draws, textures

- `video_core/engines/maxwell_3d.cpp` (4, moderate): h1 both includes. h2 HEAD without our
  IsFastDrawVertexFormat (wt:390 reads g_vertex_streams_dynamic); upstream's (wt:434-439) stays.
  h3 theirs (WriteBoundConstantBuffer, wt:802-812) plus our CbUpload `upload_scope` line. h4
  `auto result = trace && trace->data_ram ? interpreter : jit;`, then our OutsideCodeError replay
  on the JIT result (`9571d126cb`).
- `video_core/engines/maxwell_3d.h` (1): HEAD (IsFastDrawNeutralMethod, IsFastDrawReportOperand);
  IsFastDrawVertexFormat declared once.
- `video_core/engines/kepler_compute.cpp` (4, moderate): h1 both includes; h2 both QMD notes; h3
  upstream's FrontRunsMacroWord plus our `FrontNoteReport(..., bool from_macro)` (kepler_compute.h
  wt:295 has ours); h4 as maxwell_3d.cpp h4.
- `video_core/macro_turing.h` (2): both fields; sort/unique data_ram_reads and register_reads,
  then build `instructions` from `states` with the loop variable `reached` (#1502).
- `video_core/nx2_fast_draw.h` (3, moderate): theirs in all three; then delete
  g_vertex_streams_dynamic (wt:88), g_test_1161_draws (wt:92), BreakClass::DeltaOverflow (wt:121,
  its name at wt:132, its ClassOf case wt:153-154), AppendRangeOrOverflow and NoteRange
  (wt:396-399).
- `tests/video_core/fast_draw.cpp` (3): h1/h2 theirs (no TEST switch); h3 both test sets, each
  closed.
- `tests/video_core/maxwell_fast_method.cpp` (1): both includes.
- `video_core/texture_cache/texture_cache.h` (4, hard): h1 HEAD (CollectorDownload), drop
  upstream's second Configure (wt:203-217) and in ours (wt:262-283) set `ticks_to_destroy =
  EvictionAge(mode_age, min_age, used_memory(), retention_limit, out_of_memory)`, keeping #1306's
  block with a plain 3-frame age. h2 upstream's last_tick with our pending-download check in both
  ForEachItemBelow calls. h3 HEAD (empty) and add `++churn.created; churn.joins += ...` to our
  relocated image-creation block (wt:3914). h4 `VideoCommon::Churn::Add(VideoCommon::Churn::ImagesDeleted);
  ++churn.deleted; MarkStale(image);`. Qualify every unqualified `Churn::Add` (wt:3102, 3245,
  3831, 3882, 3914, 4612, 4828, 4900): upstream's nested `struct Churn` (texture_cache_base.h:383)
  hides `VideoCommon::Churn`. Drop the second eviction_age.h include (wt:41, 44).
- `video_core/texture_cache/texture_cache_base.h` (1): both (upstream's Churn/TakeChurn, our
  ClassifyDrawTargets `2f6e59a420`).
- `video_core/texture_cache/eviction_age.h` (add/add): theirs. Delete our TextureGcEffectiveMinAge
  users and hand `src/tests/video_core/texture_gc_age.cpp` to the main thread for `git rm` (area 7
  drops its CMake line).
- Clean, check: `engines/kepler_compute.h`, `nx2_fast_draw.cpp`, `renderer_vulkan/fixed_pipeline_state.cpp/.h`
  (ours equals upstream's plus our logic-op normalization), `memory_manager.cpp/.h` (#1441's
  WriteConstantBufferData, #1413's ReadKept/WriteKept next to our GetSpan fix `d3251a67c`),
  `renderer_vulkan/vk_texture_cache.cpp` (#1466's in-image blit layout next to CheezyEmu's FSR
  changes).

### Area 3: buffers, queries, rasterizer

- `video_core/buffer_cache/buffer_cache.h` (5, hard): h1 upstream's `WriteMemory(...,
  keep_page_gpu_data, engine_write)` plus our `Core::g_buffer_write_pages.Mark` (wt:307-313). h2
  and h5 (mark before arming read traps) are the same code: either side. h3 theirs
  (BeginGraphicsUniforms, UniformRangeKept, UniformKeep; wt:1878-1913). h4 our LICHII_BANK_CHECK
  block using upstream's `native_bank` (wt:2750) instead of our redeclaration (wt:2828), with
  `!memory_tracker.IsRegionGpuModified(device_addr, size)` in our conditions.
- `video_core/buffer_cache/buffer_cache_base.h` (1): both (#1530's CreateTrace, our IndexBufferMemo
  `885d5aa8ab`).
- `video_core/renderer_vulkan/vk_rasterizer.cpp` (6, moderate): h1 (wt:707) upstream's check with
  our `pipeline_cache.KeepsAttributeTypes(*draw.pipeline, FixedPipelineState::DynamicAttributeTypes(regs))`
  (`419af3688`). h2 (wt:744) theirs (else `begin`/`end` twice). h3 (wt:764) theirs, without the
  counter. h4 (wt:3004) both: our drain-check note, then `Nx2FastDraw::NoteSync()`. h5 (wt:3200)
  theirs, without ReportLocks/ReportLockSkips. h6 (wt:3252) upstream's GetFlushArea body, still
  setting `.source` (TextureCache, BufferCache, LockFree when it took neither lock); no EXP36,
  EXP60 or GetFlushAreaLocked. Outside the hunks: delete our inner `verify_kept` (wt:1515-1518;
  -Werror=shadow against wt:1499), the TEST parts of the boot line (wt:563-588, with the
  g_test_1161_draws/g_vertex_streams_dynamic stores at 575-576), 'Fast draw #1161 per second'
  (wt:909-916) with its stats fields (vk_rasterizer.h wt:539-541: widened, between_handles,
  formats_kept) and the nx2_test_switches.h include (wt:88).
- `video_core/renderer_vulkan/vk_rasterizer.h` (2): both includes, both declarations
  (InitFastDrawBreakClasses `4dc571590a`, DrawReuseProbe).
- `video_core/video_core.cpp` (2): both; drop the three 1161 entries from LogTestSwitches (wt:81-83,
  the array size at wt:80 from 7 to 4) and the nx2_test_switches.h include.
- `video_core/fence_manager.h` (1): upstream's (spare_operations, fence_thread_waiting); delete our
  duplicate recycling block (wt:383-392), its counters and the include.
- `video_core/query_cache/query_cache.h` (1, moderate): ours (the code after the hunk calls our
  RunReportOperation, query_cache_base.h).
- `video_core/query_cache/query_stream.h` (2): theirs.
- `video_core/renderer_vulkan/vk_query_cache.cpp` (2): theirs (host reset on the worker only), no
  TEST switch or include.
- `tests/video_core/buffer_cache_ports.cpp` (2, moderate): h1 both helper sets (`03a52a14db`'s
  ListWrite/UseGpuMemory and upstream's); h2 both test sets, each closed. Rewrite the expectations
  of "Uniform keep verify counts each kind of mismatch" (wt:1280): under #1441 a moved binding is
  no longer a keep (UniformRangeKept compares the range).
- Our-only files: delete `video_core/nx2_test_switches.h` (main thread `git rm`); EXP60 in
  `gpu.cpp` (handed path, :341), `rasterizer_download_area.h` (Handed, :23),
  `rasterizer_interface.h` (GetFlushAreaLocked, :216) and `gpu_wait_stats.h` (keep
  `source_names` (:90) in step with the enum); `nx2_profile.h` counters ReportLockSkips,
  ReportLocks, FenceListsRecycled, FenceListsNew (:320-324) and their 'reports and fences per
  second' line (:809-816).
- Clean, check: `query_cache/query_cache_base.h` (our ReportOp next to upstream's FreeMany and bank
  counters), `rasterizer_interface.h` (InvalidateConstantBufferData defaults to InnerInvalidation).

### Area 4: shaders and pipelines

- Upstream's file whole (`git show :3:`; checked equal to `65745c243f`'s):
  `shader_recompiler/ir_opt/texture_pass.cpp` (14), `tests/video_core/texture_handles.cpp` (11),
  `video_core/shader_handle_program.h` (5; hunk by hunk would leave two StorageBufferBase
  definitions), `shader_recompiler/ir_opt/passes.h` (2),
  `shader_recompiler/ir_opt/global_memory_to_storage_buffer_pass.cpp` (1).
- `shader_recompiler/host_translate_info.h` (clean, compile error: max_per_stage_descriptor_samplers
  twice, wt:25-26 and 28-29, fallback called twice, wt:82 and 84):
  `git show 65745c243f:src/shader_recompiler/host_translate_info.h > ...`.
- `video_core/renderer_vulkan/vk_graphics_pipeline.cpp` (11, moderate): h1 both (HoldTimeTotal with
  its brace, then TakeTextureMemoCounts); h2-h11 theirs (h2 must be: the merged DeltaBlocker has no
  ComputedHandles, wt .h:350-356). Outside: delete `computed_delta` (wt:464, 486-491), our comment,
  `computed_handles` and `record_computed` (wt:806-808; upstream's at wt:782-783 stay), the
  nx2_test_switches.h include (wt:48).
- `video_core/renderer_vulkan/vk_graphics_pipeline.h` (3): h1 both (HoldTimeTotal used at wt
  vk_pipeline_cache.cpp:3101, 3469, 3475; TakeTextureMemoCounts at wt vk_rasterizer.cpp:1058);
  h2-h3 theirs.
- `video_core/renderer_vulkan/vk_pipeline_cache.cpp` (4, moderate): h1 both: scm_rev.h,
  scope_exit.h, shutdown_progress.h, file_lock.h; drop HEAD's misplaced zstd_compression.h. h2
  theirs (boot log without LICHII_BINDLESS_TABLE). h3 ours (VulkanPipelineCacheSaveDue,
  QueueVulkanPipelineCacheSave(bool again_if_busy)) with `|| !device.CanReadPipelineCacheDuringBuilds()`
  added to its condition. h4 ours (t_build_split translation time). Outside: delete wt:767 (our
  `.max_per_stage_descriptor_samplers`, duplicate and out of order) and wt:3837-3851 (our second
  LimitPipelineSampledTextureArrays; upstream's runs before the mesh fallback, wt:3659-3675). Keep
  KeepsAttributeTypes (ours, :1168) and upstream's g_dynamic_vertex_input store (wt:870).
- `shader_recompiler/frontend/maxwell/translate_program.cpp` (1): theirs; keep the clean parts
  (TranslateProfile scopes, store extents, #1466's passthrough mask wt:407-415).
- `video_core/renderer_vulkan/vk_compute_pipeline.cpp` (1): theirs (no `[[unlikely]]`); #1462's
  removed_conditional_barrier (wt:269-276) stays.
- Clean, check: `ssa_rewrite_pass.cpp` (#1468 inside our SsaFinal scope, wt:467-491),
  `backend/spirv/spirv_emit_context.cpp` (#1462's helpers next to our #820 revert; #1539 reverts
  them in step 2), `backend/spirv/emit_spirv_context_get_set.cpp` (#1466's pruned outputs vs our
  #820 revert), `frontend/ampere/lower_to_maxwell.h` (#1462 lowering next to `a202ae1d52`),
  `shader_info.h` (upstream's plus our uses_software_matrix), `tools/shader-sweep/main.cpp`,
  `vk_pipeline_cache.h` (#1505's LoadPipelinesV2 signature), `pipeline_helper.h`,
  `renderer_opengl/gl_compute_pipeline.cpp`, `gl_graphics_pipeline.cpp`. The #1505 guards, #1523's
  record drop next to our `admit` (wt:1559-1573, 3899-3903), #1466's pruning in our
  deferred-emission loop (wt:3924-3928) and #1463's compatible-version read (wt:2216-2227) merged
  where upstream has them.

### Area 5: presentation, frame generation, FSR 4, Vulkan device, gsv

- `renderer_vulkan/renderer_vulkan.cpp` (3, moderate): wt:309/324 upstream's loop (no HDR pause,
  WantedGenerations with TakeBlockedTime, present_spacing, colour space), keeping
  `generated->render_tick = scheduler.Flush(...)`, without generated_count/index. wt:359
  `frame->render_tick = scheduler.Flush(*frame->render_ready);` plus upstream's
  EGLANTINE_STREAMLINE end-frame block. The HDR log text (wt:437) is upstream's already, as r59's.
- `renderer_vulkan/renderer_vulkan.h` (1): ours plus upstream's `Streamline::FrameGenActive()` check.
- `renderer_vulkan/vk_present_manager.cpp` (6, moderate): union of includes; StartPresentThread and
  LogPacing, a destructor that stops the present thread, our present wait thread, FrameGenSuspend;
  wt:509 `last_present_id = 0`, upstream's trace/SleepUntil/CopyToSwapchain/next_present/LogPacing,
  then our present-wait block; wt:772 our drain plus FrameGenSuspend; wt:1074 our AMD target and
  `last_present_id = swapchain.Present(...)` inside upstream's Streamline calls. Outside: delete
  PaceGeneratedFrame (call wt:492-499, body wt:543-620); PaceFrame's read (wt:678) becomes
  `static_cast<long long>(Tegra::g_nx2_guest_frame_interval_ns.load(...))`.
- `renderer_vulkan/vk_present_manager.h` (2, moderate): Frame keeps render_tick, queued_at,
  frame_gen_id, frame_gen_rect, adds upstream's present_spacing and Streamline fields, loses
  generated_count/index; our host pacer members minus the four FG pacing members, plus upstream's
  next_present, pacing_log, blocked_time; delete the PaceGeneratedFrame declaration (wt:128).
- `renderer_vulkan/vk_swapchain.cpp` (2, moderate): wt:459 upstream's scRGB unload, then
  `present_mode = ChooseSwapPresentMode(has_imm, has_mailbox, has_fifo_relaxed, surface_format.format);`,
  then our "Swapchain:" log and AMD gate unchanged (r59's CanPresent line replaces the gate if r59
  is in first). wt:557 our real_size/failures/AMD create block, then
  `if (!frame_gen_swapchain) { try { ... } catch (...) { ... } }` with upstream's catch.
- `renderer_vulkan/present/frame_gen.cpp` (3, moderate): `<chrono>` and `<cmath>`; upstream's
  unavailable/nvidia_frame_gen/SetState plus `|| type == Settings::FrameGenType::AmdFsr` in the
  early return; upstream's `SetState(UnsupportedSurface, format)`; delete our 10 s log (wt:340-366).
- `renderer_vulkan/vk_fsr4_upscaler.cpp` (3) and `.h` (2): ours whole (`git show :2:`); hunk by hunk
  leaves upstream's stray `#endif` (wt:255).
- `renderer_vulkan/vk_ocarina_camera_motion.cpp` (1): `... | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
  FrameGenCopyUsage()`; delete wt:1120 `.camera = StreamlineDlssUpscaler::Inputs::Camera{...}`
  (our Fsr4Upscaler::Inputs, ours vk_fsr4_upscaler.h:37-53, has no camera).
- `vulkan_common/vulkan_device.cpp` (1, moderate): ours (AmdFidelityFxDriver), plus
  `bool WantsFsr4Port(VkDriverId) { return false; }` for wt:837 and 2130; delete
  Device::Fsr4PortDevice (wt:1063-1093).
- `vulkan_common/vulkan_device.h` (2): both in each (PresentId/PresentWait with
  ShaderIntegerDotProduct; DefersSoftwareMatrixPipelines with CanReadPipelineCacheDuringBuilds,
  which area 4 needs); delete the fsr4_support.h include (wt:24) and Fsr4PortDevice (wt:1290).
- The 7 modify/delete files: main thread `git rm`.
- `core/hle/service/nx2_gsv.h` (5, moderate): wt:35 drop nx2_experiment.h (only EXP37 used it,
  wt:4437), keep scope_exit.h; wt:51 both (gpu_wait_stats.h, gpu_split.h); wt:2466 theirs; wt:2483
  our genlock, then `Tegra::g_nx2_guest_frame_interval_ns.store(static_cast<u64>(interval.count()),
  std::memory_order_relaxed)`; wt:4228 theirs (SetGuestMirror) and delete the EXP37 code (the
  hook setup and OnGuestIncrement, wt:4433-4482; m_syncpt_poll_now and _pending, wt:4982-4984).
- `video_core/host1x/syncpoint_manager.cpp` (1) and `.h` (2): theirs (GuestMirror,
  WithGuestValuesStable, `Increment(..., syncpoint_id)`); no guest increment hook.
- `docs/native-dlss-replacement.md` (1): ours (r59 adds its HDR bullet there).
- Our-only files: `video_core/nx2_perf.h` (delete g_guest_frame_interval_ns, :33),
  `renderer_vulkan/vk_host_pacer.cpp:234` (read Tegra's, cast; include nx2.h).
- Clean, check: `renderer_base.h` (#1508's NvidiaFrameGen; main_window.cpp's switch, wt:5575, is
  the only one and has it), `renderer_vulkan/vk_scheduler.h` (PauseWorker, Streamline only).

### Area 6: settings and frontend

- `common/settings.h` (1): upstream's hyrule_dynamic_fps and hyrule_30fps, then our
  `use_reactive_flushing{linkage, false, ...}` with its comment. `amd_fsr4{linkage, true, ...}`
  (ours:448) is untouched.
- `common/settings_enums.h` (1): `ENUM(FrameGenType, Lsfg, Fsr3, AmdFsr, NvidiaDlssG);` and
  `ENUM(AmdFrameGenVersion, Auto, Fsr31);` (owner decision 1).
- `common/recommended_settings.h` (1): both (the two AoI entries; our reactive-flushing false
  with its comment).
- `tests/common/first_run_config.cpp` (1): both (two AoI CHECK_FALSEs, ours for reactive flushing).
- `qt_common/config/shared_translation.cpp` (3, moderate): wt:198 ours (upstream's INSERT names
  `ocarina_fsr`, which our tree removed); wt:349 our AMD frame_gen_type tooltip, then upstream's
  Optical Flow and DLSS-G paragraph, plus amd_frame_gen_version; wt:727 our AmdFsr pair,
  upstream's NvidiaDlssG pair, our AmdFrameGenVersion list.
- `qt_common/config/shared_translation.h` (1): both map entries (main_window.cpp's
  `find(shown_type)->second` doesn't check).
- `qt_common/util/vk.cpp`, `vk.h` (1 each): ours.
- `yuzu/configuration/configure_graphics.cpp` (1): ours (empty). The clean #1508 parts stay:
  `removeItem(findData(NvidiaDlssG))` works by item data (shared_widget.cpp:184).
- `yuzu/main_window.cpp` (1): upstream's narrower disconnect; drop ours. Outside: the
  InstallNx2CrashDump re-install (wt:2173) and include (wt:48).
- `yuzu/main_window.h` (1): both (#1344's nextendo members, our emu_force_stop_clock and
  shutdown_progress_seen `6687ab37b7`).
- Our-only files: `yuzu/main.cpp` (include :11, call :90), `yuzu/CMakeLists.txt:182-183`;
  `yuzu/nx2_crash_dump.cpp/.h` to the main thread for `git rm`.
- Check (upstream-only): qt_common.cpp:231 installs #1453's handler; main_window.cpp's clean
  #1508 UpdateFrameGenButton around our AMD branch (wt:5531-5625), #1536's FrontendCommon calls,
  #1478's RAM dialog, #1463's loose-dump precache (wt:4711-4715).

### Area 7: CPU, JIT, core memory, build lists

- `core/hle/kernel/physical_core.cpp` (4, hard): wt:11 add logging.h and thread.h, drop immintrin.h
  with the PAUSE loop. wt:36 both anonymous-namespace blocks (CoreLockEnabled/ScopedCoreGuard and
  g_idle_stats). wt:320/379 our Idle frame (IdleSpinMicroseconds as `limit`, m_profile, ClockTrace
  PollStart/PollEnd/NoteProcessor, the Nx2Wait or condition-variable wait) with upstream's
  m_idle_poll clamp, `Common::WaitForFlag(m_is_interrupted, start + m_idle_poll)`, NextPollWindow
  and the stats block. Outside: store m_interrupt_time in Interrupt()'s CoreLockEnabled branch
  (wt:458-468; the merge put it only in the mutex branch, wt:479); delete the
  SpinUntilFor/PAUSE loop and EDEN_NX2_IDLE_PAUSE.
- `common/thread.h/.cpp` (clean): add `bool WaitForFlag(const std::atomic<u32>& flag, deadline)`
  (same body, tests `!= 0`): m_is_interrupted is atomic<u32> (core lock); Nx2PendingWake keeps
  the bool one.
- `common/spin_wait.cpp/.h` (our-only): delete MonitoredLineSliceTicks (:28, .h:46; the idle poll
  was its only caller). UsesC02 stays for the other waits.
- `core/arm/dynarmic/arm_dynarmic_64.cpp` (4, moderate): both includes; both anonymous blocks
  (Nx2EnvOn/Nx2DispatchBits, g_jit_counters); CountHint in our WFE/Yield pause case and in the
  WFI/SEV/SEVL case; `config.counters = &g_jit_counters[m_core_index]` after our #717 block.
- `dynarmic/backend/x64/a64_emit_x64.cpp` (1): `}(), any_xmm, conf.spill_to_free_gpr,
  conf.scratch_copy_to_free_reg);` (ours was a brace init, upstream's is Reset).
- `dynarmic/backend/x64/reg_alloc.h` (2): `void Reset(std::bitset<32> new_gpr_order,
  std::bitset<32> new_xmm_order, bool spill_to_gpr_ = false, bool scratch_copy_ = false) noexcept;`;
  both member sets.
- `dynarmic/backend/x64/reg_alloc.cpp` (1, moderate): upstream's Reset plus the two flag
  assignments. Outside: `MarkDirty(copy_location);` after WriteLock in our scratch_copy branch
  (wt:334), or EndOfAllocScope never frees that register.
- `core/device_memory.cpp` (1): `std::pair<size_t, size_t> result; try { result =
  buffer.TrimZeroPages(&phases); } catch (const std::bad_alloc&) { ...; continue; }` with
  upstream's warning and 60 s retry.
- `core/memory.cpp` (3, moderate): both includes; upstream's timed HandleRasterizerDownload/Write
  wrappers forwarding known_pointer to our bodies, renamed DownloadFromRasterizer/WriteToRasterizer;
  WriteSlot stays.
- `common/CMakeLists.txt` (1): windows/crash_handler.* and windows/hybrid_cpu.*.
- `core/CMakeLists.txt` (1): kernel/nx2_thread_stats.*, guest_core_profile.*, heap_trace.*.
- `video_core/CMakeLists.txt` (1): frame_gen_choice.h and front_qmd.h; delete nx2_test_switches.h
  (wt:146).
- `tests/CMakeLists.txt` (1): core/key_derivation.cpp and common/clock_trace.cpp; delete
  video_core/texture_gc_age.cpp (wt:20).
- `AGENTS.md` (1): union of rows (our clock trace row, upstream's #1472, #1373, #1437, #1446, #1453).
- Clean, check: `common/memory_detect.cpp` (#1478 next to `0c6d670797`), `core/core.cpp` (#1450
  before power-off, #1462's guest RAM release joins our trim jthread first: device_memory.h:80-87),
  `core/file_sys/patch_manager.cpp` (#1504 next to our build-id patches), `kernel/k_thread.h`,
  `kernel/kernel.cpp`, `kernel/physical_core.h` (m_is_interrupted stays atomic<u32>),
  `dynarmic/backend/x64/a64_interface.cpp`, `dynarmic/interface/A64/config.h`.

### Cross-area dependencies

- 2 -> 3, 4: nx2_fast_draw.h ends as upstream's API (no DeltaOverflow, AppendRangeOrOverflow,
  NoteRange, g_test_1161_draws, g_vertex_streams_dynamic). Area 3 removes their uses in
  vk_rasterizer.cpp and video_core.cpp; area 4's g_dynamic_vertex_input store (wt:870) stays.
- 3 -> 4, 7: area 3 deletes nx2_test_switches.h; area 4 drops its include in
  vk_graphics_pipeline.cpp, area 7 its video_core/CMakeLists.txt line.
- 2 -> 7: area 2 deletes texture_gc_age.cpp; area 7 drops it from tests/CMakeLists.txt.
- 4 -> 3: vk_graphics_pipeline.h keeps TakeTextureMemoCounts (vk_rasterizer.cpp:1058) and
  vk_pipeline_cache.cpp/.h keep KeepsAttributeTypes (TakeReusableDraw).
- 5 -> 4: vulkan_device.h keeps CanReadPipelineCacheDuringBuilds and DefersSoftwareMatrixPipelines.
- 6 -> 5: FrameGenType {Lsfg, Fsr3, AmdFsr, NvidiaDlssG} (frame_gen.cpp, renderer_vulkan.h,
  vk_swapchain.cpp, vk_amd_frame_gen.cpp:58 by name).
- 1 -> 5, 3: nx2.h keeps upstream's Nx2PendingWake and g_nx2_guest_frame_interval_ns; our
  Nx2SemaphoreWaiters signatures (OnWrite(source), OnDelayedRelease) stay, so query_cache.h's
  calls don't change. gpu_split.h's AcquireStats come back (nx2_gsv.h includes gpu_split.h).
- 5 <-> 1: none left: syncpoint_manager.* and nx2_gsv.h (EXP37 on both sides) are both in area 5.
- 7 -> 1: the u32 WaitForFlag overload is area 7's; nx2.h uses the bool one, unchanged.
- 6 -> 7: area 6 removes our crash writer; area 7 adds upstream's crash_handler to common.

## 4. Checkpoints and procedure

Procedure:
1. Gate: tuned4 still `5fc428bdc`, or the trial re-run on its new head. If r59 (HDR frame
   generation, branch r22-r59) lands first: expect the same 83 files and hunk counts
   (hdrfg/design.md section 8); vk_swapchain.cpp hunk 1 then carries r59's CanPresent line.
2. Tag `tuned4-pre-nx2c`. Resolve in the trial worktree (or a fresh `git merge --no-ff --no-commit
   65745c243f` on `r22-merge-nx2c`): seven resolvers in parallel, the rules above. Then the main
   thread: one `git add` of the resolved files, `git rm` of the 7 FSR 4 files,
   nx2_test_switches.h, texture_gc_age.cpp and nx2_crash_dump.cpp/.h.
3. Checkpoint A, commit with a message that names `65745c243f` and lists the 60 commits, push
   `r22-merge-nx2c` (a non-tuned branch: no CI).
4. Follow-up (a) (#1569), then checkpoint B. Push after each.
5. Step 2 (build 171) on top; the owner's build, the patch on tuned and the docs come after it.

Checkpoint A, before the commit:
- `git diff --name-only --diff-filter=U` empty; no `^<<<<<<< |^>>>>>>> ` in the 83 files or the
  35 clean ones; `git diff --cached --check` clean.
- Must be empty (`git grep` in src unless noted):
  - fast draws and TEST switches: `g_test_1161_draws`, `g_vertex_streams_dynamic`,
    `AppendRangeOrOverflow`, `DeltaOverflow`, `Draws1161`, `Handles1161`, `Reports1161`,
    `EDEN_NX2_TEST_1161`, `nx2_test_switches`, `computed_delta`, `record_computed`,
    `DeltaBlocker::ComputedHandles`, `FenceListsRecycled`, `ReportLockSkips`;
  - reads and syncpoints: `GetFlushAreaLocked`, `DownloadAreaSource::Handed`,
    `ReadNx2Experiment(36`, `ReadNx2Experiment(37`, `ReadNx2Experiment(60`,
    `SetGuestIncrementHook`, `RemoveGuestIncrementHook`;
  - front: `WaitForLatch`, `LatchEpoch`, `latch_cv`, `NoteDmaSemaphore`,
    `std::erase_if(semaphore_writes`;
  - textures: `TextureGcEffectiveMinAge`, `texture_gc_age`, and
    `git grep -n '[^:]Churn::Add' src/video_core/texture_cache/texture_cache.h`;
  - shaders: `LICHII_BINDLESS_TABLE`, `u32 known_bases`,
    `HandleTableNotes::Wanted()) [[unlikely]]`, `HandleTableSize` in src/shader_recompiler and
    src/video_core (the kernel's GetHandleTableSize stays), and still `noperspective_outputs`,
    `ScaleNoPerspectiveOutputs`;
  - presentation and FSR: `LICHII_FG_PACING`, `PaceGeneratedFrame`, `generated_index`,
    `generated_count`, `CheezyEmu 93d8f9b86`, `g_guest_frame_interval_ns`, `fsr4_support`,
    `Fsr4PortDevice`, `IsFsr4PortGpu`, `CheckFsr4Port`, `IsFsr4CapableGpu`,
    `vulkan_common/ffx_runtime.h`, `amd_fsr4_panel`, `ocarina_fsr`, and
    `StreamlineDlssUpscaler::Inputs::Camera` in vk_ocarina_camera_motion.cpp;
  - merge 2's, still: `marking_effects`, `MarkEffects`, `oot_effects_`,
    `OcarinaTemporalEnabled`, `oot_backend`, `pass_1440p`, `g_upload_batching`, `over_budget`;
  - CPU and crash writer: `EDEN_NX2_IDLE_PAUSE`, `MonitoredLineSliceTicks`,
    `SpinUntilFor(&m_is_interrupted`, `InstallNx2CrashDump`, `nx2_crash_dump`.
- Must match (count):
  - settings: `amd_fsr4{linkage, true` 1, `use_reactive_flushing{linkage, false` 1 (settings.h);
    `ENUM(FrameGenType, Lsfg, Fsr3, AmdFsr, NvidiaDlssG)` 1, `ENUM(AmdFrameGenVersion, Auto, Fsr31)` 1;
    `FrameGenType::AmdFsr, QStringLiteral` 1 and `FrameGenType::NvidiaDlssG,` 1 (shared_translation.h);
  - preparation: `PreparationVersion = "tuned 1"` 1 (nx2_precompile.h), `VulkanPipelineCacheVersion = 54`
    1, `A failed build kept no SPIR-V` 1, `bool PipelineCache::VulkanPipelineCacheSaveDue` 1,
    `CanReadPipelineCacheDuringBuilds()` 2 and `#include "common/scope_exit.h"`,
    `.max_per_stage_descriptor_samplers =`, `LimitPipelineSampledTextureArrays(` 1 each
    (vk_pipeline_cache.cpp); `u32 max_per_stage_descriptor_samplers{}` 1 (host_translate_info.h);
    `small_vector<ComputedHandle, 16> computed_handles` 1 (vk_graphics_pipeline.cpp);
    `GPUVAddr StorageBufferBase(` 1; `HoldTimeTotal` 1 each in vk_graphics_pipeline.h/.cpp;
  - frontend: `GetFrameGenStatus` 1 (main_window.cpp), `emu_thread->disconnect()` 2 (upstream's),
    `InstallCrashHandler` 1 (qt_common.cpp);
  - GPU: `KeepsAttributeTypes(` 1 (vk_rasterizer.cpp), `const bool verify_kept` 1,
    `WaitForGpuTick` 1 and `^DmaPusher::DmaPusher` 1 (dma_pusher.cpp),
    `boost::unordered::erase_if(semaphore_writes` 1, `Nx2PendingWake::Get().Notify()` in
    Nx2SemaphoreWaiters' OnWrite and OnDelayedRelease, `const auto Configure` 1 (texture_cache.h),
    `Tegra::g_nx2_guest_frame_interval_ns.store` 3 (nx2_gsv.h);
  - CPU: `Common::WaitForFlag(m_is_interrupted` 1, `bool WaitForFlag(const std::atomic<u32>&` 1
    (common/thread.h), `MarkDirty(copy_location)` 1, `conf.spill_to_free_gpr,
    conf.scratch_copy_to_free_reg);` 1; `FrameGenType::AmdFsr` in FrameGen::Process's early return;
    `FrameGenCopyUsage()` 2 (vk_ocarina_camera_motion.cpp).
- Whole-file checks: `git diff 65745c243f -- <texture_pass.cpp, passes.h,
  global_memory_to_storage_buffer_pass.cpp, shader_handle_program.h, texture_handles.cpp,
  host_translate_info.h>` empty; `git diff 5fc428bdc -- src/video_core/renderer_vulkan/vk_fsr4_upscaler.*`
  empty.

Checkpoint B, after follow-up (a):
- Cloud clang/Ninja build with ENABLE_WERROR (as CI) of common, core, dynarmic, video_core,
  input_common, frontend_common, qt_common, yuzu and tests.
- MinGW clang syntax check of every changed C/C++ file with the Windows CI flags (TUNE_FLAGS
  `-march=x86-64-v3 -mtune=arrowlake -ffp-contract=off`, -Werror set), as merge 2 did. Expected
  failures only where this environment lacks a header (OpenSSL, nxformat, a fixture directory).
  Hand-review the `_WIN32` code Linux can't see: crash_handler.cpp, memory_detect.cpp GetHostRam,
  physical_core.cpp's wait path, hybrid_cpu, main.cpp after the crash-writer removal.
- `python3 -I lichii-tuned/check_patch_warnings.py <tree> --base 65745c243f`.
- Unit tests in an isolated HOME/XDG_*: all pass but the clock-trace case without its variable
  (tests run #9 on r58: 1282 of 1283, 1 skipped as designed). Watch:
  - video_core: buffer_cache_ports (rewritten kind test), fast_draw, maxwell_fast_method,
    uniform_stream_reuse, eviction_age, nx2_semaphore_waiters, semaphore_waiters,
    command_snapshot_suspensions, query_stream, memory_manager_runs, memory_tracker,
    syncpoint_actions, list_macro_trace, back_idle_trace, nx2_gpu_mapping, texture_handles
    (25 cases), ssa_fixed_variables, mesh_fallback, ampere_lowering/gradient/f2i/i2f/ldc,
    shader_cache_v2_programs (#1505), shader_precache, precache_journal,
    global_memory_coherence, storage_buffer_pointer_tracking, fast_fusion_byte_store,
    frame_gen_choice;
  - common: poll_window, event (add a u32 WaitForFlag case), first_run_config,
    recommended_settings, clock_trace, memory_detect, nextendo;
  - core: nx2_thread_stats, nx2_frame_interval, key_derivation, nx_archive, file_sys_parsers,
    local_redirect; input_common: rumble_mapping.

## 5. Follow-ups (separate commits)

On the merge branch, step 1:
- (a) Cherry-pick #1569 `c169856090` (renames #1536's `values` parameter, recommended_settings.cpp/.h):
  without it our -Werror=shadow build fails. Identical to upstream, so step 2 merges it cleanly.
- (b) LogTestSwitches wording (video_core.cpp:96-109): EDEN_NX2_IDLE_SPIN_US is the idle poll's
  limit; LICHII_UMWAIT_C02 no longer touches the idle poll (#1373 polls in C0.2 itself).
- (c) HANDOFF: the merge, the decisions above, what moved upstream.

After step 2 (with the owner's post-merge cleanup, CLAUDE.md 2026-11-13: most launchers go):
- README-tuned: :78 (LICHII_FG_PACING), :192-193 (crash during the preparation: the next launch
  builds no cached pipelines, the one after resumes, upstream #999 and #1505), :199-201 (crash
  reports: a `Host crash:` line in %APPDATA%\lichii\log\eden_log.txt and eden_crash.dmp beside
  it), :247-263 (#1161's pieces are upstream's, no switches), :364 and :464-467
  (verify-ports-off), :394 (ab-5), :414 (ab-15), :428 (ab-23), :451-459 (ab-40/41: the idle poll
  is upstream's adaptive C0.2 poll; IDLE_SPIN_US its limit).
- dist/nx2_switches.txt:30 `EDEN_NX2_CRASH_DUMP=1` goes.
- Launchers whose switches are gone: delete ab-29, ab-32, ab-36; ab-23 (EXP36), ab-15
  (IDLE_PAUSE), verify-ports-off (only EDEN_NX2_TEST_1216=0 left), ab-1 and ab-5
  (LICHII_WAITPKG_SPIN=0 no longer stops UMWAIT in the idle poll), ab-40/41's rems: keep, fix or
  drop in the cleanup.
- lichii-tuned/symbolize_crash.py: read the `Host crash:` line (module+offset stack) and
  eden_crash.dmp; it only knows lichii-crash-<pid>.txt. lichii_log_summary.py needs nothing (it
  parses none of the removed lines; checked), optionally the new 'GPU thread waits for suspended
  channels in 10 s' line.
- tuned: no patch for step 1 (a pushed patch builds CI). After step 2: merge nx2 `c169856090`,
  regenerate lichii-tuned.patch, `git apply --check` on a clean `c169856090`,
  check_patch_warnings.py's default base and its SKIP regex (`src/(yuzu|...)` -> lichii),
  lichii-tuned.yml `--target yuzu` -> `lichii` and `-DYUZU_*` -> `-DLICHII_*`; push once with the
  docs.
- Step-2 work that touches step 1's resolutions: #1521's NoteSync calls placed in our acquire path
  (puller.cpp) and gpu_thread.cpp; verify-by-kind after #1521; our YUZU_* names (section 1).

After the owner's first session on the merged build:
- 'Uniform keeps in 600 frames' (EDEN_NX2_GPU_PROFILE=1) against r58. #1441 ends keeps at every
  report (`Nx2FastDraw::NoteSync()` in RasterizerVulkan::Query, wt vk_rasterizer.cpp:3013), fake
  occlusion included, and Ocarina reports per draw. If keeps collapse, measure first, then offer
  upstream a NoteSync that skips reports nothing waits on.
- Placement of #1520's 'GPUSnapshot' thread (High priority, spins 2048 pauses before sleeping) on
  the hybrid 255H; our placement doesn't know it.
- Whether #1306's 1 GiB step should start at the retention limit (#1435), from the pot-room and
  guard-house runs with EDEN_NX2_TC_LOG=1 when the owner does the texture-cache session.
- A verify session (as verify-r55, without the 1161 switches): FAST_DRAW_VERIFY 0 mismatches
  vertex input included, UBO_KEEP_VERIFY by kind, LICHII_BANK_CHECK=2 'missed' 0, READ_CHECK 0
  bytes. Optional, like the tests the owner put off.

To offer upstream (ask the owner before any outward action):
- `3880800d0`: a failed graphics pipeline keeps no stages on eviction (#1337 rebuilds it with no
  shaders); still missing at `c169856090`.
- The keeps-per-report numbers above, if they show a loss.
- A guarded re-install of #1453's handler after a launch, only if a launch is seen replacing the
  filter. A plain second InstallCrashHandler would make previous_filter point at itself
  (crash_handler.cpp:161) and recurse.

## 6. What changes for the owner

The owner gets the step-2 build (build 171 on top), not step 1. From step 1:

- Ocarina's image:
  - shadow maps from light tables read through a storage buffer at binding 32 or above (r58 can
    read a garbage base there; #1485's u64 mask), and dual-vertex programs read their own
    textures (#1485);
  - light tables of records captured while the light pointer was null no longer sample a random
    render target ("old caches boiled"; #1523);
  - one-frame white flashes from stale material constants across Ocarina's four channels (#1459);
  - a later write on a page with GPU data no longer lost (#1460).
- Speed (upstream's measurements, not ours yet):
  - command lists copied on their own thread: at Kokiri water 62.4 -> 63.8 fps, the back's wait
    for the front 92 -> 54 ms/s (#1520);
  - the GPU thread waits in hardware or sleeps instead of polling waiting channels (#1376), and
    idle guest cores wait in C0.2 with a poll that shrinks when it doesn't pay (#1373): less
    spinning on the power-limited 255H, which option 3 showed turns into clock for busy threads;
  - fewer page-protection calls (#1436, #1518), cheaper reports and fences (#1515, #1516: the
    fencing thread wakes only when it waits, ~2,400 fences/s at the water), constant-buffer data
    written straight to the bound buffer (#1441), 19-23% less SSA work per new shader (#1468),
    less JIT compile time on first visits (#1517), guest reads that skip the cache locks more
    often (#1479).
- Stability: the zero-page trim can't end a game when Windows runs out of commit (#1451); long
  sessions don't run out of SMMU space (#1458, #1528, #1532: invisible geometry); a task shader
  that culls everything no longer risks a GPU hang on native mesh shaders (#1492); stopping a game
  no longer logs a Userspace PANIC (#1450).
- Shader loading: no new preparation ('Precompiled pipelines: ... was prepared for this driver
  already'). The first boot is slow as with every build (AMD's cache starts cold per exe) and
  re-lowers the v2 records once ('Shader cache v2: N programs of another build lowered again', a
  few seconds). A few 'binds a pointer as a texture handle; capturing it again' lines are expected.
  After a crash or kill during the loading screen, the next launch builds no cached pipelines
  (log line) and doesn't prepare; the one after resumes (owner decision 2). Running official
  Lichii on the same game at the same time makes the second one build nothing at boot.
- Settings and UI:
  - FSR 4 and AMD frame generation as in r58. The frame generation list stays LSFG, Optical Flow,
    AMD FSR (DLSS-G isn't in our MinGW build). The setting keeps its stored value (decision 1).
  - Help > Apply Recommended Settings also resets per-game overrides of the table's settings
    (backed up, Restore brings them back); nothing changes without clicking (#1536).
  - New: a Nextendo sign-in box and an opt-in host redirect in Configure > Network (online play
    stays off); two unchecked Age of Imprisonment options; HD haptics for a DualSense on USB;
    two-motor rumble from both sides; a RAM warning below 16 GB (not on 32 GB).
- Logs and files:
  - a crash writes a `Host crash:` line into %APPDATA%\lichii\log\eden_log.txt and eden_crash.dmp
    next to it, always on; no more crash_dumps\lichii-crash-<pid>.txt/.dmp;
  - the 'TEST switches in effect' line loses the #1161 entries; the boot log loses
    'LICHII_BINDLESS_TABLE'; 'Handle table:' lines need EDEN_NX2_HANDLE_TABLE_NOTES=1;
  - gone: the FG pacing log lines, 'Fast draw #1161 per second'; new opt-in diagnostics:
    EDEN_NX2_IDLE_STATS, EDEN_NX2_GUEST_THREAD_STATS, EDEN_NX2_BACK_IDLE_TRACE,
    EDEN_NX2_LIST_MACRO_TRACE, EDEN_NX2_BUFFER_TRACE, EDEN_NX2_PRESENT_FENCE_TRACE,
    EDEN_NX2_DRAW_REUSE_PROBE; the GPU profile adds 'GPU thread waits for suspended channels in
    10 s' and 'Acquires:' lines (fields only upstream's acquire path fills read 0);
  - launchers ab-29, ab-32, ab-36 and ab-23 no longer change anything; ab-15, ab-1, ab-5, ab-40
    and ab-41 no longer change the idle poll (cleanup follows).
- Risks on the RX 9070 XT:
  - VRAM: images used in the last 150 frames stay until the device runs short (#1435); with our
    #1306 that means up to ~1 GiB more in use before the 3-frame age takes over. The Hyrule Castle
    pots were #1306's out-of-memory crash.
  - Uniform keeps end at every report (#1441 with Ocarina's per-draw reports): if keeps fall
    below r58's, the back gets slower there (measure).
  - The new 'GPUSnapshot' thread competes for the power-limited CPU (#1520); C0.2 idle wakes are a
    little slower than C0.1 (ab-40 stayed within the noise).
  - JIT: #1517 with our #717 scratch copy needs the MarkDirty line; a new crash or glitch: A/B
    with ab-16.
  - If Ocarina is a NUP or tree .nx with an installed update, the update's ExeFS is no longer
    applied (#1504) and our build-id patches would miss: the boot log says 'ExeFS: Ignoring
    installed update'.
  - #1441's keep gaps and #1462's SPIR-V routing change exist only in step 1; step 2 closes them
    (#1521, #1539).
  - HDR: AMD frame generation stays off under HDR until r59's patch is in.
