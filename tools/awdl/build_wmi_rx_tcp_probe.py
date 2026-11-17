"""Retain target TCP metadata before each existing receive admission decision."""
import json,re,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
parent=json.loads((OUT/'wmi-censored-lead-build.json').read_text())
base=parent['stage'];obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]==parent['sha256']
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-rx-tcp-probe-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/';write(src+'rx_tcp_probe.h',(HERE/'rx_tcp_probe.h').read_text())
p=src+'data.inc';s=read(p)
s=once(s,'static bool lx_awdl_echo_receive(','''#include "rx_tcp_probe.h"
static unsigned rx_tcp_probe_rows;
static void rx_tcp_probe_emit(const struct hk_rx_tcp_probe *p,const struct ieee80211_rx_status *s,int stage)
{
    if(!p->valid)return;
    unsigned row=__atomic_add_fetch(&rx_tcp_probe_rows,1,__ATOMIC_RELAXED);
    if(row>2048)return;
    lx_tell("awdl-rx-path: n=%u us=%llu stage=%d freq=%u status=%llx src=%u dst=%u seq=%u ack=%u flags=%u payload=%u route_known=%u\\n",
        row,lx_os_nanoseconds()/1000ull,stage,s->freq,(unsigned long long)s->flag,
        p->src,p->dst,p->seq,p->ack,p->flags,p->payload,auto_route(p->source,NULL));
}
static bool rx_tcp_probe_drop(unsigned reason,bool data,const struct hk_rx_tcp_probe *p,const struct ieee80211_rx_status *s)
{
    rx_tcp_probe_emit(p,s,(int)reason);return rx_boundary_drop(reason,data);
}
static bool lx_awdl_echo_receive(''')
a=s.index('static bool lx_awdl_echo_receive(');b=s.index('\nstatic struct ieee80211_vif *lookup_vif;',a);rx=s[a:b]
rx=once(rx,'bool audit_data=rx_boundary_begin(p,n,!!(s->flag&RX_FLAG_8023));','''bool audit_data=rx_boundary_begin(p,n,!!(s->flag&RX_FLAG_8023));
    struct hk_rx_tcp_probe probe=hk_probe_tcp(p,n,!!(s->flag&RX_FLAG_8023),echo.ip);''')
rx,n=re.subn(r'rx_boundary_drop\((RXA_\w+),audit_data\)',r'rx_tcp_probe_drop(\1,audit_data,&probe,s)',rx);assert n==7,n
rx=once(rx,'    return accepted;', '    rx_tcp_probe_emit(&probe,s,accepted?-1:-2);\n    return accepted;')
write(p,s[:a]+rx+s[b:])
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-rx-tcp-probe-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-rx-tcp-probe.o';subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-rx-tcp-probe-build.json').write_text(json.dumps(receipt,indent=2));print(json.dumps(receipt,indent=2))
