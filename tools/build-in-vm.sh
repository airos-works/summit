#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -czf - Makefile CMakeLists.txt README.md LICENSE src tests resources vendor tools docs |
    bash tools/haiku.sh 'mkdir -p /boot/home/summit && tar xzf - -C /boot/home/summit'
bash tools/haiku.sh 'cd /boot/home/summit && make -j6 && make check'
mkdir -p artifacts
scp -O -i .vm/id_ed25519 -P 2225 -o IdentitiesOnly=yes -o BatchMode=yes \
    -o UserKnownHostsFile=.vm/known_hosts \
    user@127.0.0.1:/boot/home/summit/build-modern-haiku/Summit artifacts/Summit.new
mv artifacts/Summit.new artifacts/Summit
