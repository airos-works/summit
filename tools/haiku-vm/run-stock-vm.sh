#!/usr/bin/env bash
# tools/haiku-vm/run-stock-vm.sh DIR [SSH_PORT] [VNC_DISPLAY]
#
# Boots the machine make-stock-vm.sh made in DIR in QEMU/KVM, in the
# background: SSH as "user" on 127.0.0.1:SSH_PORT (default 2241) with
# DIR/id_ed25519, the screen on VNC 127.0.0.1:VNC_DISPLAY (default 41, port
# 5941), QMP on DIR/qmp.sock. Standard VGA, an Intel e1000 network card,
# AHCI and an Intel HD Audio card (output discarded): drivers every Haiku
# release has. (Without a sound card R1/beta6's media_addon_server crashes in
# its mixer, and media playback in every program stalls.)
set -euo pipefail
DIR=${1:?usage: run-stock-vm.sh DIR [SSH_PORT] [VNC_DISPLAY]}
SSH_PORT=${2:-2241}
VNC=${3:-41}
MEMORY=${SUMMIT_VM_MEMORY:-8G}
CPUS=${SUMMIT_VM_CPUS:-8}
exec qemu-system-x86_64 -name summit-haiku-stock -machine q35,accel=kvm -cpu host \
	-smp "$CPUS" -m "$MEMORY" \
	-drive "file=$DIR/disk.raw,format=raw,if=none,id=disk" \
	-drive "file=$DIR/data.raw,format=raw,if=none,id=data" -device ahci,id=ahci \
	-device ide-hd,drive=disk,bus=ahci.0,bootindex=0 -device ide-hd,drive=data,bus=ahci.1 \
	-netdev "user,id=net,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22" -device e1000,netdev=net \
	-audiodev none,id=sound -device intel-hda -device hda-duplex,audiodev=sound \
	-vga std -display none -vnc "127.0.0.1:$VNC" \
	-qmp "unix:$DIR/qmp.sock,server,nowait" -daemonize -pidfile "$DIR/qemu.pid" \
	-usb -device usb-tablet
