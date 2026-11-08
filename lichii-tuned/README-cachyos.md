# Lichii tuned for CachyOS

The Linux build of the tuned Lichii (github.com/DahBard/Lichii, branch `tuned`): the same
source, patch and switches as the Windows build of the same run, compiled in CachyOS's own
x86-64-v3 environment against its Qt 6, FFmpeg, SDL3 and Mesa, by clang 23 with Lichii's PGO
profile, for x86-64-v3 with generic tuning (`-march=x86-64-v3 -mtune=generic`). Boost and fmt are
built in, so CachyOS updating those doesn't break it. `README-tuned.txt` describes the tuned
build and its launchers; this file covers what differs on Linux.

## Which CPUs

x86-64-v3 means AVX2, BMI2, FMA, F16C and MOVBE, which every modern Intel CPU has:

- every Intel Core from 4th gen (Haswell, 2013) on: Core i3/i5/i7/i9, Core 3/5/7/9 and Core
  Ultra, desktop and laptop;
- every Intel CPU from 12th gen (Alder Lake, 2021) on, the low end included: Pentium, Celeron,
  "Intel Processor", N100, N150, N200, N305;
- AMD Ryzen and every other Zen CPU.

It does not run on older Pentium and Celeron models (before 12th gen, Intel turned AVX off on
them) or on Atom-class Celerons such as Jasper Lake and Gemini Lake (N4500, N5100, N6000), which
lack AVX2. To check a machine: `/lib64/ld-linux-x86-64.so.2 --help | grep x86-64-v3` should
say `supported`. Nothing needs AVX-512 (Intel's consumer CPUs since 12th gen don't have it).

## Install

1. Install what it needs (the list is taken from the build itself), plus the Vulkan driver for
   the GPU: `vulkan-radeon` (RADV) for AMD, as for the RX 9070 XT, `vulkan-intel` for Intel or
   `nvidia-utils` for NVIDIA. CachyOS's installer normally has it in already.

   ```sh
   sudo pacman -S --needed $(cat cachyos-packages.txt) vulkan-radeon
   ```

2. Unpack the archive inside the download (GitHub's zip drops the executable bits; the tar keeps
   them), anywhere in your home folder:

   ```sh
   tar -xzf lichii-tuned-*-cachyos-*.tar.gz
   cd lichii-tuned-*-cachyos-*/
   ./lichii
   ```

3. In Emulation > Configure > Graphics, pick the discrete GPU as the device, not the CPU's
   integrated one: on the OCuLink laptop "AMD Radeon RX 9070 XT (RADV ...)".

## Your data

Linux keeps keys, firmware, saves, games list and the shader cache in `~/.local/share/eden`, the
settings in `~/.config/eden` and the log in `~/.local/share/eden/log/eden_log.txt`.

To use your Windows data as it is, copy the whole Windows `%APPDATA%\eden` folder into a folder
named `user` next to `lichii`: a `user` folder in the folder Lichii starts from is used instead of
the ones above (portable mode), and the launchers always start from their own folder. If you
start `lichii` by hand, start it from that folder too. The log is then `user/log/eden_log.txt`.

Lichii's own pipeline cache (`shader/`) carries over: at boot the log says how many of its
pipelines came from another driver and how many it had to leave out for this one. The driver's
own cache starts empty, so the first boot takes longer.

## Launchers

Each Windows `.bat` launcher has a `.sh` twin with the same name and variables:
`./perf-trace.sh`, `./frame-times.sh`, `./trace-fsr31.sh`, ... Send the log
(`~/.local/share/eden/log/eden_log.txt`, or `user/log/eden_log.txt` in portable mode) as on
Windows.

## Differences from Windows

- **No FSR 4.** AMD's FSR 4 libraries are Windows-only, so Ocarina's upscaling uses FSR 3.1
  (`trace-fsr31.sh` therefore matches the default here).
- **RADV instead of AMD's driver.** Mesa's driver behaves differently: the AMD-driver-specific
  workarounds (512 draws per submission, for one) don't apply, and the frame rate can be
  higher or lower than on Windows. That comparison is one of the things this build is for.
- **No hybrid core placement.** `LICHII_CPU_POLICY` (and the ab-2, ab-3, ab-4 launchers) only
  does something on Windows; the CachyOS kernel's scheduler places the threads on P- and
  E-cores itself. UMWAIT waits (`LICHII_WAITPKG_SPIN`) work on Linux too.

## Troubleshooting

- **It doesn't start after a system update** (a missing `.so`): CachyOS updated a library the
  build links to (Qt, FFmpeg, SDL3, ...). Run the newest build, or say which library is missing.
- **Stutter with "split lock" lines in `sudo dmesg`**: the kernel slows down programs that use
  split-lock atomics. `sudo sysctl kernel.split_lock_mitigate=0` turns that off until the next
  boot.
- **Window problems on Wayland**: `QT_QPA_PLATFORM=xcb ./lichii` runs it through XWayland
  instead.
