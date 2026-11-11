#!/bin/bash
# mk_userland.sh: the user-space pieces of the cellular system, built in the oneplus8-build container from userland/src into userland/obj:
#   hk_commcenter           the stand-in for CommCenter: CoreTelephony's two Mach services answered from the kernel's kern.hackios.* (signal, carrier,
#                           SMS both ways, calls through CallKit as com.apple.coretelephony: its Info.plist is a section of the program, and it is
#                           signed with the entitlements CallKit asks a telephony provider for)
#   libhackios-call.dylib   put into callservicesd by etc/inject.conf: the speaker button's route reaches the kernel's voice path
#   oslog                   reads the system's log stream for a number of seconds (os_log lines of the processes named), with long digit runs masked
# userland/build.sh does not build them yet; mk_root.sh puts them into a root image.
set -e
docker exec oneplus8-build bash -ec '
source /build/xnu-env.sh; export THEOS=/build/local-theos; cd /build/userland
SDK=$THEOS/sdks/iPhoneOS16.5.sdk; LDID=$THEOS/toolchain/linux/iphone/bin/ldid      # (ldid is not on the PATH there)
xnu-clang -arch arm64 -isysroot $SDK -O1 -fobjc-arc -Wall -Wno-unused-function -Wno-deprecated-declarations -framework Foundation -framework CallKit \
	-Wl,-sectcreate,__TEXT,__info_plist,src/hk_commcenter.info.plist -o obj/hk_commcenter src/hk_commcenter.m
$LDID -Ssrc/hk_commcenter.entitlements obj/hk_commcenter
xnu-clang -arch arm64 -isysroot $SDK -O1 -fobjc-arc -Wall -dynamiclib -install_name /hackios/lib/libhackios-call.dylib -framework Foundation \
	-o obj/libhackios-call.dylib src/inject_call.m
xnu-clang -arch arm64 -isysroot $SDK -O1 -Wall -o obj/oslog src/oslog.c
$LDID -Ssrc/oslog.entitlements obj/oslog
ls -la obj/hk_commcenter obj/libhackios-call.dylib obj/oslog | awk "{print \$5, \$9}"
'
