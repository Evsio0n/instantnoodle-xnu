#!/bin/bash
# tools/nf/boot.sh TAG "ARGS" JOBS [SECS]: one RAM-only boot of the next-features round (docs/research/next-features.md), everything on as the board's
# defaults have it and no cam= or torch= on the line. Nothing is flashed: ufs_write=0, var_store=0, /var in kernel memory. The kernel is a kept copy
# of BUILD_NF's, named by its SHA. JOBS is the root's job list, and the run ends at its "t-mark: done"; SECS is the longest it may take (default 150).
# NF_ROOT: the image (default out/nf-root.apfs). NF_SHOTS="MARK:NAME ...": when the log says t-mark: MARK, an xl screenshot to logs/nf-TAG-NAME.png.
# Never a mark on a screen with the camera's picture: it may show the owner's room, and no picture leaves the phone. What Camera does is read from the log.
# NF_DONE: another line of the log the run ends at. NF_SLEEP=1: the system may sleep (the board's default; test boots usually refuse it).
# Quiet by default (it may be night): sound_amps=0 haptic=0 in the line, NF_LOUD=1 leaves them out.
set -e
cd "$(dirname "$0")/../.."
TAG=$1; ARGS=$2; JOBS=$3; SECS=${4:-150}
# (a word in the wrong place, as a queue's shell can make it, and the kernel starts with no limit and nobody watching it: run 2026-11-06 about 04:34)
case $SECS in ''|*[!0-9]*) echo "boot.sh: SECS is '$SECS': nothing started"; exit 1 ;; esac
B=${NF_BUILD:-xnu-src/xnu-8796.121.2/BUILD_NF}/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
# (NF_KERNEL=FILE: a kept kernel, the owner's current row's say, in place of a build tree's; NF_VAR_MB=N: /var's size in RAM)
if [ -n "${NF_KERNEL:-}" ]; then
	K=$NF_KERNEL; SHA=$(shasum -a 256 $K | cut -c1-8)
else
	SHA=$(shasum -a 256 $B | cut -c1-8)
	K=out/kernels/nf-$SHA.mach
	[ -f $K ] || cp $B $K
fi
ROOT=${NF_ROOT:-out/nf-root.apfs}
QUIET="sound_amps=0 haptic=0"; [ -z "${NF_LOUD:-}" ] || QUIET=
NOSLEEP="syssleep=0 "; [ -z "${NF_SLEEP:-}" ] || NOSLEEP=
LINE="board_gpu_zap=0x8e21a000 fb_panel=amb655uv01 ufs_write=0 fb_hz=90 cpus=8 cpufreq=1 bootcpu=7 gpu=1 gpu_irq=0 gpu_dcvs=0 gpu_sleep=0 gpu_hwcg=0 ${NOSLEEP}ufs=2 root_apfs=ufs_darwin hk_data=2 hk_data_mb=${NF_VAR_MB:-512} var_data=hfs var_store=0 var_data_format=1 return_dark=1 exctrace=1 $QUIET $ARGS"
case " $LINE " in *" touchscreen=1 "*|*" wifi_again="*|*" adsp_again="*|*" ufs_write=1 "*|*" var_store=1 "*) echo "refused: $LINE"; exit 1 ;; esac
case " $ARGS " in
*" modem_gnss=1 "*|*" modem_gnss=2 "*|*" modem_radio=1 "*|*" modem_radio=2 "*|*" modem_radio=3 "*|*" modem_radio=4 "*|*" modem_radio=5 "*|*" modem_radio=6 "*)
	case " $LINE " in *" wifi=0 "*) ;; *) echo "refused: GNSS/radio modem runs require explicit wifi=0"; exit 1 ;; esac
	;;
esac
case " $ARGS " in *" modem_gnss=2 "*)
    for item in $LINE; do
        case "$item" in wifi=0) ;; wifi=*) echo "refused: modem_gnss=2 requires wifi=0 throughout"; exit 1 ;; esac
    done ;;
esac
FULL="loader_return_after=$SECS epoch=$(date +%s) sb_delay=8 $LINE"
PREFIX=$(wc -c < ../oneplus8-audit/gpu-plan/observed-loader-prefix.txt)
[ $(( ${#FULL} + PREFIX )) -le 607 ] || { echo "command line $(( ${#FULL} + PREFIX )) bytes, the loader takes 607"; exit 1; }
echo "kernel $SHA root $(shasum -a 256 $ROOT | cut -c1-8) ($ROOT) jobs $JOBS bytes $(( ${#FULL} + PREFIX )) args: $LINE"
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
export XL_KERNEL=$K XL_RAMDISK=$ROOT XL_SECS=$SECS XL_FOLLOW=1 XL_FOLLOW_KEEP=1 XL_CMDLINE="$LINE" XL_DONE="${NF_DONE:-t-mark: done}"
OUT=logs/nf-$TAG.out
touch "$OUT"
if [ -n "${NF_SHOTS:-}" ]; then
	( for s in $NF_SHOTS; do
		M=${s%%:*}; N=${s##*:}
		while :; do         # (until the boot's end kills this: it may wait long for the phone)
			L=$(ls -t logs/*-run.log.live 2>/dev/null | head -1)
			if [ -n "$L" ] && [ "$L" -nt "$OUT" ] && grep -a -q "t-mark: $M" "$L"; then
				. common/lib.sh > /dev/null; load_device instantnoodle > /dev/null
				./xl screenshot logs/nf-$TAG-$N.png 2>&1 | tail -1 | cut -c1-120
				break
			fi
			sleep 0.5
		done
	done ) &
	SHOT=$!
fi
# (the phone is shared: another session's next run may take the lock first, and then this one waits again)
for try in $(seq 1 ${NF_TRIES:-120}); do
	while ! flock -n ${XDG_RUNTIME_DIR:-/tmp}/hackios-phone.run true; do sleep 1; done
	bash -c '. common/lib.sh; load_device instantnoodle; ./xl ramdisk userland/flashboot.apfs; ./xl run' > "$OUT" 2>&1 || true
	grep -a -q 'a run is in progress' "$OUT" || break
	sleep 2
done
[ -z "${SHOT:-}" ] || kill $SHOT 2>/dev/null || true
tail -3 "$OUT" | cut -c1-200
LOG=$(grep -a -o 'logs/[0-9]*-run\.log' "$OUT" | tail -1)
[ -n "$LOG" ] || { echo "no run log named in $OUT"; exit 2; }
cat $(ls "$LOG".live.* 2>/dev/null | sort -t. -k4 -n) "$LOG" 2>/dev/null | awk '!seen[$0]++' > logs/nf-$TAG.txt
echo "$LOG -> logs/nf-$TAG.txt, $(wc -l < logs/nf-$TAG.txt) lines"
