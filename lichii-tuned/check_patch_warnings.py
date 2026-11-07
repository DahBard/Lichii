"""Compile the source files tuned4 changes and report warnings on the lines it adds.

The msvc variant treats shadowing (C4456-C4459), signed/unsigned comparisons, unused locals and
lambda captures, unhandled enumerators and, in video_core, narrowing conversions as errors. GCC
and Clang report the same cases, but common/x64/xbyak.h and lower_to_maxwell.h switch -Wshadow
and -Wconversion off for the rest of every file that includes them (MSVC ignores those pragmas),
so the files are compiled against copies of those headers without the pragmas. Qt, Windows-only
and test files are skipped: review their added lines by hand.

Needs clang++, g++ and the Boost headers (Ubuntu: clang g++ libboost-dev). The header-only
dependencies are cloned at their cpmfile.json versions into --deps.

usage: python3 -I lichii-tuned/check_patch_warnings.py <tuned4 checkout> [--base 92f021240]
       [--deps DIR] [--jobs N]
"""
import argparse
import concurrent.futures as cf
import json
import pathlib
import re
import subprocess
import sys
import tempfile

# cpmfile.json package -> include directory inside its checkout
DEPS = {
    "fmt": "include",
    "vulkan-headers": "include",
    "vulkan-memory-allocator": "include",
    "vulkan-utility-libraries": "include",
    "xbyak": ".",
    "nlohmann": "include",
    "sirit": "include",
    "spirv-headers": "include",
    "simpleini": ".",
}
SKIP = re.compile(r"^src/(yuzu|qt_common|tests|common/windows|android)/")
COMMON = ["-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Wno-unused-parameter",
          "-Wno-missing-field-initializers", "-Wno-invalid-offsetof", "-Wno-attributes",
          "-Wsign-compare", "-Wnon-virtual-dtor", "-DARCHITECTURE_x86_64=1", "-DHAS_OPENGL=1",
          "-DFMT_HEADER_ONLY", "-D_FILE_OFFSET_BITS=64", "-mcx16"]
COMPILERS = {
    "clang": ["clang++", *COMMON, "-Wshadow-all", "-Wunused-lambda-capture",
              "-Wno-braced-scalar-init", "-Wno-unused-private-field",
              "-Wno-nullability-completeness"],
    "gcc": ["g++", *COMMON, "-Wshadow", "-Wno-array-bounds", "-Wno-stringop-overread",
            "-Wno-stringop-overflow"],
}
PRAGMA_HEADERS = ["common/x64/xbyak.h", "shader_recompiler/frontend/ampere/lower_to_maxwell.h"]
DIAG = re.compile(r"^(src/[^:]+):(\d+):\d+: (warning|error|fatal error): (.*)$")


def clone_deps(source: pathlib.Path, deps: pathlib.Path) -> list[str]:
    cpm = json.loads((source / "cpmfile.json").read_text())
    includes = []
    for name, subdir in DEPS.items():
        entry = cpm[name]
        checkout = deps / name / entry["version"]
        if not checkout.exists():
            subprocess.run(["git", "-c", "advice.detachedHead=false", "clone", "-q", "--depth",
                            "1", "--branch", entry["version"],
                            f"https://github.com/{entry['repo']}", str(checkout)], check=True)
        includes += ["-I", str(checkout / subdir)]
    return includes


def added_lines(source: pathlib.Path, base: str) -> dict[str, set[int]]:
    diff = subprocess.run(["git", "-C", str(source), "diff", "-U0", base, "--", "src"],
                          capture_output=True, text=True, check=True).stdout
    added: dict[str, set[int]] = {}
    current = None
    for line in diff.splitlines():
        if line.startswith("+++ "):
            current = line[6:] if line.startswith("+++ b/") else None
            if current:
                added.setdefault(current, set())
        elif line.startswith("@@") and current:
            m = re.search(r"\+(\d+)(?:,(\d+))?", line)
            start, count = int(m.group(1)), int(m.group(2) or 1)
            added[current].update(range(start, start + count))
    return added


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--base", default="92f021240")
    parser.add_argument("--deps", type=pathlib.Path,
                        default=pathlib.Path.home() / ".cache" / "lichii-tuned-deps")
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    source = args.source.resolve()

    includes = clone_deps(source, args.deps)
    added = added_lines(source, args.base)
    units = sorted(f for f in added if f.endswith(".cpp") and not SKIP.search(f))
    # A changed header is compiled through two of the files that include it.
    for header in sorted(f for f in added if f.endswith((".h", ".inc")) and not SKIP.search(f)):
        users = subprocess.run(["git", "-C", str(source), "grep", "-l", "-F",
                                f'#include "{header.removeprefix("src/")}"', "--", "src/*.cpp"],
                               capture_output=True, text=True).stdout.split()
        # Skip users of the SPIR-V headers the build generates (absent in a plain checkout).
        generated = set(subprocess.run(["git", "-C", str(source), "grep", "-l", "-F",
                                        '#include "video_core/host_shaders/', "--", *users],
                                       capture_output=True, text=True).stdout.split()) if users else set()
        units += [u for u in sorted(users)
                  if u not in units and u not in generated and not SKIP.search(u)][:2]

    with tempfile.TemporaryDirectory() as overlay:
        for header in PRAGMA_HEADERS:
            text = (source / "src" / header).read_text()
            target = pathlib.Path(overlay) / header
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text("".join(
                l for l in text.splitlines(keepends=True)
                if not re.search(r'diagnostic ignored "-W(shadow|conversion)"', l)))
        paths = ["-I", overlay, "-I", "src", "-I", "src/video_core", "-I", "src/dynarmic/src",
                 "-I", "src/dynarmic/src/dynarmic", "-I", "externals", "-I",
                 "externals/glad/include", "-I", "externals/tz", *includes]

        def compile_unit(unit: str, flags: list[str]):
            extra = ["-Wconversion", "-Wno-sign-conversion"] if unit.startswith(
                "src/video_core/") else []
            proc = subprocess.run([*flags, *paths, *extra, unit], cwd=source,
                                  capture_output=True, text=True)
            return unit, proc.returncode, proc.stderr

        hits, failed = set(), set()
        with cf.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            jobs = {pool.submit(compile_unit, u, f): n for u in units for n, f in COMPILERS.items()}
            for job in cf.as_completed(jobs):
                name = jobs[job]
                unit, code, err = job.result()
                if code != 0:
                    first = next((l for l in err.splitlines() if "error" in l), "")
                    failed.add(f"[{name}] {unit}: {first}")
                for l in err.splitlines():
                    m = DIAG.match(l)
                    if m and int(m.group(2)) in added.get(m.group(1), ()):
                        hits.add(f"[{name}] {m.group(1)}:{m.group(2)}: {m.group(3)}: {m.group(4)}")

    print(f"{len(units)} files, {len(COMPILERS)} compilers")
    print("compile failures:", *(sorted(failed) or ["none"]), sep="\n  ")
    print("diagnostics on added lines:", *(sorted(hits) or ["none"]), sep="\n  ")
    return 1 if failed or hits else 0


if __name__ == "__main__":
    sys.exit(main())
