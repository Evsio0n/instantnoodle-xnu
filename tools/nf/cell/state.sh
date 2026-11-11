#!/bin/bash
# state.sh: what is installed on the phone, read only: the loader is entered with the pinned image (cold_recover.py --developer), asked for its look at
# the boot area (xl health: the two bundle slots and the boot guard) and for boot_a's and boot_b's SHA-256 (oem readpart: it stages a copy and logs the sum),
# and the phone goes back to the stock fastboot. Nothing is written. Through bootq.
set -u
cd "$(cd "$(dirname "$0")" && pwd)/../../.."
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
python3 tools/sensors/cold_recover.py --developer --timeout 120 || { echo "state: the loader was not entered"; exit 70; }
. common/lib.sh; load_device instantnoodle
[ "$(./xl state)" = loader ] || { echo "state: state is $(./xl state), not loader"; exit 70; }
fastboot -s xnuloader oem "readpart boot_a" 2>&1 | grep -v -E "^$|Finished" | tail -3
fastboot -s xnuloader oem "readpart boot_b" 2>&1 | grep -v -E "^$|Finished" | tail -3
./xl health
grep -a -E "readpart|sha|SHA" logs/loader.log | tail -8
python3 tools/sensors/cold_recover.py --stock-fastboot --timeout 150 || echo "state: NOT back in the stock fastboot"
for v in current-slot slot-successful:a slot-unbootable:a slot-retry-count:a; do fastboot getvar $v 2>&1 | grep -v -E "^Finished|^$" | head -1; done | tr '\n' ' '; echo
