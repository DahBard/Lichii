# lichii-tuned

Windows builds of [Lichii](https://github.com/zortos293/Lichii), tuned for an Intel Core Ultra 7 255H (Arrow Lake-H:
6 P-cores, 8 E-cores, 2 low-power E-cores) driving an AMD Radeon RX 9070 XT over OCuLink, tested
with The Legend of Zelda: Ocarina of Time.

- Since round 22 this branch's tree is upstream Lichii nx2 (`17baed716`: build 132 plus 31
  commits), and
  `lichii-tuned.patch` holds our changes on top of it: branch `tuned4` (backed up in
  `tuned4.bundle`). Round 21's trial on Fritchii v5.5 is branch `tuned5` (`tuned5.bundle`), kept
  for reference.
- `.github/workflows/lichii-tuned.yml` applies it and builds the variants on every push to
  `tuned`, or when run by hand (Actions > lichii-tuned > Run workflow). Download them from the
  run's Artifacts; each variant waits only for the same variant of an earlier run.
  - Windows: `pgo` (MinGW clang, Lichii's PGO profile, `-march=x86-64-v3 -mtune=arrowlake`),
    `pgo-lto` (the same plus ThinLTO) and `pgo-train`, with AMD's FSR 4 and frame generation
    libraries.
- `.github/workflows/lichii-tuned-tests.yml` builds upstream's unit tests (`tests`) from the same
  source with the same MinGW clang and runs them, on every push of the patch. It uploads nothing.
- PGO: profiles the owner records with a `pgo-train` build (`lichii-pgo-*.profraw`, zipped) are
  attached to pre-releases whose tag starts with `pgo-` (the first is `pgo-r49`). Each build except
  `pgo-train` downloads them (`fetch_pgo_profiles.py`) and merges them into Lichii's profile,
  weighted 4:1 toward the recorded game. Files in `pgo/` here are merged the same way.
- `HANDOFF.md`: how it's built, what's in it, the test results and the next steps.

The package's `README-tuned.txt` lists the changes and the A/B launchers that switch each one off.
