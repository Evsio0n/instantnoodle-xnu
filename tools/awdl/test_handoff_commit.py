"""Exercise both production scheduling passes, including actual carrier selection."""
import json,re,subprocess,tempfile
from pathlib import Path
from build_roc import OUT,read
from test_discovery_fairness import shim,exercise,function
from build_wmi_handoff_commit import BASE

candidate=json.loads((OUT/'wmi-handoff-commit-build.json').read_text())['stage']
pre=shim.replace('uint8_t data_mac[6];','uint8_t data_mac[6],eui64[16],observed_ipv6[16]; unsigned has_observed_ipv6,last_rx_mhz,data_address_matches;').replace('unsigned wanted,channel;', 'uint8_t local[6]; unsigned wanted,channel,active_slot,mhz;')
pre+='''
static uint8_t active_peer_ip[16];
static int lx_awdl_peer_select(const struct peer_table *t,const uint8_t *m,uint64_t n){(void)t;(void)m;(void)n;return 0;}
static unsigned auto_phy_chan(const struct lx_awdl_peer *p,uint64_t n){(void)n;return p->expired?0:48;}
static unsigned auto_freq(unsigned ch){return 5000+ch*5;}
static void lx_tell(const char *msg,...){(void)msg;}
'''
with tempfile.TemporaryDirectory() as tmp:
 for stage,fixed in [(BASE,False),(candidate,True)]:
  src=stage+'/wifi/lx/src/'; owner=read(src+'auto_owner.inc');peers=read(src+'auto_peers.inc')
  pick=peers.split('static bool auto_pick(void)',1)[1].split('static bool auto_lane_target',1)[0]
  outer=re.search(r'([^\n]*auto_schedule_queued\(\);)\n\s*if\(auto_pick\(\)\)',owner)[1]
  code='#define FIXED 1\n'+pre+read(src+'discovery_fairness.inc')+function(owner,'auto_schedule_queued')+'\n'
  if fixed:code+=owner[owner.index('static bool auto_pending_handoff('):owner.index('static bool auto_window(')]
  code+='static bool auto_pick(void)'+pick+exercise.split('int main(void)')[0]
  code+='static void acquire(void){'+outer+' assert(auto_pick());}\n'
  code+='''
int main(void){
 reset();put(0,2,1);step();now+=2500000;step();
 assert(air.wanted_serial==8);air.active=false;acquire();
 assert(air.active_serial==EXPECTED && !air.wanted_serial && !locked);
 reset();put(0,2,1);step();now+=2500000;step();air.active=false;
 air.table.peers[1].expired=true;acquire();assert(air.active_serial==7);
 reset();put(0,2,1);step();now+=2500000;step();air.active=false;
 air.table.peers[1].serial=99;acquire();assert(air.active_serial==7);
}
'''.replace('EXPECTED','8' if fixed else '7')
  p=Path(tmp);(p/'test.c').write_text(code)
  subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
print('PASS control reselects old peer; candidate activates requested peer; expired and reused slots re-evaluated')
