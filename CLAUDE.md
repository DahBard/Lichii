# CLAUDE.md

@AGENTS.md

## lichii-tuned

This repository's (DahBard/Blitchii, private since 2026-11-13) `tuned` branch builds a tuned Windows
Lichii for the owner's laptop (Core Ultra 7 255H + RX 9070 XT over OCuLink, testing Ocarina of Time). The base is our own fork, branch `tuned4`, with
upstream Lichii merged in. Round 21 (2026-11-09) briefly restarted on Fritchii v5.5 (branch `tuned5`,
now kept for reference only). On 2026-11-10 the owner chose to stay on the tuned fork after testing
r35, Fritchii v5.5 and upstream build 132 (see HANDOFF): Fritchii ran ~10% slower in steady play, and
build 132 has no graphics shader preparation.

Priorities (owner, 2026-11-10):
- Upstream Lichii (nx2 and its pull requests) comes first. Where our code and
  upstream's do the same thing, upstream's wins.
- Upstream's shader translator changes are merged by default (owner, 2026-11-12). They don't raise
  PreparationVersion: new builds have started with a cold AMD driver cache anyway, and the owner
  accepts slow shader loading once per build (2026-11-10).
- Fritchii (Frizti/Lichii, branch `oot-gather-fix`, tags `fritchii-v*`): only features with a large
  benefit for this setup or for Ocarina. From v6.0 (its own repository, 2026-11-13), the owner's
  rule below replaces that bar.
- CheezerWheezer/CheezyEmu: its FSR, FSR frame generation and Radeon optimizations, if they meet our
  standard.
- Take changes from other forks only for a very good reason or when the owner names them. Vet
  everything that doesn't come from upstream Lichii heavily.
- 2026-11-14 (owner): "do whatever guarantees the most performance. if the shortcut works, and I
  haven't reported any issue with it, then keep it in without the upstream change." Where an
  upstream change replaces one of our working shortcuts and costs performance, ours stays and that
  upstream change is left out (first: #1486 in merge 4, reverted on branch `r22-up-no1486`).

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
2. Merge with upstream Lichii right after that stable build. Don't wait for upstream's
   multithreaded GPU back thread; option 2 (the same thing) is dropped, and upstream's version comes
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

Then (owner, 2026-11-12 evening, after testing r58: "the best looking and smoothest" build so far,
"lets keep improving it"):
- AMD FSR frame generation doesn't run while HDR output is active (r58 logs with HDR Auto and
  Off): "Find out why and apply a patch."
- Plan and execute a merge with upstream Lichii up to build 169, and to 170 if it is released
  before the merge is finished. Build N is run #N of upstream's release-binaries workflow
  ("169 is in the release binaries"); only some runs get a `build-N` tag.
- The optional tests (verify-ports-off, the texture-cache session) are put off. A pgo-train
  profile comes after upstream's multithreading update, not before.
- 2026-11-13: "we are currently up to build 171" (release-binaries run #171 on nx2 =
  `c169856090`): merge 3 goes to build 171, in two steps (`65745c243f`, then build 171 on top).
  It includes upstream's rename of the yuzu names, targets and directories to Lichii (src/yuzu ->
  src/lichii, #1541/#1550-#1553) and the removal of the yuzu SPDX lines (#1544): "The rename is a
  precaution against future DMCA takedown requests. We should do it to be safe." Our own
  additions (code, launchers, CI workflow, scripts, docs) follow the same rename; keep only the
  yuzu names upstream keeps. Done on 2026-11-13: branch `r22-merge-nx2c` (tuned4 `04e8a8d2a`), the base under
  the patch is now `c169856090`; r60.
- 2026-11-13: don't port upstream #1314 (the Castle Town guard house / pot room frame rate):
  "Fritchii has a fix that will be in its next release later today." Review that release's fix
  when it is out.
- 2026-11-13, the order after merge 3: (1) a code cleanup and a file system cleanup: most of the
  A/B and other testing launchers (.bat) go, "unless they might be useful later" (done in r61: 13
  launchers kept; the code cleanup waits for the owner's test of r60/r61); (2) then wait
  for Fritchii's next release and upstream Lichii's multithreading release before adding
  anything else.
- 2026-11-13, Fritchii v6 (first set for 11:00 EST): "check for a new Fritchii Github
  repository separate from the Current Lichii Fork called Fritchii. Find the Version 6.0, or if
  there is a later version, try that one. Do a code review for Fritchii and find out what we could
  benefit from and add it to our fork. Fritchii is highly specialized to run OOT, so bias our
  decisions based on that. If something in fritchii v6 is already handled in our build
  differently, default to changing to fritchii's unless our way of doing it is specifically tuned
  to our Hardware specifications." Its pot room fix is the one wanted. Later that day the owner
  dropped the 11:00 check: v6 will be released soon, but where it will be hosted isn't known yet;
  the owner says where once it is public.
- 2026-11-13: "When you are finished with your current goals merge upstream Lichii with our 'not a
  fork'": merge 4, upstream nx2 `87d84d513c` (17 commits after build 171) without #1486, on branch
  `r22-merge-nx2e` (`r22-merge-nx2d` was the first attempt, with #1486). Done on 2026-11-14:
  tuned4 `a8788276c`, `tuned` `ada9ce77c`; the base under the patch is now `85a2cc0ac7`
  (`r22-up-no1486`); r62.
- A scheduled task ("Resume Lichii work") resumes this session after a usage cut-off until this
  work is done, as the owner asked on 2026-11-12 ("When my usage runs out, resume when it renews
  ... etc."). Since 2026-11-13 it runs once at each usage renewal, the first at 6:31 EST (the
  owner's time, instead of every 2 hours), and each run re-arms it for the next renewal 5 hours
  later. It doesn't interrupt work that is running.

- 2026-11-13: the repository moved to DahBard/Blitchii, private, with its full history; our own
  files keep no reference to the origin repository (upstream's own code and the git history stay as
  they are). Its CI builds pgo-lto on push (Actions minutes and storage are limited), keeps artifacts
  3 days and numbers builds run + 60. The owner deletes the old public fork once a build here passes.

Usage: the owner's plan has limited usage per 5-hour window. Keep sessions lean: few multi-agent
workflows, and commit and push at each checkpoint so work survives a cut-off.

Builds are Windows only. On 2026-11-11 the owner dropped the CachyOS (Linux) build: "we won't be
building towards Linux in the future". The build workflow, what the tuned build contains, the test
results and the next steps are in @lichii-tuned/HANDOFF.md.
