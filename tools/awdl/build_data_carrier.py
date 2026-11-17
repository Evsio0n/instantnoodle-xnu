"""Use the mapped peer as the local STA carrier BSSID. Keep AWDL Addr3 on air."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='baad3d07a1bf4a1e21e924dcb4230a2e52b2ea2b96e43b068be47f2c0e48d142'

def build():
    pin=json.loads((OUT/'data-port-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #424')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-carrier-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    vp=work+'src/ath11k/mac.c'
    code=read(vp)
    off=code.index('int lx_awdl_data_peer(')
    head,part=code[:off],code[off:]
    old='struct ath11k_peer *peer=ath11k_peer_find(ar->ab,av->vdev_id,sta->addr);'
    part=once(part,old,'struct ath11k_peer *peer=ath11k_peer_find(ar->ab,av->vdev_id,vif->bss_conf.bssid);')
    marker='if (!mapped) goto out;'
    part=once(part,marker,'''rcu_read_lock();
	bool by_bssid=ieee80211_find_sta(vif,vif->bss_conf.bssid)==sta;
	rcu_read_unlock();
	lx_awdl_carrier_receipt(mapped,by_bssid);
	if (!mapped || !by_bssid) goto out;''')
    write(vp,head+'extern void lx_awdl_carrier_receipt(int,int);\n'+part)
    code=read(src)
    code=once(code,'vif->bss_conf.bssid=awdl_network;','''/* Carrier BSS peer and wire AWDL network have distinct roles. */
	vif->bss_conf.bssid=awdl_target_mac;''')
    code=once(code,'static bool lx_awdl_echo_trial(','''static struct ieee80211_vif *lookup_vif;
static struct ieee80211_sta *lookup_peer;
static void *lookup_thread;
static struct ieee80211_sta *lx_awdl_carrier_find(struct ieee80211_vif *vif,const unsigned char *addr)
{
	/* The real STA association callback consumes the result synchronously
	 * under ar->conf_mutex and RCU. This scoped view is never shared with
	 * IRQ readers or a later VDEV instance. */
	if (EG(lookup_thread)!=lx_os_thread_self() || !addr || lookup_vif!=vif) return NULL;
	return lookup_peer && !memcmp(lookup_peer->addr,addr,6)?lookup_peer:NULL;
}
void lx_awdl_carrier_receipt(int htt,int lookup)
{
	lx_tell("awdl-echo: carrier_bssid_htt_peer=%d carrier_bssid_sta_lookup=%d\\n",htt,lookup);
}
static bool lx_awdl_echo_trial(''')
    code=once(code,'cfg=lx_awdl_data_peer(hw,vif,peer,&cfg_submit,&cfg_ack);','''lookup_vif=vif; lookup_peer=peer; ES(lookup_thread,lx_os_thread_self());
		cfg=lx_awdl_data_peer(hw,vif,peer,&cfg_submit,&cfg_ack);
		ES(lookup_thread,NULL); lookup_vif=NULL; lookup_peer=NULL;''')
    assert 'memcpy(packet+16,awdl_network,6);' in code
    write(src,code)
    glue=stage+'/wifi/lx/src/lx_80211.c'
    code=read(glue)
    marker='struct ieee80211_sta *\nlinuxkpi_ieee80211_find_sta('
    code=once(code,marker,'static struct ieee80211_sta *lx_awdl_carrier_find(struct ieee80211_vif *,const unsigned char *);\n'+marker)
    off=code.index('linuxkpi_ieee80211_find_sta(');end=code.index('\n}',off)
    fn=once(code[off:end],'struct lx_radio *radio = the_radio;',
        'struct lx_radio *radio = the_radio;\n\tif (radio && radio->roc.choice==48 && vif!=radio->vif) return lx_awdl_carrier_find(vif,address);')
    write(glue,code[:off]+fn+code[end:])
    dp=work+'src/ath11k/dp_tx.c'
    code=read(dp)
    code=once(code,'static void lx_awdl_data_profile(', 'extern void lx_tell(const char *,...);\nstatic void lx_awdl_data_profile(')
    code=once(code,'printk("awdl-tcl:','lx_tell("awdl-tcl:')
    write(dp,code)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_mac.o',work+'obj/ath11k_dp_tx.o',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'data-carrier-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-data-carrier.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'data-carrier-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
