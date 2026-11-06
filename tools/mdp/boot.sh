#!/bin/bash
# tools/mdp/boot.sh TAG "ARGS" [SECS] [DONE]: one RAM-only boot of the MDP steps (docs/research/mdp-driver-plan.md). Nothing is
# flashed: ufs_write=0, var_store=0, and /var in kernel memory. The kernel is a kept copy of BUILD_MDP's, named by its SHA, so a
# build meanwhile does not change what is sent. The run ends at the log line DONE (default "mdp: done"), SECS is the longest
# it may take (default 60). ARGS: this boot's own, e.g. "mdp=1". The radios, the DSP, the motor and the cameras are off unless
# ARGS say otherwise (MDP_ON="wifi adsp ..." leaves those at the board's defaults): these boots look at the display.
# Prints the kernel's SHA and writes the boot's merged log to logs/mdp-TAG.txt. MDP_ROOT: the image, default out/current-root.apfs.
set -e
cd "$(dirname "$0")/../.."
TAG=$1; ARGS=$2; SECS=${3:-60}; DONE=${4:-mdp: done}
B=xnu-src/xnu-8796.121.2/BUILD_MDP/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
SHA=$(shasum -a 256 $B | cut -c1-8)
K=out/kernels/mdp-$SHA.mach
[ -f $K ] || cp $B $K
ROOT=${MDP_ROOT:-out/current-root.apfs}
OFF=
for o in wifi adsp sound haptic cam; do
	case " ${MDP_ON:-} " in *" $o "*) ;; *) OFF="$OFF $o=0" ;; esac
done
LINE="board_gpu_zap=0x8e21a000 fb_panel=amb655uv01 ufs_write=0 fb_hz=90 cpus=8 cpufreq=1 bootcpu=7 gpu=1 gpu_irq=0 gpu_dcvs=0 gpu_sleep=0 gpu_hwcg=0 syssleep=0 ufs=2 root_apfs=ufs_darwin hk_data=2 hk_data_mb=512 var_data=hfs var_store=0 var_data_format=1 return_dark=1$OFF $ARGS"
case " $LINE " in *" touchscreen=1 "*|*" wifi_again="*|*" adsp_again="*|*" ufs_write=1 "*|*" var_store=1 "*) echo "refused: $LINE"; exit 1 ;; esac
FULL="loader_return_after=$SECS epoch=$(date +%s) sb_delay=8 $LINE"
PREFIX=$(wc -c < ../oneplus8-audit/gpu-plan/observed-loader-prefix.txt)
[ $(( ${#FULL} + PREFIX )) -le 607 ] || { echo "command line $(( ${#FULL} + PREFIX )) bytes, the loader takes 607"; exit 1; }
echo "kernel $SHA root $(shasum -a 256 $ROOT | cut -c1-8) ($ROOT) bytes $(( ${#FULL} + PREFIX )) args: $LINE"
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
export XL_KERNEL=$K XL_RAMDISK=$ROOT XL_SECS=$SECS XL_FOLLOW=1 XL_FOLLOW_KEEP=1 XL_CMDLINE="$LINE" XL_DONE="$DONE"
OUT=logs/mdp-$TAG.out
# MDP_SHOT=PATTERN: when the running boot's log says PATTERN, MDP_SHOT_COUNT (default 1) xl screenshots 5 s apart (memory: they show the frame,
# not MDP layers; with mdp=7 and up only with mdp_shots=N among ARGS) to logs/mdp-TAG.png, -2.png...
if [ -n "${MDP_SHOT:-}" ]; then
	touch "$OUT"
	( for i in $(seq 1 $(( SECS + 30 ))); do
		L=$(ls -t logs/*-run.log.live 2>/dev/null | head -1)
		if [ -n "$L" ] && [ "$L" -nt "$OUT" ] && grep -a -q "$MDP_SHOT" "$L"; then
			. common/lib.sh > /dev/null; load_device instantnoodle > /dev/null
			for n in $(seq 1 ${MDP_SHOT_COUNT:-1}); do
				./xl screenshot logs/mdp-$TAG$([ $n = 1 ] || echo -$n).png 2>&1 | tail -1 | cut -c1-120
				sleep 5
			done
			exit 0
		fi
		sleep 1
	done ) &
	SHOT=$!
fi
bash -c '. common/lib.sh; load_device instantnoodle; ./xl wait 120; ./xl ramdisk userland/flashboot.apfs; ./xl run' > "$OUT" 2>&1 || true
[ -z "${SHOT:-}" ] || wait $SHOT || true
tail -4 "$OUT" | cut -c1-200
LOG=$(grep -a -o 'logs/[0-9]*-run\.log' "$OUT" | tail -1)
[ -n "$LOG" ] || { echo "no run log named in $OUT"; exit 2; }
# every live copy, then the returned log, with each line once and in the order first seen
cat $(ls "$LOG".live.* 2>/dev/null | sort -t. -k4 -n) "$LOG" 2>/dev/null | awk '!seen[$0]++' > logs/mdp-$TAG.txt
echo "$LOG -> logs/mdp-$TAG.txt, $(wc -l < logs/mdp-$TAG.txt) lines"
