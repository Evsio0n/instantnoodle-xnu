"""Mirror the locally owned DNS-SD registration through change and withdrawal."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-registration-6K8niI'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='01d0769e4c541194327f996a709026f588960a235eb50a56cf945cd07123c975'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-native-address-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'auto_owner.inc')
s=once(s,'static void auto_owner(', 'extern int lx_os_awdl_initial_address(uint8_t address[6]);\nstatic void auto_owner(')
s=once(s,'const uint8_t *address=NULL;','''uint8_t requested[6]={0};
    const uint8_t *address=NULL;
    int address_error=lx_os_awdl_initial_address(requested);
    if(!address_error && lx_peer_unicast(requested) && memcmp(requested,r->vif->addr,6))
        address=requested;
    lx_tell("awdl-address: native_owner=%u rendezvous_errno=%d\\n",address!=NULL,address_error);''')
s=once(s,'for(unsigned i=1;i<w->n_addresses;i++)','for(unsigned i=1;!address && i<w->n_addresses;i++)')
write(src+'auto_owner.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-native-address-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-native-address.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
