"""Build anonymous per-frequency ingress diagnostics on the frozen P2P trial."""
from pathlib import Path
import json
import subprocess
from build_roc import BUS, HERE, OUT, once, read, sha, write

P2P='c3ecc4749b6f248b0955ac4d9ca4140333a58a66758cbb59ab44dc9dae1b0ab8'

def build():
    pin=json.loads((OUT/'p2p-build.json').read_text())
    if pin['sha256'] != P2P or sha(Path(pin['object'])) != P2P:
        raise ValueError('expected the first P2P candidate')
    job=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-freq-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',job+'/'],check=True)
    src=job+'/wifi/lx/src/'
    work=job+'/xnu-deps/build/ath11k/'
    api=read(src+'roc_state.h')
    api=once(api,'struct lx_awdl_roc_state {','''struct lx_awdl_roc_state {
	unsigned foreign_open, rx_all, rx_native, rx_ethernet, rx_failed;
	unsigned freq_unknown, freq_home, freq_target, freq_other;
	unsigned anyfreq_psf, anyfreq_mif, other_vendor_header;
	unsigned mgmt_wmi, mgmt_nonzero_clock, awdl_clock, awdl_clock_advances;
	unsigned long long last_rx_tsf;''')
    api=once(api,'#endif','''void lx_awdl_wmi_clock(struct ieee80211_hw *, unsigned, const void *,
    size_t, unsigned long long);
#endif''')
    write(src+'roc_state.h',api)
    write(work+'src/ath11k/roc_state.h',api)
    wh=work+'src/ath11k/wmi.h'
    raw=read(wh)
    a=raw.index('struct mgmt_rx_event_params {')
    z=raw.index('\n};',a)
    raw=raw[:z]+'\n\tu64 awdl_rx_clock;'+raw[z:]
    write(wh,raw)
    wi=work+'src/ath11k/wmi.c'
    raw=read(wi)
    raw=once(raw,'hdr->tsf_delta =  ev->tsf_delta;',
        'hdr->tsf_delta =  ev->tsf_delta;\n\thdr->awdl_rx_clock=(u64)ev->rx_tsf_u32 << 32 | ev->rx_tsf_l32;')
    anchor='\tieee80211_rx_ni(ar->hw, skb);'
    raw=once(raw,anchor,'''	extern void lx_awdl_wmi_clock(struct ieee80211_hw *, unsigned, const void *, size_t, unsigned long long);
	lx_awdl_wmi_clock(ar->hw,status->freq,skb->data,skb_headlen(skb),rx_ev.awdl_rx_clock);
'''+anchor)
    write(wi,raw)
    rc=read(src+'roc.inc')
    rc=once(rc,'if (frequency == r->target_mhz) ROC_ADD(r->foreign);','''if (frequency == r->target_mhz) {
			ROC_ADD(r->foreign); ROC_SET(r->foreign_open,1);
		}''')
    for field in ['bss','done','dequeued']:
        rc=once(rc,'ROC_ADD(r->'+field+'); break;','ROC_SET(r->foreign_open,0); ROC_ADD(r->'+field+'); break;')
    pos='static void\nlx_awdl_roc_rx('
    rc=once(rc,pos,(HERE/'provenance.inc').read_text()+'\n'+pos)
    # This call precedes CRC and channel filtering in the existing observer.
    rc=once(rc,'if (!ROC_GET(r->active) || (status->flag &',
        'lx_awdl_window_rx(radio,skb,status);\n\tif (!ROC_GET(r->active) || (status->flag &')
    rc=once(rc,'r->target_rx=r->target_psf=r->target_mif=r->home_beacon=r->home_data=0;', '''r->target_rx=r->target_psf=r->target_mif=r->home_beacon=r->home_data=0;
	r->foreign_open=r->rx_all=r->rx_native=r->rx_ethernet=r->rx_failed=0;
	r->freq_unknown=r->freq_home=r->freq_target=r->freq_other=0;
	r->anyfreq_psf=r->anyfreq_mif=r->other_vendor_header=0;
	r->mgmt_wmi=r->mgmt_nonzero_clock=r->awdl_clock=r->awdl_clock_advances=0;
	r->last_rx_tsf=0;''')
    rc=once(rc,'\treturn !rc && ROC_GET(r->started)', '''	lx_tell("awdl-roc: ingress target=%u all=%u zero_freq=%u home_freq=%u target_freq=%u other_freq=%u raw=%u ethernet=%u bad_status=%u all_psf=%u all_mif=%u other_awdl=%u\\n",
	    r->target_mhz,ROC_GET(r->rx_all),ROC_GET(r->freq_unknown),ROC_GET(r->freq_home),
	    ROC_GET(r->freq_target),ROC_GET(r->freq_other),ROC_GET(r->rx_native),ROC_GET(r->rx_ethernet),
	    ROC_GET(r->rx_failed),ROC_GET(r->anyfreq_psf),ROC_GET(r->anyfreq_mif),ROC_GET(r->other_vendor_header));
	lx_tell("awdl-roc: wmi_clock target=%u mgmt=%u nonzero=%u matching_awdl=%u advancing=%u\\n",
	    r->target_mhz,ROC_GET(r->mgmt_wmi),ROC_GET(r->mgmt_nonzero_clock),ROC_GET(r->awdl_clock),
	    ROC_GET(r->awdl_clock_advances));
	return !rc && ROC_GET(r->started)''')
    write(src+'roc.inc',rc)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_wmi.o',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'provenance-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',job+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    dest=OUT/'ath11k-cell-provenance.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True)
    (OUT/'provenance-build.json').write_text(json.dumps({'reference':P2P,'sha256':sha(dest),'object':str(dest),
        'builder':sha(Path(__file__)),'observer':sha(HERE/'provenance.inc'),'container_tree':job},indent=2)+'\n')
    print(sha(dest),dest)

if __name__=='__main__':
    build()
