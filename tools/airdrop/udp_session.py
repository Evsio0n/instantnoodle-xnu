"""Create or reopen a private per-trial UDP test token without starting a listener."""
import argparse
import hashlib
import os
from pathlib import Path
from peer_match import actual_mac_peer,frozen_peer

def prepare(directory,snapshot):
    os.umask(0o077)
    identity=frozen_peer(snapshot)
    if actual_mac_peer()!=identity:raise ValueError('Mac identity changed since snapshot')
    directory.mkdir(parents=True,exist_ok=True,mode=0o700)
    if directory.is_symlink() or directory.stat().st_mode&0o077:raise ValueError('session directory must be private')
    target=directory/'udp.token'
    if not target.exists():
        with target.open('xb') as f:f.write(os.urandom(16))
        target.chmod(0o600)
    if target.is_symlink() or not target.is_file() or target.stat().st_mode&0o077 or target.stat().st_size!=16:
        raise ValueError('invalid private session token')
    token=target.read_bytes()
    if not any(token):raise ValueError('empty session token')
    print('udp-session: snapshot_matches_current_mac=1 request_bytes=34 reply_bytes=34 request_sha256='+
          hashlib.sha256(b'HKAWDL-UDP-TEST1'+bytes([1,1])+token).hexdigest()+' reply_sha256='+
          hashlib.sha256(b'HKAWDL-UDP-TEST1'+bytes([1,2])+token).hexdigest())
    return target

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--directory',required=True,type=Path);p.add_argument('--snapshot',required=True,type=Path)
    a=p.parse_args();prepare(a.directory,a.snapshot)
