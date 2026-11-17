"""Compatibility A/B: sync and service parameters lead each action frame.

Some Apple's chipset parsers validate those first two TLVs before forwarding
an action frame. Keep every TLV body, clock, identity and grant unchanged;
move the existing service-parameters block immediately behind synchronization.
This is not a claim that all current Apple receivers require that order.
"""
import json
import subprocess
from build_roc import BUS,OUT,read,write,once,sha
BASE='/tmp/awdl-group-deadline-pnUpu4'
BASE_SHA='47242fd4dd3c8ff0c3743dd39dc5319565ca08da03f78e2da813a365e8fb7d14'


def build():
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',BASE+'/xnu-deps/build/ath11k/out/ath11k.o'],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('group-deadline control changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-tlv-order-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';s=read(src+'awdl_discovery.h')
    block='    (void)psf_tlv(&at, 6, 9); /* no advertised services */\n'
    s=once(s,block,'')
    s=once(s,'    lx_awdl_discovery_sequence(sync + 33, current_slot, slots, channel);',
        '    lx_awdl_discovery_sequence(sync + 33, current_slot, slots, channel);\n'+
        '    /* Chipset admission may inspect the first pair before the generic parser. */\n'+block.rstrip())
    write(src+'awdl_discovery.h',s)
    work=stage+'/xnu-deps/build/ath11k/'
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'wmi-tlv-order-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-tlv-order.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    result=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,
                first_tlvs=[4,6],prior_first_tlvs=[4,5],tlv_bodies_unchanged=True,hardware_verified=False)
    (OUT/'wmi-tlv-order-build.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result),flush=True)


if __name__=='__main__':build()
