#!/bin/bash
# preview.sh TAG SECS: the daily system as it will be installed, started once from the computer before it is written into a boot slot: the kernel named in
# out/cell-kernel.txt, the root in CELL_ROOT, and the installed bundle's own line (daily-installed-line.txt) with two changes: var_store=0 (the boot guard's state
# and the older archive are left alone, as the maintenance boot of 2026-11-06 did) and the agent on (iphont=1 iphont_control=1) so that the run can be looked at.
# UNLIKE boot.sh THIS WRITES: ufs_write=1 hk_data=1 mount the phone's kept /private/var (the HFS+ data volume) read-write, as every installed boot does.
# Only at the owner's word. No boot slot and no boot partition is touched. Through tools/bootq.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/../../.."
K=$(cat out/cell-kernel.txt)
TAG=$1; SECS=$2
case $SECS in ''|*[!0-9]*) echo "preview.sh: SECS is '$SECS': nothing started"; exit 1 ;; esac
ROOT=${CELL_ROOT:?the root image}
# (mdp_shots=0 goes too: the loader's line has no room for it beside the agent's two words, and a preview is looked at by its screenshots)
LINE="$(sed -e 's/ var_store=1 / var_store=0 /' -e 's/^sb_delay=8 //' -e 's/ mdp_shots=0 / /' "$HERE/daily-installed-line.txt") iphont=1 iphont_control=1"
case " $LINE " in *" touchscreen=1 "*|*" wifi_again="*|*" adsp_again="*|*" var_store=1 "*) echo "refused: $LINE"; exit 1 ;; esac
FULL="loader_return_after=$SECS epoch=$(date +%s) sb_delay=8 $LINE"
PREFIX=$(wc -c < ../oneplus8-audit/gpu-plan/observed-loader-prefix.txt)
[ $(( ${#FULL} + PREFIX )) -le 607 ] || { echo "command line $(( ${#FULL} + PREFIX )) bytes, the loader takes 607"; exit 1; }
echo "kernel $(shasum -a 256 "$K" | cut -c1-8) root $(shasum -a 256 $ROOT | cut -c1-8) bytes $(( ${#FULL} + PREFIX )) args: $LINE"
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
export XL_KERNEL=$K XL_RAMDISK=$ROOT XL_SECS=$SECS XL_FOLLOW=2 XL_FOLLOW_KEEP=1 XL_SB_DELAY=8 XL_CMDLINE="$LINE"
OUT=logs/cell-$TAG.out
exec tools/bootq run --who cell --what "the daily system's preview (writes the kept /var): $TAG" --no-touch --max $((SECS + 420)) -- \
	"bash '$HERE/inner.sh' > $OUT 2>&1; rc=\$?; tail -4 $OUT | cut -c1-200; exit \$rc"
