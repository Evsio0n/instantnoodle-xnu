#!/bin/sh
# Host-only strict QRTR wire tests.  No phone access and no source generation.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
B=${TMPDIR:-/tmp}/hackios-qrtr-wire-test-$$
trap 'rm -rf "$B"' EXIT HUP INT TERM
mkdir -p "$B/inc/sys"
ln -s "$ROOT/xnu/files/bsd/sys/hk_qrtr_wire.h" "$B/inc/sys/hk_qrtr_wire.h"
ln -s "$ROOT/xnu/files/bsd/sys/hk_qrtr.h" "$B/inc/sys/hk_qrtr.h"
ln -s "$ROOT/xnu/files/bsd/sys/hk_qrtr_core.h" "$B/inc/sys/hk_qrtr_core.h"
ln -s "$ROOT/xnu/files/bsd/sys/hk_qrtr_mhi.h" "$B/inc/sys/hk_qrtr_mhi.h"
ln -s "$ROOT/xnu/files/bsd/sys/hk_qmux_nas.h" "$B/inc/sys/hk_qmux_nas.h"
ln -s "$ROOT/xnu/files/bsd/sys/hk_abp_nas.h" "$B/inc/sys/hk_abp_nas.h"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B/inc" -o "$B/test" "$HERE/test_wire.c"
"$B/test"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B/inc" -I"$ROOT/xnu/files/bsd/sys" -o "$B/core-test" \
	"$HERE/test_core.c" "$ROOT/xnu/files/bsd/dev/hk_qrtr_core.c"
"$B/core-test"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
	-fsanitize=address,undefined -fno-sanitize-recover=all \
	-I"$B/inc" -I"$ROOT/xnu/files/bsd/sys" -o "$B/mhi-test" \
	"$HERE/test_mhi.c" "$ROOT/xnu/files/bsd/dev/hk_qrtr_core.c" \
	"$ROOT/xnu/files/bsd/dev/hk_qrtr_mhi.c"
"$B/mhi-test"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B/inc" -I"$ROOT/xnu/files/bsd/sys" -o "$B/qmux-nas-test" \
    "$HERE/test_qmux_nas.c" "$ROOT/xnu/files/bsd/dev/hk_qrtr_core.c" \
    "$ROOT/xnu/files/bsd/dev/hk_qrtr_mhi.c"
"$B/qmux-nas-test"
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$B/inc" -o "$B/abp-nas-test" "$HERE/test_abp_nas.c" \
    "$ROOT/xnu/files/bsd/dev/hk_abp_nas.c" "$ROOT/xnu/files/bsd/dev/hk_qrtr_core.c" \
    "$ROOT/xnu/files/bsd/dev/hk_qrtr_mhi.c"
"$B/abp-nas-test"
