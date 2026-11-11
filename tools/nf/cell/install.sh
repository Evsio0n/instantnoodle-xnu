#!/bin/bash
# install.sh BUNDLE: the daily system written into the phone, at the owner's word only. The loader is entered with the pinned image (cold_recover.py
# --developer); `xl install` stages the bundle and the loader writes it into the boot slot that does not hold the newest good bundle, header last, all
# read back (xnuloader/flashboot.c fb_install); the date is set; then `xl flashboot` starts the installed boot from the loader itself (its menu's
# countdown takes the newest good bundle: no key is needed, no reset). boot_a, boot_b, the slots' attributes and the data volume's contents are not
# written by this. An installed bundle has no time limit: the phone stays in iOS, and this script ends without taking it back to the fastboot.
set -u
B=$1
cd "$(cd "$(dirname "$0")" && pwd)/../../.."
export PATH=/opt/homebrew/opt/coreutils/libexec/gnubin:$PATH
[ -f "$B" ] || { echo "install: no $B"; exit 64; }
python3 tools/sensors/cold_recover.py --developer --timeout 120 || { echo "install: the loader was not entered: nothing written"; exit 70; }
. common/lib.sh; load_device instantnoodle
[ "$(./xl state)" = loader ] || { echo "install: state is $(./xl state), not loader: nothing written"; exit 70; }
echo "== before:"; ./xl health
echo "== install:"; ./xl install "$B"; rc=$?
[ $rc -eq 0 ] || { echo "install: NOT INSTALLED (rc $rc): the phone goes back to the stock fastboot"; python3 tools/sensors/cold_recover.py --stock-fastboot --timeout 150; exit 71; }
./xl settime
echo "== the installed boot, from the loader:"
XL_SECS=${INSTALL_WAIT:-30} ./xl flashboot; echo "install: flashboot ended with $? (2: it runs with no time limit, as an installed bundle does)"
