#!/bin/bash
# inner.sh: what boot.sh runs while it holds the phone. The loader is entered only with the pinned image (cold_recover.py --developer, from the
# stock fastboot, so the modem starts cold); xl never builds an image here. Afterwards the phone goes back to the stock fastboot and the slots are read.
set -u
# (CELL_ENTER_WAIT=SECS: how long the phone may take to show up in the fastboot or the loader, 1800 at the most: a ticket queued before the owner has put it there)
python3 tools/sensors/cold_recover.py --developer --timeout "${CELL_ENTER_WAIT:-120}" || { echo "inner: the loader was not entered: nothing run"; exit 70; }
. common/lib.sh; load_device instantnoodle
[ "$(./xl state)" = loader ] || { echo "inner: state is $(./xl state), not loader: nothing run"; exit 70; }
./xl wait 1; ./xl ramdisk userland/flashboot.apfs; ./xl run; rc=$?
# (Every fastboot call from here on has a time limit: with the phone gone, a reset or the maker's crash mode, fastboot waits for ever and the
# ticket with it: bootq 348, 2026-11-11.)
# CELL_FULL=FILE: the kernel's whole returned log, read while the loader still holds it (the loader's own copy keeps the first 256 KB and the last
# 1 MB, and the live log misses the kernel's usertrap: lines): oem readpart logdump + get_staged, decoded by firmware-reader/returned_log.py
if [ -n "${CELL_FULL:-}" ]; then
	n=$(timeout 20 fastboot -s xnuloader getvar result 2>&1 | sed -n 's/.*result: *returned *\([0-9][0-9]*\).*/\1/p' | head -1)
	if timeout 60 fastboot -s xnuloader oem "readpart logdump" >/dev/null 2>&1 && timeout 120 fastboot -s xnuloader get_staged "$CELL_FULL.bin" >/dev/null 2>&1; then
		python3 - "$CELL_FULL.bin" "$CELL_FULL" "${n:-}" <<'EOF'
import sys
sys.path.insert(0, '../oneplus8-audit/firmware-reader')
from returned_log import decode
raw = open(sys.argv[1], 'rb').read()
try:
    text = decode(raw, int(sys.argv[3]) if sys.argv[3] else None)
except Exception as e:
    text = decode(raw)
open(sys.argv[2], 'w').write(text)
print('inner: the whole returned log: %d bytes' % len(text))
EOF
		rm -f "$CELL_FULL.bin"
	else
		echo "inner: the whole returned log was not read"
	fi
fi
python3 tools/sensors/cold_recover.py --stock-fastboot --timeout 150 || echo "inner: NOT back in the stock fastboot"
for v in current-slot slot-successful:a slot-unbootable:a slot-retry-count:a; do timeout 10 fastboot getvar $v 2>&1 | grep -v -E "^Finished|^$" | head -1; done | tr '\n' ' '; echo
exit $rc
