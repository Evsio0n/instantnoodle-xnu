#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#define LIVE_FLIGHTS 4u
#define EG(x) (x)
struct live_flight {uint64_t lease,send_us;unsigned freed,tag;void *skb;};
static struct live_flight native_flights[LIVE_FLIGHTS];
static unsigned locked,receipts;
static void lx_os_lock(void){assert(!locked);locked=1;}
static void lx_os_unlock(void){assert(locked);locked=0;}
static void lx_tell(const char *fmt,...){(void)fmt;receipts++;}
#include "flight_stall.inc"
int main(void){
 assert(!live_carrier_stalled(999999999));
 native_flights[1]=(struct live_flight){.lease=7,.send_us=1000000,.tag=12,.skb=(void *)1};
 struct live_flight before[LIVE_FLIGHTS];memcpy(before,native_flights,sizeof(before));
 assert(!live_carrier_stalled(999999)); /* clock underflow is not a timeout */
 assert(!live_carrier_stalled(2999999));
 assert(live_carrier_stalled(3000000));assert(receipts==1);
 assert(!memcmp(before,native_flights,sizeof(before))); /* never reclaim DMA/skb */
 native_flights[1].freed=1;assert(!live_carrier_stalled(10000000));
 native_flights[1].freed=0;native_flights[1].lease=0;assert(!live_carrier_stalled(10000000));
 native_flights[3]=(struct live_flight){.lease=7,.send_us=5000000,.tag=15};
 assert(!live_carrier_stalled(6000000));assert(live_carrier_stalled(7000000));assert(!locked);
 return 0;
}
