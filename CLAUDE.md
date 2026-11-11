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

Usage: the owner's plan has limited usage per 5-hour window. Keep sessions lean: few multi-agent
workflows, and commit and push at each checkpoint so work survives a cut-off.

Builds are Windows only. On 2026-11-11 the owner dropped the CachyOS (Linux) build: "we won't be
building towards Linux in the future". The build workflow, what the tuned build contains, the test
results and the next steps are in @lichii-tuned/HANDOFF.md.
