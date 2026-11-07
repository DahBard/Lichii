# lichii-tuned

Windows builds of [Lichii](https://github.com/zortos293/Lichii) tuned for an Intel Core Ultra 7 255H
(Arrow Lake-H: 6 P-cores, 8 E-cores, 2 low-power E-cores) driving an AMD Radeon RX 9070 XT over
OCuLink, tested with The Legend of Zelda: Ocarina of Time.

- `lichii-tuned.patch` holds every source change on top of this branch's Lichii commit
  (588bfa6): the tuning itself, newer upstream commits and the open pull requests listed in the
  package's `README-tuned.txt`.
- `.github/workflows/lichii-tuned.yml` applies it and builds the Windows variants on every push to
  `tuned`: `pgo` (MinGW clang, Lichii's PGO profile, `-march=x86-64-v3 -mtune=arrowlake`) and
  `pgo-lto` (the same plus ThinLTO). Download them from the run's Artifacts. Each variant waits
  only for the same variant of an earlier run. `msvc` (the same source built like the official
  MSVC download, to tell compiler effects from source changes) is commented out until a glitch
  needs it.
- `pgo/`: profiles recorded with a `pgo-train` build (`*.profraw`, or zips of them) are merged into
  the next builds' profile. The training variant is commented out in the workflow until needed.

The package's `README-tuned.txt` lists the changes and the A/B launchers that switch each one off.
