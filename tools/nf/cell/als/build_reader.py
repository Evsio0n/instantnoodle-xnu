#!/usr/bin/env python3
"""Build a read-only calibration reader that reports the first superblock's feature bits."""
import hashlib
import re
import sys
from pathlib import Path
import shutil
import subprocess
import tempfile

K = Path(__file__).resolve().parents[4]
SRC = Path(__file__).resolve().parent / 'reader-base'
CONTAINER = 'oneplus8-build'
OUT = K / 'out/cell-als-read-cali-coe'


def main():
    receipt_mode = sys.argv[1:] == ['--verified-current']
    if sys.argv[1:] and not receipt_mode:
        raise SystemExit('usage: build_reader.py [--verified-current]')
    if receipt_mode:
        text = (K/'logs/074253-run.whole.log').read_text(errors='replace')
        observed = set(re.findall(r'(?m)^sensor-cali: captured source=persist:/engineermode/cali_coe bytes=4 blocks_read=7 metadata_crc32c=0 sha256=([a-f0-9]{64}) destination=/private/var/root/sensor-calibration/cali_coe mode=0600$', text))
        assert len(observed)==1, 'missing or contradictory original capture receipt'
        assert 'slpi-cal: source_bytes=4 opened=1 reads=2 bytes_served=4 coverage=f ascii_decimal=1' in text
        receipt = K/'out/als-calibration-249.sha256'
        receipt.write_bytes(bytes.fromhex(observed.pop()))
        receipt.chmod(0o600)
    with tempfile.TemporaryDirectory() as d:
        tmp = Path(d)
        shutil.copyfile(SRC/'cali_ext4.h',tmp/'cali_ext4.h')
        source = (SRC/'read_cali_coe.c').read_text()
        before = '    return n==CE_BS;'
        assert source.count(before)==1
        source = source.replace(before, '''    if(block==0 && n==CE_BS) {
        /* Only fixed public filesystem format fields; no raw blocks or file values. */
        const uint8_t *sb=out+1024;
        uint32_t inc=ce32(sb+96);
        printf("sensor-cali: super magic=%04x state=%04x block_log=%u rev=%u compat=%08x incompat=%08x ro_compat=%08x unsupported_incompat=%08x missing_required=%08x\\n",
            ce16(sb+56),ce16(sb+58),ce32(sb+24),ce32(sb+76),ce32(sb+92),inc,ce32(sb+100),
            inc & ~(2u|64u|128u|512u|0x2000u),66u & ~inc);
    }
''' + before)
        if receipt_mode:
            header = (tmp/'cali_ext4.h').read_text()
            anchor='!(fs->inc & ~(2u|64u|128u|512u|0x2000u))'
            assert header.count(anchor)==1
            # This is the narrowly observed layout. Mark it un-replayed, retain all
            # existing inode/extent checks, require digest and two consistent reads.
            extra='(fs->inc==0x46 && fs->ro==0x7b && ce32(b+92)==0x3c)'
            header=header.replace(anchor,'('+anchor+'||'+extra+')')
            (tmp/'cali_ext4.h').write_text(header)
            shutil.copyfile(Path(__file__).with_name('cal_receipt.h'),tmp/'cal_receipt.h')
            source=source.replace('static int acquire_file(', '#include "cal_receipt.h"\nstatic int acquire_file(')
            anchor='        if(write_private(data,size)){'
            assert source.count(anchor)==1
            source=source.replace(anchor,"""        bool approved=als_current_capture(&fs,data,size);
        if(fs.inc & 4)printf("sensor-cali: needs_recovery=1 journal_replayed=0 current_extent_bytes=%u archived_receipt=249 content_and_blocks_match=%u\\n",size,approved);
        if(approved && write_private(data,size)){""")
        (tmp/'read_cali_coe.c').write_text(source)
        remote = subprocess.check_output(['docker','exec',CONTAINER,'mktemp','-d','/tmp/cell-als-reader-XXXXXXXX'],text=True).strip()
        assert remote.startswith('/tmp/cell-als-reader-')
        try:
            subprocess.run(['docker','cp',str(tmp)+'/.',CONTAINER+':'+remote],check=True)
            cmd = ('source /build/xnu-env.sh; xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk '
                   '-Wall -Wextra -Werror -Wno-unused-parameter -O2 read_cali_coe.c -o read_cali_coe')
            subprocess.run(['docker','exec','-w',remote,CONTAINER,'bash','-ec',cmd],check=True)
            subprocess.run(['docker','cp',CONTAINER+':'+remote+'/read_cali_coe',str(tmp/'binary')],check=True)
            sha=hashlib.sha256((tmp/'binary').read_bytes()).hexdigest()
            target=OUT.parent/(OUT.name+'-'+sha)
            if not target.exists():
                shutil.copyfile(tmp/'binary',target)
                target.chmod(0o555)
            assert hashlib.sha256(target.read_bytes()).hexdigest()==sha
        finally:
            subprocess.run(['docker','exec',CONTAINER,'rm','-rf','--',remote],check=True)
    if receipt_mode:
        # Replace only after the verified-current variant built successfully.
        with tempfile.NamedTemporaryFile(mode='w',dir=OUT.parent,prefix='.cell-als-reader-',delete=False) as pin:
            pin.write(str(target)+'\n')
        Path(pin.name).replace(OUT.parent/'cell-als-reader.txt')
    print(target)

if __name__=='__main__':
    main()
