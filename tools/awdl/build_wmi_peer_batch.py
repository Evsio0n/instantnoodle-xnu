"""Drain a current peer's FIFO in bounded batches, preserving real identities."""
import json
import subprocess
from build_roc import BUS, HERE, OUT, read, write, once, sha

BASE = '/tmp/awdl-vdev-window-VY6mMD'
BASE_SHA = 'defe0922aba4be3759b2cc49ee6c5eb017a4d25fe2759c3c945309b4ac750e11'


def build():
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',BASE+'/xnu-deps/build/ath11k/out/ath11k.o'],text=True).split()[0]
    if actual!=BASE_SHA: raise ValueError('secondary ROC control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-peer-batch-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/'
    write(src+'peer_queue.inc',(HERE/'peer_queue.inc').read_text())
    live=read(src+'live_transport.inc')
    live=once(live,'static int live_open(', '#include "peer_queue.inc"\nstatic int live_open(')
    start=live.index('        unsigned offset=0;\n        if(!lane && !auto_lane_target')
    end=live.index('\n\t\tfor(unsigned j=0;j<LIVE_PER_LANE;j++)',start)
    live=live[:start]+'''        unsigned offset=live_peer_offset(q,!!lane);
        if(offset==q->count) continue;'''+live[end:]
    write(src+'live_transport.inc',live)
    owner=read(src+'auto_owner.inc')
    owner=once(owner,'struct live_frame *f=&q->slot[q->first];', '''unsigned choice=q==&live.tx?live_peer_choice(q,now):0;
        struct live_frame *f=&q->slot[(q->first+choice)%LIVE_DEPTH];''')
    # Stale chosen entries may now be in the middle of the bounded ring.
    owner=once(owner,'live_pop(q);live.dropped++;', '''if(!choice)live_pop(q);
        else {
            for(unsigned i=choice;i+1<q->count;i++)
                q->slot[(q->first+i)%LIVE_DEPTH]=q->slot[(q->first+i+1)%LIVE_DEPTH];
            q->count--;
        }
        live.dropped++;''')
    write(src+'auto_owner.inc',owner)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-peer-batch-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-peer-batch.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    result=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
                peer_batch_us=1048576,per_peer_fifo=True,secondary_data_roc=True,hardware_verified=False)
    (OUT/'wmi-peer-batch-build.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)


if __name__=='__main__':build()
