#!/bin/bash
# tools/nf/mk_extra.sh [bt] [loc] [slpi] [ipa]: what the next-features round adds to a test root, in out/nf-extra/ (mk_root.sh lays it over the root, and takes away
# what out/nf-extra/.remove names): bt = what userland/build.sh's BLUETOOTH=1 adds; loc = iOS's location daemon's job, let through mkjobs.py's DENY.
# Apple's and the vendor's files stay in out/ and fw-bt/, out of git. The programs are made in the container by tools/nf/mk_media.sh.
set -e
cd "$(dirname "$0")/../.."
X=out/nf-extra; L=out/nf-jobs
rm -rf $X $L; mkdir -p $X/LaunchDaemons $X/etc $L
: > $X/.skel
( cd userland && BLUETOOTH=1 ALLOW=locationd OSLOG=com.apple.backboardd python3 -I ./mkjobs.py --rest --root-volume ../ios165/rootfs.slt ../ios165/rootfs ../$L com.apple.bluetoothd ) | tail -1
: > $X/.remove
cp out/camera-install-root/etc/inject.conf $X/etc/inject.conf
for what in "$@"; do
	case $what in
	bt)
		mkdir -p $X/bin $X/lib $X/fw $X/hal
		cp $L/com.apple.bluetoothd.plist $L/com.apple.bluetoothuserd.plist $L/com.apple.bluetoothuiservice.plist $L/com.apple.BTServer.*.plist $X/LaunchDaemons/
		cp userland/com.hackios.bluetool.plist $X/LaunchDaemons/
		cp out/nf-media/btboot out/nf-media/bt_scan $X/bin/
		cp out/nf-media/libhackios-bt.dylib $X/lib/
		cp fw-bt/htbtfw*.tlv fw-bt/htnv*.bin $X/fw/
		cp -a ios165/rootfs/System/Library/Audio/Plug-Ins/HAL/BTAudioHALPlugin.driver $X/hal/
		echo "/hackios/lib/libhackios-bt.dylib bluetoothd" >> $X/etc/inject.conf
		echo LaunchDaemons/com.apple.BlueTool.plist >> $X/.remove
		;;
	loc)
		mkdir -p $X/bin
		cp $L/com.apple.locationd.plist $X/LaunchDaemons/
		# Interim, for RAM test roots: no fairplayd job beside locationd. Here it exits 0 every 10 s without answering, and locationd's queue waits on it for ever, and Maps
		# on locationd (run 053616); with no job the lookup fails at once (run 055706). What needs FairPlay stays unavailable, as it was. docs/KNOWN_ISSUES.md K12.
		echo LaunchDaemons/com.apple.fairplayd.H2.plist >> $X/.remove
		# Location Services' switch on from the start (locationd's key; root's and mobile's preferences: it runs as root and looks at both), as var.skel makes timed's
		mkdir -p $X/var/db/locationd-prefs
		cat > $X/var/db/locationd-prefs/com.apple.locationd.plist <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>LocationServicesEnabledIn8.0</key><true/><key>LocationServicesEnabled</key><true/></dict></plist>
PLIST
		printf 'f\t644\t0\t0\t/private/var/root/Library/Preferences/com.apple.locationd.plist\t/var/db/locationd-prefs/com.apple.locationd.plist\n' >> $X/.skel
		printf 'f\t644\t501\t501\t/private/var/mobile/Library/Preferences/com.apple.locationd.plist\t/var/db/locationd-prefs/com.apple.locationd.plist\n' >> $X/.skel
		cp out/nf-media/location_test $X/bin/
		codesign -s - -f --entitlements userland/src/location_test.entitlements $X/bin/location_test 2>&1 | tail -1
		# (battery_test open needs its entitlement, and the images' copy was built with no ldid)
		cp out/camera-install-root/bin/battery_test $X/bin/
		codesign -s - -f --entitlements userland/src/battery_test.entitlements $X/bin/battery_test 2>&1 | tail -1
		;;
	ipa)
		# The installer for an .ipa (userland/src/hkinstall.m), with what it is entitled to ask of containermanagerd and LaunchServices; codesign, as location_test
		mkdir -p $X/bin
		cp out/nf-media/hkinstall $X/bin/
		codesign -s - -f --entitlements userland/src/hkinstall.entitlements $X/bin/hkinstall 2>&1 | tail -1
		;;
	slpi)
		# The sensor DSP's firmware, as hk_slpi_boot.c reads it from fw/: the .mdt and b02 to b21, read from the phone's modem partition (not in git); b00 and b01 are in the .mdt
		mkdir -p $X/fw
		cp ../oneplus8-audit/firmware-reader/native-slpi/slpi.mdt ../oneplus8-audit/firmware-reader/native-slpi/slpi.b[0-2][0-9] $X/fw/
		rm -f $X/fw/slpi.b00 $X/fw/slpi.b01 $X/fw/slpi.b22 $X/fw/slpi.b23
		;;
	*)	echo "mk_extra.sh: what is $what?" >&2; exit 1 ;;
	esac
done
find $X \( -type d -o -type f \) -exec chmod go-w {} +
find $X -type f | wc -l
