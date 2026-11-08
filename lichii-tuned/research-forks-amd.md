# Forks and PRs relevant to AMD / Ocarina performance (research, 2026-11-07)

Method: GitHub search API plus shallow git fetches of PokeDinner, Lichii nx2 and its forks, and FidelityFX SDK v2.4.0, in a cloud session. The Djihads80/Eglantine and zortos293/Lichii pull/issue endpoints are not readable here, only search results (title/body, no comments, no diffs of PRs). "Measured" = numbers the author gives with a method; none were reproduced by me.

## 0. Headline

1. PokeDinner is NOT an AMD-performance fork. It is Franseven's Eglantine fork (git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork, unreachable from here; GitHub mirrors Apollo422/PokeDinner and 969981/PokeDinner-a-Eglantine-Fork). 563 commits, last 2026-10-31. Eglantine PR #149 (closed) merged its master (c3ce9bcd5b) into Eglantine nx2, so its work is already in Lichii nx2, via Lichii's merges plus 79f284ea (#149). I found no AMD-specific GPU speed work in it. Its AMD items are a heap-budget fix and a TSC fix for AMD CPUs, both already in our tree (see 1).
2. Strongest actionable lead for the 25-33 ms full-screen pass / submit counts: nothing found names those shader hashes (searched both repos' issues and PRs). Leads are about CPU/sync overhead, not AMD GPU shader cost. Lichii issue #403 (RX 9070 XT + 9800X3D, Windows, build 93) went from ~18 fps to ~29 fps by only setting log filter `*:Error`, high process priority and Xbox Game Bar capture off (author's table, one run each). Check that our logs are not at `*:Info`.
3. CPU-side: our tree still has `mfence` for every DMB, a `lock xadd` for every ordered load (LDAR/LDAXR/LDXR), and a HostCall for CNTPCT (checked in src/dynarmic/.../a64_emit_x64.cpp and emit_x64_memory.h). 999sian's commit d51b04d8 changes all three (see 3). It is in open PR #161, which is not in our PR list. Round 7 ports the idea (see HANDOFF.md, JIT).

## 1. PokeDinner

Repos: Apollo422/PokeDinner (mirror, master fe1230f4, tags v1.0.0 etc., adds macOS scripts); kel6967/PokeDinner-AMD (branches amd-perf-patch 0c2d5c6f, master, pokedinner-2.1.0-generic31w); WhippyWoo/PokeDinner-DKB (DKB only).

Changes in PokeDinner master relevant to AMD/GPU, all dated 2026-10-28..31 and present in our base as far as I can tell (our tree has the BC3 transcode, heap budget code and page-run tracking):
- f42b918 (Wael3rd) texture cache thresholds use the main device-local heap budget, because on AMD the host-visible device-local heap was counted twice. Our vulkan_device.cpp GetDeviceMemoryBudget already has this.
- 25b674e (Takoroka) measure TSC on AMD CPUs (no CPUID 0x15), ~10% of main guest core. Irrelevant to Intel Core Ultra (our tree has EstimateRDTSCFrequency).
- a1f254c.. ASTC GPU decode + re-compress to BC1/BC3, 2048-texel cap, no GPU wait after ASTC decode (from Eden-NS2). In base.
- Pipeline work: translated-shader reuse, shared shader modules, native graphics pipelines translated on workers (Wael3rd b59fd99, 452dc27, 9a876a7, shader_recompiler SSA speedups). CPU shader-translation speed, not GPU time.
- dynarmic: 12c2fcc skip untouched locations at IR instruction end, a2cc455 compact per-block tables, 11aac32 decode ARMv8.1-8.5. No barrier work.
- vulkan_memory_allocator falls back to host memory when VRAM is full (d39c3b4).

kel6967/PokeDinner-AMD branch amd-perf-patch (9 commits on top of PokeDinner 2.0.0, 2026-11-01, author gabrielskaminski, "patches 0014-0019"). Diff read in full. No measured gains stated.
- fd5ac1a/892af54: on VK_DRIVER_ID_AMD_PROPRIETARY, back Switch 2 sparse regions whole (`g_nx2_sparse_lazy_supported=false`, env EDEN_NX2_SPARSE_LAZY=1 forces lazy) because amdvlk64 divides by zero on sparse images (Mario Kart World), plus a zero imageGranularity fallback (Eglantine PR #221/#219). Our tree has EDEN_NX2_SPARSE_LAZY but no AMD default-off; check whether our base has the granularity fix. Possible side effect to test: lazy sparse binding on AMD may cost time in OoT, unknown.
- 15/16: buffer_cache skips redundant uniform-buffer rebinds and no-op state updates, skips storage/texture-buffer binds when the enabled mask is 0, Maxwell3D::ProcessCBBind returns early when the binding is unchanged.
- 17: GraphicsPipelineCacheKey::operator== compares unique_hashes first.
- 18: FindBuffer bounds check. 19: batch shader-cache InvalidateRegions (one mutex acquire). 48aebe4: AMD logic-op attribute scan only when logic_op.enable.
These overlap in intent with Lichii #184 (merged "perf9" paths: constant-buffer batching, index rebind suppression, native bank checks) and #412 (merged). Low expected value for a GPU-bound frame; cheap to port.

## 2. Candidates, ordered by expected impact for our case

Status column: IN = in our list, BASE = in nx2 09b78bd or earlier as far as I can see, NEW = not in base.

GPU-bound pass (25-33 ms, 4 draws; tessellated 18-26 ms) on AMD
- No fork or PR addresses these hashes. Closest: Lichii #319 (IN, AA network bypass; AMD runs HMMA in software), #300 (IN), #223 rakefire-impala "AMD Mobile GPU successful in-game launch" (open, 82 commits, 142 files, RX 5500M): stand-in copy for Ocarina's upscaling networks on AMD, EDEN_NX2_TENSOR=<hashes> to run gameplay network 6bd913e1f34e83eb for real, tensor pipelines built in the background. NEW (large; mostly overlaps #300/#319). Branch rakefire-impala/Lichii AMD-ingame-fix 385e47f410.
- Lichii #378 (IN, EDEN_AMD_FP16=1): float16 kept on AMD driver; author's target RX 6600 (RDNA2), no numbers. Worth A/B on RDNA4, which has packed FP16.
- Lichii #210 (closed) / #273 (closed, BASE): AMD shader no-opt allowlist, depth sampling float, fragment shuffle via broadcast. #273 says "broadcast fallback can increase shader cost". Our round-5 native fragment shuffle test blacked the screen.
- Lichii #354 (IN), #353 (IN): AMD correctness.
- Lichii #421, #423 (open): foliage/grass binding budgets; #423 raises SSBO budget to 64 (`EDEN_NX2_SSBO64`), #395 and #410 (merged in nx2 after our base) near-hedge/foliage. Correctness, may add GPU work.
- Lichii #288 (open, Kajcsi): skip uploads a join copy overwrites whole; OoT shadow map resized every frame in Kokiri, uploads were 160-170 ms/s on GPU back thread, fps 54-57 (RTX 4090). NEW. Directly relevant to Kokiri.
- Lichii #268 (open): texture cache 5/8 of budget before collecting; for 16 GB cards, MKW measured 120 ms/s texture upload. Our tuned build already scales thresholds to 60%.
- Lichii #296 (open): cheaper ASTC uploads (from SmileyTatsu). NEW, low value for OoT.
- Lichii #415 (open): mark native compute storage bindings only as far as the shader's stores reach; #408 "stop mid-game pipeline rebuilds and slow compute builds (ports of #174, #250, #288, #319..." open; #411 pipeline libraries + shader warm-up from game files. NEW; read their bodies before choosing (not fetched).

Render pass / submit counts (we see 1.4-2.5k passes, 500-760 submits/s)
- Lichii #161 (999sian, open). Author's text: native submission budget 128 draws -> 4096 outside MoltenVK, FragmentBarrier through DeferFragmentBarrier, EDEN_NX2_FENCE_FLUSH_US 5000, immediate SubmitNx2ListCopies flush: vkQueueSubmit ~960/s -> ~22/s, render passes ~5,887/s -> ~255/s. The same PR states these figures came from a locally modified build, not a controlled benchmark of the PR head; its stage table (Kokiri 30.0 -> 40.5 fps for the submission change alone, i9-class/ i7-9700 Windows, GPU unspecified) is "selected observations". Other testers: ~20 fps, and 31-35 -> 36 fps median on i9-12900KF/RTX 3060 Ti. Status: partial. Our tree has DeferFragmentBarrier already and DefaultNativeBudget = 128 (submission_policy.h); #194/#184 (closed) set "native desktop defaults of 128 draws and 500 us". Runtime override exists: `EDEN_NX2_FLUSH_DRAWS=<n>` (vk_rasterizer.cpp FlushWork). Cheapest experiment: A/B `EDEN_NX2_FLUSH_DRAWS=4096` through the debug server/launcher; compare submits/s and GPU ms.
- Eglantine #494 (closed, rdenno): batched queue submissions (Hyrule Warriors); #639 (closed): fix for crashes it caused (back submits while another thread uses the scheduler). Merged into Eglantine nx2 -> likely BASE, with the #639 fix; verify.
- Eglantine #361 (closed, Sssnowyyy): no render-pass break after buffer uploads when uploads are mapped; ~6 ms GPU frametime on Apple/tiled GPUs. Desktop benefit unknown, probably small on RDNA4. Probably BASE.
- Lichii #345 (IN), #147 (IN), #376 (IN): sync/readback batching.
- Lichii #231/#179 (open): resolve waiting semaphore acquires without draining the back (4,700 acquires/s in OoT, ~380 ms/s of front-thread time, claims by author; measurement on i9-13900K family). `EDEN_NX2_SEM_FAST`. Overlaps #345 (IN); do not stack blindly (Frizti's tree turned SEM_FAST on, then off again: c5c6a4b0, bcb57076).
- Lichii #178 (open): skip instance-loop writes the back ignores; +0.6% on 4090 Kokiri, +6% on a CPU-bound 30 fps case (Zarigis). #250 (open): PageFilter to skip empty cache invalidations (OoT does ~88,000 per second), +3.4% (RTX 4090 + i9-13900K, 51.96 -> 54.2 fps). #175 (open, mja00): 4-way storage-buffer range cache, "biggest perf improvement" but no numbers. Eglantine #670 (closed, Duck912): 4096-entry per-thread MaxContinuousRange cache, claimed +~20% at Kokiri spawn on NVIDIA (GPU thread 44% in page-table walking); Lichii #146/#223 do page-run tracking at map time (BASE, `Nx2Perf` gated). Check `MaxContinuousRange` in our build is the cached path.
- Lichii #418 (open, from Frizti bcb5707): pin GPU/GPUBack threads to their own cores; Frizti reports 51.3 -> 55.6 median on 7800X3D at Lake Hylia (his number, not measured on Lichii). Our tuned hybrid-CPU placement covers the Windows side; #232 merged in nx2 since base.
- Lichii #267 (open, RsMan-Dev): "60 FPS on 9950X3D + RTX 4080, 1.5x res, heaviest Kokiri spot", stack of ports; useful as a recipe of which PRs mattered.
- Frizti/Lichii oot-gather-fix (72e5ad6c, 2026-11-07, ~45 commits over nx2 09b78bd): host frame pacer genlocked to display (EDEN_NX2_HOST_PACE), "look the draw's pipeline up once" (23f40d5d), skip buffer cache lock for dirty checks on never-GPU-written pages (6247fd51; same idea merged as #413), query_cache subtraction skips, fewer shader stalls (keep pipelines < 8000, skip unbuilt native draws), preflush OoT CPU readbacks (d65fd98e) then don't (fbc7b112). Released as frizti-v3. Mostly NVIDIA/Ryzen.
- nyakuoff/Lichii-oot: perf/batch-cpu-readback-ranges (= Lichii #147, IN), perf/cache-continuous-gpu-ranges (= #146, BASE), feat/fps-limit.
- callme-sy/Lichii-Superbuild (nx2 + curated patch; changelog v2-v7): includes "Nolar AMD fixes", GPU thread wake on queued commands (500 us poll instead of 1 ms, "sharkiix port"), async native discard #224 (measured "draws held for unbuilt pipelines 816 ms -> 0-8.7 ms per 10 s" by its author). Patch file: patches/superbuild.patch on branch superbuild. justr4mzi/hortensia aggregates SmileyTatsu/Selevet/Orchid/chuppatricky; not inspected.
- Lichii #166 (open): opt-in split-pipeline switches (EDEN_NX2_PIPELINE_ASYNC, TEX_UPLOAD_ASYNC); 30 fps on i5-12400F + RTX 4060.
- Lichii #264, #243, #204, #330: not read.

Other AMD-related reports on Lichii (symptom data, not fixes)
- #212 (open): OoT RX 9070 XT + 5800X3D on RADV, 28-35 fps, 35 ms frame, GPU 41%, "blocked on the GPU fence 0 us"; CPUCore_4/5 at ~80%, GPU thread 70%. Same shape as our CPU-bound main thread.
- #403 (open): RX 9070 XT + 9800X3D Windows build 93: ~18 fps defaults, 29 fps with `*:Error` log filter + high priority + Game Bar off; one thread at 99%.
- #191 (open): per-fence Preflushable downloads ~430 MB/s on RTX 5060 Laptop; skipping gives +27%, batching /64 +51-55% draws/s on the back thread but overexposes. Mitigation direction only.
- #356 (RX 9000 at 10 fps), #322 (NaNs in far-distance draw pass #72 on 9070 XT), #165/#285 (7900 XTX).

## 3. Dynarmic / x86 host JIT

- Lichii #161 commit d51b04d8 (999sian, 2026-11-05), diff read: (a) `DMB` CRm access type LD (1) or ST (2) emits nothing on x86-64 (frontend/A64/translate/impl/system.cpp); full/ISH DMB still mfence. (b) `EmitReadMemoryMov`: 8/16/32/64-bit ordered reads (LDAR/LDAXR/LDXR) become plain MOV/MOVZX instead of `lock xadd [addr],0`; 128-bit keeps cmpxchg16b; stores keep XCHG for STLR. (c) inline `MRS CNTPCT_EL0`: rdtsc; shl/or; sub base; mul by 64-bit factor, result in rdx (config.inline_cntpct_tsc_base/factor, switch EDEN_NX2_INLINE_CNTPCT=0 disables); skips CNTPCT spin pause on non-SMT CPUs; HostTiming at Critical priority; Unsafe_IgnoreStandardFPCRValue/CodeSpeed under Unsafe accuracy. Measured claim: guest_core_2 17.4 -> 11.5 ms/frame (author's PR text, local dev build, no matched baseline). Risks I see: plain MOV for LDXR/LDAXR must not be used where dynarmic's exclusive monitor relies on the locked access (check fastmem_exclusive_access path), and the inline factor is captured at JIT creation (breaks if the clock is recalibrated). Not in our base (mfence, lock xadd and HostCall are still in our checkout).
- Eglantine #717 (open, brandonbunnelle): lock-free exclusive monitor (LDXR loads before publishing the reservation, STXR with per-slot lock cmpxchg). Claims the global monitor lock was the whole CPU slowdown for OoT vistas (bisect: ignoring the monitor recovered FPS; Auto accuracy already enables Unsafe_IgnoreGlobalMonitor). No numbers. Correctness-sensitive. Worth a look since our main guest cores are CPU-bound; check whether our accuracy setting already ignores the monitor.
- PokeDinner 12c2fcc/a2cc455 (Takoroka): small JIT compile-time/memory trims. Eglantine #207 (closed): commit x64 code cache incrementally. Eglantine #525: CI dynarmic tests only.
- Lichii #174 (open): unroll K8 group of software HMMA (shader side).

## 4. FSR 4 / FSR 3.1

Facts (sources below):
- AMD FSR SDK 2.4 (tag v2.4.0 of GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, announced Oct 2026) adds Vulkan support for the ML upscaler ("FSR Upscaling (ML)", 4.1.2) and frame generation. ML upscaling is integrated through the "FSR API" (ffx api) and the signed binary distribution; I confirmed signedbin/ in the repo contains amd_fidelityfx_loader_vk.dll/.lib, amd_fidelityfx_upscaler_dx12.dll, amd_fidelityfx_upscaler_vk.dll and amd_fidelityfx_framegeneration_vk.dll, header Kits/FidelityFX/api/include/vk/ffx_api_vk.hpp, docs Kits/FidelityFX/docs/techniques/super-resolution-ml.md (note: "FSR Upscaling 4 requires integration using the AMD FSR API and use of the signed binary distribution"). Samples: sample sources are MIT; the SDK binaries' license (Kits/FidelityFX/docs/license.md) permits redistribution in binary form only, no reverse engineering.
- Hardware: gamingonlinux/Khronos/PCGH report ML 4.1.2 now on RX 6000/7000/9000 and selected Ryzen iGPUs (INT8 models), RDNA4 uses FP8. Reference timings in the SDK docs for RX 9070 XT, FP8, performance mode: 3840x2160 1316 us, 1920x1080 352 us (AMD numbers). So RDNA4-only no longer holds as of SDK 2.4; older articles (TweakTown) predate it.
- Inputs: color, depth, motion vectors, exposure, jitter (same contract as FSR 3.1 temporal upscaler; details in the doc's "Providing motion vectors" and "Depth buffer configurations").
- Existing Vulkan games are not auto-upgraded by the driver; integration needed. OptiScaler added an FSR 4 path for Vulkan titles through a DX12 bridge before SDK 2.4 (wccftech link).
Emulator integrations found: none for FSR 4. Lichii/Eglantine have FSR 3 upscaling and FSR3 frame generation (PokeDinner dafacbb "Added FSR 3 Upscaling and Frame Generation" by Djihads80; Eglantine #346 "FSR 3.1 implementation for DLSS games", #465 "Run Fire Emblem's own DLSS network, with FSR 3.1 as the fallback"; Lichii #194 closed FSR3 frame-gen fixes, #298 closed LSFG 3.1), and NVIDIA DLSS in place of OoT's AA network (Lichii #253, #387). Searches for FSR4/FidelityFX PRs in both repos found nothing beyond those. Feasibility: the Vulkan ffx API takes VkImages for color/depth/motion vectors, so a drop-in beside the DLSS replacement path (#253/#387) is plausible, but motion vector convention and jitter from OoT's guest need checking; not tested by anyone I found.

Sources: https://www.gamingonlinux.com/2026/10/amd-fsr-sdk-2-4-is-out-with-fsr-upscaling-4-1-2-for-older-hardware-and-vulkan-support/ ; https://www.khronos.org/news/permalink/amd-fsr-sdk-2.4-brings-fsr-4-upscaling-and-frame-generation-to-vulkan ; https://www.pcgameshardware.de/FidelityFX-Super-Resolution-Software-277617/News/FSR-4-1-Vulkan-SDK-2-4-Treiber-Updade-FSR-3-1-1558170/ ; https://wccftech.com/optiscaler-adds-fsr-4-support-to-vulkan-titles-before-amd-could/ ; https://gpuopen.com/learn/amd-fsr4-gpuopen-release/ ; https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK (v2.4.0). (I could only read the SDK repo; the news pages came from search snippets, gamingonlinux fetch was blocked.)

## 5. Suggested order of experiments (my judgement)

1. Verify log filter / priority (issue #403) and Game Bar; cheap, 11 fps claimed.
2. `EDEN_NX2_FLUSH_DRAWS=4096` A/B (no rebuild). Compare submits/s, render passes/s, GPU ms.
3. Port d51b04d8 dynarmic parts (DMB LD/ST elision, plain-MOV ordered loads, inline CNTPCT) for the CPU-bound main thread; review LDXR risk. Consider Eglantine #717 after.
4. Add #250, #178, #288, #175 (small, independent, measured only on NVIDIA/Zen).
5. Treat the 25-33 ms full-screen pass as an unknown: capture with the frame dump / RenderDoc, A/B EDEN_AMD_FP16=1, and EDEN_NX2_REJECT_HASH on those programs; nothing upstream explains it.
6. FSR 4 is a separate feature project (SDK 2.4 Vulkan, binary-only DLLs).

## 6. Sharkiix (2026-11-07)

Repo: Shakoquo/Lichii-sharkiix-version (GitHub user Shakoquo, "SHarkiix"), "mainly for AMD GPUs,
optimised for Zelda Ocarina of Time", tested on an RX 9060 XT. Fetched with
`git fetch https://github.com/Shakoquo/lichii-sharkiix-version <branch>`.

- No history of its own. v1/v2 (branch nx2) = zortos293/Lichii 99ac3d59 (2026-11-05) + 28f594ac
  (faster tensor-op emulation on AMD, semaphore acquire resumes, texture cache GC min age, a
  sampler without comparison for non-Dref reads, blend state refreshed on every draw without
  EDS3 blend). v3.0 = "chuppatricky amd testbuild v13 (imported source tree)" + shared shader
  cache import, Linux build, texture cache cap at 3/4 of 12 GiB+ budgets. v3.1/3.1.1 = "build-11
  source (fix/ocarina-of-time) as received" + VK_EXT_graphics_pipeline_library on AMD RDNA 3+
  (EDEN_NX2_GPL, background LTO relinks; crashed RX 6600/6650 XT on driver 2.0.353).
- Its README tells users to reject s5:c72b6ea746c00433 (ground mist, the mesh draw
  m1:b41e5eadb0568672) and s5:3ddfb1d7a854c849 (fog/light-shaft composite) because they draw a
  white veil there. Those are the passes that cost most in sunlit areas in our traces; with #292
  they render correctly here, so part of Sharkiix's AMD speed is effects turned off.
- AMD fixes in its imported trees and not in Lichii nx2 (9fa4a71): query copies wait only on the
  newest query (ported in round 10), NaN-defined FMin/FMax (emit_spirv_floating_point.cpp), a
  barrier so draws don't read unfinished compute output (vk_rasterizer.cpp, vk_scheduler.cpp), an
  atomics spin on a non-coherent cache line (spirv_emit_context.cpp), global memory helpers that
  overflowed AMD's compiler, render targets the fragment shader never writes, AMD safe compile
  (EDEN_NX2_AMD_SAFE_COMPILE) and VK_AMD_buffer_marker breadcrumbs.
- Verdict: don't migrate. Each release replaces the whole tree with someone else's snapshot, its
  base is older than ours and has more glitches, and the occlusion fix (round 9/10) already gives
  more than its query change. Port single fixes with credit.

Other combined forks seen: justr4mzi/hortensia ("the best of the Eglantine forks (Lichii,
Selevet, Orchid, SmileyTatsu, chuppatricky...) in one build"), jordenjacobsen-ai/Lichii-Frizti-
v3-plus (Frizti v3 + Linux, AMD and Ocarina foliage fixes). Upstream #412 already ports Frizti
and Selevet back-thread savings.

## 7. Fork survey, 2026-11-07 evening (for AMD, Intel hybrid CPUs and stability)

Fetched into `refs/forks/*` of the working clone (shallow); upstream nx2 at `d5494116`.

- **zortos293/Lichii nx2**: 85 commits since `9fa4a71` (one day). Merged many PRs tuned4 carried
  (#329, #346, #353, #299, #481, #478, #465, #446). New: #521/#583 HMMA on 16x16x16
  cooperative matrices (RDNA4: the AA network 24.4 -> 8.1 ms, "41 -> 60 fps" with the network
  on; we bypass it, so only the "Game" AA mode gains), #475 (NVIDIA 16x8x16), #432 NaN-defined
  min/max (bumps the pipeline cache version 53 -> 54: all caches rebuild; #591 open would keep
  v53 caches), #526/#518 clang-cl + ThinLTO + an Ocarina PGO profile (`lichii-windows.profdata`,
  MSVC ABI, LLVM 22.1.8: does not match our MinGW build's names), stability fixes (#310 JIT code
  commit on Windows, #485 decommit race crash, #440, #427, #495, #435, #444, #438, #439, #365
  Overwolf hooks, #330, #331), #250 cache invalidation filter (+3% Kokiri on NVIDIA), #192 mesh
  and guarded pipelines in the disk cache, #550. A test merge into tuned4: 27 files, 53 conflict
  hunks (thread.cpp hybrid placement vs #232/#418/#530, the split front vs #345, shader
  recompiler storage buffers, texture cache).
- **Open upstream PRs**: #573 (sanchodecuba11-del, RX 9070 XT, driver 2.0.416, Kokiri): AMD
  query copies without WAIT_BIT (9 -> 22 fps; superseded here by answering occlusion reports),
  texture collector at 3/5 of the budget (already ours), compute e0a992208d87eeb8 unoptimized,
  512 draws per submission on AMD (35 -> 50 fps at the Deku Tree), parallel unswizzles. #579
  pipeline libraries + warm-up from play history (first-visit stutter; 30-45 min warm-up). #559
  (no fps change). #570 occlusion reset threshold (OoT water flicker). #591, #575, #589.
- **Frizti/Lichii v3** (`frizti-v3` tag; also merged by tomk134/lichii-custom, Kitsu,
  Vendichii, Firefly, Lichii-Ally): drain-free semaphore acquire resolution (547ce11b4 +
  f362e5a5f, port of #231: acquire drains 4165/s -> 50/s), macro DMA read snapshots (8ac3666f7,
  EvilOOT's fix), preflushed reads flush on demand (049f87818), pinned split GPU threads,
  genlocked host pacing and low-latency 30 fps, fewer shader stalls (pipelines < 8000, skip
  unbuilt native draws). Most conflict with #345 in tuned4.
- **Frizti, rechecked 2026-11-07 ~19:30 UTC**: no release after `frizti-v3` (72e5ad6c, the
  low-latency 30 fps commit); the nx2 head (f66ea633) only adds a README note and
  `test/no-sky-fold` is a 2026-11-05 bisect branch. Its preflush fix 049f87818 applies to tuned4
  but by Frizti's own count didn't help (213 vs 232 white-flash reports): skip unless preflush
  becomes the default. Kitsu (built on Frizti) has a 4-line exit-hang fix, 4276c9c7d
  (QThread::terminate deadlock on File > Exit), which applies cleanly: take it next round.
- **Kradnah/Firefly** (OoT): 64 KiB uniform bank uploads without page protection churn
  (bd87a1a37, ~11% of the back thread upstream; tuned4's #345 compares bank pages with memcmp on
  every bind instead), skip upload scans of unwritten buffers (f5c4496f8), cheaper back-thread
  bookkeeping (8052143d9).
- **justr4mzi/hortensia** (combined): notsapinho's pipeline reuse while no key register changed
  (dc10c9537), clean-range rescans skipped (cbe9a2c59), linear uploads from guest memory; AMD:
  37b01132333ef8df unoptimized (driver 2.0.353), DK Bananza RDNA pipeline crash fix (632c2c50f).
- **MythosOfs/Orchid**: AMD-only, developed on an RX 9070 XT (RADV). Moved on to Yoshi, Wonder and
  Xenoblade; `upstream/amd-vk-hardening` has robustness fixes (query copy destinations
  disjoint, tessellation pipelines the driver would refuse).
- **Djihads80/Eglantine**: kraibse's AMD port of #273 (stalling compute shaders, misaligned SSBO
  splits, depth sampling, CAS retries).
- **No change since the last look**: Sharkiix (3.1.1), PokeDinner (GitHub mirror stale since
  2026-11-04; the Eden Forgejo is blocked from the cloud session), kel6967/PokeDinner-AMD.
- Others seen: SmileyTatsu (cheaper ASTC uploads = upstream #296, texture memory limit setting),
  Selevet performance builds 7-10 (OoT), Lichii-Superbuild (curated PGO), Lichii-Ally (Zen 5
  APU scheduling), Anka (NUP decompression cache), Elysium, MiauLichiiFork, Kitsu, Vendichii.
- Guide: riyadhHDA/wichforks lists forks by GPU and game; it names Orchid for AMD and Lichii /
  Frizti for OoT.

## 8. Round 13 (2026-11-07 night)

- **zortos293/Lichii nx2** at `92f021240`: four commits since `d5494116`. #606 runs NVIDIA DLSS
  Super Resolution for Ocarina with camera motion vectors from depth
  (`vk_ocarina_camera_motion.cpp`) and jitter: the inputs an FSR 3.1 or FSR 4 path for Ocarina
  needs too. #569 draws task/mesh pipelines through compute without `VK_EXT_mesh_shader`, #616 is
  the AA network bypass as merged, #574 is CI. All merged into tuned4 with the 98 before them.
- **Open #591** (jpkto): a new cache version starts from the newest compatible older cache
  (54 from 53, then 52), so #432's bump doesn't throw caches away. Ported.
- **Frizti**: still no release after `frizti-v3`. Its drain-free acquire polls (547ce11b4 +
  f362e5a5f) are ported onto #345's front (`StableSemaphore` instead of #179's
  `SemaphoreReadableNow`; the fence release is deferred to the back, since `GpuSplit::Run` is a
  full drain here). Its macro DMA read snapshots (8ac3666f7) are not: they cut the same report-
  page drains that round 13 removes for answered occlusion reports.
- **Kitsu**: new commits are UI only (console-style home, box art); its exit-hang fix 4276c9c7d
  is in.

## 9. Round 14 (2026-11-08): upstream builds 115-117 and open pull requests

- Builds 116 (`92f021240`) and 117 (`89440b29a`) are the newest releases; 113-115 were never
  published. Round 13's base already was build 116. Its notes credit Ocarina's "60+ fps" for many
  users to #551 (foliage), #583/#582 (the AA network on matrix units: "41 -> 60 fps on RDNA4", which
  the owner's "No sharpening" skips anyway), #446, #250, #481, #580. Build 117 only keeps HMMA off
  AMD GPUs without matrix cores (#657).
- After 117: #664 makes GPU Clocks Boost (the default) and Overclock shrink Switch 2 GPU durations
  again; Ocarina sizes its dynamic resolution from them (upstream: 854x480 -> 1920x1080 with Boost).
  #679 imports AMD fixes from the CheezerWheezer survey (several were already ours).
- Open pull requests taken: #415 (store extents bound native compute write marks; the owner's
  download trace shows exactly its over-marked 2.8-8 MiB skinning ranges), #288, #699, #700, #621,
  #687, #695 (AMD FSR 3.1 for Ocarina, opt-in), #296, #609, #610, #611, #589, #645. Left: #640,
  #408, #652, #579, #559, #698, #655, #166, #615, #575. (#161's upload-word counter turned out to
  be in already: #184 merged a per-word form, `clean_words`/`ForEachDirtyUploadRange`.)

## 10. Round 15 (2026-11-08): FSR 4 through the AMD FSR SDK 2.4

- zortos293/Lichii nx2 is still at `bf4c42e22`: nothing new to merge.
- FSR 4 is integrated through the AMD FSR API: `ffxQuery(GET_VERSIONS)` with a `VkDeviceContext`
  lists the upscaler versions for the device, `ffxCreateContext` takes
  `ffxCreateContextDescUpscale` + `ffxCreateContextDescUpscaleVersion` (`FFX_UPSCALER_VERSION`
  4.1.2) + `ffxCreateBackendVKDesc` + `ffxOverrideVersion`, and `ffxDispatch` records into a
  `VkCommandBuffer`, with `FfxApiResource` = a `VkImage` plus its description and state
  (UNORDERED_ACCESS = GENERAL; the backend returns the images to it). The loader exports only
  ffxCreateContext/Destroy/Configure/Query/Dispatch; the upscaler DLL (36 MB) imports
  vulkan-1.dll and the MSVC runtime (MSVCP140, VCRUNTIME140, _1), which AMD's driver installer
  brings. The SDK's own Vulkan sample enables VK_KHR_cooperative_matrix, VK_EXT_shader_float8
  (with shaderFloat8CooperativeMatrix) and VK_KHR_compute_shader_derivatives ("FSR4 INT8
  prepass"); the backend source (`Kits/FidelityFX/backend/vk/ffx_vk.cpp`) only queries what the
  device supports, so the application has to enable them. The doc's limitation line says RX 7000
  and newer; its timings list RX 6000 INT8 too. 9070 XT, FP8, 1440p->4K target: ~1.3 ms (AMD).
- Inputs FSR 4 doesn't need (it makes its own reactivity) may still be given; this build passes
  #644's effects bias as both masks, as FSR 3.1 does. No jitter yet (#253 has a Halton camera
  jitter for DLSS): without it the upscaler can't recover sub-pixel detail.

## 11. Round 16 (2026-11-08): Fritchii v4 and v4.1

- Frizti/Lichii renamed its builds Fritchii; tags `fritchii-v4` and `fritchii-v4.1` (hotfix
  6664b8726), 131 commits over `frizti-v3`, many with Ocarina measurements in the message (the
  pacer's on an RTX 4080 at 120 Hz). The owner reports v4.1 is considered the best-performing
  fork. Fetched as `refs/forks/frizti-tags/*` in the session clone.
- Switches on by default in v4.1 (`nx2_debug_control.h`, `LoadExperiments`): EXP8 = 15 (texture
  upload hitches; Hyrule Field 1% low 30.3 -> 35.1), EXP9 = 1 (bank pages compared only when
  they may have changed; idle 54.5 -> 57.7 fps, walking 56.6 -> 58.9, verified 0 missed in 120 s),
  EXP10 = 1 (macro DMA read drains only for compute, uploads, copies and releases), EXP14 = 1
  (unchanged macro register writes dropped first), EXP21 = 1 (storage-pointer texture handles
  bind nothing), the DMA snapshot (`EDEN_NX2_DMA_SNAPSHOT=9`, EvilOOT's; Lake Hylia 46.5 ->
  52.3 there) and the host frame pacer (`EDEN_NX2_HOST_PACE=1`, with Mailbox enforced).
  Everything else (EXP2, 3, 5, 6, 7, 12, 13, 16-19) is off by default there.
- Ported: EXP8, EXP9, EXP21, 9b8fef282 + record grouping, 55bba8812 (#408's attribute keys) and,
  opt-in, the host pacer with genlock. EXP10's cause differs here: our remaining macro read
  waits are constant buffer overlaps (94%, `EDEN_NX2_MME_DMA_STATS`), so round 16 reads those
  words from the front's shadow instead (our own fix). EXP14 matches our FrontWrite. Its
  settings revision 3 (resolution 1x, FXAA, CPU Auto, both shader caches) matches the owner's
  config already; f6d42b871 stopped enforcing reactive flushing.
- Its upstream imports (f319e1076, 2c6c844f4, af2bbd3ce, aff77e7fb, 34ae8336e, the compute
  pipeline fixes) were already in tuned4 through our merges; the updater, branding and
  installer commits don't apply; 5aefec443 merges zortos's v53/v54 caches into Fritchii's own
  `vulkan-frizti.fork-v52.bin`, while tuned4 uses zortos's v54 file directly.
- The host pacer's gains are on the display, not in frame rate: at 60 fps on 120 Hz, frames
  shown for exactly two refreshes 97-99% (91-96% without), longest interval 22-34 ms (70-200).
  On a FreeSync display below 60 fps VRR does more; the owner's monitor is unknown.

## 12. Round 17 (2026-11-08): "Lichii OoT AMD" and the fork survey

- The owner's link: medicinauvsecretaria-lang/Lichii (r/SwitchPirates2, "Ocarina of Time
  Switch 2 on AMD GPUs Lichii build"), releases `lichii-oot-amd-r1` and `-r2`. Its default
  branch is upstream nx2 `bf4c42e22` untouched; the tags sit on branch `rx6600-r5500` (the
  author's RX 6600 and Ryzen 5 5500, by the name): PantACRO4life's #579/#741 graphics pipeline
  library warm-up (24 commits, `perf/gpl-shader-warmup-v2`) plus four of its own. 7d96f498f and
  63fc50e99 build only Windows PGO with `/clang:-march=znver3 /clang:-mtune=znver3`
  (`release-binaries.yml`) and save the driver cache every 32 new pipelines, at most every 10 s
  (was 128 and 30 s), and the used list every 5 s (15). 965e3e61f journals every pipeline
  library and link call and builds monolithic pipelines after a call that never returned (AMD's
  driver crashing in amdvlk64.dll on RDNA 2); f38bbcb62 catches access violations in pipeline
  creation and writes crash reports.
- Why it "may not work for Intel CPUs": `-march=znver3` lets the compiler use any Zen 3
  instruction. SSE4a (EXTRQ/INSERTQ, which LLVM can pick for some vector shuffles), CLZERO,
  MWAITX and RDPRU exist on no Intel CPU; SHA, VAES, VPCLMULQDQ, RDPID and CLWB are missing on
  Intel CPUs before Ice Lake/Alder Lake. Any of them in the program stops an Intel CPU with an
  illegal instruction. Measured here: 117 Lichii files (video_core, the shader recompiler,
  common, dynarmic's x64 backend) compiled by clang 18 with `-O3 -march=znver3` contain none
  of them, so on a recent Intel CPU (Arrow Lake has everything else Zen 3 has) the downloads
  would likely start, and the warning is the author's caution (tested on AMD only). Their
  binaries couldn't be checked (the cloud session can't download release assets). Nothing in
  the source depends on the CPU: built with x86-64-v3, as tuned4 is, it runs on any Intel Core
  since Haswell.
- Migrated: the cache saves (`403a22b82`); the compute half of the warm-up, through upstream
  #781 (merged 2026-11-08: compute programs found in the game files, built on the loading
  screen), seeded Ask (`8b3a3c54a`, tuned defaults 3). Not migrated: graphics pipeline
  libraries and the graphics warm-up, heavy skip, fast first builds and both guards. Fast-linked
  libraries draw white fringes on AMD's Windows driver (AMD-Gfx-Drivers#126; c196b4f28 links
  them optimized there) and crashed it often enough that the fork needed the two guards, the
  owner's preload already builds every cached pipeline at boot, and the code collides with our
  pipeline cache. Shared functions as calls (`EDEN_NX2_GRAPHICS_NOINLINE`, `_COMPUTE_NOINLINE`)
  compile faster, but Fritchii measured -2.6 fps on NVIDIA with a broader DontInline: A/B it on
  the 9070 XT before taking it.
- Its unreleased branches: `win/hybrid-core-placement` (4a2b6003a: CPU sets of the highest
  efficiency class for time-critical threads; "within noise" on an i9-13900K) is what
  hybrid_cpu.cpp already does per class (P, E, LP-E); `perf/oot-acquire-no-drain` resolves
  waiting acquires without draining, as tuned4 has since round 13 (Frizti's), plus experiment
  commits upstream later merged.
- Fork survey (heads in the session clone, `refs/forks/*/head`). Taken: tomk134 e8e50e797
  (unswizzle contiguous guest memory in place, `43321b8db`), Elysium fc0ac4b3b (scratch buffer
  for the other runs, `d05a02108`) and 4a680b22d (IADD overflow's negative bound in both shader
  backends, `da24ac9c1`; a brute-force check agrees, its test needs Elysium's SPIR-V helpers).
  Upstream's open #836 (image recycler), #810 (shared Protect lock) and #784 (16-byte swizzle,
  1.7-2.4x in its microbenchmark here) too. Not taken: Eglantine 1e2c2dd4d (Lichii lowers BRA.U
  already; HSETP2's constant buffer form is a Mario Kart World fix and no Ocarina log shows
  that rejection), Kitsu e05b28cf1 (dynarmic block ranges by page, for Tears of the Kingdom's
  invalidations), Vendichii 368450b57 (AMD FSR 3 frame generation, FidelityFX SDK 1.1.4: a
  later option for showing 60 fps, not frame rate), Orchid's upload/copy synchronization and
  download-ordering fixes (cc04f48b3, f2eb04bab, 225ed15fb: no Ocarina symptom yet),
  Lichii-Superbuild (docs and a Zen 3 preset), Sharkiix (nothing new). #806 (NVN draw
  parameters, Hyrule Field grass on AMD) waits for an A/B at the field spawn.
- Upstream's open #805 (decoupled split threads: about 700 ms/s of front waits, claimed 46-49 ->
  60 fps on an RX 7900 XTX) and pieces of #717 (guest JIT and kernel, Ocarina's native macros;
  #831's author measured 1% lows 18.8 -> 33.0 on a 9800X3D + RX 9070 XT) are the next big
  levers. Both are drafts that conflict with tuned4 in 25-31 files; the survey's order and
  per-commit notes are in HANDOFF's next steps.
