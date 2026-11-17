"""Replay actual old/new discovery admission at an early-grant rendezvous."""
import json
import subprocess
import tempfile
from pathlib import Path
from build_roc import OUT,read

candidate=json.loads((OUT/'wmi-visit-reserved-build.json').read_text())
old=read(candidate['base']+'/wifi/lx/src/live_transport.inc')
new=read(candidate['stage']+'/wifi/lx/src/live_transport.inc')
def prefix(s):
 a=s.index('static void live_discovery(')
 b=s.index('\tunsigned n=(unsigned)lx_awdl_discovery_action(',a)
 return s[a:b]+'''(void)data;(void)cap;(void)w;(void)destination;(void)subtype;(void)v;
 emitted++;directed+=visit_mif;live.last_action_us=now;
 if(visit_mif)last_visit_sent=visit_epoch;
 }'''
stub=r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#define EG(x) (x)
#define LX_AWDL_DISCOVERY_MAX 289
#define LX_AWDL_PEER_SLOTS 1
struct ieee80211_hw {int n;};
struct ieee80211_sta_ht_cap {unsigned cap,ampdu_density,ampdu_factor;struct {unsigned rx_mask[1];} mcs;bool ht_supported;};
struct band {struct ieee80211_sta_ht_cap ht_cap;};
struct phy {struct band *bands[1];};
struct lx_radio {struct ieee80211_hw hw;struct {uint64_t foreign_us;} roc;struct phy *wiphy;};
struct ieee80211_vif {int n;};
struct linuxkpi_ieee80211_channel {unsigned center_freq,band;};
struct lx_awdl_discovery_window {uint64_t now_us,anchor_us,grant_end_us;unsigned ieee_sequence,af_period_tu;};
struct lx_awdl_discovery_ht {unsigned capabilities,ampdu_parameters,rx_mcs0;};
struct awdl_window {unsigned channel;bool in_guarded_interval;};
struct lx_awdl_peer {uint64_t serial;unsigned seen_mif;uint8_t mac[6];};
static struct {struct {struct lx_awdl_peer peers[1];} table;} air;
static struct {bool mgmt_pending,mgmt_frame;uint64_t last_action_us,anchor_us;unsigned seq,next_mif;} live;
static uint64_t visit_serial=7,last_visit_sent=0,visit_until=1050000,now_us=1000000;
static unsigned visit_epoch=1,visit_channel=6,emitted,directed;
static bool granted=true;
static uint64_t lx_os_nanoseconds(void){return now_us*1000;}
static bool lx_awdl_data_foreign(struct ieee80211_hw *h){(void)h;return granted;}
static unsigned lx_awdl_home_basis(struct ieee80211_hw *h){(void)h;return 1;}
static void follow_announcement(struct lx_awdl_discovery_window *w,unsigned c){(void)w;(void)c;}
static unsigned channel_of(unsigned f){assert(f==2437);return 6;}
static void lx_os_lock(void){}
static void lx_os_unlock(void){}
static bool awdl_peer_project(const struct lx_awdl_peer *p,uint64_t now,unsigned guard,struct awdl_window *w){
 (void)p;assert(guard==8);w->channel=6;w->in_guarded_interval=now>=1008000 && now<1050000;return true;
}
'''
tail=r'''
int main(void){
 struct band b={0};struct phy phy={{&b}};struct lx_radio r={.roc={1000000},.wiphy=&phy};
 struct ieee80211_vif v={0};struct linuxkpi_ieee80211_channel ch={2437,0};
 air.table.peers[0]=(struct lx_awdl_peer){.serial=7,.seen_mif=1};
 /* Grant arrives eight ms before the remote guarded interval. */
 live_discovery(&r,&v,&ch);
#if NEW
 assert(!emitted);  /* preserve the rendezvous rather than broadcasting early */
#else
 assert(emitted==1 && !directed);
#endif
 now_us=1010000;live_discovery(&r,&v,&ch);
#if NEW
 assert(emitted==1 && directed==1 && last_visit_sent==visit_epoch);
 now_us=1012000;live_discovery(&r,&v,&ch);assert(emitted==1); /* no repeat bypass */
 visit_epoch++;live.last_action_us=1009000;now_us=1020000;
 live_discovery(&r,&v,&ch);assert(emitted==2 && directed==2); /* prior frame cannot steal new visit */
 visit_epoch++;now_us=1030000;granted=false;
 live_discovery(&r,&v,&ch);assert(emitted==2); /* no grant, no frame */
 granted=true;live.mgmt_pending=true;live_discovery(&r,&v,&ch);assert(emitted==2);
 live.mgmt_pending=false;now_us=1060000;live.last_action_us=0;
 live_discovery(&r,&v,&ch);assert(emitted==3 && directed==2); /* missed plan does not block later broadcast */
#else
 assert(emitted==1 && !directed); /* old pacing suppresses the entire rendezvous */
 (void)visit_until;
#endif
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='awdl-visit-reservation-') as d:
 for version,source in ((0,old),(1,new)):
  c=Path(d)/f'test{version}.c';exe=c.with_suffix('');c.write_text(stub+prefix(source)+tail)
  subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',f'-DNEW={version}',str(c),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True)
print('PASS actual old prefix wastes early grant and suppresses MIF; new prefix reserves window, sends once, preserves grant/pending/pacing gates')
