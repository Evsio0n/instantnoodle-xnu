#!/bin/bash
# in the container (docker exec oneplus8-build bash /build/tools/nf/mk_media.sh): this round's libhackios-media.dylib, as userland/build.sh makes it, to out/nf-media/.
source /build/xnu-env.sh
cd /build/userland || exit 1
export THEOS=/build/local-theos
DYN="xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1"
$DYN -O2 -dynamiclib -install_name /hackios/lib/libhackios-media.dylib -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework IOSurface -lobjc \
	-o /build/out/nf-media/libhackios-media.dylib src/inject_media.c src/camera_device.c src/camera_isp.c src/camera_gpu.c 2>&1 | grep -E "error|not found" | head -20
# the Bluetooth pieces (userland/build.sh's BLUETOOTH=1), kept beside it: mk_root.sh's *.dylib takes only the libraries a root is to have
$DYN -dynamiclib -install_name /hackios/lib/libhackios-bt.dylib -o /build/out/nf-media/libhackios-bt.dylib src/inject_bt.c 2>&1 | grep -E "error|not found" | head
$DYN -o /build/out/nf-media/btboot src/btboot.c 2>&1 | grep -E "error|not found" | head
$DYN -fobjc-arc -framework Foundation -Wl,-sectcreate,__TEXT,__info_plist,src/bt_scan.plist -o /build/out/nf-media/bt_scan src/bt_scan.m 2>&1 | grep -E "error|not found" | head
# ... and the round's checks: numbers of a photograph on the phone (no picture leaves it), and what locationd says
$DYN -framework CoreFoundation -framework CoreGraphics -framework ImageIO -o /build/out/nf-media/photo_info src/photo_info.c 2>&1 | grep -E "error|not found" | head
$DYN -fobjc-arc -framework Foundation -framework CoreLocation -o /build/out/nf-media/location_test src/location_test.m 2>&1 | grep -E "error|not found" | head
$DYN -o /build/out/nf-media/threadsof src/threadsof.c 2>&1 | grep -E "error|not found" | head
$DYN -fobjc-arc -framework Foundation -o /build/out/nf-media/hkinstall src/hkinstall.m 2>&1 | grep -E "error|not found" | head
ls -la /build/out/nf-media/
