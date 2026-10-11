#!/usr/bin/env bash
# tools/haiku-vm/ssh.sh [COMMAND...]: a shell on the stock Haiku machine of
# run-stock-vm.sh (SUMMIT_HAIKU_VM: its directory, default .vm/haiku-r1beta6;
# SUMMIT_HAIKU_VM_PORT: its SSH port, default 2241).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
DIR=${SUMMIT_HAIKU_VM:-$ROOT/.vm/haiku-r1beta6}
exec ssh -i "$DIR/id_ed25519" -p "${SUMMIT_HAIKU_VM_PORT:-2241}" -o IdentitiesOnly=yes -o BatchMode=yes \
	-o UserKnownHostsFile="$DIR/known_hosts" -o StrictHostKeyChecking=accept-new \
	-o ConnectTimeout=10 user@127.0.0.1 "$@"
