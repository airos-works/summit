#!/usr/bin/env python3
"""Name the frames of a Summit crash report with the build's symbol tables.

Summit's packages are stripped, so a report's frames read
"#3 libWebKit.so.1+0x1a2b3c unexported code after …". air/OS CI keeps every
build's symbol tables (airos-ci apps/store-symbols.py), served at
http://192.168.1.200/symbols/: by GNU build ID, which the report's image list
prints for the engine and Summit, and otherwise (the engine's third-party
libraries) by the package version in the report's heading. This prints the
report with each frame named by its function, and where the build kept line
tables (the engine and Summit since 12 November 2026, for 180 days), the file
and line, with the calls inlined there (innermost first, as a debugger shows
them), read with llvm-symbolizer:

  #3 libWebKit.so.1+0x1a2b3c WebCore::RenderBox::nextSiblingBox() const
       at WebCore/PrivateHeaders/WebCore/RenderBox.h:812
       inlined into WebCore::RenderBlockFlow::layoutBlockChildren(…)+0x200
       at Source/WebCore/rendering/RenderBlockFlow.cpp:1229

  tools/symbolize-crash-report.py crash-20261111-101500-WebProcess-1234.txt
  tools/symbolize-crash-report.py --store /data2/airos/artifacts/symbols REPORT

A frame names the function that contains its address (return addresses are
already moved into their call). Line tables are cached decompressed in
~/.cache/summit-symbols (about 1.2 GB per engine build).
"""
import argparse
import bisect
import json
import lzma
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import urllib.error
import urllib.request

DEFAULT_STORE = 'http://192.168.1.200/symbols/'
CACHE = pathlib.Path(os.environ.get('XDG_CACHE_HOME', pathlib.Path.home() / '.cache')) / 'summit-symbols'

PT_LOAD, PF_W = 1, 2
SHT_SYMTAB, SHF_EXECINSTR = 2, 4
STT_NOTYPE, STT_FUNC, STT_GNU_IFUNC = 0, 2, 10

FRAME = re.compile(r'^(?P<lead>\s*#\d+ )(?P<image>[^\s+]+)\+0x(?P<offset>[0-9a-fA-F]+)(?P<rest>.*)$')
IMAGE = re.compile(r'^\s+0x[0-9a-fA-F]+-0x[0-9a-fA-F]+ (?P<path>\S+)(?: build-id (?P<id>[0-9a-f]+))?\s*$')
PACKAGE = re.compile(r'^\*\*(?:Summit|Engine):\*\* (?P<name>summit(?:_webkit)?)-(?P<version>\d\S*?)'
                     r'(?:-(?P<arch>x86_64|arm64))?(?:\.hpkg)?(?:\s|$)')
SYSTEM = re.compile(r'^\*\*System:\*\* .*?, (?P<machine>x86_64|arm64|aarch64), ')


class Store:
    def __init__(self, location):
        self.location = location.rstrip('/')
        self.remote = re.match(r'https?://', location) is not None

    def fetch(self, relative):
        """The file's bytes, or None when the store has no such file."""
        if not self.remote:
            path = pathlib.Path(self.location) / relative
            return path.read_bytes() if path.exists() else None
        try:
            with urllib.request.urlopen(f'{self.location}/{relative}', timeout=60) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code == 404:
                return None
            raise

    def object(self, relative):
        """A stored object, decompressed into ~/.cache/summit-symbols: its
        path, or None when the store has no such object."""
        cached = CACHE / relative.removesuffix('.xz')
        if cached.exists():
            return cached
        cached.parent.mkdir(parents=True, exist_ok=True)
        partial = cached.with_name(cached.name + '.part')
        if self.remote:
            try:
                source = urllib.request.urlopen(f'{self.location}/{relative}', timeout=60)
            except urllib.error.HTTPError as error:
                if error.code == 404:
                    return None
                raise
        else:
            path = pathlib.Path(self.location) / relative
            if not path.exists():
                return None
            source = open(path, 'rb')
        with source, lzma.open(source) as data, open(partial, 'wb') as out:
            shutil.copyfileobj(data, out, 1 << 20)
        partial.replace(cached)
        return cached


def symbolizer():
    found = shutil.which('llvm-symbolizer')
    if not found:
        candidates = sorted(pathlib.Path('/usr/bin').glob('llvm-symbolizer-*')) \
            + sorted(pathlib.Path('/usr/lib').glob('llvm-*/bin/llvm-symbolizer'))
        found = str(candidates[-1]) if candidates else None
    return found


def short_path(path):
    """A source path from the build machine, from the tree's root."""
    path = re.sub(r'^.*?/(?:src|build)/webkit-[^/]+/', '', path)
    path = re.sub(r'^.*?/include/c\+\+/', 'c++/', path)
    return re.sub(r'^.*?/appbuild-[^/]+/[^/]+/src/', '', path)


def line_frames(tool, path, addresses):
    """{address: [(function, file:line), ...] innermost first} from line tables."""
    if not tool or not addresses:
        return {}
    output = subprocess.run([tool, f'--obj={path}', '--output-style=JSON', '--inlines', '--demangle',
                             *[hex(a) for a in addresses]], capture_output=True, text=True).stdout
    result = {}
    for entry in json.loads(output or '[]'):
        frames = [(f.get('FunctionName', '?'), f'{short_path(f["FileName"])}:{f["Line"]}')
                  for f in entry.get('Symbol', []) if f.get('FileName') and f.get('Line')]
        if frames:
            result[int(entry['Address'], 16)] = frames
    return result


class SymbolTable:
    """The functions of a 64-bit little-endian ELF file, by address."""

    def __init__(self, data):
        if data[:4] != b'\x7fELF' or data[4] != 2 or data[5] != 1:
            raise ValueError('not a 64-bit little-endian ELF file')
        phoff, shoff = struct.unpack_from('<QQ', data, 0x20)
        phentsize, phnum, shentsize, shnum = struct.unpack_from('<HHHH', data, 0x36)
        # Haiku's image text: the first page of the segments that are not
        # writable. The report's offsets count from there.
        bases = []
        for i in range(phnum):
            p_type, flags, _offset, vaddr = struct.unpack_from('<IIQQ', data, phoff + i * phentsize)
            if p_type == PT_LOAD and not flags & PF_W:
                bases.append(vaddr)
        self.text_base = (min(bases) if bases else 0) & ~0xfff
        sections = [struct.unpack_from('<IIQQQQIIQQ', data, shoff + i * shentsize) for i in range(shnum)]
        entries = {}
        for _name, sh_type, _flags, _addr, offset, size, link, _info, _align, entsize in sections:
            if sh_type != SHT_SYMTAB:
                continue
            strings = sections[link][4]
            for at in range(offset, offset + size, entsize):
                st_name, st_info, _other, st_shndx, value, st_size = struct.unpack_from('<IBBHQQ', data, at)
                kind = st_info & 0xf
                if not value or not st_name or st_shndx == 0 or st_shndx >= len(sections):
                    continue
                if kind not in (STT_FUNC, STT_GNU_IFUNC, STT_NOTYPE):
                    continue
                if not sections[st_shndx][2] & SHF_EXECINSTR:
                    continue
                end = data.index(b'\0', strings + st_name)
                name = data[strings + st_name:end].decode(errors='replace')
                # Local labels and ARM mapping symbols ($x, $d) name no function.
                if name.startswith(('.L', '$')):
                    continue
                known = entries.get(value)
                if known is None or (known[2] == STT_NOTYPE and kind != STT_NOTYPE):
                    entries[value] = (name, st_size, kind)
        self.addresses = sorted(entries)
        self.entries = [entries[a] for a in self.addresses]

    def lookup(self, offset):
        """(name, distance, inside) of the function at offset from the text."""
        address = self.text_base + offset
        index = bisect.bisect_right(self.addresses, address) - 1
        if index < 0:
            return None
        name, size, _kind = self.entries[index]
        distance = address - self.addresses[index]
        return name, distance, size == 0 or distance < size


def demangle(names):
    names = sorted(set(names))
    tool = shutil.which('c++filt')
    if not tool or not names:
        return {name: name for name in names}
    output = subprocess.run([tool], input='\n'.join(names) + '\n', capture_output=True, text=True).stdout
    lines = output.split('\n')
    return {name: (lines[i] if i < len(lines) and lines[i] else name) for i, name in enumerate(names)}


def package_of(path):
    """The Summit package an image is from; None for the system's."""
    if '/summit-webkit/' in path:
        return 'summit_webkit'
    if 'libWebKitLegacy' in path:
        return 'summit_webkitlegacy'
    if path.rsplit('/', 1)[-1] == 'Summit':
        return 'summit'
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('report', type=pathlib.Path)
    parser.add_argument('--store', default=os.environ.get('SUMMIT_SYMBOLS', DEFAULT_STORE),
                        help=f'symbol store: a URL or a directory (default {DEFAULT_STORE}, or $SUMMIT_SYMBOLS)')
    parser.add_argument('--sdk', help='the build target when the report cannot tell (x86_64, arm64, x86_64-r1beta6)')
    args = parser.parse_args()
    store = Store(args.store)
    lines = args.report.read_text(errors='replace').split('\n')

    packages, images, arch = {}, {}, None
    in_images = False
    for line in lines:
        if match := PACKAGE.match(line):
            packages[match['name']] = match['version']
            arch = arch or match['arch']
        if match := SYSTEM.match(line):
            arch = arch or {'aarch64': 'arm64'}.get(match['machine'], match['machine'])
        if line.startswith('Images (text segments):'):
            in_images = True
            continue
        if in_images and (match := IMAGE.match(line)):
            path = match['path']
            images.setdefault(path.rsplit('/', 1)[-1], (path, match['id']))
    # Older reports have no "x86_64" on the package; the legacy library is
    # the engine's version.
    packages.setdefault('summit_webkitlegacy', packages.get('summit_webkit'))
    report_ids = {identity for _path, identity in images.values() if identity}

    builds_of = {}

    def build_for(package):
        """The build of the package the report ran: the one listing an image
        build ID of the report, else the --sdk one, else the newest."""
        if package in builds_of:
            return builds_of[package]
        chosen, note = None, None
        version = packages.get(package)
        if version and arch:
            data = store.fetch(f'builds/{package}-{version}-{arch}.json')
            builds = json.loads(data)['builds'] if data else []
            matching = [b for b in builds if report_ids & {f.get('buildId') for f in b['files']}]
            if args.sdk:
                matching = matching or [b for b in builds if b['sdk'] == args.sdk]
            if matching:
                chosen = matching[-1]
            elif builds:
                chosen = max(builds, key=lambda b: b['built'])
                if len({tuple(f['sha256'] for f in b['files']) for b in builds}) > 1:
                    note = (f'{package} {version}: {len(builds)} different builds '
                            f'({", ".join(b["sdk"] for b in builds)}); took {chosen["sdk"]} of {chosen["built"]} '
                            f'(--sdk chooses)')
            else:
                note = f'{package} {version} ({arch}): no symbols in the store'
        elif package in ('summit', 'summit_webkit'):
            note = f'the report names no {package} package (a development build?)'
        builds_of[package] = chosen
        if note:
            print(f'symbolize: {note}', file=sys.stderr)
        return chosen

    tables = {}

    def table_for(image):
        """(symbol table, path of its line tables or None) of an image."""
        if image in tables:
            return tables[image]
        table, lines_file = None, None
        path, identity = images.get(image, ('', None))
        object_path = lines_path = None
        if identity:
            base = f'build-id/{identity[:2]}/{identity[2:]}'
            object_path, lines_path = base + '.debug.xz', base + '.lines.xz'
        package = package_of(path or image)
        if not object_path and package:
            build = build_for(package)
            for entry in (build or {}).get('files', []):
                if entry['path'].rsplit('/', 1)[-1] == image:
                    object_path, lines_path = entry['symbols'], entry.get('lines')
        symbols_file = store.object(object_path) if object_path else None
        if object_path and symbols_file is None:
            print(f'symbolize: {image}: {object_path} is not in the store', file=sys.stderr)
        if symbols_file:
            table = SymbolTable(symbols_file.read_bytes())
            if lines_path and tool:
                lines_file = store.object(lines_path)
        tables[image] = (table, lines_file)
        return tables[image]

    tool = symbolizer()
    if not tool:
        print('symbolize: no llvm-symbolizer: functions only, no files or lines', file=sys.stderr)
    found, wanted = [], {}
    total = sum(1 for line in lines if FRAME.match(line))
    for index, line in enumerate(lines):
        match = FRAME.match(line)
        if not match:
            continue
        table, lines_file = table_for(match['image'])
        result = table.lookup(int(match['offset'], 16)) if table else None
        if result:
            address = table.text_base + int(match['offset'], 16)
            found.append((index, match, result, lines_file, address))
            if lines_file:
                wanted.setdefault(lines_file, set()).add(address)
    located = {path: line_frames(tool, path, sorted(addresses)) for path, addresses in wanted.items()}
    names = demangle(name for _i, _m, (name, _d, _in), _l, _a in found)
    with_lines = 0
    for index, match, (name, distance, inside), lines_file, address in found:
        named = f'{names[name]}+0x{distance:x}' + ('' if inside else ' (past its end)')
        head = f'{match["lead"]}{match["image"]}+0x{match["offset"]} '
        frames = located.get(lines_file, {}).get(address) if lines_file else None
        if not frames:
            lines[index] = head + named
            continue
        with_lines += 1
        indent = ' ' * (len(match['lead']) - len(match['lead'].lstrip()) + 5)
        text = []
        for depth, (function, where) in enumerate(frames):
            outer = depth == len(frames) - 1
            label = named if outer else function
            text.append((head if depth == 0 else f'{indent}inlined into ') + label)
            text.append(f'{indent}at {where}')
        lines[index] = '\n'.join(text)
    sys.stdout.write('\n'.join(lines))
    print(f'symbolize: named {len(found)} of {total} frames, {with_lines} with files and lines',
          file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
