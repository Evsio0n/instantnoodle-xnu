"""Yield to independent real scans; remove only the direct VDEV after its leases drain."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='1ae41a6ac14cacd25e97c46cdbcf98ec719715b18e243128e6f964b3aef88657'

def build():
    pin=json.loads((OUT/'live-worker-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #431')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-scoped-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    d=stage+'/wifi/lx/src/'
    vendor=work+'src/ath11k/'
    hp=d+'roc_state.h';hdr=read(hp)
    hdr=once(hdr,'struct lx_awdl_roc_state {','''enum lx_awdl_scan_owner { LX_AWDL_SCAN_IDLE, LX_AWDL_SCAN_OTHER, LX_AWDL_SCAN_OTHER_ROC, LX_AWDL_SCAN_OTHER_SAME_VDEV, LX_AWDL_SCAN_OURS };
unsigned lx_awdl_request_token(struct ieee80211_hw *,struct ieee80211_vif *,unsigned);
int lx_awdl_scan_state(struct ieee80211_hw *,unsigned,unsigned);
int lx_awdl_retiring_secondary(struct ieee80211_hw *,struct ieee80211_vif *);
struct lx_awdl_roc_state {
	unsigned request_seq, scan_backoffs;
	void *request_thread;''')
    write(hp,hdr);write(vendor+'roc_state.h',hdr)
    mp=vendor+'mac.c'; code=read(mp)
    a=code.index('static int ath11k_mac_op_hw_scan(')
    code=code[:a]+'#include "roc_state.h"\n'+(HERE/'scan_ownership.inc').read_text()+'\n'+code[a:]
    marker='ar->scan.is_roc = false;\n\t\tar->scan.vdev_id = arvif->vdev_id;'
    code=once(code,marker,marker+'\n\t\tawdl_scan_acquired(ar,vif,0);')
    marker='ar->scan.roc_notify = true;'
    code=once(code,marker,marker+'\n\t\tawdl_scan_acquired(ar,vif,chan->center_freq);')
    a=code.index('static void ath11k_mac_op_remove_interface(');b=code.index('static void ath11k_mac_op_configure_filter',a)
    part=code[a:b]
    marker='if (arvif->vdev_type == WMI_VDEV_TYPE_STA)\n\t\tath11k_mac_11d_scan_stop(ar);'
    part=once(part,marker,'''if (arvif->vdev_type == WMI_VDEV_TYPE_STA)
		ath11k_mac_stop_11d_scoped(ar,lx_awdl_retiring_secondary(hw,vif)?(int)arvif->vdev_id:-1);''')
    code=code[:a]+part+code[b:]
    marker='void ath11k_mac_11d_scan_stop(struct ath11k *ar)'
    a=code.index(marker);b=code.index('\n}',a)
    part=code[a:b].replace(marker,'static void ath11k_mac_stop_11d_scoped(struct ath11k *ar,int required_vdev)')
    marker='mutex_lock(&ar->ab->vdev_id_11d_lock);'
    part=once(part,marker,marker+'\n\tif (required_vdev>=0 && ar->vdev_id_11d_scan!=(u32)required_vdev) { mutex_unlock(&ar->ab->vdev_id_11d_lock); return; }')
    code=code[:a]+part+'\n}\nvoid ath11k_mac_11d_scan_stop(struct ath11k *ar) { ath11k_mac_stop_11d_scoped(ar,-1); }'+code[b+2:]
    write(mp,code)
    rp=d+'roc.inc'; code=read(rp)
    marker='void\nlx_awdl_roc_wmi('
    code=once(code,marker,'''unsigned lx_awdl_request_token(struct ieee80211_hw *hw,struct ieee80211_vif *vif,unsigned freq)
{
	struct lx_radio *radio=radio_of(hw);struct lx_awdl_roc_state *r=&radio->roc;
	if (!radio->persistent_awdl || !ROC_GET(r->active) || freq!=r->target_mhz ||
	    r->vdev!=lx_awdl_roc_vdev(vif) || r->request!=1 || r->scan!=0xa000 ||
	    ROC_GET(r->request_thread)!=lx_os_thread_self()) return 0;
	return ROC_GET(r->request_seq);
}
int lx_awdl_retiring_secondary(struct ieee80211_hw *hw,struct ieee80211_vif *vif)
{
	struct lx_radio *radio=radio_of(hw);
	return radio->persistent_awdl && vif!=radio->vif && ROC_GET(radio->roc.deleting) &&
	    lx_awdl_roc_vdev(vif)==radio->roc.p2p_id;
}
static bool lx_awdl_clear_secondary(struct lx_radio *r,unsigned id)
{
	if (!r->persistent_awdl) return lx_awdl_roc_driver_idle(&r->hw);
	int state=lx_awdl_scan_state(&r->hw,ROC_GET(r->roc.request_seq),id);
	return state!=LX_AWDL_SCAN_OURS && state!=LX_AWDL_SCAN_OTHER_SAME_VDEV;
}
'''+marker)
    marker='ROC_SET(r->active,1);'
    code=once(code,marker,'if (radio->persistent_awdl) { ROC_ADD(r->request_seq); ROC_SET(r->request_thread,lx_os_thread_self()); }\n\t'+marker)
    marker='int rc=radio->ops->remain_on_channel(hw,roc_vif,chan,duration,IEEE80211_ROC_TYPE_NORMAL);'
    code=once(code,marker,marker+'\n\tROC_SET(r->request_thread,NULL);')
    a=code.index('int cancel=0, cancelled=0;');b=code.index('limit=lx_os_nanoseconds()+1500000000ull;',a)
    part=code[a:b];a0=part.index('cancelled=1;');b0=part.index('mutex_unlock(',a0)
    part=part[:a0]+'''mutex_lock(&hw->wiphy->mtx);
		if (!radio->persistent_awdl || (ROC_GET(r->started) && !ROC_GET(r->done) &&
		    lx_awdl_scan_state(hw,ROC_GET(r->request_seq),r->vdev)==LX_AWDL_SCAN_OURS)) {
			cancelled=1;cancel=radio->ops->cancel_remain_on_channel(hw,roc_vif);
		}
		'''+part[b0:]
    code=code[:a]+part+code[b:]
    code=once(code,'if (mode==48 && distinct && lx_awdl_roc_driver_idle(hw) &&', 'if (mode==48 && distinct && lx_awdl_clear_secondary(radio,id) &&')
    code=once(code,'if (__atomic_load_n(&r->iterating,__ATOMIC_SEQ_CST) || !lx_awdl_roc_driver_idle(hw)) {',
             'if (__atomic_load_n(&r->iterating,__ATOMIC_SEQ_CST) || !lx_awdl_clear_secondary(radio,id)) {')
    code=once(code,'bool clean=ack==1 && after==before && lx_awdl_roc_driver_idle(hw);',
             'bool clean=ack==1 && after==before && lx_awdl_clear_secondary(radio,id);')
    write(rp,code)
    lp=d+'live_transport.inc';code=read(lp)
    marker='/* Discovery requests ch6'
    code=once(code,marker,'''int occupied=lx_awdl_scan_state(&r->hw,EG(r->roc.request_seq),echo.vdev);
		if (occupied==LX_AWDL_SCAN_OURS) {ES(live.stop,1);break;}
		if (occupied!=LX_AWDL_SCAN_IDLE) {
			unsigned count=EA(r->roc.scan_backoffs)+1;
			if (count==1 || !(count%32)) lx_tell("awdl-live: external_scan_backoff=%u class=%d provider_ready=%u\\n",count,occupied,EG(live.ready));
			live_retire();live_deliver();live_presence();lx_pause_ms(125);continue;
		}
		'''+marker)
    marker='if (outcome<0 || !lx_awdl_roc_driver_idle(&r->hw)) {ES(live.stop,1);break;}'
    code=once(code,marker,'if (outcome<0 || lx_awdl_scan_state(&r->hw,EG(r->roc.request_seq),echo.vdev)==LX_AWDL_SCAN_OURS) {ES(live.stop,1);break;}')
    write(lp,code)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_mac.o',work+'obj/lx_lx_80211.o'],check=True)
    # The ROC state layout is only allocated/accessed by lx_80211.c. Other
    # pinned vendor objects use typed function calls, never these field offsets.
    with (OUT/'live-scoped-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-live-scoped.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'live-scoped-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
