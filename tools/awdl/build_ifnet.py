#!/usr/bin/env python3
"""Compile and check the unbound network adapter in a private NF XNU stage."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from build_control import HERE, ROOT, sha, stage as stage_control

def stage(path):
    stage_control(path)
    registry=path/'iokit/Kernel'
    registry.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(HERE/'async_events.inc',registry/'async_events.inc')
    original=ROOT/'xnu/files/iokit/Kernel/HKWLAN.cpp'
    shutil.copyfile(original,registry/original.name)
    shutil.copyfile(HERE/'ifnet_registry.inc',registry/'ifnet_registry.inc')
    subprocess.run(['patch','--batch','-p1','-i',str(HERE/'ifnet-registry.patch')],cwd=path,check=True)
    shutil.copyfile(HERE/'ifnet_adapter.c',path/'bsd/dev/hk_awdl_ifnet.c')
    for unit in ('bonjour_store.inc','bonjour_offload.h','native_query.h'):
        shutil.copyfile(HERE/unit,path/'bsd/dev'/unit)
    shutil.copyfile(HERE/'ifnet_peer.inc',path/'bsd/dev/ifnet_peer.inc')
    shutil.copyfile(HERE/'ifnet_transport.h',path/'bsd/sys/hk_awdl_transport.h')

def main(out=None):
    out=out or ROOT/'out/awdl-kernel/ifnet'
    out.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        p=Path(tmp);stage(p)
        remote='/build/'+str(p.relative_to(ROOT))
        shell='''source /build/xnu-env.sh
python3 - "$1" <<'INNER'
from pathlib import Path
import sys,subprocess,shlex
p=Path(sys.argv[1]); nf=Path('/build/xnu-src/nf-tree/xnu-8796.121.2')
obj=nf/'BUILD_NF/obj/DEVELOPMENT_ARM64_BCM2837'
def flags(path):
    return shlex.split(path.read_bytes().strip(b'\\0\\r\\n ').decode().replace('((1024 * 256) / MCLBYTES)','((1024*256)/MCLBYTES)'))
for subsystem, file, product in [('bsd','bsd/dev/hk_apple80211.c','hk_apple80211.o'),('bsd','bsd/dev/hk_awdl_ifnet.c','hk_awdl_ifnet.o'),('iokit','iokit/Kernel/HKWLAN.cpp','HKWLAN.cpo')]:
    directory=obj/subsystem/'DEVELOPMENT'
    if subsystem=='iokit':
        cc=flags(directory/'.CXXFLAGS')
        cc[1:1]=['-I',str(p/'bsd'),'-I',str(obj/'bsd'),'-I',str(nf/'iokit/Kernel')]
        cc+=flags(directory/'.CFLAGS')[1:]
    else:
        cc=flags(directory/'.CFLAGS');cc[1:1]=['-I',str(p/'bsd'),'-I',str(nf/'bsd/dev')]
    cc+=['-ffile-prefix-map='+str(p)+'=/hk-awdl-ifnet-stage']
    subprocess.run(cc+['-c',str(p/file),'-o',str(p/product)],cwd=directory,check=True)
INNER'''
        subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build','oneplus8-build','bash','-ec',shell,'build',remote],check=True)
        hashes={};defined=set();used=set()
        for n in ('hk_apple80211.o','hk_awdl_ifnet.o','HKWLAN.cpo'):
            object=p/n;hashes[n]=sha(object)
            for line in subprocess.check_output(['nm','-g',str(object)],text=True).splitlines():
                part=line.split()
                if len(part)<2 or not part[-1].startswith('_hk_awdl_'):continue
                (used if part[-2]=='U' else defined).add(part[-1])
        if used-defined:raise ValueError('unresolved adapter symbols: '+str(used-defined))
        generation=hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()[:16]
        dest=out/generation;dest.mkdir(exist_ok=True)
        for n in hashes:shutil.copyfile(p/n,dest/n)
        sources={str(path.relative_to(p)):sha(path) for directory in ('bsd','iokit')
                 for path in sorted((p/directory).rglob('*')) if path.is_file()}
        record=dict(objects=str(dest),sha256=hashes,sources=sources,backend_registered=False,hardware_verified=False)
        (out/'build.json').write_text(json.dumps(record,indent=2)+'\n')
        print(json.dumps(record,indent=2))
        return record
if __name__=='__main__':main()
