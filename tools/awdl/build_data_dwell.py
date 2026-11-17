"""Bound the unchanged #425 native data frame to a freshly confirmed channel-6 ROC."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='ddaf5e54c72e130f1ca3592c9bf01272c29264dbf55f57e6236695b9f234b936'

def build():
    pin=json.loads((OUT/'data-carrier-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #425')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-dwell-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    code=read(src)
    code=once(code,'struct sk_buff *msdu;','''struct sk_buff *msdu, *window_frame;
	struct ieee80211_sta *window_peer;
	unsigned foreign_submit,foreign_desc,foreign_tail,foreign_wbm;
	unsigned long long submit_us,desc_us,wbm_us;''')
    marker='int lx_awdl_echo_ctx('
    code=once(code,marker,(HERE/'data_window.inc').read_text()+'\n'+marker)
    marker='ES(echo.ring,ring); ES(echo.desc,desc);'
    code=once(code,marker,'ES(echo.foreign_desc,lx_awdl_data_foreign(hw)); ES(echo.desc_us,lx_os_nanoseconds()/1000ull);\n\t'+marker)
    marker='if (progress>=needed) ES(echo.eaten,1);'
    code=once(code,marker,'if (progress>=needed) { ES(echo.foreign_tail,lx_awdl_data_foreign(hw)); ES(echo.eaten,1); }')
    marker='ES(echo.source,src); ES(echo.reason,reason);'
    code=once(code,marker,'ES(echo.foreign_wbm,lx_awdl_data_foreign(hw)); ES(echo.wbm_us,lx_os_nanoseconds()/1000ull);\n\t'+marker)
    a=code.index('struct ieee80211_tx_control control={.sta=peer};')
    b=code.index('ops->tx(hw,&control,skb);',a)+len('ops->tx(hw,&control,skb);')
    code=code[:a]+'''echo.window_frame=skb; echo.window_peer=peer;
				bool completed=lx_awdl_listen_channel(radio,vif,6);
				if (echo.window_frame) { dev_kfree_skb_any(echo.window_frame); echo.window_frame=NULL; }
				echo.window_peer=NULL;
				lx_tell("awdl-echo: data_roc_completed=%u submit_foreign=%u desc_foreign=%u tcl_tail_observed_foreign=%u wbm_foreign=%u start_us=%llu desc_us=%llu wbm_us=%llu\\n",
				    completed,EG(echo.foreign_submit),EG(echo.foreign_desc),EG(echo.foreign_tail),EG(echo.foreign_wbm),
				    EG(echo.submit_us),EG(echo.desc_us),EG(echo.wbm_us));
				if (!lx_awdl_roc_driver_idle(hw)) {
					lx_tell("awdl-echo: unresolved ROC, retaining owned VDEV\\n");
					return false;
				}'''+code[b:]
    write(src,code)
    rp=stage+'/wifi/lx/src/roc.inc'
    roc=read(rp)
    old='if (!rc && (r->choice==47 || r->choice==48)) lx_awdl_psf_once(radio,roc_vif,chan);'
    roc=once(roc,old,'''if (!rc && echo.window_frame) lx_awdl_data_foreign_send(radio,roc_vif,chan);
	else if (!rc && (r->choice==47 || r->choice==48)) lx_awdl_psf_once(radio,roc_vif,chan);''')
    old='while (!rc && (!ROC_GET(r->done) || !ROC_GET(r->expired)) && lx_os_nanoseconds()<limit)\n\t\tlx_pause_ms(10);'
    roc=once(roc,old,'''while (!rc && (!ROC_GET(r->done) || !ROC_GET(r->expired)) && lx_os_nanoseconds()<limit) {
		if (EG(echo.submitted)) lx_awdl_data_poll(hw);
		lx_pause_ms(10);
	}''')
    write(rp,roc)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'data-dwell-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-data-dwell.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'data-dwell-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
