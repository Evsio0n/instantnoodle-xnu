"""Admit the ready carrier BSS event on its proven local channel, preserve true ROC result."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='1992426ce2dd813a02b5c50d14675faf9b8f617daff45fb1d703347caa5f77bc'

def build():
    pin=json.loads((OUT/'data-dwell-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #426')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-bss-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    code=read(src)
    original=(HERE/'data_window.inc').read_text()
    code=once(code,original,'static void lx_awdl_data_poll(struct ieee80211_hw *);\n'+(HERE/'data_bss_window.inc').read_text())
    code=once(code,'ES(echo.foreign_desc,lx_awdl_data_foreign(hw));','ES(bw.at_desc,lx_awdl_home_basis(hw)); ES(echo.foreign_desc,lx_awdl_data_foreign(hw));')
    code=once(code,'ES(echo.foreign_wbm,lx_awdl_data_foreign(hw));','ES(bw.at_wbm,lx_awdl_home_basis(hw)); ES(echo.foreign_wbm,lx_awdl_data_foreign(hw));')
    old='bool completed=lx_awdl_listen_channel(radio,vif,6);'
    code=once(code,old,'''int outcome=lx_awdl_station_window(radio,vif,ch);
				if (outcome<0) { lx_tell("awdl-echo: ROC worker still owns VDEV, retaining\\n"); return false; }
				bool completed=outcome>0;''')
    write(src,code)
    rp=stage+'/wifi/lx/src/roc.inc'; roc=read(rp)
    a=roc.index('void\nlx_awdl_roc_wmi('); b=roc.index('\n}',a)
    roc=roc[:b]+'\n\tlx_awdl_window_signal(hw,vdev,event,frequency,reason);'+roc[b:]
    a=roc.index('void\nlx_awdl_roc_notify('); b=roc.index('\n}',a)
    roc=roc[:b]+'\n\tif (ready && EG(bw.armed) && bw.radio==radio_of(hw) && r->vdev==echo.vdev) { ES(bw.ready,1); EA(bw.signal); lx_os_wakeup(&bw.signal); }'+roc[b:]
    a=roc.index('if (!rc && echo.window_frame) lx_awdl_data_foreign_send(')
    b=roc.index('lx_awdl_psf_once(radio,roc_vif,chan);',a)+len('lx_awdl_psf_once(radio,roc_vif,chan);')
    roc=roc[:a]+'if (!rc && !EG(bw.armed) && (r->choice==47 || r->choice==48)) lx_awdl_psf_once(radio,roc_vif,chan);'+roc[b:]
    # Only the station owner samples the data ring for this new transaction.
    roc=once(roc,'if (EG(echo.submitted)) lx_awdl_data_poll(hw);','if (!EG(bw.armed) && EG(echo.submitted)) lx_awdl_data_poll(hw);')
    write(rp,roc)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'data-bss-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-data-bss.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'data-bss-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
