#!/bin/bash
# tools/ipa/pack.sh IPA OUT.tar: an .ipa as the phone's hkinstall takes it (userland/src/hkinstall.m): Payload/NAME.app exactly as it is in the .ipa, and ents/BUNDLE.plist, each binary's
# entitlements as its own signature lists them, since reading those on the phone would need a Mach-O parser for no gain. Prints OUT.tar's SHA-256. Refuses an encrypted binary the phone would run: it could not.
# Untrusted input: the .ipa is unpacked into a directory of its own and nothing in it is run.
set -e
IPA=$1; OUT=$2
[ -f "$IPA" ] && [ -n "$OUT" ] || { echo "usage: pack.sh IPA OUT.tar"; exit 2; }
W=$(mktemp -d "${TMPDIR:-/tmp}/ipa-pack.XXXXXX")
trap 'rm -rf "$W"' EXIT
( cd "$W" && unzip -q "$OLDPWD/$IPA" 2>/dev/null || unzip -q "$IPA" )
mkdir "$W/ents"
A=$(ls -d "$W"/Payload/*.app | head -1)
[ -d "$A" ] || { echo "no .app in $IPA"; exit 1; }
find "$A" -type f -perm -u+x -print0 | while IFS= read -r -d '' F; do
	if file "$F" | grep -q Mach-O && xcrun otool -l "$F" | grep -q "cryptid 1"; then
		# (a Watch app is for the watch: the phone never runs it, and it goes as it is, as nothing in a bundle is changed)
		case "$F" in
		"$A"/Watch/*) echo "encrypted, for the watch, not run here: ${F#"$A"/}" ;;
		*) echo "encrypted: ${F#"$W"/}"; exit 1 ;;
		esac
	fi
done
for B in "$A" "$A"/PlugIns/*.appex; do
	[ -f "$B/Info.plist" ] || continue
	ID=$(plutil -extract CFBundleIdentifier raw "$B/Info.plist")
	EXE=$(plutil -extract CFBundleExecutable raw "$B/Info.plist")
	codesign -d --entitlements - --xml "$B/$EXE" 2>/dev/null > "$W/ents/$ID.plist" || true
	[ -s "$W/ents/$ID.plist" ] || rm -f "$W/ents/$ID.plist"
done
( cd "$W" && COPYFILE_DISABLE=1 tar --format ustar --no-xattrs --no-mac-metadata -cf - Payload ents ) > "$OUT"
ls -l "$OUT" | awk '{print $5, "bytes"}'
ls "$W/ents" | wc -l | awk '{print $1, "entitlement lists"}'
shasum -a 256 "$OUT" | cut -d' ' -f1
