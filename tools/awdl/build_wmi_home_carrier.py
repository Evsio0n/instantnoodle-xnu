"""Prefer a real advertised infrastructure channel on new carrier selection.

Existing valid-carrier retention remains unchanged; no periodic retune or
fixed channel. The fallback is still the peer's advertised predominant
eligible channel. Every data frame retains real ROC and peer-window gates.
"""
import json
import subprocess
from build_roc import BUS, HERE, OUT, read, write, once, sha
BASE='/tmp/awdl-peer-batch-iAR085'
BASE_SHA='1b86883bdc5b12acf5ecfed23eabe4907044ba98e970671777e3f3e3a4ca3774'


def build():
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',BASE+'/xnu-deps/build/ath11k/out/ath11k.o'],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('native Mac SHA-verified control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-home-carrier-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/'
    write(src+'HomeCarrier.h',(HERE/'HomeCarrier.h').read_text())
    peers=read(src+'auto_peers.inc')
    peers=once(peers,'#include "peer_table.h"','#include "peer_table.h"\n#include "HomeCarrier.h"')
    peers=once(peers,'(void)now;unsigned best=0,score=0;', '''(void)now;unsigned best=0,score=0;
    struct lx_radio *r=air.radio;
    unsigned home=(r && r->joined && r->channel && r->channel->def.chan)?
        channel_of(r->channel->def.chan->center_freq):0;
    unsigned shared=hk_awdl_home_carrier(p->clock.slots,home,
        home && auto_channel(r,home,true));
    if(shared)return shared;''')
    write(src+'auto_peers.inc',peers)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-home-carrier-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-home-carrier.o'
    subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    result=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
                prefer_advertised_infrastructure_channel=True,fixed_channel=False,
                valid_carrier_retention_unchanged=True,hardware_verified=False)
    (OUT/'wmi-home-carrier-build.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result),flush=True)


if __name__=='__main__':build()
