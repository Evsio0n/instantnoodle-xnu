"""Only copy the frozen Mac target file into an existing native-client RAM root."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from native_root import K, contents, sha
from peer_match import frozen_peer

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--base',type=Path,required=True)
    p.add_argument('--snapshot',type=Path,required=True)
    p.add_argument('--output-dir',type=Path,required=True)
    a=p.parse_args()
    os.umask(0o077)
    mac,ip=frozen_peer(a.snapshot)
    target_bytes=(a.snapshot/'mac-awdl.peer').read_bytes()
    out=a.output_dir.resolve()
    out.mkdir(mode=0o700,parents=True,exist_ok=True)
    rel='share/airdrop/mac-awdl.peer'
    base=a.base.resolve();before=contents(base);old_hash=sha(base)
    if rel not in before or before[rel][:3]!=(0o100600,0,0):
        raise ValueError('base target must already be a private root-owned plain file')
    wanted=hashlib.sha256(target_bytes).hexdigest()
    delta=before[rel][-1]!=wanted
    final=base
    if delta:
        with tempfile.TemporaryDirectory(dir=out) as td:
            folder=Path(td);tree=folder/'root';tree.mkdir(mode=0o755)
            if contents(base,tree)!=before:raise ValueError('base changed while unpacking')
            (tree/rel).write_bytes(target_bytes);tree.chmod(0o755)
            image=folder/'image.apfs'
            subprocess.run([sys.executable,str(K/'tools/apfs/mkapfs.py'),'-z','--plain','fw','-o',str(image),'hackios='+str(tree)],check=True,stdout=subprocess.DEVNULL)
            expected=dict(before);expected[rel]=(*before[rel][:3],wanted)
            if contents(image)!=expected:raise ValueError('unrelated root content changed')
            digest=sha(image);final=out/f'native-root-{digest}.apfs';image.chmod(0o400);image.replace(final)
    data=dict(base=str(base),base_sha256=old_hash,root=str(final),root_sha256=sha(final),
              snapshot_sha256=hashlib.sha256(mac+ip).hexdigest(),modified=[rel] if delta else [],
              verified_entries=len(before),client_sha256=before['bin/airdrop_native_client'][-1],
              peer_mode='0600',owner='root:wheel',mac_access=False)
    (out/'root.json').write_text(json.dumps(data,indent=2)+'\n')
    print(json.dumps(data,indent=2))

if __name__=='__main__':main()
