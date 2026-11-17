#include <stdint.h>
#include <assert.h>
#include <string.h>
#define LIVE_DEPTH 4u
struct live_frame {uint64_t queued_us,lease;unsigned tag;uint8_t bytes[99];};
struct live_fifo {unsigned first,count;struct live_frame slot[LIVE_DEPTH];};
static struct {struct live_fifo tx,groups;unsigned dropped;} live;
static unsigned locked;
static void lx_os_lock(void){assert(!locked);locked=1;}
static void lx_os_unlock(void){assert(locked);locked=0;}
static void lx_tell(const char *format,...){(void)format;}
#include "queue_age.inc"
int main(void){
 live.tx.first=3;live.tx.count=4;
 for(unsigned i=0;i<4;i++){struct live_frame *f=&live.tx.slot[(3+i)%4];f->tag=i+1;f->lease=9;memset(f->bytes,i+17,sizeof(f->bytes));f->queued_us=(i%2)?2500000:1000000;}
 struct live_frame second=live.tx.slot[0],fourth=live.tx.slot[2];
 live_queue_age_tick(3000000);assert(!locked && live.dropped==2 && live.tx.count==2 && live.tx.first==3);
 assert(!memcmp(&live.tx.slot[3],&second,sizeof(second)));assert(!memcmp(&live.tx.slot[0],&fourth,sizeof(fourth)));
 struct live_fifo before=live.tx;live_queue_age_tick(2499999);assert(!memcmp(&before,&live.tx,sizeof(before))); /* clock underflow */
 live.groups.count=1;live.groups.slot[0].queued_us=2500000;
 live_queue_age_tick(4499999);assert(live.tx.count==2 && live.groups.count==1);
 live_queue_age_tick(4500000);assert(!live.tx.count && !live.groups.count && live.dropped==5);
 return 0;
}
