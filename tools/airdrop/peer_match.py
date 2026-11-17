"""Compare the operator's local Mac, packed test root and linked AWDL peer without revealing addresses."""
import argparse
import hashlib
import ipaddress
import os
from pathlib import Path
import plistlib
import re
import struct
import subprocess
import sys
import tempfile
import shutil

K=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(K/'tools/apfs'))
import apfs

def actual_mac_peer():
    link=subprocess.check_output(['/sbin/ifconfig','awdl0'],text=True,stderr=subprocess.DEVNULL)
    ips={ipaddress.IPv6Address(x).packed for x in re.findall(r'\binet6\s+([0-9a-fA-F:]+)%awdl0\b',link)}
    ips={a for a in ips if a[:8]==bytes.fromhex('fe80000000000000')}
    tree=plistlib.loads(subprocess.check_output(['/usr/sbin/ioreg','-a','-r','-c','AppleBCMWLANProximityInterface','-d','1']))
    macs={node['IOMACAddress'] for node in tree if (node.get('BSD Name')=='awdl0' or node.get('IOInterfaceName')=='awdl0')
          and isinstance(node.get('IOMACAddress'),bytes) and len(node['IOMACAddress'])==6}
    if len(ips)!=1 or len(macs)!=1:raise ValueError('ambiguous or absent Mac AWDL interface/address')
    ip=next(iter(ips));mac=next(iter(macs))
    iid=bytes([mac[0]^2])+mac[1:3]+b'\xff\xfe'+mac[3:]
    if not any(mac) or mac[0]&1 or ip[8:]!=iid:raise ValueError('Mac link-local and native IORegistry MAC do not agree')
    return mac,ip

def capture(target):
    mac,ip=actual_mac_peer()
    target.parent.mkdir(mode=0o700,parents=True,exist_ok=True)
    fields={'controlled-mac-address.bin':mac,'controlled-mac-ipv6.bin':ip,
            'mac-awdl.peer':(str(ipaddress.IPv6Address(ip))+'\n').encode()}
    if not target.exists():
        stage=Path(tempfile.mkdtemp(prefix='.peer-',dir=target.parent))
        try:
            for name,raw in fields.items():
                p=stage/name
                with p.open('xb') as stream:stream.write(raw)
                p.chmod(0o600)
            try:stage.rename(target)
            except OSError:
                if not target.is_dir():raise
        finally:
            if stage.exists():shutil.rmtree(stage)
    if target.is_symlink() or target.stat().st_mode&0o077 or frozen_peer(target)!=(mac,ip):
        raise ValueError('snapshot path already contains different or nonprivate inputs')
    print('mac-peer: unique=1 native_ioreg_eui64_matches=1 private_snapshot=1 snapshot_sha256='+hashlib.sha256(mac+ip).hexdigest())

def frozen_peer(folder):
    names=('controlled-mac-address.bin','controlled-mac-ipv6.bin','mac-awdl.peer')
    for name in names:
        f=folder/name
        if f.is_symlink() or not f.is_file() or f.stat().st_mode&0o077:
            raise ValueError('expected regular private snapshot files')
    mac,ip,text=[(folder/n).read_bytes() for n in names]
    if len(mac)!=6 or not any(mac) or mac[0]&1 or len(ip)!=16 or ip[:8]!=bytes.fromhex('fe80000000000000'):
        raise ValueError('invalid frozen peer')
    if ip[8:]!=bytes([mac[0]^2])+mac[1:3]+b'\xff\xfe'+mac[3:]:
        raise ValueError('snapshot L2 and IPv6 do not match')
    if text!=(str(ipaddress.IPv6Address(ip))+'\n').encode():
        raise ValueError('snapshot file for the native client does not match')
    return mac,ip

def symbol_bytes(path):
    names={'_awdl_target_mac':6,'_awdl_target_ip':16}
    raw=path.read_bytes()
    if len(raw)<32 or struct.unpack_from('<I',raw)[0]!=0xfeedfacf:raise ValueError('expected ARM64 Mach-O')
    sections=[];offset=32
    for i in range(struct.unpack_from('<I',raw,16)[0]):
        cmd,size=struct.unpack_from('<II',raw,offset)
        if size<8 or offset+size>len(raw):raise ValueError('invalid load command')
        if cmd==0x19:
            vm,_,file,size_on_disk=struct.unpack_from('<QQQQ',raw,offset+24)
            sections.append((vm,file,size_on_disk))
        offset+=size
    symbols={}
    for l in subprocess.check_output(['nm','-an',str(path)],text=True).splitlines():
        parts=l.split()
        if len(parts)!=3 or parts[2] not in names or not re.fullmatch('[0-9a-fA-F]+',parts[0]):continue
        addr=int(parts[0],16);name=parts[2];length=names[name]
        matches=[file+addr-vm for vm,file,n in sections if vm<=addr and addr+length<=vm+n]
        if len(matches)!=1 or name in symbols:raise ValueError('ambiguous controlled endpoint symbol')
        pos=matches[0];symbols[name]=raw[pos:pos+length]
    if set(symbols)!=set(names):raise ValueError('controlled endpoint symbols absent')
    return symbols['_awdl_target_mac'],symbols['_awdl_target_ip']

def root_ipv6(path):
    c=apfs.Container(str(path))
    try:
        v=apfs.pick_volume(c,None);r=v.lookup('/share/airdrop/mac-awdl.peer')
        if not r:raise ValueError('private client peer is absent')
        f=v.inode(r[0])
        if not f.is_reg or f.mode&0o777!=0o600 or f.owner or f.group:raise ValueError('unexpected peer mode/owner')
        return ipaddress.IPv6Address(f.contents().decode().strip()).packed
    finally:c.close()

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--capture',type=Path)
    p.add_argument('--snapshot',type=Path,default=K/'out/airdrop-ui/native-current-peer')
    p.add_argument('--root',type=Path);p.add_argument('--radio',type=Path);p.add_argument('--kernel',type=Path)
    a=p.parse_args();os.umask(0o077)
    if a.capture:
        if a.root or a.radio or a.kernel:p.error('capture and comparison are separate')
        capture(a.capture);return
    if not (a.root and a.radio and a.kernel):p.error('compare requires --radio --kernel --root')
    frozen=frozen_peer(a.snapshot);current=actual_mac_peer();r=symbol_bytes(a.radio);k=symbol_bytes(a.kernel);root=root_ipv6(a.root)
    flags={'snapshot_matches_current_mac':frozen==current,'radio_matches_snapshot':r==frozen,
           'radio_kernel_match':r==k,'root_matches_radio':root==r[1],
           'root_matches_current_mac_ipv6':root==current[1],'radio_matches_current_mac_l2_l3':r==current}
    print('mac-peer: '+' '.join(f'{key}={int(value)}' for key,value in flags.items()))
    if not all(flags.values()):sys.exit(1)

if __name__=='__main__':main()
