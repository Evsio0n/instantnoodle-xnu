#include "native_query.h"
extern unsigned lx_os_awdl_query_bytes(uint64_t lease,uint8_t *out,unsigned cap);
/* Execute native subscriptions with the existing owned multicast FIFO. A
 * fresh question is generated only while that exact native lease/subscription
 * exists. Answers still come from real peers and the stock mDNSResponder. */
static struct {
    unsigned busy;
    uint64_t lease,sent;
    uint8_t questions[8*(259+2)],frame[14+40+8+12+259];
} registered_queries;
static void registered_query_tick(uint64_t now)
{
    unsigned expected=0;
    if(!__atomic_compare_exchange_n(&registered_queries.busy,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return;
    uint64_t lease=EG(live.lease),generation=EG(live.generation);
    if(!lease || !EG(live.accept) || EG(live.stop))goto done;
    if(registered_queries.lease!=lease){registered_queries.lease=lease;registered_queries.sent=0;}
    if(registered_queries.sent && now-registered_queries.sent<1000000ull)goto done;
    unsigned bytes=lx_os_awdl_query_bytes(lease,registered_queries.questions,sizeof(registered_queries.questions));
    for(unsigned at=0;at+2<=bytes;){
        unsigned size=registered_queries.questions[at]|(unsigned)registered_queries.questions[at+1]<<8;at+=2;
        if(size>bytes-at)break;
        unsigned n=hk_query_frame(registered_queries.frame,sizeof(registered_queries.frame),echo.mac,echo.ip,registered_queries.questions+at,size);
        if(n){
            int rc=live_output(&live,generation,lease,registered_queries.frame,n);
            static unsigned receipts;
            if(receipts++<32 || rc)lx_tell("awdl-query: question_bytes=%u frame_bytes=%u queued_rc=%d lease=%llu\n",size,n,rc,lease);
        }
        at+=size;
    }
    if(bytes)registered_queries.sent=now;
done:
    __atomic_store_n(&registered_queries.busy,0,__ATOMIC_RELEASE);
}
