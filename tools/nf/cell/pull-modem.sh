#!/bin/bash
# pull-modem.sh: the modem firmware partition (modem_a, the FAT volume the modem's images are read from) copied to out/cell-fw/modem_a.img, read only,
# from slot a's recovery (TWRP). For the modem's message database (image/qdsp6m.qdb), with which its terse DIAG messages are read. Vendor firmware:
# stays in out/cell-fw/ (not in git), never in git or in documents. The phone's serial: tools/sensors/stock-serial.local. Run through tools/bootq (it holds the phone); ends in the stock fastboot.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd); K=$(cd "$HERE/../../.." && pwd)
cd "$K"
SER=$(cat tools/sensors/stock-serial.local); A="adb -s $SER"; FW=$K/out/cell-fw; mkdir -p -m 700 "$FW"
python3 tools/sensors/cold_recover.py --stock-fastboot --timeout 150
fastboot -s $SER reboot recovery </dev/null 2>&1 | head -1
for i in $(seq 1 60); do s=$(adb devices 2>/dev/null | sed -n 2p | awk '{print $2}'); [ "$s" = recovery ] && break; sleep 2; done
[ "$s" = recovery ] || { echo "pull-modem: recovery did not come up (adb: ${s:-none})"; exit 1; }
[ "$($A shell getprop ro.boot.slot_suffix </dev/null | tr -d '\r')" = _a ] || { echo "pull-modem: not slot a"; exit 1; }
SZ=$($A shell blockdev --getsize64 /dev/block/by-name/modem_a </dev/null | tr -d '\r')
echo "modem_a is $SZ bytes"
$A exec-out "dd if=/dev/block/by-name/modem_a bs=1048576 2>/dev/null" </dev/null > "$FW/modem_a.img"
chmod 600 "$FW/modem_a.img"
GOT=$(stat -f %z "$FW/modem_a.img")
echo "copied $GOT bytes"
$A reboot bootloader </dev/null
for i in $(seq 1 40); do f=$(fastboot devices 2>/dev/null | awk '{print $1":"$2}' | head -1); [ "$f" = "$SER:fastboot" ] && break; sleep 2; done
for v in current-slot slot-successful:a slot-unbootable:a slot-retry-count:a; do fastboot -s $SER getvar $v 2>&1 | grep -v -E "^Finished|^$" | head -1; done | tr '\n' ' '; echo
[ "$GOT" = "$SZ" ]
