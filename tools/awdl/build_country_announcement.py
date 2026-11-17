"""Native-format control with explicit country/social/address TLV layout.

The Mac's actual peer snapshot read our social-map byte as country code and
never admitted the peer. Use the explicit unknown-country form documented by
OWL, preserving radio regulatory policy and every other native-mode setting.
"""
import json,subprocess
from build_roc import BUS,HERE,OUT,write,sha
BASE='/tmp/awdl-directed-uV9TkF'
BASE_SHA='5c9e69cbad9653799674aaf507eef8a8c08b08d50493b01ccaa6c56f0b3e9de7'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('native directed-announcement base changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-country-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'
    write(stage+'/wifi/lx/src/awdl_discovery.h',(HERE/'awdl_discovery.h').read_text())
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o'],check=True)
    with (OUT/'country-announcement-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-country-announcement.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    receipt=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,additional_peer=False,fixed_endpoint=False,native_wifi=True,explicit_country=True,lifetime_seconds=420)
    (OUT/'country-announcement-build.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt))
if __name__=='__main__':build()
