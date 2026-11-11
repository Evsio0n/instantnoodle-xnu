#!/bin/bash
# tools/ipa/try.sh prepare TAG TAR BUNDLE: the job list and root of one RAM boot that installs TAR (tools/ipa/pack.sh) with hkinstall and opens BUNDLE, not logged in to anything; /var in RAM, so nothing
#   is kept. HK_BASE=IMAGE: laid over that root image, the owner's row's, else tools/nf/mk_root.sh's. HK_CLAIM=1: register with TrollStore's signer fields (userland/src/hkinstall.m says when: only
#   after a launch has been seen refused for want of them). The job list holds this Mac's address, so it is made here.
#   HK_TAP="X Y": a finger on the icon there (fractions of the screen), as FrontBoard refuses a program's request where it took a finger's (run 124145). The finger goes first and the program's request after
#   the last picture, for its answer in the log: a refusal puts up an alert, which takes the next touch (run 124448). HK_WAIT=SECONDS: how long to watch it.
#   HK_SETTLE=SECONDS: that long after the home screen before the fetch, past wifid's link change and the camera's start (run 130759 stalled 0.25 s after them).
# tools/ipa/try.sh run TAG TAR 'ARGS': the boot, with the server bound to this Mac's address on its default interface, serving that one file, and gone when this ends. Through the queue, as one quoted string:
#   tools/bootq run --who next-features --what "..." --no-touch --max 460 -- "bash tools/ipa/try.sh run TAG TAR 'ARGS'". NF_KERNEL may name the row's kernel.
set -e
cd "$(dirname "$0")/../.."
STEP=$1; TAG=$2; TAR=$3
PORT=18770
IF=$(route -n get default 2>/dev/null | awk '/interface:/{print $2}')
IP=$(ipconfig getifaddr "$IF" || true)
[ -n "$IP" ] || { echo "this Mac has no address on its default interface"; exit 1; }
J=out/rc.jobs.$TAG
case $STEP in
prepare)
	BUNDLE=$4
	[ -f "$TAR" ] || { echo "no $TAR"; exit 1; }
	SHA=$(shasum -a 256 "$TAR" | cut -d' ' -f1)
	FIRST="/hackios/bin/hkinstall open $BUNDLE"; LAST=
	[ -z "${HK_TAP:-}" ] || { FIRST="/hackios/bin/rwtouch wait 3 tap $HK_TAP"; LAST="/hackios/bin/hkinstall alarm 30 open $BUNDLE"; }
	cat > "$J" <<JOBS
# $TAG: an .ipa installed and opened, not logged in; /var in RAM, so nothing is kept
/hackios/bin/rwtouch uptime 40 idle 40 25 ${HK_SETTLE:+wait $HK_SETTLE }mark $TAG-home
/hackios/bin/hkinstall alarm 400 ${HK_CLAIM:+claim }wait-net 120 fetch $IP $PORT /$(basename "$TAR") $SHA /var/tmp/hkinstall.tar unpack /var/tmp/hkinstall.tar /var/containers/hk-unpack install /var/containers/hk-unpack state $BUNDLE
/hackios/bin/rwtouch wait 5 mark $TAG-icon
$FIRST
/hackios/bin/rwtouch wait ${HK_WAIT:-25} mark $TAG-app
${LAST:+$LAST
}/hackios/bin/rwtouch wait 5 mark done
JOBS
	if [ -n "${HK_BASE:-}" ]; then
		R=out/$TAG-tree
		rm -rf "$R"
		python3 -I tools/ipa/overlay_root.py "$HK_BASE" "$R"
		cp out/nf-extra/bin/hkinstall "$R/bin/hkinstall"
		cp "$J" "$R/etc/rc.jobs"
		python3 tools/apfs/mkapfs.py -z --plain fw -o "out/$TAG-root.apfs" "hackios=$R" | tail -1 | cut -c1-100
		rm -rf "$R"
	else
		bash tools/nf/mk_root.sh "$J" "out/$TAG-root.apfs"
	fi
	chmod 600 "out/$TAG-root.apfs"
	shasum -a 256 "out/$TAG-root.apfs" | cut -c1-16
	;;
run)
	ARGS=${4:-}
	python3 -I tools/ipa/serve.py "$TAR" "$IP" "$PORT" 420 > "logs/nf-$TAG-serve.txt" 2>&1 &
	SERVER=$!
	trap 'kill $SERVER 2>/dev/null || true; wait $SERVER 2>/dev/null || true; kill -0 $SERVER 2>/dev/null && echo "the server is STILL RUNNING" || echo "the server has stopped: $(tail -1 logs/nf-'"$TAG"'-serve.txt)"' EXIT
	NF_VAR_MB=${NF_VAR_MB:-1536} NF_SHOTS="$TAG-icon:icon $TAG-app:app" NF_ROOT="out/$TAG-root.apfs" bash tools/nf/boot.sh "$TAG" "$ARGS" "$J" 400
	;;
*)	echo "prepare or run?"; exit 2 ;;
esac
