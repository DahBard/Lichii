# lichii-tuned

Windows and CachyOS (Linux) builds of [Lichii](https://github.com/zortos293/Lichii) tuned for an
Intel Core Ultra 7 255H (Arrow Lake-H: 6 P-cores, 8 E-cores, 2 low-power E-cores) driving an AMD
Radeon RX 9070 XT over OCuLink, tested with The Legend of Zelda: Ocarina of Time.

- `lichii-tuned.patch` holds every source change on top of this branch's Lichii commit
  (`ad477ef1f`): the tuning itself, newer upstream commits, ports from other forks and the open
  pull requests listed in the package's `README-tuned.txt`. Its history is branch `tuned4`
  (backed up in `tuned4.bundle`).
- `.github/workflows/lichii-tuned.yml` applies it and builds the Windows variants on every push
  to `tuned`, and the CachyOS ones when run by hand (Actions > lichii-tuned > Run workflow,
  `only: cachyos`, or `all` for both). Download them from the run's Artifacts; each variant
  waits only for the same variant of an earlier run.
  - Windows: `pgo` (MinGW clang, Lichii's PGO profile, `-march=x86-64-v3 -mtune=arrowlake`) and
    `pgo-lto` (the same plus ThinLTO), with AMD's FSR 4 libraries. `msvc` (the same source built
    like the official MSVC download, to tell compiler effects from source changes) is commented
    out until a glitch needs it.
  - CachyOS (manual runs only): `cachyos-pgo` and `cachyos-pgo-lto`, built in CachyOS's x86-64-v3 container
    against its own libraries with clang 23 and the same profile, for x86-64-v3 with generic
    tuning: any Intel Core from Haswell on and every Intel CPU from 12th gen on.
    `README-cachyos.md` (shipped in the package) covers installing and what differs on Linux;
    `make_linux_launchers.py` writes a `.sh` twin of every Windows launcher.
- `pgo/`: profiles recorded with a `pgo-train` build (`*.profraw`, or zips of them) are merged into
  the next builds' profile. The training variant is commented out in the workflow until needed.
- `HANDOFF.md`: how it's built, what's in it, the test results and the next steps.

The package's `README-tuned.txt` lists the changes and the A/B launchers that switch each one off.
