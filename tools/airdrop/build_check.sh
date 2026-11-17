#!/bin/sh
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
umask 077
mkdir -p out/airdrop-ui/mode-v2
docker exec -e THEOS=/build/local-theos -w /build oneplus8-build bash -ec '
  source /build/xnu-env.sh
  xnu-clang -arch arm64 -miphoneos-version-min=16.5 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk \
    -fobjc-arc -Wall -Wextra -Werror -O2 -framework Foundation tools/airdrop/observe.m \
    -o out/airdrop-ui/airdrop_check
  xnu-clang -arch arm64 -miphoneos-version-min=16.5 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk \
    -fobjc-arc -Wall -Wextra -Werror -O2 -framework Foundation tools/airdrop/mode.m \
    -o out/airdrop-ui/mode-v2/airdrop_mode
  xnu-clang -arch arm64 -miphoneos-version-min=16.5 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk \
    -std=c11 -Wall -Wextra -Werror -O2 tools/airdrop/capability.c -o out/airdrop-ui/airdrop_capability
'
chmod 0755 out/airdrop-ui/airdrop_check out/airdrop-ui/mode-v2/airdrop_mode out/airdrop-ui/airdrop_capability
COPYFILE_DISABLE=1 tar --format=ustar -C out/airdrop-ui -cf out/airdrop-ui/airdrop_check.tar airdrop_check
COPYFILE_DISABLE=1 tar --format=ustar -C out/airdrop-ui/mode-v2 -cf out/airdrop-ui/mode-v2/airdrop_mode.tar airdrop_mode
COPYFILE_DISABLE=1 tar --format=ustar -C out/airdrop-ui -cf out/airdrop-ui/airdrop_capability.tar airdrop_capability
shasum -a 256 out/airdrop-ui/airdrop_check out/airdrop-ui/airdrop_check.tar out/airdrop-ui/mode-v2/airdrop_mode out/airdrop-ui/airdrop_capability
