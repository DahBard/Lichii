#!/usr/bin/env python3
"""Summarize a Lichii eden_log.txt (best with perf-trace.bat: EDEN_NX2_GPU_PROFILE=1 and
EDEN_DKB_FRAME_TRACE=1): setup, changed settings, core placement, frame rate and hangs, CPU time
per thread, GPU time per shader, GPU thread zones, pipeline builds, memory pressure and errors."""
import collections
import re
import statistics
import sys

# [  12.345678] Render_Vulkan <Info> video_core/renderer_vulkan/vk_scheduler.cpp:684:ReportWorkTimes: msg
LINE = re.compile(r"^\[\s*(\d+\.\d+)\]\s+(\S+)\s+<(\w+)>\s+(\S+?):(\d+):(\S*?):\s(.*)$")
SETUP_KEYS = ("Lichii Version", "Booting game", "Hybrid CPU", "CPU:", "Driver", "Device",
              "Vulkan", "GPU:", "lichii-tuned:", "anti-aliasing", "PGO training", "Texture cache thresholds",
              "Memory margins", "VRAM", "device memory", "AMD FSR 4:", "Preflushing",
              "Pipeline cache", "Total Pipeline Count")
MEMORY_KEYS = ("out of device memory", "over the memory budget", "placed in host memory",
               "device lost", "device loss", "not reached in 4 s", "timed out", "out_of_")


def pct(values, q):
    values = sorted(values)
    return values[min(len(values) - 1, int(q * len(values)))]


def describe(values, unit=""):
    if not values:
        return "n/a"
    return (f"median {statistics.median(values):.1f}{unit}, p90 {pct(values, 0.9):.1f}{unit}, "
            f"max {max(values):.1f}{unit} ({len(values)} samples)")


def parse(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.match(raw.rstrip("\r\n"))
            if m:
                yield (float(m.group(1)), m.group(2), m.group(3), m.group(6), m.group(7))


def main(path):
    setup, settings, placements = [], [], set()
    fps_windows, role_ms = [], collections.defaultdict(list)
    gpu_span, gpu_sum, gpu_work = [], [], collections.defaultdict(lambda: [0.0, 0, 0.0])
    # Frames sampled without per-draw timestamps (round 8 on): their GPU time is not inflated.
    gpu_plain_span, gpu_plain_sum, gpu_upload, traffic = [], [], [], []
    gpu_work_samples, slow_submits = 0, collections.Counter()
    zones, sched = collections.defaultdict(list), []
    pipelines, deferred, presents = [], [], []
    memory, errors, warnings = collections.Counter(), collections.Counter(), collections.Counter()
    runtime, tc_stats, tc_uploads, vma, skipped = [], [], [], [], 0
    present_stats, present_frames = [], []
    finish_sites = collections.defaultdict(list)
    # Round 15: each frame's time (DKB frame_ms, from 1 s windows) and the guest core profile.
    frame_ms, guest_cores = [], collections.defaultdict(lambda: collections.defaultdict(list))
    # Round 16: the bank check and mixed macro read verify counts, and the host frame pacer.
    bank_checks, mixed_verify, host_pacing, displays = [], [], [], []
    for t, cls, level, func, msg in parse(path):
        m = re.match(r"Native bank check \(mode (\d+), 10 s\): (\d+) binds, .*; missed (\d+)", msg)
        if m:
            bank_checks.append((t, int(m.group(1)), int(m.group(2)), int(m.group(3))))
            continue
        m = re.match(r"Mixed MME DMA reads \(verify\): (\d+) checked, (\d+) differed", msg)
        if m:
            mixed_verify.append((t, int(m.group(1)), int(m.group(2))))
            continue
        m = re.match(r"Host pacing (on|off): (\d+) frames at ([\d.]+) Hz; shown for 1/2/3/4/5\+ "
                     r"refreshes: (\d+)/(\d+)/(\d+)/(\d+)/(\d+) \(same refresh (\d+)\); interval "
                     r"mean ([\d.]+) ms, max ([\d.]+);", msg)
        if m:
            host_pacing.append((t, m.group(1), int(m.group(2)), float(m.group(3)),
                                [int(m.group(i)) for i in range(4, 10)], float(m.group(10)),
                                float(m.group(11))))
            continue
        if msg.startswith("Host pacing: display refresh") or msg.startswith("Host pacing ("):
            displays.append(f"{t:9.2f}  {msg[:160]}")
            continue
        m = re.match(r"DKB frame_ms n=\d+((?: [\d.]+)*)$", msg)
        if m:
            frame_ms.append((t, [float(v) for v in m.group(1).split()]))
            continue
        m = re.match(r"Guest core (\d+) per second: guest code ([\d.]+) ms \(([\d.]+) entries, "
                     r"([\d.]+) slow memory accesses, [\d.]+ slow exclusive stores\), supervisor "
                     r"calls ([\d.]+) ms \(([\d.]+)\), kernel ([\d.]+) ms, idle spin ([\d.]+) ms, "
                     r"idle wait ([\d.]+) ms \| calls: (.*)", msg)
        if m:
            core = guest_cores[int(m.group(1))]
            for name, index in (("guest code", 2), ("slow accesses/s", 4), ("calls", 5),
                                ("calls/s", 6), ("kernel", 7), ("idle spin", 8), ("idle wait", 9)):
                core[name].append(float(m.group(index)))
            core["top"].append(m.group(10))
            continue
        if any(k in msg for k in SETUP_KEYS) and len(setup) < 60:
            setup.append(f"{t:9.2f}  {msg[:220]}")
        if cls == "Config" and re.match(r"^(M[-C]|-C) ", msg):
            settings.append(msg)
        if msg.startswith("Core placement:"):
            placements.add(msg)
        m = re.match(r"DKB cpu through_frame=\d+ frames=(\d+) window_wall_ms=([\d.]+)(.*)", msg)
        if m:
            frames, wall = int(m.group(1)), float(m.group(2))
            if wall > 0:
                fps_windows.append((t, frames * 1000.0 / wall, wall))
            for role, value in re.findall(r"(\w+)_cpu_ms_per_frame=([\d.]+)", m.group(3)):
                role_ms[role].append(float(value))
        m = re.match(r"DKB gpu_frame .*gpu_frame_span_ms=(-?[\d.]+) gpu_submission_sum_ms=([\d.]+)", msg)
        if m:
            u = re.search(r"gpu_upload_sum_ms=([\d.]+) draws_timed=(\d)", msg)
            plain = u is not None and u.group(2) == "0"
            if float(m.group(1)) >= 0:
                (gpu_plain_span if plain else gpu_span).append(float(m.group(1)))
            (gpu_plain_sum if plain else gpu_sum).append(float(m.group(2)))
            if u:
                gpu_upload.append(float(u.group(1)))
        m = re.match(r"DKB gpu_work .*slowest:(.*)", msg)
        if m:
            gpu_work_samples += 1
            for key, total, count, peak in re.findall(
                    r"([sm]\d:[0-9a-f]+(?:\+s\d:[0-9a-f]+)?)=([\d.]+)ms/(\d+)x\(max ([\d.]+)ms", m.group(1)):
                entry = gpu_work[key]
                entry[0] += float(total)
                entry[1] += int(count)
                entry[2] = max(entry[2], float(peak))
        m = re.match(r"DKB gpu_submission .*gpu_ms=([\d.]+) .*shaders=(\S*)", msg)
        if m:
            slow_submits[m.group(2)[:120] or "(none)"] += 1
        m = re.match(r"(.+?) per second:(.*)", msg)
        if m and cls in ("HW_GPU", "HW.GPU") and "methods" not in m.group(1) and "MME" not in m.group(1):
            for part in m.group(2).split(" | "):
                part = part.strip()
                z = re.match(r"(.+?) ([\d.]+) ms \(([\d.]+)/s\)$", part)
                if part.startswith("waits for the back by site:"):
                    for site in part.split(":", 1)[1].split(","):
                        w = re.match(r"\s*(.+?) ([\d.]+) ms \(", site)
                        if w:
                            zones[(m.group(1), "wait for back: " + w.group(1))].append(float(w.group(2)))
                elif z:
                    zones[(m.group(1), z.group(1))].append(float(z.group(2)))
                elif part.startswith("host_cpu_busy_ms_per_s="):
                    busy = part.split("=")[1].split()[0]
                    if busy != "unavailable":
                        zones[(m.group(1), "(CPU busy)")].append(float(busy))
        m = re.match(r"Scheduler per second: (\d+) submits, (\d+) render passes, (\d+) finishes "
                     r"waiting ([\d.]+) ms", msg)
        if m:
            sched.append(tuple(float(x) for x in m.groups()))
        m = re.match(r"Staging traffic per second: ([\d.]+) MiB uploaded through the stream buffer, "
                     r"([\d.]+) MiB through host memory, ([\d.]+) MiB downloaded", msg)
        if m:
            traffic.append(tuple(float(x) for x in m.groups()))
        m = re.match(r"(\w+) pipeline built on the GPU thread in ([\d.]+) ms", msg)
        if m:
            pipelines.append((t, m.group(1), float(m.group(2))))
        if "compute shader" in msg and ("Deferred" in msg or "rejected" in msg):
            deferred.append(f"{t:9.2f}  {msg[:200]}")
        m = re.match(r"Runtime pipelines in the last 10 s: (\d+) built, ([\d.]+) ms on the GPU thread "
                     r"\(([\d.]+) ms of it graphics SPIR-V\), ([\d.]+) ms graphics SPIR-V on workers, "
                     r"([\d.]+) ms of draws held", msg)
        if m:
            runtime.append((t, int(m.group(1)), float(m.group(2)), float(m.group(5))))
        m = re.match(r"Texture cache last 600 frames: created (\d+) images, deleted (\d+) \(collector "
                     r"(\d+), (\d+) costly, (\d+) downloaded(?: in (\d+) GPU waits)?\)", msg)
        if m:
            tc_stats.append((t,) + tuple(int(x or -1) for x in m.groups()))
        m = re.match(r"Texture cache last 600 frames: (\d+) uploads from guest memory, (\d+) MiB", msg)
        if m:
            tc_uploads.append((t, int(m.group(1)), int(m.group(2))))
        m = re.match(r"VMA heap (\d+): usage (\d+) MiB of budget (\d+) MiB", msg)
        if m:
            vma.append((t, int(m.group(1)), int(m.group(2)), int(m.group(3))))
        m = re.match(r"Async shaders: (\d+) pipeline lookups skipped", msg)
        if m:
            skipped = int(m.group(1))
        m = re.match(r"Finish sites: ([\d.]+)/s waiting ([\d.]+) ms/s at (\S+)", msg)
        if m:
            finish_sites[m.group(3)].append((float(m.group(1)), float(m.group(2))))
        m = re.match(r"nx2:vi2 last 600 presents: frame (\d+) us \(max (\d+), (\d+) over 100 ms\), "
                     r"blocked on the GPU fence (\d+) us \(max (\d+)\), composite (\d+) us, vblank "
                     r"pacing (\d+) us, (\d+) ran a frame ahead", msg)
        if m:
            present_stats.append((t,) + tuple(int(x) for x in m.groups()))
        m = re.match(r"DKB frame present=\d+ .*cpu_frame_wall_ms=(-?[\d.]+) cpu_present_wall_ms=([\d.]+) "
                     r"fence_wall_ms=([\d.]+) composite_wall_ms=([\d.]+)", msg)
        if m:
            present_frames.append(tuple(float(x) for x in m.groups()))
        m = re.match(r"nx2:vi2 229 present #(\d+)", msg)
        if m:
            presents.append((t, int(m.group(1))))
        lowered = msg.lower()
        for key in MEMORY_KEYS:
            if key in lowered:
                memory[key] += 1
        shape = re.sub(r"0x[0-9a-fA-F]+|[0-9a-fA-F]{8,}|\d+", "#", msg)[:160]
        if level in ("Error", "Critical"):
            errors[f"{cls}: {shape}"] += 1
        elif level == "Warning":
            warnings[f"{cls}: {shape}"] += 1

    print("== Setup")
    print("\n".join(setup) or "(none)")
    print("\n== Settings changed from defaults (M = modified, C = per-game)")
    print("\n".join(settings) or "(none logged)")
    print("\n== Core placement")
    print("\n".join(sorted(placements)) or "(none: policy off, non-hybrid CPU or old build)")

    print("\n== Frame rate (DKB, ~1 s windows)")
    if fps_windows:
        rates = [fps for _, fps, _ in fps_windows]
        print(f"  {describe(rates, ' fps')}")
        hangs = [(t, wall) for t, fps, wall in fps_windows if wall > 1500]
        print(f"  windows under 10 fps: {sum(r < 10 for r in rates)}, under 20: "
              f"{sum(r < 20 for r in rates)}, windows longer than 1.5 s (hangs): {len(hangs)}")
        for t, wall in hangs[:15]:
            print(f"    hang ending at {t:8.1f} s: {wall / 1000:.1f} s")
        line = ""
        for t, fps, _ in fps_windows:
            line += f"{t:6.0f}s {fps:4.0f} |"
            if len(line) > 90:
                print("  " + line)
                line = ""
        if line:
            print("  " + line)
    for (t0, n0), (t1, n1) in zip(presents, presents[1:]):
        if n1 > n0 and t1 > t0:
            print(f"  presents {n0}-{n1}: {(n1 - n0) / (t1 - t0):5.1f} per s ({t0:.0f}-{t1:.0f} s)")

    if frame_ms:
        # Gameplay: after the first frame over 1.5 s (the load into the save), else the longest.
        hangs = [i for i, (_, window) in enumerate(frame_ms) if max(window, default=0) > 1500]
        load = hangs[0] if hangs else max(range(len(frame_ms)),
                                          key=lambda i: max(frame_ms[i][1], default=0))
        for label, part in (("whole log", frame_ms), (f"from {frame_ms[load][0]:.0f} s (after the "
                                                      "first load)", frame_ms[load + 1:])):
            values = sorted(v for _, window in part for v in window)
            if not values:
                continue
            n = len(values)
            low = lambda f: 1000.0 * max(1, int(f * n)) / sum(values[-max(1, int(f * n)):])
            over = lambda ms: 100.0 * sum(v > ms for v in values) / n
            print(f"\n== Frame times, {label}: {n} frames, {1000.0 * n / sum(values):.1f} fps")
            print(f"  ms p50 {pct(values, 0.5):.1f}, p90 {pct(values, 0.9):.1f}, p99 {pct(values, 0.99):.1f}, "
                  f"max {values[-1]:.1f}; 1% low {low(0.01):.1f} fps, 0.1% low {low(0.001):.1f} fps; "
                  f"over 33.4 ms {over(33.4):.1f}%, over 50 ms {over(50):.1f}%, over 100 ms {over(100):.2f}%")
    if bank_checks or mixed_verify:
        print("\n== Round 16 checks (verify-r16.bat: missed and differed should be 0)")
        for t, mode, binds, missed in bank_checks:
            if mode == 2 or missed:
                print(f"  {t:8.1f} s: bank check mode {mode}, {binds} binds, missed {missed}")
        if bank_checks:
            print(f"  bank check: {len(bank_checks)} reports, missed in total "
                  f"{sum(b[3] for b in bank_checks)}")
        if mixed_verify:
            last = mixed_verify[-1]
            print(f"  mixed macro reads: {last[1]} checked, {last[2]} differed (last report, "
                  f"{last[0]:.1f} s)")
    if host_pacing or displays:
        print("\n== Host frame pacer (LICHII_HOST_PACE; refreshes each frame stayed on screen)")
        for line in displays[:6]:
            print(f"  {line}")
        for t, state, frames, hz, shown, mean, worst in host_pacing:
            total = sum(shown[:5]) or 1
            spread = " ".join(f"{100.0 * n / total:.0f}%" for n in shown[:5])
            print(f"  {t:8.1f} s: {state:3} {frames:4} frames at {hz:7.3f} Hz, 1/2/3/4/5+ {spread}, "
                  f"same refresh {shown[5]}, interval mean {mean:.2f} ms, max {worst:.1f}")
    if guest_cores:
        print("\n== Guest cores (ms per second, every 10 s)")
        for core, values in sorted(guest_cores.items()):
            parts = ", ".join(f"{name} {statistics.median(values[name]):.0f}"
                              for name in ("guest code", "calls", "kernel", "idle spin", "idle wait",
                                           "calls/s", "slow accesses/s"))
            print(f"  core {core}: {parts}")
            print(f"    last calls: {values['top'][-1][:200]}")

    print("\n== Host CPU ms per frame by thread (DKB)")
    for role, values in sorted(role_ms.items(), key=lambda kv: -statistics.median(kv[1])):
        print(f"  {role:24} {describe(values, ' ms')}")

    print("\n== GPU time per frame (DKB, one sampled frame per second)")
    if gpu_plain_span or gpu_plain_sum:
        print(f"  frame span: {describe(gpu_plain_span, ' ms')}")
        print(f"  submissions sum: {describe(gpu_plain_sum, ' ms')}")
        print(f"  frames with timed draws (slower by the timestamps): span {describe(gpu_span, ' ms')}")
        print(f"                                                      sum  {describe(gpu_sum, ' ms')}")
    else:
        print(f"  frame span: {describe(gpu_span, ' ms')}")
        print(f"  submissions sum: {describe(gpu_sum, ' ms')}")
    if gpu_upload:
        print(f"  of which upload command buffers: {describe(gpu_upload, ' ms')}")
    if gpu_work_samples:
        print(f"  slowest shaders, average GPU ms per sampled frame over {gpu_work_samples} samples:")
        for key, (total, count, peak) in sorted(gpu_work.items(), key=lambda kv: -kv[1][0])[:15]:
            print(f"    {key:42} {total / gpu_work_samples:7.2f} ms  ({count} runs, max {peak:.2f} ms)")
    if slow_submits:
        print("  submissions over 10 ms by shader set:")
        for shaders, n in slow_submits.most_common(8):
            print(f"    {n:4} x {shaders}")

    print("\n== GPU threads, ms per second of wall time (median over 10 s windows)")
    rows = sorted(((statistics.median(v), k, len(v)) for k, v in zones.items()), reverse=True)
    for med, (thread, zone), n in rows[:30]:
        print(f"  {thread[:14]:14} {zone[:40]:40} {med:7.1f}  (max {max(zones[(thread, zone)]):.0f})")
    if finish_sites:
        print("  full GPU waits (Finish) by call site, median over the 10 s reports that list them:")
        rows = sorted(((statistics.median(w for _, w in v), statistics.median(c for c, _ in v), k, len(v))
                       for k, v in finish_sites.items()), reverse=True)
        for wait, count, site, n in rows[:10]:
            print(f"    {site:44} {count:6.1f}/s waiting {wait:6.1f} ms/s  ({n} reports)")
    if sched:
        cols = list(zip(*sched))
        print(f"  scheduler: {statistics.median(cols[0]):.0f} submits/s, "
              f"{statistics.median(cols[1]):.0f} render passes/s, {statistics.median(cols[2]):.0f} "
              f"finishes/s waiting {statistics.median(cols[3]):.1f} ms/s (max {max(cols[3]):.0f})")
    if traffic:
        cols = list(zip(*traffic))
        print(f"  staging traffic, MiB/s (median over 10 s reports): {statistics.median(cols[0]):.1f} "
              f"uploaded through the stream buffer, {statistics.median(cols[1]):.1f} through host "
              f"memory, {statistics.median(cols[2]):.1f} downloaded (max {max(cols[0]) + max(cols[1]):.0f} up)")

    print("\n== Pipelines built on the GPU thread (the frame waits for these)")
    if pipelines:
        times = [ms for _, _, ms in pipelines]
        print(f"  {len(times)} builds, {sum(times) / 1000:.1f} s in total, max {max(times):.0f} ms, "
              f"over 100 ms: {sum(ms > 100 for ms in times)}")
        for t, kind, ms in sorted(pipelines, key=lambda p: -p[2])[:10]:
            print(f"    {t:9.2f} s  {kind:9} {ms:8.1f} ms")
    else:
        print("  (none)")
    print("\n".join(deferred[:20]))

    print("\n== Present thread, averages per 600 presents (game blocked on the GPU = fence)")
    for t, frame, worst, hitches, fence, worst_fence, composite, pace, ahead in present_stats:
        print(f"  {t:8.1f} s: frame {frame / 1000:5.1f} ms (max {worst / 1000:6.0f}, {hitches:3} over "
              f"100 ms), fence wait {fence / 1000:5.1f} ms (max {worst_fence / 1000:5.0f}), composite "
              f"{composite / 1000:4.1f} ms, pacing {pace / 1000:4.1f} ms, ran ahead {ahead}/600")
    if present_frames:
        cols = list(zip(*present_frames))
        print(f"  sampled frames: frame {describe([c for c in cols[0] if c >= 0], ' ms')}")
        print(f"                  fence wait {describe(list(cols[2]), ' ms')}")
    if not present_stats and not present_frames:
        print("  (none)")

    print("\n== Pipelines built at run time (10 s reports)")
    if runtime:
        print(f"  {sum(r[1] for r in runtime)} built; {sum(r[2] for r in runtime) / 1000:.1f} s on the GPU "
              f"thread; draws held {sum(r[3] for r in runtime) / 1000:.1f} s waiting for unbuilt pipelines")
        for t, built, gpu_ms, held_ms in runtime:
            if held_ms > 100 or gpu_ms > 100:
                print(f"    {t:8.1f} s: {built:4} built, {gpu_ms:7.0f} ms GPU thread, {held_ms:7.0f} ms held")
    else:
        print("  (none)")
    if skipped:
        print(f"  async shaders skipped {skipped} lookups of unbuilt pipelines")

    print("\n== Texture cache and VRAM (every 600 frames)")
    for t, created, deleted, collector, costly, downloaded, waits in tc_stats:
        print(f"  {t:8.1f} s: created {created}, deleted {deleted} (collector {collector}, "
              f"{downloaded} downloaded" + (f" in {waits} GPU waits)" if waits >= 0 else ")"))
    for t, count, mib in tc_uploads:
        print(f"  {t:8.1f} s: {count} uploads from guest memory, {mib} MiB")
    for t, heap, usage, budget in vma:
        flag = "  <-- over budget" if usage > budget else ""
        print(f"  {t:8.1f} s: VMA heap {heap}: {usage} of {budget} MiB{flag}")

    if fps_windows:
        print("\n== Timeline, 10 s buckets: fps, slowest ~1 s window, pipeline builds")
        buckets = collections.defaultdict(lambda: [0, 0.0, 0.0, 0, 0.0])
        for t, fps, wall in fps_windows:
            b = buckets[int(t // 10) * 10]
            b[0] += round(fps * wall / 1000.0)
            b[1] += wall
            b[2] = max(b[2], wall)
        for t, built, gpu_ms, held_ms in runtime:
            b = buckets[int(t // 10) * 10]
            b[3] += built
            b[4] += held_ms + gpu_ms
        for start in sorted(buckets):
            frames, wall, worst, built, ms = buckets[start]
            fps = frames * 1000.0 / wall if wall else 0.0
            print(f"  {start:6d}-{start + 10:<6d} {fps:5.1f} fps  slowest window {worst / 1000:4.1f} s  "
                  f"pipelines {built:4} ({ms:6.0f} ms built or held)")

    print("\n== Memory and timeouts")
    for key, n in memory.most_common():
        print(f"  {key}: {n}")
    print("\n== Most frequent errors")
    for msg, n in errors.most_common(15):
        print(f"  {n:6} x {msg}")
    print("\n== Most frequent warnings")
    for msg, n in warnings.most_common(15):
        print(f"  {n:6} x {msg}")


if __name__ == "__main__":
    main(sys.argv[1])
