"""Retain valid current carrier across real Mac channel occupancy changes."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-browse-refresh-kOGf8J'
BASE_SHA='4cef7bc977e7629f98c2f272f56021c26a15f314b7a40680998d9b91de90f307'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    if subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]!=BASE_SHA:raise ValueError('refresh source changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-discovery-fair-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';s=read(src+'live_transport.inc')
    s=once(s,'static void live_data(', '#include \"UnicastHold.h\"\nstatic void live_data(')
    s=once(s,'if(!lane)air.unicast_until_us=now+10000000ull;', 'air.unicast_until_us=hk_awdl_unicast_hold(air.unicast_until_us,now,!lane,live.scratch.proto==LP_MDNS);')
    write(src+'UnicastHold.h',(HERE/'UnicastHold.h').read_text())
    write(src+'live_transport.inc',s)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-discovery-fair-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-discovery-fair.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,clock_transition_audit=True,retain_advertised_carrier=True,browse_query_refresh_seconds=90,discovery_does_not_extend_data_hold=True)
    (OUT/'wmi-discovery-fair-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
