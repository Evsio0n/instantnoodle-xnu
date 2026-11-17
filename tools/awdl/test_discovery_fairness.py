"""Replay starvation against the actual control and candidate schedulers.

Keep appending a fresh active-peer copy behind an aged discovery copy and
an unrelated head entry. Verify bounded selection without packet changes,
then exercise the firmware-flight, unicast, lease and carrier-slice cases.
No RF operations are performed by this test.
"""
import json
from pathlib import Path
import subprocess
import tempfile
from build_roc import OUT,read

r=json.loads((OUT/'wmi-discovery-fairness-build.json').read_text())


def function(source,name):
    at=source.index('static void '+name+'(');brace=source.index('{',at)
    end=brace+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[at:end]


shim=r'''
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#define LIVE_DEPTH 8u
#define LX_AWDL_PEER_SLOTS 8u
#define LP_MDNS 4u
struct live_frame {unsigned proto,tag;uint64_t queued_us;uint8_t link_target[6],bytes[80];};
struct live_fifo {unsigned first,count;struct live_frame slot[LIVE_DEPTH];};
struct lx_awdl_peer {uint64_t serial;bool seen_mif,expired;unsigned version,device_class;uint8_t data_mac[6];};
struct peer_table {struct lx_awdl_peer peers[LX_AWDL_PEER_SLOTS];};
static struct {struct peer_table table;unsigned wanted,channel;bool active;
 uint64_t active_serial,wanted_serial,unicast_until_us;} air;
static struct {struct live_fifo tx,groups;uint64_t generation;unsigned dropped;} live;
static uint8_t active_peer_mac[6]={2};
static uint64_t now;static unsigned flights,locked;
static uint64_t lx_os_nanoseconds(void){return now*1000;}
static void lx_os_lock(void){assert(!locked);locked=1;}
static void lx_os_unlock(void){assert(locked);locked=0;}
static int auto_find_locked(const uint8_t *mac,uint64_t t){
 (void)t;assert(locked);
 for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++)
  if(air.table.peers[i].serial && !air.table.peers[i].expired &&
     !memcmp(mac,air.table.peers[i].data_mac,6))return (int)i;
 return -1;
}
static unsigned lx_awdl_peer_expired(const struct peer_table *t,uint64_t n){
 (void)n;unsigned result=0;for(unsigned i=0;i<LX_AWDL_PEER_SLOTS;i++)if(t->peers[i].expired)result|=1u<<i;return result;
}
static bool lx_peer_unicast(const uint8_t *m){return !(m[0]&1) && m[0];}
static unsigned live_count_flights_locked(uint64_t lease){(void)lease;assert(locked);return flights;}
/* The unicast policy is independently tested by test_peer_queue.c. */
static unsigned live_peer_choice(const struct live_fifo *q,uint64_t t){(void)q;(void)t;return 0;}
static void live_pop(struct live_fifo *q){q->first=(q->first+1)%LIVE_DEPTH;q->count--;}
'''
exercise=r'''
static void put(unsigned n,unsigned target,unsigned proto){
 struct live_frame *f=&live.groups.slot[(live.groups.first+n)%LIVE_DEPTH];
 memset(f,0,sizeof(*f));f->tag=n+1;f->proto=proto;f->queued_us=1000000;
 f->link_target[0]=target;memset(f->bytes,(int)(n+17),sizeof(f->bytes));
}
static void reset(void){
 memset(&live,0,sizeof(live));memset(&air,0,sizeof(air));flights=0;locked=0;now=1000000;
 air.active=true;air.active_serial=7;air.channel=48;live.generation=3;
 for(unsigned i=0;i<2;i++){
  air.table.peers[i].serial=7+i;air.table.peers[i].seen_mif=true;
  air.table.peers[i].version=1;air.table.peers[i].device_class=2;
  air.table.peers[i].data_mac[0]=2+2*i;
 }
 live.groups.first=7;live.groups.count=3;
 put(0,4,1); /* Non-mDNS head must not hide the aged discovery behind it. */
 put(1,4,LP_MDNS);put(2,2,LP_MDNS);
#if FIXED
 discovery_slice_generation=discovery_slice_serial=discovery_slice_since=0;
 discovery_slice_channel=0;
#endif
}
static void step(void){
 live.groups.slot[(live.groups.first+2)%LIVE_DEPTH].queued_us=now;
 air.unicast_until_us=now+10000000ull;
 struct live_fifo before=live.groups;
 auto_schedule_queued();
 assert(!locked && !live.dropped);
 assert(!memcmp(&before,&live.groups,sizeof(before))); /* Routing only. */
}
int main(void){
 reset();step();unsigned selected_at=0;
 for(unsigned i=1;i<=80;i++){
  now=1000000ull+250000ull*i;step();
  if(air.wanted_serial){selected_at=i;break;}
 }
#if FIXED
 assert(selected_at && selected_at<=9);
 assert(air.wanted==1 && air.wanted_serial==8);
#else
 assert(!selected_at && !air.wanted_serial);
#endif
 reset();step();now=4000000;flights=1;step();assert(!air.wanted_serial);
 reset();air.active=false;step();assert(air.wanted_serial==8);
 reset();step();now=4000000;live.tx.count=1;live.tx.slot[0].link_target[0]=2;
 step();assert(!air.wanted_serial); /* Native unicast remains first. */
 reset();step();now=4000000;air.table.peers[1].expired=true;
 step();assert(!air.wanted_serial); /* Do not fabricate a stale recipient. */
 reset();step();now=4000000;
 live.groups.slot[(live.groups.first+1)%LIVE_DEPTH].queued_us=now+1;
 step();assert(!air.wanted_serial);
#if FIXED
 /* New generation and a physical carrier change get a fresh slice. */
 reset();step();now=4000000;live.generation++;step();assert(!air.wanted_serial);
 now+=1048576;step();assert(air.wanted_serial==8);
 reset();step();now=4000000;air.channel=149;step();assert(!air.wanted_serial);
 now+=1048576;step();assert(air.wanted_serial==8);
 puts("PASS candidate: aged other-peer discovery selected behind MLD/fresh active copies; payload immutable; unicast, firmware flight, expiry and generation/channel slice checks");
#else
 puts("PASS control reproduction: fresh active-peer copies starve the other peer throughout a 20-second replay");
#endif
 return 0;
}
'''
if __name__=='__main__':
    control='/tmp/awdl-social-owner-wczfAl/wifi/lx/src/'
    candidate=r['stage']+'/wifi/lx/src/'
    with tempfile.TemporaryDirectory(prefix='awdl-discovery-fairness-') as folder:
        folder=Path(folder)
        for name,src,fixed in [('control',control,False),('candidate',candidate,True)]:
            code='#define FIXED '+str(int(fixed))+'\n'+shim
            if fixed:code+=read(src+'discovery_fairness.inc')+'\n'
            else:code+=read(src+'GroupDeadline.h')+'\n'
            code+=function(read(src+'auto_owner.inc'),'auto_schedule_queued')+'\n'+exercise
            c=folder/(name+'.c');binary=folder/name;c.write_text(code)
            subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
