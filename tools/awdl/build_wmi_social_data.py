"""Serve native data in the peer windows advertised on social channels."""
import subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
base='/tmp/awdl-sync-election-pO26ds'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='53da35226e23c37135be68bbb187930fda59b3b3f34b8f83e26bb1e36b307950'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-social-data-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'auto_owner.inc')
s=once(s,'if (lx_awdl_data_foreign(&r->hw) && auto_channel(r,channel,true)) live_discovery(r,v,ch);', '''if (lx_awdl_data_foreign(&r->hw) && auto_channel(r,channel,true)) {
            live_discovery(r,v,ch);
            /* If the action frame announces attendance here, also serve
             * the active peer during its guarded window on this channel. */
            if (bw.vif==v && bw.control_id==echo.vdev && air.active && !air.prepeer)
                live_data(r,v,ch,1);
        }''')
write(src+'auto_owner.inc',s)
s=read(src+'live_transport.inc')
s=once(s,'if(lx_awdl_home_basis(hw)==3)return air.mhz;',
    'if(EG(bw.armed) && bw.control_id==echo.vdev && bw.own_mhz==air.listen_mhz)return bw.own_mhz;\n    if(lx_awdl_home_basis(hw)==3)return air.mhz;')
s=once(s,'live_bound(hw,skb,vdev,LIVE_WMI_DATA_RING,desc,0);','''if(EG(bw.armed) && bw.control_id==vdev && frequency==bw.own_mhz && frequency!=air.mhz)
        lx_tell("awdl-social: native_data ticket=%u frequency=%u peer=%llu\\n",f->tag,frequency,air.active_serial);
    live_bound(hw,skb,vdev,LIVE_WMI_DATA_RING,desc,0);''')
write(src+'live_transport.inc',s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-social-data-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-social-data.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
