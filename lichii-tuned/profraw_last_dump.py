"""Cut a profile recorded by a pgo-train build down to the last dump of each session.

usage: python3 -I lichii-tuned/profraw_last_dump.py [--llvm-profdata PATH] IN OUT

A pgo-train build writes its profile every 30 s (src/common/pgo_training.cpp). Its file name
(lichii-pgo-%p.profraw) does not merge, so compiler-rt appends a whole raw profile of the counts
so far on every write. llvm-profdata merge reads every profile in a file and adds them up, which
counts the start of the session once per dump; the session's last dump holds all of it.
compiler-rt truncates a profile file only at startup, before this one is named, so a later session
with the same process id appends its dumps to the file.

This finds the raw headers in IN (the 64-bit raw magic at an 8-byte aligned offset, followed by
the first header's version). In one process every dump is at least as long as the one before (only
its value profile data grows), so a shorter dump starts another session. The last dump of each
session, up to the next header or the end of IN, is written to OUT; a file with one dump is copied
unchanged. With --llvm-profdata, `PATH show` must read a dump: when it does not read a session's
last dump (the process died while writing it), the one before is tried, and so on; a shorter dump
it does not read was cut short and starts no session. Exit status 0 when OUT was written, 1 (and
no OUT) when no dump can be used.
"""

import argparse
import mmap
import os
import shutil
import struct
import subprocess
import sys

# INSTR_PROF_RAW_MAGIC_64 as an x86-64 program writes it.
MAGIC = struct.pack('<Q', 0xff6c70726f667281)
# Raw header fields up to NamesSize by raw format version (LLVM's InstrProfData.inc: 9 is
# LLVM 18, 10 is LLVM 19-22, 11 is LLVM 23), to print the dump's counts.
_FIRST_FIELDS = ('Magic', 'Version', 'BinaryIdsSize', 'NumData', 'PaddingBytesBeforeCounters',
                 'NumCounters', 'PaddingBytesAfterCounters', 'NumBitmapBytes',
                 'PaddingBytesAfterBitmapBytes')
HEADER_FIELDS = {
    9: _FIRST_FIELDS + ('NamesSize',),
    10: _FIRST_FIELDS + ('NamesSize',),
    11: _FIRST_FIELDS + ('NumUniformCounters', 'PaddingBytesAfterUniformCounters',
                         'UniformCountersDelta', 'NamesSize'),
}
COPY_CHUNK = 16 << 20


class Unreadable(Exception):
    """llvm-profdata reads none of the dumps."""


def log(message):
    print(f'profraw_last_dump: {message}', flush=True)


def fail(message):
    print(f'profraw_last_dump: {message}', file=sys.stderr, flush=True)
    return 1


def find_dumps(data):
    """Start offsets of the raw profiles in data, or [] if it does not start with one."""
    if len(data) < 16 or data[:8] != MAGIC:
        return []
    version = data[8:16]
    starts = [0]
    position = data.find(MAGIC, 8)
    while position != -1:
        # compiler-rt pads every section, so each dump is a multiple of 8 bytes long.
        if position % 8 == 0 and data[position + 8:position + 16] == version:
            starts.append(position)
        position = data.find(MAGIC, position + 1)
    return starts


def sessions(starts, ends, readable):
    """The dumps' indices by session."""
    groups = [[0]]
    longest = ends[0] - starts[0]
    for index in range(1, len(starts)):
        size = ends[index] - starts[index]
        if size >= longest:
            longest = size
        elif readable(index):
            groups.append([])
            longest = size
        groups[-1].append(index)
    return groups


def header_counts(data, start, end, version):
    fields = HEADER_FIELDS.get(version)
    if fields is None or end - start < 8 * len(fields):
        return 'header not decoded'
    values = dict(zip(fields, struct.unpack_from(f'<{len(fields)}Q', data, start)))
    return (f"{values['NumData']} functions, {values['NumCounters']} counters, "
            f"{values['NamesSize']} bytes of names")


def copy_ranges(data, ranges, path):
    with open(path, 'wb') as out:
        for start, end in ranges:
            for position in range(start, end, COPY_CHUNK):
                out.write(data[position:min(position + COPY_CHUNK, end)])


def show(tool, path):
    """(True, summary) if `llvm-profdata show` reads the profile, else (False, its error)."""
    try:
        result = subprocess.run([tool, 'show', path], capture_output=True, text=True,
                                errors='replace', timeout=600, check=False)
    except subprocess.TimeoutExpired:
        return False, 'timed out'
    lines = [line.strip() for line in (result.stderr + '\n' + result.stdout).splitlines()]
    lines = [line for line in lines if line]
    if result.returncode == 0:
        return True, '; '.join(line for line in lines if line.startswith('Total functions'))
    errors = [line for line in lines if 'error' in line.lower()]
    return False, (errors or lines or [f'exit status {result.returncode}'])[0][:400]


def cut(data, source, target, tool):
    starts = find_dumps(data)
    if not starts:
        return fail(f'{source}: not a 64-bit raw profile (no raw magic at its start)')
    count = len(starts)
    version = struct.unpack_from('<Q', data, 8)[0] & 0xffffffff
    log(f'{source}: {count} dump{"s" if count != 1 else ""}, raw profile version {version}')
    ends = starts[1:] + [len(data)]
    part = target + '.part'
    verdicts = {}

    def readable(index):
        if not tool:
            return True
        if index not in verdicts:
            copy_ranges(data, [(starts[index], ends[index])], part)
            ok, detail = show(tool, part)
            if not ok:
                log(f'{source}: llvm-profdata does not read dump {index + 1} of {count}: '
                    f'{detail}')
                # Every dump has the first one's version, so none would be read.
                if 'version mismatch' in detail:
                    raise Unreadable
            verdicts[index] = (ok, detail)
        return verdicts[index][0]

    try:
        groups = sessions(starts, ends, readable)
        if len(groups) > 1:
            log(f'{source}: {len(groups)} sessions (a shorter dump starts one): dumps ' +
                ', '.join(f'{group[0] + 1}-{group[-1] + 1}' if len(group) > 1 else
                          f'{group[0] + 1}' for group in groups))
        chosen = []
        for number, group in enumerate(groups, 1):
            last = next((index for index in reversed(group) if readable(index)), None)
            if last is None:
                log(f'{source}: llvm-profdata reads no dump of session {number}; left out')
            else:
                chosen.append(last)
        if chosen:
            copy_ranges(data, [(starts[index], ends[index]) for index in chosen], part)
            os.replace(part, target)
            for index in chosen:
                start, end = starts[index], ends[index]
                checked = ''
                if tool:
                    checked = f'; llvm-profdata reads it ({verdicts[index][1] or "show: ok"})'
                if count == 1:
                    which = 'its only dump, copied unchanged'
                else:
                    which = f'dump {index + 1} of {count} at byte {start}'
                log(f'{target}: {which}: {end - start} bytes, '
                    f'{header_counts(data, start, end, version)}{checked}')
            if len(chosen) > 1:
                log(f'{target}: {len(chosen)} dumps, one per session, '
                    f'{sum(ends[index] - starts[index] for index in chosen)} bytes')
            return 0
    except Unreadable:
        pass
    finally:
        if os.path.exists(part):
            os.remove(part)
    return fail(f'{source}: no dump that llvm-profdata reads; {target} not written')


def main():
    parser = argparse.ArgumentParser(
        description='Write the last dump of each session in a .profraw that a pgo-train build '
                    'appended to every 30 s.')
    parser.add_argument('--llvm-profdata', metavar='PATH',
                        help='use only dumps that `PATH show` reads')
    parser.add_argument('input', metavar='IN', help='the recorded .profraw')
    parser.add_argument('output', metavar='OUT', help='where to write the last dumps')
    args = parser.parse_args()
    tool = None
    if args.llvm_profdata:
        tool = shutil.which(args.llvm_profdata)
        if tool is None:
            return fail(f'{args.llvm_profdata}: not found')
    try:
        if os.path.exists(args.output) and os.path.samefile(args.input, args.output):
            return fail('IN and OUT are the same file')
        with open(args.input, 'rb') as file:
            if os.fstat(file.fileno()).st_size == 0:
                return fail(f'{args.input}: empty')
            with mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
                return cut(data, args.input, args.output, tool)
    except OSError as error:
        return fail(str(error))


if __name__ == '__main__':
    sys.exit(main())
