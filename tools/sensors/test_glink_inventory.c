#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include "slpi_glink_core.h"
static struct sg_core g;static uint8_t last[256];static unsigned tx,fail_tx;
static int emit(void*c,const uint8_t*b,unsigned n){assert(n<=256&&!(n&7));if(fail_tx)return 0;memcpy(last,b,n);tx++;return 1;}
static void note(void*c,unsigned e,uint32_t a,uint32_t b,uint32_t d,uint32_t f){}
static unsigned frame(uint8_t*b,unsigned c,unsigned a,uint32_t v,const void*p,unsigned n){unsigned size=(8+n+7)&~7u;memset(b,0,size);sg16(b,c);sg16(b+2,a);sg32(b+4,v);if(n)memcpy(b+8,p,n);return size;}
static int feed(unsigned c,unsigned a,uint32_t v,const void*p,unsigned n){uint8_t b[1024];unsigned z=frame(b,c,a,v,p,n);return sg_receive(&g,b,z);}
static void fresh(void){memset(&g,0,sizeof(g));tx=fail_tx=0;assert(sg_init(&g,0,emit,note));}
static void opened(unsigned features){fresh();assert(feed(0,1,features,0,0));assert(feed(1,1,features,0,0));assert(g.agreed&&g.asked);assert(feed(2,7,7,"IPCRTR",7));assert(feed(4,1,0,0,0));assert(g.open&&g.offered&&g.qcount==1);uint8_t i[8];sg32(i,512);sg32(i+4,9);assert(feed(5,7,1,i,8));assert(g.qcount==0&&g.peer[0].used&&slpi_u16(last)==9);assert(feed(features?14:6,7,9,0,0));}
static unsigned packet(uint8_t*p,unsigned type,uint32_t service){memset(p,0,52);sg32(p,1);sg32(p+4,type);sg32(p+8,9);sg32(p+12,SG_CTRL);sg32(p+20,20);sg32(p+24,1);sg32(p+28,SG_CTRL);sg32(p+32,type);sg32(p+36,service);sg32(p+40,1);sg32(p+44,9);sg32(p+48,service+1);return 52;}
static int receive_packet(const uint8_t*p,unsigned n){uint8_t body[1024];sg32(body,n);sg32(body+4,0);memcpy(body+8,p,n);return feed(9,7,1,body,n+8);}
static bool first_next(void *page, struct hk_fastrpc_invoke *m)
{
 return hk_fastrpc_encode_first_next(m,page,4096,0x30000000,0x80000000,0x541,73,42);
}
int main(void){uint8_t p[128],body[128],f[1024];
 opened(1);packet(p,2,0);assert(receive_packet(p,52)&&g.hello_received==1);packet(p,4,400);assert(receive_packet(p,52));assert(g.services[0].service==400&&g.services[0].node==9&&g.services[0].port==401);packet(p,5,400);assert(receive_packet(p,52)&&!g.services[0].present);
 opened(0);packet(p,2,0);assert(receive_packet(p,52));assert(slpi_u16(last)==5); /* nonreuse must reoffer */
 opened(1);packet(p,4,400);sg32(body,24);sg32(body+4,28);memcpy(body+8,p,24);assert(feed(9,7,1,body,32));sg32(body,28);sg32(body+4,0);memcpy(body+8,p+24,28);assert(feed(12,7,1,body,36)&&g.services[0].service==400);
 opened(1);sg32(body,4);sg32(body+4,0);memset(body+8,0,4);assert(!feed(12,7,1,body,12));
 opened(1);sg32(body,4);sg32(body+4,0xffffffff);assert(!feed(9,7,1,body,12));
 fresh();assert(feed(1,1,0,0,0)&&!g.agreed&&g.features==0&&slpi_u16(last)==0);assert(feed(1,1,0,0,0)&&g.agreed);fresh();assert(!feed(1,0,1,0,0));fresh();unsigned n=frame(f,5,7,0xffffffff,0,0);assert(sg_frame_length(f,n,16384)==-1);
 fresh();n=frame(f,2,7,0xffffffff,0,0);assert(sg_frame_length(f,n,16384)==-1);
 for(unsigned z=0;z<8;z++){fresh();assert(!sg_receive(&g,f,z));}
 opened(1);packet(p,4,400);sg32(p+20,0xffffffff);assert(!receive_packet(p,52));
 opened(1);for(unsigned i=0;i<64;i++){packet(p,4,i+1);assert(receive_packet(p,52));}packet(p,4,999);assert(!receive_packet(p,52)&&g.service_overflow==1);
 fresh();fail_tx=1;assert(!feed(0,1,1,0,0)&&g.broken);
 assert(sg_ring_bounds(16384,0,0));assert(sg_ring_room(16384,0,0)==16368);assert(sg_ring_used(16384,8,16376)==16);assert(sg_ring_room(16384,16376,8)==0);assert(!sg_ring_bounds(16384,16384,0));assert(!sg_ring_bounds(16384,1,0));
 /* Hardware frame6: channel2, one128-byte remote intent with ID0. */
 for(unsigned reuse=0;reuse<2;reuse++){
  fresh();assert(feed(0,1,reuse,0,0));assert(feed(1,1,reuse,0,0));
  assert(feed(2,2,7,"IPCRTR",7));assert(feed(4,1,0,0,0));
  sg32(body,128);sg32(body+4,0);assert(feed(5,2,1,body,8));
  assert(!g.broken&&g.qcount==0&&g.peer[0].present&&g.peer[0].used&&g.peer[0].id==0);
  assert(slpi_u16(last)==9&&slpi_u32(last+4)==0);
  assert(feed(reuse?14:6,2,0,0,0));assert(!g.peer[0].used&&g.peer[0].present==reuse);
 }
 /* Lookup is gated, targets discovered service400, and preserves native QMI bytes. */
 opened(1);packet(p,2,0);assert(receive_packet(p,52));g.lookup=1;assert(sg_tick(&g)&&!g.lookup_sent);
 packet(p,4,400);assert(receive_packet(p,52)&&g.lookup_sent&&g.lookup_node==9&&g.lookup_port==401);
 assert(slpi_u16(last)==9&&slpi_u32(last+8)==32+sizeof(sg_lookup_qmi));
 assert(slpi_u32(last+28)==SG_SEE_PORT&&slpi_u32(last+40)==9&&slpi_u32(last+44)==401);
 assert(!memcmp(last+48,sg_lookup_qmi,sizeof(sg_lookup_qmi)));
 unsigned before=tx;assert(sg_tick(&g)&&tx==before); /* never resend */
 packet(p,1,0);sg32(p+12,401);sg32(p+28,SG_SEE_PORT);
 p[32]=2;p[37]=13;p[38]=0;p[33]=1;p[34]=0;p[35]=0x20;p[36]=0;
 assert(receive_packet(p,52)&&g.reply_count==1&&g.reply_size[0]==20);
 assert(!memcmp(g.replies[0],p+32,20));
 sg32(p+12,402);assert(receive_packet(p,52)&&g.reply_count==1); /* unrelated endpoint ignored */
 sg32(p+12,401);assert(receive_packet(p,52)&&g.reply_count==1); /* duplicate txn ignored */
 opened(1);packet(p,2,0);assert(receive_packet(p,52));g.lookup=2;
 packet(p,4,400);assert(receive_packet(p,52)&&g.lookup_sent);
 assert(sizeof(sg_lookup_qmi)==sizeof(sg_lookup_ambient_light_qmi));
 assert(!memcmp(last+48,sg_lookup_ambient_light_qmi,sizeof(sg_lookup_ambient_light_qmi)));

 /* Seven subscriptions on one port, distinct transactions; all late replies
  * retained, with no resend or fixed eight-packet truncation. */
 opened(1);packet(p,2,0);assert(receive_packet(p,52));g.lookup=3;
 packet(p,4,400);assert(receive_packet(p,52)&&g.lookup_sent==1);
 for(unsigned q=0;q<SG_QUERY_COUNT;q++){
  const struct sg_query *rq=&sg_queries[q];
  assert(slpi_u16(last)==9&&slpi_u32(last+8)==32+rq->size);
  assert(slpi_u32(last+28)==SG_SEE_PORT&&slpi_u16(last+49)==q+1);
  assert(last[48]==0&&slpi_u16(last+51)==0x20);
  assert(!memcmp(last+51,rq->bytes+3,rq->size-3));
  if(q+1<SG_QUERY_COUNT){assert(feed(14,7,9,0,0));assert(g.lookup_sent==q+2);}
 }
 assert(g.lookup_sent==SG_QUERY_COUNT);
 before=tx;assert(sg_tick(&g)&&tx==before);
 packet(p,1,0);sg32(p+12,401);sg32(p+28,SG_SEE_PORT);p[32]=2;p[37]=13;p[38]=0;p[35]=0x20;p[36]=0;
 for(unsigned i=0;i<SG_QUERY_COUNT;i++){p[33]=(uint8_t)(i+1);p[34]=0;assert(receive_packet(p,52));}
 for(unsigned i=0;i<SG_QUERY_COUNT;i++){p[33]=(uint8_t)(i+1);assert(receive_packet(p,52));}
 assert(g.reply_count==SG_QUERY_COUNT&&!g.broken&&g.lookup_sent==SG_QUERY_COUNT);
 /* Captured SUID indication follows the response with the same sequence. */
 static const uint8_t indication[] = {0x4,0x1,0x0,0x21,0x0,0x45,0x0,0x1,0x8,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x2,0x37,0x0,0x35,0x0,0xa,0x12,0x9,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x12,0x1f,0xd,0x0,0x3,0x0,0x0,0x11,0xb5,0x48,0x10,0x63,0x0,0x0,0x0,0x0,0x1a,0xf,0xa,0xd,0x61,0x6d,0x62,0x69,0x65,0x6e,0x74,0x5f,0x6c,0x69,0x67,0x68,0x74};
 sg32(p+20,sizeof(indication));memcpy(p+32,indication,sizeof(indication));
 assert(receive_packet(p,32+sizeof(indication)) && g.reply_count==SG_QUERY_COUNT+1);
 assert(!memcmp(g.replies[SG_QUERY_COUNT],indication,sizeof(indication)));
 p[33]=0;assert(receive_packet(p,32+sizeof(indication)) && g.reply_count==SG_QUERY_COUNT+2);
 /* No feature mode: no query even after a server appears. */
 opened(1);packet(p,2,0);assert(receive_packet(p,52));packet(p,4,400);
 assert(receive_packet(p,52)&&g.lookup_sent==0);
 /* Remote channel numbers are chosen by the peer. An intent ID may
  * overlap IPCRTR without being added to IPCRTR's pool. */
 opened(1);g.rpc_probe=1;
 static const char rpc[]="fastrpcglink-apps-dsp";
 assert(feed(2,13,sizeof(rpc),rpc,sizeof(rpc))&&g.rpc_remote==13);
 assert(slpi_u16(last)==2&&slpi_u16(last+2)==2);
 assert(feed(4,2,0,0,0)&&g.rpc_ack);
 sg32(body,128);sg32(body+4,0);assert(feed(5,13,1,body,8)&&g.rpc_intents==1);
 assert(!g.peer[1].present && g.open && g.theirs==7);
 assert(feed(7,13,128,0,0)&&slpi_u16(last)==8&&slpi_u32(last+4)==0);
 assert(feed(3,13,0,0,0)&&!g.rpc_remote&&!g.rpc_ack&&g.open);
 opened(1);g.rpc_probe=2;
 assert(feed(2,9,sizeof(rpc),rpc,sizeof(rpc))&&feed(4,2,0,0,0));
 assert(slpi_u16(last)==7 && slpi_u16(last+2)==2 && slpi_u32(last+4)==40);
 unsigned sent_request_count=tx;
 assert(feed(4,2,0,0,0)&&tx==sent_request_count);
 assert(feed(8,9,1,0,0)&&g.rpc_intent_reply==2);
 sg32(body,40);sg32(body+4,0);assert(feed(5,9,1,body,8)&&g.rpc_max_intent==40);
 /* Owned attach must wait for a grant and a suitable peer intent.
  * A packet retirement or an early/stale response cannot complete DMA. */
 opened(1);g.rpc_probe=3;g.rpc.prepared=true;g.rpc.message.ctx=0x1022;
 assert(feed(2,13,sizeof(rpc),rpc,sizeof(rpc))&&feed(4,2,0,0,0));
 assert(g.rpc.offered&&!g.rpc.sent);
 assert(feed(8,13,1,0,0)&&!g.rpc.sent);
 sg32(body,40);sg32(body+4,0);assert(feed(5,13,1,body,8));
 assert(!g.rpc.sent); /* grant alone precedes SEE startup on the device */
 packet(p,2,0);assert(receive_packet(p,52)&&!g.rpc.sent);
 packet(p,4,400);assert(receive_packet(p,52));
 assert(g.rpc.sent&&slpi_u16(last)==9&&slpi_u16(last+2)==2&&slpi_u32(last+4)==0);
 assert(slpi_u32(last+8)==40&&!memcmp(last+16,&g.rpc.message,40));
 assert(feed(14,13,0,0,0)&&g.rpc.tx_done&&!g.rpc.completed);
 memset(body,0,32);sg32(body,16);sg32(body+8,0x2022);
 assert(feed(9,13,1,body,24)&&g.rpc.stale==1&&!g.rpc.completed);
 sg32(body,24);sg32(body+8,0x1022);sg32(body+20,2);sg32(body+28,2);
 assert(feed(9,13,1,body,32)&&g.rpc.early==1&&!g.rpc.completed);
 sg32(body,12);sg32(body+4,12);
 assert(feed(9,13,1,body,20)&&!g.rpc.completed);
 memset(body,0,20);sg32(body,12);sg32(body+8,3);sg32(body+16,2);
 assert(feed(12,13,1,body,20)&&g.rpc.completed&&g.rpc.flags==3&&g.rpc.retval==0);
 assert(!feed(9,13,1,body,20)); /* truncated reply */
 /* init2 on the same session. The peer may release or recycle its TX buffer. */
 for(unsigned reuse=0;reuse<2;reuse++){
  opened(reuse);g.rpc_probe=4;g.rpc.prepared=true;g.rpc_tgid=73;
  g.rpc.message.ctx=0x1002;g.rpc.message.tid=42;
  packet(p,2,0);assert(receive_packet(p,52));packet(p,4,400);assert(receive_packet(p,52));
  assert(feed(2,13,sizeof(rpc),rpc,sizeof(rpc))&&feed(4,2,0,0,0));
  assert(feed(8,13,1,0,0));sg32(body,40);sg32(body+4,0);assert(feed(5,13,1,body,8));
  assert(g.rpc.sent&&!g.listener_started);
  memset(body,0,32);sg32(body,16);sg32(body+8,0x1002);
  assert(feed(9,13,1,body,24)&&g.rpc.completed);
  assert(sg_tick(&g)&&!g.listener_started); /* wait for retirement too */
  assert(feed(reuse?14:6,13,0,0,0));assert(sg_tick(&g)&&g.listener_started);
  if(!reuse){
   assert(!g.rpc.sent&&slpi_u16(last)==7);assert(feed(8,13,1,0,0));
   sg32(body,40);sg32(body+4,4);assert(feed(5,13,1,body,8));
  }
  assert(g.rpc.sent && slpi_u16(last)==9);
  assert(g.rpc.message.ctx==0x2002 && g.rpc.message.handle==3 && g.rpc.message.scalars==0x03000000);
  assert(g.rpc.message.tid==42 && g.rpc.message.client_id==73 && !g.rpc.message.addr && g.rpc.message.size==4096);
  memset(body,0,32);sg32(body,16);sg32(body+8,0x1002);
  assert(feed(9,13,1,body,24)&&!g.rpc.completed&&g.rpc.stale==1);
  sg32(body+8,0x2002);assert(feed(9,13,1,body,24)&&g.rpc.completed&&!g.rpc.retval);
 }
 /* Only arm the first callback after a confirmed init2 reply and its TX retirement. */
 g.rpc_probe=5;g.rpc.tx_done=false;g.rpc_reusable=true;
 static uint8_t callback_page[4096];g.rpc_owner=callback_page;g.prepare_next=first_next;
 assert(sg_tick(&g)&&!g.next_started);g.rpc.tx_done=true;
 assert(sg_tick(&g)&&g.next_started&&g.rpc.sent&&!g.rpc.completed);
 assert(g.rpc.message.ctx==0x3002&&slpi_u16(last)==9);
 memset(body,0,32);sg32(body,16);sg32(body+8,0x2002);
 assert(feed(9,13,1,body,24)&&!g.rpc.completed);
 sg32(body+8,0x3002);assert(feed(9,13,1,body,24)&&g.rpc.completed);
 /* Two RPC contexts remain live while next2 is pending. */
 g.rpc_probe=6;g.next_started=true;g.rpc.completed=false;g.rpc.tx_done=false;
 g.rpc_reusable=true;g.foreground.prepared=true;g.foreground.message.ctx=0x4012;
 assert(sg_tick(&g)&&!g.foreground.sent);
 assert(feed(14,13,g.rpc.peer_id,0,0)&&sg_tick(&g)&&g.foreground.sent);
 memset(body,0,32);sg32(body,16);sg32(body+8,0x4012);sg32(body+16,5);
 assert(feed(9,13,1,body,24)&&g.foreground.completed&&g.foreground.retval==5&&!g.rpc.completed);
 assert(feed(14,13,g.foreground.peer_id,0,0)&&g.foreground.tx_done);
 sg32(body+8,0x3002);sg32(body+16,0);
 assert(feed(9,13,1,body,24)&&g.rpc.completed&&!g.rpc.retval&&g.foreground.retval==5);
 puts("PASS GLINK version/open/intents/reuse/fragmentation/control inventory/backpressure/caps/malformed frames/ring wrap; synthetic only");}
