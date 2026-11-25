#!/usr/bin/env python3
"""Regenerate engine/patches/0001-haiku-port.patch from the host WebKit tree.

The host checkout (.cache/WebKit) is the pinned upstream commit with the port
applied as working-tree changes. This stages everything (or only --only paths), writes the complete
diff against the pin and records its digest in engine/sources.lock.json.
"""
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def git(directory, *args, env=None):
    return subprocess.run(['git', '-C', str(directory), *args], check=True,
                          stdout=subprocess.PIPE, env=env).stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=pathlib.Path, default=ROOT / '.cache/WebKit')
    parser.add_argument('--check', action='store_true', help='Only report whether the patch is current.')
    parser.add_argument('--only', nargs='+', metavar='PATH',
                        help='Stage only these paths (relative to the WebKit tree) and leave other working-tree '
                             'changes out of the patch, to land one change while others are still in progress.')
    args = parser.parse_args()
    lock_path = ROOT / 'engine/sources.lock.json'
    lock = json.loads(lock_path.read_text())
    if git(args.source, 'rev-parse', 'HEAD').decode().strip() != lock['upstream']['commit']:
        raise SystemExit('Checkout is on a different upstream revision.')
    env = None
    if args.check:
        # Check on a copy of the index, so in-progress working-tree changes stay unstaged.
        index = pathlib.Path(git(args.source, 'rev-parse', '--git-path', 'index').decode().strip())
        if not index.is_absolute():
            index = args.source / index
        scratch = tempfile.NamedTemporaryFile(prefix='summit-index-', delete=False)
        scratch.write(index.read_bytes())
        scratch.close()
        env = dict(os.environ, GIT_INDEX_FILE=scratch.name)
    try:
        paths = args.only or ['CMakeLists.txt', 'Configurations', 'Source', 'Tools']
        git(args.source, 'add', '-A', '--', *paths, env=env)
        patch = git(args.source, 'diff', '--cached', '--no-ext-diff', '--no-textconv', '--binary',
                    '--full-index', 'HEAD', env=env)
    finally:
        if env:
            os.unlink(env['GIT_INDEX_FILE'])
    digest = hashlib.sha256(patch).hexdigest()
    if digest == lock['patch']['sha256']:
        print('Engine patch is current:', digest)
        return
    if args.check:
        raise SystemExit('Engine patch is stale; tree digest ' + digest)
    (ROOT / lock['patch']['path']).write_bytes(patch)
    lock['patch']['sha256'] = digest
    lock_path.write_text(json.dumps(lock, indent=2) + '\n')
    print('Updated engine patch:', digest)


if __name__ == '__main__':
    main()
