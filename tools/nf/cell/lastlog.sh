#!/bin/bash
# lastlog.sh FILE: the log the last run left in the loader's partition (logdump), read without running anything: the loader is entered with the pinned
# image (cold_recover.py --developer), the partition staged and fetched, and the phone goes back to the stock fastboot. Nothing is written. Through bootq.
set -u
OUT=$1
cd "$(cd "$(dirname "$0")" && pwd)/../../.."
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
python3 tools/sensors/cold_recover.py --developer --timeout 120 || { echo "lastlog: the loader was not entered"; exit 70; }
. common/lib.sh; load_device instantnoodle
[ "$(./xl state)" = loader ] || { echo "lastlog: state is $(./xl state), not loader"; exit 70; }
if fastboot -s xnuloader oem "readpart logdump" >/dev/null 2>&1 && fastboot -s xnuloader get_staged "$OUT.bin" >/dev/null 2>&1; then
	echo "lastlog: the partition: $(wc -c < "$OUT.bin") bytes"
else
	echo "lastlog: the partition was not read"
fi
./xl lastlog "$OUT.xl" 2>&1 | tail -1
python3 tools/sensors/cold_recover.py --stock-fastboot --timeout 150 || echo "lastlog: NOT back in the stock fastboot"
for v in current-slot slot-successful:a slot-unbootable:a slot-retry-count:a; do fastboot getvar $v 2>&1 | grep -v -E "^Finished|^$" | head -1; done | tr '\n' ' '; echo
