"""Extend the frozen ROC radio object with one additional, passive P2P VDEV."""
from pathlib import Path
import json
import subprocess
from build_roc import BUS, HERE, OUT, once, read, sha, write

ROC = 'cc61b98e9cf256da674ad03b86012189bd10c62faa6bee2b84d4b80bfcb27880'

def build():
    pin=json.loads((OUT/'roc-build.json').read_text())
    if pin['sha256']!=ROC or sha(Path(pin['object']))!=ROC:
        raise ValueError('The reference ROC object does not match the #417 pin')
    job=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-p2p-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',job+'/'],check=True)
    src=job+'/wifi/lx/src/'
    work=job+'/xnu-deps/build/ath11k/'
    api=read(src+'roc_state.h')
    api=once(api,'struct lx_awdl_roc_state {','''struct lx_awdl_roc_state {
	struct ieee80211_vif *visible;
	unsigned iterating, deleting, delete_events, p2p_id;''')
    api=once(api,'#endif','''unsigned long long lx_awdl_allocated_vdevs(struct ieee80211_hw *);
void lx_awdl_p2p_deleted(struct ieee80211_hw *, unsigned);
#endif''')
    write(src+'roc_state.h',api)
    write(work+'src/ath11k/roc_state.h',api)
    wi=work+'src/ath11k/wmi.c'
    wmi=read(wi)
    wmi=once(wmi,'unsigned lx_awdl_roc_vdev(struct ieee80211_vif *vif)','''unsigned long long lx_awdl_allocated_vdevs(struct ieee80211_hw *hw)
{
	struct ath11k *ar=hw->priv;
	mutex_lock(&ar->conf_mutex);
	unsigned long long allocated=ar->allocated_vdev_map;
	mutex_unlock(&ar->conf_mutex);
	return allocated;
}
unsigned lx_awdl_roc_vdev(struct ieee80211_vif *vif)''')
    # The include used by #417 appears after this earlier event handler.
    wmi=once(wmi,'\tcomplete(&ar->vdev_delete_done);','''	extern void lx_awdl_p2p_deleted(struct ieee80211_hw *, unsigned);
	lx_awdl_p2p_deleted(ar->hw,vdev_id);
	complete(&ar->vdev_delete_done);''')
    write(wi,wmi)
    lx=read(src+'lx_80211.c')
    lx=once(lx,'radio->roc.choice==44 || radio->roc.choice==149','radio->roc.choice==44 || radio->roc.choice==46 || radio->roc.choice==149')
    i=lx.index('linuxkpi_ieee80211_iterate_interfaces(')
    j=lx.index('\n}',i)
    lx=lx[:j]+'''
	struct lx_awdl_roc_state *r=&radio_of(hw)->roc;
	if (r->choice==46) {
		__atomic_fetch_add(&r->iterating,1,__ATOMIC_SEQ_CST);
		struct ieee80211_vif *second=__atomic_load_n(&r->visible,__ATOMIC_SEQ_CST);
		if (second) function(argument,second->addr,second);
		__atomic_fetch_sub(&r->iterating,1,__ATOMIC_SEQ_CST);
	}
'''+lx[j:]
    write(src+'lx_80211.c',lx)
    code=read(src+'roc.inc')
    code=once(code,'static void\nlx_awdl_listen_once(struct lx_radio *radio)','static bool\nlx_awdl_listen_channel(struct lx_radio *radio, struct ieee80211_vif *roc_vif, unsigned c)')
    i=code.index('static bool\nlx_awdl_listen_channel(')
    front,body=code[:i],code[i:]
    a=body.index('\tunsigned c = r->choice;')
    b=body.index('\tstruct ieee80211_hw *hw',a)
    body=body[:a]+'''	/* Reset only this window's fields while no matched ROC is active. */
	r->started=r->foreign=r->bss=r->done=r->dequeued=r->reason=r->ready=r->expired=0;
	r->target_rx=r->target_psf=r->target_mif=r->home_beacon=r->home_data=0;
'''+body[b:]
    body=body.replace(' return;',' return false;').replace('lx_awdl_roc_vdev(radio->vif)','lx_awdl_roc_vdev(roc_vif)')
    body=body.replace('remain_on_channel(hw,radio->vif','remain_on_channel(hw,roc_vif')
    pos=body.index('\n}\n#undef ROC_GET')
    body=body[:pos]+'''
	return !rc && ROC_GET(r->started) && ROC_GET(r->ready) && ROC_GET(r->foreign) &&
	    ROC_GET(r->done) && ROC_GET(r->expired) && !ROC_GET(r->dequeued) &&
	    lx_awdl_roc_driver_idle(hw) && retained && (ROC_GET(r->home_beacon)||ROC_GET(r->home_data));'''+body[pos:]
    write(src+'roc.inc',front+body+'\n'+(HERE/'p2p.inc').read_text())
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_wmi.o',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'p2p-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',job+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    dest=OUT/'ath11k-cell-p2p.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True)
    (OUT/'p2p-build.json').write_text(json.dumps({'reference':ROC,'sha256':sha(dest),'object':str(dest),
        'builder':sha(Path(__file__)),'p2p':sha(HERE/'p2p.inc'),'container_tree':job},indent=2)+'\n')
    print(sha(dest),dest)

if __name__=='__main__':
    build()
