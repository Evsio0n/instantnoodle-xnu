"""Mirror the locally owned DNS-SD registration through change and withdrawal."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-native-address-NA1SUA'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='6cbf8f8ce0d0593c384f4ba0a973236754c922622a7587f4af5906fc5a10c351'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-native-service-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'live_transport.inc')
s=once(s,'static void live_discovery(', 'extern unsigned lx_os_awdl_service_bytes(uint64_t lease,uint8_t *out,unsigned cap);\nstatic void live_discovery(')
s=once(s,'n+=extra+detail_bytes;','''unsigned native=lx_os_awdl_service_bytes(live.lease,data+n,sizeof(data)-n);
    if(native){
        static unsigned count;
        if(count++<10)lx_tell("awdl-offload: action bytes=%u channel=%u\\n",native,channel_of(ch->center_freq));
        n+=native;
    } else n+=extra+detail_bytes;''')
write(src+'live_transport.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-native-service-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-native-service.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
