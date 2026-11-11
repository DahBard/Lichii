# Lichii tuned: project handoff

State of the work on 2026-11-10 (round 22), for the next Claude session.

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
  compiled before playing. Every build ships `EDEN_NX2_PRELOAD_ALL_PIPELINES=1` in its
  `nx2_switches.txt` (the owner's request, round 14: each new download's copy lacked it, and r22
  booted with 6,313 of the ~19,400 cached pipelines).
- The log is `%APPDATA%\lichii\log\eden_log.txt` since the nx2 merge (#1253's data folder;
  before it `%APPDATA%\eden\log`). The previous launch's log is `eden_log.txt.old.txt`.
- Sources (the owner, round 20): upstream Lichii (zortos293/Lichii nx2 and its pull requests) and
  Fritchii (Frizti/Lichii, branch `oot-gather-fix`, tags `fritchii-v*`) are the main influences;
  take from other forks only for a very good reason or when the owner names one.
- Windows only since 2026-11-11. The owner: "drop CachyOS, we won't be building towards Linux in
  the future". The CachyOS job, `README-cachyos.md` and `make_linux_launchers.py` are gone; the
  round 16-21 notes below describe the old job.

## How builds are made

- Since round 22 (2026-11-10): branch `tuned` = upstream Lichii nx2 `17baed716` (build 132 plus
  31 commits, merged in `2d9ea59c5`) + the workflow + this folder. Round 21's Fritchii tree
  (branch `tuned5`) stays in the history. The patch is `git diff --binary 17baed716 tuned4`
  (393 files, 2.9 MB), backed up in `tuned4.bundle` (`git fetch lichii-tuned/tuned4.bundle
  tuned4:tuned4` in a clone that has zortos293/Lichii nx2 up to `17baed716`). Variants: `pgo`, `pgo-lto` and `pgo-train`;
  `msvc` is off again. The 'AMD FidelityFX libraries' step puts AMD's signed FSR and frame
  generation DLLs (SDK v2.4.0, SHA-256 checked) next to lichii.exe for CheezyEmu's FSR port.
- Branch `tuned` of DahBard/Lichii = upstream nx2 `17baed716` (round 22's merge; `d2eaae4f3` = build 132 before it; `cbb512ee7` merged in round 19, `281477eac`
  in round 18; `ad477ef1f` = the 53 pull requests of 2026-11-08 in round 17, `bf4c42e22` = build 117 + 3 in
  round 14, `92f021240` = build 116 in round 13, `588bfa6` before) + `.github/workflows/lichii-tuned.yml` + this folder. The
  workflow applies `lichii-tuned/lichii-tuned.patch` and builds the Windows variants on every
  push to `tuned` that touches `lichii-tuned/**` or the workflow (a commit message with
  `[skip ci]` doesn't), and on a manual run (`workflow_dispatch`).
- Variants (windows-2022): `pgo` (MSYS2 UCRT64 clang, repo's `eglantine.profdata`,
  `-march=x86-64-v3 -mtune=arrowlake -ffp-contract=off`), `pgo-lto` (+ ThinLTO) and `pgo-train`
  (instrumented, writes `lichii-pgo-*.profraw`). `msvc` (built like the official MSVC download)
  has been off since 2026-11-07.
  - Recorded profiles: the owner zips them and attaches the zip to a pre-release tagged `pgo-*`
    (the first is `pgo-r49`, 92 MB).
  - Every non-train variant downloads them with `lichii-tuned/fetch_pgo_profiles.py` and merges
    them with Lichii's. So do files in `lichii-tuned/pgo/`.
  - Since r55 each recording is first cut to the last dump of each session
    (`lichii-tuned/profraw_last_dump.py`): a pgo-train build appends all its counts so far every
    30 s, and r50-r54 merged all 34 dumps of `pgo-r49`, the start of the session up to 34 times.
    The cut is weighted 70:1 (a whole file still 4:1) so that by counts `pgo-r49` stays 97% of
    the merged profile, as in r50-r54 (70 x 5.08e11 against eglantine.profdata's 1.04e12; at 4:1
    it would have been 66%). r55's PGO input still changed: the start of the session no longer
    counts up to 34 times.
  - A recording llvm-profdata can't read is left out with a warning; if none can be read, the
    build uses Lichii's profile alone. Artifacts: `lichii-tuned-r<run>-<variant>` and
    `lichii-tuned-r<run>-<variant>-symbols` (lichii.exe before stripping, for
    `lichii-tuned/symbolize_crash.py`). Each variant queues only behind the
  same variant of the previous run. ~40-50 min with warm caches.
- Removed on 2026-11-11: job `cachyos` (manual runs only since round 17; same workflow and run number; artifacts `lichii-tuned-r<run>-cachyos-<variant>`,
  each a tar.gz inside the zip so the executable bits survive): variants `pgo` and `pgo-lto` in
  the `cachyos/cachyos-v3` container (x86-64-v3 repos; the v4 one needs AVX-512) on
  ubuntu-24.04, with the runner's SDK folders deleted through volume mounts for disk space.
  pacman installs the docs/Deps.md Arch list plus clang/lld/llvm, qt6-svg, qt6-wayland,
  libpulse, alsa-lib. Clang: CachyOS's if its `llvm-profdata` reads `eglantine.profdata` (index
  format 14 = LLVM 23+), else LLVM 23.1.5's release tarball (SHA-256 pinned, only clang, lld and
  the llvm tools extracted, cached as `.llvm`; its binaries need only libc, libm, zlib, libstdc++
  and libgcc_s). Flags: the repo's "v3" preset `-march=x86-64-v3 -mtune=generic -ffp-contract=off`
  plus the PGO profile; system Qt, SDL3 and FFmpeg, bundled Vulkan headers, `Boost_USE_STATIC_LIBS`
  and `fmt_FORCE_BUNDLED` (CachyOS moves both sonames often). The package has the stripped
  `lichii`, `nx2_switches.txt` (checked for the preload switch), the Windows README,
  `README-cachyos.md`, `cachyos-packages.txt` (pacman owners of what `ldd` shows, plus the Qt
  Wayland and SVG plugins) and a `.sh` twin of every `.bat` launcher, written by
  `lichii-tuned/make_linux_launchers.py` (fails on any .bat line it doesn't understand). Round 16:
  run #24 failed on both platforms. CachyOS failed in pacman (`zlib` conflicts with CachyOS's
  `zlib-ng-compat`; dropped) and was rebuilt alone as run #25 (`workflow_dispatch` with
  `only: cachyos`; `windows` rebuilds the other). Windows failed on the host pacer's
  `#pragma comment(lib, "dwmapi.lib")` (MinGW clang: unknown pragma, an error under
  `-Werror=all`; video_core now links dwmapi in CMake) and was rebuilt alone as run #26. Run #25 built both
  CachyOS variants (~30 min each without a cache, on 4 cores) with CachyOS's own clang 23.1.4
  (no LLVM download) and its Boost 1.92 linked statically: 12 MB packages, 25 launchers, no
  missing library; run #26 built both Windows variants. Its `cachyos-packages.txt` still listed
  every library `ldd` resolves (~150 packages, FFmpeg's included); from round 17 it lists only
  the owners of the program's direct dependencies (`readelf -d` NEEDED), as the README expects. The cloud
  session can't test this job locally: its proxy blocks the CachyOS and Arch mirrors.
- Source of the patch: branch `tuned4` of DahBard/Lichii (r36 with build 132 merged in
  `0e30416f4`, round 22, CheezyEmu's FSR port up to `aae4566f5`, and nx2 `17baed716` merged in
  `853ff8213` with follow-ups up to `36948bc36`, and r50's changes up to `cd5482ece`, builds A, B and r54 up to
  `cbe51e88a`, and r55 up to `3bf40a3c2`; pushing it doesn't trigger builds).
  `lichii-tuned/tuned4.bundle` holds the same history as a backup (`git fetch
  lichii-tuned/tuned4.bundle tuned4:tuned4` in a clone that has zortos293/Lichii nx2 up to
  `17baed716`, as `tuned` does). The patch is `git diff --binary 17baed716 tuned4` (393 files,
  2.9 MB); check it
  with `git apply --check` on a clean `tuned` checkout before committing it. To move to a newer
  upstream: merge it into tuned4 and into `tuned`, then diff tuned4 against the merged upstream
  commit.
- Before turning `msvc` on, run `python3 -I lichii-tuned/check_patch_warnings.py <tuned4 checkout>`.
  It compiles the changed files with clang and GCC and reports warnings on added lines that the msvc
  variant treats as errors (shadowing, signed/unsigned compares, unused locals and captures,
  unhandled enumerators, video_core conversions); a changed header is compiled through two files
  that include it. The pgo builds don't catch them: `common/x64/xbyak.h` turns -Wshadow and
  -Wconversion off for the rest of every file that includes it. Qt, Windows-only and test files need
  a look by hand. Locally the files that need opus, stb, ReShade's `effect_module.hpp`, the generated
  host shaders, `windows.h` or SDL3 fail, and the cloud's clang 18 crashes on the three
  `nvhost_*gpu.cpp` files at any commit; ignore them.
- That script compiles for Linux, so `#ifdef _WIN32` code goes unchecked (round 16's pragma).
  Check it as the Windows CI compiles it: `apt-get install mingw-w64-x86-64-dev
  g++-mingw-w64-x86-64-posix`, then `clang++ --target=x86_64-w64-mingw32 -fsyntax-only` with
  the flags of a Windows job's compile line (`-std=gnu++20 -Werror=all -Werror=extra
  -Werror=missing-declarations -Werror=shadow -Werror=unused` and its `-Wno-` list), Boost from
  a folder holding only a `boost` symlink and FFmpeg from one holding only the `libav*`
  symlinks. The old MinGW headers there break `xbyak_util.h` (`EfficiencyClass`); MSYS2's don't.
  The header-only dependencies are the ones check_patch_warnings.py clones
  (`<deps>/<package>/<version>/include`). Round 17 checked its 22 changed files with Windows
  code or Windows-relevant changes this way: all clean.
- The workflow's "AMD FSR 4 libraries" step downloads AMD's signed `amd_fidelityfx_loader_vk.dll`
  and `amd_fidelityfx_upscaler_vk.dll` and the SDK license from GPUOpen-LibrariesAndSDKs/
  FidelityFX-SDK `7539ae8` (v2.4.0) into the package and checks their SHA-256 (binary
  redistribution with the license; nothing of AMD's is in the repository).
- The cloud session's network blocks GitHub archive downloads (so a full CMake configure fails)
  and the Actions log host; git clones work. Read job logs with the GitHub MCP `get_job_logs`
  tool: a large `tail_lines` saves the log to a file you can grep.

## What the tuned build contains (tuned5, from round 21; tuned4 below)

Round 22 (2026-11-10): back to tuned4. After the three-way test (Test results), the owner chose
to stay on the tuned fork: not Fritchii, and no fresh start on build 132. Priorities are in
CLAUDE.md. Plan, in this order:
1. Merge build 132 (`d2eaae4f3`, still the nx2 tip) into tuned4 on branch `r22-merge132`. The
   merge has 239 conflict hunks in 65 files. Upstream's code wins where both sides do the same
   thing: shader cache v2 (#1036); upstream's FSR 3.1/4 and jitter (#1060-#1062) in place of
   ours; #1056 and #1028 in place of our #805 merge; #949 and #979 in place of Fritchii v3's
   acquires.
2. FSR, frame generation and Radeon work from CheezerWheezer/CheezyEmu, vetted.
3. A shader preparation that completes once and persists.
Then `tuned` goes back to the tuned4 layout (upstream tree + workflow + this folder).

Step 1 done: `0e30416f4` (tuned4 = r36 + build 132; four agents resolved the conflicts by area).
- Upstream's code where both sides did the same thing:
  - Shader cache v2 (#1036). EDEN_NX2_PREBUILD_STREAM and EDEN_NX2_REKEY_FOREIGN_PIPELINES are
    gone (v54 loader only), and so is `pipeline_record_file`. The preparation, preload-all and
    async discard/translate sit on top.
  - Upstream's Ocarina FSR 3.1/4 and jitter (#1060-#1062) replace ours. LICHII_FSR4*,
    LICHII_FSR_JITTER and LICHII_FSR_REACTIVE are removed, with ab-14, ab-19 and trace-fsr31.
    Our always-on float8 and compute-derivatives device features are removed too.
  - #1016's HostTiming sleep replaces our high-resolution timer (EDEN_HIRES_CLOCK, ab-21).
  - #984 replaces our translation generations.
- The split's macro and read-back path keeps our later #805 merge as the base. Upstream's
  #949/#1056 are an earlier port of the same PR, and our files depend on the later API. On top
  it takes upstream's new pieces: #949's scheduler-ownership fix and latch, the #979/#1014
  mirror, #1028 read-back, #965/#975/#1039 profiling and the live-read check. Fritchii v3's
  acquires stay behind LICHII_TUNED4_ACQUIRES=1 (off).
- New upstream switches: EDEN_NX2_STUTTER_TRACE (+ _DETAIL_MS), EDEN_NX2_LIVE_READ_CHECK,
  EDEN_NX2_HOSTTIMING_STATS, EDEN_NX2_MEM_TRACE.
- Tuned defaults version 4 (frontend_common/config.cpp) seeds these once more: async shaders,
  async presentation, reactive flushing off, GPU video decoding, GPU accuracy Low and OoT AA No
  sharpening. Round 21's Fritchii runs had reset them in the shared config. It also seeds
  Ocarina's FSR off: upstream's FSR 3.1/4 is opt-in (Graphics: 'Use AMD FSR 3.1 upscaling in
  Ocarina of Time', 'Use AMD FSR 4 in place of FSR 3.1'), and its jitter, which has no off
  switch, is unchecked for the border shimmer.
- Local check: clang -fsyntax-only on 381 units (all of video_core plus every changed non-Qt
  file). The only failures were headers the sandbox lacks, plus block_of_code.cpp and codec.cpp,
  which fail the same way on r36. No warnings on added lines except lambda init-captures.
- To watch on the laptop:
  - preflush (ours) now runs next to upstream's early read-back downloads (#1028); A/B with
    EDEN_NX2_PREFLUSH=0;
  - HostTiming on E-cores with #1016's sleep (EDEN_NX2_HOSTTIMING_STATS=1);
  - 'split drain' time, since the back_quiet change interacts with our tick hook;
  - the first boot converts the v54 cache to v2 ('Shader cache v2 started from the version 54
    cache').
- Risks the agents noted: our parallel translation skips #1036's texture-pool flush, as r36
  did; the precache now runs on hardware_concurrency-1 workers; #949's waiter table has 64
  slots.

Round 21 (2026-11-09): the Fritchii base.
- The owner ran Fritchii v5.5 on the laptop and found it smoother. I first read its log as
  60 fps against our ~30; that was its title-screen video. In gameplay it ran ~32 fps against
  r28's 34-42 (Test results, 2026-11-09, CORRECTION). Decision, made on the misread: restart the
  tuned fork on Fritchii and bring our changes over in small groups, each A/B-able against
  plain Fritchii. The old fork stays on `tuned4` (`e9226a635`, r36, built but not tested). With
  the correction the owner decides again whether to keep this base.
- First build (run #37, built in #39 and #40 after build fixes): plain Fritchii v5.5 plus
  `EDEN_NX2_PRELOAD_ALL_PIPELINES=1` (the owner's standing choice, `6fa98f063`), in three
  variants: `msvc` (as Fritchii ships), `pgo` and `pgo-lto` (our MinGW clang + PGO +
  `-march=x86-64-v3 -mtune=arrowlake`). It answers whether our toolchain helps or hurts on the
  laptop.
- Build fixes on tuned5, none of which changes behaviour: tuned4's ThinLTO fallback for MSYS2
  clang (`c817bd803`), dwmapi linked through CMake instead of `#pragma comment` (`95b3bd0e3`),
  and Fritchii's locals renamed where they hid another name (`939e9bde3`, `ea2f48a62`,
  `66500175c`; MSVC's /we4456-4459 and MinGW clang's -Werror=shadow). Fritchii's download
  reports MSVC 1950 (VS 2026); windows-2022 has 19.44.
- Three-way test, 2026-11-10 (Test results). In what was probably the same area, Fritchii ran
  ~10% slower in steady play than r35 and build 132. Its smoothness came from building no
  pipelines in play (a warm cache, async shaders on), which our builds can match.
  Recommended: keep tuned4 (r36) and hold the port below until one A/B. The owner decides.
- To bring over next, each with an A/B launcher, in an order set by the Fritchii comparison:
  - Radeon: FSR 4, with camera jitter off by default (the water shimmer).
  - Laptop CPU: hybrid core placement and the spin/wait tweaks.
  - Diagnostics: the GUI crash writer (`da111dfcf`) and our perf logging.
  - The A/B launchers.
  Where Fritchii has its own version of an idea, Fritchii's stays.

## What the tuned4 build contained (rounds 1-20)

Round 20 (2026-11-09): upstream Lichii's open pull requests and Fritchii v5.2, the owner's two
sources from now on (upstream nx2 had nothing past `cbb512ee7`). Round 19 was not tested; run #30
is the next test.
- Upstream pull requests, cherry-picked with `-x`: #970, #974, #954, #969, #998, #933, #955 (vertex
  buffer binds skipped while still bound; our camera-motion overlay invalidates the scheduler
  state, so its own binds can't pass for the guest's), #976 (built-in Texture Pool x2 patch for
  the Castle Town guard house freeze with Max Resolution, and bad overlaps that a newer GPU write
  covers freed after 4 frames, ahead of our batched collector downloads), #911 (a fence-delayed
  payload release latches the waiting channels when the back processes it, not after a host GPU
  round trip; merged with our latch wake-ups and acquire stats), #922 (8-64 MiB images from
  256 MiB blocks, +6% fps measured on an RX 9070 XT), #1008, #957, #939, #1005 and #980 (Ocarina's
  culled mesh draws from GPU-written task counts without the front reading them, ~2,000 waits a
  second; it sits next to #805's compute list-indirect path, and its register-write hook comes
  before our state-register fast path in `Maxwell3D::CallMethod`). From #1002 only `45523ba94`:
  the instrumented ZBIC decoder corrupted the main NSO, the pgo-train "redump your files" error.
- Not taken: #995 (#969 is the same fix), #984 (our translation generations are already unique),
  #979 (the semaphore mirror is for other games, nothing in Ocarina's logs, and it changes the
  release paths #805 rewrote), #956 and #949 (#805's semaphore view covers them), #914, #961, #971
  (opt-in async discard only), #999, #959 (Fritchii's version taken), the rest of #1002
  (Fritchii's timer instead; `pgo_training` already dumps every 30 s) and diagnostics, Android
  and UI pull requests.
- Fritchii v5.2: `050dbe618` (8f09b0fd9: a guest write waits for a GPU download landing under it
  instead of being overwritten), the pipeline cache robustness work `6687ab37b` (slow teardown
  waited for), `4ff78d378` (background builds stop with the game), `730629643` (driver cache
  written during long builds), `97060abe9`, `4a9596c2c`, `98b89f131`, `5538af81b`, `d5d184e35`
  (logs), `39f1be83b` (f52631cf6: ASTC decode and unswizzle stores bounds-checked, AMD corruption
  and device loss), `22d96360d` (5e8db75aa: core timing on a high-resolution waitable timer,
  `EDEN_HIRES_CLOCK=0` and ab-21 restore the old wait), `cd0d323cb` (464fbb45b: staging buffers
  may go over the memory budget instead of aborting).
- Fritchii's shader preparation (owner's request, `fac2c0a4d`, `2ea4a0790`, `c70881e42`):
  `nx2_shader_extract`, `nx2_recipe` and `nx2_precompile` as Fritchii v5.2 has them. A recipe for
  the title (`EDEN_NX2_PRECOMPILE_RECIPE`, the title's shader folder or `<exe dir>/recipes`;
  Fritchii's Ocarina file is `OoT v131072 (cache v54).nx2recipe`) is assembled with the game's
  files into `precompiled.fork-v54.nx2prep`, and its records run as jobs of the loading-screen
  precache, on every CPU thread (Ask asks at launch; ~15 minutes the first time; then the stamp
  `precompiled_prepared.fork-v54.txt` skips it for that file, driver, GPU and release).
  - Records made for a device with other dynamic states are skipped, as in Fritchii. Only
    Fritchii's two adjustments apply (its `RecordedKeyForThisDevice`): this driver's fields,
    and 9ea66c827's static logic op for a record made with a dynamic logic op while the logic
    op is off. That second one covers most of the recipe on AMD.
  - r34 and r35 converted more (`ConvertRecordedKey`, plus round 17's loader conversion
    `d92a29336`/`eb2b411de`). r36 dropped both at the owner's request (`ac88e58de`).
  Fritchii: Ocarina's worst walking freezes on a first visit ~700-850 ms -> 40-70 ms. Not taken:
  #896's fast path (kept infos and SPIR-V) and canonical keys (they would re-key the disk cache);
  no checkpoint, so a skipped preparation starts over (the driver's cache answers what was built).
  The summary line (`Precompiled pipelines: N of M prepared ...`) counts the records the disk cache
  held and, per missing dynamic state, those this device can't build (`5387f2a61`, as Fritchii
  v5.3's 1fa49c5c3 logs its skips). The package ships the owner's copy of Fritchii's recipe,
  `lichii-tuned/recipes/OoT_v131072_cache_v54.nx2recipe` (1.8 MB; NX2RCP01, format 1, title
  0400486024DEC000, game version 131072, cache version 54), copied by the workflow into
  `recipes/` next to the executable (Windows and CachyOS); `e0b1f284f` notes it in the README.
- r34 test (owner, 2026-11-09): the preparation crashed near its end; the log just stops at
  568 s, mid-line.
  - r35 reverted #820 (`b5e399c6a`, as Fritchii 9b30eec8c does; it also removes Fritchii's white
    drawbridge fog). It also added upstream #999's journal of builds in flight (`4218f2e08`,
    `2302bb211`, `d355c4620`).
  - That diagnosis was wrong. The ~1,700 'Patch topology not used with tessellation' warnings
    come from the recipe's task+mesh pipelines: a mesh program sits in the TessellationEval
    slot, and the recipe has no tessellation programs. #820 never touched those pipelines.
  - r35 crashed too, 2,719 s into the preparation (44 minutes). WER reported BEX64, c0000005
    with parameter 8 (an execute fault), fault module StackHash, 'PCH_DE_FROM_ntdll+0x1628A4'.
    WER LocalDumps, set up by the owner, wrote no dump.
  - The cause, found by r36's integration review and confirmed: on AMD's drivers a software
    HMMA compute program builds on `deferred_workers` (bace6509e, an upstream pull request
    Fritchii doesn't have). The precache's compute jobs and the recipe's 4,088 compute records
    (the file's last; Ocarina's AA network around record 116,440) dropped the pipeline at
    once. Its queued build then ran on freed memory, calling a std::function there.
  - `91efbc685` builds them in the job. ComputePipeline still has no destructor that waits:
    callers keep a pipeline until IsBuildTaskDone().
  - Each r35 crash poisoned up to 16 innocent in-flight records for this driver
    (precache-jobs.poison). This is harmless; deleting the file builds them again.
  - `da111dfcf` adds EDEN_NX2_CRASH_DUMP to the GUI. It is on in the tuned nx2_switches.txt
    and writes lichii-crash-<pid>.txt and .dmp to %APPDATA%\eden\crash_dumps, without admin
    rights. It was tested under Wine with a thread pool callback into non-executable memory.
- Later the same day (owner: merge what's new since the last check): upstream nx2 had merged 15
  pull requests past `cbb512ee7` (to `0ab7736df`). Taken: #961 (`fbf12eafd`, all-zero guest pages
  trimmed from the working set; Ocarina boots with ~7 GB of them) and #951 (`9d1aed504`,
  `EDEN_NX2_MEM_TRACE`; texture cache thresholds logged only when they change), with
  `0c6d67079` (#951's PROCESS_MEMORY_COUNTERS_EX2 spelled out for older MinGW headers); #957 was
  in already. Not taken: #1036 (shader cache v2 replaces the vulkan.fork-v54 cache and its
  loader, which tuned4's preload-all, kept pipelines, record conversions, F2's robustness work and
  the recipe preparation sit on: its own round), #949 and #979/#1014 (semaphore paths #805
  rewrote; #949 is mostly #805's view), #1028 (#805's read-back; overlaps the download landing
  and helps with reactive flushing, off here), #1016 (Fritchii's timer is in), #984, #923, #947
  and the diagnostics #965, #975, #1039. Open #1032 (macro DMA reads of report slots without the
  drain, Kokiri 42 -> 47 fps upstream) is what round 15's shadow and #805 already do (r28: the
  front's macro read waits gone). Fritchii v5.3-v5.5: 9ea66c827 (AMD: recipe records with a
  dynamic logic op) is in RecordedKeyForThisDevice (r36); revision 5's EDS2 and dynamic vertex input
  are this build's defaults (the owner's r28 log has them unchanged); the rest is UI, notes,
  DLSS and NVIDIA.
- Fritchii v5.2's remaining items (r36): three agents ported them on branches
  `r20-fritchii-core`, `r20-fritchii-shaders` and `r20-fritchii-rt`. Each branch was reviewed
  area by area, with verifiers trying to refute every finding, then merged on
  `r20-integration`. Every change has its switch, and ab-22/23/24 bring round 19 back per group.
  - Guest memory:
    - `3f0ce586f`: EXP35=2 with EXP36 and EXP60. The guest's reads and writes of cached memory
      skip the caches' locks. A write to a clean page is collected and applied before the next
      command list, acquire or accelerated copy.
    - `33707525a`: #700's accumulator half is reverted; `EDEN_NX2_INVALIDATE_BLOCKS=1` keeps it.
    - `78b9e52d8`: EXP9=3 with 8b7bb3fab's node map.
    - `7f82adc67`: EXP18=4 and EXP29 & 4.
    - `80515e067`: EXP37, off as in Fritchii.
    - `daa657dc6`: SEC_RESERVE.
    - Review fixes that go beyond Fritchii:
      - `fd501a1d5`: Fritchii's c2d2f50d1 sizes a range from the block start, so it leaves out
        bytes written mid-block. Ranges are now exact.
      - `13dda6404`: a collected write that crosses the dirty managers' 2 KiB pages is split per
        page instead of being cut short by a 32-bit shift.
      - `4728e4d7d`: the apply runs before a list's work, SEM_EXECUTE and accelerated copies;
        `910ec2b1e`: also when a Switch 2 SemaphoreOperation acquire is met or resumes (a draw
        after an acquire the CPU released mid-list read the old host copy).
      - `c4a576b92`: LICHII_BANK_CHECK=2 counts what mode 3 misses.
  - Shaders:
    - Async translation on two workers, on by default (`58412f763`, `72e5f19c9`), with
      `d38567d97` (page table lock) and `5e3e8bf12` (environment snapshots;
      `EDEN_NX2_ASYNC_TRANSLATE=2` checks them).
    - Async discard mode 2 (`2f6e59a42`): with native_async_draws at Wait, a draw waiting for its
      pipeline is skipped only into targets cleared in this render frame and a recent earlier
      one.
    - Prebuild streaming (`3f301266e`: c62c15add, 166f173ab, fc6d4cab4).
    - #896's translation profiler (`ca992b6b3`). The cheaper SSA reads are off in Fritchii and
      left out.
  - Render targets:
    - `8334d0ecf`: d69ecefe3.
    - `49bce6d4c`, `0b56556f3`: the slow clear log.
    - EXP43 relocation: `569b99fdf`; b3396c01f's guard by address in `ff8168795`; =3 by default
      in `447ae09ff`; =4 tombstones in `a10bfaea1`, where `1ceae31ba` stops counting CPU reads
      the moved image serves.
    - The render-frame clock and cold sweep, on by default (`302832a36`, `dd6e29831`,
      `17a9666a3`).
    - `4cb87fe88`: EXP42 switches tuned4's image recycler, which already covers Fritchii's
      VkImage pool.
    - `cef2cef01`: churn counters.
    - `7e9a2d80d`: async discard's clear frames count in render frames.
  - Integration (`r20-integration`, fast-forwarded into tuned4): `b3d6e4904` (a shadowing
    warning), `1ceae31ba` (EXP43=4 counted CPU reads the moved image serves), `d520eea69` (a
    comment), `910ec2b1e` (above). Then a review of the combined branches (the acquire fix, r35's
    journal and recipe against the shaders branch, guest memory against render targets, the
    defaults and launchers) confirmed two defects. One was the preparation crash below
    (`91efbc685`). The other was ab-23 leaving reads lock-free (`ae609615c` sets EXP36=1;
    verify-r20 now names the bank check's mode 3 count). `e9226a635` lets ab-24 keep the
    render-frame clock (mode 2 async discard counts clears by it) and puts the disk cache
    loader's flag check before its driver count. F1's 7 unit tests pass under ASan and UBSan.
  - Known lows, reported by the reviews but not verified or fixed:
    - A pipeline translated asynchronously classifies storage-buffer pointers on the worker,
      possibly well after the draw, and they are not checked again when the draw adopts it.
    - Mode 2 judges a draw only by its render targets.
    - EXP9=3 skips the per-frame compare, so a bank page changed only by a writer that sets no
      flag (semaphore, QMD or query releases, texture write-backs) can stay stale;
      LICHII_BANK_CHECK=2 (verify-r20) counts such misses.
    - While the render-frame clock is not live, a present tick between two clears in one frame
      makes a target look recurring.
- Left out by choice: graphics pipeline libraries (Fritchii turns them off on
  AMD's Windows driver), the shader warm-up (#579, needs them), #896's precompiled fast path and
  canonical keys (they would re-key the disk cache), UI, branding and settings revisions.
- Shader precompile (the owner: under a minute after deleting the shaders, expected ~15): r18's
  "Precache shaders" (#781, Ask) builds the compute programs found in the game files plus the
  cached graphics pipelines the boot left for later, so with the shader cache deleted it had
  little to build. The 15 minutes are Fritchii's recipe preparation, ported above; it needs the
  recipe file next to lichii.exe (`recipes/`) or in the title's shader folder.

Round 19 (2026-11-09), after r28's water artifact:
- `4f2e4a70f`: the effects mask Ocarina's temporal upscale builds (pixels changed after the fog
  pass: glows, particles and, it turns out, water) is FSR's reactive and T&C mask, so FSR shows
  the current frame there; under round 18's jitter that is a new sub-pixel sample each frame
  and fine detail shimmered (the owner: textures where water meets the screen's edges, gone
  with ab-14). The bias pass also carried last frame's marks along the motion vectors with the
  history position clamped to the image, spreading an edge pixel's mark along the border while
  the camera moved. Now history outside the frame is skipped, and jittered frames scale the
  mask by `LICHII_FSR_REACTIVE` (default 0.5; 1 = round 18, ab-19). Unjittered (DLSS) frames
  keep the full mask. Not confirmed on the game yet: if the edges still shimmer, try
  `LICHII_FSR_REACTIVE=0`, then a frame dump of the water pass.
- ab-20 turns #717's kernel spins off (`EDEN_NX2_KSPIN=0`, `EDEN_NX2_ARB_SPIN=0`): r28's
  WaitForAddress time across the guest cores rose from ~35 to ~107 ms/s (the arbiter spin), on
  a CPU that is power-limited all the time.
- Upstream #932 merged (`646523fba`, a macOS build type fix).

Round 18 (2026-11-09): upstream nx2 merged up to `281477eac`, camera jitter for FSR, and the
ports deferred for their conflicts (#805, #717's guest CPU parts), done first as the owner
asked. Nothing of round 18 has run in the game yet.
- Merge `1105c716c` (11 pull requests of the evening of 2026-11-08): #858 (cheaper per-draw work
  on the split back thread: alias syncs only when an alias changed, a per-pipeline texture memo,
  delta draws keeping the previous draw's views; fast draws are always on upstream), #921 (its
  fixes), #910 (no null image preparation on memo hits, Ocarina), #878 (compute IR freed before
  a synchronous compile), #880, #902, #918, #885, #879, #912, #881. Pipelines with computed
  texture handles (tuned4's handle tables) use neither the memo nor delta draws (#858's
  `ComputedHandles` blocker); the Ocarina 4K and effects captures stay unconditional (upstream
  now builds them only with Streamline; FSR 4 uses them here).
- `479bbb1b5`: camera jitter while FSR (4 or 3.1) upscales Ocarina. Frames after an evaluated
  one offset the viewports of the scene camera's draws (vertex/mesh constant buffer 3 holding a
  perspective projection, #253's test) into a scene-sized Z32 depth buffer by a Halton (2, 3)
  sub-pixel jitter in FSR's cycle (8 phases same size, 18 for 1440p to 4K), chosen at the
  frame's first draw and fixed for the frame (#253 lost geometry to an EQUAL depth test when its
  jitter turned off mid-frame); the evaluation is told the offset (FSR convention: texel k
  samples k + 0.5 - jitter, which a viewport moved by the jitter gives). The motion vectors use
  the unjittered camera matrices, so they leave the jitter out. DLSS keeps unjittered frames.
  `LICHII_FSR_JITTER=0` (ab-14) as before. Unit tests in `src/tests/video_core/ocarina_upscale.cpp`.
- The workflow builds `pgo-train` again, for an Ocarina profile (none was ever recorded:
  `lichii-tuned/pgo/` is empty).
- #717's guest CPU parts (merge `5f0e02841`, branch `r18-pr717`, ported by an Opus subagent
  from eb3553bcb), every piece behind an `EDEN_NX2_*` switch, on unless 0:
  - `06d6d329b` dynarmic x64 codegen: flag reuse (`_JIT_FLAGS`), a 2^16-entry 4-way dispatch
    table (`_JIT_DISPATCH`: 0 = old 2^20 table, 8-24 = bits), compact exits, far code, address
    folding into fastmem (`_JIT_FOLD_ADDR`, 64-bit fastmem only), `_JIT_ZEXT`,
    `_JIT_SPILL_GPR`, `_JIT_SCRATCH_COPY`, `_JIT_NO_FRAME`, back-edge halt checks, a 32-entry
    RSB, YIELD/WFE pauses (`_HINT_PAUSE`); off under CPU accuracy Paranoid. #717's TSO loads
    are round 7's `LICHII_JIT_TSO` and were not added twice; native LSE atomics are left out
    (#831's author traced a boot panic to their CAS fallback). Two fixes over #717: the
    zero-extend skip only for values in a GPR (a 32-bit spill slot would reload 4 stale bytes),
    no flags-in-ZF path with verbose JIT debugging. `37ca9e142`: RegAlloc shadowing.
  - `98843dfae` memory: trap fast paths (`_TRAP_FASTPATH`: the trapping access's host
    pointer reused, a write slot remembers its device page while mappings stay, four read
    areas per core) and a write slot for non-core threads (`_SYS_SLOT`; System has
    NUM_CPU_CORES + 1 dirty managers).
  - `3fa3e9c89` kernel: spin-then-wait kernel locks (`_KSPIN`, `_KSPIN_US` 20), arbiter waits
    watching the word (`_ARB_SPIN` µs, 8), spin core lock (`_CORE_LOCK`), `_IDLE_PAUSE` (only
    without WAITPKG; tuned4's UMWAIT idle stays), `_CONTEXT_GUARD`, `_FAST_TLS`,
    `_OFFSETOF_CACHE`, `_CACHE_LOCK_WAIT`. No thread pinning, no mutex owner hook.
  - `05b4d4aaa` Ocarina patches through svc 0x80+ hooks: host zstd with the game's dictionaries
    (`_OOT_NATIVE_ZSTD`), 8-byte strcmp (`_OOT_FAST_LOOKUP`), resource poll backoff
    (`_OOT_POLL_BACKOFF`, µs), wider multi-core affinity masks (`_OOT_WIDE_AFFINITY`); in
    Add-Ons like the other patches, original bytes checked. Not checked against the game
    binary here: the strcmp patch's exit register state, the zstd callers' use of the context.
  - Not ported: the native Ocarina macros (macro_hle_oot, they overlap #805's macro work), the
    GPU-thread, cache and drain changes (duplicates of #805), the DLSS CMake condition.
  - Launchers: ab-15 (all of it off), ab-16 (codegen off), ab-17 (Ocarina patches off).
- #805 (merge `da05287c7`, branch `r18-pr805`, 47 commits of head 34621dcd6 by an Opus
  subagent; cherry-picks keep jpkto as author): acquires from a front semaphore view and QMD
  view instead of drains, macros that read memory run on the back (`EDEN_NX2_BACK_MACROS`),
  command lists on GPU-written pages wait only for the words GPU work wrote and only up to the
  last host sync, GPU-written compute list parameters through vkCmdDispatchIndirect
  (`EDEN_NX2_LIST_INDIRECT`), upload batching (`LICHII_UPLOAD_BATCH=0` off), back-thread CPU
  cuts, fence-flagged releases, the race fixes. tuned4's paths moved behind switches:
  `LICHII_TUNED4_ACQUIRES=1` (#345 fast paths, Frizti polls, `LICHII_ACQUIRE_POLL`),
  `LICHII_TUNED4_DMA_READS=1` (shadow, mixed reads, report drain; with
  `EDEN_NX2_BACK_MACROS=0` the round 17 behaviour), `LICHII_LIST_WRITER_WAIT=all`; tuned4's
  front write tracking runs only with one of them. Verify: `EDEN_NX2_DRAIN_CHECK`,
  `_DMA_READ_VERIFY`, `_BACK_MACRO_VERIFY=<n>`, `_LIST_VERIFY`, `_LIST_INDIRECT_CHECK`;
  stats `EDEN_NX2_ACQUIRE_STATS`, `_LIST_STATS`, `EDEN_NX2_GPU_PROFILE=2`. Skipped: 1457bd01f
  and f3702f3b9 (tuned4 has #699 and #792), the GPU-cost diagnostics. Not ported yet: upload
  reordering (ddf01f582, a0ecdb040: buffer-use ticks the port would have to adapt by hand)
  and the read-download set (13 commits from 1072d4a1e to 34621dcd6: they only act when the
  guest reads GPU-written memory, and they touch the memory.cpp handlers #717 changed).
  `EDEN_NX2_MME_DMA_STATS` now describes only tuned4's paths. Launchers: ab-18 (#805 off),
  verify-r18.
- `core/memory.cpp`: #805's trap counters sit in #717's write handler; #717's non-core slot
  is not counted (the counters check their bounds).

Round 17 (2026-11-08): upstream nx2 merged up to `ad477ef1f`, the owner's new AMD fork migrated
for Intel, and the fork survey's picks. The owner skipped testing round 16 (r26), so its checks
(verify-r16, ab-12) carry over.
- Merge `0bb650836` (53 pull requests). Upstream's final versions replace tuned4's copies of
  #816 (store extent; Cap64K goes), #815 and #591 (pipeline cache: a key's first record is
  built; exact duplicates, the same stored key, and incomplete records are removed from the file
  by the serialization thread; this replaces round 16's newest-first retry, record groups and
  failed-record set), #817/#818 (join copies that write a whole image; StalePages), #778
  (Fritchii v4 imports without switches), #268 (5/8 of the VRAM budget), #621, #687, #488. New:
  #781 (`precache_shaders`: compute programs found in the game files, built on the loading
  screen), #769 (GPU thread pinning, on by default), #841 (task/mesh records build without
  VK_EXT_mesh_shader, so our drop of them went), #812, #752 (a channel waits for a GPU-written
  list without blocking the others). Kept from tuned4: the record conversions, #408's attribute
  keys (deduplicated in memory only: the file keeps every record, so builds that key them apart
  lose nothing), EXP8's copy skip, banded untiles, no cold hints,
  batched collector downloads, the front's write tracking and mixed macro reads, the genlock
  beside #655's timer pacing, and the raw Dynamic FPS step, now also applied to #655's FPS
  Unlock patch (it shares the routine head TuneDynamicFps patches).
- `fc44e7064`: `pin_gpu_threads` off by default: #769's pinning would leave the six emulated
  cores the four P-cores the GPU threads don't take, where hybrid_cpu.cpp spreads them over P-
  and E-cores.
- Open upstream pull requests: #836 (`d708c15c7`, `3b339161f`: Ocarina's images recycled
  instead of created and freed every frame), #810 (`f4375c906`, `a2b91c3b5`: HostMemory::Protect
  shares the placeholder lock), #784 (`4ebb88a88`, `b6bbe06b1`: 16-byte units untiled from a
  per-GOB offset table, subrects in runs; combined with our line pool, its test passes, also
  with layouts large enough for the pool; 1024x1024 RGBA8 unswizzle 0.50 -> 0.29 ms, native
  tiling 0.67 -> 0.28 ms on the cloud's 4 cores).
- From medicinauvsecretaria-lang's "Lichii OoT AMD" (research section 12): `403a22b82` saves the
  driver cache every 32 new pipelines at most every 10 s and the used list every 5 s;
  `8b3a3c54a` seeds #781's precache as Ask (tuned defaults 3). Its downloads are built with
  `-march=znver3`, the reason they "may not work for Intel CPUs"; tuned4 stays x86-64-v3.
- Forks: `43321b8db` (tomk134 e8e50e797: unswizzle contiguous guest memory in place;
  `EDEN_NX2_UNSWIZZLE_COPY=1` copies, ab-13), `d05a02108` (Elysium fc0ac4b3b: a thread_local
  scratch buffer for the runs still copied), `da24ac9c1` (Elysium 4a680b22d: the IADD overflow
  flag's negative bound in the SPIR-V and GLSL backends).
- Launchers: ab-13-round16-texture-reads; ab-12 lost `LICHII_NULL_POINTER_HANDLES`.

Round 16 (2026-11-08): Fritchii v4 and v4.1 (Frizti/Lichii, tags `fritchii-v4`, `fritchii-v4.1`;
the owner says people consider v4.1 the best-performing fork) ported, and our own fixes for what
r23 showed. Fritchii's numbered switches are read too (`EDEN_NX2_EXP<n>`).
- EXP9 (`5acb2d398`, Fritchii 9e8cb73ed, 1a64fe500, 4c4948805): native 64 KiB bank pages are
  compared only when they may have changed: CPU writes the memory tracker only collected (its
  `last_address` never resets, so later writes went unnoticed), buffer cache writes, recheck
  marks, CPU-dirty pages, pages that changed within 120 frames, and once a frame
  (`DevicePageFlags g_collected_write_pages, g_buffer_write_pages`). r23: ~650,000 compares a
  second, 99.7% unchanged, bank check 164 ms/s of the back. `LICHII_BANK_CHECK` 1 (default), 2
  compare all and log what 1 missed, 3 no frame check, 0 as before. Replaces round 15's
  `LICHII_BANK_DEDUPE`.
- EXP8 (`54dd4bf3c`, Fritchii 7d75f9133, 7cdbcc3fa; Hyrule Field 1% low 30.3 -> 35.1 there):
  idle staging buffers kept, no guest upload for a new 2D image GPU copies overwrite whole,
  banded untiles on the transcode workers, no HintCold. `LICHII_TEXTURE_UPLOADS` bits (15; since
  round 17 bit 1 switches nothing: upstream #778 always keeps idle buffers).
- EXP21 (`2b72dc569`, Fritchii d603d98bb): a texture handle word that is a storage buffer
  address binds no texture (waterfall flicker). Round 17: upstream #778 does it always, so
  `LICHII_NULL_POINTER_HANDLES` went.
- Our own: macro DMA reads take still-queued constant buffer words from the front's shadow and
  the rest from memory when nothing queued may change them (`b4c9e78b7`; r23: 94% of those
  waits, 208 ms/s of the front). `LICHII_DMA_MIXED_READS` 1, 2 verify, 0 off.
- Pipeline cache (`4afd7df1b`, Fritchii 9b8fef282 and its record grouping): records of one key
  are tried newest first, a key whose records all fail is stored again at run time, compute
  translations newest first (round 17: upstream #815's first record per key replaces the
  graphics part). `fc68cbdd3` (Fritchii 55bba8812 = zortos293/Lichii#408): graphics
  keys hold only the vertex attribute types the native vertex program reads (none for task and
  mesh), stored records are masked at load; `LICHII_KEY_ALL_ATTRIBUTES=1` as before.
- Fritchii's host frame pacer and genlock (`fe9a0f3f4`; v3's 53ba1b2c7 to 72e5ad6c2 with v4's
  82a514211, 817edaee3, 7d5ddd36b), opt-in: `LICHII_HOST_PACE=1` holds frames for their refresh
  (VK_KHR_present_id/present_wait, a wait thread, the window's monitor rate) and locks the
  game's frame period to whole measured refreshes; `=2` only logs "Host pacing off:" lines; unset
  leaves presentation untouched (no extensions, no thread). Fritchii has it on by default with
  Mailbox. Its measured gain is even display at 60 fps on 120 Hz (two refreshes per frame 97-99%
  vs 91-96%), not frame rate; the owner's monitor rate and FreeSync state are unknown.
- The list writer wait experiment is reverted (`ec4ac2957`): r23 hit a GPU-written list entry
  126 s after boot.
- Not ported, with reasons: EXP10 (macro read drains narrowed to compute/copies; built on
  Frizti's DMA snapshot, and our waits are constant buffer overlaps, fixed above); EXP14 (same
  as our FrontWrite's unchanged-register drop); EXP2, EXP3, EXP12, EXP17, EXP18, EXP19 (off by
  default in v4.1); EXP4 (superseded by EXP9); EXP5, EXP7, EXP13, EXP16 (diagnostics or
  measured no gain); updater, branding, dist, debug tools; #215's wait (not needed with fake
  occlusion); 86726f688 (we keep every pipeline); e9a04cc5a thread pinning (our own placement);
  5aefec443 cache merge (we already share zortos's version 54 file); settings revision 3 and
  f6d42b871 (the owner's settings already match: 1x, FXAA, CPU Auto, disk and driver caches,
  docked; Mailbox only matters with the pacer; 8 GB layout changes nothing with 32 GB of RAM).
  Fritchii's upstream imports were in already (f319e1076 = 68caa6c75, 2c6c844f4 = 79c151694,
  af2bbd3ce = 8cb1ec8a3, aff77e7fb = d4bc14e41, 34ae8336e = 2e69168aa, the compute fixes by the
  6645721f2 merge).
- Launchers: verify-r16 (`LICHII_BANK_CHECK=2`, `LICHII_DMA_MIXED_READS=2`), ab-12 (all of round
  16's switches back to round 15), trace-host-pace, trace-display; trace-no-list-wait went.

Round 15 (2026-11-08, the 60 fps plan's first round; upstream nx2 had nothing new):
- AMD FSR 4 upscales Ocarina by default (`vk_fsr4_upscaler.cpp`): the AMD FSR API's loader and
  AMD's signed FSR 4 upscaler, loaded at run time from the exe folder, newest 4.x version the
  API offers for the GPU, the same inputs as #695's FSR 3.1 (#606 colour, D32 depth, camera
  motion in render pixels, #644 effects masks, unit exposure, no jitter, non-linear sRGB-like
  colour, near 0.1, far 25000), recorded into the scheduler's command buffer; the API's dispatch
  checks log through `AMD FSR API:` lines. Any failure (DLL, version, context, dispatch) logs an
  `AMD FSR 4:` line and FSR 3.1 runs. `ocarina_fsr` defaults on and is seeded once into
  existing configs (`lichii_tuned_defaults` 2). Devices with VK_EXT_shader_float8 and
  VK_KHR_compute_shader_derivatives enable them (FSR 4's FP8 and INT8 networks). Switches:
  `LICHII_FSR4=0` (FSR 3.1), `LICHII_FSR4_VERSION=<name>`, `LICHII_FSR4_DEBUG=0`,
  `EDEN_NX2_DLSS_REPLACEMENT=native` (no FSR).
- Preflush is on by default for Ocarina (`EDEN_NX2_PREFLUSH=0` off).
- Measurement: each frame's time and the 1% lows (`frame_time_stats.h`: `DKB frame_ms` every
  second, `Frame times:` every 10 s; `LICHII_FRAME_TIMES=1`, implied by the perf variables);
  each guest core's time by activity (`guest_core_profile.cpp`: guest code, supervisor calls
  by name, kernel, idle spin, idle wait, JIT memory accesses through callbacks); bind stage
  buffers split into uniform buffers, bank check, storage buffers and stage texture buffers,
  with bank pages compared per second. `EDEN_NX2_MME_DMA_STATS=1` in perf-trace.bat says why
  macro reads and acquires still wait.
- A native 64 KiB bank page compared for a draw is not compared again for the draw's next stage
  (`LICHII_BANK_DEDUPE=0` compares every bind): up to 16 page compares per bank bind, the
  likely bulk of bind stage buffers' 8.7 ms a frame.
- Experiment `LICHII_LIST_WRITER_WAIT=0` (reverted in round 16: Ocarina has GPU-written list
  entries).
- #161's upload-word counter turned out to be in already (#184's per-word clean_words and
  ForEachDirtyUploadRange, and the index-buffer rebind skip).
- Launchers: trace-fsr31, trace-no-fsr, trace-no-preflush, trace-no-list-wait, frame-times;
  trace-fsr, trace-preflush and ab-7 went (now defaults).

Round 14 (2026-11-08): upstream nx2 merged up to `bf4c42e22` (build 117 #657, #659, #664 GPU Clocks
shrink Switch 2 GPU durations again: Ocarina sizes its dynamic resolution from them, #679 AMD fixes:
blend key always rebuilt, null texture past the TIC pool, preloaded pipelines never evicted). Open
upstream PRs on top: #415 (native compute stores bound the GPU-written mark: the 4-8 MiB skinning
ranges behind the read-trap stalls and preflush downloads; setting "Native compute write mark"),
#288 (Kokiri shadow map uploads a join copy overwrites), #699 (draws after a deferred clear stay in
the render pass), #700 (invalidation blocks, query walks stop early), #621 (Windows one-tick polls),
#687 (fence release while acquires wait; ported onto our acquire polls), #695 (opt-in AMD FSR 3.1 in
place of Ocarina's output pass; `trace-fsr.bat`), #296 (ASTC uploads), #609, #610, #611, #589, #645.
Not taken: #640 (cache record repair), #408,
#652, #579, #559 (fake occlusion leaves no pending reports), #698 (our placement doesn't use #232's).

Round 13 (2026-11-07 night): upstream nx2 merged up to `92f021240` (102 commits since
`09b78bd2df`), which brings everything rounds 10-12 cherry-picked plus #432 (pipeline cache
v54), HMMA on cooperative matrices (#475, #583: the "Game" AA network on RDNA4), mesh pipelines
through compute (#569), NVIDIA DLSS for Ocarina with camera motion vectors
(`vk_ocarina_camera_motion.cpp`, #606: groundwork for FSR 3.1/4), Ocarina's AA bypass as merged
(#616), `pin_gpu_threads` (#418, #530; off, our hybrid placement stays), #351 table-pointer
pools bound to the storage buffer reach (replaces our 256 MiB cap), #397, #440, #178, #245, #186,
64 storage buffers (6645721f2). Merge choices are in the merge commit `82ad59467`. On top: #591
(v53 caches carry over to v54), a log line counting cached pipelines left out by reason, cached
pipelines from other drivers adopted (d92a29336), Kitsu's exit-hang fix, Frizti v3's drain-free
acquire polls (`LICHII_ACQUIRE_POLL=0` undoes), MME DMA reads of report pages drain only for
reports that can stay pending (`LICHII_REPORT_DRAIN=all` undoes), expiring preflush marks
(`LICHII_PREFLUSH_FRAMES`, 120).
What follows describes rounds 1-12.

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
  `trace-real-occlusion.bat` (round 10), `trace-downloads.bat` and `trace-old-drains.bat` (round
  13; `trace-strict.bat` went in round 13: strict placement didn't help in r11 or r18).

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
- Run #18 (round 12) built `pgo` and `pgo-lto` (artifacts `lichii-tuned-r18-pgo`,
  `lichii-tuned-r18-pgo-lto`).
- r18 at 1x, docked, ScaleForce, gameplay only (after the load into the save; the title screen
  runs 50-60 fps): "maybe slightly smoother than r16 but not by much"; no flashes, no crashes
  (each log's userspace panic is the game's own at shutdown, after "Force stopping EmuThread").
  - `perf-trace.bat`, 94 s: median 25.2 fps (r16 24.6), p10 14.1 (18.5), p90 34.8. Two 2.3 s
    hangs in new areas building 46 pipelines (draws held 3.2 s). GPU busy 37.2 ms a frame (40.5),
    submissions 9.1 ms (15.3), 654 submits a second (974: the 512-draw AMD budget). Guest cores 4
    and 5 30.2 and 29.4 ms a frame (31.5, 30.6), the back thread 30.1 (32.4): bind stage buffers
    195, other 134, method 106, draw 72 ms/s. Read-trap waits at buffer_cache.h:407 62 a second,
    104 ms/s (49, 90).
  - `trace-strict.bat`, 91 s: median 22.5 fps, guest cores 4 and 5 32.9 and 31.6 ms a frame: no
    gain, as in r11. Drop strict placement.
  - `trace-preflush.bat`, 130 s: "no flicker, stutters felt shorter, smoother at first, evened off
    towards the end". Full GPU waits 64 -> 6 a second (105 -> 29 ms/s), but downloads grew 556 MiB/s
    -> 1.4-1.6 GiB/s over the two minutes: a preflushable mark is never removed, and every later GPU
    write over a marked page is copied back at every fence (~30 a frame). The game reads two hot
    pages (0x12fdfc000, 0x44941000: a third of the sampled reads) and hundreds of pages in two
    ranges (0x2b69xxxx-0x2b8dxxxx, 0x3019xxxx-0x3058xxxx) a few times a second each. Fps fell from
    22-35 to 15-20 as the downloads grew (median 20.6); guest cores 4 and 5 40.2 and 37.9 ms a
    frame, GPU busy 46.8 ms. Round 13 lets marks expire.
- Runs #19 (round 13), #20 (13b) and #21 (13c: logic op keys) built `pgo` and `pgo-lto` (artifacts
  `lichii-tuned-r21-pgo`, `lichii-tuned-r21-pgo-lto`).
- r21 with the shared build-86 cache (all four launchers): Total Pipeline Count 19,416-19,450 at
  boot (was 1,707): 21,062 records from another driver, all turned into this device's logic op form,
  3,368 duplicates once they were. Runtime builds in game: 1-16 per 10 s. Gameplay (93-139 s each):
  perf 27.8 fps median (r18 25.2), p10 20.7 (14.1); old drains 26.7; preflush 27.4 (downloads flat
  at 400-520 MiB/s, no growth); downloads 27.2. The owner: "all relatively similar", old drains
  maybe slower, preflush maybe faster. New waits against old: the front's CPU time 12.9 against
  22.4 ms a frame, macro DMA flush drains 279 -> 0 ms/s and split drain 383 -> 74, but macro DMA
  read waits 46 -> 202 and writer waits 407 -> 712 ms/s: the front now waits on the back for the
  same work in other places. Acquire waits barely moved (246 -> 233 ms/s): most acquires aren't on
  a stable word. Read-trap waits 59-75 a second (92-100 ms/s on the guest cores). The download trace:
  GPU writes of 2.8-8 MiB (0x2b4ce800 +0x800000, 0x302ff200 +0x441e00, ...) about once a frame each,
  5-6.4 GB queued per 10 s, copied back from the 0x2b6x-0x2b8x and 0x304x-0x305x regions the CPU
  reads: #415's over-marked skinning bindings.
- Run #22 (round 14) built `pgo` and `pgo-lto` (artifacts `lichii-tuned-r22-pgo`,
  `lichii-tuned-r22-pgo-lto`); it predates the shipped `EDEN_NX2_PRELOAD_ALL_PIPELINES=1`.
- r22's per-core CPU per frame in gameplay (median): guest cores 4 and 5 28.9 and 28.2 ms, the
  back 27.9, guest cores 3, 1, 0 and 2 22.9, 19.7, 17.1 and 17.0, the front 13.1, the Vulkan
  worker 5.6: all six guest cores are busy, which a 6 P-core laptop CPU shares with the three GPU
  threads. Round 15's guest core profile says how much of that is spinning or waiting. The
  front's waits on the back by site: list writers 318-352 ms/s (5,300-5,800/s), acquire 213-219
  (1,100-1,200/s), macro dma read 196-257 (400-500/s, macro 0xe2c reads ~20,000/s); the back's
  split wait 175 ms/s.
- r22 (19,467 pipelines at boot; owner: no white leaves since round 1, no flicker, nothing
  missing): perf 27.2 fps median (p10 17.7), GPU Clocks Normal 27.3 (GPU busy 8.5 ms against 8.3:
  #664 changed nothing visible), preflush 28.0 (p10 20.8, no hang over 1.5 s), FSR 27.3. #415:
  preflush downloads 400-520 -> 21-40 MiB/s and its full GPU waits 9 ms/s; without preflush the
  read traps still wait 61-74 a second (65-77 ms/s). FSR (`trace-fsr.bat`): "FSR 3.1 upscaler:
  2560x1440 -> 3840x2160", Ocarina temporal presented 3840x2160, GPU +1.1 ms a frame; the owner
  saw "extreme hitching" once: a 1.35 s guest stall (vsync callback 1 s late, no pipeline build,
  no GPU wait), likely a game-side load. Each of guest cores 4 and 5 and the back thread still use
  27-29 ms a frame; the front 13; the GPU works 8-10 ms of each ~36 ms frame.
- Run #23 (round 15) built `pgo` and `pgo-lto` (artifacts `lichii-tuned-r23-pgo`,
  `lichii-tuned-r23-pgo-lto`), with AMD's FSR 4 libraries checked by hash in the package.
- r23 (pgo-lto; logs perm = perf-trace, frame-times, fsr31 with the owner AFK for minutes,
  trace-no-list-wait): FSR 4 works ("the API offers 4.1.2, 3.1.6, 2.3.4; using 4.1.2", "upscaler
  4.1.2 ready for 2560x1440 -> 3840x2160 in 687 ms (302 MiB of GPU memory)"), ~2-3 ms more GPU
  a frame than FSR 3.1; preflush on. Title and menus 60 fps; gameplay 26-34 fps per 10 s window
  (perf-trace median 27.7, p10 20.5; frame-times 30-33), 1% lows mostly 10-20 fps (FSR 3.1 while
  AFK: 20-26, a still scene). Still CPU-bound: per frame the back 27.6 ms, guest cores 4 and 5
  27.1 and 26.2 ms (guest code 758 and 730 ms/s, spinning 45, waiting 150-175: real game code),
  cores 3, 1, 0, 2 21-16, the front 13.3; GPU submissions ~12.4 ms of a 35.6 ms frame. Back:
  busy 756 ms/s plus split wait 226, bank check 164, method 105, uniform buffers 60. Front waits
  on the back: list writers 310 ms/s, macro dma read 208 (94% constant buffer overlaps), acquire
  180. Guest cores make ~110,000-130,000 slow (callback) memory accesses a second each. The list
  wait experiment met a GPU-written entry at 149.7 s (126 s after boot), as gameplay began, and
  turned itself off: it never ran in gameplay (the menus before ran 60 fps either way). Round 16 targets the bank check, the dma read waits and the
  upload and pipeline hitches behind the 1% lows.
- Runs #24-#26 (round 16): #26 built Windows, #25 CachyOS. Not tested: the owner went on to
  round 17.
- Run #27 (round 17, a push: Windows only, the CachyOS job skipped) built `pgo` and `pgo-lto`
  (artifacts `lichii-tuned-r27-pgo`, `lichii-tuned-r27-pgo-lto`, ~90 MB each; pgo-lto in 15 min,
  pgo in 28 min with warm caches), the FSR 4 libraries checked by hash.
- r27 (pgo-lto; logs perf, display = trace-display, r16 = verify-r16; 2026-11-09). The owner
  deleted the shader cache to build it in play (Lichii's developer recommends that for the
  best long-run performance; boot built 1,006 pipelines, so the 1% lows, 4.4 fps, are first
  builds and say nothing yet). The owner sees the P-cores average ~3.0 GHz while playing, at
  most 4.2 GHz (HWiNFO; the 255H's P-cores are rated 5.1 GHz): power-limited.
  - Perf, from 62 s: 29.0 fps (p50 30.9 ms). Per frame: guest cores 4 and 5 26.2 and 24.8 ms,
    the back 22.7 (r23: 27.6; round 16's bank check is 81 ms/s, r23 164), cores 3/1/0/2
    19.3-15.2, the front 13.4, VulkanWorker 4.2; GPU submissions 11.7 ms of a ~29 ms span. The
    guest cores now bound the frame: core 4 runs 778 ms/s of JIT'd guest code, ~144,000 slow
    memory accesses/s, ~11,500 SleepThread calls/s (a yield loop), ~2,300 WaitForAddress and
    ~1,700 SignalToAddress/s; idle spin 44-128 ms/s per core. Front waits on the back: list
    writers 282 ms/s, macro dma read 217, acquire 159 (unchanged from r23); "resume pending"
    134 ms/s at ~80,000/s. MME DMA reads: 97.6% no wait; the ~540/s that wait are "constant
    buffer" overlaps that round 16's mixed reads turn down (no "Mixed MME DMA reads" line was
    logged), so #805's back-run macros are the fix.
  - The menus ran exactly 30 fps until 68 s: the owner's FPS Limit setting was 30 then (the
    log's config dumps; 0 afterwards and in the other logs), not a regression.
  - Display (LICHII_HOST_PACE=2): "present wait unavailable": AMD's driver has no
    VK_KHR_present_wait, so the host pacer can't run on this setup at all.
  - verify-r16: bank check missed 0 in every report; no mixed read differed (none checked).
- Run #28 (round 18, Windows only; CachyOS skipped) built `pgo`, `pgo-lto` and `pgo-train`
  (artifacts `lichii-tuned-r28-pgo` 90 MB, `-pgo-lto` 90 MB, `-pgo-train` 97 MB): the first
  real compile and link of #805, #717's CPU parts and the jitter together.
- r28 (pgo-lto; logs perf, verify-r18, ab-14, ab-17, ab-18; 2026-11-09). The owner: "Smoothest
  performance by far", with shimmering textures where water meets the screen's edges that
  ab-14 (no jitter) didn't have (round 19's fix).
  - Perf, whole log: 37.5 fps (r27 ~29), p50 24.8 ms, 1% low 6.0 (still building pipelines: the
    owner's cache started empty in r27). Per frame: the back 20.0 ms (r27 22.7), guest core 5
    20.0, cores 0/1/4/2/3 15.1-12.5 (r27: core 4 26.2, core 5 24.8), the front 10.8 (13.4).
    Guest code over all six cores ~66 ms a frame (r27 ~105): #717's codegen and its Ocarina
    patches, which also spread the game's heaviest thread over the cores (ab-17, patches off:
    cores 4/5 back at 23.8/21.8 ms, core 4's guest code 762 ms/s instead of ~400; most likely
    `EDEN_NX2_OOT_WIDE_AFFINITY`). Front waits on the back: list writers 50 ms/s (r27 282),
    macro dma read gone (217), acquire 216 (159; more frames), new "map" 57 and "list copy
    writers" 35; the front sleeps 320 ms/s. ab-18 (#805 off): the front 18.4 ms a frame, list
    writers 268, macro dma read 122. ab-14 (no jitter): the same CPU times as perf.
  - The back now: method 117 ms/s (3.78 M methods/s; 2.95 M 3D register writes, 1.12 M of them
    the value already there, 0x586 alone 529 k/s), compute 75, other 91, bank check 84, uniform
    buffers 49, update buffers 37, draw 37, read handles 33; texture memo: ~40% of draws
    resolved from it. EDEN_NX2_SKIP_SAME is on, so the repeats only cost their dispatch.
  - verify-r18: every count 0: no acquire "not met after the drain", 0 of 112,015 command lists
    differed, list indirect 0 differ, texture memo and geometry scans 0 mismatches. The back
    macro check sampled nothing ("0 checked against the old way" with
    EDEN_NX2_BACK_MACRO_VERIFY=64).
  - HWiNFO during play (ThinkBook 16 G7+ IAH, Lenovo Vantage at maximum): CPU package up to ~80 W,
    system ~100 W, "Core Power Limit Exceeded" Yes 100% of the time, P-core effective clocks
    2.6-3.3 GHz, E-cores 1.0-1.5 GHz, ~75 °C, no thermal throttling: power-limited, not
    thermal. Every spin or wasted cycle on any core costs the bottleneck threads clock speed.
  - pgo-train started through perf-trace.bat failed to load the game with the frontend's
    "Error while loading ROM! (XXXX-YYYY) ... Please redump your files" dialog (the owner's
    dump works in every other build). Only the instrumented build differs (-fprofile-generate;
    `common/pgo_training.cpp` only writes profiles), so asked for its log and the dialog's code.
- Run #29 (round 19, Windows only) built `pgo`, `pgo-lto` and `pgo-train` (artifacts
  `lichii-tuned-r29-*`, 90/90/97 MB). Not tested yet.
- Run #30 (round 20, Windows only) built `pgo`, `pgo-lto` and `pgo-train` from tuned4 `cfe4481f0`
  (success, 16 minutes). Superseded by run #31 before testing.
- Run #31 (round 20 with Fritchii's shader preparation, Windows only): tuned4 `bd3ce79e8`.
- Run #32 (round 20 with upstream #961, #951 and the preparation counts, Windows only): tuned4
  `23a9407df`. Run #33 (a stamp file renamed apart from Fritchii's) was cancelled and its commit
  dropped at the owner's request, in favour of shipping the recipe.
- Run #32 built (success, 25 minutes).
- Run #34 (round 20 with the Ocarina recipe in the package, Windows only): tuned4 `e0b1f284f`,
  built `pgo`, `pgo-lto` and `pgo-train` (success, 12 minutes; artifacts `lichii-tuned-r34-*`,
  92/92/99 MB, each 1.8 MB larger than r32's: `recipes/OoT_v131072_cache_v54.nx2recipe` next to
  lichii.exe). Crashed in the preparation (above).
- Run #35 (r34 plus the preparation crash fix, Windows only): tuned4 `6102de6f2`, built `pgo`,
  `pgo-lto` and `pgo-train` (success, 9 minutes; artifacts `lichii-tuned-r35-*`), without the
  agents' Fritchii ports.
- Run #36 (round 20's Fritchii ports, the preparation crash fix, recorded pipelines from
  another device skipped as in Fritchii, the GUI crash writer; Windows only): tuned4 `e9226a635`,
  built `pgo`, `pgo-lto` and `pgo-train` (success, 30 minutes; artifacts `lichii-tuned-r36-*`,
  92/92/99 MB).
  - Test first: does the preparation finish? Same pipelines as r35, so expect ~45-50 minutes
    once per build (the stamp holds the build version). Fritchii says ~15 minutes on an 8-core
    desktop CPU for AMD. The gap is not explained yet; compare the r36 log's rate, or a
    Fritchii log from the same laptop.
  - Not tested: the owner decided (2026-11-09) to restart the tuned fork on Fritchii instead
    (below).
- Owner, 2026-11-09, after running Fritchii v5.5 on the laptop: it "felt smoother than any of
  our runs so far". I read its log as a steady 60 fps against our ~30, and on that the owner
  decided to restart on Fritchii.
  - CORRECTION (2026-11-10): the 60 fps lines ('nx2:vi2 last 600 presents: frame 16693 us',
    1346-1446 s) are the title screen's 99 s video (nx2:vdecsvc frame 1 at 1345 s, frame 5947
    at 1451 s). r28's title screen ran the same (17,263 us, R28 log line 1527).
  - In gameplay (1572-1680 s, ~107 s) Fritchii ran 30.0-30.9 ms frames (~32 fps), with 46-331 ms
    worst frames per 10 s, 0-6 frames over 100 ms per 600 presents, and 172 pipelines built in
    game with 5,751 ms of held draws.
  - r28 in gameplay ran 23.5-29.6 ms frames (34-42 fps; 'Frame times' 37 fps), with ~65 ms
    median worst frames. The scenes differ after the first seconds, and r28 ran FSR 4 and
    perf-trace.
  - So the restart brings no frame rate. What is real is the preparation: Fritchii's took
    562 s for 120,093 pipelines (~30% from AMD's driver cache, so not a cold start); assembling
    the recipe took another 714 s. r35 had run 2,624 s when it crashed near the end.
  - What may explain "smoother" (inferred, untested):
    - Presentation: Fritchii re-applies VSync Mailbox at every launch, and the owner had
      Exclusive fullscreen. Our builds default to FIFO and Borderless, and log neither the
      present mode they got nor the fullscreen mode.
    - Image: FSR 4's camera jitter shimmer. Fritchii uses FXAA and ScaleForce.
    - Settings drift: our builds share Fritchii's qt-config.ini. Fritchii saves a value equal
      to its own default as 'default', and our build then loads ours. r35 ran with async shaders
      off, async presentation off and CPU video decoding.
    - Time step: Fritchii's Dark patch averages ~8 frames (50 ms cap); ours uses each frame's
      own time (100 ms cap). LICHII_DYNFPS_SMOOTHING=8 with LICHII_DYNFPS_MIN_FPS=20
      reproduces Dark's.
  - Preparation speed causes (tuned4):
    - The task/mesh build slot is 1 on this laptop: EDEN_NX2_HEAVY_BUILD_SLOTS=0 lifts it.
      Fritchii has no limit.
    - #781's 3,874 compute programs run ahead of the recipe.
    - Our records cost ~2.3x Fritchii's for a reason not yet measured. Candidates are
      translation, forced 32-wide subgroups, and opening the recipe file per record.
  - The full comparison is in the session's workflow output (compare-fritchii-gameplay).
  The plan as decided: start a new branch from fritchii-v5.5 and bring our changes over in
  A/B-able groups:
  - Radeon: FSR 4, with camera jitter off by default.
  - Laptop CPU: core placement and spin/wait tweaks.
  - Diagnostics, including the crash writer.
  - Launchers.
  The first build is plain Fritchii through our workflow (MSVC and clang+PGO). The owner also
  reported that r35 in game still showed the bright shimmer where water meets the screen edges,
  unchanged from r28. HANDOFF round 19 notes it was gone with ab-14 (no FSR 4 jitter), so
  round 19's mask change did not fix it.
- Run #37 (round 21's first build, `0a62df141`) failed in all three variants: `pgo-lto` at
  CMake's IPO check, `msvc` on a name that hid a global (C4459), `pgo` on one that hid a local
  (-Werror=shadow). Builds now pass `-k 0` to ninja, so one run lists every error left.
- Run #38 failed at 'Apply the tuning patch' in every variant: the runner's core.autocrlf made
  the patch CRLF, and a hunk ending without a newline no longer matched.
  `lichii-tuned/.gitattributes` keeps `*.patch` byte for byte.
- Run #39 (tuned5 `ea2f48a62`, 11 files over Fritchii v5.5): `pgo` and `pgo-lto` built
  (success; artifacts `lichii-tuned-r39-pgo`, `lichii-tuned-r39-pgo-lto`). `msvc` failed at
  1773/1929 on four names in Fritchii's code that hid a member or parameter (C4457 and C4458 in
  kepler_compute.cpp and maxwell_3d.cpp). Ninja stopped there: the Qt frontend waits for
  video_core, so it was never compiled. Renamed in `66500175c`. A local syntax check of the 25
  frontend files Fritchii changes (clang and GCC with Qt 6.4 headers, Linux and MinGW targets)
  found nothing else MSVC would stop on.
- Run #40 (tuned5 `66500175c`): all three variants built (success; `msvc` 17 minutes, `pgo`
  and `pgo-lto` 5 with warm caches; artifacts `lichii-tuned-r40-msvc` 125 MB,
  `lichii-tuned-r40-pgo` and `lichii-tuned-r40-pgo-lto` 92 MB). r40's `pgo` and `pgo-lto`
  differ from r39's only by the renames, so r40 is the one set to test.
- Run #41 (round 22, tuned4 `0e30416f4` = r36 + build 132): failed in all three variants at
  850/1880 on src/common/host_memory.cpp:426, 'class member cannot be redeclared'. We had ported
  #961 before upstream merged it, and the merge kept both copies of the Windows-only
  BackingOffset. Fixed in `604dbd24f`. A MinGW-target syntax check (the round 17 method) of the
  merge's Windows files and the patched Qt files finds nothing else. The Linux check can't see
  `#ifdef _WIN32` code.
- Run #42 (tuned4 `604dbd24f` = r36 + build 132 + the fix): all three variants built (success,
  ~40 minutes; artifacts `lichii-tuned-r42-pgo-lto` and `-pgo` 62 MB, `-pgo-train` 68 MB, smaller
  than r40's because AMD's FSR DLLs are no longer bundled). r42 is the round 22 merge build to test.
- r42 test (owner, 2026-11-10): the precache took much longer than before and Lichii used 23 GB
  of RAM, over twice the usual. The log reads 'Shader precache: 0 cached graphics pipelines, 3876
  of 3996 compute programs and 120124 precompiled pipelines to build on 15 threads' (137.7 s).
  Two causes in the merge:
  - The ~124k jobs went to upstream's shader job scheduler all at once. Its memory budget counts
    only running stages, so translated pipelines waiting for a driver worker piled up.
  - Its driver pool has hardware_concurrency/2 threads (8), and the driver builds bound the
    precache.
  Fixed in tuned4 `5ce380aae`: at most 4 jobs per worker in flight, and the CPU workers run the
  builds themselves, as before the merge.
- Run #43 (tuned4 `5ce380aae`): all three variants built (success, ~6 minutes with warm caches;
  artifacts `lichii-tuned-r43-*`). r43 is r42 plus the precache fix: the build to test the
  preparation with.
- Round 22, step 2 (part) and step 3 (part), for r44:
  - Ocarina FSR without camera jitter or texture LOD bias unless LICHII_FSR_JITTER=1 (tuned4
    `6888a931b`). Upstream's #1061 jitter offsets the scene draws whenever FSR upscales, with no
    switch. CheezyEmu's Ocarina FSR hands FSR a {0, 0} jitter and uses no bias. FSR 3.1/4 can be
    turned on again without the border shimmer.
  - The preparation persists (tuned4 `5f428467d`):
    - The stamp holds Nx2Precompile::PreparationVersion ('tuned 1') instead of the build
      version. Raise it when a translator or key change makes AMD's driver cache miss.
    - Our own file names (`precompiled_prepared.tuned-v54.txt`).
    - Progress is saved every 30 s and on a skip or quit (`precompiled_progress.tuned-v54.txt`),
      and the next launch resumes there.
  - CheezyEmu's small AMD fixes: the blend-state rebuild (858c11554) and #300's HMMA deferral are
    in tuned4 already (round 10's Sharkiix port; `bace6509e`). robustBufferAccess2 (c08bd7432)
    doesn't apply: the owner's driver has no VK_EXT_robustness2 (r42 log).
  - Still to port from CheezyEmu: AMD frame generation (vk_amd_frame_gen, present and swapchain
    hooks, HUD mask shaders), the FSR 4 black-output fallback to FSR 3.1 (3260bc547, e44b16bed;
    upstream fixed the float8 cause with CheezerWheezer's finding), and the FSR presets.
- Run #44 (tuned4 `5f428467d`): all three variants built (success, ~8 minutes; artifacts
  `lichii-tuned-r44-*`). r44 is the build to prepare shaders with: it prepares once, resumes if
  interrupted, and later builds keep it until PreparationVersion is raised.
- r43 test (owner, 2026-11-10): 'the smoothest the game has felt by far ... significantly
  smoother than r35', and graphically the worst, as expected with every anti-aliasing off. Owner's
  settings: anti_aliasing None, scaling filter NearestNeighbor, OoT AA NoSharpening, FSR off; v4's
  async shaders and presentation on.
  - The preparation finished for the first time on the tuned fork: 'Precompiled pipelines: 120085
    of 120124 prepared ... 39 failed', 'Shader precache: finished, 123953 built, 47 failed, 0 not
    reached, 2690.9 s'. The 8 builds in flight when r42 was closed are skipped as poisoned.
  - Play (2949-3049 s, 10-s windows): 33.1-52.7 fps, p50 17.5-28.2 ms; 600-present windows
    19.1-27.1 ms. Some windows had 13-14 runtime pipeline builds (async shaders on). The worst
    windows were 34.0 fps (p99 104 ms) and 39.4 fps. Title and menus were capped at 60 Hz.
  - r44 prepares once more (new stamp format and name), with AMD's driver cache warm from this
    run.
- r44 tests (owner, 2026-11-10):
  - The preparation ran once and then held. A later launch logged '... was prepared for this
    driver already'. The two LP E-cores sit idle and parked during it, by design (round 17's
    placement keeps every thread off the SoC tile).
  - With FSR 4 on: 'Looks much better. No screen border glimmer.' AMD's runtime downloaded into
    `%APPDATA%\eden\fidelityfx` (FidelityFX SDK 2.4.0, upscalers 4.1.2, 3.1.6 and 2.3.4 offered).
    FSR 4.1.2 runs 2560x1440 -> 3840x2160 with jitter (0.000,0.000).
  - Play (101-191 s, 10-s windows): 44.4-57.1 fps, p50 16.6-22.3 ms, p99 25-39 ms, 1% lows
    18.6-32.8 fps, 0.4-2.4% of frames over 33 ms. The 600-present windows were 17.4-21.8 ms.
    FSR 4 costs no frame rate here: the GPU has room.
  - Owner's other settings: anti-aliasing None, scaling filter NearestNeighbor, OoT AA
    NoSharpening.
- Frame generation: the tree has upstream's built-in one ('Enable Frame Generation': type FSR 3
  (default) or LSFG, multiplier 2-4, target frame rate). It runs on host shaders, without AMD's
  runtime. Try it before porting CheezyEmu's AMD frame generation (AMD's FidelityFX runtime, HUD
  mask, swapchain hooks: large and risky).
- r44 frame generation test (owner): Optical Flow at 2x, target 0, flow scale 95: 'I didn't
  notice any difference ... I don't believe framegen was actually on'. The log has no line about
  it: upstream's built-in frame generation reports its state only to the status bar.
  - Likely cause: the generated frames are written from shaders into the present manager's
    frames, which take the swapchain's format. The swapchain always picks B8G8R8A8_UNORM, and
    without shader writes to that format (likely on AMD's driver) frame generation stops at
    UnsupportedSurface.
  - Fixed in tuned4 (round 22, for r45): with frame generation on, the swapchain takes
    R8G8B8A8_UNORM or A2B10G10R10_UNORM_PACK32 when the driver can't store to B8G8R8A8.
  - New log lines: 'Swapchain: <format>, present mode <mode>, shader writes to it
    supported|unsupported'; a warning when frame generation can't run; and every 10 s 'Frame
    generation (Optical Flow): N game frames + M generated per second' (CheezyEmu 93d8f9b86).
  - Frame generation also forces FIFO (VSync) at the monitor's refresh rate.
  - The owner's monitor is 3840x2160, so FSR 4's 4K output is 1:1 and the scaling filter doesn't
    matter.
  - The monitor runs at 160 Hz with FreeSync ('Advanced FreeSync', owner, 2026-11-10). Frame
    generation's forced FIFO with VRR is tear-free. At 2x the ~45-57 fps base gives ~90-115 fps
    inside the VRR range: keep the target at 0.
- Run #45 (tuned4 `9073c8718`): all three variants built (success, ~6 minutes; artifacts
  `lichii-tuned-r45-*`). r45 is r44 plus the frame generation swapchain fix and its log lines. No
  new shader preparation: PreparationVersion is unchanged.
- r45 frame generation test (owner): 'Framegen still did not work', yet the status bar showed 'FG
  OPTICAL FLOW INTERPOLATION (BUILT-IN) 2X' the whole time. The owner also raised
  nx2_frame_rate_limit to 120. The log:
  - 'Swapchain: VK_FORMAT_B8G8R8A8_UNORM, present mode VK_PRESENT_MODE_FIFO_KHR, shader writes to
    it supported'. The driver can store to BGRA8, so r45's format change never applied (harmless,
    kept for drivers that can't).
  - 'Frame generation (Optical Flow): 42 game frames + 42 generated per second at 3840x1976': it
    ran and generated one frame per game frame (82+71, 94+62, 27+21, 40+37 ... 53+53).
  - The swapchain was 3840x1976 with the status bar visible: a maximized window, not
    fullscreen. FSR 4's 4K output was shrunk with Nearest Neighbor, and FreeSync may not engage
    behind DWM.
  - The cause: renderer_vulkan.cpp presents each generated frame and then its game frame back to
    back. With VRR (or a refresh that is no multiple of the game's rate) the generated frame shows
    for one 6 ms refresh, so motion looks unchanged.
  - Fixed in tuned4 (for r46): the present thread spaces a sequence's frames by
    interval / (generations + 1). LICHII_FG_PACING=0 restores upstream's back-to-back presents.
- Run #46 (tuned4 `c1f7068cf`): all three variants built (success, ~6 minutes; artifacts
  `lichii-tuned-r46-*`). r46 is r45 plus the frame generation pacing.
- r46 test (owner): 'slightly smoother than before ... No ghosting and input feels normal'. The
  log shows one generated frame per game frame (46-59 + 46-59 per second, fullscreen 3840x2160
  until a window resize at ~155 s). It can't show where the frames went out. tuned4 `c3fe6e601`
  (for r47) logs 'Frame generation pacing: game frames every X ms; a generated frame went out Y ms
  after the frame before it, its game frame Z ms after it' every 10 s. Even 2x spacing gives
  Y = Z = X/2. It also logs with LICHII_FG_PACING=0.
- Run #47 (tuned4 `c3fe6e601`): all three variants built (success, ~7 minutes; artifacts
  `lichii-tuned-r47-*`). r47 is r46 plus the pacing log.
- r47 pacing check (owner's log): the generated frames sit halfway. Per 10 s: interval 16.6 ms,
  gaps 8.7/8.5; 21.5 -> 10.9/11.0; 18.8 -> 9.6/9.7; 17.4 -> 8.7/9.0 ms. Each gap is a median
  0.51 of the interval. A load window (20 game + 13 generated fps) gave 15.4/15.3 ms around stalls.
  At 271 s the swapchain went to 3574x1974 (a window again).
- CheezyEmu's AMD frame generation (owner asked why not use it): on an RX 9070 XT it is AMD's FSR
  4 frame generation (ML, FP8; FSR 3.1 elsewhere) through AMD's runtime
  (`amd_fidelityfx_framegeneration_vk.dll` beside the loader). AMD's swapchain does the pacing,
  and HUD masks keep the HUD clean. The cost is a ~1,000-line module plus a swapchain replacement
  (acquire and present through AMD's runtime). It is built on CheezyEmu's own FSR 4 runtime,
  upscaler and Ocarina motion code, while ours are upstream's (#1060-#1062), so it needs adapting
  rather than copying. Upstream's runtime download lacks the frame generation DLL.
- Owner (2026-11-10): 'Rip out our FSR4 features in favor of adapting CheezyEmu's. His emulator
  specializes in AMD compatibility and is thoroughly vetted.' The port (inventory, port, three
  reviews, fixes) runs on branch `r22-cheezy-fsr` from tuned4 `c3fe6e601`:
  - Removed: upstream's #1060-#1062 and our LICHII_FSR_JITTER gate.
  - Taken from cheezy/test `4baa89ec5`: the FSR 4 upscaler, the runtime loader (DLLs next to the
    exe), the Ocarina FSR path with HUD masks and presets, the FSR 4 output check with its FSR
    3.1 fallback, and AMD FSR 4 frame generation (FrameGenType::AmdFsr).
  - Kept: the built-in frame generation with our pacing.
  - The workflow's 'AMD FidelityFX libraries' step is back. It puts SDK v2.4.0's signed loader
    (38a56a01...), upscaler (4a7e895a...) and frame generation DLL (d002dabf...) next to
    lichii.exe. The same files CheezyEmu commits.
- CheezyEmu port done (tuned4 `aae4566f5`, fast-forwarded from `r22-cheezy-fsr`): `e08fadcbb`
  (SDK 2.4 headers with frame generation, in `externals/FidelityFX-SDK-2.4`), `a205b1f07` (the
  port), `1b41dda80` (README), `499960272` (review fixes), `aae4566f5` (a blank line).
  - CheezyEmu compiles its Ocarina code only in MSVC builds (`EGLANTINE_STREAMLINE`). Here it is
    compiled in every build; only the NVIDIA DLSS backend stays gated.
  - Settings: `amd_fsr4` (default now on) turns on Ocarina's AMD FSR: FSR 4 on the RX 9070 XT,
    AMD's FSR 3.1 elsewhere. New: `oot_fsr_preset` (Native), `oot_docked_output` (4K),
    `amd_frame_gen_version` (Auto), FrameGenType::AmdFsr (opt-in, needs a restart).
    `ocarina_fsr` is gone. TunedDefaultsVersion stays 4.
  - Needs Resolution 1X. The status bar shows 'FSR 4.x.x' while FSR runs, else 'FSR (reason)'.
  - AMD frame generation counts as 0 built-in generations, so our pacing (LICHII_FG_PACING) stays
    idle with it. The swapchain format choice and forced FIFO apply to the built-in types only.
  - Left out: CheezyEmu's experiment switches (hudless present, HUD rects/diff, FG lag, FSR 4
    HDR, Link depth, rigid-object motion), FSR 4 for other games' DLSS replacement, and its
    unrelated commits (Polaris/GCN, robustBufferAccess2, query flush timeouts and others).
  - Three reviews (fidelity, integration, build) found 14 issues; 12 were fixed in full and 1
    in part. Replacing AMD's swapchain now waits for the last recorded prepare pass. The
    device-wide idles take the submit lock because AMD submits from its own threads. The status
    bar no longer races the GPU thread. Kept as in CheezyEmu: its lock across AMD's present.
  - Checks: every changed C++ file compiles for Linux (clang, gcc) and MinGW clang with no new
    warnings; the Qt files pass fe_check; the shaders compile with glslangValidator. Not run in
    an emulator.
  - Watch in the owner's logs: 'the last prepare pass (tick N) wasn't submitted in time' (a
    fullscreen toggle or alt-tab with AMD frame generation), 'AMD frame generation:' lines, and
    the frame rate against r44 (FSR 4 on: 44-57 fps). Per-draw actor tracking now runs for every
    indexed draw in Ocarina while FSR is on.
- Run #48 (tuned4 `aae4566f5`): all three variants built (success, ~32 minutes; artifacts
  `lichii-tuned-r48-*`, with AMD's loader, upscaler and frame generation DLLs). r48 is r47 with
  CheezyEmu's FSR port in place of upstream's FSR. Owner's FSR settings for 1440p -> 4K as in r44:
  docked, Res 1X, preset Native (or Quality: the same 2560x1440 docked with 4K output), docked
  output 4K; the log should say 'FSR renders from 2560x1440 of 2560x1440' and 'AMD FSR: upscaler
  4.x.x running, 2560x1440 -> 3840x2160'.
- r48 test (owner, 2026-11-10): 'Everything seems to be working perfectly. The best the game has
  felt (aside from the stutters from building shaders which go away with more playtime).'
  - Settings: docked, Exclusive fullscreen, VSync Immediate, Res1X, FSR preset Native, 4K output,
    AMD FSR frame generation (Auto).
  - FSR: FSR 4.1.2 running 2560x1440 -> 3840x2160, output check passed. AMD frame generation:
    FSR 4.0.2 generating frames, HUD masked per pixel. Five swapchain rebuilds (fullscreen
    toggles, 3840x2160 <-> 3574x1974); no 'wasn't submitted in time' warning; no errors in play.
  - Game frame rate in play 41-53 fps per 10 s (r44 FSR 4 without FG: 44-57), shown doubled.
    1% lows 19-31 fps outside two rough windows. At 285-315 s the rate fell to 27 fps, with 20%
    of frames over 33 ms. In those windows the back thread's compute zone took 254-290 ms/s
    (4300-4700 dispatches/s) against 85-107 ms/s elsewhere (6300-7900/s).
  - The first AMD frame generation context took ~10 s to create (frame 28 waited 9.9 s for an
    acquire). Later rebuilds took ~0.2 s.
  - Shader preparation: 'prepared for this driver already', so no re-preparation.
  - The boot shows a cold AMD driver cache. 3026 cached pipelines took 146 s (22 -> 169 s).
    Boot pipeline loads across builds:

    | Build | Pipelines | Load time |
    |---|---|---|
    | r44, a later launch | 2228 | 7 s |
    | r45 | 2999 | 173 s |
    | r46 | 3017 | 15 s |
    | r47 | 3020 | 171 s |
    | r48 | 3026 | 146 s |

    Play builds took 40-2000 ms each, the driver compiling them. So the 120k pipelines prepared
    in r43/r44 didn't stay in AMD's cache for r45, r47 and r48. Our stamp still says
    'prepared', because it assumes the driver cache carries over between builds.
  - Likely cause: AMD's Windows Vulkan cache (%LOCALAPPDATA%\AMD\VkCache) is kept per install
    folder or per exe. Each build was extracted to a new folder; r46 may have been a second
    launch or an overwrite. AMD doesn't document this.
  - Owner test asked for:
    1. Relaunch r48 from the same folder: the boot should be fast.
    2. Copy r48 into the r44 folder (where the 120k preparation ran) and launch from there.
    If that boot is fast and the stutters are gone, AMD keys its cache by folder. Then every
    build goes into that one folder, and the stamp should include the install folder, so that
    a new folder prepares once. If a new build is cold even in the same folder, AMD keys by the
    exe, and the preparation needs a different approach.
  - Owner, second launch of r48 from the same folder: 'loading took around 15 seconds.
    Everything is working as intended.' So the driver cache works within a build and folder.
    Test still open: per folder or per exe. The merged build goes over the r48 folder; its first
    launch's load time decides.
    - Fast: AMD keys by folder. Offer one forced preparation in that folder
      (EDEN_NX2_PRECOMPILED_FORCE=1, ~45 min; a forced run starts over and doesn't resume), so
      that new areas don't stutter. Add the install folder to the stamp, so that a new folder
      prepares once on its own.
    - ~2.5 min again: the cache resets per exe, and every build starts cold. Find another way to
      carry pipelines across builds (Lichii's own VkPipelineCache data, or pipeline binaries).
  - Owner (2026-11-10): 'dont worry about that. Loading shaders slowly once per build upgrade is
    a fine price to pay.' Dropped: no forced preparation, no install-folder stamp, no
    cross-build pipeline carry-over. The stamp logic stays as it is.
- Upstream merge (round 22): nx2 `17baed716` (build 132 + 31) on branch `r22-merge-nx2`, then
  tuned4 fast-forwarded to it. Plan: `merge-plan-nx2.md`.
  - The merge `853ff8213`: 18 files and 33 hunks, each resolved as planned.
  - Upstream's #1245/#1246 replace our upload batching (LICHII_UPLOAD_BATCH is gone) and our
    dispatch barrier ordering.
  - #959 replaces our over-budget buffer fallback.
  - GetFrameGenStatus sums the AMD and built-in frame generation counters, so #796's "N FPS + M
    generated" can't underflow.
  - Follow-ups:
    - `7665cf5f5`: the import keeps file times.
    - `57c6fa941`: the prepared stamp survives a time-only change when the recipe note matches.
    - `bc370c18f`: the crash folder isn't created before the import.
    - `c7c78bd3b` and `ff550600e`: Help > Apply Recommended Settings uses the owner's values.
    - `c16ec4f9e` and `bc22d8072`: docs and launchers name %APPDATA%\lichii; README-tuned.txt
      gains the first-launch import steps.
    - `36948bc36`: the tuned code defaults become precache Always, reactive flushing off, async
      draws Bounded and VRAM Aggressive.
  - Checks on 69 units, for Linux clang and MinGW clang, with the CI warning flags: clean.
    nvhost_as_gpu.cpp was checked with g++ only (local clang 18 crashes on it, tuned4 included).
    Stand-in Catch2 runs: profile_migration 13/13 (the new time case fails without the fix) and
    recommended_settings 5/5. first_run_config's 3 failures are pre-existing: our seeding
    rewrites old configs, which upstream's tests forbid. Tests aren't built in CI.
  - Owner's first launch of the merged build:
    1. Import dialog: Eden -> Keep Old Data, with Clear shader cache unchecked.
    2. Restart once: the import launch doesn't read nx2_switches.txt.
    3. Expect one slow boot, because AMD's driver cache starts cold per exe (accepted). The log
       should say 'was prepared for this driver already', not a 120k preparation.
- Run #49 (tuned `e1ae59949` = nx2 `17baed716` + tuned4 `36948bc36`): all three variants built
  (success, ~33 minutes; artifacts `lichii-tuned-r49-*`, AMD DLLs included). r49 is r48 plus the
  upstream merge and the tuned defaults. To test:
  - the import (Eden -> Keep Old Data, then restart once);
  - the 'prepared already' line in `%APPDATA%\lichii\log\eden_log.txt`;
  - FSR 4 and AMD frame generation;
  - the ocarina crackle fix;
  - the 'N FPS + M generated' display.
- r49 test (owner, 2026-11-11, second launch after the import):
  - Import: every path is in `%APPDATA%\lichii`, and the stamp held ('was prepared for this
    driver already').
  - Settings: the new defaults read as defaults ('--'): precache Always, reactive flushing off,
    async draws Bounded, VRAM Aggressive.
  - Boot: 3329 pipelines in 17 s, first frame at 39 s.
  - FSR 4.1.2 2560x1440 -> 3840x2160, output check passed. AMD frame generation 4.0.2
    generating. AMD's swapchain was first made at the 320x320 placeholder window (harmless,
    rebuilt at the real size).
  - Play: 43-58 fps per 10 s (r48 42-53), 1% lows 21-33 fps. One rough window when FSR
    started (max 1.04 s). The back thread's compute zone 84-153 ms/s (r48 up to 290).
  - Upstream lines: texture cache thresholds 8888/9656 MiB with 21 GB of commit left ('the
    device budget bounds the thresholds', #1210); #1236 refreshed a reused range at an area
    load; #1231's compute bindings past 8 MiB.
  - No device loss and no new errors: the nx2:gsv 903 and prepo:u 10500 stubs were in r48 too.
  - Owner: no ocarina crackle, no white patches after area changes, nothing wrong noticed.
- r50 (tuned4 `cd5482ece`, branch `r22-r50`): the two confirmed items from the Fritchii review
  (0b in the next steps), plus a counter and a test fix.
  - `f5f9e135b`, the zero-page trim. Fritchii's early-exit page check (81ab48f7d); a 300 s
    backoff; every pass over 100 ms logged with its phases (working set query, page reads,
    unlock runs); EDEN_NX2_ZERO_TRIM=0 with the `ab-25-no-zero-trim.bat` A/B against perf-trace.
    The bulk-mapping wait was not taken.
  - `61bba960f` and `6f2ca4c73`, upstream's open PR #1180 cherry-picked unchanged: the water
    ripples keep a remainder per water object. FPS Unlock and Dynamic FPS go from 2 to 5
    records each: a hook at 0x475F8, a nop at 0x4760C, a cave at 0x3C46300-0x3C46393, and timer
    state at 0x4E9FE00-0x4E9FE97 (state_size 0x98). No overlap with our other built-ins.
  - `46dfd5ab5`: with EDEN_NX2_GPU_PROFILE, the 'Scheduler per second' line counts flushes with
    nothing recorded since the last submit. This decides whether Fritchii's EXP61 matters here.
  - `cd5482ece`: the built-in patch test expected the eight-frame Dynamic FPS average.
  - The first build to merge the owner's recorded profile: the `pgo-r49` release, downloaded by
    `fetch_pgo_profiles.py`. Check 'fetch_pgo_profiles:' and 'Merged 1 recorded profile(s)' in
    the CI log.
- Run #50 (tuned `40f6521ab` = nx2 `17baed716` + tuned4 `cd5482ece`): all three variants built
  (artifacts `lichii-tuned-r50-*`).
  - The recorded profile was used: 'fetch_pgo_profiles: pgo-r49/lichii-pgo-22360.zip: 92445945
    bytes', then 'Merged 1 recorded profile(s) into merged.profdata'.
  - The profile changes every object, so the compiler cache missed: pgo took 41 min to build
    (job 45 min), pgo-lto 59 min (job 64 min); pgo-train, which doesn't use it, 13 min.
- r50 test (owner, 2026-11-11; other areas than r49, so frame rates don't compare directly):
  - First launch of the build: 4985 cached pipelines in ~306 s (cold AMD cache, accepted).
    'prepared for this driver already'. 'FPS Unlock (built-in) (5 records)': #1180 is on.
    FSR 4.1.2 and AMD frame generation as before.
  - Play: 57-60 fps in light areas, 36-42 fps in heavier ones. No device loss.
  - Zero-page passes:
    - 0.90-1.10 s at 4.2-9.0 GB resident (r49: 1.33-1.37 s at 7.5-8.5 GB): page reads 700-860
      ms, working set query 150-280 ms, unlock runs 73-218 ms. The early exit saved ~20%:
      touching ~2 M pages is the cost.
    - The backoff works: 10 -> 20 -> 40 -> 80 s, reset by big trims at area loads.
    - Frames around 5 passes in play: 0, 0, 1, 3 and 16 frames over 30 ms. In r49 every pass
      gave 5-18.
    - Next: an A/B with ab-25 (no passes; the owner has 32 GB). Or run the pass on the two
      low-power E-cores our placement leaves idle: they are on the SoC tile, outside the compute
      tile's L3.
  - Empty flushes ('Scheduler per second'): 0-10/s in play. Fritchii's EXP61 (4,600/s there)
    would do nothing here. Settled.
  - Recorded PGO profile: no measurable gain.
    - The back thread's draw zone per draw: 0.593 us in r49, 0.586 us in r50.
    - Guest code per dispatcher entry: 13.7-18.4 us in r49, 15.2-17.3 us in r50 (JIT code;
      PGO doesn't affect it).
    - Lichii's own eglantine.profdata already covers the hot paths. It does no harm, so keep it.
  - New: 16 'Unmapped Device WriteBlock' lines at 396.77 s, into the 1 MiB range that also had
    'Unmapped Device ReadBlock' lines at 357.17 s (buffer_cache.h:4388). A write-back into
    memory the game had freed, in an area r49 didn't visit; none of r50's changes touch it.
  - The 'Userspace PANIC' after 'Shutdown requested' is the usual forced stop.
- Three-way test (owner, 2026-11-10): r35 `pgo-lto`, the official Fritchii v5.5 download and
  upstream Lichii build 132 (`nx2-build-132`, MSVC 1944). The logs were compared by several
  agents; the lines below were rechecked by hand. All three ran Exclusive fullscreen, VSync
  Immediate, Res1X, ScaleForce and FXAA. Fritchii and 132 had FSR off; r35 ran FSR 4 to 4K.
  - Area: probably the same starting area. The four largest guest buffers have the same sizes
    in r35 and 132, and three of the four in Fritchii.
  - Steady moving play ('last 600 presents'):
    - r35: 21.4 and 22.7 ms (r35 log L2548, L2784).
    - 132: 21.1-23.1 ms (L2061, L2122, L6204, L6257).
    - Fritchii: 24.1 and 24.6 ms (L1459, L1792).
    So Fritchii was ~10% slower, as on 11-09. On the same 10 s Worst-frame clock r35 ran
    42.7-47.5 fps and Fritchii 39.1-41.6. No build waited on the GPU fence.
  - Stutter in play:
    - Fritchii built 0 pipelines (L1916). Its worst frame was 65.1 ms, in the first 10 s
      (L1319).
    - r35 built at least 20 in its first 12 s of play. That gave 9 frames of 50-141 ms; of its
      2391 play frames (mean 22.0 ms), none after 72.9 s was over 42 ms.
    - 132 held draws for 1-3.4 s AMD compiles on new content (L2533), also on its warm second
      boot (window max 3.5 s, L6151). It has no graphics-pipeline preparation: its precache is
      3899 compute programs in 155 s (L373). Its async shaders default off.
  - Why, mostly run state rather than code:
    - Fritchii ran with async shaders and async presentation on (its defaults). r35 and 132 had
      both off; r35's were reset by the shared qt-config.ini (Settings drift above).
    - Fritchii's disk cache held 3307 pipelines, 843 of them merged at boot (L320) from the
      shared vulkan.fork-v54.bin. r35 had just added its runtime builds to that file, so
      Fritchii likely ran last.
    - r35's preparation was skipped after 9.9 s (L397: the loading screen's Skip button).
    - Fritchii's host pacer did not run: 'present wait unavailable, frames are not paced'
      (L298).
    - Time step: Fritchii used Dark's ~8-frame average and r35 its raw step. The owner
      preferred the raw step in round 7.
  - Leads for Fritchii's slower pace (untested):
    - Its GPU threads are not pinned on this 6-P-core CPU (L313; EDEN_NX2_PIN_THREADS=1 forces
      pinning).
    - It runs without upstream's texture memo (its EXP32, off by default) and without #858,
      #921 and #1079.
  - Not measurable from these logs: choppiness below 33 ms (only r35 logs each frame) and what
    reached the screen.
  - r36 and Fritchii write the same `precompiled_prepared.fork-v54.txt`, each with its own
    build version (e9226a635 nx2_precompile.cpp:160, fritchii-v5.5
    vk_pipeline_cache.cpp:3680). Alternating the two re-runs each one's preparation.
  - Recommended to the owner:
    - No fresh start on 132: it had the worst stutter and has no preparation.
    - Keep tuned4 (r36), and hold the port onto tuned5 until one A/B:
      1. Fritchii.
      2. Fritchii with EDEN_NX2_PIN_THREADS=1.
      3. r36 with async shaders and presentation on, FSR off and its preparation finished.
      Capture frame times in each run. Run Fritchii first, because of the shared stamp.
    - Low confidence: r36's preparation is untested, and the smoothness below 33 ms is
      unmeasured.
- The owner, 2026-11-08, playing r26 for fun: a downloaded ~90,000-pipeline Ocarina cache
  "worked the first time I loaded it", then r26 loaded only ~23,000. Likely cause: round 16
  keys graphics pipelines by the vertex attribute types their programs read (#408) and builds
  each key once, so records that differ only in attribute types the shaders never read are one
  pipeline. A cache gathered over many sessions and builds holds many such copies (#408 counted
  191 keys for 31 shader sets of Ocarina's task and mesh programs), while a build without the
  masking (round 15, another fork) counts every record. r26 logs "Pipeline cache: N records
  repeat a pipeline already cached" (round 17: "N graphics records share their pipeline with
  an earlier one") beside "Total Pipeline Count". Nothing is lost, and neither round rewrites
  the file for it (round 17's #815 removes only exact duplicates and incomplete records). Not
  confirmed from a log yet: if the repeats are far fewer than ~67,000, read the "left out"
  lines and check that `nx2_switches.txt` next to lichii.exe still has
  `EDEN_NX2_PRELOAD_ALL_PIPELINES=1` (the log doesn't show the switches).
- The owner's shared Ocarina pipeline cache (~22,000 pipelines, recorded with Lichii build 86 =
  `b06d1818e`, cache v53 like r18) never loaded: put in place of `shader\0400486024dec000\
  vulkan.fork-v53.bin`, it built ~1,700 pipelines at boot (even with
  `EDEN_NX2_PRELOAD_ALL_PIPELINES=1`), the game built everything again in game, and the file grew
  with the new records. Cause: a graphics key holds the recording driver's id and version
  (`FixedPipelineState::driver_id`, `driver_version`, inside `Size()`), so no record from another
  GPU or driver version ever met a runtime key, and records whose dynamic state flags differ were
  dropped at load without a word. Round 13 logs the drops by reason; d92a29336 gives records this
  driver's id and version and converts records made without dynamic vertex input (NVIDIA before
  580.119.02 has it off) and with the other color write enable form. Other flag differences
  (an EDS level) still drop; the log names them. A file of another cache version is deleted at
  boot, so the owner keeps a copy. With 22,000 records, `EDEN_NX2_PRELOAD_ALL_PIPELINES=1` builds
  them all at boot (~0.45 MB of driver memory each); without it 4,096 build at boot and the rest
  in the background.
- r20 boot with that cache (owner keeps `EDEN_NX2_PRELOAD_ALL_PIPELINES=1`, 16 GB VRAM): "started
  from the version 53 cache", all 21,062 graphics records from another driver, and all 21,062 left
  out as "recorded with extended dynamic state 2 logic op on (off here)"; Total Pipeline Count
  1707 (the compute ones). eb2b411de: a key holds op 0 while the logic op is off (Refresh and at
  load), so those records turn into this device's form unless their logic op is on.
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

0. Round 22's queue (owner, 2026-11-10), in order:
   - Test the CheezyEmu FSR 4 and frame generation port (done in tuned4 `aae4566f5`, run #48).
   - Done: merged upstream Lichii nx2 `17baed716` (31 commits after build 132), tuned4
     `36948bc36`. Next: the owner's test of the merged build. Check:
     - the import;
     - the 'prepared already' line;
     - FSR 4 and AMD frame generation;
     - the ocarina crackle fix (#1196);
     - the performance against r48.
     Earlier notes from the planning: Planned in `lichii-tuned/merge-plan-nx2.md` (per-hunk detail in
     `merge-plan-nx2.json`): 18 conflicted files, 29 hunks. Blocker: #1253's data-folder import
     copies without file times, which would re-prepare all ~120k pipelines; Copy() must keep them
     (same push as the merge). Owner decisions (2026-11-10): the recommended table matches the
     owner's config (precache Always, pinning off, reactive flushing off, async draws Bounded,
     VRAM Aggressive); import with Keep Old Data, sharing %APPDATA%\lichii with official builds.
     The merge runs on branch `r22-merge-nx2`.
   - Analyze the newest Fritchii builds (tags up to fritchii-v5.7; we last read v5.5) for
     anything worth taking.
0b. Fritchii v5.6-v5.8.2 review (2026-11-11). Workflow `wug5im6pd`: 146 commits, four area
   analysts and four skeptics. Details in `lichii-tuned/fritchii-v58-review.json`.
   - Confirmed (awaiting the owner's go-ahead):
     1. Zero-page trim fixes on upstream #961's code: 81ab48f7d's early-exit page check, a
        300 s backoff, every pass over 100 ms logged with its phases, EDEN_NX2_ZERO_TRIM=0 for
        an A/B, and optionally the bulk-mapping wait. In r49 each pass takes 1.33-1.37 s at
        7.5-8.5 GB resident and lines up with 1-2 s windows of 25-30 ms frames.
     2. Water ripples at fractional frame steps: merge upstream's OPEN PR #1180 (e7ea60c2b +
        1169d58c5), not Fritchii's d389685a4/063dbb741. At 43-58 fps the ripples run at
        72-97% speed.
   - Watch upstream:
     - #1161, cheaper reports and fences: ~1%, unmeasured.
     - #1186/#1181, register shadowing: no rejection seen in the owner's logs; they change the
       SPIR-V.
     - #1225, the JIT warm-up.
     - #1129, the texture cache's critical threshold.
     - The mesh fallback setting.
     - EXP48 and EXP63.
   - Rejected:
     - EXP61: our 500 us flush batch already avoids the empty submits.
     - EXP58: it conflicts with upstream #976, which we carry.
     - EXP56: it would undo upstream #311.
     - JIT_FAST_COMPILE / #1266: verification is already off in all_safe_optimizations.
     - Fritchii's translator-hash stamp: it would invalidate the owner's preparation.
     - AMD_APP_CACHE, the high refresh mode (it competes with AMD frame generation), and the
       settings revisions (FXAA None: the owner already runs AA None).
0c. The GPU back thread is the bottleneck (2026-11-11). Workflow `wf8k93g9e`, details in
   `lichii-tuned/back-thread-plan.json`.
   - Per game frame in the r49 and r50 logs:

     | Thread | Busy |
     |---|---|
     | Back thread | 18.4 / 21.3 ms of wall time; unprofiled CPU (`gpu_back_cpu_ms_per_frame`) 15.6-21.5 ms |
     | Each guest core | 7-9.5 ms |
     | Front | ~8.3 ms busy |
     | Vulkan worker | ~4.5 ms |
     | GPU (submission sum) | 10.9 ms (p90 12.9) |

   - Back thread zones per frame:
     - method 3.4 ms (~104k methods);
     - compute 2.3-3.4 ms (16-22 us per dispatch);
     - 'other' 2.1-2.6 ms;
     - uniform buffers 1.5 ms;
     - per-draw zones ~5-6 ms;
     - 0 delta draws.
   - The profile itself costs ~1 ms per frame: CallMethod skips the WriteStateRegister shortcut,
     and non-3D entries take full scopes. Normal play should run without EDEN_NX2_GPU_PROFILE.
   - Skeptic's verdict: builds A and B save ~3-5 ms, taking typical play from ~45-50 to ~52-58
     fps. Heavy areas (Castle Town, water) end around 15-17 ms. A locked 60 everywhere is
     uncertain and needs build C, plus the texture collector Finish hitches gone (upstream
     #1306/#1311) for the 1% lows.
   - Plan:
     - Now (no build): pin_gpu_threads=true A/B.
     - Build A: upstream #1216 9ad7c7cee behind a TEST switch (vertex stride words,
       render-enable addresses, back-macro DeferWords: the two biggest fast-draw breaks); a
       break-mask counter; #1251 (ring cache-line layout); quick wins (#1161 58144d258,
       d5c9589d0, 0593fd75f, cccac7bb7, the InvalidateGPUCache early-out); #717 CONTIG behind a
       switch; profile fixes; compute sub-zones; TEST zones.
     - Build B: #1161's fast-draw half (792e12a47); #1274 FoldInstance; bank-check bitsets;
       #1161's reports/fences half; per-draw trims; maybe EXP63.
     - Build C, by data: pair blocks, a dispatch memo, delta-after-lookup, the stream buffer in
       host memory, fence submit batching.
   - Owner (2026-11-11): 'Start the build now ... I accept all of your recommendations on moving
     forward'. B and C follow the plan. The ab-25 (no zero trim) test comes later.
   - Build A done: tuned4 `7adb1ae18` (branch `r22-r51`, workflow `wl1j39f8q`: one
     implementer, two reviewers, one fixer).
     - Run #51 (`93e92691a`): `pgo` and `pgo-train` built. `pgo-lto` failed at the link:
       ld.lld reported an undefined "thread-local initialization routine for
       Nx2Profile::t_compute_programs". That was the first `inline thread_local` in a header
       with a constructor.
     - `7adb1ae18` moves it into a function-local thread_local, the tree's existing pattern for
       non-trivial thread_locals.
     - Build A ships as run #52 (artifacts `r52-*`). Build B therefore becomes run #53 or later;
       its branch keeps the name `r22-r52`.
     - Run #52 passed (07:23 UTC). Artifacts `lichii-tuned-r52-pgo-lto`, `-pgo` and
       `-pgo-train`. The PGO step merged the recorded `pgo-r49` profile (92 MB zip).
     - The owner's test plan:
       - perf-trace-noprofile (the baseline);
       - ab-26;
       - perf-trace (the full profile, for build C);
       - verify-r51;
       - optionally ab-27, ab-28, ab-25 and pin_gpu_threads.
     - Rule for new code: no `inline thread_local` of a non-trivial type in a header.
     - #1216 9ad7c7cee picked with DjouWish's authorship; it keeps our Writes::Caches.
       EDEN_NX2_TEST_1216=0 (ab-26) restores tuned4's breaks. g_dynamic_strides follows the
       pipeline cache's DynamicFeatures (EDS and VIDS, with the setting). Boot line:
       'Fast draws: extended dynamic state ..., dynamic strides ...'.
     - #1251 5100362a6 (layout, no switch).
     - #1161 58144d258, d5c9589d0, 0593fd75f and cccac7bb7, plus the InvalidateGPUCache
       early-out. The switchable ones sit behind EDEN_NX2_TEST_QUICK=0 (ab-28).
     - #717's CONTIG check (MaxContinuousRange before VirtualRangeCache::Query, graphics and
       compute) behind EDEN_NX2_TEST_CONTIG=0 (ab-27).
     - Profile lines:
       - fast-draw break masks, the 'different kind' split and the breaking thunks;
       - ring entries per kind;
       - compute sub-zones and 'Slow dispatch';
       - submits by site;
       - fence submit/commit, the idle hook, deferred calls and lock holds;
       - the stream copy (at =2) and a stream-write probe at startup.
     - New launchers: perf-trace-noprofile (the baseline for the unprofiled A/Bs), ab-26/27/28,
       verify-r51 (FAST_DRAW_VERIFY, BACK_MACRO_VERIFY=64, PERF_VERIFY).
     - Under the profile, the back macros' 3D writes now count under 'deferred call' rather than
       'method'. Compare 'method' and 'other' with r50 only through unprofiled frame times.
     - Before nx2: consider taking the InvalidateGPUCache early-out on the GPU thread only. The
       unit tests now run in CI (below).
   - Unit tests: `.github/workflows/lichii-tuned-tests.yml` (2026-11-11).
     - It builds upstream's `tests` program from this branch plus the patch, with the Windows
       builds' MinGW clang (no PGO, warnings not fatal), and runs it.
     - It is its own workflow, so lichii-tuned.yml's run numbers (the build names) don't move.
     - It runs on every patch push, or by hand (workflow_dispatch on `tuned`; it worked from the
       API although the workflow is not on the default branch).
     - Run 1 (build A): 2031 of 2032 build steps compiled. tests/core/file_sys_parsers.cpp needs
       `-fsized-deallocation`: MinGW's libstdc++ declares the sized operator delete that
       fssystem_bucket_tree.h uses only with that flag. It is upstream code, and lichii.exe
       never instantiates it.
     - Run 3 (build B): 1222 of 1224 test cases passed. Two independent traces found no defect,
       so be33a34bc adapts the two expectations:
       - the #1216 fence test: #1161's AppendRange merges overlapping ranges, so the test now
         writes its short report at another address;
       - our native bank test (7864fd62e), failing since the EXP9 port: mode 3 compares only
         pages a reason applies to, so the test now flags the page as a collected write.
     - Run 4 (build B + be33a34bc, 2026-11-11 09:45 UTC): all 1224 test cases passed
       (70.9 M assertions). A warm run takes ~4 minutes.
   - Build B done: tuned4 `0d3fb0540` (branch `r22-r52`), run #53 passed at 09:03 UTC. Artifacts
     `lichii-tuned-r53-pgo-lto`, `-pgo` and `-pgo-train`; its launchers are named r52
     (verify-r52, ab-29..35). The owner tests build A (r52) first. Workflow
     `wncy3e2y2` (implementer, reviewer, fixer), then a deep review `wp8s3x0f9`: one finder per item
     and two skeptics per finding.
     - #1161 792e12a47's fast-draw half, a hand port behind EDEN_NX2_TEST_1161_DRAWS (ab-29):
       - full delta lists widen;
       - handle checks cover the handle words only;
       - attribute formats become draw parameters.
     - #1274 FoldInstance (EDEN_NX2_FOLD_INSTANCES, ab-30). The fold is flushed at every Reserve,
       Publish, Drain, WaitFor and FrontDmaRead.
     - Native bank check bitsets (EDEN_NX2_TEST_BANK_BITS, ab-31). LICHII_BANK_CHECK=2 adds
       'bitsets missed'.
     - #1161's reports/fences half (EDEN_NX2_TEST_1161_REPORTS, ab-32). Not switchable: the
       report operation layout and the run-time GetPointer.
       - #1099 was not taken: our tree has nx2's delayed releases instead of b3f4ee7d3's
         is_synced.
     - Per-draw trims (EDEN_NX2_TEST_DRAW_TRIMS, ab-33).
     - Per-thread GPU page translation cache (EDEN_NX2_TEST_XLATE_CACHE, ab-34).
     - #1306 and #1311 (EDEN_NX2_TEST_1306, ab-35; #1306 fixes the RX 9070 XT device-heap crash
       breaking pots in Hyrule Castle).
     - Launcher verify-r52.
     - The deep review confirmed one major defect, fixed in `419af3688`: with ab-29's change on, a
       delta draw that kept attribute formats consumed Dirty::VertexInput without refreshing
       PipelineCache::vertex_attribute_types. A later full draw with other programs could then key
       a now-constant attribute with its old type, reading undefined vertex input.
       KeepsAttributeTypes now compares the types the programs read (as the key holds them) and
       stores the registers' types. FAST_DRAW_VERIFY could not see it.
     - Two minor fixes:
       - verify counting with the switch off;
       - `0d3fb0540`: on the drawing thread, deferred buffer invalidations now apply before a
         download.
     - Not taken yet: e05a72847 (computed-handle delta draws). It waits for build A's mask run at
       the Kokiri water.
   - Owner's build A logs (r52, 2026-11-11):
     - Runs: perf-trace-noprofile x2 (the first crashed), ab-26, perf-trace, verify-r51. No
       visual glitches; verify-r51 is very slow, as expected.
     - First look over whole runs: back thread 14.7 ms/frame unprofiled (median 14.6), 52.0 fps
       in gameplay windows, 1% low 22.3. ab-26: 16.1 (median 14.5), 49.9 fps, 1% low 18.0.
       Profiled: r52 19.9 (median 17.2) vs r50 21.0 (median 19.0).
     - Dynamic strides are on (EDS and VIDS). The stream buffer takes 5.6-5.7 GB/s over OCuLink,
       against 33-37 GB/s into host memory.
     - Full analysis: workflow `wb5xmbo0v` (four analysts, a synthesis and a skeptic who
       recomputed every number; scripts in the session scratchpad).
       - Correct: verify-r51 resolved 510,316 delta draws in full with 0 mismatches. 0 device loss,
         faults, memory traces or shader rejections in all five logs. BACK_MACRO_VERIFY=64 only
         covered the 64 title-screen calls of macro 0x142.
       - #1216 is neutral: +0.14-0.2 ms/frame of back CPU (95% interval includes 0), front
         +0.27 ms. It removed the breaks it targeted: deferred-call breaks went from 3-16k/s to
         47-128/s and 0x700 to 0. But those gaps now end at:
         - attribute formats (0x458-0x47f): 12-20k/s, against r50's ~4.5k;
         - computed-handle pipelines: 11.3k/s, against 2.8k.
         Delta draws rose 16% (683 vs 590 per frame), and 'prepare us per draw' is unchanged at
         4.5. Build B's ab-29 (attribute formats) targets the first of these.
       - Build A vs r50, both profiled: -0.9 to -1.15 ms/frame at 2000 draws (low-medium
         confidence). The profile costs 0.8-1.05 us/draw (1.6-3.2 ms/frame), not ~1 ms. There is
         no unprofiled r50 run, so the real gain is between ~0 and ~2 ms. Against r49: no gain.
       - 60 fps holds where back CPU is at most ~13 ms (windows at 1836-2129 draws/frame held it
         with 10.7-15.1 ms). Content matters more than draw count. The idle gap waiting for the
         front's publishes is 3.5-3.8 ms. Heavy scenes: back 19.6 ms, frame 24.1 ms.
       - Next limits:
         - busiest guest core 12-14.6 ms with spin;
         - front 8.9-9.8 ms;
         - Vulkan worker 5.1-5.5 ms;
         - GPU submission sum 11.3-12.0 ms median, which may exclude frame generation.
       - 1% lows come from scene loads (1-2.6 s frames) and Finish spikes.
       - Build C evidence (the skeptic's corrections included):
         - #16 e05a72847 (computed-handle delta draws): mean 11.3k/s, peaks 17-26k/s. Plausible
           to take (upstream, and the largest remaining fast-draw lever). The 0.4-1.2 ms is a
           profiled ceiling, and the handle-data checks come after the counter.
         - #26 stream buffer in host memory: qualifies by the literal rule (5.53-5.70 GB/s < 8).
           The saving is unmeasured: the probe times 8 MiB bursts, while real writes are ~400 B
           copies (108-202k/s). Its host figure of 83,886 GB/s is a probe bug: the optimizer
           removed the copy.
         - FSR 4's dispatch on the back thread (shader 6bd913e1f34e83eb, ~0.9 ms): 0.55-1.2
           ms/frame in the 'compute FSR' zone. Sub-zone it first; the plan keeps the FSR path
           unchanged.
         - #23b dirty bitmap: 'compute synchronize' is 0.7-1.2 ms/frame. Sub-zone it first.
         - #17 and #19 need an EDEN_NX2_GPU_PROFILE=2 run. #28 and #30 are decided on r54 (ab-29,
           FoldInstance).
         - Rejected by their numbers: #20 (resolve is 1-4% of a dispatch), #21, #23a, #24, #27,
           #29.
   - The crash (perf-trace-noprofile 1, Hyrule Field to Kokiri Forest), from
     %APPDATA%\lichii\crash_dumps\lichii-crash-19116.txt and a 0-byte .dmp:
     - An access violation on the GPU back thread: draw, ConfigureImpl, texture upload,
       ExecuteUnswizzlePlan, SwizzleImpl's GOB loop. The read was at input + 0xaa000 in a 3 MiB
       run (384 blocks of 8 KiB, line 0, block 85), and the page was the first byte past the
       14 GiB guest memory backing.
     - Cause: 43321b8db (round 16, from tomk134/lichii-custom e8e50e797) read a run in place
       after IsContinuousRange, which checks only device contiguity, and then read the whole run
       from the first page's host pointer. The SMMU maps device pages to guest memory page by
       page, so the read took other guest memory (garbled textures, silently) and crashed at the
       end of guest memory.
     - Fix d3251a67c: MemoryManager::GetSpan, which also checks the continuity_tracker. Workflow
       `wgxv9g29t` confirmed it two ways:
       - a refuter traced every alternative;
       - an ASan harness ran 145k plan geometries with no read past guest_bytes.
     - The ~15 s freeze before the crash was MiniDumpWriteDump with MiniDumpWithFullMemoryInfo
       walking the placeholders. cbe51e88a drops that flag, falls back to a plain dump, logs the
       memory layout at boot, and adds a unit test.
     - These ship as r54 (build B + fix).
     - The same crash again on r53 (owner's ab31 run, lichii-crash-17360.txt), so not
       ab31-specific:
       - same registers: r13 0x600, rbx 0x55, r15 0x154;
       - the input again exactly 0xaa000 before the end of guest memory;
       - a LinePool worker thread this time;
       - the crash function (r53 0x549560) is byte-identical to r52's 0x59bfa0, and the
         faulting instruction is the same vmovups [rax+r9];
       - the .dmp is again 0 bytes (r53 still had the old flags).
       It looks like one texture the game places at the top of guest memory at a scene load.
       r54 has the fix.
   - Build B logs (r53: ab29-ab35, perf-trace, verify-r52; r54: noprofile, fine). Workflow
     `w2kwfrehr` with a skeptic; the follow-up with the r54 baseline is `wr7fp8z10`.
     - Correct. verify-r52 found:
       - bank: 1,669,120 binds, 0 missed, 0 bitsets missed;
       - read downloads: 127, 0 bytes differ;
       - fast draw: 720,136 draws, 0 mismatches;
       - texture memo and geometry: 0.
       - The delta-descriptor and UBO-keep verifies were vacuous: FAST_DRAW_VERIFY leaves no
         delta draws.
       - No device loss or faults, memory traces, rejections or image failures.
       - New only in ab35: 16 'Read64 of unmapped GPU address' lines at one load (two addresses),
         probably content.
     - Not faster than build A: B-A +0.26..+0.46 ms/frame of back CPU (fits and pairs; profiled
       +0.07..+0.24), inside the 0.2-0.6 ms that separates A's own sessions. The r54 all-on run
       (15.99 ms at 2000 draws/frame, 20.28 at 3000) matches the one-off consensus.
     - Every switch's back interval includes 0 (|effect| < ~0.5 ms):
       - #1161 draws: attribute-format and overflow breaks went to 0; delta share 37% -> 46%;
         state-changed full draws 662 -> 428 per frame at 2000 draws. But full draws cost
         6.48 -> 7.42 us and delta draws +0.13-0.28 us.
       - Bank bitsets: the bank check zone -0.21 ms/frame (CI -0.30..-0.10), the only clear back
         saving.
       - FoldInstance: front -0.3 ms (weak evidence).
       - Trims: moved 0.31 ms from flush work into draw, net 0.
       - Reports, translation cache: noise.
       - #1306: 'halves Finish waits' is unsupported.
     - Lesson: 'full draws converted x (full - delta cost)' over-predicts by >1 ms, because
       fast-draw gains reappear as dearer full draws. The 1-1.5 ms estimated for #16 is
       unsupported.
     - 60 fps needs back <= ~13 ms. It runs 13.7 ms in steady medium play (1670 draws/frame),
       16.0 at 2000 and 20.3 at 3000.
     - Build C: #28 fails (13-43 register-only gaps per frame against 100); #20 stays rejected;
       #26 qualifies only by the literal probe rule (the fine profile's stream copy is ~2 ms/s:
       see the follow-up).
   - ab-25 (EDEN_NX2_ZERO_TRIM=0, r54, profiled, 223 s): the zero-page trim does not cause
     hitches. Keep it on.
     - In runs with trims, frames within 2 s of a pass look worse (r53 perf-trace: 14% over
       33 ms near passes against 4.6% elsewhere; r54-fine 15.8% against 4.9%).
     - The passes land next to area loads. In ab-25 (no passes) the same game-time windows are
       as bad or worse: 57-63 s 52% over 33 ms against 48%/36% with trims, 96-101 s 31% against
       16%/25%, 176-181 s 9% against 7%/3%.
     - So the hitches there are the loads. The trim frees several GB of RAM.
     - Script: scratchpad trimscripts/trim_hitches.py.
     - Run #54 passed (15:19 UTC): `lichii-tuned-r54-pgo-lto`, `-pgo` and `-pgo-train`. Unit
       tests run 5: all 1225 test cases pass, including the new host-split GetSpan and unswizzle
       test.
     - The owner tests r54 instead of r53. The plan, with r53's launchers:
       - perf-trace-noprofile;
       - ab-29;
       - ab-30;
       - perf-trace;
       - a perf-trace-fine.bat the owner creates (EDEN_NX2_GPU_PROFILE=2, to decide #17, #19
         and #26);
       - verify-r52.
   - Dropped: EDS3 blending (it would change every pipeline key and invalidate the
     preparation), hot pages (stale data), raising MaxWrites, CB fusion, #717 as a bundle,
     EXP26/28/53/61, #1131.
   - Same-route r49 and r50 perf traces (owner, 2026-11-11), compared with r52-r54. Workflow
     `we21ul2p8` (route, back, zone and PGO analysts, skeptics, synthesis).
     - Builds A and B run 0.15-0.45 ms/frame of back CPU faster than r50 at 2000 draws/frame
       (profiled; central ~0.3), not the planned 1.5-3 ms. B is not faster than A.
     - r50 - r49 (-1.1 ms) is mostly r49's slow session (a cold driver cache, vkQueueSubmit
       +10%). The build's share is -0.5 +/- 0.6 ms.
     - Unprofiled back CPU in ms/frame at 1500/2000/3000 draws per frame:
       - r50: ~13.9 / 16.1-16.2 / 20.5-20.7;
       - A: 13.43 / 15.71 / 20.28;
       - B: 13.91 / 16.06 / 20.37.
     - Why the plan fell short:
       - the fast-draw conversions were small net: chained breaks, conversions on pipelines
         that take no delta draws, and delta draws got dearer (1.05 -> 1.36 us);
       - work moved between zones instead of going away (the trims moved 0.34 ms; B's method
         zone grew 0.17-0.23);
       - the recorded PGO profile's staleness costs 0-0.4 ms.
     - The profile's own cost: EDEN_NX2_GPU_PROFILE=1 costs 1.9-2.3 ms/frame at 2000
       draws/frame (2.5-4.7 fps), =2 costs 2.4-2.7. That is why the noprofile runs look faster;
       time builds with unprofiled runs.
     - MapPhysicalMemory runs at similar rates in every session (549-715 calls/s), but its host
       cost per call varies 143-342 us between sessions and 16-1141 us between 10 s windows.
       That drives the 23-44 fps spread after loads (S2a) between sessions of one build. r55's
       heap trace measures it.
     - 60 fps outlook:

       | Play | Draws/frame | Result |
       |---|---|---|
       | Light | 1000-1400 | 59 fps |
       | Steady medium | ~1700 | 55-57 fps, 0.5-1.4 ms short |
       | Heavier | 2000 | 2.5-2.9 ms short |
       | Third area | ~2300 | ~5 ms short, 41-44 fps |
       | Heaviest areas | 3250-3500 | 6.5-7 ms short, 40-44 fps |

       Build C realistically saves 1-1.5 ms (range 0.5-3). After the back, the limits are the
       busiest guest core (11.6-14.7 ms) and the front (7.8-10.9).
     - Build B's switches:
       - Unconditional in r56 (no performance claim): #1161 draws (after a clean verify-r55),
         bank bitsets, #1306/#1311, QUICK, CONTIG and FoldInstance.
       - #1161 reports stay unless a fine A/B shows they cost >= 0.1 ms.
       - The trims and the translation cache go (off by default since r55).
       - Decided by r55's arms: #1216 is dropped if ab-26 minus the baseline is <= -0.2 ms;
         #16 becomes unconditional if ab-36 shows >= 0.3 ms saved and verify-r55 is clean.
       - The zero-page trim stays.
   - Owner decisions (2026-11-11):
     - First option 3: more clock for the back thread on the power-limited CPU (less spinning,
       pinning, the power mode).
     - Then option 1: the per-draw cost from ~4.3 to ~1.9 us (pre-decoded state, Ocarina macro
       HLE, delta draws nearly everywhere).
     - Frame generation stays AMD FSR 2x. The adaptive target rate (frame_gen_target_rate)
       drives only Lichii's own frame generation.
   - r55 (run #55, tuned4 `3bf40a3c2` = branch `r22-r55`): build C part 1 and option 3's tests.
     - Build C part 1: #16 (e05a72847, delta draws for pipelines with computed texture handles;
       ab-36 turns it off), zones inside 'compute synchronize' and 'compute FSR', the stream
       write probe fixed, the per-draw trims and the translation cache off by default
       (EDEN_NX2_TEST_DRAW_TRIMS=1 / EDEN_NX2_TEST_XLATE_CACHE=1 bring one back), no clock reads
       per draw unless logged, and the reason a late publish to an idle back waited (profile).
     - EDEN_NX2_HEAP_TRACE: the host cost of MapPhysicalMemory and UnmapPhysicalMemory every
       10 s, in every timing launcher (the 24-44 fps spread between sessions after loads).
     - One boot line, "TEST switches in effect:", names every TEST and A/B switch and its state.
     - Option 3 (the power-limited CPU):
       - EDEN_NX2_CLOCK_TRACE (src/common/clock_trace.*, Windows): every 10 s the clock, CPU
         time (polls and work), P/E/LP-E share and migrations of the back, front, Vulkan worker
         and guest cores; each core class's busy, utility and clock; package power where Windows
         has it; the power mode, AC and battery. A start line has the core classes, the plan, the
         power mode and the plan's processor settings. In every timing launcher.
       - EDEN_NX2_PIN_GPU_THREADS=0/1/2 overrides the pin_gpu_threads setting for one run; 2
         (TEST) pins only the GPU front and back and keeps other threads off their cores.
       - LICHII_UMWAIT_C02=1 (TEST): UMWAIT asks for C0.2, and an idle guest core waits its
         whole poll in one UMWAIT.
       - Arms (unprofiled, frame, heap and clock traces): ab-37 no kernel spinning, ab-38
         EDEN_NX2_PIN_GPU_THREADS=2, ab-39 C0.2, ab-40 all three with a 30 us idle poll, ab-41
         the 30 us poll alone. README-tuned has the run protocol.
     - pgo-train names each session's file lichii-pgo-<time>-<pid>.profraw. CI: each recording
       is cut to its last dump and merged 70:1; a symbols artifact per variant; the tests
       workflow also runs the clock trace's unit test with EDEN_NX2_CLOCK_TRACE=1 on its
       Windows runner (the only real Windows the trace runs on before the owner's laptop).
     - Reviews: the assembly review fixed a use-after-free in the heap trace (a stopped
       session's globals), launchers without the heap trace, missing switch-line entries and
       three CI problems. Option 3: workflow `wv9q5mza3` (implement, code and measurement
       reviews: 22 findings, all addressed in 11 commits); its fixes were re-reviewed in
       `wuy4loezg`.
     - Profile: r55 changes the back loop, Backoff and the idle path that pgo-r49 covers, so
       compare r55 only with its own baseline.
     - Test plan (frame generation AMD FSR 2x throughout):
       - Sitting 1, option 3, by README-tuned's protocol: one warm-up baseline, one baseline
         with HWiNFO open (clock check), then blocks N, ab-37, ab-39, ab-40, ab-38 and the
         reverse (N = perf-trace-noprofile), a third block if there is time.
       - Sitting 2, build C: A B C C B A A B C with A = perf-trace-noprofile, B = ab-36 (#16
         off), C = ab-26 (#1216 off); perf-trace-fine once; verify-r55 for ~5 min at the Kokiri
         water and Castle Town.
1. Plan to 60 fps with 1% lows above 30 at 1440p -> 4K FSR 4 (agreed with the owner after r22).
   r27: the frame is ~33 ms; the guest cores 4 and 5 (26.2, 24.8 ms) now bound it, then the back
   (22.7): each must fall under ~15 ms; the GPU has room (~12 ms of 16.7 with FSR 4). The host
   pacer can't run on AMD's driver (no VK_KHR_present_wait); FreeSync is the answer below 60.
   Done since: round 16's bank check (back 27.6 -> 22.7), #253's jitter idea for FSR 4
   (round 18), the list writer wait (#805, round 18). Still open: which pages miss fastmem
   (~144,000 slow accesses a second per core; #717's trap fast paths make them cheaper, not
   fewer), parallel command recording if per-draw work still dominates the back, area-load
   stalls (ASTC decode and uploads off the back thread, #166's NUP cache).
2. Round 20's build (run #30; round 19's checks carry over): the water at the screen's edges with
   the default (reactive 0.5), ab-19 (round 18's mask) and ab-14 (no jitter); ab-20 (no kernel
   spins) and ab-21 (round 19's timer wait) against the default on the power-limited laptop
   (frame rate, guest core and back times); the front's list writer and drain waits after #980
   and #911; pgo-train should load the game now: record a session with it for an Ocarina
   profile. r36: first, does the shader preparation finish? (The 'Precompiled pipelines: N of
   M prepared' line in the log; if it crashes, the two files in %APPDATA%\eden\crash_dumps.)
   Then round 20's Fritchii ports: the default against ab-22 (shaders), ab-23
   (guest memory) and ab-24 (render targets), each logged like perf-trace (frame rate, back
   and guest core times, the slow clear and churn lines), and verify-r20 for a while (its
   'differ' count, its tombstone hits and the bank check's 'without the frame check (mode 3)'
   count must stay 0). r28's
   bottlenecks: the back (20.0 ms a frame: 3.78 M methods/s, 1.12 M 3D register writes that
   repeat the value, 0x586 alone 529 k/s; can the front drop them before they are queued?) and
   guest core 5 (20.0 ms, ~640 ms/s of guest code). Then: the rest of #805 (upload reordering
   ddf01f582/a0ecdb040, the read-download set), #717's native Ocarina macros
   (`macro_hle_oot.*`), a recorded Ocarina PGO profile once the pgo-train load failure is
   understood (needs its log), #579/#741's graphics warm-up only if graphics pipeline
   libraries work on RDNA 4 (AMD-Gfx-Drivers#126), the noinline switches after an A/B, #806
   after an A/B at the Hyrule Field spawn (grass), Vendichii's FSR 3 frame generation if the
   owner wants 60 shown frames before 60 real ones. Power: the CPU is power-limited all the time
   (~80 W package), so less spinning anywhere means higher clocks for the bottleneck threads;
   a placement that puts the lighter guest cores on E-cores under the power limit is untested
   (strict placement, all on P-cores, didn't help in r11/r18).
3. Summarize the new log with `python3 -I lichii-tuned/lichii_log_summary.py eden_log.txt`; check
   draws held for pipelines, the front's drain after #345, and the `Finish sites:` lines.
4. Then: remove the biggest Finish sources; look at the 17.5 ms submission (tessellation?);
   try `ab-8-skip-unbuilt-draws.bat` for first-time shaders and `ab-4-strict-placement.bat`;
   PGO training on Ocarina. If a glitch shows up that the official build doesn't have and the
   A/B launchers don't remove, turn `msvc` back on to tell the compiler from the source.
5. Each round, port new commits from the included pull requests (#354 was still changing late
   on 2026-11-06): `git fetch https://github.com/zortos293/Lichii refs/pull/<n>/head` and compare
   patch ids with tuned4. Per `AGENTS.md`, also check Eglantine (Djihads80/Eglantine).
