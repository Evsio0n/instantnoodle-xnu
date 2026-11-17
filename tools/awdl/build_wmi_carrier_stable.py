"""Retain valid current carrier across real Mac channel occupancy changes."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-clock-audit-NGlpF4'
BASE_SHA='5ecb1d79916a83fcf68e203e01ae3af185c0712b9aefeb47d145e1d04aed7d4b'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    if subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]!=BASE_SHA:raise ValueError('refresh source changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-carrier-stable-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';s=read(src+'auto_owner.inc')
    s=once(s,(HERE/'carrier_retune.inc').read_text(),(HERE/'carrier_stable.inc').read_text())
    write(src+'auto_owner.inc',s)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-carrier-stable-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-carrier-stable.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,clock_transition_audit=True,retain_advertised_carrier=True)
    (OUT/'wmi-carrier-stable-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
