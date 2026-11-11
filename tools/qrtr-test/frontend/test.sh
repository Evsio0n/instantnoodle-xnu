#!/bin/sh
set -eu
H=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
K=$(CDPATH='' cd -- "$H/../../.." && pwd)
B=$(mktemp -d);trap 'rm -rf "$B"' EXIT
mkdir "$B/sys"
for f in hk_qrtr hk_qrtr_wire hk_qrtr_core hk_qrtr_mhi hk_qmux_nas hk_abp_nas hk_qmux_owner hk_qmux_ingress hk_abp_nas_worker hk_qmux_frontend; do
  ln -s "$K/xnu/files/bsd/sys/$f.h" "$B/sys/$f.h"
done
for san in address,undefined thread; do
  ${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -pthread -DHK_QMUX_FRONTEND_TEST -I"$B" \
    -fsanitize="$san" -fno-sanitize-recover=all "$H/test_frontend.c" \
    "$K/xnu/files/bsd/dev/hk_qrtr_core.c" "$K/xnu/files/bsd/dev/hk_qrtr_mhi.c" \
    "$K/xnu/files/bsd/dev/hk_abp_nas.c" "$K/xnu/files/bsd/dev/hk_abp_nas_worker.c" \
    "$K/xnu/files/bsd/dev/hk_qmux_frontend.c" -o "$B/front"
  "$B/front"
done
