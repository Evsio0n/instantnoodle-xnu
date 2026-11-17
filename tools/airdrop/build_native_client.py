"""Build the on-phone native IPv6 diagnostic; optionally package the local Mac's actual awdl0 LLA."""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
from peer_match import frozen_peer

HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
OUT=ROOT/'out/airdrop-ui/live-abi'
def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

def pack(target,files):
    def fields(m):
        m.uid=m.gid=0;m.uname=m.gname='';m.mtime=0
        return m
    with tarfile.open(target,'w',format=tarfile.USTAR_FORMAT) as t:
        for p in files:t.add(p,arcname=p.name,filter=fields)
    target.chmod(0o600)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--include-local-mac',action='store_true')
    parser.add_argument('--output-dir',type=Path,default=OUT)
    parser.add_argument('--snapshot',type=Path,help='pack the peer from an existing, frozen private snapshot')
    parser.add_argument('--udp-session',type=Path,help='enable UDP and include the existing private session token')
    parser.add_argument('--include-launcher',action='store_true',help='include the native system unlock/foreground requester')
    args=parser.parse_args()
    if args.snapshot and args.include_local_mac:parser.error('use only one target source')
    if args.udp_session and not args.snapshot:parser.error('UDP requires a frozen snapshot')
    os.umask(0o077)
    out=args.output_dir.resolve()
    out.mkdir(parents=True,exist_ok=True)
    app=out/'airdrop_native_client'
    command=('source /build/xnu-env.sh\n'
             'xnu-clang -arch arm64 -miphoneos-version-min=16.5 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk '
             '-Itools/awdl -Wall -Wextra -Werror -O2 tools/airdrop/native_client.c tools/airdrop/native_echo.c '
             '-o "$1"')
    if args.udp_session:
        command=command.replace('tools/airdrop/native_echo.c',
            '-DHK_AWDL_UDP=1 tools/airdrop/native_echo.c tools/airdrop/native_udp.c')
    subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build','oneplus8-build',
                    'bash','-ec',command,'build',str(app.relative_to(ROOT))],check=True)
    app.chmod(0o755)
    files=[app]
    if args.include_launcher:
        launcher=out/'airdrop_frontboard_launch'
        build_launcher=('source /build/xnu-env.sh\n'
            'xnu-clang -arch arm64 -miphoneos-version-min=16.5 -isysroot /build/local-theos/sdks/iPhoneOS16.5.sdk '
            '-fobjc-arc -fblocks -framework Foundation -Wall -Wextra -Werror -O2 tools/airdrop/frontboard_launch.m -o "$1"')
        subprocess.run(['docker','exec','-e','THEOS=/build/local-theos','-w','/build','oneplus8-build',
            'bash','-ec',build_launcher,'build',str(launcher.relative_to(ROOT))],check=True)
        launcher.chmod(0o755);files.append(launcher)
    tar=out/'client.tar';pack(tar,files)
    receipt=dict(binary=str(app),binary_sha256=sha(app),tar=str(tar),tar_sha256=sha(tar),hardware_verified=False)
    if args.include_launcher:receipt.update(launcher=str(launcher),launcher_sha256=sha(launcher))
    if args.snapshot:
        mac,ip=frozen_peer(args.snapshot)
        inputs=[app,args.snapshot/'mac-awdl.peer']
        if args.udp_session:
            token=args.udp_session/'udp.token'
            if token.is_symlink() or not token.is_file() or token.stat().st_mode&0o077 or token.stat().st_size!=16:
                raise ValueError('private 16-byte test token required')
            inputs.append(token)
            receipt.update(udp_port=61743,transport='udp')
        private=out/'client-and-peer.tar';pack(private,inputs)
        receipt.update(private_tar=str(private),private_tar_sha256=sha(private),peer_source='frozen snapshot',
                       snapshot_sha256=hashlib.sha256(mac+ip).hexdigest(),peer_mode='0600')
    if args.include_local_mac:
        text=subprocess.check_output(['/sbin/ifconfig','awdl0'],text=True,stderr=subprocess.DEVNULL)
        found={v for v in re.findall(r'\binet6\s+([0-9a-fA-F:]+)%awdl0\b',text) if ipaddress.IPv6Address(v).is_link_local}
        if len(found)!=1:raise SystemExit('one unambiguous Mac awdl0 link-local address is required')
        address=out/'mac-awdl.peer'
        address.write_text(next(iter(found))+'\n');address.chmod(0o600)
        private=out/'client-and-peer.tar';pack(private,[app,address])
        receipt.update(private_tar=str(private),private_tar_sha256=sha(private),peer_source='local Mac awdl0 link-local',peer_mode='0600')
    (out/'build.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print(json.dumps(receipt,indent=2))

if __name__=='__main__':main()
