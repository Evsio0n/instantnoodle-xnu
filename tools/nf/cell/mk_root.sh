#!/bin/bash
# mk_root.sh TAG JOBS [FILE=DEST ...]: a test root for the cellular runs: the shared current root of the RAM tests (out/current-root, read only here:
# copied), without the Wi-Fi network file (these runs have wifi=0 and the image then holds nobody's passphrase), with JOBS as etc/rc.jobs and each
# FILE put at DEST inside it. Writes out/cell-TAG-root.apfs and prints its sha256.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); K=$(cd "$HERE/../../.." && pwd)
TAG=$1; JOBS=$2; shift 2
# (CELL_BASE=DIR: that root instead, as unpack_root.py made it of an image: for example the installed Metal 3 root)
W=$K/out/cell-root-work; rm -rf "$W"; cp -a "${CELL_BASE:-$K/out/current-root}" "$W"
rm -f "$W/fw/network"
# (CELL_WIFI=1: with the network the radio joins by itself, from fw-wifi/network as out/mk-current-root.sh does: the owner's, never read here,
# shown or logged; the image is then 0600 and is not shared)
[ -n "${CELL_WIFI:-}" ] && [ -f "$K/fw-wifi/network" ] && install -m 0600 "$K/fw-wifi/network" "$W/fw/network"
cp "$JOBS" "$W/etc/rc.jobs"
for pair in "$@"; do src=${pair%%=*}; dst=${pair#*=}; mkdir -p "$W/$(dirname "$dst")"; cp "$src" "$W/$dst"; case "$dst" in *.plist) chmod 644 "$W/$dst" ;; *) chmod 755 "$W/$dst" ;; esac; done
cd "$K"
python3 tools/apfs/mkapfs.py -z --plain fw -o "out/cell-$TAG-root.apfs" "hackios=$W" | tail -1 | cut -c1-120
[ -n "${CELL_WIFI:-}" ] && chmod 600 "$K/out/cell-$TAG-root.apfs"
echo "root $(shasum -a 256 "out/cell-$TAG-root.apfs" | cut -c1-16) out/cell-$TAG-root.apfs"
rm -rf "$W"
