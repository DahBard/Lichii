# CLAUDE.md

@AGENTS.md

## lichii-tuned

This fork's `tuned` branch builds a tuned Windows Lichii for the owner's laptop (Core Ultra 7 255H +
RX 9070 XT over OCuLink, testing Ocarina of Time). The base is our own fork, branch `tuned4`, with
upstream Lichii merged in. Round 21 (2026-11-09) briefly restarted on Fritchii v5.5 (branch `tuned5`,
now kept for reference only). On 2026-11-10 the owner chose to stay on the tuned fork after testing
r35, Fritchii v5.5 and upstream build 132 (see HANDOFF): Fritchii ran ~10% slower in steady play, and
build 132 has no graphics shader preparation.

Priorities (owner, 2026-11-10):
- Upstream Lichii (zortos293/Lichii: nx2 and its pull requests) comes first. Where our code and
  upstream's do the same thing, upstream's wins.
- Upstream's shader translator changes are merged by default (owner, 2026-11-12). They don't raise
  PreparationVersion: new builds have started with a cold AMD driver cache anyway, and the owner
  accepts slow shader loading once per build (2026-11-10).
- Fritchii (Frizti/Lichii, branch `oot-gather-fix`, tags `fritchii-v*`): only features with a large
  benefit for this setup or for Ocarina.
- CheezerWheezer/CheezyEmu: its FSR, FSR frame generation and Radeon optimizations, if they meet our
  standard.
- Take changes from other forks only for a very good reason or when the owner names them. Vet
  everything that doesn't come from upstream Lichii heavily.

Round 22, in this order:
1. Merge build 132.
2. Replace our FSR 4 and its camera jitter with CheezyEmu's FSR, frame generation and Radeon work.
   The jitter makes the screen borders shimmer brightly all the time, most visibly on water.
3. Make the shader preparation complete once and persist. On the tuned fork it never finished, and
   it re-ran all ~120k pipelines at every launch; Fritchii prepares once.

Then (owner, 2026-11-10), in this order:
1. Finish and test the CheezyEmu FSR 4 and frame generation port.
2. A big merge with upstream Lichii: done on 2026-11-10 with nx2 `17baed716` (plan:
   `lichii-tuned/merge-plan-nx2.md`). The data folder is now `%APPDATA%\lichii`, shared with
   official Lichii builds.
3. Once that merge is complete, analyze the newest Fritchii builds for anything worth taking.

Then (owner, 2026-11-12; the options are in HANDOFF's next steps), in this order:
1. Option 3. If it improves performance enough that medium-intensity areas (steady play at
   ~1700 draws a frame) hold a steady 50 fps or more, integrate it into a stable build. If not,
   do option 1 first.
2. Merge with upstream Lichii right after that stable build. Don't wait for Zortos's
   multithreaded GPU back thread; option 2 (the same thing) is dropped, and his version comes
   in with a later merge once it is released. The owner had it started on 2026-11-12, before
   option 3's verdict: done that day with nx2 `2eb5f081c` (merge `bd1ce62ff` on branch
   `r22-merge-nx2b`, now tuned4 and the base under the patch).
3. After the merge, critically review Fritchii's new commits for anything worth adding.

Then (owner, later on 2026-11-12):
- Option 3 is done: the owner fixed the Windows power mode (Best performance; EPP 0 and boost
  aggressive helped a little more, not significantly). "Use your best judgement on what to include
  from option 3 considering the results of the best performance run."
- Assume CI run #56 (r55 + merge 2) goes well. Review Fritchii's new commits (since
  fritchii-v5.8.2) and CheezyEmu (cheezy/test since our port base `4baa89ec5`). Take only what is
  highly likely to (1) noticeably reduce the number or severity of stutters, (2) reduce the GPU
  back thread's frame times, or (3) greatly improve quality of life; for Fritchii also a large
  benefit for this setup or for Ocarina.
- Done on 2026-11-12: r58 (CI run #58), the stable build with the cleanup and the one change the
  review took (CheezyEmu's kept FSR mesh list); the review's findings are in HANDOFF. The
  scheduled resume task that carried the work across the owner's usage windows is deleted.

Usage: the owner's plan has limited usage per 5-hour window. Keep sessions lean: few multi-agent
workflows, and commit and push at each checkpoint so work survives a cut-off.

Builds are Windows only. On 2026-11-11 the owner dropped the CachyOS (Linux) build: "we won't be
building towards Linux in the future". The build workflow, what the tuned build contains, the test
results and the next steps are in @lichii-tuned/HANDOFF.md.
