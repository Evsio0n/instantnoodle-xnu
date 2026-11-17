"""Observe peer NS/NA for the real local link address on the confirmed #427 carrier."""
import json,os,subprocess
from pathlib import Path
from build_roc import BUS,HERE,OUT,ROOT,read,write,sha,once
from build_data import MAC_TOKEN,IP_TOKEN
PRIOR='72363e840669a3cb068f1b2a1f82ca9c7a45daeecd1ee4e7b881905010731e23'

def build():
    pin=json.loads((OUT/'data-bss-build.json').read_text())
    if sha(Path(pin['object']))!=PRIOR: raise ValueError('expected #427')
    root=ROOT/'out/airdrop'
    ip=(root/'controlled-mac-ipv6.bin').read_bytes()
    peer=(root/'controlled-mac-derived-address.bin').read_bytes()
    if len(ip)!=16 or len(peer)!=6 or ip[11:13]!=b'\xff\xfe' or peer!=bytes([ip[8]^2])+ip[9:11]+ip[13:]:
        raise ValueError('endpoint mismatch')
    def equip(b):
        if b.count(MAC_TOKEN)!=1 or b.count(IP_TOKEN)!=1: raise ValueError('invalid private object')
        return b.replace(MAC_TOKEN,peer).replace(IP_TOKEN,ip)
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-nd-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',pin['container_tree']+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'; src=stage+'/wifi/lx/src/data.inc'
    original=subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o'])
    if equip(original)!=Path(pin['object']).read_bytes(): raise ValueError('unmatched baseline')
    code=read(src)
    code=once(code,'#include "echo6.h"','#include "echo6.h"\n#include "nd6_observe.h"')
    code=once(code,'unsigned acked, released,','unsigned nd_watch,nd_ns_group,nd_ns_uni,nd_na_local,nd_before_tx,nd_eth_short,nd_eth_ip,nd_native,nd_amsdu;\n\tunsigned acked, released,')
    code=once(code,'static bool lx_awdl_echo_receive(', (HERE/'nd_rx.inc').read_text()+'\nstatic bool lx_awdl_echo_receive(')
    code=once(code,'if (!EG(echo.receive) || echo.hw!=&radio->hw ||',
        'if ((!EG(echo.receive) && !EG(echo.nd_watch)) || echo.hw!=&radio->hw ||')
    code=once(code,'const uint8_t *ip;\n\tsize_t bytes;', 'const uint8_t *da,*ip;\n\tsize_t bytes;\n\tunsigned format=0;')
    a=code.index('if (n<14 || memcmp(p,echo.mac,6)')
    b=code.index('ip=p+14; bytes=n-14;',a)+len('ip=p+14; bytes=n-14;')
    code=code[:a]+'''if (n<14 || memcmp(p+6,awdl_target_mac,6) || !lx_awdl_nd_l2(p)) return false;
		da=p;
		if (p[12]==0x86 && p[13]==0xdd) { format=1; ip=p+14; bytes=n-14; }
		else if (p[12]==8 && p[13]==0 && n>=22 && p[14]==3 && p[15]==4 &&
		    p[18]==0 && p[19]==0 && p[20]==0x86 && p[21]==0xdd) { format=2; ip=p+22; bytes=n-22; }
		else return false;'''+code[b:]
    code=once(code,'memcmp(p+4,echo.mac,6) || memcmp(p+10,awdl_target_mac,6) ||','!lx_awdl_nd_l2(p+4) || memcmp(p+10,awdl_target_mac,6) ||')
    code=once(code,'if (n<h || (h==26 && (p[24]&0x80))) return false;',
        'if (n<h) return false;\n\t\tif (h==26 && (p[24]&0x80)) { EA(echo.nd_amsdu); return false; }')
    code=once(code,'ip=ipv6.packet; bytes=ipv6.length;', 'da=p+4; ip=ipv6.packet; bytes=ipv6.length;')
    i=code.index('static bool lx_awdl_echo_receive(');j=code.index('\n}',i)
    begin=code.index('EA(echo.received);',i,j)
    code=code[:begin]+'return lx_awdl_nd_input(da,ip,bytes,format);'+code[j:]
    code=once(code,'if (!port) {\n\t\tuint8_t *packet;', 'if (!port) {\n\t\tES(echo.nd_watch,1);\n\t\tuint8_t *packet;')
    code=once(code,'ES(echo.receive,0);\n\tES(echo.timedout,','''ES(echo.nd_watch,0);
	ES(echo.receive,0);
	lx_tell("awdl-nd: ns_for_local_multicast=%u ns_for_local_unicast=%u na_claiming_local=%u nd_before_echo=%u verified_native=%u verified_eth_ipv6=%u verified_eth_short=%u scoped_raw_amsdu_unsupported=%u generated_responses=0\\n",
	    EG(echo.nd_ns_group),EG(echo.nd_ns_uni),EG(echo.nd_na_local),EG(echo.nd_before_tx),EG(echo.nd_native),EG(echo.nd_eth_ip),EG(echo.nd_eth_short),EG(echo.nd_amsdu));
	ES(echo.timedout,''')
    write(src,code)
    write(stage+'/wifi/lx/src/nd6_observe.h',(HERE/'nd6_observe.h').read_text())
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'data-nd-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],stdout=log,stderr=subprocess.STDOUT,check=True)
    linked=equip(subprocess.check_output(['docker','exec',BUS,'cat',work+'out/ath11k.o']))
    dest=OUT/'ath11k-cell-data-nd.o'; temp=dest.with_suffix('.new')
    fd=os.open(temp,os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600)
    with os.fdopen(fd,'wb') as f: f.write(linked)
    os.replace(temp,dest)
    (OUT/'data-nd-build.json').write_text(json.dumps({'parent':PRIOR,'sha256':sha(dest),'object':str(dest),
        'container_tree':stage,'builder':sha(Path(__file__))},indent=2)+'\n')
    print(sha(dest),dest)
if __name__=='__main__': build()
