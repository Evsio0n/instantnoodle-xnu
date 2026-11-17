#!/bin/bash
# mk_root.sh TAG JOBS [FILE=DEST ...]: a test root for the cellular runs: the shared current root of the RAM tests (out/current-root, read only here:
# copied), without the Wi-Fi network file (these runs have wifi=0 and the image then holds nobody's passphrase), with JOBS as etc/rc.jobs and each
# FILE put at DEST inside it. Writes out/cell-TAG-root.apfs and prints its sha256.
set -e
HERE=$(cd "$(dirname "$0")" && pwd); K=$(cd "$HERE/../../.." && pwd)
TAG=$1; JOBS=$2; shift 2
# (CELL_BASE=DIR: that root instead, as unpack_root.py made it of an image: for example the installed Metal 3 root.
# A boot with the daily system's line (metal=2 metal_backend=vk camtl=1) NEEDS the installed system's root here,
# ../oneplus8-audit/cellular-data-next/claude-takeover/root-install3: out/current-root is of 4 to 6 November, and with its
# Metal library that line brings the boot logo up on a red ground (run 172147, 2026-11-11, the owner's photograph).)
W=$K/out/cell-root-work; rm -rf "$W"; cp -a "${CELL_BASE:-$K/out/current-root}" "$W"
rm -f "$W/fw/network"
# (CELL_WIFI=1: with the network the radio joins by itself, from fw-wifi/network as out/mk-current-root.sh does: the owner's, never read here,
# shown or logged; the image is then 0600 and is not shared)
[ -n "${CELL_WIFI:-}" ] && [ -f "$K/fw-wifi/network" ] && install -m 0600 "$K/fw-wifi/network" "$W/fw/network"
cp "$JOBS" "$W/etc/rc.jobs"
# (CELL_SEED=DIR: that directory as the root's seed of translated shaders and pipelines, share/air-cache, in place of the base's; CELL_SEED=none: no
# seed at all, for the harvest of a new libvulkan, whose keys the old seed's entries cannot have: tools/metal/loop/cache_take.py)
if [ -n "${CELL_SEED:-}" ]; then rm -rf "$W/share/air-cache"; [ "$CELL_SEED" = none ] || { cp -R "$CELL_SEED" "$W/share/air-cache"; chmod -R a+rX "$W/share/air-cache"; }; fi
# (CELL_ANGELS=1: the launch angels' jobs in their own directory of the root, LaunchAngels, as iOS has them (their names: ios165/rootfs/System/Library/LaunchAngels).
# A base made before 2026-11-11 has them among LaunchDaemons, where launchd takes them for daemons and SpringBoard then lays PosterBoard's scene over the whole
# display (docs/KNOWN_ISSUES.md K15). The root's init must bind the directory (userland/src/init.c of that date): give one as FILE=sbin/init.)
if [ -n "${CELL_ANGELS:-}" ]; then
  mkdir -p "$W/LaunchAngels"
  for f in "$K"/ios165/rootfs/System/Library/LaunchAngels/*.plist; do n=$(basename "$f"); if [ -f "$W/LaunchDaemons/$n" ]; then mv "$W/LaunchDaemons/$n" "$W/LaunchAngels/$n"; fi; done
  echo "angels: $(ls "$W/LaunchAngels" | wc -l | tr -d ' ') jobs in LaunchAngels"
fi
for pair in "$@"; do src=${pair%%=*}; dst=${pair#*=}; mkdir -p "$W/$(dirname "$dst")"; cp "$src" "$W/$dst"; case "$dst" in *.plist) chmod 644 "$W/$dst" ;; *) chmod 755 "$W/$dst" ;; esac; done
cd "$K"
python3 tools/apfs/mkapfs.py -z --plain fw -o "out/cell-$TAG-root.apfs" "hackios=$W" | tail -1 | cut -c1-120
[ -n "${CELL_WIFI:-}" ] && chmod 600 "$K/out/cell-$TAG-root.apfs"
echo "root $(shasum -a 256 "out/cell-$TAG-root.apfs" | cut -c1-16) out/cell-$TAG-root.apfs"
rm -rf "$W"
