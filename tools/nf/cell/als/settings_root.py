#!/usr/bin/env python3
"""Add the read-only mobile Settings observer to a supplied full-desktop RAM root."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
K = HERE.parents[3]
sys.path.insert(0, str(K / 'tools/apfs'))
import apfs

JOB = 'LaunchDaemons/com.hackios.front-light-observe.plist'
BIN = 'bin/front_light_observe'

def sha(path):
    with path.open('rb') as source:
        return hashlib.file_digest(source, 'sha256').hexdigest()

def contents(image, dest=None):
    result = {}
    container = apfs.Container(str(image))
    try:
        vol = apfs.pick_volume(container, None)
        for name, _, fid, kind in apfs.walk(vol, apfs.ROOT_DIR_INO_NUM):
            rel = name.lstrip('/')
            inode = vol.inode(fid)
            path = dest / rel if dest else None
            data = b''
            if kind == 4:
                if path:
                    path.mkdir(exist_ok=True)
            elif inode.is_link:
                data = inode.symlink_target_bytes()
                if path:
                    path.symlink_to(os.fsdecode(data))
            elif inode.is_reg:
                data = inode.contents()
                if path:
                    path.write_bytes(data)
            else:
                raise ValueError(f'unsupported object in input: {rel}')
            if path:
                os.chmod(path, inode.mode & 0o7777, follow_symlinks=False)
            result[rel] = (inode.mode, inode.owner, inode.group, hashlib.sha256(data).hexdigest())
        return result
    finally:
        container.close()

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('base', type=Path, nargs='?', default=K / 'out/cell-als-candidate-root.apfs')
    ap.add_argument('--refresh', action='store_true', help='Replace the existing diagnostic observer only')
    args = ap.parse_args()
    container = 'oneplus8-build'
    remote = subprocess.check_output(['docker','exec',container,'mktemp','-d','/tmp/front-light-observe.XXXXXXXX'],text=True).strip()
    os.umask(0o077)
    try:
        command = (
            'source /build/xnu-env.sh\n'
            'xnu-clang -arch arm64 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk '
            '-fobjc-arc -Wall -Wextra -Werror -O2 -framework Foundation -framework IOKit '
            'tools/nf/cell/als/settings_observe.m -o "$1/observe"'
        )
        subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build',container,'bash','-ec',command,'bash',remote],check=True)
        with tempfile.TemporaryDirectory(prefix='front-light-root-',dir=K/'out') as tmp:
            stage = Path(tmp) / 'root'
            stage.mkdir(mode=0o755)
            original = contents(args.base, stage)
            if args.refresh:
                assert JOB in original and BIN in original, 'observer is not present in this root'
            else:
                assert JOB not in original and BIN not in original
            path = stage / BIN
            subprocess.run(['docker','cp',container+':'+remote+'/observe',str(path)],check=True)
            path.chmod(0o755)
            job = stage / JOB
            job.write_bytes(plistlib.dumps(dict(Label='com.hackios.front-light-observe',
                ProgramArguments=['/hackios/'+BIN],UserName='mobile',RunAtLoad=True,
                StandardOutPath='/dev/console', StandardErrorPath='/dev/console')))
            job.chmod(0o644)
            stage.chmod(0o755)
            image = Path(tmp) / 'settings.apfs'
            subprocess.run([sys.executable,str(K/'tools/apfs/mkapfs.py'),'-z','--plain','fw','-o',str(image),'hackios='+str(stage)],check=True,stdout=subprocess.DEVNULL)
            readback = contents(image)
            assert set(readback) - original.keys() == {JOB, BIN} - original.keys()
            mutable = {JOB, BIN} if args.refresh else set()
            assert all(readback.get(k) == v for k,v in original.items() if k not in mutable), 'original root changed'
            assert readback[BIN][-1] == sha(path) and readback[JOB][-1] == sha(job)
            digest = sha(image)
            target = K / 'out' / f'cell-als-settings-{digest}.apfs'
            image.chmod(0o400)
            image.replace(target)
            output = dict(base=str(args.base.resolve()),base_sha256=sha(args.base),root=str(target),
                          root_sha256=digest,observer_sha256=sha(path),original_entries_verified=len(original)-len(mutable),
                          refreshed_entries=sorted(mutable))
            (K/'out/cell-als-settings-root.json').write_text(json.dumps(output,indent=2)+'\n')
            print(json.dumps(output,indent=2))
    finally:
        subprocess.run(['docker','exec',container,'rm','-rf','--',remote],check=True)
if __name__ == '__main__':
    main()
