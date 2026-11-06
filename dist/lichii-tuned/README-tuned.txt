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
 * The texture cache keeps up to 60% of the VRAM budget (about 9 GB of a 16 GB card) before
   it evicts, instead of 5 GB, so scenes you return to are not decoded and uploaded again.
 * Ocarina of Time fixes from open Lichii pull requests: a setting to skip the game's
   anti-aliasing network (#319; AMD GPUs run its tensor math in software), AMD drivers no
   longer stall on those shaders (#300), white flashes at GPU accuracy Low (#337), fog white
   haze (#292), leaf holes from split warps (#329), held draws build their pipeline at once
   (#321), Dynamic FPS keeps real speed down to 10 fps (#299), opt-in readback preflush (#259),
   no violet halos from shader exports AMD leaves undefined (#353), no green tint growing from
   shadow lookups through wrong texture handles (#354), batched texture evictions (#376),
   precise macro reads and cheaper semaphore waits on the split GPU thread (#345).
 * Recommended speed settings, applied once on first start (change them freely afterwards):
   GPU accuracy Low, asynchronous shaders on, reactive flushing off, asynchronous presentation
   on, GPU video decoding, and "Ocarina of Time anti-aliasing" = No sharpening (Graphics
   settings; "Game" brings the game's network back).

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
 ab-6-official-texture-cache.bat  texture cache collects from 5 GiB as in official Lichii
 ab-7-preflush.bat            preflushes GPU readbacks (EDEN_NX2_PREFLUSH=1): fewer readback stalls
                              over OCuLink, but some flicker was reported
 ab-8-skip-unbuilt-draws.bat  skips a draw while its new pipeline compiles instead of waiting
                              (EDEN_NX2_ASYNC_DISCARD=1): fewer hitches on new content, an object
                              can be missing for a few frames; logs like perf-trace.bat
 ab-9-amd-fp16.bat            keeps float16 shader math on AMD (EDEN_AMD_FP16=1); official builds
                              turn it off for a driver bug in another game, so look for wrong colors
 ab-10-run-ahead.bat          the game prepares the next frame while the GPU renders this one
                              even when the GPU is behind (EDEN_NX2_FRAMES_AHEAD=2), so a slow
                              GPU does not idle between frames; logs like perf-trace.bat
 perf-trace.bat               default settings plus GPU thread zones (every 10 s) and frame timing
                              (every second) in the log (EDEN_NX2_GPU_PROFILE=1,
                              EDEN_DKB_FRAME_TRACE=1)

The log (eden_log.txt in the emulator's Log folder) starts with a "Hybrid CPU:" line and one
"Core placement:" line per thread, so you can check what each launcher did.

PGO training build (the "pgo-train" download)
---------------------------------------------
This build records which code your game runs, so the next "pgo" build can be optimized for it.
It runs slower than the others; that is expected. Run lichii.exe from it, play Ocarina through
a few typical areas for 10-15 minutes, then stop the game and close the emulator normally. It
leaves lichii-pgo-<number>.profraw files next to lichii.exe (it also saves them every 30 s).
Zip them and upload the zip to lichii-tuned/pgo in the fork; the next builds use them.
