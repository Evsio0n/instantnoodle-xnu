#!/bin/sh
# Build our mobile probe with the same local SDK used to pin the RAM root.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: build_abp_match.sh OUTPUT" >&2
    exit 2
fi
destination=$1
root=$(cd "$(dirname "$0")/../../.." && pwd)
temporary=$(mktemp -d)
trap 'rm -rf "$temporary"' EXIT
cd "$root"
xcrun --sdk iphoneos clang -target arm64-apple-ios16.5 \
    -isysroot "$root/local-theos/sdks/iPhoneOS16.5.sdk" \
    -Wall -Wextra -Werror -O2 -framework CoreFoundation -framework IOKit \
    tools/nf/stock-cellular/abp_match.c -o "$temporary/abp_match"
codesign --sign - --identifier org.hackios.abp-match "$temporary/abp_match"
codesign --verify --strict "$temporary/abp_match"
cd - >/dev/null
install -m 0755 "$temporary/abp_match" "$destination"
