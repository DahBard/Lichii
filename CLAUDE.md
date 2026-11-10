# CLAUDE.md

@AGENTS.md

## lichii-tuned

This fork's `tuned` branch builds a tuned Windows Lichii for the owner's laptop (Core Ultra 7 255H +
RX 9070 XT over OCuLink, testing Ocarina of Time). Since round 21 (2026-11-09, the owner's decision
after Fritchii v5.5 felt smoother on the laptop; its "60 fps" was the title-screen video, and in
gameplay it ran ~32 fps against our r28's 34-42, see HANDOFF) the base is Fritchii v5.5: `tuned`'s tree
is Fritchii's, and our changes live on branch `tuned5`, brought over from the old fork (branch
`tuned4`) in small groups, each A/B-able against plain Fritchii. The workflow can also build a CachyOS (Linux)
package (round 16), but since round 17 the owner wants a Linux build only when they ask for one:
pushes build Windows only; dispatch the workflow with `only: cachyos` when asked. The Linux build
must run on any modern Intel CPU (x86-64-v3, no AVX-512). Since round 20 the owner's sources are
upstream Lichii (zortos293/Lichii: nx2 and its pull requests) and Fritchii (Frizti/Lichii, branch
`oot-gather-fix`, tags `fritchii-v*`): take changes from other forks only for a very good reason or
when the owner names them. The build workflow, what the tuned build contains, the test results and
the next steps are in @lichii-tuned/HANDOFF.md.
