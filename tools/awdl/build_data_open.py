"""Restore ath11k's normal open-peer data policy on the private #422 link."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='1dbe9d2108e166cc6cf7edcc559be6b05e3094d45865c4bd19ecd70055db0d63'

def build():
    pin=json.loads((OUT/'data-peer-up-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #422')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-open-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    vp=work+'src/ath11k/mac.c'
    code=read(vp)
    start=code.index('/* The peer\'s identity was provisioned locally')
    end=code.index('arg.auth_flag=false;',start)+len('arg.auth_flag=false;')
    code=code[:start]+'''/* Keep ath11k_peer_assoc_h_basic's normal local open-link policy.
	 * This test is limited to the explicitly provisioned point-to-point
	 * peer. Protected contexts do not enter this path. */
	if (!arg.auth_flag || arg.safe_mode_enabled) goto out;'''+code[end:]
    write(vp,code)
    code=read(src)
    code=once(code,'independent_napi=%u air_association=0','independent_napi=%u local_data_permitted=%u remote_assoc_seen=0')
    code=once(code,'cfg,cfg_submit,cfg_ack,up,soft);','cfg,cfg_submit,cfg_ack,up,soft,!cfg && !up);')
    write(src,code)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_mac.o',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'data-open-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-data-open.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'data-open-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
