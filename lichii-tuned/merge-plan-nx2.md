# Upstream merge plan: nx2 `17baed716` into tuned4

Planned 2026-11-10 (round 22). The merge starts after the owner has tested r48 (CheezyEmu's FSR
port) and any fixes it needs are on tuned4. Workflow `wd5hvn2q1` wrote the plan: seven area
planners, an open-PR survey and a cross-area critique. The main thread checked it. The per-hunk
resolutions are in `merge-plan-nx2.json`. Line numbers there refer to a trial merge of tuned4
`aae4566f5`, so re-run the trial merge on the final tuned4 and re-map the hunks before resolving.

## Scope

- Upstream nx2 `17baed716` has 31 commits on top of build 132 (`d2eaae4f3`): 101 files,
  +4534/-559. No PR merged after it (checked 2026-11-10 22:20).
- The trial merge leaves 18 conflicted files with 29 hunks:
  - vk_rasterizer.cpp 6, main_window.cpp 4, vk_scheduler.cpp 3;
  - 2 each in vk_scheduler.h, vk_buffer_cache.cpp, settings.h, decoders.cpp, performance_overlay.cpp;
  - 1 each in the other 10 files.
- 25 more files changed on both sides merged without conflicts. The critique reviewed them.
- Verdicts:
  - Take all 31 commits.
  - #784 is already in tuned4 byte for byte: keep ours in decoders.cpp.
  - Adapted: #1249, #1128, #1246, #1253, #1200, #796, #959.
  - #941 is NVIDIA-only and auto-merges.
- None of the 31 commits touches the shader recompiler, pipeline keys, host shaders,
  CACHE_VERSION or PreparationVersion. So the merge itself doesn't invalidate the owner's
  prepared pipelines, apart from the import problem below.

## Blocker: the data-folder import (#1253) and the shader preparation

#1253 moves the profile from `%APPDATA%\eden` to `%APPDATA%\lichii`. On the first launch a
dialog offers to import the old folder (Copy, Move or Link). The import's recovery journal lives
in `%APPDATA%\lichii.migration`.

The problem is in `ProfileMigration::Copy()` (`src/common/fs/profile_migration.cpp:195-208`). It
uses `fs::copy_file`, and our MinGW/libstdc++ build doesn't keep file modified times when it
copies. Our prepared stamp includes the `.nx2prep` file's time (`vk_pipeline_cache.cpp:2449-2454`;
the main thread checked this), and so does upstream's warm marker (`shader_warm_builder.cpp:330`).
A Copy or Move import would therefore re-prepare all ~120k pipelines. The import runs once per
profile, so a build without the fix can't be undone except by hand.

Required, in the same push as the merge:
1. In `Copy()`, after `fs::copy_file(...)`, add
   `fs::last_write_time(target, fs::last_write_time(item.path()));`. Add a Catch2 case "Copy keeps
   file times" in `src/tests/common/profile_migration.cpp`; it must fail without the fix.
2. A safety net in our own code. If the stamp (and the progress key) differs only in the modified
   time, and the `.recipe.txt` note matches the recipe, rewrite the stamp and skip the preparation.
   This also covers Move, restores from a backup and manual copies.
3. `nx2_crash_dump.cpp`: don't create the default crash folder at install. It would create
   `%APPDATA%\lichii` before the import runs. Create the folder in the handler instead.
4. Text changes:
   - `%APPDATA%\eden` -> `%APPDATA%\lichii` in nx2_crash_dump.h, dist/nx2_switches.txt:22 and :27,
     perf-trace.bat:2, README-tuned.txt:5 and :191, HANDOFF and CLAUDE.md.
   - Add a "First launch: import" section to README-tuned.txt.
   - Remove `LICHII_UPLOAD_BATCH=0` from ab-18-round17-gpu-threads.bat: upstream's #1245
     replaces our upload batching.

The owner's import steps:
1. Close every Lichii, Fritchii and Eden window. Check there is free space for a second copy of
   `%APPDATA%\eden`; optionally back it up.
2. In the dialog, pick Eden, then "Keep Old Data" (Copy). Leave "Clear shader cache" unchecked.
3. After "Data was imported successfully", close Lichii and launch it again. The import launch
   doesn't read the owner's nx2_switches.txt.

Copy leaves `%APPDATA%\eden` alone. Build 132 and Fritchii keep using it, so the tuned build no
longer shares settings with them, and round 21's overwrite can't happen again. Link shares one
folder and always keeps the preparation, but brings that overwrite back.

## Resolution by area

| Area | Upstream | Files | Resolution |
|---|---|---|---|
| DLSS presets | #1249, #1128 | settings.h/.cpp, settings_enums.h, renderer_base.h, renderer_vulkan.h, vk_rasterizer.cpp (3 hunks), vk_ocarina_camera_motion.cpp | Keep both sides' settings, enums and virtuals. In settings.h keep **our `amd_fsr4{linkage, true, ...}`** and drop upstream's `ocarina_fsr`. In vk_rasterizer.cpp's TryOcarinaUpscale hunks, the port's code wins. In vk_ocarina_camera_motion.cpp, keep ours and rename the ctor parameter `pool` -> `pool_`. Nothing changes on AMD: Streamline is MSVC-only. |
| Vulkan submit | #1245, #1246, #1154 | vk_scheduler.cpp/.h, upload_batch.h, vk_buffer_cache.cpp, vk_rasterizer.cpp (3 hunks), vk_pipeline_cache.cpp | Upstream's #1245 is the same mechanism as our 5bc127b41/af2f01087 (from #805), so take theirs and delete our leftovers: the duplicate members in vk_scheduler.h, the duplicate include, `g_upload_batching`. #1246 = our dc31ee678: take its naming (`barrier_recorded`) without `marking_effects`. #1154: take `mapped_env`. |
| Memory | #959, #1210, #1231, #1199, #1236, #853 | vulkan_memory_allocator.cpp, buffer_cache.h | Take #959 in place of our cd0d323cb, also catching `VK_ERROR_OUT_OF_HOST_MEMORY`; drop `over_budget`. Take #1210 (the OoT OOM fix), #1199 and #1236 (OoT white areas after area changes). |
| Frontend FPS | #796 (+ #1249/#1200/#1264 hunks) | main_window.cpp/.h, performance_overlay.cpp, renderer_vulkan.h | Upstream's measured "N FPS + M generated" replaces our "(60 + FG)" text. Keep the FSR/FG buttons, idle reasons and the overlay features line. Make GetFrameGenStatus monotonic: upstream's u64 subtraction underflows when FG is toggled. Watch for the hidden `frame_gen` redeclaration in main_window.cpp. One resolver handles all of these files. |
| Recommended settings | #1200, #1212, #1264 | settings.h, main_window.cpp/.h | Take the Help > Apply/Restore mechanism. There is no prompt at startup, and nothing changes until the owner clicks Apply. Adapt the table in `recommended_settings.h` so Apply doesn't undo the tuned values (owner decision below). |
| Data folder | #1253, #1258 | (no conflicts) | See the blocker above. |
| Small, CI | #1196 (ocarina crackle), #850, #1197, #1108, #1126, #972, #1168, #1252, #1257, #784, #1254, #1255, #1260 | decoders.cpp | `git checkout --ours` on decoders.cpp. The rest auto-merges. Move `check_patch_warnings.py`'s base to `17baed716`. |

Cross-area rules from the critique:
- One resolver per file for vk_rasterizer.cpp, main_window.cpp, renderer_vulkan.h and
  settings.h.
- Checkpoint greps that must come back empty: `ocarina_fsr`, `marking_effects`, `MarkEffects`,
  `oot_effects_`, `OcarinaTemporalEnabled`, `oot_backend`, `pass_1440p`, `native_resolution`,
  `barrier_first`, `program_env`, `over_budget`, `g_upload_batching`, `amd_frame_gen`, and the
  `game_presets.h` include.
- Checkpoint greps that must match: `amd_fsr4{linkage, true`, and exactly one
  `GetFrameGenStatus` in main_window.cpp.

## Procedure

0. Gate: the owner has tested r48 and any port fixes are on tuned4. Check nx2 is still
   `17baed716`. If it moved, merge `17baed716` first and take the newer commits in a follow-up.
1. Tag `tuned4-pre-nx2`. Branch `r22-merge-nx2` in a scratch worktree. Run
   `git merge --no-ff --no-commit 17baed716` and confirm the 18 files and 29 hunks.
2. Resolve in this order, one resolver per file:
   - the mechanical hunks (upload_batch.h, vk_scheduler.*, vk_buffer_cache.cpp,
     vk_pipeline_cache.cpp, buffer_cache.h, decoders.cpp);
   - vulkan_memory_allocator.cpp;
   - settings and DLSS;
   - vk_rasterizer.cpp (all 6 hunks);
   - the frontend.
3. Checkpoint A, before committing the merge:
   - no conflict markers;
   - `git diff --cached --check`;
   - the greps above.
   Commit the merge with all 31 commits listed, and push `r22-merge-nx2` (pushing it doesn't
   trigger CI).
4. Follow-up commits:
   - (a) the Copy() time fix and its test;
   - (b) the stamp safety net;
   - (c) the crash-dump folder;
   - (d) the recommended-settings table;
   - (e) the text changes.
5. Checkpoint B:
   - Cloud clang/Ninja build of video_core, yuzu, tests, profile_migration_tests and
     profile_import_tests with -Werror.
   - Run the tests in an isolated `HOME`/`XDG_*`, because upstream's config now throws on a
     legacy ~/.local/share/eden.
   - `check_patch_warnings.py --base 17baed716`.
   - Hand-review the `_WIN32` lines. The Linux build can't see them.
6. Fast-forward tuned4 and push it. On `tuned`:
   - Merge nx2.
   - Regenerate the patch against `17baed716`.
   - Run `git apply --check` on a clean `17baed716` tree.
   - Update README, HANDOFF, CLAUDE.md and the workflow comment.
   - Push once, with the merge commit, the patch and the docs together, so no CI run applies the
     old patch to the new base.
7. Watch pgo and pgo-lto for -Werror failures. Fix them on tuned4, regenerate the patch and push.

## Owner decisions

1. **Recommended-settings table.** Help > Apply only changes settings when the owner clicks it,
   but upstream's table would undo four tuned values. Planned edits:
   - Shader precache: Ask instead of Off. Off skips the preparation.
   - GPU thread pinning: off instead of on, for the hybrid 255H.
   - Reactive flushing: off instead of on. Owner's call; upstream's value is defensible.
   - Native async draws: Wait instead of Bounded. Wait keeps Fritchii v5's async discard. Owner's
     call too.
2. **Import mode.** Copy (recommended; Fritchii and 132 keep `%APPDATA%\eden`) or Link.
3. **Upstream report.** Upstream's warm marker has the same copy-time problem in MinGW and Linux
   builds. Offering the Copy() fix upstream would be an outward action, so only with the owner's
   go-ahead.

## Owner's first test after the merge

1. **Import:** steps above. `%APPDATA%\lichii.migration\complete` exists, and `%APPDATA%\eden` is
   unchanged.
2. **Second launch, no game:**
   - No recommended-settings dialog.
   - Settings, saves, add-ons and nx2_switches are intact. No DLSS rows on AMD.
3. **Ocarina boot:**
   - No precache prompt.
   - The log shows 'Precompiled pipelines: ... was prepared for this driver already'.
   - No ~120k 'pipelines to build' line.
   - Boot time is close to r48's second launch.
4. **FSR and frame generation:**
   - 2560x1440 -> 3840x2160, and the status bar shows 'FSR 4.x.x'.
   - With frame generation on, the FPS reads 'N FPS + M generated'.
   - Toggling frame generation mid-game shows no huge number.
5. **Audio:** sustained ocarina notes without crackle (#1196).
6. **Performance A/B against r48** in the same scenes (perf-trace):
   - fps and 1% lows;
   - back-thread ms/s;
   - commands per frame;
   - the 'Native compute storage bindings' and 'Host memory ... thresholds' lines.

   The upstream changes we take despite a possible CPU cost: #1231 (1 GiB read-only compute
   bindings), #1236 (per-page refresh at area loads), #1245 (a CanAdd check per region), #1197
   (a zstd scan at RomFS build). Report any measured cost upstream rather than diverge.
7. **Area changes** (Kokiri -> Hyrule Field, Castle Town, indoors): no white areas. A
   30-minute session: no memory-budget errors, no device loss.

## Open upstream PRs (survey of 167; none in this merge)

- **Later candidates:**
  - #1266 and #1265: dynarmic per-block CPU cuts, small.
  - #354: Ocarina green tint on AMD. We carry its early commits; about 20 newer ones are
    missing.
  - #1180: Ocarina water ripples with the FPS patches.
  - #1129: texture collector ceiling on 10+ GiB cards. It must compose with #1210.
  - #1186, #1181, #1008: Ocarina shader fixes.
- **Shader PRs:** decide the stamp policy before taking any of them. Shader front-end changes
  alter the SPIR-V, so AMD's driver cache misses, while our stamp still says "prepared". Either
  bump PreparationVersion with the owner's agreement, or accept one rebuild at boot.
- **Don't take:** the warm-up and pipeline-library series (#1166, #1173, #1174, #1177, #1178,
  #1183, #1188). By their own descriptions they make the precache run again.
- **Watch:**
  - Frame generation: #1224 (overlaps LICHII_FG_PACING), #783.
  - CPU and Ocarina timing: #1225 (JIT warm-up, large), #1244, #911, #1099, #933, #974/#1216.
  - AMD driver: #1087, #1088.
  - Memory and textures: #976, #836.
