"""Add local VDEV_UP/down and actual TCL consumption receipts to the #421 driver."""
from pathlib import Path
import json,os,subprocess
from build_roc import BUS,HERE,ROOT,OUT,once,read,sha,write
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='0f22ba34da78ac1d81c65ade518c1aedaa468d91603b96c72b6f8757d8d1e192'

def run(*args):
    subprocess.run(['docker','exec',BUS,*args],check=True)
def build():
    pin=json.loads((OUT/'data-peer-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #421')
    base=pin['container_tree']; mark=OUT/'data-up-stage.txt'
    if mark.exists(): job=mark.read_text().strip()
    else:
        job=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-link-XXXXXX'],text=True).strip()
        mark.write_text(job+'\n')
    if not job.startswith('/tmp/awdl-link-') or '/' in job.removeprefix('/tmp/awdl-link-'): raise ValueError('bad stage')
    run('cp','-a',base+'/.',job+'/')
    src=job+'/wifi/lx/src/'; work=job+'/xnu-deps/build/ath11k/'; vendor=work+'src/ath11k/'
    target=ROOT/'out/airdrop'
    ip=(target/'controlled-mac-ipv6.bin').read_bytes()
    peer=(target/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('peer must match verified current link-local')
    orig=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid linked fixture')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    if equip(orig)!=Path(pin['object']).read_bytes(): raise ValueError('staged baseline mismatch')
    defs='''struct ieee80211_sta;
int lx_awdl_data_link(struct ieee80211_hw *,struct ieee80211_vif *,struct ieee80211_sta *,int);
unsigned lx_awdl_data_tail(struct ieee80211_hw *,unsigned);
int lx_awdl_data_soft_separate(void);
void lx_awdl_data_slot(struct ieee80211_hw *,struct sk_buff *,unsigned,unsigned,unsigned);
'''
    hdr=once(read(src+'roc_state.h'),'#endif',defs+'#endif')
    write(src+'roc_state.h',hdr);write(vendor+'roc_state.h',hdr)
    write(vendor+'mac.c',read(vendor+'mac.c')+'\n'+(HERE/'data_link.inc').read_text())
    pci=read(src+'lx_pci.c')
    pci=once(pci,'static void\nsoft_wake(void)','''int lx_awdl_data_soft_separate(void)
{
	return soft.self && soft.self!=lx_os_thread_self();
}
static void
soft_wake(void)''')
    write(src+'lx_pci.c',pci)
    dp=read(vendor+'dp_tx.c')
    key='lx_awdl_echo_desc(ar->hw,skb,arvif->vdev_id,ti.ring_id,ti.desc_id,ti.encap_type==HAL_TCL_ENCAP_TYPE_NATIVE_WIFI);'
    dp=once(dp,key,key+'\n\tlx_awdl_data_slot(ar->hw,skb,tcl_ring->u.src_ring.cached_tp,tcl_ring->u.src_ring.hp,tcl_ring->ring_size);')
    dp+='''\nunsigned lx_awdl_data_tail(struct ieee80211_hw *hw,unsigned ring)
{
	struct ath11k *ar=hw->priv;
	struct ath11k_base *ab=ar->ab;
	if (ring>=ab->hw_params.hal_params->num_tx_rings) return ~0u;
	struct hal_srng *q=&ab->hal.srng_list[ab->dp.tx_ring[ring].tcl_data_ring.ring_id];
	spin_lock_bh(&q->lock);
	unsigned tail=q->initialized && q->u.src_ring.tp_addr ? READ_ONCE(*q->u.src_ring.tp_addr) : ~0u;
	spin_unlock_bh(&q->lock);
	return tail;
}
'''
    write(vendor+'dp_tx.c',dp)
    code=read(src+'data.inc')
    code=once(code,'unsigned acked, released,','unsigned tail_initial, head_goal, slots, eaten, timedout;\n\tunsigned acked, released,')
    key='void lx_awdl_echo_wbm('
    code=once(code,key,'''void lx_awdl_data_slot(struct ieee80211_hw *hw,struct sk_buff *skb,unsigned tail,unsigned end,unsigned size)
{
	if (!EG(echo.active) || hw!=echo.hw || skb!=EG(echo.msdu) || !EG(echo.bound)) return;
	ES(echo.tail_initial,tail); ES(echo.head_goal,end); ES(echo.slots,size);
}
static void lx_awdl_data_poll(struct ieee80211_hw *hw)
{
	unsigned size=EG(echo.slots);
	if (!size || EG(echo.eaten)) return;
	unsigned first=EG(echo.tail_initial), last=EG(echo.head_goal);
	unsigned now=lx_awdl_data_tail(hw,EG(echo.ring));
	if (now>=size || first>=size || last>=size || last==first) return;
	unsigned progress=(now+size-first)%size, needed=(last+size-first)%size;
	if (progress>=needed) ES(echo.eaten,1);
}
'''+key)
    code=once(code,'ES(echo.acked,acked); EA(echo.wbm);','''ES(echo.acked,acked); EA(echo.wbm);
	if (EG(echo.timedout)) lx_tell("awdl-echo: late_wbm=1 source=%u reason=%u tries=%u tqm_ack=%u\\n",src,reason,attempts,acked);''')
    code=once(code,'ES(echo.local_drop,local_drop); ES(echo.released,1);','''ES(echo.local_drop,local_drop); ES(echo.released,1);
	if (EG(echo.timedout)) lx_tell("awdl-echo: late_release=1 local_free_path=%u\\n",local_drop);''')
    code=once(code,'if (!c && EG(echo.mapped)==1 && EG(echo.start)==1 && !EG(echo.start_status)) {','''int up=-ECANCELED, down=-ECANCELED;
	bool soft=lx_awdl_data_soft_separate();
	if (!c && EG(echo.mapped)==1 && EG(echo.start)==1 && !EG(echo.start_status) && soft) {
		mutex_lock(&w->mtx);
		up=lx_awdl_data_link(hw,vif,peer,1);
		mutex_unlock(&w->mtx);
	}
	lx_tell("awdl-echo: vdev_up_submit=%d independent_napi=%u air_association=0\\n",up,soft);
	if (!up) {''')
    code=once(code,'lx_os_nanoseconds()<until) lx_pause_ms(10);\n\tES(echo.receive,0);','''lx_os_nanoseconds()<until) { lx_awdl_data_poll(hw); lx_pause_ms(10); }
	lx_awdl_data_poll(hw);
	ES(echo.receive,0);
	ES(echo.timedout,EG(echo.submitted) && !EG(echo.released));
	lx_tell("awdl-echo: tcl_tail_passed_slot=%u wbm_before_down=%u released_before_down=%u\\n",
	    EG(echo.eaten),EG(echo.wbm),EG(echo.released));
	if (!up) {
		mutex_lock(&w->mtx);
		down=lx_awdl_data_link(hw,vif,peer,0);
		mutex_unlock(&w->mtx);
		lx_tell("awdl-echo: vdev_down_submit=%d timeout=%u\\n",down,EG(echo.timedout));
		if (!down && EG(echo.timedout)) {
			until=lx_os_nanoseconds()+1200000000ull;
			while (!EG(echo.released) && lx_os_nanoseconds()<until) { lx_awdl_data_poll(hw); lx_pause_ms(10); }
		}
	}''')
    code=once(code,'if (EG(echo.submitted) && !EG(echo.released)) {','if ((!up && down) || (EG(echo.submitted) && !EG(echo.released))) {')
    write(src+'data.inc',code)
    run('rm',*[work+'obj/'+name+'.o' for name in ['lx_lx_pci','lx_lx_80211','ath11k_dp_tx','ath11k_mac']])
    with (OUT/'data-up-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',job+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-data-up.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'data-up-build.json').write_text(json.dumps({'object':str(dest),'sha256':sha(dest),'parent':PRIOR,
        'container_tree':job,'builder':sha(Path(__file__)),'vdev_ops':sha(HERE/'data_link.inc')},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
