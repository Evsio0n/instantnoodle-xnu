"""Carry the selected discovery peer into its first directed MIF per grant."""
import subprocess
from build_roc import BUS,OUT,read,write,once,sha
base='/tmp/awdl-peer-visits-PAPn7G'
obj='/xnu-deps/build/ath11k/out/ath11k.o'
assert subprocess.check_output(['docker','exec',BUS,'sha256sum',base+obj],text=True).split()[0]=='0ca2e17c7063e1b38fcc48deb71612c5f859452db36856783bb870ab757bffa6'
stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-destination-XXXXXX'],text=True).strip()
subprocess.run(['docker','exec',BUS,'cp','-a',base+'/.',stage+'/'],check=True)
src=stage+'/wifi/lx/src/'
s=read(src+'live_transport.inc')
s=once(s,'static void live_discovery(','''static uint64_t visit_serial, last_visit_sent;
static unsigned visit_channel,visit_epoch,mgmt_visit_epoch;
static uint64_t mgmt_visit_peer;
static void live_discovery(''')
s=once(s,'unsigned subtype=live.next_mif?3:0;', '''unsigned subtype=live.next_mif?3:0;
    uint8_t destination[6];
    bool visit_mif=false;
    unsigned channel=channel_of(ch->center_freq);
    if(visit_serial && visit_epoch!=last_visit_sent && channel==visit_channel){
        lx_os_lock();
        for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++){
            const struct lx_awdl_peer *p=&air.table.peers[i];
            struct awdl_window window;
            if(p->serial==visit_serial && p->seen_mif && awdl_peer_project(p,now,8,&window) &&
               window.channel==channel && window.in_guarded_interval){
                memcpy(destination,p->mac,6);visit_mif=true;subtype=3;break;
            }
        }
        lx_os_unlock();
    }''')
s=once(s,'if(subtype==3 && (++mif_count&1)', '''if(visit_mif){memcpy(data+4,destination,6);directed=true;}
        else if(!visit_serial && subtype==3 && (++mif_count&1)''')
s=once(s,'memcpy(p,data,n);live.seq++;','''memcpy(p,data,n);live.seq++;
    ES(mgmt_visit_epoch,visit_mif?visit_epoch:0);
    ES(mgmt_visit_peer,visit_mif?visit_serial:0);
    if(visit_mif){
        last_visit_sent=visit_epoch;
        lx_tell("awdl-mif-tx: epoch=%u peer=%llu ch=%u issue=%llu dst=%02x%02x%02x%02x%02x%02x\\n",
                visit_epoch,visit_serial,channel,now,p[4],p[5],p[6],p[7],p[8],p[9]);
    }''')
write(src+'live_transport.inc',s)
s=read(src+'roc.inc')
s=once(s,'discovery_target=best;discovery_channel=winner;', '''discovery_target=best;discovery_channel=winner;
    visit_serial=best.serial;visit_channel=winner;++visit_epoch;''')
s=once(s,'unsigned count=++announcements;', '''unsigned count=++announcements;
        if(EG(mgmt_visit_epoch))lx_tell("awdl-mif-done: epoch=%u peer=%llu status=%u time=%llu\\n",
            EG(mgmt_visit_epoch),EG(mgmt_visit_peer),status,lx_os_nanoseconds()/1000ull);''')
s=once(s,'uint64_t opened=ROC_GET(slot_opened),closed=ROC_GET(slot_closed);','''uint64_t opened=ROC_GET(slot_opened),closed=ROC_GET(slot_closed);
    lx_tell("awdl-grant: token=%u ch=%u submit=%llu start=%u foreign=%u bss=%u done=%u reason=%u rc=%d open=%llu close=%llu end=%llu\\n",
       ROC_GET(r->request_seq),c,receipt_begin,ROC_GET(r->started),ROC_GET(r->foreign),ROC_GET(r->bss),
       ROC_GET(r->done),ROC_GET(r->reason),rc,opened,closed,lx_os_nanoseconds()/1000ull);''')
write(src+'roc.inc',s)
p=src+'auto_owner.inc';write(p,once(read(p),'discovery_channel=0;discovery_target.serial=0;', 'discovery_channel=0;discovery_target.serial=0;visit_serial=0;visit_channel=0;'))
subprocess.run(['docker','exec',BUS,'rm',stage+'/xnu-deps/build/ath11k/obj/lx_lx_80211.o'],check=True)
with (OUT/'wmi-destination-build.log').open('w') as log:
    subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
dest=OUT/'ath11k-wmi-destination.o'
subprocess.run(['docker','cp',BUS+':'+stage+obj,str(dest)],check=True)
print(stage,sha(dest))
