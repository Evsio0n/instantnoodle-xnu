"""Complete local peer-port and BSSID bookkeeping on the private #423 link."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='059e145f1c194ef69bc0303430cbab188e28f2df45b2374119563770a3e6817a'

def build():
    pin=json.loads((OUT/'data-open-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #423')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-port-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    vp=work+'src/ath11k/mac.c'
    code=read(vp)
    code=once(code,'if (enable) rc=ath11k_wmi_vdev_up(ar,av->vdev_id,0,vif->bss_conf.bssid,NULL,0,0);',
        'if (enable) { av->aid=vif->cfg.aid; ether_addr_copy(av->bssid,vif->bss_conf.bssid); rc=ath11k_wmi_vdev_up(ar,av->vdev_id,av->aid,av->bssid,NULL,0,0); }')
    write(vp,code)
    code=read(src)
    where='if (!up) {\n\t\tuint8_t *packet;'
    code=once(code,where,'''int port=-ECANCELED;
	if (!up) {
		mutex_lock(&w->mtx);
		port=ops->sta_state(hw,vif,peer,IEEE80211_STA_ASSOC,IEEE80211_STA_AUTHORIZED);
		mutex_unlock(&w->mtx);
	}
	lx_tell("awdl-echo: post_up_local_port_submit=%d remote_assoc_seen=0\\n",port);
	if (!port) {
		uint8_t *packet;''')
    # Linux sets the peer's local flag before WMI submit; unwind even on an error.
    where='down=lx_awdl_data_link(hw,vif,peer,0);'
    code=once(code,where,'''int unwind=ops->sta_state(hw,vif,peer,IEEE80211_STA_AUTHORIZED,IEEE80211_STA_ASSOC);
		lx_tell("awdl-echo: local_port_released=%u\\n",!unwind);
		'''+where)
    code+='\nint lx_awdl_descriptor_role(struct ieee80211_hw *hw,struct ieee80211_vif *v) { struct lx_radio *r=radio_of(hw); if (r->roc.choice!=48) return 0; if (v==r->vif) return 1; return EG(echo.active) && hw==echo.hw && lx_awdl_roc_vdev(v)==echo.vdev?2:0; }\n'
    write(src,code)
    dp=work+'src/ath11k/dp_tx.c'; data=read(dp); anchor='int ath11k_dp_tx(struct ath11k *ar,'
    data=once(data,anchor,'extern int lx_awdl_descriptor_role(struct ieee80211_hw *,struct ieee80211_vif *);\n'+(HERE/'data_profile.inc').read_text()+'\n'+anchor)
    anchor='ti.tid = ath11k_dp_tx_get_tid(skb);'
    data=once(data,anchor,anchor+'\n\tlx_awdl_data_profile(ar,arvif,arsta,&ti);')
    write(dp,data)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_mac.o',work+'obj/ath11k_dp_tx.o',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'data-port-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-data-port.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'data-port-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
