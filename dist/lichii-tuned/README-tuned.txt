Lichii tuned for Intel Core Ultra 200H (Arrow Lake-H) + AMD Radeon RX 9070 XT
==============================================================================

Test build of Lichii from github.com/DahBard/Blitchii (a private repository), branch
"tuned". Same emulator and the same user data folder as the official Lichii builds,
%APPDATA%\lichii (Fritchii and build 132 keep %APPDATA%\eden; see "First launch: import"
below), plus:

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
 * Shaders stay compiled: every cached pipeline is compiled at boot, not just the ones the last
   sessions used (EDEN_NX2_PRELOAD_ALL_PIPELINES=1 in nx2_switches.txt, which overrides the
   "Build all cached shaders before game start" checkbox). Built pipelines are kept up to a
   quarter of the RAM, and one evicted past that is built again from its kept SPIR-V without
   translating its shaders (upstream since the 2026-11-12 merge).
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
 * Ocarina's AMD FSR and AMD frame generation are CheezyEmu's (CheezerWheezer's Lichii fork
   for AMD GPUs), since round 22. Upstream's FSR 3.1/FSR 4 with its camera jitter and texture
   LOD bias are gone (with LICHII_FSR_JITTER and the FSR 4 download panel), and so is the
   shimmer at the screen borders. FSR runs without jitter. The settings are in Graphics, in the
   "AMD FSR & Frame Generation" box:
   - "Use AMD FSR 4 in Ocarina of Time" (on by default) replaces the game's upscale pass with
     AMD FSR 4 through AMD's own runtime: the amd_fidelityfx_*.dll files next to lichii.exe
     (amd_fidelityfx_license.md is their license). Docked, the game renders 1440p and FSR
     writes 4K. Without FSR 4 for the GPU it uses AMD's FSR 3.1. If FSR 4 draws a black
     picture, it switches to FSR 3.1 for the session. Needs Resolution 1X. The "RCAS
     Sharpness" slider sets its sharpening.
   - "Ocarina of Time FSR preset" (Native by default) renders the 3D scene smaller and lets
     FSR upscale it: Quality, Balanced, Performance or Ultra Performance. Native AA renders at
     the output size. Docked with 4K output, Quality is 2560x1440 and Performance 1920x1080.
     The preset patches the game's render scale when the game starts.
   - "Ocarina of Time docked output": 4K (the default), or 1440p, which is lighter and makes no
     4K image.
   - AMD frame generation: tick "Enable Frame Generation", pick "AMD FSR Frame Generation
     (Ocarina, restart)" as the type, and restart Lichii. It doubles the frames on screen
     through AMD's own swapchain and pacing (upstream's frame generation pacing applies only to
     the built-in Optical Flow and LSFG types). "AMD Frame Generation Version" is Auto (FSR 4's on the
     RX 9070 XT) or FSR 3.1 (lighter). The HUD is masked, so it stays still in generated
     frames. Since r59 it also runs with HDR output in the HDR10 format (Graphics > HDR
     format), through the same AMD swapchain, which hands the display Lichii's HDR10 metadata.
     With scRGB it stays off and the FG button shows "(SCRGB)"; if AMD's swapchain can't start
     with HDR, Lichii presents HDR without frame generation and the button shows "(HDR)" (the
     log's "AMD frame generation" lines say why; AMD Frame Generation Version FSR 3.1 is worth
     a try then).
   The status bar, and the performance overlay in fullscreen, show "FSR 4.x.x" while FSR
   upscales. When it doesn't, they show "FSR (reason)", for example RES 1X, or NO CAMERA in
   menus. Clicking it turns FSR on or off. With AMD frame generation the FG button shows
   "FG AMD FSR 2X", the frame rate reads like "Game: 60 FPS + 60 generated", and the
   overlay's footer shows "FG +60" with the FSR/FG line under it.
 * Since the upstream merge of 2026-11-12, the first launch switches the theme to Lichii Dark
   and shows a What's new dialog, once each (Configure > General > UI switches the theme back).
   HDR output (Configure > Graphics) is Off by default; since r59 AMD frame generation runs
   with HDR output in the HDR10 format (not scRGB).
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
   before game start": now Always; Ask or Off there). Those builds are compiled for AMD Zen 3
   CPUs (-march=znver3), which is why they may not start on Intel CPUs; this build stays
   compiled for any x86-64-v3 CPU.
 * Upstream now pins the GPU threads to cores of their own by default (GraphicsExtra > Hacks,
   "Pin GPU threads to their own cores"); this build leaves it off: on the Core Ultra 7 255H
   it would put the emulated CPU cores on the four P-cores left instead of spreading them over
   P- and E-cores.
 * Round 18: AMD FSR 4 (and its FSR 3.1 fallback) now gets a camera jitter: each frame the
   scene is drawn shifted by a different fraction of a pixel and FSR is told the shift, so it
   collects new detail over the frames instead of the same samples (sharper edges and fine
   detail, less crawling). Removed in round 22 with CheezyEmu's FSR, which runs without
   jitter (the jitter made the screen's edges shimmer, most visibly with water). Upstream Lichii's newest commits are in too, among them cheaper
   per-draw work on the GPU back thread (texture lookups reused while a pipeline's textures
   stay the same). The run also has a "pgo-train" download again: 10-15 minutes of Ocarina
   with it records a profile that makes the next builds' own code faster on this CPU (see the
   end of this file).
 * Round 18, for the emulated CPU cores that now hold the frame back (upstream pull request
   #717): the code generated for the game's CPU code is tighter (fewer flag reloads, smaller
   jump tables and exits, folded addresses, fewer register spills), reads and writes of
   memory the GPU caches take a faster path, the emulated kernel's locks and waits spin
   briefly before sleeping, and Ocarina gets three patches that do game work on the host:
   decompressing its zstd data, comparing strings 8 bytes at a time, and backing off its
   resource polling. Each can be switched off: ab-15 turns all of it off, ab-16 only the code
   generation, ab-17 only the Ocarina patches (also in the game's Add-Ons). Left out: #717's
   atomic instructions (a reported crash at boot) and its thread pinning.
 * Round 18, for the GPU threads (upstream pull request #805, "Ocarina of Time holds 60 fps on
   AMD" on its author's PC): the front GPU thread no longer waits for the back one to finish
   everything queued before it decides a semaphore wait, reads memory for a macro or reads a
   command list the GPU wrote (r27: about 660 ms of every second of waiting). It keeps its own
   view of the semaphores the GPU releases, runs memory-reading macros on the back thread,
   waits only for the words GPU work wrote, and lets the GPU read compute parameters it wrote
   itself; uploads are batched into fewer commands (upstream's #1245 since round 22, always
   on). ab-18 brings back round 17's behaviour for the rest, verify-r18 checks the shortcuts
   against the slow way while you play.
 * Round 20, from upstream Lichii's open pull requests: the GPU thread draws Ocarina's
   culled mesh draws from the counts the GPU wrote, without waiting for them (#980, about 2,000
   waits a second); one channel's semaphore release frees the channel waiting on it when the
   GPU thread reaches it, not a host GPU round trip later (#911); 8-64 MiB images come from
   256 MiB memory blocks instead of one allocation each (#922, +6% fps measured on an
   RX 9070 XT); fewer redundant vertex and index buffer binds (#955, #954); cheaper polling
   of a suspended list copy (#933); fast draws survive writes that change nothing (#974);
   compute dispatches continue while a pipeline is rebuilt (#969); texture descriptors read
   without a flush during shader translation (#970); task/mesh pipelines built a few at a
   time (#998); the Castle Town guard house fix (#976: a built-in Texture Pool x2 patch,
   on in Add-Ons, and superseded images freed after 4 frames); smaller fixes (#957, #939,
   #1005, #1008). The pgo-train build loads the game again (#1002's part that keeps the
   game-file decoder out of the profile instrumentation). From upstream's main branch: guest
   memory pages that only hold zeros are taken out of RAM (#961; Ocarina boots with ~7 GB of
   them), and the texture cache limits are logged only when they change (#951).
 * Round 20, from Fritchii v5.2: core timing waits on a high-resolution timer on Windows
   (since round 22 upstream's own version, #1016), every ASTC decode and unswizzle store is bounds-checked
   (AMD corruption and device loss), staging buffers may go over the memory budget instead of
   aborting, guest writes wait for a GPU download landing under them instead of being
   overwritten, and the pipeline cache is more robust: background builds stop when the game
   stops, the driver's cache is written during long builds, a slow shutdown that still makes
   progress is waited for instead of killed, and pipelines other builds stored are counted
   instead of logged as errors.
 * Round 20, Fritchii's shader preparation: this package ships Fritchii's precompile recipe for
   Ocarina of Time v131072 (recipes\OoT_v131072_cache_v54.nx2recipe, next to lichii.exe). With
   shader precache at Always (the default), before the game starts every pipeline the game
   is predicted to draw is built once, so the GPU driver has them compiled before
   they are first drawn (about 15 minutes, once per GPU driver and build; Fritchii measured
   Ocarina's worst walking freezes going from ~700-850 ms to 40-70 ms). As in Fritchii, a
   pipeline recorded for a GPU with other features is skipped, not converted (r34 and r35
   converted them), except Fritchii's own case: one recorded with a dynamic logic op while the
   logic op is off is built with this driver's static one (most of the recipe on AMD). Keep the
   recipes folder next to lichii.exe; other games' recipes can go there too. EDEN_NX2_PRECOMPILE=0 turns it off. If a crash ends the preparation, the next
   launch skips the pipelines it was building (upstream #999). Upstream #820's emulation of
   noperspective varyings is reverted, as in Fritchii (it also caused white fog at the Hyrule
   Field drawbridge there). r34's and r35's preparation crashed near its end because of
   something else: on AMD's driver, its compute jobs dropped pipelines whose build was still
   queued, and the queued build then ran on freed memory. Fritchii doesn't queue these builds.
   r36 builds them in the job itself.
 * Crashes: since r60 upstream's crash handler writes a "Host crash:" line (where it crashed
   and the call stack) into %APPDATA%\lichii\log\eden_log.txt and eden_crash.dmp next to it.
   Send both. It replaces this package's crash_dumps writer.
 * Round 20, more of Fritchii v5.2, each group with an A/B launcher that brings round 19 back:
   - Shaders (ab-22): a new pipeline is translated on two worker threads instead of the GPU
     thread, so a new effect stalls the frame less; with "Switch 2 draws with unbuilt
     pipelines" at Wait, a draw whose pipeline is still building is skipped only into targets
     the game clears every frame, so menus and one-time effects still wait (the default is now
     Bounded: up to 8 ms of waiting per frame, then a skip).
   - Guest memory (ab-23): the emulated CPU's reads and writes of memory the GPU never wrote skip
     the caches' locks, and its writes reach the GPU before the next command list, semaphore or
     copy; the uniform bank check skips its once-a-frame compare; two #717 trims; cache
     invalidations cover exactly the bytes written; the 12 GB guest memory section is committed
     as the game uses it instead of all at boot.
   - Render targets (ab-24): a render target the game moves is relocated instead of rebuilt, and
     the chain images left behind are freed after 4 frames, timed by a render-frame clock
     (Fritchii measured about +3 fps in Castle Town).
   verify-r20 checks these shortcuts against the slow way while you play.
 * r50:
   - The zero-page passes (#961: guest pages holding only zeros leave RAM) cost less. A page
     check stops at its first non-zero bytes, a pass that finds little lets the next one wait
     up to 5 minutes instead of 1, and every pass over 100 ms is logged with where its time
     went ("Zero-page pass" lines). In r49 each pass took about 1.35 s and lined up with 1-2 s
     of 25-30 ms frames. From Fritchii; ab-25 turns the passes off for comparison.
   - Ocarina's water ripples keep running when a frame isn't a whole 1/60 s, with FPS Unlock
     or Dynamic FPS (upstream Lichii's open pull request #1180). At 43-58 fps they ran at
     72-97% speed; in frames shorter than 1/60 s they stopped, and the water's edge around
     Link went jagged.
   - perf-trace's "Scheduler per second" line also counts the GPU submissions with nothing
     new in them, to see whether skipping them (Fritchii's EXP61) would help here.
   - The first build optimized with your recorded Ocarina PGO profile.
 * r51, aimed at the GPU back thread (18-21 ms a frame against a 16.7 ms budget):
   - Upstream Lichii #1216: a vertex stream's stride word, the render enable addresses and the
     back macros' calls no longer end the cheap repeat draws, which made about 28,000 draws a
     second take the full path at Kokiri. ab-26 brings r50's behaviour back.
   - Upstream #1251's layout of the split GPU thread's queue: the two threads stop pulling the
     same cache line back and forth (no switch; compare against r50 on the same route).
   - Small upstream speedups: no second flush after each fence, the driver's memory budget asked
     at most every 50 ms, flat texture lookup tables, no copies of compute image handles; and
     no cache gather at fences when the CPU wrote nothing.
   - Upstream #717's check: storage bindings in one piece skip a page walk of up to 1 GiB.
   - More logging with perf-trace: which kinds of state change end repeat draws, compute
     dispatch steps and slow dispatches, fence and lock times, and a one-time "Stream buffer
     write rate" line. perf-trace-noprofile logs the frame trace alone, for the frame rate you
     actually get; verify-r51 checks the repeat draws and back macros against the slow way.
     In profiled logs the back macros' 3D writes now count under "deferred call", not
     "method", so compare those two zones with r50 only through the unprofiled frame times.
 * r52, the next round for the GPU back thread:
   - Upstream #1161's repeat draw changes: a long run of constant writes no longer ends the
     cheap repeat draws (the "delta overflow" breaks), writes next to a draw's texture handles
     no longer count as handle changes, and vertex attribute format changes keep them when the
     pipeline is the same. ab-29 turns them off.
   - Upstream #1274: the instances of an instance loop reach the GPU back thread as one count
     instead of ~380,000 writes a second.
   - The bank check skips the pages no write touched in bulk.
   - Upstream #1161's report and fence trims: no memory allocation per query report, reused
     fence lists, no buffer cache lock for reports nobody reads back, the query pool reset off
     the GPU thread. ab-32 turns off the switchable ones.
   - Upstream #1306 and #1311: the texture collector frees faster the further past its limit,
     for the device-memory crash breaking pots in Hyrule Castle.
   - perf-trace logs what each change does, and verify-r52 checks them against the slow ways.
 * r55, more for the GPU back thread (build C, part 1):
   - Upstream #1161's last repeat draw change: draws whose textures the shader picks from data
     in memory (about 31,000 a second at Kokiri's water) take the cheap repeat path again; each
     one checks that data first. ab-36 turns it off.
   - perf-trace splits "compute FSR" (the FSR 4 dispatch) and "compute synchronize" into their
     steps, and the one-time "Stream buffer write rate" line measures for real, with small
     copies too. perf-trace-fine adds the finer zones (slower).
   - The log names every TEST switch, and the A/B launchers' other switches (zero-page passes,
     kernel spinning, and since option 3 below the GPU thread pinning, UMWAIT and its C0.2
     state, the idle poll and the clock trace), and whether each is on ("TEST switches in
     effect:", when the GPU starts), so a launcher whose switch did not take effect shows there.
   - Draws no longer read the clock for a time only perf-trace and the verify launchers log
     (two reads a draw, about 4,000 a frame).
   - Heap trace: in the second area after a load the same build runs 24-44 fps from session to
     session, because the game's 550-715 memory maps and unmaps a second cost the host 143-342
     microseconds each. perf-trace, perf-trace-fine, perf-trace-noprofile, the unprofiled A/B
     launchers (ab-26 to ab-41) and ab-25 (no zero-page passes, one suspect) now log where that
     time goes every 10 s ("MapPhysicalMemory per second" and "UnmapPhysicalMemory per second"
     lines: the lock wait, allocation (with the memory it committed), zero fill, page table and
     Windows mapping times (with the regions mapped per call), the freeing of unmapped pages,
     page faults, the RAM free and commit charge, and what the trace itself costs;
     EDEN_NX2_HEAP_TRACE=1).
   - perf-trace splits its "publishes to an idle back late by" time by why the words waited.
   - verify-r55 checks the repeat draws while they run.
   - Option 3, for that power limit. The laptop's CPU runs at its power limit all the time;
     HWiNFO's effective clocks for its P-cores, where the GPU back thread that holds the frame
     back runs, were 2.6-3.3 GHz (they count idle time too). Spinning less, pinning threads or
     another power mode may give that thread more clock; these launchers measure it before
     bigger changes to the back thread's work.
     The clock trace (EDEN_NX2_CLOCK_TRACE=1, now in perf-trace, perf-trace-fine,
     perf-trace-noprofile and ab-25 to ab-41) logs a "Clock trace
     (EDEN_NX2_CLOCK_TRACE):" line a second after the game starts: the CPU's P-cores, E-cores
     and low-power E-cores with their base clocks, the power plan, the power mode (and the one
     chosen in Settings, on Windows 11 23H2 and later), AC power, battery saver, the battery and
     the plan's processor settings. Then every 10 s a "Clock trace per second" line: for the GPU
     back thread (back), the GPU thread (front), the Vulkan worker, the emulated CPU cores
     together and the one with the most work, the average clock of the cores it ran on while
     they were not halted (ghz), its CPU time a second (cpu_ms) split into polls (poll_ms: its
     waits for another thread before it sleeps, which Windows counts as running) and the rest
     (work_ms), how much of it was on P-, E- and low-power E-cores (p, e, lpe) and how often it
     was seen on another core (migrations, at least that many); for each kind of core how busy
     it was (busy), its utility (util: busy times its clock over its base clock, so above 100%
     in turbo) and its clock, with each P-core as clock@busy; the CPU's power in watts where
     Windows shows it; the power mode, AC, battery saver and battery (changed=yes when one
     changed since the line before); and what the trace's own thread costs.
     ghz counts whatever else ran on those cores, polls included, and leaves out the halted
     time HWiNFO's effective clock counts, so it reads higher than HWiNFO: compare the arms
     with each other, not with HWiNFO. The back's and the front's cpu_ms, and
     gpu_back_cpu_ms_per_frame, include their polls for each other, and the arms change exactly
     that waiting: judge an arm first by its frame times, then by the back's work_ms per frame
     and the P-cores' ghz at the same package power.
       ab-40  less spinning all at once: the emulated kernel's locks and waits sleep at once
              instead of spinning first, waiting cores idle in the deeper C0.2 state instead of
              C0.1 (an idle emulated core waits its whole poll at once rather than in
              4-microsecond pieces), and an idle emulated core polls 30 microseconds before it
              sleeps instead of 100 (shorter, not gone: without the poll another game's frames
              took 42 ms instead of 34-35)
       ab-41  the 30-microsecond poll alone: only if ab-40 helps
     How to run them. One run per arm cannot tell a few percent from the difference between
     sessions (the same build ran 24-44 fps after loads from session to session), so:
     - Every run: plugged in, the battery full or held by Vantage's conservation mode so that
       it does not charge, the same Windows power mode (Best performance) and Lenovo Vantage
       mode, frame generation at AMD FSR 2x, HWiNFO and other monitors closed, two minutes idle
       before the launch, the same save and the same 3-4 minute route through a heavy area, not
       the area right after a load. Quit normally and copy eden_log.txt with the launcher and
       the Vantage mode in its name (the log cannot read Vantage).
     - Once, before the arms, not counted: one baseline run with HWiNFO open, to compare its
       "Core Clock" averages (not "Core Effective Clock") for P- and E-cores with the trace's P
       and E ghz over the same stretch. The first "Clock trace" line names the kinds of core
       whose base clock is not the TSC rate: if their ghz is off by that ratio, say so.
     - Order, with N for perf-trace-noprofile: N once to warm up, not counted; then N, ab-40,
       ab-40, N, and that block again if there is time: two runs of each could not tell
       ab-40's 2-4% from the spread between runs at full power.
     - Compare the 10 s windows of the route only: leave out the first ones after the game
       starts (the trace starts during boot and the pipeline preload), any with a load, and
       any whose map and unmap times (heap trace) stand out. Take each arm minus the N of the
       same block for median fps, 1% lows, the back's work per frame, P-core ghz and package
       power.
     - An arm helps only if every block agrees and its mean gain is larger than N's spread
       between blocks: with two or three blocks, about 5% or more.
     Compare these runs with this build's own baseline only, not with r54's: the PGO profile
     was recorded on r49, and r55 changes more of the code it covers (the GPU back thread's
     loop among it), so comparing r55 with r54 needs a profile recorded with r55's pgo-train
     first. The "TEST switches in effect" line and the first "Clock trace" line show what each
     run had.
 * r58, the stable build: r55's defaults, without the test switches that are settled, on
   upstream Lichii nx2 2eb5f081c (r57 was that merge alone).
   - Permanent, without their A/B launchers: r51's small speedups and #717's check, r52's
     instance folding, the bank check's bulk skips and #1306.
   - Removed: r52's per-draw trims and per-thread GPU page translations, off since r55 (no gain
     in r53 and r54).
   - Option 3 ends with r55's defaults, which passed the 50 fps test in medium play with
     Windows in Best performance. The GPU-threads-only pinning arm (frames 27-34% longer) and
     the arms that tried no kernel spinning or C0.2 alone are gone; ab-40 and ab-41 stay for a
     later A/B in Best performance, and EDEN_NX2_PIN_GPU_THREADS=0 or 1 still overrides the
     "Pin GPU threads to their own cores" setting for one run.
   - Each game start logs Windows' power mode next to the "TEST switches in effect" line: the
     mode in effect and the one chosen in Settings, AC power or battery, the power plan and its
     boost mode and energy performance preferences ("Windows power mode" line). In "best power
     efficiency" on AC power a warning follows it (see the settings below).
   - The verify launchers' "Uniform keeps" line counts each kind of mismatch: no longer
     streamed, and changed since the previous draw because the slot read another range or the
     command buffer is older (the keep itself refuses both) or because the bytes are stale. Each
     kind logs its first 16 every 600 frames.
   - verify-ports-off is verify-r55 with the switches of ab-26, ab-29 and ab-36 off: if the
     uniform keep mismatches stay at verify-r55's rate, they don't come from those three ports,
     and their switches can go.
   - From CheezyEmu: Ocarina's FSR keeps its list of moving characters between frames instead
     of building it anew each frame (two poses of 13 KB per character), which saves the GPU
     back thread a few tenths of a millisecond a frame (the FSR actors zone).
   - No NX converter in this build: upstream's library for it links only with MSVC.
     Loading .nx games is not affected.
 * r59: r58 with AMD frame generation under HDR output (HDR10; see the frame generation notes
   above). Nothing changes with HDR off.
 * r60: upstream Lichii build 171 merged (69 commits since build 160), with r59's HDR frame
   generation:
   - Ocarina's image: light-table shadow maps read through storage buffers at binding 32 or
     above, and dual-vertex programs read their own textures (#1485); light tables of records
     captured with a null pointer no longer sample a random render target (#1523); no one-frame
     white flashes from stale material constants (#1459); a later write on a page with GPU data
     is no longer lost (#1460).
   - Speed (upstream's measurements): command lists are copied on their own thread (#1520); the
     GPU thread waits in hardware or sleeps instead of polling waiting channels (#1376); idle
     guest cores wait in C0.2 with a poll that shrinks when it doesn't pay (#1373); fewer
     page-protection calls (#1436, #1518), cheaper reports and fences (#1515, #1516), constant
     data written straight to the bound buffer with its keep gaps closed (#1441, #1521), less
     work per new shader (#1468) and less JIT compile time (#1517).
   - Stability: the zero-page trim no longer ends a game when Windows runs out of commit
     (#1451); long sessions keep their SMMU space (#1458, #1528, #1532); stopping a game no
     longer logs a Userspace PANIC (#1450).
   - Shader loading: no new preparation ("prepared for this driver already"). The first boot is
     slow as with every build and lowers the v2 records again once. After a crash or kill during
     the loading screen, the next launch builds no cached pipelines and doesn't prepare; the one
     after resumes (#1505).
   - VRAM: images used in the last 150 frames stay until the card runs short (#1435); with
     #1306 that is up to ~1 GiB more in use before the 3-frame age takes over.
   - The verify launchers' "Uniform keeps" line counts "no longer streamed" and "changed since
     the previous draw": #1521's verify checks only the ranges a keep binds again.
   - Launchers whose switches are gone (a cleanup follows): ab-23, ab-29, ab-32 and ab-36 no
     longer change anything; ab-1, ab-5, ab-15, ab-40 and ab-41 no longer change the idle poll
     (EDEN_NX2_IDLE_SPIN_US is now its limit).
   - Upstream renamed its yuzu names, folders and targets to Lichii; lichii.exe, the data folder
     and the settings are unchanged. New in Configure: a Nextendo sign-in box and an opt-in host
     redirect (Network; online play stays off), two unchecked Age of Imprisonment options, and
     Help > Apply Recommended Settings also resets per-game overrides of the table's settings
     (backed up; Restore brings them back).
 * r61: r60 from the new private repository, with 35 of the test launchers removed (the list
   below has the 13 left; verify-r55.bat is now verify.bat). The emulator itself is r60's.
 * Recommended speed settings, applied once on first start (change them freely afterwards):
   GPU accuracy Low, asynchronous shaders on, reactive flushing off, asynchronous presentation
   on, GPU video decoding, shader precache Always, Switch 2 draws with unbuilt pipelines
   Bounded, VRAM usage Aggressive, and "Ocarina of Time anti-aliasing" = No sharpening
   (Graphics settings; "Game" brings the game's network back). Help > Apply Recommended
   Settings keeps these.
 * Windows power mode: Best performance while plugged in (Settings > System > Power & battery >
   Power mode). In "best power efficiency" the clock trace showed the E-cores at 1.1 GHz, and
   Lichii ran slower. Energy performance preference 0 is optional, a small gain: powercfg
   /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR PERFEPP 0, the same for PERFEPP1, then
   powercfg /setactive SCHEME_CURRENT.

The "msvc" download is the same source built like the official MSVC build (MSVC, no PGO, no
CPU tuning flags, PAUSE spinning instead of UMWAIT). Use it to tell whether a graphical glitch
comes from the compiler (only the pgo builds show it) or from the source.

A/B launchers (this build with one change switched off or on; compare the same scene each time).
r61 removed the A/Bs whose questions are settled or whose switches are gone, the old traces and
the verify runs before r55; the notes above still name some of them for the builds that had them.
 ab-6-official-texture-cache.bat  texture cache collects from 5 GiB, as official Lichii did
                              before 2026-11-08
 ab-11-official-shader-cache.bat  boot compiles only what the last sessions used
                              (EDEN_NX2_PRELOAD_ALL_PIPELINES=0)
 ab-16-round17-jit.bat        #717's code generation off, logged like perf-trace.bat: the first
                              test when the emulated CPU crashes or glitches
 ab-17-no-oot-patches.bat     the Ocarina patches off (zstd, string compares, poll backoff, wider
                              core masks), logged like perf-trace.bat
 ab-25-no-zero-trim.bat       no zero-page passes (EDEN_NX2_ZERO_TRIM=0): pages holding only
                              zeros stay in RAM (about 7 GB more in Ocarina), logged like
                              perf-trace.bat: compare the frames around perf-trace's
                              "Zero-page pass" lines, the 1% lows and the heap trace's lines
 ab-26-no-1216.bat            repeat draws end at stride words, render enable addresses and
                              back macro calls as in r50 (EDEN_NX2_TEST_1216=0); unprofiled:
                              compare with perf-trace-noprofile.bat
 trace-no-fsr.bat             Ocarina's own upscale pass instead of AMD FSR, no 4K output
                              (EDEN_NX2_DLSS_REPLACEMENT=native), logged like perf-trace.bat
 trace-real-occlusion.bat     runs the GPU occlusion queries again (LICHII_FAKE_OCCLUSION=0),
                              logged like perf-trace.bat: slower, for comparison
 verify.bat                   checks this build against the slow ways while you play, with
                              perf-trace's logging ("Native bank check" missed and bitsets
                              missed, "Read download check" differing bytes, the delta descriptor
                              and uniform keep mismatches should stay 0, no "Nx2Perf" errors);
                              slower. It was verify-r55.bat
 perf-trace.bat               default settings plus GPU thread zones, emulated CPU cores, frame
                              times and the heap and clock traces in the log
                              (EDEN_NX2_GPU_PROFILE=1, EDEN_DKB_FRAME_TRACE=1,
                              EDEN_NX2_MME_DMA_STATS=1, EDEN_NX2_HEAP_TRACE=1,
                              EDEN_NX2_CLOCK_TRACE=1). Draws are timed one sampled frame in four
                              (LICHII_TRACE_DRAWS_EVERY=4). The launchers "logged like
                              perf-trace.bat" have its profile and frame trace, but only ab-25
                              also has the heap and clock traces, which r55 added here
 perf-trace-fine.bat          perf-trace.bat plus the fine zones and counters
                              (EDEN_NX2_GPU_PROFILE=2): slower, for where the time goes
 perf-trace-noprofile.bat     default settings with only the frame, heap and clock traces
                              (EDEN_DKB_FRAME_TRACE=1, EDEN_NX2_HEAP_TRACE=1,
                              EDEN_NX2_CLOCK_TRACE=1; the heap and clock traces since r55):
                              gpu_back_cpu_ms_per_frame and frame times without the zones' cost;
                              the baseline for ab-26, which logs the same
 frame-times.bat              default settings with only each frame's time logged
                              (LICHII_FRAME_TIMES=1): the 1% lows without the other logging

Each start replaces the log, so copy it after every run you want to compare.

The log (eden_log.txt in the emulator's Log folder) starts with a "Hybrid CPU:" line and one
"Core placement:" line per thread, so you can check what each launcher did.

First launch: import
--------------------
Lichii builds now keep their data in %APPDATA%\lichii (upstream's change), and this build shares
that folder with the official Lichii builds; Fritchii and build 132 keep %APPDATA%\eden. The
first Lichii start with the new folder offers to import an older profile, once for all of them.
Close Fritchii and every other Lichii or Eden window first, then:
 1. Pick Eden, then "Keep Old Data": it copies, and %APPDATA%\eden stays as it is.
 2. Leave "Clear shader cache" unchecked, so the prepared shaders come along.
 3. After "Data was imported successfully", close Lichii and start it once more (the import
    start doesn't read your nx2_switches.txt).

PGO training build (the "pgo-train" download)
---------------------------------------------
This build records which code your game runs, so the next "pgo" build can be optimized for it.
It runs slower than the others; that is expected. Run lichii.exe from it, play Ocarina through
a few typical areas for 10-15 minutes, then stop the game and close the emulator normally. It
leaves lichii-pgo-<time>-<number>.profraw files next to lichii.exe (saved every 30 s).
Zip them and attach the zip to a new pre-release in the fork whose tag starts with "pgo-"
(for example pgo-r49): GitHub > Releases > Draft a new release, target the tuned branch, and
tick "Set as a pre-release". Zips over 25 MB are fine there; release files can be up to 2 GB.
The next builds download every pgo-* release's zip and merge it with Lichii's own profile,
weighted 4:1 toward the recorded game. If the compiler can't read an old profile, the build
carries on with Lichii's own.
