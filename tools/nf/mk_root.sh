#!/bin/bash
# tools/nf/mk_root.sh JOBS [IMAGE]: a test root of the next-features round. Base: out/mk-current-root.sh's recipe (its own directory and image, not
# the shared out/current-root); on top, this round's libraries from out/nf-media/ and what tools/nf/mk_extra.sh left in out/nf-extra/.
set -e
cd "$(dirname "$0")/../.."
J=$(pwd)/$1; I=${2:-out/nf-root.apfs}; R=out/nf-root
( cd out && sed 's#out/current-root#out/nf-root#g' mk-current-root.sh | bash -s "$J" ) > /dev/null
cp out/nf-media/libhackios-media.dylib $R/lib/
[ ! -f out/nf-media/photo_info ] || cp out/nf-media/photo_info $R/bin/
[ ! -f out/nf-media/threadsof ] || cp out/nf-media/threadsof $R/bin/
if [ -d out/nf-extra ]; then
	cp -R out/nf-extra/. $R/
	while read -r gone; do rm -f "$R/$gone"; done < out/nf-extra/.remove
	cat out/nf-extra/.skel >> $R/etc/var.skel
	rm -f $R/.remove $R/.skel
fi
python3 tools/apfs/mkapfs.py -z --plain fw -o $I hackios=$R | tail -1 | cut -c1-120
shasum -a 256 $I | cut -c1-16
