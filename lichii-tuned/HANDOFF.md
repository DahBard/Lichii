# Lichii tuned: project handoff

State of the work on 2026-11-07 (end of the second session), for the next Claude session.

## Goal

Make [Lichii](https://github.com/zortos293/Lichii) (Switch / Switch 2 emulator; Eden / Eglantine
fork) run as fast and as cleanly as possible on the owner's setup, and iterate with the owner:
build, the owner tests and sends `eden_log.txt`, analyze, next build.

**Primary goal (set by the owner on 2026-11-07):** a constant 60 fps with the game rendering
2560x1440 and FSR 4 upscaling to 4K. Every round should move toward it, by optimizing and by
researching other Switch 2 emulator forks for what Lichii lacks, especially on AMD. Smaller issues
(movement speed, single glitches) come second.

- Windows 11, Intel Core Ultra 7 255H laptop (Arrow Lake-H: 6 P-cores, 8 E-cores on the shared
  L3, 2 low-power E-cores on the SoC tile, no SMT), AMD Radeon RX 9070 XT over OCuLink (PCIe 4.0
  x4), monitor on the 9070 XT. AMD driver 2.0.416, Vulkan 1.4.359.
- Game: The Legend of Zelda: Ocarina of Time (Switch 2), program id `0400486024DEC000`, resolution
  1x. Docked the game renders 2560x1440; handheld it renders 1920x1080 inside the 2560x1440
  layout. The "1X (720p/1080p)" scale label is Switch 1 wording.
- The owner wants the recommended speed settings as defaults, and asked for shaders to be
  compiled before playing.
- The log is `%APPDATA%\eden\log\eden_log.txt` (the previous launch's is `eden_log.txt.old.txt`).

## How builds are made

- Branch `tuned` of DahBard/Lichii = upstream commit `588bfa6` + `.github/workflows/lichii-tuned.yml`
  + this folder. The workflow applies `lichii-tuned/lichii-tuned.patch` and builds on every push to
  `tuned` that touches `lichii-tuned/**` or the workflow (a commit message with `[skip ci]` doesn't).
- Variants (windows-2022): `pgo` (MSYS2 UCRT64 clang, repo's `eglantine.profdata`,
  `-march=x86-64-v3 -mtune=arrowlake -ffp-contract=off`), `pgo-lto` (+ ThinLTO). Commented out
  until needed: `msvc` (built like the official MSVC download, to A/B a glitch that only the pgo
  builds show; off since 2026-11-07) and `pgo-train` (instrumented, writes
  `lichii-pgo-*.profraw`). Profiles put in `lichii-tuned/pgo/` (raw or zipped) are merged 4:1
  with Lichii's. Artifacts: `lichii-tuned-r<run>-<variant>`. Each variant queues only behind the
  same variant of the previous run. ~40-50 min with warm caches.
- Source of the patch: branch `tuned4` of DahBard/Lichii (115 commits over 588bfa6; pushing it
  doesn't trigger builds). `lichii-tuned/tuned4.bundle` holds the same history as a backup
  (`git fetch lichii-tuned/tuned4.bundle tuned4:tuned4` in a clone that has 588bfa6). The patch
  is `git diff --binary 588bfa6 tuned4`; check it with `git apply --check` on a clean 588bfa6
  checkout before committing it.
- Before turning `msvc` on, run `python3 -I lichii-tuned/check_patch_warnings.py <tuned4 checkout>`.
  It compiles the changed files with clang and GCC and reports warnings on added lines that the msvc
  variant treats as errors (shadowing, signed/unsigned compares, unused locals and captures,
  unhandled enumerators, video_core conversions); a changed header is compiled through two files
  that include it. The pgo builds don't catch them: `common/x64/xbyak.h` turns -Wshadow and
  -Wconversion off for the rest of every file that includes it. Qt, Windows-only and test files need
  a look by hand. Locally `vk_frame_dump.cpp` (needs stb) and `nvhost_ctrl_gpu.cpp` (the cloud's
  clang crashes on it at any commit) fail; ignore them.
- The cloud session's network blocks GitHub archive downloads (so a full CMake configure fails)
  and the Actions log host; git clones work. Read job logs with the GitHub MCP `get_job_logs`
  tool: a large `tail_lines` saves the log to a file you can grep.

## What the tuned build contains (tuned4)

Upstream nx2 up to `09b78bd2df`, which is build 93, the newest published build (builds 89-92 were
never published), so its AMD fixes (#273, #295) are in. Round 10 cherry-picked ten of the thirteen
nx2 commits since then (nx2 at `9fa4a71`): #413, #414, #410, #185, #398, #241, #197, #149, #203
cleanly and #412 (GPU back thread bookkeeping) with `UpdateDynamicStates` kept on our `const
GraphicsPipeline*` signature. Not taken (conflicts): #397 (buffer downloads), #215 (query results
before macro DMA reads; touches the split front like #345) and #232 (hybrid CPU threads; overlaps
`src/common/windows/hybrid_cpu.*`). Round 11 added open pull requests from 2026-11-07 for the
CPU-bound frame: #481 (RangeSet as a sorted array, ~5% of the back thread), #478 (page walk reuse,
~4%), #488 (same-value instance stores dropped on the split front; its sink flag kept beside #345's
FrontNoteWrites), #463 (no uploads of stale guest memory; MarkStale before Evict and ClearStaleImage
in our batched collector's FlushPending), #448 (AMD: uneven-mip ASTC stays RGBA8, dark terrain
lines), #421 (OoT foliage tables). Not taken: #446 (our #345 loop seldom sleeps: PendingSleep 2.2
ms/s, present fence wait 0), #465, #235, #231, #243, #434. Round 12 (after the fork survey in
research-forks-amd.md section 7) added 25 nx2 commits from `9fa4a71..d5494116` that apply cleanly:
stability #310, #485, #427, #495, #435, #444, #438, #439, #365, #330, #331, #436, #333, performance
#250, #428, #404, pipeline cache #192, #550, shader fixes #279, #311, #429, #433, #425, #189, #199;
and from open #573 the AMD 512-draw submission budget, compute e0a992208d87eeb8 unoptimized and
parallel unswizzles. Not taken yet: #432 (cache version bump), HMMA cooperative matrices, the
clang-cl PGO profile, #440, #178, #245, #351, #186 (conflicts). Plus open upstream pull requests at
their heads as of 2026-11-07 00:00 UTC:

- #319 "Ocarina of Time anti-aliasing: No sharpening" bypasses the game's tensor AA network
  (AMD runs its HMMA math in software); #300 builds those HMMA pipelines off the wait path on AMD.
- #337 white flashes at GPU accuracy Low, #292 fog haze plus (round 4) the same 256 MiB cap for
  graphics table pointers (Kokiri Forest water and foliage turned black after an area change on
  an RX 7800 XT), #329 leaf holes, #321 held draws build their pipeline inline, #299 Dynamic FPS
  down to 10 fps, #259 opt-in preflush (`EDEN_NX2_PREFLUSH=1`).
- #345 (9 commits) precise macro DMA reads and cheaper semaphore acquires on the split GPU front
  (its author measured ~48% of the front draining the back in Ocarina). Round 4 added its four
  review fixes: no more learned acquires (r9 skipped acquires that had succeeded before, which
  could run the commands after an unmet one), semaphore fast paths only when no queued or
  undownloaded write can change the value, command pages rechecked after every writer wait,
  write tracking keyed by device address. Large; if a round is unstable, drop it first.
- #346 denormal controls, #353 render target write mask (violet halos on AMD), #354 bindless
  handle tracking for the clustered lighting shadow lookups (green tint growing on AMD; round 4
  added texture array sampler limits, arrays only with nonuniform indexing, and dual vertex
  program fixes), #376 texture cache collector downloads with one GPU wait (round 4 added its
  review fix: r9 could flush a full batch inside Cleanup and delete images the LRU walk still
  pointed to), #378 source part (`EDEN_AMD_FP16=1`).

Own changes:

- Hybrid CPU placement on Windows (`src/common/windows/hybrid_cpu.*`, `LICHII_CPU_POLICY=
  off|soft|auto|strict`, default auto): GPU/GPUBack/VulkanWorker on P-cores, guest cores and
  pipeline builders on P+E cores, HostTiming on E-cores, nothing on the low-power island.
- UMWAIT (WAITPKG) instead of PAUSE spinning in the split GPU threads and idle guest cores
  (`src/common/spin_wait.*`, `LICHII_WAITPKG_SPIN=0` off; clang/GCC builds only).
- Texture cache thresholds scaled to 60% of the VRAM budget (official: 5 GiB critical).
- Recommended settings seeded once (`src/frontend_common/config.cpp`, marker
  `lichii_tuned_defaults=1`): GPU accuracy Low, async shaders, reactive flushing off, async
  presentation, GPU video decoding, Ocarina AA No sharpening.
- Built pipelines are kept (`LICHII_KEEP_PIPELINES`, default 4096; 0 = upstream) and every cached
  pipeline is built at boot within that budget. Upstream destroyed pipelines unused for 9000
  frames: three minutes on the title screen threw away 1,076 of 1,236 and gameplay froze 2.3 s.
- `EDEN_NX2_FRAMES_AHEAD=2`: present waits for the previous frame only, even with the GPU behind.
- Profiling: Finish (full GPU wait) call sites in the 10 s scheduler report (`Finish sites:`).
- PGO training support (`src/common/pgo_training.*`), ThinLTO fallback for MSYS2 clang
  (`externals/cmake-modules/UseLTO.cmake`).
- Package extras in `dist/lichii-tuned/`: `README-tuned.txt`, `perf-trace.bat`
  (`EDEN_NX2_GPU_PROFILE=1`, `EDEN_DKB_FRAME_TRACE=1`) and A/B launchers ab-1 to ab-11.
- MSVC build fix (round 4): four declarations from #319, #345 and #376 that hid a class member
  or a local are renamed.
- Round 5's `EDEN_AMD_FRAGMENT_SHUFFLE=1` (native fragment shuffles on AMD; since #273 they run
  as broadcast loops because a driver crashed on one Ocarina shader, Eglantine #618) was dropped
  in round 6: black screen on this card, and the heavy passes don't shuffle.
- Dynamic FPS (round 6, default since round 7): `LICHII_DYNFPS_SMOOTHING=<1|2|4|8>` (default 1)
  and `LICHII_DYNFPS_MIN_FPS=<5-60>` rewrite two instructions of the routine after it is applied:
  the average of the measured frame time (`add x12, x12, x14, asr #N` at +0x44; Dark's mod
  averages 8 frames) and the cap (`mov x14, #10` at +0x24; Dark's mod: 20). Each frame's own time
  felt smoothest to the owner; the 20 fps floor felt worse. Without the mod the game steps 1/60 s
  per frame (slow motion).
- JIT (round 7, x86-64): DMB LD/ST emit nothing, 8-64-bit load-acquires and inline exclusive
  loads are plain MOVs instead of LOCK XADD (STLR keeps XCHG), and CNTPCT_EL0 is computed inline
  as high64(lfence;rdtsc * WallClock::cntpct_rdtsc_factor) with GetCNTPCT's polling pause kept
  via `A64JitState::last_cntpct`. Idea from upstream #161 (d51b04d8), whose inline counter used
  the Switch 1 rate. `LICHII_JIT_TSO=0` / `LICHII_JIT_INLINE_CNTPCT=0` restore the old code.
- GPU trace (round 7): mesh, indirect count and byte count draws are timed too; mesh draws show
  as `m1:<task/mesh>+s5:<fragment>` in `DKB gpu_work` and as `mesh_draws=` per sampled submission.
- GPU trace (round 8): draws are timed in one sampled frame of four (`LICHII_TRACE_DRAWS_EVERY`,
  default 4, 0 = never), because their timestamps lengthen the frame they measure; `gpu_frame` lines
  say `draws_timed=0/1`, and the summary script reports the untimed frames as the frame's GPU time.
  Each sampled submission records when its upload command buffer finished (`gpu_upload_sum_ms`,
  `upload_ms`), the 10 s scheduler report adds `Staging traffic per second` (stream buffer, host
  memory, downloads), and boot logs `Memory heaps:`, `Memory types:` and the stream buffer's memory
  type (`Stream buffer:`).
- `LICHII_WFI_DEFER=1` (round 8, off; launcher dropped in round 10, no gain in r14): a guest wait
  for idle inside a render pass is deferred like a fragment barrier, as on MoltenVK, and its full
  barrier recorded after the render pass ends.
- Occlusion reports answered without host queries (round 9 switch, default for Ocarina since round
  10, `query_cache.h` FakeOcclusionSamples): each ZPass sample count report is written at once with
  4096 samples, like a counter Lichii doesn't support (which reads 1), and no host occlusion query
  begins, so the game sees everything as visible. `LICHII_FAKE_OCCLUSION=<samples>` sets it for any
  title, `=0` runs the queries (`trace-real-occlusion.bat`).
- Ported from Shakoquo/Lichii-sharkiix-version (round 10): on AMD a query result copy waits only for
  the newest query (`vk_query_cache.cpp`, ~5 ms a frame with real queries; from its 3.1 tree, also
  in chuppatricky's AMD test build v13), and samplers read without a depth reference drop the
  comparison (`vk_texture_cache.cpp`, from 28f594ac: AMD returned the 0/1 compare result, a black
  Ocarina forest fog). Not ported: its always-refreshed blend state (costs back-thread time per
  draw, no symptom here).
- Launchers with the perf-trace variables: `trace-preflush.bat`, `trace-fp16.bat`,
  `trace-flush4096.bat` (`EDEN_NX2_FLUSH_DRAWS=4096`), `trace-old-jit.bat` (round 7's JIT off),
  `trace-real-occlusion.bat` (round 10), `trace-strict.bat` (round 11).

## Test results so far

- Round 1: a bit faster than official build 93 but more glitches; 4K under 5 fps, 1080p ~20 fps
  with 0-fps hitches every ~5 s, music cutting out, brightening artifacts.
- Round 2b (r7-pgo), 44 min log: the bright flashing is gone (owner confirmed).
  - Boot built ~1,229 cached pipelines in ~42 s. Title movie/menus ~53 fps, GPU-bound (~18.7 ms):
    the AA network still runs until the first tonemap pass.
  - Hitches: draws held 15.7 s for unbuilt pipelines, mostly the evictions above
    (288 s: 349 rebuilt, 2.25 s held; 1,352 s: 159 rebuilt, 2.5 s held). Fixed in round 3b.
  - One gameplay area ran ~17 fps for 14 minutes (57.5 ms frames), another ~31 fps. Not GPU-bound:
    the game never waited on the present fence. The split GPU threads wait on each other: front
    "split drain" ~415 ms/s (semaphore acquires ~300, macro DMA reads ~105, list writers ~102),
    back "split wait" ~470 ms/s. #345 targets this.
  - GPU work ~40 ms per frame in the 17 fps area; one 49-draw submission takes ~17.5 ms (shaders
    s1:b682ea2c148fe1e8 s5:046ea18d9f62eba6 s1:d7ec5ba11453e891 s5:b7bd625f79e3ced1
    s1:89b176edbafc8868 s1:60afd8140ce31738; not covered by the per-draw timing). 256 pipelines
    log "Patch topology not used with tessellation".
  - 28-61 full GPU waits (Finish) a second costing 140-170 ms/s; sites are logged from round 3b.
  - Guest cores 4 and 5 use ~40-43 ms of CPU per frame in the 17 fps area.
- Run #9 (round 3b): `pgo` and `pgo-lto` built (artifacts `lichii-tuned-r9-pgo`,
  `lichii-tuned-r9-pgo-lto`); `msvc` failed at 914/1880 on C4458 (#319's `state` in
  kepler_compute.cpp) and C4456/C4457 (#345's `[source, count]` in maxwell_3d.cpp), fixed in
  round 4. Rounds 3 and 3b are untested.
- Run #10 (round 4): `pgo` and `pgo-lto` built (artifacts `lichii-tuned-r10-pgo`,
  `lichii-tuned-r10-pgo-lto`); `msvc` was cancelled 10 minutes into its build, with no error
  before that.
- r10-pgo-lto, perf-trace, ~4.5 min in Kokiri village right after the Deku Tree, first visit
  with this pipeline cache. Docked: the game renders 2560x1440 (round 2b was handheld, a
  1920x1080 crop), so the numbers don't compare directly.
  - No crash: the 2010-0212 panic at the end comes from stopping emulation (force stop).
  - Gameplay median 18.4 fps (p10 8.6, p90 40.2). GPU busy per frame median 33.7 ms, p90 66 ms:
    the ~10 fps stretches are GPU-bound. Two passes dominate: s1:89b176edbafc8868
    s5:046ea18d9f62eba6 s1:d7ec5ba11453e891 s5:b7bd625f79e3ced1 at 25-33 ms, sometimes with only
    4 draws (heavy full-screen fragment work; round 2b's 17.5 ms at 1080p scaled by the pixel
    count), and a tessellated pass with s3:3019c6c744e7ecda at 18-26 ms (79-92 draws).
  - Boot built 1,322 cached pipelines. 341 new ones held draws for 19.7 s in total and caused
    all five 1.6-2.5 s freezes.
  - Read traps: 27-161 guest CPU reads of GPU-written pages per second (41% on two pages,
    0x12fdfc000 and 0x44a14000) and 18-78 full GPU waits per second at buffer_cache.h:407
    (`DownloadMemory`), 64-200 ms/s.
  - Split threads: front split drain ~270 and writer wait ~230 ms/s, back split wait ~460 ms/s.
    Never blocked on the present fence. The AA bypass engaged on the first gameplay frame.
- r10-pgo-lto again, second launch, same area and mode: boot built 1,663 cached pipelines; 11
  new ones held draws 0.8 s (19.7 s before), one 2.2 s freeze. Gameplay median 13.1 fps (p10
  9.2, p90 30.8), GPU busy per frame median 53.3 ms (62% of frames over 40 ms): more time in
  the heavy views. Frame rate follows GPU time window by window. The "1X (720p/1080p)" label is
  the resolution scale's Switch 1 wording; the game composites 2560x1440 docked.
- Run #11 (round 5): `pgo` and `pgo-lto` built (artifacts `lichii-tuned-r11-pgo`,
  `lichii-tuned-r11-pgo-lto`). r11-pgo-lto, docked 1440p, same area, 1-1.5 min each:
  - `trace-native-shuffle.bat`: music but a black screen at 0 fps (no log). Native AMD fragment
    shuffles don't work on this driver.
  - The shuffle lines name ~185 fragment shaders, but not the heavy passes' (s5:046ea18d9f62eba6,
    s5:b7bd625f79e3ced1, s5:292b3848bd8c1dbf, s5:f16638b51d8e93ac): the loop isn't their cost.
  - `trace-fp16.bat`: one wrong color on the start screen; GPU per frame in heavy views 37-74 ms
    (baseline 40-77): no clear gain.
  - `trace-preflush.bat`: no flicker seen. CPU reads of GPU-written memory 73-108/s -> 10/s,
    buffer_cache.h:407 waits 27-52/s -> 4/s (1.7 ms/s). Fps unchanged (median 13.2, GPU 55 ms per
    frame): heavy views are GPU-bound, and light views stay at 21-34 fps with the front waiting
    on the game ("pending sleep" 220-500 ms/s; guest cores 4-5 used 35-46 ms per frame in r10).
- The owner sees Link and NPCs move too fast in every run. The guest counter is right (31.25 MHz
  reported and delivered), speed limit 100%, standard CPU settings. Suspect: the built-in Dynamic
  FPS mod, which replaces the game's time step with the smoothed measured frame time, capped at
  100 ms here (#299) and 50 ms upstream.
- r11 with the mod off, `LICHII_CPU_POLICY=strict` and the perf-trace variables: super slow
  motion, so the mod is needed. Guest core 4 still used 45 ms per frame on P-cores only (fps
  ~15, GPU 34-72 ms per frame): strict placement doesn't help. The mod's 8-frame average keeps
  a long step after slow frames, which runs the game up to 2-3x real time for ~0.5 s each time
  the frame rate recovers (more with #299's 100 ms cap); round 6's switches test that.
- Run #12 (round 6) built `pgo` and `pgo-lto`. Each frame's own time felt smoothest; Dark's 20 fps
  floor slowed down and sped up constantly. That run was handheld: the game rendered 1920x1080, yet
  GPU time per frame stayed at 35-67 ms (median 36 in gameplay) against 34-77 ms docked at 1440p,
  and guest core 4 used 41.7 ms per frame. The few-draw full-screen pass fell to 12 ms (~30 ms at
  1440p), but the rest of the GPU time is spread over ~40 submissions per frame at ~1 ms each, so it
  is mostly not pixel cost: per-submission and render pass overhead (1,400-2,500 render pass ends
  per second) and untimed mesh draws are the suspects.
- Run #13 (round 7) built `pgo` and `pgo-lto` (artifacts `lichii-tuned-r13-pgo`,
  `lichii-tuned-r13-pgo-lto`).
- r13 with Lichii's resolution at 3x (`trace-old-jit.bat`, `trace-flush4096.bat`, docked): 3x
  renders 7680x4320 (the docked base is 2560x1440), past the 9 GiB VRAM cap (an 8K image left
  unbacked, an 18.9 s freeze with the GPU back thread at 904 ms/s in clears); gameplay 2-16 fps. FSR
  1 looked worse than ScaleForce there because it was scaling 8K down to 4K. The same full-screen
  draws took 20-80x longer in some sampled frames: s1:621c4755238870a8+s5:3ddfb1d7a854c849 0.7 ms or
  15-54 ms, s1:ee1f5e9a94e748d4+s5:1d78d4e5c4e9b0e4 190-330 ms,
  m1:b41e5eadb0568672+s5:c72b6ea746c00433 1 ms or 8-21 ms; those frames match the 1-5 fps windows.
  The owner sees bright areas run much worse than shadowy ones.
- r13 `perf-trace.bat` at 1x, docked: gameplay median 13.9 fps (earlier docked runs 13.1-18.2
  depending on the area). Guest core 4 used 703 ms/s and core 5 625 ms/s against 750-828 and 688-734
  before: round 7's JIT cuts guest CPU time, but the frame rate is limited elsewhere. Sampled
  gameplay frames: span 30-120 ms, submissions 14-84 ms, yet their 800-3,700 timed draws add up to
  5-15 ms; slow frames have 3-4x the draws of fast ones (~800 at 29 ms vs 2,000-3,700 at 60-120 ms).
  No heavy full-screen draw appeared at 1x. ~46 submissions and ~145 render passes a frame; render
  pass ends per second: vk_rasterizer.cpp:1881 (WaitForIdle) 520-640, vk_query_cache.cpp:1640
  340-410, vk_buffer_cache.cpp:545 (uploads that can't move to the upload command buffer) 165-250,
  vk_graphics_pipeline.cpp:781 (new framebuffer) 175-190, vk_rasterizer.cpp:1297 (clears) 140-160,
  vk_query_cache.cpp:270 115-125, vk_rasterizer.cpp:2343 (submission budget) 110-140. Read traps:
  38.7 full GPU waits a second at buffer_cache.h:407, 139 ms/s. No image fell back to host memory at
  1x.
- The owner's laptop BIOS forces Resizable BAR off; GPU-Z: "PCIe x16 5.0 @ x4 4.0". The stream
  buffer (128 MiB) prefers device-local host-visible memory, which is then the 256 MiB BAR window
  (round 8 logs where it lands: `Stream buffer:`); uploads over 1/16 of it and all downloads go
  through host memory across the x4 link.
- Run #14 (round 8) built `pgo` and `pgo-lto` (artifacts `lichii-tuned-r14-pgo`,
  `lichii-tuned-r14-pgo-lto`).
- r14 `trace-wfi-defer.bat` at 1x, docked, ScaleForce (no artifacts with it or with
  `perf-trace.bat`; only this log was sent): gameplay median 14.2 fps. The stream buffer is in
  memory type 2, device-local (heaps: 16,048 MiB device-local, 18,347 MiB host, 256 MiB BAR).
  Staging traffic ~95 MiB/s through the stream buffer, 5 through host memory, 15 downloaded (~7 MB a
  frame), and upload command buffers take ~0 ms of GPU time: the x4 link is not the limit. Untimed
  gameplay frames: GPU busy median 47.6 ms a frame, 64% of the frame time (27-99%), light views ~15
  ms. In timed frames the 900-3,000 draws and dispatches are 13-18% of the busy time (8-10 ms of
  50-80 ms); the rest is between draws. Deferring waits for idle removed vk_rasterizer.cpp:1881, but
  the render passes ended at the next clear (vk_rasterizer.cpp:1297, 283-416/s) or query instead
  (1,729-2,282 render passes a second): no fps change, so drop the switch unless a later round needs
  it. The game reports ~20,000 occlusion counts a second (~1,400 a frame), reads them on the CPU to
  set render enable (~19,000 `DKB_CONDITION ... cpu` a second), and those reads cause the 33.7 full
  GPU waits a second (163 ms/s) at buffer_cache.h:407.
- Run #15 (round 9) built `pgo` and `pgo-lto` (artifacts `lichii-tuned-r15-pgo`,
  `lichii-tuned-r15-pgo-lto`).
- r15 `trace-fake-occlusion.bat` at 1x, docked (the owner's second log was the old r11 fp16 one;
  compared with r13 and r14 instead): gameplay median 14.5 -> 27.5 fps, p10 11.6 -> 17.9, p90 23.0
  -> 34.7; the owner: "finally feels markedly better", no flares or objects through walls. Untimed
  GPU busy 12.5 ms a frame (p90 18.6) against 47.6: the GPU is no longer the limit. Per frame the
  guest cores 4 and 5 use 29 and 28 ms (797 and 766 ms/s), the GPU back thread 28 ms (766 ms/s), the
  front 21 ms: those set the frame rate now.
- Run #16 (round 10) built `pgo` and `pgo-lto` (artifacts `lichii-tuned-r16-pgo`,
  `lichii-tuned-r16-pgo-lto`).
- r16 `perf-trace.bat` at 1x, docked, 113 s of gameplay: "the best to play so far, slightly smoother
  than r15". Median 24.7 fps (r15 27.5, other spots), p10 19.4 (17.9), p90 35.6. GPU busy 15.3 ms a
  frame. Guest cores 4 and 5 797 and 766 ms/s (32 and 31 ms a frame), the back thread 797 ms/s (33
  ms a frame, 54,600 draws a second): bind stage buffers 192 ms/s, other 110, method 114 (3.5 M
  register writes a second, 1.5 M unchanged), draw 89, compute 61. Front: writer wait 416, split
  drain 386, macro DMA flush waits 289, acquire waits 251 ms/s. Read-trap GPU waits 49 a second, 90
  ms/s.
- Run #17 (round 11) built `pgo` and `pgo-lto` (artifacts `lichii-tuned-r17-pgo`,
  `lichii-tuned-r17-pgo-lto`).
- Sharkiix (2026-11-07): Shakoquo/Lichii-sharkiix-version, AMD- and Ocarina-focused, tested on an RX
  9060 XT. No history of its own: v1/v2 are Lichii `99ac3d59` plus one commit (faster tensor
  emulation, semaphore acquire resumes, texture cache GC min age, the sampler and blend fixes), 3.0
  imports chuppatricky's AMD test build v13, 3.1/3.1.1 import a "build-11 (fix/ocarina-of-time)"
  tree and add graphics pipeline libraries on RDNA 3+ (fewer freezes on new pipelines). Its README
  rejects the ground mist (s5:c72b6ea746c00433) and the fog/light-shaft composite
  (s5:3ddfb1d7a854c849) because they draw a white veil there: the same passes that cost most in
  sunlit areas here, where #292 draws them right. Verdict: don't migrate (snapshot imports, more
  glitches, older base); port pieces. Other candidates in those trees: NaN-defined min/max, a
  compute-to-draw barrier, an atomics spin fix, AMD safe compile; combined forks justr4mzi/hortensia
  and jordenjacobsen-ai/Lichii-Frizti-v3-plus.
- Research (2026-11-07; author claims, not reproduced): PokeDinner (Franseven's Eglantine fork) is
  not an AMD performance fork and was merged into Eglantine (#149). Lichii issue #403 (RX 9070 XT,
  build 93): ~18 -> ~29 fps after log filter `*:Error`, high process priority and Xbox Game Bar
  capture off; our logs are small (~4,700 lines in 4 minutes), so the filter matters less here.
  Upstream #408 (opened 2026-11-07, after the snapshot) keys pipelines only by the vertex attributes
  a program reads (Ocarina's task and mesh programs had 191 keys for 31 shader sets), evicts
  pipelines only past a RAM budget and stops inlining the global memory helpers (compute builds of
  up to 7.6 s). Its author reports 60 fps (the vsync cap) at 2560x1440 on an RTX 3060 with the GPU
  ~65% busy, so the 9070 XT's 35-77 ms per frame comes from AMD or this eGPU link, not from the
  game's workload. FSR SDK 2.4.0 (October 2026) runs the FSR 4 ML upscaler on Vulkan through AMD's
  signed `amd_fidelityfx_loader_vk.dll` / `amd_fidelityfx_upscaler_vk.dll` (binary redistribution
  only), RX 6000 and newer. Like FSR 3.1 it needs depth, motion vectors and jitter, and Ocarina
  renders with neither jitter nor motion vectors (`docs/nx2/oot-network-bypass.md`). Open upstream
  #253 adds both for NVIDIA DLSS in place of the network (Halton jitter on the scene camera's
  viewport, camera motion vectors from depth), and the tree already runs AMD's FSR 3.1 in place of
  other games' DLSS (`vk_fsr3_upscaler.cpp`). Going from 1440p to 4K at that point also needs the
  resolution scaler to scale only the images after the network. The owner's settings use the
  ScaleForce window filter and FXAA; the window filters "FSR 1" and "FSR 1 Soft"
  (`ScalingFilter::Fsr3`: Lichii's own optical flow and accumulation, not AMD's FSR 3) can scale
  1440p to 4K today. No window filter is AMD's FSR 3.1: that only stands in for a game's own DLSS
  call, which Ocarina doesn't make. Details and links: `lichii-tuned/research-forks-amd.md`.

## Next steps

1. Run #18 (round 12; the owner had not tested r17 yet, r18 includes it), docked, 1x, ScaleForce,
   the same Kokiri spots: `perf-trace.bat`, `trace-strict.bat`, `trace-preflush.bat`; watch for
   wrong textures or flashes after area changes (#463) and for crashes. Then the big step: merge
   upstream nx2 (`d5494116` or newer) into tuned4: 27 files and 53 conflict hunks in a test merge;
   keep #345's split front and our hybrid placement or move to upstream's #232/#418 thread pinning.
   After that: Frizti v3's drain-free acquires (front waits 251 ms/s in r16) and DMA snapshots
   against #345's versions; measure #345's per-bind native bank memcmp with
   `EDEN_NX2_MME_DMA_STATS=1` (Firefly's upload replaces it); build with clang-cl to use upstream's
   Ocarina PGO profile; #432 with #591 so caches survive; HMMA cooperative matrices for the "Game"
   AA mode. For 4K: #253's jitter and motion vectors into the FSR 3.1 path, then FSR 4.
2. Summarize the new log with `python3 -I lichii-tuned/lichii_log_summary.py eden_log.txt`; check
   draws held for pipelines, the front's drain after #345, and the `Finish sites:` lines.
3. Then: remove the biggest Finish sources; look at the 17.5 ms submission (tessellation?);
   try `ab-8-skip-unbuilt-draws.bat` for first-time shaders and `ab-4-strict-placement.bat`;
   PGO training on Ocarina. If a glitch shows up that the official build doesn't have and the
   A/B launchers don't remove, turn `msvc` back on to tell the compiler from the source.
4. Each round, port new commits from the included pull requests (#354 was still changing late
   on 2026-11-06): `git fetch https://github.com/zortos293/Lichii refs/pull/<n>/head` and compare
   patch ids with tuned4. Per `AGENTS.md`, also check Eglantine (Djihads80/Eglantine).
