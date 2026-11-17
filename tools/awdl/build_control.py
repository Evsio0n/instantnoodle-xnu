#!/usr/bin/env python3
"""Cross-compile the control overlay with the existing NF XNU flags in an isolated tree."""
import hashlib
import os
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
SOURCE=ROOT/'xnu/files/bsd/dev/hk_apple80211.c'
SOURCE_SHA='277112d761b6441dd90c1da3bfea38c9e3f0d1fe11aabd4a97eb2e9923659fc3'

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def stage(target):
    if sha(SOURCE)!=SOURCE_SHA:
        raise ValueError('hk_apple80211.c differs from the reviewed 20F66 bridge source')
    dev=target/'bsd/dev';dev.mkdir(parents=True,exist_ok=True)
    sys=target/'bsd/sys';sys.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(SOURCE,dev/'hk_apple80211.c')
    shutil.copyfile(HERE/'apple80211_provider.h',sys/'hk_awdl_provider.h')
    shutil.copyfile(HERE/'apple80211_control.inc',dev/'apple80211_control.inc')
    if os.environ.get('AWDL_NATIVE_ADDRESS') == '1':
        shutil.copyfile(HERE/'native_address.inc',dev/'native_address.inc')
        control = dev/'apple80211_control.inc'
        control.write_text('#define HK_AWDL_NATIVE_ADDRESS 1\n'+control.read_text())
    subprocess.run(['patch','--batch','-p1','-i',str(HERE/'apple80211-control.patch')],cwd=target,check=True)

def main():
    out=ROOT/'out/awdl-kernel/control';out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        build=Path(tmp);stage(build)
        command='''source /build/xnu-env.sh
python3 - "$1" <<'INNER'
import pathlib, shlex, subprocess, sys
base=pathlib.Path('/build/xnu-src/nf-tree/xnu-8796.121.2')
work=base/'BUILD_NF/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT'
stage=pathlib.Path(sys.argv[1])
flags=shlex.split((work/'.CFLAGS').read_bytes().strip(b'\\0\\r\\n ').decode().replace('((1024 * 256) / MCLBYTES)','((1024*256)/MCLBYTES)'))
flags[1:1]=['-I', str(stage/'bsd'), '-I', str(base/'bsd/dev')]
flags += ['-ffile-prefix-map='+str(stage)+'=/hk-awdl-control-stage']
subprocess.run(flags+['-c', str(stage/'bsd/dev/hk_apple80211.c'), '-o',str(stage/'hk_apple80211.o')],cwd=work,check=True)
INNER'''
        remote='/build/'+str(build.relative_to(ROOT))
        subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build','oneplus8-build','bash','-ec',command,'build',remote],check=True)
        obj=build/'hk_apple80211.o'
        target=out/f'hk_apple80211-{sha(obj)}.o'
        shutil.copyfile(obj,target)
        symbols=subprocess.check_output(['nm','-g',str(target)],text=True)
        for name in ['_hk_apple80211_ioctl','_hk_awdl_provider_register','_hk_awdl_provider_set_ready','_hk_awdl_provider_unregister']:
            if ' T '+name not in symbols:
                raise ValueError(f'missing control symbol {name}')
        record=dict(object=str(target),sha256=sha(target),control_source=SOURCE_SHA,
            patch_sha256=sha(HERE/'apple80211-control.patch'),api_sha256=sha(HERE/'apple80211_provider.h'),
            implementation_sha256=sha(HERE/'apple80211_control.inc'),provider_registered=False)
        (out/'build.json').write_text(json.dumps(record,indent=2)+'\n')
        print(json.dumps(record,indent=2))
if __name__=='__main__':main()
