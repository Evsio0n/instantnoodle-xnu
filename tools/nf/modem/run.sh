#!/bin/bash
# tools/nf/modem/run.sh: the modem's FAT reader and Sahara server on this computer, against FAT16 and FAT32 images made by macOS's own tools and a simulated device. No phone.
set -e
cd "$(dirname "$0")"
O=${TMPDIR:-/tmp}/modem-test; mkdir -p "$O"
cc -std=gnu11 -Wall -Wextra -g -fsanitize=address,undefined -o "$O/test_modem" test_modem.c modem_fat.c modem_sahara.c
for spec in "16 32" "32 128"; do
	set -- $spec
	I="$O/fat$1.img"; rm -f "$I"
	dd if=/dev/zero of="$I" bs=1m count="$2" 2> /dev/null
	D=$(diskutil image attach --noMount "$I" 2>&1 | tail -1 | grep -o '/dev/disk[0-9]*' | head -1)
	/sbin/newfs_msdos -F "$1" -v "MODEM$1" "$D" > /dev/null 2>&1
	diskutil mount "$D" > /dev/null
	python3 -I mkimage.py "/Volumes/MODEM$1"
	sync
	diskutil eject "$D" > /dev/null
	"$O/test_modem" "$I" | grep -E "^fat:|FAILED|PASS"
done
