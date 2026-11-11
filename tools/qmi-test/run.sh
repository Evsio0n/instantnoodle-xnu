#!/bin/bash
# Build and run the tests of the QMI helpers (bsd/dev/hk_qmi.c of the kernel tree) on this
# machine, under the address and undefined-behaviour sanitizers.
#
# With ath11k's sources at hand (tools/mk-ath11k.sh fetches them; ATH11K=<directory> to say where
# else they are) its own message tables are tested too; without, only what needs none of them.
# Nothing of ath11k is kept: its tables are taken out of qmi.c into the build directory each time.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
K=${HK_QMI_KROOT:-$ROOT/xnu/files}              # the working BSD copies; override for a full kernel tree
[ -f "$K/bsd/dev/hk_qmi.c" ] || K=$ROOT/xnu-src/xnu-8796.121.2
ATH=${ATH11K:-$ROOT/xnu-deps/build/ath11k/src/ath11k}
B=$ROOT/xnu-deps/build/qmi-test
rm -rf "$B"
mkdir -p "$B/inc/sys"
ln -s "$K/bsd/sys/hk_qmi.h" "$K/bsd/sys/hk_qrtr.h" "$K/bsd/sys/hk_qmi_loc.h" "$K/bsd/sys/hk_qmi_radio.h" "$B/inc/sys/"
FLAGS="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -pthread"
if [ -f "$ATH/qmi.c" ]; then
	"$HERE/extract.py" "$ATH" > "$B/tables.inc"
	${CC:-cc} $FLAGS -I"$B/inc" -I"$B" -I"$HERE/shim" -I"$ATH" -o "$B/test" "$HERE/test_qmi.c" "$K/bsd/dev/hk_qmi.c"
	echo "qmi-test: with ath11k's tables from $ATH ($(cat "$ATH/../.release" 2>/dev/null | cut -d' ' -f1))"
else
	echo "qmi-test: no ath11k sources in $ATH: only the tests that need none"
	${CC:-cc} $FLAGS -DNO_ATH11K -I"$B/inc" -o "$B/test" "$HERE/test_qmi.c" "$K/bsd/dev/hk_qmi.c"
fi
"$B/test"
${CC:-cc} $FLAGS -I"$B/inc" -o "$B/loc-test" "$HERE/test_loc.c" "$K/bsd/dev/hk_qmi.c" "$K/bsd/dev/hk_qmi_loc.c"
"$B/loc-test"
${CC:-cc} $FLAGS -I"$B/inc" -o "$B/loc-client-test" "$HERE/test_loc_client.c" "$K/bsd/dev/hk_qmi.c" "$K/bsd/dev/hk_qmi_loc.c"
"$B/loc-client-test"
${CC:-cc} $FLAGS -I"$B/inc" -o "$B/radio-test" "$HERE/test_radio.c" "$K/bsd/dev/hk_qmi.c" "$K/bsd/dev/hk_qmi_radio.c"
"$B/radio-test"

${CC:-cc} $FLAGS -I"$B/inc" -o "$B/uim-logical-test" "$HERE/test_uim_logical.c" "$K/bsd/dev/hk_qmi.c" "$K/bsd/dev/hk_qmi_radio.c"
"$B/uim-logical-test"
