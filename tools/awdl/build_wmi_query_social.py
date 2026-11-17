"""Execute actual native SET120 browse keys through software IPv6 mDNS."""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
parent=json.loads((OUT/'wmi-registered-query-build.json').read_text())
base=parent['stage'];obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]==parent['sha256']
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-query-social-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
p=src+'auto_owner.inc';s=read(p)
s=once(s,'if (lx_awdl_data_foreign(&r->hw) && auto_channel(r,channel,true)) live_discovery(r,v,ch);', '''if (lx_awdl_data_foreign(&r->hw) && auto_channel(r,channel,true)) {
            live_discovery(r,v,ch);
            /* Service queued native data on this actual secondary-VDEV
             * grant too. live_data retains real peer/window admission. */
            if(bw.vif==v && bw.control_id==echo.vdev && air.active && !air.prepeer)
                live_data(r,v,ch,1);
        }''')
write(p,s)
p=src+'live_transport.inc';s=read(p)
s=once(s,'if(lx_awdl_home_basis(hw)==3)return air.mhz;',
    'if(EG(bw.armed) && bw.control_id==echo.vdev && bw.own_mhz==air.listen_mhz)return bw.own_mhz;\n    if(lx_awdl_home_basis(hw)==3)return air.mhz;')
write(p,s)
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-query-social-build.log').open('w') as log:
 subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-query-social.o';subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
receipt=dict(stage=stage,radio=str(dest),sha256=sha(dest),base=base)
(OUT/'wmi-query-social-build.json').write_text(json.dumps(receipt,indent=2));print(json.dumps(receipt,indent=2))
