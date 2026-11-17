"""Add a single locally generated ch6 PSF to an already firmware-confirmed P2P dwell."""
from pathlib import Path
import json
import subprocess
from build_roc import BUS, HERE, OUT, once, read, sha, write

PRIOR='39cbdcdac22df2b81b9da17d6f402fe2e416c36ac575c9ad70542118b85b127c'

def build():
    pin=json.loads((OUT/'provenance-build.json').read_text())
    if pin['sha256']!=PRIOR or sha(Path(pin['object']))!=PRIOR:
        raise ValueError('expected the quantified P2P RX candidate')
    job=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-psf-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',job+'/'],check=True)
    src=job+'/wifi/lx/src/'
    work=job+'/xnu-deps/build/ath11k/'
    api=read(src+'roc_state.h')
    api=once(api,'struct lx_awdl_roc_state {','''struct sk_buff;
struct lx_awdl_roc_state {
	struct sk_buff *tx_frame;
	unsigned long long foreign_us, tx_begin_us, tx_queue_return_us, tx_wmi_us, tx_fw_us, tx_free_us;
	unsigned tx_called, tx_permit, tx_bound, tx_desc, tx_submit_seen, tx_fw;
	int tx_submit_rc;
	unsigned tx_fw_status, tx_dropped, tx_cancel_before_wmi, tx_released;''')
    api=once(api,'#endif','''int lx_awdl_psf_prepare(struct ieee80211_hw *,struct sk_buff *,unsigned,unsigned,unsigned);
void lx_awdl_psf_submit_result(struct ieee80211_hw *,unsigned,int);
void lx_awdl_psf_fw(struct ieee80211_hw *,struct sk_buff *,unsigned,unsigned);
void lx_awdl_psf_release(struct ieee80211_hw *,struct sk_buff *,int);
int lx_awdl_psf_worker_independent(struct ieee80211_hw *);
#endif''')
    write(src+'roc_state.h',api)
    write(work+'src/ath11k/roc_state.h',api)
    write(src+'psf.h',(HERE/'psf.h').read_text())
    # Each compatibility workqueue has a distinct XNU worker. Refuse this
    # diagnostic on WMI's own consumer or when the worker is not running.
    base=read(src+'lx_base.c')
    base=once(base,'static void\nworker(void *argument)','''int lx_awdl_other_workqueue(struct workqueue_struct *wq)
{
	if (!wq) return 0;
	struct lx_workqueue *q=(struct lx_workqueue *)wq;
	return q->thread && q->thread!=lx_os_thread_self() && !q->dying;
}
static void
worker(void *argument)''')
    write(src+'lx_base.c',base)
    wmi_file=work+'src/ath11k/wmi.c'
    wmi=read(wmi_file)
    wmi=once(wmi,'static u32 ath11k_wmi_mgmt_get_freq(','''#include "roc_state.h"
extern int lx_awdl_other_workqueue(struct workqueue_struct *);
int lx_awdl_psf_worker_independent(struct ieee80211_hw *hw)
{
	struct ath11k *ar=hw->priv;
	return lx_awdl_other_workqueue(ar->ab->workqueue_aux);
}
static u32 ath11k_wmi_mgmt_get_freq(''')
    wmi=once(wmi,'ret = ath11k_wmi_cmd_send(wmi, skb, WMI_MGMT_TX_SEND_CMDID);','''int tracked=lx_awdl_psf_prepare(ar->hw,frame,vdev_id,buf_id,cmd->chanfreq);
	ret = tracked<0 ? tracked : ath11k_wmi_cmd_send(wmi, skb, WMI_MGMT_TX_SEND_CMDID);
	if (tracked>0) lx_awdl_psf_submit_result(ar->hw,buf_id,ret);''')
    wmi=once(wmi,'dma_unmap_single(ar->ab->dev, skb_cb->paddr, msdu->len, DMA_TO_DEVICE);',
             'dma_unmap_single(ar->ab->dev, skb_cb->paddr, msdu->len, DMA_TO_DEVICE);\n\tlx_awdl_psf_fw(ar->hw,msdu,tx_compl_param->desc_id,tx_compl_param->status);')
    write(wmi_file,wmi)
    lx=read(src+'lx_80211.c')
    lx=once(lx,'radio->roc.choice==46 ||','radio->roc.choice==46 || radio->roc.choice==47 ||')
    lx=once(lx,'if (r->choice==46) {','if (r->choice==46 || r->choice==47) {')
    for fn,which in [('linuxkpi_ieee80211_tx_status',0),('linuxkpi_ieee80211_free_txskb',1)]:
        a=lx.index(fn+'(struct ieee80211_hw *hw, struct sk_buff *skb')
        b=lx.index('\n}',a)
        section=once(lx[a:b],'dev_kfree_skb_any(skb);',
                     'lx_awdl_psf_release(hw,skb,'+str(which)+');\n\tdev_kfree_skb_any(skb);')
        lx=lx[:a]+section+lx[b:]
    write(src+'lx_80211.c',lx)
    code=read(src+'roc.inc')
    code=once(code,'ROC_ADD(r->foreign); ROC_SET(r->foreign_open,1);','''ROC_SET(r->foreign_us,lx_os_nanoseconds()/1000ull);
			ROC_ADD(r->foreign); ROC_SET(r->foreign_open,1);''')
    anchor='static bool\nlx_awdl_listen_channel('
    code=once(code,anchor,(HERE/'psf_tx.inc').read_text()+'\n'+anchor)
    code=once(code,'unsigned long long limit=lx_os_nanoseconds()+1800000000ull;', '''if (!rc && r->choice==47) lx_awdl_psf_once(radio,roc_vif,chan);
	unsigned long long limit=lx_os_nanoseconds()+1800000000ull;''')
    code=once(code,'mode!=149 && mode!=46','mode!=149 && mode!=46 && mode!=47')
    code=once(code,'if (mode!=46) {','if (mode!=46 && mode!=47) {')
    code=once(code,'unsigned c=social[i];','unsigned c=social[i];\n\t\t\tif (mode==47 && c!=6) continue;')
    # roc.inc drops its macros before the P2P owner function.
    anchor='\t__atomic_store_n(&r->visible,NULL,__ATOMIC_SEQ_CST);'
    code=once(code,anchor,'''	if (mode==47 && !lx_awdl_psf_quiesced(radio)) {
		__atomic_store_n(&r->visible,NULL,__ATOMIC_SEQ_CST);
		lx_tell("awdl-p2p: unresolved native TX owner, VDEV kept until teardown\\n");
		return;
	}
'''+anchor)
    write(src+'roc.inc',code)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_wmi.o',work+'obj/lx_lx_80211.o',
                    work+'obj/lx_lx_base.o'],check=True)
    with (OUT/'psf-build.log').open('w') as f:
        subprocess.run(['docker','exec',BUS,'bash',job+'/tools/mk-ath11k.sh'],stdout=f,stderr=subprocess.STDOUT,check=True)
    dest=OUT/'ath11k-cell-psf.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True)
    (OUT/'psf-build.json').write_text(json.dumps({'reference':PRIOR,'sha256':sha(dest),'object':str(dest),
        'builder':sha(Path(__file__)),'encoder':sha(HERE/'psf.h'),'tx_owner':sha(HERE/'psf_tx.inc'),
        'container_tree':job},indent=2)+'\n')
    print(sha(dest),dest)

if __name__=='__main__':
    build()
