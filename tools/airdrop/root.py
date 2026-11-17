#!/usr/bin/env python3
"""Build a separate full-desktop RAM image with the two AirDrop readers and sender package."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import subprocess
import sys
import tempfile

K=Path(__file__).resolve().parents[2]
OUT=K/'out/airdrop-ui'
BASE_SHA='8013c8a8bae8b77a7e688d5e0b731eba087c255a8eb8253065034e42ba30a1f3'
KERNEL_SHA='4ac0cdd3d3f85e6542104bffc9aea2bdc49713e4302c8458649442cf35d07876'
sys.path.insert(0,str(K/'tools/nf/cell/als'))
from settings_root import contents, sha

def main():
    os.umask(0o077)
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode',type=Path,default=OUT/'mode-v2/airdrop_mode')
    parser.add_argument('--sharing-log', action='store_true', help='enable OSLog stderr only for sharingd')
    parser.add_argument('--wifid-log', action='store_true', help='enable OSLog stderr only for wifid')
    parser.add_argument('--kernel', type=Path, help='explicit integration kernel to use for the payload budget')
    parser.add_argument('--kernel-sha', help='expected SHA-256 of the explicit kernel')
    args=parser.parse_args()
    if (args.kernel is None) != (args.kernel_sha is None):
        parser.error('--kernel and --kernel-sha must be supplied together')
    base=K/'out/trollstore-lite'/f'ram-root-{BASE_SHA}.apfs'
    kernel=args.kernel or K/'out/kernels'/f'cell-{KERNEL_SHA}.mach'
    kernel_sha=args.kernel_sha or KERNEL_SHA
    if sha(base)!=BASE_SHA or sha(kernel)!=kernel_sha:
        raise ValueError('unexpected base or kernel generation')
    sender=json.loads((OUT/'test-app/build.json').read_text())
    tar=Path(sender['tar'])
    if sha(tar)!=sender['tar_sha256'] or sender['bundle_id']!='com.hackios.airdrop-test':
        raise ValueError('sender artifact mismatch')
    additions={
        'bin/airdrop_check':(OUT/'airdrop_check',0o755),
        'bin/airdrop_mode':(args.mode,0o755),
        'bin/airdrop_capability':(OUT/'airdrop_capability',0o755),
        'share/airdrop/AirDropTest.tar':(tar,0o644),
    }
    job='etc/rc.jobs'
    with tempfile.TemporaryDirectory(prefix='ars-',dir=OUT) as tmp:
        root=Path(tmp)/'root';root.mkdir(mode=0o755)
        before=contents(base,root)
        assert 'bin/hkinstall' in before and job in before
        assert 'share/airdrop' not in before and not additions.keys() & before.keys()
        (root/'share/airdrop').mkdir(mode=0o755)
        (root/'share/airdrop').chmod(0o755)
        for name,(source,mode) in additions.items():
            dest=root/name;shutil.copyfile(source,dest);dest.chmod(mode)
        old=(root/job).read_bytes()
        # Ask the existing installer to stage only the owner-shipped sender.
        # Keep the real signature properties and continue to report its return status.
        line=('/hackios/bin/hkinstall alarm 30 unpack /hackios/share/airdrop/AirDropTest.tar '
              '/private/var/tmp/airdrop-test-bootstrap install /private/var/tmp/airdrop-test-bootstrap '
              'state com.hackios.airdrop-test\n')
        new=old+(b'' if old.endswith(b'\n') else b'\n')+line.encode()
        (root/job).write_bytes(new);root.chmod(0o755)
        edited={job: hashlib.sha256(new).hexdigest()}
        logs=[]
        for label, enabled in [('com.apple.sharingd', args.sharing_log), ('com.apple.wifid', args.wifid_log)]:
            if not enabled: continue
            rel=f'LaunchDaemons/{label}.plist'
            path=root/rel
            prior=path.read_bytes()
            state=plistlib.loads(prior)
            if state.get('Label')!=label: raise ValueError('unexpected daemon at '+rel)
            env=dict(state.get('EnvironmentVariables',{}))
            env.update(OS_ACTIVITY_DT_MODE='YES', OS_LOG_STDERR='YES')
            updated=dict(state,EnvironmentVariables=env)
            fmt=plistlib.FMT_BINARY if prior.startswith(b'bplist00') else plistlib.FMT_XML
            encoded=plistlib.dumps(updated,fmt=fmt,sort_keys=False)
            if plistlib.loads(encoded)!=updated: raise ValueError('plist serialization mismatch: '+rel)
            path.write_bytes(encoded)
            edited[rel]=hashlib.sha256(encoded).hexdigest()
            logs.append(label)
        image=Path(tmp)/'root.apfs'
        subprocess.run([sys.executable,str(K/'tools/apfs/mkapfs.py'),'-z','--plain','fw','-o',str(image),
                        'hackios='+str(root)],check=True,stdout=subprocess.DEVNULL)
        after=contents(image)
        assert set(after)-set(before)==set(additions)|{'share/airdrop'}
        assert not set(before)-set(after)
        assert all(after[k]==v for k,v in before.items() if k not in edited)
        for rel,digest in edited.items():
            assert after[rel][:3]==before[rel][:3] and after[rel][-1]==digest
        for name,(source,mode) in additions.items():
            assert after[name][-1]==sha(source) and (after[name][0]&0o777)==mode
        cap=63*1024*1024
        total=image.stat().st_size+kernel.stat().st_size+65536
        if total>cap:raise ValueError(f'payload {total} exceeds {cap}')
        digest=sha(image);target=OUT/f'airdrop-root-{digest}.apfs';image.chmod(0o400);image.replace(target)
        receipt=dict(root=str(target),root_sha256=digest,root_bytes=target.stat().st_size,kernel=str(kernel),
            kernel_sha256=kernel_sha,kernel_bytes=kernel.stat().st_size,base_sha256=BASE_SHA,
            existing_entries=len(before),existing_entries_identical=len(before)-len(edited),logging_jobs=logs,
            appended_job=line.rstrip(),added_entries=sorted(set(after)-set(before)),
            payload_budget_with_64KiB=total,margin_bytes=cap-total,sender=sender['cdhash'])
        # Preserve the manifests of earlier source/kernel generations.
        (OUT/f'root-{receipt["root_sha256"][:12]}-{kernel_sha[:12]}.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print(json.dumps(receipt,indent=2))

if __name__=='__main__':
    main()
