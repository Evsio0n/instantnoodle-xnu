#!/bin/sh
set -eu
HERE=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
K=$(CDPATH='' cd -- "$HERE/../.." && pwd)
B=$(mktemp -d);trap 'rm -rf "$B"' EXIT
mkdir -p "$B/sys"
for n in hk_qrtr hk_qrtr_core hk_qrtr_wire hk_qrtr_mhi hk_qmux_nas hk_abp_nas hk_qmux_owner hk_qmux_ingress hk_abp_nas_worker; do
  ln -s "$K/xnu/files/bsd/sys/$n.h" "$B/sys/$n.h"
done
for sanitizer in address,undefined thread; do
  ${CC:-cc} -std=gnu11 -g -O1 -Wall -Wextra -Werror -pthread -fsanitize="$sanitizer" -fno-sanitize-recover=all \
      -I"$B" "$HERE/test_qmux_owner.c" "$K/xnu/files/bsd/dev/hk_qrtr_core.c" "$K/xnu/files/bsd/dev/hk_qrtr_mhi.c" \
      "$K/xnu/files/bsd/dev/hk_abp_nas.c" -o "$B/owner-test"
  for test in start-failed early-stop normal stop-in-callback timeout overflow; do "$B/owner-test" "$test"; done
  ${CC:-cc} -std=gnu11 -g -O1 -Wall -Wextra -Werror -pthread -fsanitize="$sanitizer" -fno-sanitize-recover=all \
      -I"$B" "$HERE/test_worker_api.c" "$K/xnu/files/bsd/dev/hk_qrtr_core.c" "$K/xnu/files/bsd/dev/hk_qrtr_mhi.c" \
      "$K/xnu/files/bsd/dev/hk_abp_nas.c" "$K/xnu/files/bsd/dev/hk_abp_nas_worker.c" -o "$B/api-test"
  for test in failed-start normal timeout overflow producer-before-lock; do "$B/api-test" "$test"; done
done
