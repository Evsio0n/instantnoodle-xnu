#include <assert.h>
#include <string.h>
#include "native_query.h"
#include "mdns_unicast.h"
#define LIVE_DEPTH 4u
struct live_frame { uint64_t lease,queued_us;size_t n;uint8_t link_target[6],bytes[512]; };
struct live_fifo {unsigned first,count;struct live_frame slot[LIVE_DEPTH];};
#include "mdns_pending.inc"
int main(void){
 struct live_fifo q={.first=3,.count=1};
 uint8_t frame[512],mac[6]={2,0,0,0,0,1},target[6]={2,0,0,0,0,2},ip[16]={0xfe,0x80};
 const uint8_t question[]={8,'_','a','i','r','d','r','o','p',4,'_','t','c','p',5,'l','o','c','a','l',0,0,12,0,1};
 ip[15]=1;unsigned n=hk_query_frame(frame,sizeof(frame),mac,ip,question,sizeof(question));assert(n);
 q.slot[3].lease=7;q.slot[3].n=n;q.slot[3].queued_us=123;
 memcpy(q.slot[3].link_target,target,6);memcpy(q.slot[3].bytes,frame,n);
 struct live_fifo before=q;
 assert(live_pending_question(&q,7,target,frame,n));assert(!memcmp(&before,&q,sizeof(q)));
 assert(!live_pending_question(&q,8,target,frame,n));target[5]=3;assert(!live_pending_question(&q,7,target,frame,n));target[5]=2;
 frame[n-1]^=1;assert(!live_pending_question(&q,7,target,frame,n));frame[n-1]^=1;
 frame[64]|=0x80;hk_mdns_checksum(frame,n);memcpy(q.slot[3].bytes,frame,n);
 assert(!live_pending_question(&q,7,target,frame,n)); /* identical answer is not a question */
 q.count=0;assert(!live_pending_question(&q,7,target,frame,n));
 return 0;
}
