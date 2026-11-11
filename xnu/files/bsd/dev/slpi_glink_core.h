/* Bounded IPCRTR protocol core and opt-in SEE availability inventory.
 * No allocator, hardware access, sensor configuration or registry writes. */
#ifndef SLPI_GLINK_CORE_H
#define SLPI_GLINK_CORE_H
#include "slpi_smem_ro.h"
#define SG_MAX 0x4400u
#define SG_CTRL 0xfffffffeu
static int sg_ring_bounds(uint32_t size,uint32_t head,uint32_t tail){return size>=1024&&size<=65536&&!(size&7)&&head<size&&tail<size&&!((head|tail)&7);}
static uint32_t sg_ring_used(uint32_t size,uint32_t head,uint32_t tail){return head>=tail?head-tail:size-tail+head;}
static uint32_t sg_ring_room(uint32_t size,uint32_t head,uint32_t tail){if(!sg_ring_bounds(size,head,tail))return 0;uint32_t free=size-sg_ring_used(size,head,tail);return free<16?0:free-16;}
struct sg_intent {uint32_t id,size;unsigned used,present;};
struct sg_rx {uint32_t size,fill,left;uint8_t data[SG_MAX];};
struct sg_control {uint32_t type,node,from_port,to_port,words[5];};
struct sg_service {uint32_t service,instance,node,port;unsigned present;};
/* Diagnostic ambient_light SUID lookup only; no sensor configuration or registry writes. */
#define SG_SEE_PORT 0x4001u
static const uint8_t sg_lookup_qmi[] = {0x0,0x1,0x0,0x20,0x0,0x3f,0x0,0x1,0x38,0x0,0x36,0x0,0xa,0x12,0x9,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x0,0x2,0x0,0x0,0x1a,0x4,0x8,0x1,0x10,0x0,0x22,0x15,0x12,0x13,0xa,0xd,0x61,0x6d,0x62,0x69,0x65,0x6e,0x74,0x5f,0x6c,0x69,0x67,0x68,0x74,0x10,0x1,0x18,0x1,0x10,0x1,0x0,0x0};
static const uint8_t sg_lookup_all_qmi[] = {0x0,0x1,0x0,0x20,0x0,0x3f,0x0,0x1,0x38,0x0,0x36,0x0,0xa,0x12,0x9,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x0,0x2,0x0,0x0,0x1a,0x4,0x8,0x1,0x10,0x0,0x22,0x15,0x12,0x13,0xa,0xd,0x61,0x6d,0x62,0x69,0x65,0x6e,0x74,0x5f,0x6c,0x69,0x67,0x68,0x74,0x10,0x1,0x18,0x0,0x10,0x1,0x0,0x0};
#define SG_QUERY_COUNT 7
struct sg_query { const char *name; const uint8_t *bytes; unsigned size; };
static const uint8_t sg_lookup_registry_qmi[] = {0x00,0x01,0x00,0x20,0x00,0x3a,0x00,0x01,0x33,0x00,0x31,0x00,0x0a,0x12,0x09,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x00,0x02,0x00,0x00,0x1a,0x04,0x08,0x01,0x10,0x00,0x22,0x10,0x12,0x0e,0x0a,0x08,0x72,0x65,0x67,0x69,0x73,0x74,0x72,0x79,0x10,0x01,0x18,0x00,0x10,0x01,0x00,0x00};
static const uint8_t sg_lookup_timer_qmi[] = {0x00,0x01,0x00,0x20,0x00,0x37,0x00,0x01,0x30,0x00,0x2e,0x00,0x0a,0x12,0x09,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x00,0x02,0x00,0x00,0x1a,0x04,0x08,0x01,0x10,0x00,0x22,0x0d,0x12,0x0b,0x0a,0x05,0x74,0x69,0x6d,0x65,0x72,0x10,0x01,0x18,0x00,0x10,0x01,0x00,0x00};
static const uint8_t sg_lookup_accel_qmi[] = {0x00,0x01,0x00,0x20,0x00,0x37,0x00,0x01,0x30,0x00,0x2e,0x00,0x0a,0x12,0x09,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x00,0x02,0x00,0x00,0x1a,0x04,0x08,0x01,0x10,0x00,0x22,0x0d,0x12,0x0b,0x0a,0x05,0x61,0x63,0x63,0x65,0x6c,0x10,0x01,0x18,0x00,0x10,0x01,0x00,0x00};
static const uint8_t sg_lookup_gyro_qmi[] = {0x00,0x01,0x00,0x20,0x00,0x36,0x00,0x01,0x2f,0x00,0x2d,0x00,0x0a,0x12,0x09,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x00,0x02,0x00,0x00,0x1a,0x04,0x08,0x01,0x10,0x00,0x22,0x0c,0x12,0x0a,0x0a,0x04,0x67,0x79,0x72,0x6f,0x10,0x01,0x18,0x00,0x10,0x01,0x00,0x00};
static const uint8_t sg_lookup_ambient_light_qmi[] = {0x00,0x01,0x00,0x20,0x00,0x3f,0x00,0x01,0x38,0x00,0x36,0x00,0x0a,0x12,0x09,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x00,0x02,0x00,0x00,0x1a,0x04,0x08,0x01,0x10,0x00,0x22,0x15,0x12,0x13,0x0a,0x0d,0x61,0x6d,0x62,0x69,0x65,0x6e,0x74,0x5f,0x6c,0x69,0x67,0x68,0x74,0x10,0x01,0x18,0x00,0x10,0x01,0x00,0x00};
static const uint8_t sg_lookup_proximity_qmi[] = {0x00,0x01,0x00,0x20,0x00,0x3b,0x00,0x01,0x34,0x00,0x32,0x00,0x0a,0x12,0x09,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x00,0x02,0x00,0x00,0x1a,0x04,0x08,0x01,0x10,0x00,0x22,0x11,0x12,0x0f,0x0a,0x09,0x70,0x72,0x6f,0x78,0x69,0x6d,0x69,0x74,0x79,0x10,0x01,0x18,0x00,0x10,0x01,0x00,0x00};
static const uint8_t sg_lookup_mag_qmi[] = {0x00,0x01,0x00,0x20,0x00,0x35,0x00,0x01,0x2e,0x00,0x2c,0x00,0x0a,0x12,0x09,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x11,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0xab,0x15,0x00,0x02,0x00,0x00,0x1a,0x04,0x08,0x01,0x10,0x00,0x22,0x0b,0x12,0x09,0x0a,0x03,0x6d,0x61,0x67,0x10,0x01,0x18,0x00,0x10,0x01,0x00,0x00};
static const struct sg_query sg_queries[SG_QUERY_COUNT] = {
 {"registry",sg_lookup_registry_qmi,sizeof(sg_lookup_registry_qmi)},
 {"timer",sg_lookup_timer_qmi,sizeof(sg_lookup_timer_qmi)},
 {"accel",sg_lookup_accel_qmi,sizeof(sg_lookup_accel_qmi)},
 {"gyro",sg_lookup_gyro_qmi,sizeof(sg_lookup_gyro_qmi)},
 {"ambient_light",sg_lookup_ambient_light_qmi,sizeof(sg_lookup_ambient_light_qmi)},
 {"proximity",sg_lookup_proximity_qmi,sizeof(sg_lookup_proximity_qmi)},
 {"mag",sg_lookup_mag_qmi,sizeof(sg_lookup_mag_qmi)},
};
#include "hk_fastrpc_wire.h"
#include "slpi_rpc_call.h"
#include "hk_sensor_dependencies.h"
#include "hk_wise_sample.h"
enum { SG_REGISTRY_CALLBACK_LIMIT = 8192 };
/* Registry requests use the existing SSC client port and their own transaction
 * namespace. All four reads are gated on a real, late registry SUID event. */
#define SG_REG_TXN_BASE 0x4100u
struct sg_core {
 struct sr_client registry;
 struct sd_state deps;
 struct va_state vendor;bool vendor_attrs;
 struct vw_state wise;bool wise_stream;
 struct sg_rpc_call rpc, foreground;
 bool foreground_armed, module_start, foreground_reusable;
 bool (*prepare_module_start)(void *, struct hk_fastrpc_invoke *);
 uint32_t rpc_tgid; bool listener_started, next_started, rpc_reusable;
 unsigned callbacks;
 bool (*reply_callback)(void *,struct hk_fastrpc_invoke *,unsigned);
 void *rpc_owner; bool (*prepare_next)(void *, struct hk_fastrpc_invoke *);
 unsigned lookup,lookup_sent,lookup_node,lookup_port,reply_count,query_base;
 uint64_t std_activity_us;bool wrote_registry;uint32_t query_txn_expected,query_txn_seen;
 unsigned reply_size[48],capture_dropped;uint8_t replies[48][1100];
 int(*emit)(void*,const uint8_t*,unsigned);void(*note)(void*,unsigned,uint32_t,uint32_t,uint32_t,uint32_t);void*ctx;
 unsigned broken,features,version_rounds,agreed,asked,ack_sent,acknowledged,open,offered,theirs;
 unsigned received,sent,other_opens,hello_sent,hello_received,intent_requested,qrtr_in,lookups;
 unsigned rpc_probe,rpc_remote,rpc_requested,rpc_ack,rpc_intents;
 unsigned rpc_intent_requested,rpc_intent_reply,rpc_max_intent;
 struct sg_intent peer[64];struct sg_rx rx[10];struct sg_control pending[32];unsigned qhead,qcount;
 struct sg_service services[64];unsigned service_overflow,service_events;char others[16][33];
};
static void sg16(uint8_t*p,unsigned v){p[0]=v;p[1]=v>>8;}
static void sg32(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=v>>(8*i);}
static int sg_fail(struct sg_core*g){g->broken=1;return 0;}
static int sg_emit(struct sg_core*g,unsigned cmd,unsigned p1,uint32_t p2,const void*body,unsigned n){
 uint8_t b[256];unsigned bytes=(8+n+7)&~7u;if(n>sizeof(b)-8||g->broken)return sg_fail(g);
 for(unsigned i=0;i<bytes;i++)b[i]=0;sg16(b,cmd);sg16(b+2,p1);sg32(b+4,p2);for(unsigned i=0;i<n;i++)b[8+i]=((const uint8_t*)body)[i];
 if(!g->emit(g->ctx,b,bytes))return sg_fail(g);g->sent++;return 1;
}
/* Determine complete frame length without overflow;0 incomplete,-1 invalid. */
static int sg_frame_length(const uint8_t*b,unsigned available,unsigned ring){
 if(available<8)return 0;unsigned c=slpi_u16(b),n=8;uint32_t p=slpi_u32(b+4);
 if(c==2){if(!p||p>32)return -1;n+=p;}
 else if(c==5){if(p>64)return -1;n+=p*8;}
 else if(c==9||c==12){if(available<16)return 0;uint32_t chunk=slpi_u32(b+8);if(!chunk||chunk>SG_MAX)return -1;n=16+chunk;}
 n=(n+7)&~7u;if(ring<32||n>ring-16)return -1;return available<n?0:(int)n;
}
static int sg_queue(struct sg_core*g,uint32_t type,uint32_t node,uint32_t fp,uint32_t tp,const uint32_t words[5]){
 if(g->qcount==32)return sg_fail(g);struct sg_control*c=&g->pending[(g->qhead+g->qcount)%32];c->type=type;c->node=node;c->from_port=fp;c->to_port=tp;for(unsigned i=0;i<5;i++)c->words[i]=words[i];g->qcount++;return 1;
}
static int sg_qrtr(struct sg_core*g,const uint8_t*b,unsigned n){
 uint32_t type,fromnode,fromport,tonode,toport,confirm,size,at;
 if(n>=32&&slpi_u32(b)==1){type=slpi_u32(b+4);fromnode=slpi_u32(b+8);fromport=slpi_u32(b+12);confirm=slpi_u32(b+16);size=slpi_u32(b+20);tonode=slpi_u32(b+24);toport=slpi_u32(b+28);at=32;}
 else if(n>=16&&b[0]==3){type=b[1];confirm=b[2]&1;size=slpi_u32(b+4);at=16+b[3];fromnode=slpi_u16(b+8);fromport=slpi_u16(b+10);tonode=slpi_u16(b+12);toport=slpi_u16(b+14);if(fromport==0xfffe)fromport=SG_CTRL;if(toport==0xfffe)toport=SG_CTRL;if(tonode==0xffff)tonode=~0u;}
 else return sg_fail(g);
 if(at>n||!size||size>n-at||n-at-size>3||confirm>1||(tonode!=1&&tonode!=~0u))return sg_fail(g);
 for(unsigned i=at+size;i<n;i++)if(b[i])return sg_fail(g);
 if(++g->qrtr_in>1024)return sg_fail(g);
 if(confirm&&type!=7){uint32_t w[5]={7,tonode,toport,0,0};if(!sg_queue(g,7,fromnode,toport,fromport,w))return 0;}
 if(type==7)return 1;
 if(toport!=SG_CTRL){
  if(g->lookup&&g->lookup_sent&&type==1&&toport==SG_SEE_PORT&&fromnode==g->lookup_node&&fromport==g->lookup_port){
   /* Only responses belong to request transactions. Indications have their
    * own sequence and may repeat as a subscription produces updates. */
   const uint8_t *qmi = b + at;
   if(size < 7 || slpi_u16(qmi + 5) != size - 7)return sg_fail(g);
   unsigned message = slpi_u16(qmi + 3);
   bool registry_ack=false;
   if(g->rpc_probe==8 && g->query_base==SG_QUERY_COUNT && g->registry.port){
    if(qmi[0]==2 && message==0x20 && slpi_u16(qmi+1)>=SG_REG_TXN_BASE &&
       slpi_u16(qmi+1)<SG_REG_TXN_BASE+SR_COUNT){
     if(!sr_ack(&g->registry,fromnode,fromport,qmi,size))return 1;
     registry_ack=true;
    }else if(qmi[0]==2 && message==0x20 && slpi_u16(qmi+1)>=SD_TXN_BASE &&
             slpi_u16(qmi+1)<SD_TXN_BASE+SD_COUNT){
     if(!sd_ack(&g->deps,&g->registry,qmi,size))return 1;
     registry_ack=true;
    }else if(g->vendor_attrs && qmi[0]==2 && message==0x20 &&
             slpi_u16(qmi+1)>=VA_TXN && slpi_u16(qmi+1)<VA_TXN+VA_COUNT){
     if(!va_ack(&g->vendor,&g->registry,qmi,size))return 1;
     registry_ack=true;
    }else if(g->wise_stream && qmi[0]==2 && message==0x20 && slpi_u16(qmi+1)==VW_TXN){
     if(!vw_ack(&g->wise,&g->registry,qmi,size))return 1;
     registry_ack=true;
    }else if(qmi[0]==4 && message==0x21){
     (void)sr_observe_lookup(&g->registry,fromnode,fromport,qmi,size);
     (void)sr_report(&g->registry,fromnode,fromport,qmi,size);
     sd_report(&g->deps,&g->registry,qmi,size);
     if(g->vendor_attrs)va_report(&g->vendor,&g->registry,qmi,size);
     if(g->wise_stream)vw_report(&g->wise,&g->vendor,&g->registry,qmi,size);
    }
   }
   if(registry_ack){ /* fall through to bounded packet capture */ }
   else if(qmi[0] == 2 && message == 0x20){
    unsigned txn = slpi_u16(qmi + 1);
    if(txn == 0 || txn > SG_QUERY_COUNT*2 || !(g->query_txn_expected & (1u << txn)) ||
       (g->query_txn_seen & (1u << txn)))return 1;
    g->query_txn_seen |= 1u << txn;
   }else if(qmi[0] != 4 || (message != 0x21 && message != 0x22))return 1;
   if(size>1100)return sg_fail(g);
   /* The parser above remains live after the bounded diagnostic capture fills. */
   if(g->reply_count>=48){g->capture_dropped++;return 1;}
   unsigned i=g->reply_count++;g->reply_size[i]=size;
   for(unsigned j=0;j<size;j++)g->replies[i][j]=b[at+j];
   g->note(g->ctx,24,i,size,fromnode,fromport);
  }
  return 1;
 }
 if(size<4)return sg_fail(g);const uint8_t*p=b+at;uint32_t command=slpi_u32(p);
 if(type!=1&&type!=command)return sg_fail(g);
 if(command==2){g->hello_received++;if(!g->hello_sent){uint32_t w[5]={2,0,0,0,0};g->hello_sent=1;if(!sg_queue(g,2,fromnode,SG_CTRL,SG_CTRL,w))return 0;}g->note(g->ctx,2,fromnode,0,0,0);}
 else if(command==4||command==5){
  if(size<20)return sg_fail(g);uint32_t service=slpi_u32(p+4),inst=slpi_u32(p+8),node=slpi_u32(p+12),port=slpi_u32(p+16);int found=-1,empty=-1;
  for(unsigned i=0;i<64;i++){struct sg_service*s=&g->services[i];if(s->present&&s->node==node&&s->port==port)found=i;if(!s->present&&empty<0)empty=i;}
  if(g->registry.port==port && g->registry.node==node &&
     (command==5 || service!=400 || inst!=1))g->registry.invalidated=true;
  if(command==5){if(found>=0)g->services[found].present=0;}
  else{if(found<0)found=empty;if(found<0){g->service_overflow++;return sg_fail(g);}g->services[found]=(struct sg_service){service,inst,node,port,1};}
  if(g->service_events++<32)g->note(g->ctx,command,service,inst,node,port);
 }else if(command==10){if(size<12)return sg_fail(g);if(g->lookups++<32)g->note(g->ctx,10,slpi_u32(p+4),slpi_u32(p+8),fromnode,fromport);}
 else if(command==3){if(g->registry.node==fromnode)g->registry.invalidated=true;for(unsigned i=0;i<64;i++)if(g->services[i].node==fromnode)g->services[i].present=0;g->note(g->ctx,3,fromnode,0,0,0);}
 return 1;
}
static int sg_offer(struct sg_core*g,unsigned i){uint8_t b[8];sg32(b,g->rx[i].size);sg32(b+4,i+1);return sg_emit(g,5,1,1,b,8);}
static int sg_rpc_send_if_ready(struct sg_core *g);
static bool
sg_registry_reads_done(const struct sg_core *g)
{
    const unsigned all=(1u<<SR_COUNT)-1;
    const unsigned late_acks=((1u<<SG_QUERY_COUNT)-1)<<(SG_QUERY_COUNT+1);
    const struct sr_client *r=&g->registry;
    if(g->rpc_probe!=8 || !g->wrote_registry || g->lookup!=3 ||
       g->query_base!=SG_QUERY_COUNT || g->lookup_sent!=SG_QUERY_COUNT ||
       (g->query_txn_seen&late_acks)!=late_acks || !g->agreed || !g->open ||
       !g->hello_received || g->broken || g->qcount || r->invalidated ||
       !sl_nonzero(r->registry) || r->sent!=all || (r->ack_ok&r->ack_fail) ||
       ((r->ack_ok&r->received)|r->ack_fail)!=all)return false;
    /* No half-received QRTR packet or outstanding AP transmission. A
     * pending listener.next2 call is quiesced by the existing owned stop. */
    for(unsigned i=0;i<10;i++)if(g->rx[i].fill)return false;
    for(unsigned i=0;i<64;i++)if(g->peer[i].used)return false;
    return true;
}

static int
sg_send_registry_read(struct sg_core *g)
{
    struct sr_client *c=&g->registry;
    if(g->rpc_probe!=8 || g->qcount || g->query_base!=SG_QUERY_COUNT ||
       g->lookup!=3 || g->lookup_sent!=SG_QUERY_COUNT || c->invalidated ||
       !sl_nonzero(c->registry))return 1;
    unsigned group=0;
    while(group<SR_COUNT && (c->sent & (1u<<group)))group++;
    bool dependency=group==SR_COUNT;
    if(dependency){
        if(!g->deps.started_us)return 1;
        group=0;
        while(group<SD_COUNT && (g->deps.sent&(1u<<group)))group++;
        if(group==SD_COUNT && !g->vendor_attrs)return 1;
    }
    uint16_t txn=(uint16_t)((dependency?SD_TXN_BASE:SG_REG_TXN_BASE)+group);
    uint8_t b[8+32+128]={0};
    bool attributes=dependency && group==SD_COUNT,sample=false;
    size_t qmi;
    if(attributes){
        qmi=va_next(&g->vendor,c,&txn,b+40,128);
        if(!qmi && g->wise_stream){
            qmi=vw_request(&g->wise,&g->vendor,c,b+40,128);
            if(qmi){sample=true;txn=VW_TXN;}
        }
        if(!qmi)return 1;
    }else qmi=dependency?sd_request(group,b+40,128):sr_request(c,group,txn,b+40,128);
    if(!qmi)return sg_fail(g);
    unsigned payload=32+(unsigned)qmi;
    int slot=-1;
    for(unsigned i=0;i<64;i++)if(g->peer[i].present && !g->peer[i].used &&
        g->peer[i].size>=payload){slot=(int)i;break;}
    if(slot<0){
        if(!g->intent_requested){g->intent_requested=1;return sg_emit(g,7,1,payload,NULL,0);}
        return 1;
    }
    sg32(b,payload);sg32(b+8,1);sg32(b+12,1);sg32(b+16,1);
    sg32(b+20,SG_SEE_PORT);sg32(b+28,(uint32_t)qmi);sg32(b+32,c->node);sg32(b+36,c->port);
    if(!sg_emit(g,9,1,g->peer[slot].id,b,40+(unsigned)qmi))return 0;
    g->peer[slot].used=1;
    if(sample)g->wise.sent=1;
    else if(attributes)g->vendor.sent|=1u<<(txn-VA_TXN);
    else if(dependency)g->deps.sent|=1u<<group;
    else if(!sr_sent(c,group,txn))return sg_fail(g);
    g->note(g->ctx,sample?28:attributes?27:dependency?26:25,attributes?(unsigned)txn-VA_TXN:group,txn,c->node,c->port);
    return 1;
}

static int sg_tick(struct sg_core*g){
 if(g->broken)return 0;
 if(g->agreed&&!g->asked){if(!sg_emit(g,2,1,7,"IPCRTR",7))return 0;g->asked=1;}
 if(g->agreed&&g->theirs&&!g->ack_sent){if(!sg_emit(g,4,g->theirs,0,0,0))return 0;g->ack_sent=1;}
 if(g->agreed&&g->theirs&&g->ack_sent&&g->acknowledged&&!g->open){g->open=1;g->note(g->ctx,20,1,g->theirs,g->features,0);for(unsigned i=0;i<10;i++)if(!sg_offer(g,i))return 0;g->offered=1;}
 if(!g->open)return 1;
 if(!g->hello_sent){uint32_t w[5]={2,0,0,0,0};g->hello_sent=1;if(!sg_queue(g,2,~0u,SG_CTRL,SG_CTRL,w))return 0;}
 if(!g->qcount&&g->lookup&&g->lookup_sent<(g->lookup==3?SG_QUERY_COUNT:1)&&g->hello_received){
  struct sg_service*service=0;
  for(unsigned i=0;i<64;i++)if(g->services[i].present&&g->services[i].service==400&&g->services[i].instance==1){service=&g->services[i];break;}
  if(service){
   /* All lookups append to one QRTR client; each has a distinct QMI txn.
    * Events can arrive after a later request, so no event advances this index. */
   if(g->lookup_sent&&(service->node!=g->lookup_node||service->port!=g->lookup_port))return sg_fail(g);
   const uint8_t *query=sg_lookup_qmi;unsigned query_size=sizeof(sg_lookup_qmi);
   if(g->lookup==2){query=sg_lookup_ambient_light_qmi;query_size=sizeof(sg_lookup_ambient_light_qmi);}
   else if(g->lookup==3){query=sg_queries[g->lookup_sent].bytes;query_size=sg_queries[g->lookup_sent].size;}
   const unsigned payload=32+query_size;int slot=-1;
   for(unsigned i=0;i<64;i++)if(g->peer[i].present&&!g->peer[i].used&&g->peer[i].size>=payload){slot=i;break;}
   if(slot<0){if(!g->intent_requested){g->intent_requested=1;return sg_emit(g,7,1,payload,0,0);}return 1;}
   uint8_t b[8+32+sizeof(sg_lookup_qmi)];for(unsigned i=0;i<sizeof(b);i++)b[i]=0;
   sg32(b,payload);sg32(b+8,1);sg32(b+12,1);sg32(b+16,1);sg32(b+20,SG_SEE_PORT);
   sg32(b+28,query_size);sg32(b+32,service->node);sg32(b+36,service->port);
   for(unsigned i=0;i<query_size;i++)b[40+i]=query[i];
   if(g->lookup==3)sg16(b+41,g->query_base+g->lookup_sent+1);
   if(!sg_emit(g,9,1,g->peer[slot].id,b,40+query_size))return 0;
   g->peer[slot].used=1;g->lookup_node=service->node;g->lookup_port=service->port;
   if(g->rpc_probe==8 && g->lookup==3 && g->query_base==SG_QUERY_COUNT && !g->registry.port)
    (void)sr_bind(&g->registry,service->service,service->instance&255,service->instance>>8,
                  service->node,service->port);
   unsigned txn=g->lookup==3?g->query_base+g->lookup_sent+1:1;g->query_txn_expected|=1u<<txn;
   g->note(g->ctx,23,service->node,service->port,query_size,g->lookup_sent);g->lookup_sent++;
  }
 }
 if(!g->qcount){if(!sg_send_registry_read(g))return 0;return sg_rpc_send_if_ready(g);}int intent=-1;
 for(unsigned i=0;i<64;i++)if(g->peer[i].present&&!g->peer[i].used&&g->peer[i].size>=52){intent=i;break;}
 if(intent<0){if(!g->intent_requested){g->intent_requested=1;return sg_emit(g,7,1,52,0,0);}return 1;}
 struct sg_control*c=&g->pending[g->qhead];uint8_t data[60];sg32(data,52);sg32(data+4,0);sg32(data+8,1);sg32(data+12,c->type);sg32(data+16,1);sg32(data+20,c->from_port);sg32(data+24,0);sg32(data+28,20);sg32(data+32,c->node);sg32(data+36,c->to_port);for(unsigned i=0;i<5;i++)sg32(data+40+4*i,c->words[i]);
 if(!sg_emit(g,9,1,g->peer[intent].id,data,60))return 0;g->peer[intent].used=1;g->qhead=(g->qhead+1)%32;g->qcount--;return 1;
}
/* GLINK credits are serialized on channel 2. FastRPC completions can arrive
 * in either order after next2's transport packet has been retired. */
static int sg_foreground_send(struct sg_core *g)
{
    struct sg_rpc_call *fg=&g->foreground;
    if (g->rpc_probe<6 || !g->next_started || !g->rpc.tx_done || !fg->prepared)
        return 1;
    bool acquire=false;
    if (!g->foreground_armed) {
        g->foreground_armed=true;
        acquire=!g->rpc_reusable;
    } else if (fg->tx_done && fg->completed && !fg->retval && !g->module_start) {
        if (!g->prepare_module_start || !g->prepare_module_start(g->rpc_owner,&fg->message))
            return sg_fail(g);
        g->module_start=true;
        fg->sent=fg->completed=fg->tx_done=false;
        fg->flags=fg->stale=fg->early=0;
        acquire=!g->foreground_reusable;
    }
    if (acquire) {
        g->rpc.have_peer=false;g->rpc_intent_reply=0;
        return sg_emit(g,7,2,sizeof(fg->message),0,0);
    }
    if (fg->sent || !g->rpc.have_peer || g->rpc_intent_reply!=2) return 1;
    uint8_t body[8+sizeof(fg->message)]={0};
    sg32(body,sizeof(fg->message));
    memcpy(body+8,&fg->message,sizeof(fg->message));
    fg->peer_id=g->rpc.peer_id;
    fg->sent=true;
    return sg_emit(g,9,2,fg->peer_id,body,sizeof(body));
}

static int sg_rpc_send_if_ready(struct sg_core *g)
{
    struct sg_rpc_call *call = &g->rpc;
    if(g->rpc_probe==8 && g->next_started && g->module_start && g->foreground.tx_done &&
       call->completed && call->tx_done && !call->retval && g->callbacks<SG_REGISTRY_CALLBACK_LIMIT){
        bool reuse=g->callbacks?g->rpc_reusable:g->foreground_reusable;
        if(!g->reply_callback || !g->reply_callback(g->rpc_owner,&call->message,6+g->callbacks))
            return sg_fail(g);
        g->callbacks++;
        call->sent=call->completed=call->tx_done=false;
        call->flags=call->early=call->stale=0;
        if(!reuse){
            call->have_peer=false;g->rpc_intent_reply=0;
            return sg_emit(g,7,2,sizeof(call->message),0,0);
        }
    }
    if (g->rpc_probe>=4 && call->completed && call->tx_done && !call->retval &&
        (!g->listener_started || (g->rpc_probe>=5 && !g->next_started))) {
        if (g->listener_started) {
            if (!g->prepare_next || !g->prepare_next(g->rpc_owner,&call->message)) return sg_fail(g);
            g->next_started=true;
        } else {
        if (!g->rpc_tgid) return sg_fail(g);
        /* Matching OnePlus ioctl ABI: user TGID, method 3, zero arguments.
         * A zero-used buffer has addr 0 but buf_page_size(0) is 4096. */
        call->message.client_id=(int32_t)g->rpc_tgid;
        call->message.ctx=hk_fastrpc_ctx(2,0,HK_FASTRPC_SENSORS_PD);
        call->message.handle=HK_FASTRPC_LISTENER_HANDLE;
        call->message.scalars=hk_fastrpc_scalars(HK_FASTRPC_LISTENER_INIT2_METHOD,0,0);
        call->message.addr=0;
        call->message.size=4096;
        g->listener_started=true;
        }
        call->sent=call->completed=call->tx_done=false;
        call->early=call->stale=call->flags=0;
        if (!g->rpc_reusable) {
            call->have_peer=false;
            g->rpc_intent_reply=0;
            return sg_emit(g,7,2,sizeof(call->message),0,0);
        }
    }
    if (g->rpc_probe < 3 || !call->prepared || !call->offered || call->sent ||
        !call->have_peer || g->rpc_intent_reply != 2 || !g->rpc_ack)
        return sg_foreground_send(g);
    bool see_ready = false;
    for (unsigned i = 0; i < 64; i++)
        if (g->services[i].present && g->services[i].service == 400 && g->services[i].instance == 1) {
            see_ready = true;
            break;
        }
    if (!g->hello_received || !see_ready) return 1;
    uint8_t body[8 + sizeof(call->message)] = {0};
    sg32(body, sizeof(call->message));
    memcpy(body + 8, &call->message, sizeof(call->message));
    call->sent = true; /* never retry an ambiguous publication */
    return sg_emit(g, 9, 2, call->peer_id, body, sizeof(body));
}

static int sg_rpc_receive_data(struct sg_core *g, const uint8_t *b, unsigned command, unsigned intent)
{
    struct sg_rpc_call *call = &g->rpc;
    unsigned chunk = slpi_u32(b + 8), left = slpi_u32(b + 12);
    if (!call->sent || !call->offered || intent != 1 ||
        chunk > sizeof(call->response) - call->fill ||
        left > sizeof(call->response) - call->fill - chunk ||
        (command == 9 && call->fill) ||
        (command == 12 && (!call->fill || call->remaining != chunk + left)))
        return sg_fail(g);
    memcpy(call->response + call->fill, b + 16, chunk);
    call->fill += chunk;
    call->remaining = left;
    if (left) return 1;

    const uint8_t *r = call->response;
    if (call->fill != 16 && call->fill != 24) return sg_fail(g);
    uint64_t ctx = slpi_u32(r) | ((uint64_t)slpi_u32(r + 4) << 32);
    unsigned flags = call->fill == 24 ? slpi_u32(r + 12) : 0;
    if ((call->fill == 24 && slpi_u32(r + 20) != 2) || flags > 3)
        return sg_fail(g);
    struct sg_rpc_call *recipient = call;
    if (g->foreground.sent && g->foreground.message.ctx == ctx)
        recipient=&g->foreground;
    if (ctx != recipient->message.ctx) recipient->stale++;
    else if (flags == 1 || flags == 2) recipient->early++;
    else if (!recipient->completed) {
        recipient->retval = (int32_t)slpi_u32(r + 8);
        recipient->flags = flags;
        recipient->completed = true;
    }
    call->fill = 0;
    if (!sg_emit(g, g->features & 1 ? 14 : 6, 2, 1, NULL, 0)) return 0;
    if (!(g->features & 1)) {
        uint8_t offer[8];sg32(offer, sizeof(call->response));sg32(offer+4,1);
        return sg_emit(g,5,2,1,offer,sizeof(offer));
    }
    return 1;
}
static int sg_receive(struct sg_core*g,const uint8_t*b,unsigned n){
 if(n<8||g->broken||++g->received>(g->rpc_probe==8?3u*SG_REGISTRY_CALLBACK_LIMIT+512u:4096u))return sg_fail(g);int framed=sg_frame_length(b,n,65536);if(framed<0||(unsigned)framed!=n)return sg_fail(g);
 unsigned c=slpi_u16(b),p1=slpi_u16(b+2);uint32_t p2=slpi_u32(b+4);
 /* Diagnostic channel 2 only exchanges GLINK channel control. Record peer
  * intents separately. This channel has no local receive intents or DMA arena. */
 static const char rpc_name[]="fastrpcglink-apps-dsp";
 if(g->rpc_probe && c==2 && p2==sizeof(rpc_name)){
  unsigned i=0;
  while(i<p2 && b[8+i]==(uint8_t)rpc_name[i])i++;
  if(i==p2){
   if(!g->agreed || !p1 || p1==g->theirs || g->rpc_requested)return sg_fail(g);
   g->rpc_remote=p1;
   if(!sg_emit(g,4,p1,0,0,0) || !sg_emit(g,2,2,sizeof(rpc_name),rpc_name,sizeof(rpc_name)))return 0;
   g->rpc_requested=1;
   return 1;
  }
 }
 if(g->rpc_requested && c==4 && p1==2){
  g->rpc_ack=1;
  /* sizeof native smq_msg: pid/tid (8), invoke header (16), page (16). */
  if(g->rpc_probe>=3 && !g->rpc.offered){
   uint8_t offer[8];sg32(offer,sizeof(g->rpc.response));sg32(offer+4,1);
   if(!g->rpc.prepared || !sg_emit(g,5,2,1,offer,sizeof(offer)))return sg_fail(g);
   g->rpc.offered=true;
  }
  if(g->rpc_probe>=2 && !g->rpc_intent_requested){
   if(!sg_emit(g,7,2,40,0,0))return 0;
   g->rpc_intent_requested=1;
  }
  return 1;
 }
 if(g->rpc_remote && p1==g->rpc_remote){
  if(c==5){
   for(unsigned j=0;j<p2;j++){
    unsigned size=slpi_u32(b+8+j*8);
    if(!size)return sg_fail(g);
    if(g->rpc_probe>=3 && !g->rpc.have_peer && size>=sizeof(g->rpc.message)){
     g->rpc.peer_id=slpi_u32(b+12+j*8);g->rpc.have_peer=true;
    }
    if(size>g->rpc_max_intent)g->rpc_max_intent=size;
   }
   g->rpc_intents+=p2;return sg_rpc_send_if_ready(g);
  }
  if(c==8){
   if(!g->rpc_intent_requested || p2>1)return sg_fail(g);
   g->rpc_intent_reply=p2?2:1;
   return sg_rpc_send_if_ready(g);
  }
  if(c==7)return sg_emit(g,8,2,0,0,0);
  if(c==3){g->rpc_ack=0;g->rpc_remote=0;return sg_emit(g,11,p1,0,0,0);}
  if(c==9||c==12)return g->rpc_probe>=3 ? sg_rpc_receive_data(g,b,c,p2) : sg_fail(g);
  if((c==6||c==14) && g->rpc_probe>=3){
   if(g->foreground.sent && !g->foreground.tx_done){
    if(p2!=g->foreground.peer_id || (c==14 && !(g->features&1)))return sg_fail(g);
    g->foreground.tx_done=true;g->foreground_reusable=c==14;return 1;
   }
   if(!g->rpc.sent || p2!=g->rpc.peer_id || (c==14 && !(g->features&1)))return sg_fail(g);
   g->rpc.tx_done=true;g->rpc_reusable=c==14;return 1;
  }
 }
 if(c==0){
  if(!p1||++g->version_rounds>8)return sg_fail(g);unsigned previous=g->features;if(p1==1)g->features&=p2;
  if(g->open&&previous!=g->features)return sg_fail(g);
  if(!sg_emit(g,1,1,g->features,0,0))return 0;
  if(g->agreed&&previous!=g->features){g->agreed=0;if(!sg_emit(g,0,1,g->features,0,0))return 0;}
 }
 else if(c==1){
  if(!p1||++g->version_rounds>8)return sg_fail(g);
  if(p1==1&&p2==g->features)g->agreed=1;
  else{if(g->open)return sg_fail(g);g->agreed=0;if(p1==1)g->features&=p2;if(!sg_emit(g,0,1,g->features,0,0))return 0;}
 }
 else if(c==2){
  for(unsigned i=0;i<p2;i++)if((i+1==p2&&b[8+i]!=0)||(i+1<p2&&(b[8+i]<32||b[8+i]>126)))return sg_fail(g);
  const char*name="IPCRTR";unsigned match=p2==7;for(unsigned i=0;match&&i<7;i++)if(b[8+i]!=(uint8_t)name[i])match=0;
  if(!match){unsigned index=g->other_opens++;if(index<16){for(unsigned i=0;i<p2;i++)g->others[index][i]=(char)b[8+i];g->note(g->ctx,21,p1,index,0,0);}return 1;}
  if(!p1||(g->theirs&&g->theirs!=p1))return sg_fail(g);g->theirs=p1;
 }else if(c==4){if(p1!=1||!g->asked)return sg_fail(g);g->acknowledged=1;}
 else if(c==5){if(p1!=g->theirs||!p1)return sg_fail(g);g->intent_requested=0;for(unsigned i=0;i<p2;i++){
   uint32_t size=slpi_u32(b+8+i*8),id=slpi_u32(b+12+i*8);/* Remote intent ID 0 is valid: native SLPI advertises it. Presence has its own flag. */if(!size||size>0x100000)return sg_fail(g);int found=-1,empty=-1;
   for(unsigned j=0;j<64;j++){if(g->peer[j].present&&g->peer[j].id==id)found=j;if(!g->peer[j].present&&empty<0)empty=j;}
   if(found<0)found=empty;if(found<0||g->peer[found].used)return sg_fail(g);g->peer[found]=(struct sg_intent){id,size,0,1};
  }}
 else if(c==6||c==14){if(p1!=g->theirs||(c==14&&!(g->features&1)))return sg_fail(g);unsigned found=0;for(unsigned i=0;i<64;i++)if(g->peer[i].present&&g->peer[i].id==p2){if(!g->peer[i].used)return sg_fail(g);g->peer[i].used=0;g->peer[i].present=c==14;found=1;}if(!found)return sg_fail(g);}
 else if(c==7){if(p1==g->theirs){/* Preallocated10 intents suffice; deny extra allocation. */if(!sg_emit(g,8,1,0,0,0))return 0;}}
 else if(c==8){if(p1!=g->theirs||p2>1)return sg_fail(g);if(!p2)return sg_fail(g);}
 else if(c==9||c==12){
  if(!g->open||p1!=g->theirs||!p2||p2>10)return sg_fail(g);struct sg_rx*r=&g->rx[p2-1];uint32_t chunk=slpi_u32(b+8),left=slpi_u32(b+12);
  if(chunk>r->size-r->fill||left>r->size-r->fill-chunk)return sg_fail(g);
  if((c==9&&r->fill)||(c==12&&(!r->fill||r->left!=chunk+left)))return sg_fail(g);
  for(unsigned i=0;i<chunk;i++)r->data[r->fill+i]=b[16+i];r->fill+=chunk;r->left=left;
  if(!left){if(!sg_qrtr(g,r->data,r->fill))return 0;r->fill=0;if(!sg_emit(g,g->features&1?14:6,1,p2,0,0))return 0;if(!(g->features&1)&&!sg_offer(g,p2-1))return 0;}
 }else if(c==13){g->note(g->ctx,13,0,0,0,0);}
 else if(c==3){if(p1==g->theirs){(void)sg_emit(g,11,p1,0,0,0);return sg_fail(g);}}
 else if(c==11||c==15){}else return sg_fail(g);
 return sg_tick(g);
}
static int sg_init(struct sg_core*g,void*ctx,int(*emit)(void*,const uint8_t*,unsigned),void(*note)(void*,unsigned,uint32_t,uint32_t,uint32_t,uint32_t)){
 /* Caller zeroes static context before ownership start, never during live reuse. */
 g->ctx=ctx;g->emit=emit;g->note=note;g->features=1;for(unsigned i=0;i<10;i++)g->rx[i].size=i<5?0x800:i<8?0x2000:0x4400;return sg_emit(g,0,1,1,0,0);
}
#endif
