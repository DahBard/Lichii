Lichii tuned for Intel Core Ultra 200H (Arrow Lake-H) + AMD Radeon RX 9070 XT
==============================================================================

Test build of Lichii (github.com/zortos293/Lichii) from github.com/DahBard/Lichii, branch
"tuned". Same emulator and the same user data folder as the official build, plus:

 * Compiled by clang with Lichii's PGO profile, for x86-64-v3 (AVX2, BMI2, FMA) and tuned
   for Arrow Lake. It runs on any CPU with AVX2 (Intel Haswell / AMD Zen and newer).
 * Hybrid CPU core placement (Windows): the GPU command threads (GPU, GPUBack, VulkanWorker)
   run on the P-cores, the emulated CPU cores and the low-priority pipeline builds on P- and
   E-cores, core timing on an E-core, and no emulator thread on the two 2.5 GHz low-power
   E-cores.
 * Intel WAITPKG waits: the GPU threads and idle emulated cores wait for each other with
   UMWAIT (the core idles until the other thread writes) instead of spinning on PAUSE, which
   leaves more of a laptop CPU's power budget to the busy cores.
 * The texture cache keeps up to 5/8 of the VRAM budget (about 9.5 GB of a 16 GB card) before
   it evicts, instead of 5 GB, so scenes you return to are not decoded and uploaded again.
 * Ocarina of Time fixes from open Lichii pull requests: a setting to skip the game's
   anti-aliasing network (#319; AMD GPUs run its tensor math in software), AMD drivers no
   longer stall on those shaders (#300), white flashes at GPU accuracy Low (#337), fog white
   haze and black water and foliage after area changes (#292), leaf holes from split warps
   (#329), held draws build their pipeline at once (#321), Dynamic FPS keeps real speed down
   to 10 fps (#299), opt-in readback preflush (#259), no violet halos from shader exports AMD
   leaves undefined (#353), no green tint growing from shadow lookups through wrong texture
   handles (#354), batched texture evictions (#376), precise macro reads and cheaper semaphore
   waits on the split GPU thread (#345).
 * Shaders stay compiled: every cached pipeline is compiled at boot and kept, not just the ones
   the last sessions used (EDEN_NX2_PRELOAD_ALL_PIPELINES=1 in nx2_switches.txt; official
   builds destroy one unused for 9000 frames, so a few minutes on the title screen threw away
   the shaders compiled at boot and gameplay froze while rebuilding them).
   Shaders the game has never shown yet can't be compiled ahead; ab-8 skips their draws.
 * The emulated CPU skips memory fences that x86 CPUs don't need (ARM DMB LD/ST and
   load-acquires) and reads the game's clock without leaving the generated code.
 * On AMD's driver GPU work is submitted every 512 draws instead of 128 (each submission costs
   more there), and large new textures are unswizzled on up to four helper threads.
 * Ocarina's occlusion checks (about 1,400 "is this visible?" questions a frame) are answered
   "visible" at once instead of by the GPU: on this Radeon they cost most of the frame, and
   answering them doubled the frame rate (LICHII_FAKE_OCCLUSION=0 runs them again).
 * Ocarina's built-in Dynamic FPS mod takes each frame's own time instead of an average of the
   last 8 frames, so the game no longer runs fast for a moment after a slow stretch
   (LICHII_DYNFPS_SMOOTHING=8 brings the average back).
 * The split GPU thread waits less for its back thread: a waiting semaphore is polled from
   memory instead of draining the back each time (ported from Frizti's fork), and macro reads of
   pages with already-answered occlusion reports no longer drain it.
 * Older shader caches carry over: a new cache version starts from the newest compatible older
   one (Lichii #591), and the log says how many cached pipelines were left out and why (another
   GPU or another dynamic state setting).
 * Ocarina's compute shaders mark only the memory they really write as GPU-written (Lichii
   #415): fewer stalls when the game reads its own data and less copied back from the GPU
   (Graphics > Advanced > "Native compute write mark", Full = as before). Kokiri Forest's
   shadow map resizes no longer upload what a copy overwrites (#288), and draws after a clear
   stay in their render pass (#699).
 * "GPU Clocks" (Boost by default) again shortens how long the game thinks its GPU work takes
   (Lichii #664). Ocarina sizes its render resolution from that, so it can render sharper and
   ask more of the GPU; Normal keeps the old behaviour.
 * AMD FSR 4 upscales Ocarina to 4K by default (docked: the game's 2560x1440 to 3840x2160),
   with AMD's FSR 4 libraries from the AMD FSR SDK 2.4 next to lichii.exe
   (amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll; AMD's license is in
   amd_fidelityfx_license.md). Where FSR 4 can't run (another GPU, missing libraries, an
   error), FSR 3.1 takes over and the log says why ("AMD FSR 4:" lines). The setting is
   "Use AMD FSR upscaling in Ocarina of Time" (Graphics); trace-fsr31.bat compares FSR 3.1.
 * GPU readbacks are preflushed in Ocarina by default (fewer stalls when the game reads its own
   GPU data back; EDEN_NX2_PREFLUSH=0 turns it off, trace-no-preflush.bat compares).
 * The log can show each frame's time and the 1% lows ("Frame times:" lines every 10 s with
   perf-trace.bat or frame-times.bat) and, with perf-trace.bat, what each emulated CPU core
   spends its time on ("Guest core N per second:").
 * Round 16, from Fritchii v4.1 (Frizti's fork, the fastest Ocarina build of the moment) and
   our own logs: the native uniform bank check compares only pages that may have changed
   (r23: ~650,000 page compares a second, 99.7% unchanged; LICHII_BANK_CHECK=0 compares all
   again), cheaper texture uploads on the GPU thread (staging buffers kept, uploads that GPU
   copies overwrite skipped, large untiles on helper threads; LICHII_TEXTURE_UPLOADS=0 as
   before), texture handles that are really storage buffer addresses bind no texture (the
   waterfall flicker; upstream Lichii now does this too), macro reads that overlap
   queued constant buffer writes take those words from the queue instead of waiting (94% of
   those waits in r23; LICHII_DMA_MIXED_READS=0 as before), pipelines keyed only by the vertex
   attributes their programs read, so the same shaders stop compiling again for leftover
   attribute types (LICHII_KEY_ALL_ATTRIBUTES=1 as before). ab-12 turns the switchable ones
   off at once; verify-r16.bat checks the two new shortcuts against the old way.
 * Fritchii's host frame pacer, off unless asked for: LICHII_HOST_PACE=1 (trace-host-pace.bat)
   holds each frame for its display refresh and locks the game's frame clock to the display,
   for evenly shown frames at 60 fps on 120 Hz displays; LICHII_HOST_PACE=2
   (trace-display.bat) only measures how many refreshes frames stay on screen.
 * Round 17: upstream Lichii's 53 pull requests of 2026-11-08. Among them: the shader cache
   builds each pipeline from its first record and removes exact duplicates and broken records
   from the file; GPU copies that overwrite a whole texture skip its upload; the texture cache
   keeps 5/8 of the VRAM budget, as this build already did. Also upstream pull requests that
   are still open: faster texture untiling in 16-byte blocks (#784, 1.7 to 2.4 times as fast
   in our test), Ocarina's images recycled instead of created and freed every frame (#836),
   and fewer waits between memory protection changes (#810). Textures in one contiguous piece
   of memory are untiled in place instead of being copied out first (from tomk134's fork;
   ab-13 copies again), and GPU shaders' signed adds report overflow correctly at the
   boundary (from the Elysium fork).
 * From the "Lichii OoT AMD" builds (medicinauvsecretaria-lang): the driver's shader cache is
   saved more often (after 32 new pipelines, at most every 10 s; the list of used pipelines
   every 5 s), so a driver crash loses less, and at launch the emulator offers to precompile
   the game's compute shaders on the loading screen (Graphics > Advanced, "Precache shaders
   before game start": now Ask; Always or Off there). Those builds are compiled for AMD Zen 3
   CPUs (-march=znver3), which is why they may not start on Intel CPUs; this build stays
   compiled for any x86-64-v3 CPU.
 * Upstream now pins the GPU threads to cores of their own by default (GraphicsExtra > Hacks,
   "Pin GPU threads to their own cores"); this build leaves it off: on the Core Ultra 7 255H
   it would put the emulated CPU cores on the four P-cores left instead of spreading them over
   P- and E-cores.
 * Round 18: AMD FSR 4 (and its FSR 3.1 fallback) now gets a camera jitter: each frame the
   scene is drawn shifted by a different fraction of a pixel and FSR is told the shift, so it
   collects new detail over the frames instead of the same samples (sharper edges and fine
   detail, less crawling). It runs only while FSR upscales Ocarina; ab-14 turns it off
   (LICHII_FSR_JITTER=0). Upstream Lichii's newest commits are in too, among them cheaper
   per-draw work on the GPU back thread (texture lookups reused while a pipeline's textures
   stay the same). The run also has a "pgo-train" download again: 10-15 minutes of Ocarina
   with it records a profile that makes the next builds' own code faster on this CPU (see the
   end of this file).
 * Recommended speed settings, applied once on first start (change them freely afterwards):
   GPU accuracy Low, asynchronous shaders on, reactive flushing off, asynchronous presentation
   on, GPU video decoding, and "Ocarina of Time anti-aliasing" = No sharpening (Graphics
   settings; "Game" brings the game's network back). Round 15 adds Ocarina's FSR upscaling on.

The "msvc" download is the same source built like the official MSVC build (MSVC, no PGO, no
CPU tuning flags, PAUSE spinning instead of UMWAIT). Use it to tell whether a graphical glitch
comes from the compiler (only the pgo builds show it) or from the source.

A/B launchers (this build with one change switched off or on; compare the same scene each time):
 ab-1-stock-threading.bat     no core placement and no UMWAIT: only the compiler changes remain
 ab-2-no-core-placement.bat   Windows places every thread (LICHII_CPU_POLICY=off)
 ab-3-soft-placement.bat      only keeps threads off the low-power E-cores (LICHII_CPU_POLICY=soft)
 ab-4-strict-placement.bat    emulated CPU cores on P-cores too, pipeline builds on E-cores only
                              (LICHII_CPU_POLICY=strict)
 ab-5-no-umwait.bat           PAUSE spinning as before (LICHII_WAITPKG_SPIN=0)
 ab-6-official-texture-cache.bat  texture cache collects from 5 GiB, as official Lichii did
                              before 2026-11-08
 ab-8-skip-unbuilt-draws.bat  skips a draw while its new pipeline compiles instead of waiting
                              (EDEN_NX2_ASYNC_DISCARD=1): fewer hitches on new content, an object
                              can be missing for a few frames; logs like perf-trace.bat
 ab-9-amd-fp16.bat            keeps float16 shader math on AMD (EDEN_AMD_FP16=1); official builds
                              turn it off for a driver bug in another game, so look for wrong colors
 ab-10-run-ahead.bat          the game prepares the next frame while the GPU renders this one
                              even when the GPU is behind (EDEN_NX2_FRAMES_AHEAD=2), so a slow
                              GPU does not idle between frames; logs like perf-trace.bat
 ab-11-official-shader-cache.bat  boot compiles only what the last sessions used, and unused
                              pipelines are destroyed after 9000 frames (LICHII_KEEP_PIPELINES=0)
 ab-12-round15-paths.bat      round 16's new shortcuts off (bank check, texture uploads,
                              mixed macro reads, attribute keys), logged like perf-trace.bat:
                              if a glitch or a frame-rate change goes away here, one of them
                              caused it
 ab-13-round16-texture-reads.bat  textures copied out of guest memory before they are untiled,
                              as in round 16 (EDEN_NX2_UNSWIZZLE_COPY=1), logged like
                              perf-trace.bat
 ab-14-no-fsr-jitter.bat      FSR 4 without round 18's camera jitter (LICHII_FSR_JITTER=0),
                              logged like perf-trace.bat: compare edges and fine detail
 verify-r16.bat               compares round 16's bank check and mixed macro reads with the old
                              way on every use and logs differences ("missed" and "differed"
                              should stay 0); slower
 trace-host-pace.bat          Fritchii's host frame pacer on (LICHII_HOST_PACE=1), logged like
                              perf-trace.bat plus "Host pacing on:" lines every 10 s
 trace-display.bat            the pacer only measuring (LICHII_HOST_PACE=2): the same lines
                              without holding frames, and the display's refresh rate
 trace-fp16.bat               ab-9 with the logging of perf-trace.bat
 trace-flush4096.bat          submits GPU work every 4096 draws instead of every 512 (AMD)
                              (EDEN_NX2_FLUSH_DRAWS=4096), logged like perf-trace.bat
 trace-old-jit.bat            the emulated CPU without round 7's faster barriers and counter
                              reads (LICHII_JIT_TSO=0, LICHII_JIT_INLINE_CNTPCT=0), logged
                              like perf-trace.bat
 trace-fsr31.bat              AMD FSR 3.1 instead of FSR 4 (LICHII_FSR4=0), logged like
                              perf-trace.bat
 trace-no-fsr.bat             Ocarina's own upscale pass, no 4K output
                              (EDEN_NX2_DLSS_REPLACEMENT=native), logged like perf-trace.bat
 trace-no-preflush.bat        no preflushed readbacks (EDEN_NX2_PREFLUSH=0), logged like
                              perf-trace.bat
 trace-downloads.bat          perf-trace.bat plus a list of what the GPU copies back and which
                              GPU-written memory the game reads (EDEN_NX2_DOWNLOAD_TRACE=1)
 trace-old-drains.bat         the split GPU thread waits for its back thread as in r18
                              (LICHII_ACQUIRE_POLL=0, LICHII_REPORT_DRAIN=all), logged like
                              perf-trace.bat: for comparison, or if the new waits misbehave
 trace-real-occlusion.bat     runs the GPU occlusion queries again (LICHII_FAKE_OCCLUSION=0),
                              logged like perf-trace.bat: slower, for comparison
 perf-trace.bat               default settings plus GPU thread zones, emulated CPU cores and
                              frame times in the log (EDEN_NX2_GPU_PROFILE=1,
                              EDEN_DKB_FRAME_TRACE=1, EDEN_NX2_MME_DMA_STATS=1). Draws are timed
                              one sampled frame in four (LICHII_TRACE_DRAWS_EVERY=4)
 frame-times.bat              default settings with only each frame's time logged
                              (LICHII_FRAME_TIMES=1): the 1% lows without the other logging

Each start replaces the log, so copy it after every run you want to compare.

The CachyOS (Linux) download of the same run has a .sh twin of each launcher and its own
README-cachyos.md (no FSR 4 there: AMD's libraries are Windows-only).
The log (eden_log.txt in the emulator's Log folder) starts with a "Hybrid CPU:" line and one
"Core placement:" line per thread, so you can check what each launcher did.

PGO training build (the "pgo-train" download)
---------------------------------------------
This build records which code your game runs, so the next "pgo" build can be optimized for it.
It runs slower than the others; that is expected. Run lichii.exe from it, play Ocarina through
a few typical areas for 10-15 minutes, then stop the game and close the emulator normally. It
leaves lichii-pgo-<number>.profraw files next to lichii.exe (it also saves them every 30 s).
Zip them and upload the zip to lichii-tuned/pgo in the fork; the next builds use them.
