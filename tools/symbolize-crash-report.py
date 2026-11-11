#!/usr/bin/env python3
"""Name the frames of a Summit crash report with the build's symbol tables.

Summit's packages are stripped, so a report's frames read
"#3 libWebKit.so.1+0x1a2b3c unexported code after …". air/OS CI keeps every
build's symbol tables (airos-ci apps/store-symbols.py), served at
http://192.168.1.200/symbols/: by GNU build ID, which the report's image list
prints for the engine and Summit, and otherwise (the engine's third-party
libraries) by the package version in the report's heading. This prints the
report with each frame named by its function:

  #3 libWebKit.so.1+0x1a2b3c WebCore::RenderBox::layout()+0x12

  tools/symbolize-crash-report.py crash-20261111-101500-WebProcess-1234.txt
  tools/symbolize-crash-report.py --store /data2/airos/artifacts/symbols REPORT

A frame names the function that contains its address (return addresses are
already moved into their call). Symbols are what the binaries carry: no file
or line (the engine is built without debug information).
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

    def symbols(self, relative):
        """A symbol object (decompressed), cached under ~/.cache/summit-symbols."""
        cached = CACHE / relative.removesuffix('.xz')
        if cached.exists():
            return cached.read_bytes()
        data = self.fetch(relative)
        if data is None:
            return None
        data = lzma.decompress(data)
        cached.parent.mkdir(parents=True, exist_ok=True)
        cached.write_bytes(data)
        return data


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
        if image in tables:
            return tables[image]
        table = None
        path, identity = images.get(image, ('', None))
        object_path = f'build-id/{identity[:2]}/{identity[2:]}.debug.xz' if identity else None
        package = package_of(path or image)
        if not object_path and package:
            build = build_for(package)
            for entry in (build or {}).get('files', []):
                if entry['path'].rsplit('/', 1)[-1] == image:
                    object_path = entry['symbols']
        data = store.symbols(object_path) if object_path else None
        if object_path and data is None:
            print(f'symbolize: {image}: {object_path} is not in the store', file=sys.stderr)
        if data:
            table = SymbolTable(data)
        tables[image] = table
        return table

    found = []
    for index, line in enumerate(lines):
        match = FRAME.match(line)
        if not match:
            continue
        table = table_for(match['image'])
        result = table.lookup(int(match['offset'], 16)) if table else None
        if result:
            found.append((index, match, result))
    names = demangle(name for _i, _m, (name, _d, _in) in found)
    for index, match, (name, distance, inside) in found:
        named = f'{names[name]}+0x{distance:x}' + ('' if inside else ' (past its end)')
        lines[index] = f'{match["lead"]}{match["image"]}+0x{match["offset"]} {named}'
    sys.stdout.write('\n'.join(lines))
    named_frames = len(found)
    total = sum(1 for line in lines if FRAME.match(line))
    print(f'symbolize: named {named_frames} of {total} frames', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
