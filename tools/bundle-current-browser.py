#!/usr/bin/env python3
"""Freeze the newly built native browser and its two WebKit libraries."""
import datetime
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
ENGINE = pathlib.Path('/boot/home/summit-webkit/WebKitBuild/Release')
BUILD = ROOT / 'build-current'


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for data in iter(lambda: source.read(1024 * 1024), b''):
            value.update(data)
    return value.hexdigest()


def dynamic(path, name):
    result = subprocess.run(['readelf', '-d', str(path)], capture_output=True, text=True, check=True)
    match = re.search(r'\((?:' + name + r')\).*?\[([^\]]*)\]', result.stdout)
    return match.group(1) if match else None


def relocate(path, search_path, required=False):
    old = dynamic(path, 'RUNPATH|RPATH')
    if old is None:
        if required:
            raise RuntimeError(f'{path.name} has no configured library search path')
        return
    script = path.parent / '.relocate.cmake'
    script.write_text(f'file(RPATH_CHANGE FILE [==[{path}]==] OLD_RPATH [==[{old}]==] NEW_RPATH [==[{search_path}]==])\n')
    try:
        subprocess.run(['cmake', '-P', str(script)], check=True, stdout=subprocess.DEVNULL)
    finally:
        script.unlink(missing_ok=True)
    if dynamic(path, 'RUNPATH|RPATH') != search_path:
        raise RuntimeError(f'Failed to set the private library path for {path.name}')


def main():
    executable = BUILD / 'Summit'
    if not executable.is_file():
        raise SystemExit('Build the browser against the current engine before bundling it.')
    inputs = json.loads((BUILD / 'engine-inputs.json').read_text())
    libraries = []
    for name in ['libWebKitLegacy.so', 'libJavaScriptCore.so']:
        source = (ENGINE / 'lib' / name).resolve(strict=True)
        if source.parent != (ENGINE / 'lib').resolve():
            raise SystemExit(f'{name} does not resolve to the current engine build.')
        soname = dynamic(source, 'SONAME')
        if not soname or pathlib.Path(soname).name != soname:
            raise SystemExit(f'Invalid library identity in {source}')
        libraries.append((name, soname, source))

    bundle = pathlib.Path(tempfile.mkdtemp(prefix='bundle-', dir=BUILD))
    try:
        (bundle / 'lib').mkdir()
        shutil.copy2(executable, bundle / 'Summit')
        shutil.copytree(ROOT / 'resources', bundle / 'resources')
        shutil.copy2(ROOT / 'LICENSE', bundle / 'LICENSE-Summit')
        shutil.copy2(ROOT / 'README.md', bundle / 'PROJECT-README.md')
        (bundle / 'README.txt').write_text(
            'Summit development bundle for Haiku R1/beta6 x86_64.\n'
            'Run ./Summit. The lib directory contains its private WebKit build.\n'
            'build-manifest.json records the pinned engine source and library hashes.\n'
            'WebKit sources and build instructions accompany the host bundle.\n')
        source_root = ENGINE.parents[1] / 'Source'
        for license_file in source_root.rglob('*'):
            if license_file.is_file() and license_file.name.upper().startswith(('LICENSE', 'COPYING')):
                destination = bundle / 'licenses/WebKit' / license_file.relative_to(source_root)
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(license_file, destination)
        (bundle / 'licenses/FontAwesome').mkdir(parents=True)
        for name in ('LICENSE.txt', 'README.md'):
            shutil.copy2(ROOT / 'resources/artwork/fontawesome' / name, bundle / 'licenses/FontAwesome' / name)
        (bundle / 'licenses/nlohmann').mkdir(parents=True)
        shutil.copy2(ROOT / 'vendor/nlohmann/LICENSE.MIT', bundle / 'licenses/nlohmann/LICENSE.MIT')
        (bundle / 'licenses/CLD2').mkdir(parents=True)
        for name in ('LICENSE', 'README.md'):
            shutil.copy2(ROOT / 'vendor/cld2' / name, bundle / 'licenses/CLD2' / name)
        originals = {'Summit': digest(executable)}
        for name, soname, source in libraries:
            destination = bundle / 'lib' / source.name
            shutil.copy2(source, destination)
            originals['lib/' + source.name] = digest(source)
            relocate(destination, '$ORIGIN', required=name == 'libWebKitLegacy.so')
            for alias in {name, soname} - {source.name}:
                (bundle / 'lib' / alias).symlink_to(source.name)
        relocate(bundle / 'Summit', '$ORIGIN/lib', required=True)
        manifest = {
            'created_at': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'engine_inputs': inputs,
            'original_sha256': originals,
            'bundled_sha256': {name: digest(bundle / name) for name in originals},
        }
        (bundle / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
        (BUILD / 'latest-bundle.json').write_text(json.dumps({'bundle': str(bundle), **manifest}, indent=2) + '\n')
        print(json.dumps({'bundle': str(bundle), **manifest}, indent=2))
    except BaseException:
        shutil.rmtree(bundle)
        raise


if __name__ == '__main__':
    main()
