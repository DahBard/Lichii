# Ocarina of Time performance experiments

## Target and ownership (2026-11-09)

Target: correct gameplay approaching 170 real frames/s (5.882 ms/frame), excluding
generated or duplicated frames. The local test host is an Intel i7-11700 (8 cores,
16 threads), NVIDIA RTX 3080 10 GiB (driver 615.78.08), 32 GiB RAM, Linux/GNOME
Wayland and a 170 Hz display. It is not the Windows host described in AGENTS.md.

Integration branch: `perf/oot-170hz`, starting at `ec0c23dc0f`. Existing Claude
workers own GPUBack buffer/pipeline work (`t3/98a75d06`) and frame generation /
presentation (`t3/126c3bb0`). C4 owns front-thread work. Changes and builds in the
integration checkout are coordinated with their orchestrator.

The existing baseline is about 29.5 real presents/s in the saved Hyrule Field
scene. This is not evidence of a fundamental engine limit, nor of correct game
simulation at 170 Hz. Emulation speed and simulation timing still need separate
validation before any high-frame-rate claim.

## Measurement protocol

Use the existing isolated `.cache/rig` through `tools/lab.sh` (exclusive flock),
never a second emulator or the owner's configuration. `tools/sbench.sh` restores
the owner cache snapshot, loads the newest save, and runs four frame-counted
pan/walk cycles. Settings: High GPU accuracy, docked, 1x resolution, owner VSync
and 170 FPS limit, Bounded 2 ms native async draws. Frame generation remains off.
Record actual settings from each run's log. Reject runs with concurrent compiler
load, memory pressure, different starting scenes, or crashes. Keep binary hashes,
logs, screenshot, input results and frame-time response per run.

`frame_times.fps`, median and p99 use the full measurement window. Its
`intervals_ms` contains only the last 120 rounded intervals: do not use it to
claim full-window 1% or 0.1% lows. Presents measure renderer delivery, not distinct
simulation steps or display scanout. Verify screenshots and game timing separately.

## C4-01: GPU split cache contention (retained)

Hypothesis: the front's hot ring positions share cache space with `back_engine`,
which the back writes at engine switches. Separating them reduces cache transfers
without changing command ordering, synchronization or rendered output.

Sources: [upstream issue 807](https://github.com/Eglantine-Community/Eglantine/issues/807)
and existing fork commit `b22e54a24b47f4de4fcb332825c76f9a3185cad7` (layout-only port
of `886ac17e004156c34596d2664f4f9c4824f0ed44`). The issue's Mario Kart measurements
do not establish an Ocarina gain. `upstream/nx2` was fetched and is fully contained
in the starting branch (376 local commits ahead, zero behind). PR 686's macro
stack experiment and PR 794's GPU split diff were inspected; neither supplies
this layout in this branch.

Candidate: `alignas(128)` on `write_position` and `back_engine` in `gpu_split.h`.
No experiment switch or behavior change. Baseline binary:
`.cache/C4/base/lichii`, SHA-256
`ec959a31d1911012ddf75e27438841ba46055f6c2f79ff5dafb50bc667d0879e`.

Validation: existing tests, then baseline/candidate/candidate/baseline gameplay
runs using separately preserved binaries. Retain only a repeatable improvement;
otherwise revert and preserve the negative result here. The existing GCC Release
build is used for comparable local A/B; clang is not installed on this host.

Next candidates, in order: reduce measured macro JIT overhead, then investigate
batching macro writes. Front-thread gains may be hidden while GPUBack remains
the critical path; do not infer an FPS gain from microbenchmarks alone.

Results (FG off, High accuracy, unchanged settings):

| Run | Scenario | Layout | FPS | Median ms | p99 ms | Max ms |
|---|---|---|---:|---:|---:|---:|
| C4_a2_base | Pan/walk | Baseline | 28.84 | 34.97 | 61.32 | 100.65 |
| C4_b_aligned | Pan/walk | Aligned | 29.97 | 33.58 | 59.65 | 90.29 |
| C4_c_aligned | Pan/walk | Aligned | 29.99 | 33.66 | 55.32 | 95.93 |
| C4_d_base | Pan/walk | Baseline | 29.39 | 34.47 | 61.27 | 92.89 |
| C4_e_static_base | Fixed field | Baseline | 29.72 | 31.89 | 40.93 | 74.53 |
| C4_f_static_aligned | Fixed field | Aligned | 30.34 | 31.24 | 44.13 | 48.21 |

The two candidate gameplay runs beat both baseline runs (about 3% gain in mean
FPS across the pairs). The fixed-field pair supports the direction but its p99
was worse, so this is not a proven improvement in every tail metric. The six
runs are a small sample, not evidence across all scenes. No shader builds or
held draws occurred in their gameplay windows. The first `C4_a_base` run was
excluded after drive failure. Fixed-view screenshots show the same scene and no
obvious visual regression; animation differs. All 1040 existing test cases passed.
The interrupted assistant turn did not interrupt `C4_f_static_aligned`; its
normal completion and cleanup were recovered.

Candidate SHA-256: `7490906c509713c7cf638dbc7761a42866629d8da78181f04a47acb74bc62cbc`.
Artifacts: `.cache/C4/`, `.cache/rig/runs/C4_*` (local, not uploaded).

## C4-02: full-window frame statistics

Add 1% and 0.1% lows, p99.9 and population standard deviation to `frame_times`.
This measures all retained intervals rather than the 120-element rounded tail.
A synthetic 1000-interval regression with one early 101 ms spike fails on the
old implementation; tests also cover empty windows and rounded-up tail counts.
This is measurement support, not an FPS optimization.

## C4-03: macro DMA handoffs (investigating)

`W2_h1_trace` shows thousands of DMA read drains per second, chiefly macro
0xe2c. Instrumentation and concurrent build load contaminated that trace's FPS;
use it to locate work, not as a performance baseline.

Existing origin branch `egla91/back-macros-nx2`, commit `26aad9dc55`, runs eligible
memory-reading macros on the back. It needs effect analysis, register ownership
tracking and APIs absent here; evaluate as a separate substantial port.
Origin PR 1032 skips some waits and query flushes, but explicitly acknowledges
unresolved stale-query correctness. Do not adopt that shortcut.
