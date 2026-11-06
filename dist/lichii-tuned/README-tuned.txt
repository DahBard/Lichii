Lichii tuned for Intel Core Ultra 200H (Arrow Lake-H) + AMD Radeon RX 9070 XT
==============================================================================

Test build of Lichii (github.com/zortos293/Lichii) from github.com/DahBard/Lichii, branch
"tuned". Same emulator and the same user data folder as the official build, plus:

 * Compiled by clang with Lichii's PGO profile, for x86-64-v3 (AVX2, BMI2, FMA) and tuned
   for Arrow Lake. It runs on any CPU with AVX2 (Intel Haswell / AMD Zen and newer).
 * Hybrid CPU core placement (Windows): the GPU command threads (GPU, GPUBack, VulkanWorker)
   run on the P-cores, the emulated CPU cores on P- and E-cores, pipeline builds and core
   timing on E-cores, and no emulator thread on the two 2.5 GHz low-power E-cores.
 * Intel WAITPKG waits: the GPU threads and idle emulated cores wait for each other with
   UMWAIT (the core idles until the other thread writes) instead of spinning on PAUSE, which
   leaves more of a laptop CPU's power budget to the busy cores.

A/B launchers (this build with one change switched off; compare the same scene each time):
 ab-1-stock-threading.bat     no core placement and no UMWAIT: only the compiler changes remain
 ab-2-no-core-placement.bat   Windows places every thread (LICHII_CPU_POLICY=off)
 ab-3-soft-placement.bat      only keeps threads off the low-power E-cores (LICHII_CPU_POLICY=soft)
 ab-4-strict-placement.bat    emulated CPU cores on P-cores too (LICHII_CPU_POLICY=strict)
 ab-5-no-umwait.bat           PAUSE spinning as before (LICHII_WAITPKG_SPIN=0)
 perf-trace.bat               default settings plus per-second GPU thread zones and frame timing
                              in the log (EDEN_NX2_GPU_PROFILE=1, EDEN_DKB_FRAME_TRACE=1)

The log (eden_log.txt in the emulator's Log folder) starts with a "Hybrid CPU:" line and one
"Core placement:" line per thread, so you can check what each launcher did.
