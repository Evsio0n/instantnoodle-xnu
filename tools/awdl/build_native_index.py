"""Native-format NoDS control using the existing peer AST index.

HAL documents SEARCH_INDEX as effective only for index search. The inherited
station default is address-Y content search, despite carrying a mapped index.
Keep the country-layout native radio; change only the secondary data lookup.
"""
import json,subprocess
from build_roc import BUS,OUT,read,write,once,sha
BASE='/tmp/awdl-country-idvVy7'
BASE_SHA='44ef2c448c6c966d40c585ef0cbf55bf234e420fe91ce5a78cdc0bfdac772226'
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('native country-layout base changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-native-index-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    work=stage+'/xnu-deps/build/ath11k/'
    path=work+'src/ath11k/dp_tx.c';s=read(path)
    s=once(s,'lx_awdl_data_profile(ar,arvif,arsta,&ti);', '''if(arsta && lx_awdl_descriptor_role(ar->hw,arvif->vif)==2) {
        ti.search_type=HAL_TX_ADDR_SEARCH_INDEX;
        ti.addr_search_flags=HAL_TX_ADDRX_EN;
        static unsigned announced;
        if(!__atomic_exchange_n(&announced,1,__ATOMIC_RELAXED))
            lx_tell("awdl-auto: native_mapped_index=1 secondary_only=1 extra_peer=0\\n");
    }
    lx_awdl_data_profile(ar,arvif,arsta,&ti);''')
    write(path,s)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/ath11k_dp_tx.o'],check=True)
    with (OUT/'native-index-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-native-index.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,native_mapped_index=True,additional_peer=False,fixed_endpoint=False,native_wifi=True,explicit_country=True,lifetime_seconds=420)
    (OUT/'native-index-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
