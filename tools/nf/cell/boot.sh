#!/bin/bash
# boot.sh TAG SECS ["MORE ARGS"]: one RAM boot of the kernel named in out/cell-kernel.txt (build.py) with the cellular path (cell=1), through
# tools/bootq. Nothing is written: ufs_write=0, var_store=0, /var in memory. Wi-Fi, Bluetooth, the audio DSP and the speakers off.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE/../../.."
K=$(cat out/cell-kernel.txt)
TAG=$1; SECS=$2; MORE=${3:-}
case $SECS in ''|*[!0-9]*) echo "boot.sh: SECS is '$SECS': nothing started"; exit 1 ;; esac
ROOT=${CELL_ROOT:-out/stock-cellular-ui-888873f6614dd16d87257b39597e418acf3442ca9e6fb70baa46b5b6bc24720a.apfs}
LINE="fb_panel=amb655uv01 cpus=8 bootcpu=7 gpu=1 board_gpu_zap=0x8e21a000 wifi=0 adsp=0 bt=0 sound_amps=0 syssleep=0 ufs=2 ufs_write=0 root_apfs=ufs_darwin hk_data=2 hk_data_mb=1024 var_store=0 var_data=hfs var_data_format=1 modem=5 cell=1 modem_quiet=3 modem_chans=${CELL_CHANS:-139} modem_bringup=1 modem_tftp=1 modem_blk=1024 modem_efs=1 modem_service=1 modem_qrtr=$((SECS - 70))${MORE:+ $MORE}"
# (CELL_LINE="...": that line instead of the one above, for a run with the daily system's arguments; MORE is still added)
[ -n "${CELL_LINE:-}" ] && LINE="$CELL_LINE${MORE:+ $MORE}"
case " $LINE " in *" touchscreen=1 "*|*" wifi_again="*|*" adsp_again="*|*" ufs_write=1 "*|*" var_store=1 "*) echo "refused: $LINE"; exit 1 ;; esac
FULL="loader_return_after=$SECS epoch=$(date +%s) sb_delay=8 $LINE"
PREFIX=$(wc -c < ../oneplus8-audit/gpu-plan/observed-loader-prefix.txt)
[ $(( ${#FULL} + PREFIX )) -le 607 ] || { echo "command line $(( ${#FULL} + PREFIX )) bytes, the loader takes 607"; exit 1; }
echo "kernel $(shasum -a 256 "$K" | cut -c1-8) root $(shasum -a 256 $ROOT | cut -c1-8) bytes $(( ${#FULL} + PREFIX )) args: $LINE"
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
export XL_KERNEL=$K XL_RAMDISK=$ROOT XL_SECS=$SECS XL_FOLLOW=2 XL_FOLLOW_KEEP=1 XL_SB_DELAY=8 XL_CMDLINE="$LINE"
# (CELL_DONE=REGEX: the run ends as soon as the kernel's log says it; SECS is then only the longest it may take)
[ -n "${CELL_DONE:-}" ] && export XL_DONE="$CELL_DONE"
OUT=logs/cell-$TAG.out
exec tools/bootq run --who cell --what "cellular, the Pro's driver: $TAG" --no-touch --max $((SECS + 420)) -- \
	"bash '$HERE/inner.sh' > $OUT 2>&1; rc=\$?; tail -4 $OUT | cut -c1-200; exit \$rc"
