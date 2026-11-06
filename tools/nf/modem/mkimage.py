"""The files for test_modem.c's FAT images, written into the mounted volume argv[1] by macOS's own msdos driver: nested directories, long names, a fragmented file."""
import os,sys
def pattern(seed,n):
    return bytes((((at*2654435761)&0xffffffff)>>13^seed)&0xff for at in range(n))
d=os.path.join(sys.argv[1],'image','sdx55m'); os.makedirs(d)
def w(name,data,mode='wb'):
    with open(os.path.join(d,name),mode) as f: f.write(data); f.flush(); os.fsync(f.fileno())
w('sbl1.mbn',pattern(1,547904)); w('multi_image_qti.mbn',pattern(2,12616))
w('spacer.bin',b'x'*70000)
w('big.mbn',pattern(3,3333333)); w('mdmddr.mbn',pattern(4,40))
# fragmented: grown after another file was laid down behind it
full=pattern(5,200000)
w('frag.mbn',full[:100000]); w('spacer2.bin',b'y'*200000); w('frag.mbn',full[100000:],'ab')
