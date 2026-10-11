#!/usr/bin/env bash
# tools/haiku-vm/make-stock-vm.sh ISO DIR [DATA_GIB] [PUBLIC_KEY]
#
# A stock Haiku test machine for Summit's Haiku build, made from a release's
# anyboot image without the installer:
#
#   DIR/disk.raw  a copy of the image (its Haiku volume keeps the image's
#                 size, about 400 MB free: room for packages), set up to start
#                 the desktop directly (no first-boot prompt) and an SSH server
#                 that accepts PUBLIC_KEY (default DIR/id_ed25519.pub) for the
#                 user "user"
#   DIR/data.raw  an empty BFS volume "Data" of DATA_GIB (default 24), mounted
#                 at /Data at boot, for profiles and test files
#
# Nothing else of the release is changed. (BFS cannot grow the image's volume
# that far: it cannot move its log.) Needs a Haiku host build of bfs_shell:
# BFS_SHELL, or the first one under
# /mnt/HaikuWork/build/*/objects/linux/*/release/tools/bfs_shell.
# Boot it with tools/haiku-vm/run-stock-vm.sh.
set -euo pipefail

ISO=${1:?usage: make-stock-vm.sh ISO DIR [DATA_GIB] [PUBLIC_KEY]}
DIR=${2:?usage: make-stock-vm.sh ISO DIR [DATA_GIB] [PUBLIC_KEY]}
DATA_GIB=${3:-24}
KEY=${4:-$DIR/id_ed25519.pub}
[[ -f $KEY ]] || { echo "no public key $KEY" >&2; exit 1; }
BFS_SHELL=${BFS_SHELL:-$(ls /mnt/HaikuWork/build/*/objects/linux/*/release/tools/bfs_shell/bfs_shell 2>/dev/null | head -n 1)}
[[ -x $BFS_SHELL ]] || { echo "no bfs_shell (set BFS_SHELL)" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cp --sparse=always "$ISO" "$DIR/disk.raw.new"
# The image's Haiku (BFS) partition, MBR entry 1.
read -r start size < <(python3 - "$DIR/disk.raw.new" <<'PY'
import struct, sys
mbr = open(sys.argv[1], "rb").read(512)
entry = mbr[446:462]
if entry[4] != 0xeb:
    sys.exit("the first partition is not BFS")
lba, count = struct.unpack("<II", entry[8:16])
print(lba * 512, count * 512)
PY
)

# Files to put on the disk.
mkdir -p "$work/boot" "$work/ssh"
cat > "$work/boot/UserBootscript" <<'SH'
#!/bin/sh
# Summit test machine: the data volume, and an SSH server (stock Haiku starts
# none), with host keys made the first time.
mountvolume Data
keys=/boot/system/settings/ssh
[ -f $keys/ssh_host_ed25519_key ] || sh /boot/system/boot/post-install/sshd_keymaker.sh
/boot/system/bin/sshd
# Tests read the screen, and a blanked screen gets no app_server drawing.
(while :; do
	for team in $(ps | awk '$1 ~ /screen_blanker$/ { print $2 }'); do kill $team; done
	sleep 30
done) &
SH
cp "$KEY" "$work/ssh/authorized_keys"
# The first-boot prompt (language and keyboard) appears while this is missing.
: > "$work/Locale settings"

# bfs_shell's exit status is that of its unmount, which fails while the
# session's files are open; the listing below is the check.
"$BFS_SHELL" --start-offset "$start" --end-offset "$((start + size))" "$DIR/disk.raw.new" \
	> "$work/bfs.log" 2>&1 <<FSSH || true
mkdir -p /myfs/home/config/settings/boot
mkdir -p /myfs/home/config/settings/ssh
cp :$work/boot/UserBootscript /myfs/home/config/settings/boot/UserBootscript
cp :$work/ssh/authorized_keys /myfs/home/config/settings/ssh/authorized_keys
cp ":$work/Locale settings" "/myfs/home/config/settings/Locale settings"
chmod 755 /myfs/home/config/settings/boot/UserBootscript
chmod 700 /myfs/home/config/settings/ssh
chmod 600 /myfs/home/config/settings/ssh/authorized_keys
ls /myfs/home/config/settings/boot /myfs/home/config/settings/ssh
sync
quit
FSSH
grep -q 'UserBootscript' "$work/bfs.log" && grep -q 'authorized_keys' "$work/bfs.log" \
	|| { cat "$work/bfs.log" >&2; exit 1; }
mv "$DIR/disk.raw.new" "$DIR/disk.raw"

rm -f "$DIR/data.raw"
truncate -s "${DATA_GIB}G" "$DIR/data.raw"
"$BFS_SHELL" --initialize "$DIR/data.raw" Data > "$work/init.log" 2>&1 < /dev/null \
	|| { cat "$work/init.log" >&2; exit 1; }
echo "$DIR: disk.raw from $(basename "$ISO"), data.raw ($DATA_GIB GiB)"
