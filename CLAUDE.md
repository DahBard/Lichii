# CLAUDE.md

@AGENTS.md

## lichii-tuned

This fork's `tuned` branch builds a tuned Windows Lichii for the owner's laptop (Core Ultra 7 255H +
RX 9070 XT over OCuLink, testing Ocarina of Time). The workflow can also build a CachyOS (Linux)
package (round 16), but since round 17 the owner wants a Linux build only when they ask for one:
pushes build Windows only; dispatch the workflow with `only: cachyos` when asked. The Linux build
must run on any modern Intel CPU (x86-64-v3, no AVX-512). The build workflow, what the tuned build
contains, the test results and the next steps are in @lichii-tuned/HANDOFF.md.
