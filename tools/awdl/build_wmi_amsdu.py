"""HT multicast-to-unicast with a retained logical multicast MSDU DA."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-wmi-fanout-fi8ce5'
BASE_SHA='8b8dcbd478aafe3e21b98eeb3ae43f4ef19765431b20217ff9fa68fe8bbda188'
def build():
 obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
 actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]
 if actual!=BASE_SHA:raise ValueError('long fanout base changed')
 stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-wmi-amsdu-XXXXXX'],text=True).strip();subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
 src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/';driver=work+'src/ath11k/'
 write(src+'amsdu_wire.h',(HERE/'amsdu_wire.h').read_text())
 s=read(src+'live_transport.inc');s=once(s,'#include "lx_awdl_transport.h"','#include "lx_awdl_transport.h"\n#include "amsdu_wire.h"')
 s=once(s,'uint8_t *frame;struct sk_buff *skb=frame_new(r,(unsigned)(40+bytes),&frame);','bool amsdu=f->group && air.table.peers[air.active_slot].has_ht;\n\tuint8_t *frame;struct sk_buff *skb=frame_new(r,(unsigned)((amsdu?56:40)+bytes),&frame);')
 marker='f->wire_group=!!(frame[4]&1);'
 s=once(s,marker,'''if(amsdu && !lx_awdl_amsdu_ipv6(frame,56+bytes,live.scratch.bytes,live.scratch.n,v->addr,live.scratch.link_target,(uint16_t)seq)) {
        dev_kfree_skb_any(skb);ES(f->free_path,2);ES(f->free_us,now);ES(f->freed,1);return;
    }
    const uint8_t *wire_ip=frame+(amsdu?56:40);
    '''+marker)
 s=once(s,'frame[64]==0xff','wire_ip[24]==0xff')
 s=once(s,'const uint8_t *ip = frame + 40;','const uint8_t *ip = wire_ip;')
 s=once(s,'if(f->group)lx_tell("awdl-wmi-data: multicast_unicast_copy', 'if(amsdu)lx_tell("awdl-wmi-data: ht_amsdu_multicast ticket=%u\\n",f->tag);\n        if(f->group)lx_tell("awdl-wmi-data: multicast_unicast_copy')
 write(src+'live_transport.inc',s)
 w=read(driver+'wmi.c');w=once(w,'params->tx_params_dword1=tx_params_valid?0x00108400u:0x00100000u;', '''params->tx_params_dword1=tx_params_valid?0x00108400u:0x00100000u;
            if(tx_params_valid && frame->len>=26 && frame->data[0]==0x88 && (frame->data[24]&0x80)) {
                /* HT20 MCS0 is mandatory for HT recipients. */
                params->tx_params_dword0=0x40100100u;
                params->tx_params_dword1=0x00120400u;
            }''');write(driver+'wmi.c',w)
 subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o',work+'obj/ath11k_wmi.o'],check=True)
 with (OUT/'wmi-amsdu-build.log').open('w') as log:subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
 dest=OUT/'ath11k-wmi-amsdu.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
 r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,ht_amsdu_multicast=True,offchan_data_wmi=True,additional_peer=False,fixed_endpoint=False,explicit_country=True,lifetime_seconds=900)
 (OUT/'wmi-amsdu-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
