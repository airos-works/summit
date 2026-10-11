#!/usr/bin/env python3
"""Install pinned public FFmpeg headers for the system's optional media libraries.

This builds no codecs and changes no OS packages. The browser checks the three
shared-library ABI versions before using this public API at runtime.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, default=ROOT / '.cache/ffmpeg-headers')
    parser.add_argument('--prefix', type=Path)
    args = parser.parse_args()
    lock_path = ROOT / 'engine/ffmpeg-headers.lock.json'
    lock = json.loads(lock_path.read_text())
    prefix = args.prefix or Path('/boot/home/summit-deps/ffmpeg-' + lock['version'] + '-headers')
    if not prefix.is_absolute():
        raise SystemExit('--prefix must be absolute')
    manifest_path = prefix / 'summit-dependency-manifest.json'
    if manifest_path.exists():
        manifest = json.loads(manifest_path.read_text())
        if (manifest.get('lock_sha256') == digest(lock_path) and manifest.get('files')
                and all((prefix / path).is_file() and digest(prefix / path) == expected
                        for path, expected in manifest['files'].items())):
            print('Verified public FFmpeg headers:', prefix)
            return
    args.cache.mkdir(parents=True, exist_ok=True)
    archive_path = args.cache / ('ffmpeg-' + lock['version'] + '.tar.xz')
    if not archive_path.exists():
        with tempfile.NamedTemporaryFile(dir=args.cache, suffix='.download', delete=False) as temporary:
            download = Path(temporary.name)
        try:
            urllib.request.urlretrieve(lock['url'], download)
            if digest(download) != lock['sha256']:
                raise RuntimeError('Downloaded FFmpeg archive checksum mismatch')
            # NamedTemporaryFile makes it private; a cache shared by build
            # users (air/OS CI) needs it readable.
            download.chmod(0o644)
            download.replace(archive_path)
        finally:
            download.unlink(missing_ok=True)
    if digest(archive_path) != lock['sha256']:
        raise RuntimeError('Cached FFmpeg archive checksum mismatch')
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ffmpeg-headers-', dir=prefix.parent) as directory:
        work = Path(directory)
        with tarfile.open(archive_path) as archive:
            for member in archive.getmembers():
                if (member.name.startswith('/') or '..' in Path(member.name).parts
                        or member.isdev() or member.issym() or member.islnk()):
                    raise RuntimeError('Unexpected FFmpeg archive entry: ' + member.name)
            archive.extractall(work)
        source = work / ('ffmpeg-' + lock['version'])
        subprocess.run(['./configure', '--prefix=' + str(prefix), '--disable-everything',
                        '--disable-autodetect', '--disable-programs', '--disable-doc', '--disable-x86asm',
                        '--disable-avdevice', '--disable-avfilter', '--disable-swresample',
                        '--disable-swscale', '--disable-postproc'], cwd=source, check=True)
        subprocess.run(['make', 'install-headers'], cwd=source, check=True)
        licenses = prefix / 'share/licenses/ffmpeg-headers'
        licenses.mkdir(parents=True, exist_ok=True)
        for name in ('COPYING.LGPLv2.1', 'LICENSE.md'):
            shutil.copy2(source / name, licenses / name)
    files = {str(path.relative_to(prefix)): digest(path) for path in sorted(prefix.rglob('*'))
             if path.is_file() and path != manifest_path}
    manifest_path.write_text(json.dumps({'lock_sha256': digest(lock_path), 'abi': lock['abi'],
                                         'files': files}, indent=2) + '\n')
    print('Installed public FFmpeg headers:', prefix)


if __name__ == '__main__':
    main()
