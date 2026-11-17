#define main packet_adaptation_fixture
#include "test_mdns_unicast.c"
#undef main
#define LIVE_DEPTH 32u
#define LIVE_MTU 1500u
#define LX_AWDL_PEER_SLOTS 8u
#define LP_MDNS 4u
struct live_frame {unsigned tag,proto;uint64_t queued_us,lease;size_t n;uint8_t link_target[6],bytes[1514];};
struct live_fifo {unsigned first,count;struct live_frame slot[LIVE_DEPTH];};
struct mdns_refresh_cache {uint64_t serial,lease,wire_us,next_us;size_t n;uint8_t bytes[1514];};
struct lx_awdl_peer {uint64_t serial;bool has_observed_ipv6;uint8_t data_mac[6],observed_ipv6[16],eui64[16];};
static struct {bool active,prepeer;unsigned active_slot;uint64_t active_serial;struct {struct lx_awdl_peer peers[8];}table;} air;
static struct {bool accept,stop;uint64_t lease;unsigned next_tag;struct live_fifo tx;} live;
static struct mdns_refresh_cache mdns_refresh[8];
static uint64_t mdns_transfer_serial[8],mdns_transfer_until[8],mdns_query_serial[8],mdns_query_until[8];
static unsigned mdns_refresh_sent;
static uint64_t now=100000000;
static bool fresh=true;
static bool auto_active_fresh(void){return fresh;}
static uint64_t lx_os_nanoseconds(void){return now*1000;}
static void lx_os_lock(void){}
static void lx_os_unlock(void){}
static void lx_tell(const char *s,...){(void)s;}
static bool live_put(struct live_fifo *q,uint64_t lease,const uint8_t *bytes,size_t n){
 if(q->count==LIVE_DEPTH)return false;
 struct live_frame *f=&q->slot[(q->first+q->count)%LIVE_DEPTH];f->lease=lease;f->n=n;memcpy(f->bytes,bytes,n);q->count++;return true;
}
#include "mdns_refresh.inc"
static void setup(void){
 memset(&air,0,sizeof(air));memset(&live,0,sizeof(live));memset(mdns_refresh,0,sizeof(mdns_refresh));
 air.active=true;air.active_serial=44;air.table.peers[0].serial=44;memcpy(air.table.peers[0].data_mac,peer_mac,6);memcpy(air.table.peers[0].eui64,peer_ip,16);
 live.accept=true;live.lease=7;mdns_refresh[0].serial=44;mdns_refresh[0].lease=7;mdns_refresh[0].wire_us=now-1000;
 uint8_t p[100];mdns_refresh[0].n=question(p);memcpy(mdns_refresh[0].bytes,p,mdns_refresh[0].n);
 mdns_transfer_serial[0]=44;mdns_transfer_until[0]=now+15000000;fresh=true;
}
int main(void){
 setup();uint64_t original_epoch=mdns_refresh[0].wire_us;uint8_t original[1514];memcpy(original,mdns_refresh[0].bytes,mdns_refresh[0].n);
 mdns_refresh_tick();assert(live.tx.count==1);assert(mdns_refresh[0].wire_us==original_epoch);assert(!memcmp(original,mdns_refresh[0].bytes,mdns_refresh[0].n));
 struct live_frame *f=&live.tx.slot[0];assert(f->proto==LP_MDNS && f->lease==7);assert(!memcmp(f->link_target,peer_mac,6));assert(awdl_wire_checksum(f->bytes+14,f->n-14)==1);
 assert(!memcmp(f->bytes+62,original+62,f->n-62));
 live.tx.count=0;mdns_refresh_tick();assert(!live.tx.count);
 setup();live.tx.count=1;unsigned tag=live.next_tag;mdns_refresh_tick();assert(live.tx.count==1 && live.next_tag==tag);
 setup();mdns_transfer_until[0]=now;mdns_refresh_tick();assert(!live.tx.count);
 setup();mdns_refresh[0].wire_us=now-30000000;mdns_refresh_tick();assert(!live.tx.count);
 setup();mdns_refresh[0].lease=8;mdns_refresh_tick();assert(!live.tx.count);
 setup();mdns_transfer_serial[0]=45;mdns_refresh_tick();assert(!live.tx.count);
 setup();fresh=false;mdns_refresh_tick();assert(!live.tx.count);
 setup();live.stop=true;mdns_refresh_tick();assert(!live.tx.count);
 return 0;
}
