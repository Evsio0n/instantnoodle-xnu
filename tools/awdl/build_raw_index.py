"""Isolate RAW NoDS data plus the already mapped peer's AST index.

Station address-Y content search assumes a normal BSS. The AWDL MPDU has
NoDS addresses; preserve that header and use the existing live peer mapping.
No firmware peer allocation, fixed endpoint, channel or global RAW change.
"""
import json,subprocess
from build_roc import BUS,OUT,read,write,once,sha
from follow_build import apply_raw
BASE='/tmp/awdl-directed-uV9TkF'
BASE_SHA='5c9e69cbad9653799674aaf507eef8a8c08b08d50493b01ccaa6c56f0b3e9de7'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('directed-announcement base changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-raw-index-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    apply_raw(stage)
    work=stage+'/xnu-deps/build/ath11k/'
    path=work+'src/ath11k/dp_tx.c';text=read(path)
    text=once(text,'lx_awdl_data_profile(ar,arvif,arsta,&ti);', '''if(arsta && lx_awdl_descriptor_role(ar->hw,arvif->vif)==2) {
        /* The normal owner keeps this mapped peer alive through retirement.
         * hal_tx.c puts bss_ast_idx into SEARCH_INDEX. Only the AWDL role
         * uses NoDS frames; preserve the normal primary station's lookup. */
        ti.search_type=HAL_TX_ADDR_SEARCH_INDEX;
        ti.addr_search_flags=HAL_TX_ADDRX_EN;
        static unsigned announced;
        if(!__atomic_exchange_n(&announced,1,__ATOMIC_RELAXED))
            lx_tell("awdl-auto: raw_mapped_index=1 secondary_only=1 extra_peer=0\\n");
    }
    lx_awdl_data_profile(ar,arvif,arsta,&ti);''')
    write(path,text)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o',work+'obj/ath11k_mac.o',work+'obj/ath11k_wmi.o',work+'obj/ath11k_dp_tx.o'],check=True)
    with (OUT/'raw-index-scoped-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-raw-index-scoped.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    receipt=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,additional_peer=False,fixed_endpoint=False,raw_mapped_index=True,native_receive_fix=True,lifetime_seconds=420)
    (OUT/'raw-index-scoped-build.json').write_text(json.dumps(receipt,indent=2)+'\n');print(json.dumps(receipt))
if __name__=='__main__':build()
