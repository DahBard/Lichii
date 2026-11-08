#!/usr/bin/env python3
"""Writes a Linux shell launcher for each Windows .bat launcher of the tuned build.

The .bat launchers (dist/lichii-tuned, added by the patch) all set environment variables and
start lichii.exe next to them. Each becomes <name>.sh with the same comments and variables,
starting ./lichii from its own folder (also where a portable `user` folder is looked for).

    python3 -I make_linux_launchers.py <dir with .bat files> <output dir>
"""

import re
import sys
from pathlib import Path

SET = re.compile(r"set ([A-Za-z_][A-Za-z0-9_]*)=([^&]*)")
START = re.compile(r'^start "" "%~dp0lichii\.exe" %\*$')
SAFE_VALUE = re.compile(r"^[A-Za-z0-9_.,:/=+-]*$")

# Windows paths the comments mention, as on Linux.
PATHS = [
    (r"%APPDATA%\eden\log\eden_log.txt", "~/.local/share/eden/log/eden_log.txt"),
    (r"%APPDATA%\eden", "~/.local/share/eden"),
    ("lichii.exe", "lichii"),
    (".bat", ".sh"),
]


def convert(bat: Path) -> str:
    comments: list[str] = []
    variables: list[tuple[str, str]] = []
    started = False
    for raw in bat.read_text(encoding="utf-8", errors="strict").splitlines():
        line = raw.strip()
        if not line or line.lower() == "@echo off":
            continue
        if line.lower() == "rem" or line.lower().startswith("rem "):
            text = line[3:].strip()
            for windows, linux in PATHS:
                text = text.replace(windows, linux)
            comments.append(f"# {text}".rstrip())
            continue
        if line.lower().startswith("set "):
            pairs = SET.findall(line)
            if not pairs:
                raise ValueError(f"{bat.name}: can't read {line!r}")
            for name, value in pairs:
                value = value.strip()
                if not SAFE_VALUE.match(value):
                    raise ValueError(f"{bat.name}: unexpected value {value!r} for {name}")
                variables.append((name, value))
            continue
        if START.match(line):
            started = True
            continue
        raise ValueError(f"{bat.name}: unexpected line {line!r}")
    if not started:
        raise ValueError(f"{bat.name}: does not start lichii.exe")
    lines = ["#!/bin/sh", *comments]
    lines += [f"export {name}={value}" for name, value in variables]
    lines += ['cd "$(dirname "$0")" || exit 1', 'exec ./lichii "$@"', ""]
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    source, output = Path(sys.argv[1]), Path(sys.argv[2])
    output.mkdir(parents=True, exist_ok=True)
    bats = sorted(source.glob("*.bat"))
    if not bats:
        print(f"no .bat launchers in {source}", file=sys.stderr)
        return 1
    for bat in bats:
        script = output / f"{bat.stem}.sh"
        script.write_text(convert(bat), encoding="utf-8")
        script.chmod(0o755)
        print(script.name)
    return 0


if __name__ == "__main__":
    sys.exit(main())
