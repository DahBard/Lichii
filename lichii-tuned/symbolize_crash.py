"""Names the functions in a Lichii crash report (the "Host crash:" lines of eden_log.txt).

The report gives code addresses as `lichii.exe+0x<offset>`, the offset from the module's load
address (an RVA). Given the unstripped lichii.exe of the same build (the CI's
lichii-tuned-r<run>-<variant>-symbols artifact), this prints every report line that holds such an
address with the function it falls in, read from the exe's COFF symbol table:

    python3 -I symbolize_crash.py eden_log.txt lichii.exe
    python3 -I symbolize_crash.py eden_log.txt lichii.exe --method symbolizer

--method nm (default) runs llvm-nm and gives function+offset; --method symbolizer runs
llvm-symbolizer --relative-address (adds file:line and inlined frames only if the exe has debug
info). --tools <dir> picks the LLVM tools' folder. Both files are only read: the report as text,
the exe by the LLVM tools (no shell) and for its PE header.
"""

import argparse
import bisect
import os
import re
import shutil
import struct
import subprocess
import sys

MAX_REPORT_BYTES = 16 << 20
# No function of Lichii's is a MiB long.
FAR_FROM_SYMBOL = 1 << 20


def find_tool(name, tools_dir):
    candidates = []
    if tools_dir:
        candidates += [os.path.join(tools_dir, name), os.path.join(tools_dir, name + '.exe')]
    found = shutil.which(name)
    if found:
        candidates.append(found)
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    # Versioned names (Debian's llvm-nm-18, say).
    for directory in os.environ.get('PATH', '').split(os.pathsep):
        try:
            entries = sorted(os.listdir(directory), reverse=True)
        except OSError:
            continue
        for entry in entries:
            if re.fullmatch(re.escape(name) + r'-\d+(\.exe)?', entry):
                return os.path.join(directory, entry)
    return None


def pe_image_base(path):
    """The PE32+ image base from the exe's headers (the address llvm-nm adds to each RVA)."""
    with open(path, 'rb') as file:
        header = file.read(4096)
    if len(header) < 0x40 or header[:2] != b'MZ':
        raise ValueError(f'{path}: not a PE file (no MZ header)')
    pe = struct.unpack_from('<I', header, 0x3c)[0]
    if pe + 24 + 32 > len(header) or header[pe:pe + 4] != b'PE\0\0':
        raise ValueError(f'{path}: not a PE file (no PE signature)')
    optional = pe + 24
    magic = struct.unpack_from('<H', header, optional)[0]
    if magic == 0x20b:  # PE32+
        return struct.unpack_from('<Q', header, optional + 24)[0]
    if magic == 0x10b:  # PE32
        return struct.unpack_from('<I', header, optional + 28)[0]
    raise ValueError(f'{path}: unknown optional header magic {magic:#x}')


def read_report(path, module):
    with open(path, 'rb') as file:
        data = file.read(MAX_REPORT_BYTES + 1)
    if len(data) > MAX_REPORT_BYTES:
        raise ValueError(f'{path}: over {MAX_REPORT_BYTES >> 20} MiB, not a crash report')
    text = data.decode('utf-8', errors='replace')
    pattern = re.compile(r'(?<![\w.-])' + re.escape(module) + r'\+(0x[0-9a-fA-F]{1,16}|0)\b',
                         re.IGNORECASE)
    lines = []
    for line in text.splitlines():
        offsets = [int(match.group(1), 16) for match in pattern.finditer(line)]
        if offsets:
            lines.append((line.rstrip(), offsets))
    return lines


def run(arguments, stdin_text=None):
    result = subprocess.run(arguments, input=stdin_text, capture_output=True, text=True,
                            errors='replace', check=False)
    if result.returncode != 0:
        raise RuntimeError(f'{os.path.basename(arguments[0])} failed ({result.returncode}): '
                           f'{result.stderr.strip()[:500]}')
    return result.stdout


def names_from_nm(exe, offsets, tools_dir):
    nm = find_tool('llvm-nm', tools_dir)
    if nm is None:
        raise RuntimeError('llvm-nm not found (PATH or --tools)')
    image_base = pe_image_base(exe)
    # An absolute path: a file name can't pass for an option.
    output = run([nm, '--defined-only', '--numeric-sort', '--demangle', os.path.abspath(exe)])
    starts, names = [], []
    for line in output.splitlines():
        parts = line.split(' ', 2)
        if len(parts) != 3 or parts[1] not in ('t', 'T'):
            continue
        try:
            address = int(parts[0], 16)
        except ValueError:
            continue
        if address < image_base:
            continue
        starts.append(address - image_base)
        names.append(parts[2])
    if not starts:
        raise RuntimeError(f'{exe}: no code symbols; is it the stripped lichii.exe? Use the '
                           f'-symbols artifact')
    result = {}
    for offset in offsets:
        index = bisect.bisect_right(starts, offset) - 1
        if index < 0:
            result[offset] = '?'
            continue
        # Several names can share an address (aliases): take the last listed, as llvm-nm sorts.
        delta = offset - starts[index]
        name = f'{names[index]}+{delta:#x}' if delta else names[index]
        if delta >= FAR_FROM_SYMBOL:
            name += ' (far past that symbol: is the exe from the same build as the report?)'
        result[offset] = name
    return result


def names_from_symbolizer(exe, offsets, tools_dir):
    symbolizer = find_tool('llvm-symbolizer', tools_dir)
    if symbolizer is None:
        raise RuntimeError('llvm-symbolizer not found (PATH or --tools)')
    pe_image_base(exe)  # a PE file, else stop here
    ordered = sorted(offsets)
    output = run([symbolizer, f'--obj={os.path.abspath(exe)}', '--relative-address', '--functions=linkage',
                  '--demangle', '--inlining'],
                 ''.join(f'{offset:#x}\n' for offset in ordered))
    # One block per address: function and location lines, then an empty line.
    blocks = output.replace('\r\n', '\n').split('\n\n')
    result = {}
    for offset, block in zip(ordered, blocks):
        lines = [line for line in block.split('\n') if line]
        frames = []
        for index in range(0, len(lines) - 1, 2):
            function, location = lines[index], lines[index + 1]
            if function == '??':
                function = '?'
            frames.append(function if location.startswith('??') else f'{function} ({location})')
        result[offset] = ' <- inlined in '.join(frames) if frames else '?'
    if all(name == '?' for name in result.values()):
        raise RuntimeError(f'{exe}: no address resolved; is it the stripped lichii.exe or one '
                           f'from another build? Use the -symbols artifact')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('report', help='lichii-crash-<pid>.txt')
    parser.add_argument('exe', help='lichii.exe with symbols (the -symbols artifact)')
    parser.add_argument('--method', choices=('nm', 'symbolizer'), default='nm')
    parser.add_argument('--tools', help='folder of llvm-nm / llvm-symbolizer')
    parser.add_argument('--module', default='lichii.exe',
                        help='module name the report uses (default lichii.exe)')
    args = parser.parse_args()
    try:
        lines = read_report(args.report, args.module)
        if not lines:
            print(f'No {args.module}+0x... addresses in {args.report}')
            return 0
        offsets = sorted({offset for _, line_offsets in lines for offset in line_offsets})
        resolve = names_from_nm if args.method == 'nm' else names_from_symbolizer
        names = resolve(args.exe, offsets, args.tools)
    except (OSError, ValueError, RuntimeError) as error:
        print(f'symbolize_crash: {error}', file=sys.stderr)
        return 1
    for line, line_offsets in lines:
        print(line)
        for offset in dict.fromkeys(line_offsets):
            print(f'    {args.module}+{offset:#x} = {names.get(offset, "?")}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
