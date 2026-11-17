#!/usr/bin/env python3
"""Create a private RAM add-on for one native link-local echo to the configured Mac."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tarfile
import tempfile

K=Path(__file__).resolve().parents[2]
OUT=K/'out/airdrop-ui/live-abi'
BASE_SHA='55df720d2e49078434464c961cd3c52f5c7cfceeeb836669026a4a7d4d62aa69'
LIMIT=63*1024*1024
OVERHEAD=65536
sys.path.insert(0,str(K/'tools/nf/cell/als'))
from settings_root import contents,sha

def budget(root,kernel=None,expected=None):
    room=LIMIT-OVERHEAD-root.stat().st_size
    record=dict(payload_cap=LIMIT,overhead_bytes=OVERHEAD,max_kernel_bytes=room)
    if kernel:
        actual=sha(kernel)
        if actual!=expected:raise ValueError('unexpected kernel SHA')
        used=root.stat().st_size+kernel.stat().st_size+OVERHEAD
        if used>LIMIT:raise ValueError(f'kernel and root exceed payload capacity by {used-LIMIT} bytes')
        record.update(kernel=str(kernel.resolve()),kernel_sha256=actual,kernel_bytes=kernel.stat().st_size,
                      payload_bytes=used,margin_bytes=LIMIT-used)
    return record

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--kernel',type=Path)
    parser.add_argument('--client-receipt',type=Path,default=OUT/'build.json')
    parser.add_argument('--output-dir',type=Path,default=OUT)
    parser.add_argument('--kernel-sha')
    parser.add_argument('--interface-only',action='store_true',help='bootstrap the real interface without a target address or test packet')
    parser.add_argument('--test-app-receipt',type=Path,help='replace only the source-built test app package with this verified build')
    parser.add_argument('--check-budget',type=Path,help='read a previously built root and verify only its actual payload budget')
    args=parser.parse_args()
    if bool(args.kernel)!=bool(args.kernel_sha):parser.error('--kernel and --kernel-sha are a pair')
    if args.check_budget:
        if not args.kernel:parser.error('--check-budget requires the candidate kernel')
        print(json.dumps(budget(args.check_budget,args.kernel,args.kernel_sha),indent=2));return
    os.umask(0o077)
    base=K/'out/airdrop-ui'/f'airdrop-root-{BASE_SHA}.apfs'
    if sha(base)!=BASE_SHA:raise ValueError('unexpected base RAM root')
    out=args.output_dir.resolve()
    out.mkdir(parents=True,exist_ok=True)
    client=json.loads(args.client_receipt.read_text())
    package_key='tar' if args.interface_only else 'private_tar'
    private=Path(client[package_key])
    if sha(private)!=client[package_key+'_sha256']:raise ValueError('client package changed')
    paths={'airdrop_native_client':('bin/airdrop_native_client',0o755),'mac-awdl.peer':('share/airdrop/mac-awdl.peer',0o600)}
    if args.interface_only:
        paths.pop('mac-awdl.peer')
        if client.get('launcher'):
            paths['airdrop_frontboard_launch']=('bin/airdrop_frontboard_launch',0o755)
    if client.get('transport')=='udp':
        paths['udp.token']=('share/airdrop/udp.token',0o600)
    job='etc/rc.jobs'
    with tempfile.TemporaryDirectory(dir=out,prefix='native-root-') as tmp:
        stage=Path(tmp)/'root';stage.mkdir(mode=0o755)
        original=contents(base,stage)
        wanted={}
        app_job='share/airdrop/AirDropTest.tar'
        changed_app=False
        if args.test_app_receipt:
            app=json.loads(args.test_app_receipt.read_text());package=Path(app['tar'])
            if app.get('bundle_id')!='com.hackios.airdrop-test' or sha(package)!=app['tar_sha256'] or app_job not in original:
                raise ValueError('test app package identity changed')
            shutil.copyfile(package,stage/app_job);changed_app=True
        with tarfile.open(private,'r:') as package:
            if sorted(package.getnames())!=sorted(paths):raise ValueError('unexpected package members')
            for source,(name,mode) in paths.items():
                member=package.getmember(source)
                if name in original or not member.isfile() or member.mode!=mode:raise ValueError('member type/mode mismatch')
                if source=='mac-awdl.peer' and member.size>64:raise ValueError('invalid endpoint file length')
                data=package.extractfile(member).read()
                file=stage/name;file.write_bytes(data);file.chmod(mode);wanted[name]=hashlib.sha256(data).hexdigest()
        old=(stage/job).read_bytes()
        invoke='/hackios/bin/airdrop_native_client /hackios/share/airdrop/mac-awdl.peer'
        if args.interface_only:
            invoke='/hackios/bin/airdrop_native_client --interface-only'
        if client.get('transport')=='udp':
            invoke+=' --udp /hackios/share/airdrop/udp.token'
        new=old+(b'' if old.endswith(b'\n') else b'\n')+invoke.encode()+b'\n'
        (stage/job).write_bytes(new);stage.chmod(0o755)
        image=Path(tmp)/'image.apfs'
        subprocess.run([sys.executable,str(K/'tools/apfs/mkapfs.py'),'-z','--plain','fw','-o',str(image),'hackios='+str(stage)],
                       check=True,stdout=subprocess.DEVNULL)
        after=contents(image)
        if set(after)!=set(original)|set(wanted):raise ValueError('unexpected APFS file set')
        for name,before in original.items():
            final=(before[:3]+(hashlib.sha256(new).hexdigest(),)) if name==job else before
            if changed_app and name==app_job:
                final=before[:3]+(app['tar_sha256'],)
            if after[name]!=final:raise ValueError('existing RAM root changed: '+name)
        for name,mode in paths.values():
            node=after[name]
            if (node[0]&0o777)!=mode or node[1:3]!=(0,0) or node[-1]!=wanted[name]:
                raise ValueError('failed APFS readback '+name)
        digest=sha(image);target=out/f'native-root-{digest}.apfs'
        measurements=budget(image,args.kernel,args.kernel_sha)
        image.chmod(0o400);image.replace(target)
        receipt=dict(base_sha256=BASE_SHA,root=str(target),root_sha256=digest,root_bytes=target.stat().st_size,
            source_package_sha256=client[package_key+'_sha256'],native_client_sha256=client['binary_sha256'],
            existing_entries=len(original),unmodified_entries=len(original)-1-int(changed_app),modified_existing=[job]+([app_job] if changed_app else []),
            added_paths=[name for name,_mode in paths.values()],appended_command=invoke,**measurements)
        (out/('root-'+digest[:12]+'.json')).write_text(json.dumps(receipt,indent=2)+'\n')
        (out/'root.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print(json.dumps(receipt,indent=2))
if __name__=='__main__':main()
