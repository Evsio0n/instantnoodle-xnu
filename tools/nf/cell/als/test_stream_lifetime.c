/* Offline reproduction of run 080611. Only the fixture timestamp advances.
 * This executable links the production parser and never opens device I/O. */
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "slpi_glink_core.h"

static struct sg_core g;
static unsigned emitted;
static bool reply_ok=true;
static unsigned reply_seq;
static int emit(void *unused,const uint8_t *bytes,unsigned size) {
 (void)unused;(void)bytes;assert(size>=8);emitted++;return 1;
}
static void note(void *unused,unsigned kind,uint32_t a,uint32_t b,uint32_t c,uint32_t d){
 (void)unused;(void)kind;(void)a;(void)b;(void)c;(void)d;
}
static bool reply(void *unused,struct hk_fastrpc_invoke *message,unsigned seq){
 (void)unused;(void)message;reply_seq=seq;return reply_ok;
}

/* #406 dumped QRTR payload 1849 before its capture array filled. */
static const unsigned char qmi[]={
    0x04,0x26,0x07,0x21,0x00,0x4e,0x00,0x01,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x02,0x40,0x00,0x3e,0x00,0x0a,0x12,0x09,0x62,0x61,0x43,0x54,0x52,0x4c,
    0x22,0x35,0x11,0x39,0x31,0x91,0x2c,0x43,0x5d,0x52,0x5f,0x12,0x28,0x0d,0x01,0x04,
    0x00,0x00,0x11,0x95,0xaa,0x48,0x64,0x04,0x00,0x00,0x00,0x1a,0x18,0x0a,0x14,0x00,
    0x20,0x8e,0x44,0x00,0x00,0xe0,0x40,0x00,0x00,0x40,0x41,0x00,0x00,0x00,0x00,0x00,
    0x00,0x00,0x00,0x10,0x03
};
static void setup(struct sl_events *events) {
 memset(&g,0,sizeof(g));
 assert(sg_init(&g,NULL,emit,note));
 assert(sl_indication(qmi,sizeof(qmi),events) && events->count==1);
 g.agreed=g.open=1;g.theirs=2;g.rpc_probe=8;g.query_base=SG_QUERY_COUNT;
 g.lookup=3;g.lookup_sent=SG_QUERY_COUNT;g.lookup_node=g.registry.node=9;
 g.lookup_port=g.registry.port=12;g.registry.client_id=events->client;
 g.wise_stream=g.vendor_attrs=true;g.vendor.ack=g.vendor.received=1;
 g.vendor.uid[0]=events->source;g.vendor.count[0]=1;
 struct sl_attrs *a=&g.vendor.attrs[0];a->source=events->source;
 a->available=a->physical=true;a->stream=1;
 a->present=sl_bit(SL_TYPE)|sl_bit(SL_NAME)|sl_bit(SL_VENDOR)|
            sl_bit(SL_PHYSICAL)|sl_bit(SL_AVAILABLE)|sl_bit(SL_STREAM);
 strcpy(a->name,"stk33502");strcpy(a->vendor,"oplus");strcpy(a->type,"wise_light");
 assert(vw_mode(&g.vendor,&g.wise.config));g.wise.sent=g.wise.accepted=1;
}
int main(void){
 struct sl_events events;setup(&events);
 const unsigned payload_size=sizeof(qmi)+32;
 unsigned char frame[136]={0},*qrtr=frame+16,*body=qrtr+32;
 sg16(frame,9);sg16(frame+2,2);sg32(frame+4,3);sg32(frame+8,payload_size);
 sg32(qrtr,1);sg32(qrtr+4,1);sg32(qrtr+8,9);sg32(qrtr+12,12);
 sg32(qrtr+20,sizeof(qmi));sg32(qrtr+24,1);sg32(qrtr+28,SG_SEE_PORT);
 memcpy(body,qmi,sizeof(qmi));
 const unsigned timestamp=51;
 uint64_t first=events.event[0].ticks, last=first;
 for(unsigned i=0;i<50000;i++){
  last=first+(uint64_t)i*3840000;
  sg32(body+timestamp,(uint32_t)last);sg32(body+timestamp+4,(uint32_t)(last>>32));
  assert(sg_receive(&g,frame,sizeof(frame)));
  assert(g.wise.latest.ticks==last && g.wise.latest.status==3);
 }
 assert(g.wise.count==50000 && g.qrtr_in==50000 && g.received==50000 &&
        g.reply_count==1850 && g.capture_dropped==48150 && !g.broken);
 /* Duplicate sample and valid traffic from a different endpoint never advance the source. */
 assert(sg_receive(&g,frame,sizeof(frame)) && g.wise.count==50000);
 sg32(qrtr+8,8);assert(sg_receive(&g,frame,sizeof(frame)) && g.wise.count==50000);sg32(qrtr+8,9);
 sg32(body+timestamp,(uint32_t)(last+1));sg32(body+timestamp+4,(uint32_t)((last+1)>>32));
 body[26]^=1;assert(sg_receive(&g,frame,sizeof(frame)) && g.wise.count==50000);body[26]^=1;
 uint32_t lux=slpi_u32(body+63);sg32(body+63,0x7fc00000);
 assert(sg_receive(&g,frame,sizeof(frame)) && g.wise.count==50000 && g.wise.bad_events==1);
 sg32(body+63,lux);

 g.qrtr_in=g.received=g.capture_dropped=UINT32_MAX;
 assert(sg_receive(&g,frame,sizeof(frame)));assert(g.qrtr_in==UINT32_MAX && g.received==UINT32_MAX && g.capture_dropped==UINT32_MAX);

 /* Make complete listener replies through the normal dispatcher across its former limit. */
 g.next_started=g.module_start=g.foreground.tx_done=true;
 g.rpc_reusable=g.foreground_reusable=true;g.reply_callback=reply;
 for(unsigned i=0;i<10000;i++){
  g.rpc.completed=g.rpc.tx_done=true;
  assert(sg_rpc_send_if_ready(&g) && g.callbacks==i+1 && reply_seq==i+6);
 }
 g.callback_sequence=UINT32_MAX;g.callbacks=UINT32_MAX;
 g.rpc.completed=g.rpc.tx_done=true;
 assert(sg_rpc_send_if_ready(&g) && reply_seq==6 && g.callbacks==UINT32_MAX);
 g.rpc.completed=g.rpc.tx_done=true;reply_ok=false;
 assert(!sg_rpc_send_if_ready(&g) && g.broken);

 setup(&events);g.wise.count=1;g.qrtr_in=2050;
 frame[8]=255;assert(!sg_receive(&g,frame,sizeof(frame)) && g.broken);frame[8]=(uint8_t)payload_size;
 setup(&events);g.wise.count=1;g.qrtr_in=2050;
 body[5]++;assert(!sg_receive(&g,frame,sizeof(frame)) && g.broken);body[5]--;
 setup(&events);g.wise.count=1;g.registry.invalidated=true;assert(!sg_resident_light(&g));
 setup(&events);g.wise_stream=false;g.qrtr_in=2048;
 assert(!sg_receive(&g,frame,sizeof(frame)) && g.broken);
 puts("PASS 50,000 framed front reports, 10,000 listener replies, saturated counters, tag wrap, foreign/duplicate events, malformed framing and identity invalidation");
 return 0;
}
