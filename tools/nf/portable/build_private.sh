#!/bin/bash
# Invoked inside the existing build container, with a private source/output directory.
set -eu
. /build/xnu-env.sh
base=/build/xnu-src/nf-tree/xnu-8796.121.2
cd "$base/BUILD_NF/obj/DEVELOPMENT_ARM64_BCM2837"
python3 - "$1" "$base" <<'PY'
import pathlib,sys,subprocess,shlex
p=pathlib.Path(sys.argv[1]);h=pathlib.Path.cwd();bsd=h/'bsd/DEVELOPMENT'
def flags(file):
    return shlex.split(file.read_bytes().strip(b'\0\n\r ').decode().replace('((1024 * 256) / MCLBYTES)','((1024*256)/MCLBYTES)'))
subprocess.run(flags(bsd/'.CFLAGS')+['-Wno-unused-function','-I',sys.argv[2]+'/bsd/dev','-c',str(p/'instantnoodle_modem.c'),'-o',str(p/'modem.o')],cwd=bsd,check=True)
osf=h/'osfmk/DEVELOPMENT'
subprocess.run(flags(osf/'.CFLAGS')+['-I',sys.argv[2]+'/osfmk/arm64','-c',str(p/'loader_return.c'),'-o',str(p/'loader_return.o')],cwd=osf,check=True)
ld=flags(h/'.LDFLAGS');assert ld.pop()=='-lcc_kext'
files=(h/'link.filelist').read_text().splitlines();old=str(bsd/'instantnoodle_modem.o');assert files.count(old)==1
old_return=str(osf/'loader_return.o');assert files.count(old_return)==1
files=[str(p/'loader_return.o') if f==old_return else f for f in files]
(p/'link.files').write_text('\n'.join(str(p/'modem.o') if f==old else f for f in files)+'\n')
subprocess.run(ld+['-filelist',str(p/'link.files'),'version.o','lastkerneldataconst.o','lastkernelconstructor.o','nonlto.o','-o',str(p/'debug.mach'),'-lcc_kext','-lfirehose_kernel','-lpthread_kext','/build/xnu-deps/build/ath11k/out/ath11k.o'],check=True)
subprocess.run(['strip','-S',str(p/'debug.mach'),'-o',str(p/'portable.mach')],check=True)
PY
xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk -Os -Wall -Wextra -Werror -o "$1/portable_http" "$1/portable_http.c"
