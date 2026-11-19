#!/usr/bin/env python3
"""Build private WebTransport archives for a Haiku target with OpenSSL 3.5."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


MANIFEST = 'summit-webtransport-manifest.json'
SOURCES = (
    ('ngtcp2', '1.26.0', '7804aa0fa1c39942ba322a90805824c37231f7675b08d0c3cc4332858a7b085e'),
    ('nghttp3', '1.19.0', '228e17f5cc3d4de7bfced9912bd09de30e7f45741cc47503cf88d3fa9209e155'),
)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def prepare(args):
    target = '-'.join(name + '-' + version for name, version, _ in SOURCES)
    prefix = args.deps_dir.resolve() / target
    if prefix.exists():
        return validate_existing(prefix)

    prefix.parent.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env['CFLAGS'] = env.get('CFLAGS', '') + ' -O2 -fPIC -fvisibility=hidden'
    with tempfile.TemporaryDirectory(dir=prefix.parent, prefix='.webtransport-') as directory:
        work = Path(directory)
        stage = work / 'stage'
        for name, version, expected in SOURCES:
            release = name + '-' + version
            filename = release + '.tar.xz'
            archive = args.archive_dir / filename if args.archive_dir else work / filename
            if not args.archive_dir:
                url = f'https://github.com/ngtcp2/{name}/releases/download/v{version}/{filename}'
                subprocess.run(['curl', '--fail', '--location', '--retry', '3', '-o', archive, url], check=True)
            if sha256(archive) != expected:
                raise RuntimeError(f'Unexpected SHA-256 for {archive}')
            subprocess.run(['tar', '-xJf', archive.resolve(), '-C', work], check=True)
            source = work / release
            install = work / (name + '-install')
            options = ['--enable-lib-only', '--disable-shared', '--enable-static', '--prefix=' + str(install)]
            if args.host:
                options.append('--host=' + args.host)
            if name == 'ngtcp2':
                options.extend(['--with-openssl', '--without-gnutls', '--without-boringssl',
                                '--without-wolfssl', '--without-picotls'])
            for command in (['./configure', *options], ['make', '-j', str(args.jobs)], ['make', 'install']):
                subprocess.run(command, cwd=source, env=env, stdout=sys.stderr, check=True)
            shutil.copytree(install / 'include' / name, stage / 'develop/headers' / name)
            lib = stage / 'develop/lib'
            lib.mkdir(parents=True, exist_ok=True)
            archives = ['ngtcp2', 'ngtcp2_crypto_ossl'] if name == 'ngtcp2' else ['nghttp3']
            for library in archives:
                shutil.copy2(install / 'lib' / ('lib' + library + '.a'), lib)
            licenses = stage / 'share/licenses' / name
            licenses.mkdir(parents=True)
            shutil.copy2(source / 'COPYING', licenses)
            if name == 'nghttp3':
                shutil.copy2(source / 'lib/sfparse/COPYING', licenses / 'sfparse-COPYING')
        hashes = {str(path.relative_to(stage)): sha256(path) for path in sorted(stage.rglob('*')) if path.is_file()}
        (stage / MANIFEST).write_text(json.dumps(hashes, indent=2) + '\n')
        try:
            stage.rename(prefix)
        except OSError:
            # Another build can finish the same revision while ours is compiling.
            if not prefix.is_dir():
                raise
            return validate_existing(prefix)
    return prefix


def validate_existing(prefix):
    for name, expected in json.loads((prefix / MANIFEST).read_text()).items():
        if sha256(prefix / name) != expected:
            raise RuntimeError(f'Private WebTransport input has changed: {prefix / name}')
    return prefix


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deps-dir', type=Path, default=Path('/boot/home/summit-deps'))
    parser.add_argument('--archive-dir', type=Path, help='Use previously downloaded release tarballs')
    parser.add_argument('--host', help='GNU host triplet for a cross build')
    parser.add_argument('--jobs', type=int, default=3)
    print(prepare(parser.parse_args()))
