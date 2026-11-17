"""Build the bounded AWDL ROC observer from the retained cellular ath11k objects."""
from pathlib import Path
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = ROOT / 'out' / 'awdl-kernel'
BASE = '/tmp/ath-cell'
BUS = 'oneplus8-build'
CONTROL = 'a67239fe8edeed537ea0cb5b468c0d2f651b34c88368e4e029b36ac4270f836b'

def read(path):
    return subprocess.check_output(['docker','exec',BUS,'cat',path]).decode()

def write(path, source):
    subprocess.run(['docker','exec','-i',BUS,'sh','-c','cat > "$1"','sh',path],
                   input=source.encode(),check=True)

def once(source, old, new):
    if source.count(old) != 1:
        raise ValueError(old.splitlines()[0])
    return source.replace(old,new)

def sha(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f,'sha256').hexdigest()

def build():
    OUT.mkdir(parents=True,exist_ok=True)
    job = subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-roc-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',job+'/'],check=True)
    obj = job+'/xnu-deps/build/ath11k'
    before = subprocess.check_output(['docker','exec',BUS,'sha256sum',obj+'/out/ath11k.o'],text=True)
    if before.split()[0] != CONTROL:
        raise ValueError('cellular control object mismatch')
    lx = (ROOT/'wifi/lx/src/lx_80211.c').read_text()
    lx = once(lx,'#include "lx_awdl_rx.h"','#include "lx_awdl_rx.h"\n#include "roc_state.h"')
    lx = once(lx,'bool awdl_observe;', 'struct lx_awdl_roc_state roc;\n\tbool awdl_observe;')
    lx = once(lx,'radio->awdl_observe = lx_os_option("wifi_awdl_observe") == 1;', '''
	radio->roc.choice=lx_os_option("wifi_awdl_observe");
	radio->awdl_observe=(radio->roc.choice==1 || radio->roc.choice==6 ||
	    radio->roc.choice==44 || radio->roc.choice==149);''')
    lx = once(lx,'void\nlinuxkpi_ieee80211_rx(', '#include "roc.inc"\n\nvoid\nlinuxkpi_ieee80211_rx(')
    lx = once(lx,'\tradio->frames++;\n\tif (status->flag & RX_FLAG_MMIC_ERROR)',
              '\tradio->frames++;\n\tif (radio->awdl_observe) lx_awdl_roc_rx(radio,skb,status);\n\tif (status->flag & RX_FLAG_MMIC_ERROR)')
    lx = once(lx,'lx_tell("lx: THE NETWORK IS JOINED: frames can pass\\n");',
              'lx_tell("lx: THE NETWORK IS JOINED: frames can pass\\n");\n\tlx_awdl_listen_once(radio);')
    write(job+'/wifi/lx/src/lx_80211.c',lx)
    write(job+'/wifi/lx/src/lx_awdl_rx.h',(ROOT/'wifi/lx/src/lx_awdl_rx.h').read_text())
    write(job+'/wifi/lx/src/roc.inc',(HERE/'roc.inc').read_text())
    api=(HERE/'roc_state.h').read_text()
    write(job+'/wifi/lx/src/roc_state.h',api)
    write(obj+'/src/ath11k/roc_state.h',api)

    wi = obj+'/src/ath11k/wmi.c'
    wmi = read(wi)
    anchor='static void ath11k_scan_event(struct ath11k_base *ab, struct sk_buff *skb)'
    wmi=once(wmi,anchor,'''#include "roc_state.h"

int lx_awdl_roc_driver_idle(struct ieee80211_hw *hw)
{
	struct ath11k *ar=hw->priv;
	spin_lock_bh(&ar->data_lock);
	int idle=ar->scan.state==ATH11K_SCAN_IDLE && !ar->scan_channel && !ar->scan.roc_freq;
	spin_unlock_bh(&ar->data_lock);
	return idle;
}

unsigned lx_awdl_roc_vdev(struct ieee80211_vif *vif)
{
	return ath11k_vif_to_arvif(vif)->vdev_id;
}

'''+anchor)
    offset=wmi.index(anchor)
    front, handler=wmi[:offset],wmi[offset:]
    switch='\tswitch (scan_ev.event_type) {'
    hook='''
	if (ar->scan.is_roc && ar->scan.vdev_id==scan_ev.vdev_id) {
		int kind=-1;
		switch (scan_ev.event_type) {
		case WMI_SCAN_EVENT_STARTED: kind=LX_ROC_START; break;
		case WMI_SCAN_EVENT_FOREIGN_CHAN: kind=LX_ROC_FOREIGN; break;
		case WMI_SCAN_EVENT_BSS_CHANNEL: kind=LX_ROC_BSS; break;
		case WMI_SCAN_EVENT_COMPLETED: kind=LX_ROC_DONE; break;
		case WMI_SCAN_EVENT_DEQUEUED: kind=LX_ROC_DEQUEUED; break;
		default: break;
		}
		if (kind>=0)
			lx_awdl_roc_wmi(ar->hw,scan_ev.vdev_id,scan_ev.scan_id,scan_ev.scan_req_id,
			    kind,scan_ev.channel_freq,scan_ev.reason);
	}
'''
    wmi=front+once(handler,switch,hook+switch)
    write(wi,wmi)
    compat=obj+'/freebsd/sys/compat/linuxkpi/common/include/net/mac80211.h'
    defs=read(compat)
    for name,value in [('ieee80211_ready_on_channel',1),('ieee80211_remain_on_channel_expired',0)]:
        key=name+'(struct ieee80211_hw *hw)\n{\n\tTODO();\n'+('/* XXX-BZ We need to see that. */\n' if value else '')+'}'
        defs=once(defs,key,name+'(struct ieee80211_hw *hw)\n{\n\textern void lx_awdl_roc_notify(struct ieee80211_hw *, int);\n\tlx_awdl_roc_notify(hw, '+str(value)+');\n}')
    write(compat,defs)
    # These three objects own the inline callbacks or the code modified above.
    subprocess.run(['docker','exec',BUS,'rm',obj+'/obj/ath11k_wmi.o',obj+'/obj/ath11k_mac.o',
                    obj+'/obj/lx_lx_80211.o'],check=True)
    with (OUT/'roc-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',job+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    dest=OUT/'ath11k-cell-roc.o'
    subprocess.run(['docker','cp',BUS+':'+obj+'/out/ath11k.o',str(dest)],check=True)
    (OUT/'roc-build.json').write_text(json.dumps({'base':CONTROL,'object':str(dest),
        'sha256':sha(dest),'builder':sha(Path(__file__)),'roc':sha(HERE/'roc.inc'),'container_tree':job},indent=2)+'\n')
    print(sha(dest),dest)

if __name__=='__main__':
    build()
