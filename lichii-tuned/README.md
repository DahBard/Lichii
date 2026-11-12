# lichii-tuned

Windows builds of [Lichii](https://github.com/zortos293/Lichii), tuned for an Intel Core Ultra 7 255H (Arrow Lake-H:
6 P-cores, 8 E-cores, 2 low-power E-cores) driving an AMD Radeon RX 9070 XT over OCuLink, tested
with The Legend of Zelda: Ocarina of Time.

- Since 2026-11-12 this branch's tree is upstream Lichii nx2 `2eb5f081c` (`17baed716`, build 132
  plus 31 commits, from round 22), and
  `lichii-tuned.patch` holds our changes on top of it: branch `tuned4` (backed up in
  `tuned4.bundle`). Round 21's trial on Fritchii v5.5 is branch `tuned5` (`tuned5.bundle`), kept
  for reference.
- `.github/workflows/lichii-tuned.yml` applies it and builds the variants on every push to
  `tuned`, or when run by hand (Actions > lichii-tuned > Run workflow). Download them from the
  run's Artifacts; each variant waits only for the same variant of an earlier run.
  - Windows: `pgo` (MinGW clang, Lichii's PGO profile, `-march=x86-64-v3 -mtune=arrowlake`),
    `pgo-lto` (the same plus ThinLTO) and `pgo-train`, with AMD's FSR 4 and frame generation
    libraries.
  - `lichii-tuned-r<run>-<variant>-symbols`: that variant's lichii.exe before it was stripped, for
    `symbolize_crash.py` (names the functions in a `lichii-crash-*.txt`).
- `.github/workflows/lichii-tuned-tests.yml` builds upstream's unit tests (`tests`) from the same
  source with the same MinGW clang and runs them, on every push of the patch, then runs the clock
  trace's test with `EDEN_NX2_CLOCK_TRACE=1` on the runner. It uploads nothing.
- PGO: profiles the owner records with a `pgo-train` build (`lichii-pgo-*.profraw`, zipped) are
  attached to pre-releases whose tag starts with `pgo-` (the first is `pgo-r49`). Each build except
  `pgo-train` downloads them (`fetch_pgo_profiles.py`), keeps the last dump of each session
  (`profraw_last_dump.py`: the recording appends all its counts so far every 30 s) and merges
  that into Lichii's profile, weighted 70:1 toward the recorded game. By counts that keeps `pgo-r49`
  at 97% of the merged profile, as when r50-r54 merged all 34 of its dumps 4:1 (a file that can't
  be cut is still merged whole, 4:1). Files in `pgo/` here are merged the same way.
- `HANDOFF.md`: how it's built, what's in it, the test results and the next steps.

The package's `README-tuned.txt` lists the changes and the A/B launchers that switch each one off.
