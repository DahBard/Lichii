# Lichii tuned: project handoff

State of the work on 2026-11-06 (end of the first session), for the next Claude session.

## Goal

Make [Lichii](https://github.com/zortos293/Lichii) (Switch / Switch 2 emulator; Eden / Eglantine
fork) run as fast and as cleanly as possible on the owner's setup, and iterate with him: build,
he tests and sends `eden_log.txt`, analyze, next build.

- Windows 11, Intel Core Ultra 7 255H laptop (Arrow Lake-H: 6 P-cores, 8 E-cores on the shared
  L3, 2 low-power E-cores on the SoC tile), AMD Radeon RX 9070 XT over OCuLink (PCIe 4.0 x4),
  monitor on the 9070 XT. AMD driver 2.0.416, Vulkan 1.4.359.
- Game: The Legend of Zelda: Ocarina of Time (Switch 2), program id `0400486024DEC000`. Runs in
  handheld mode, 2560x1440 layout with a 1920x1080 crop, resolution 1x.
- He wants the recommended speed settings as defaults, and asked for shaders to be compiled
  before playing.
- His log is `%APPDATA%\eden\log\eden_log.txt` (the previous launch's is `eden_log.txt.old.txt`).

## How builds are made

- Branch `tuned` of DahBard/Lichii = upstream commit `588bfa6` + `.github/workflows/lichii-tuned.yml`
  + this folder. The workflow applies `lichii-tuned/lichii-tuned.patch` and builds on every push to
  `tuned` that touches `lichii-tuned/**` or the workflow (a commit message with `[skip ci]` doesn't).
- Variants (windows-2022): `pgo` (MSYS2 UCRT64 clang, repo's `eglantine.profdata`,
  `-march=x86-64-v3 -mtune=arrowlake -ffp-contract=off`), `pgo-lto` (+ ThinLTO), `msvc` (built
  like the official MSVC download, for compiler A/B of glitches). `pgo-train` (instrumented, writes
  `lichii-pgo-*.profraw`) is commented out; profiles put in `lichii-tuned/pgo/` (raw or zipped) are
  merged 4:1 with Lichii's. Artifacts: `lichii-tuned-r<run>-<variant>`. Each variant queues only
  behind the same variant of the previous run. ~40-50 min with warm caches.
- Source of the patch: branch `tuned4` (36 commits over 588bfa6). Its full history is
  `lichii-tuned/tuned4.bundle`: in any Lichii clone, `git fetch lichii-tuned/tuned4.bundle
  tuned4:tuned4`. The patch is `git diff --binary 588bfa6 tuned4`; check it with
  `git apply --check` on a clean 588bfa6 checkout before committing it.
- The first session had no git push access to the fork, so it committed the patch through the
  GitHub web upload page in Chrome. With the repository attached to the session, push instead.
  `tuned4` itself was never pushed: push it as its own branch (it doesn't trigger builds).

## What the tuned build contains (tuned4)

Upstream nx2 up to `09b78bd2df`, plus open upstream pull requests:

- #319 "Ocarina of Time anti-aliasing: No sharpening" bypasses the game's tensor AA network
  (AMD runs its HMMA math in software); #300 builds those HMMA pipelines off the wait path on AMD.
- #337 white flashes at GPU accuracy Low, #292 fog haze, #329 leaf holes, #321 held draws build
  their pipeline inline, #299 Dynamic FPS down to 10 fps, #259 opt-in preflush (`EDEN_NX2_PREFLUSH=1`).
- #345 (5 commits) precise macro DMA reads and cheaper semaphore acquires on the split GPU front
  (its author measured ~48% of the front draining the back in Ocarina). Large; if round 3 is
  unstable, drop it first.
- #346 denormal controls, #353 render target write mask (violet halos on AMD), #354 bindless
  handle tracking for the clustered lighting shadow lookups (green tint growing on AMD),
  #376 texture cache collector downloads with one GPU wait, #378 source part (`EDEN_AMD_FP16=1`).

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

## Next steps

1. Run #9 ("Round 3b") on the fork's Actions page: when `pgo-lto` / `pgo` finish, have the owner
   test r9-pgo-lto (judge from the second launch; the first recompiles shaders) with
   `perf-trace.bat` at the same spots, and compare with the numbers above.
2. Summarize the new log with `python3 -I lichii-tuned/lichii_log_summary.py eden_log.txt`; check
   draws held for pipelines, the front's drain after #345, and the `Finish sites:` lines.
3. Then: remove the biggest Finish sources; look at the 17.5 ms submission (tessellation?);
   try `ab-8-skip-unbuilt-draws.bat` for first-time shaders and `ab-4-strict-placement.bat`;
   PGO training on Ocarina; per `AGENTS.md`, check Eglantine (Djihads80/Eglantine) for fixes to
   port; compare `msvc` against `pgo` if glitches remain.
