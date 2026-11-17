"""Read-only phase/election telemetry on the TCP-verified retune radio."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-wmi-retune-bjML6b'
BASE_SHA='4f2d699840a9b493e4d8d2d4e1f3209feec4e86211d009f0db96804889b21959'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    if subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]!=BASE_SHA:raise ValueError('retune control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-phase-audit-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    path=stage+'/wifi/lx/src/live_transport.inc';s=read(path)
    anchor='    live_bound(hw,skb,vdev,LIVE_WMI_DATA_RING,desc,0);'
    s=once(s,anchor,(HERE/'tx_phase_audit.inc').read_text()+anchor);write(path,s)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-phase-audit-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-phase-audit.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,passive_phase_audit=True,decision_changes=False,lifetime_seconds=900)
    (OUT/'wmi-phase-audit-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
