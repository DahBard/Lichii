# lichii-tuned

Windows builds of [Lichii](https://github.com/zortos293/Lichii) tuned for an Intel Core Ultra 7 255H
(Arrow Lake-H: 6 P-cores, 8 E-cores, 2 low-power E-cores) driving an AMD Radeon RX 9070 XT over
OCuLink, tested with The Legend of Zelda: Ocarina of Time.

- `lichii-tuned.patch` holds every source change on top of this branch's Lichii commit.
- `.github/workflows/lichii-tuned.yml` applies it and builds two Windows variants on every push to
  `tuned`: `pgo` (MinGW clang, Lichii's PGO profile, `-march=x86-64-v3 -mtune=arrowlake`) and
  `pgo-lto` (the same plus ThinLTO, experimental). Download them from the run's Artifacts.

The package's `README-tuned.txt` lists the changes and the A/B launchers that switch each one off.
