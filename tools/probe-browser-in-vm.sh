#!/usr/bin/env bash
set -euo pipefail
SUMMIT_ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$SUMMIT_ROOT"
tar -cf - Makefile tests/BrowserProbe.cpp src/ui/Messages.h |
    bash tools/haiku.sh 'tar -xf - -C /boot/home/summit'
bash tools/haiku.sh 'cd /boot/home/summit && make build-modern-haiku/summit_browser_probe'
python3 -c 'import json, sys; print(json.dumps(sys.argv[1:]))' "$@" |
    bash tools/haiku.sh 'python3.10 -c "import json, subprocess, sys; raise SystemExit(subprocess.run([\"/boot/home/summit/build-modern-haiku/summit_browser_probe\", *json.load(sys.stdin)]).returncode)"'
