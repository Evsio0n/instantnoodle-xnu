/* Bounded IPCRTR-only protocol core. No allocator, hardware or SEE interface. */
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
struct sg_core {
 unsigned lookup,lookup_sent,lookup_node,lookup_port,reply_count;
 unsigned reply_size[8];uint8_t replies[8][1100];
 int(*emit)(void*,const uint8_t*,unsigned);void(*note)(void*,unsigned,uint32_t,uint32_t,uint32_t,uint32_t);void*ctx;
 unsigned broken,features,version_rounds,agreed,asked,ack_sent,acknowledged,open,offered,theirs;
 unsigned received,sent,other_opens,hello_sent,hello_received,intent_requested,qrtr_in,lookups;
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
   if(size>1100||g->reply_count>=8)return sg_fail(g);
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
  if(command==5){if(found>=0)g->services[found].present=0;}
  else{if(found<0)found=empty;if(found<0){g->service_overflow++;return sg_fail(g);}g->services[found]=(struct sg_service){service,inst,node,port,1};}
  if(g->service_events++<32)g->note(g->ctx,command,service,inst,node,port);
 }else if(command==10){if(size<12)return sg_fail(g);if(g->lookups++<32)g->note(g->ctx,10,slpi_u32(p+4),slpi_u32(p+8),fromnode,fromport);}
 else if(command==3){for(unsigned i=0;i<64;i++)if(g->services[i].node==fromnode)g->services[i].present=0;g->note(g->ctx,3,fromnode,0,0,0);}
 return 1;
}
static int sg_offer(struct sg_core*g,unsigned i){uint8_t b[8];sg32(b,g->rx[i].size);sg32(b+4,i+1);return sg_emit(g,5,1,1,b,8);}
static int sg_tick(struct sg_core*g){
 if(g->broken)return 0;
 if(g->agreed&&!g->asked){if(!sg_emit(g,2,1,7,"IPCRTR",7))return 0;g->asked=1;}
 if(g->agreed&&g->theirs&&!g->ack_sent){if(!sg_emit(g,4,g->theirs,0,0,0))return 0;g->ack_sent=1;}
 if(g->agreed&&g->theirs&&g->ack_sent&&g->acknowledged&&!g->open){g->open=1;g->note(g->ctx,20,1,g->theirs,g->features,0);for(unsigned i=0;i<10;i++)if(!sg_offer(g,i))return 0;g->offered=1;}
 if(!g->open)return 1;
 if(!g->hello_sent){uint32_t w[5]={2,0,0,0,0};g->hello_sent=1;if(!sg_queue(g,2,~0u,SG_CTRL,SG_CTRL,w))return 0;}
 if(!g->qcount&&g->lookup&&!g->lookup_sent&&g->hello_received){
  struct sg_service*service=0;
  for(unsigned i=0;i<64;i++)if(g->services[i].present&&g->services[i].service==400&&g->services[i].instance==1){service=&g->services[i];break;}
  if(service){
   const unsigned payload=32+sizeof(sg_lookup_qmi);int slot=-1;
   for(unsigned i=0;i<64;i++)if(g->peer[i].present&&!g->peer[i].used&&g->peer[i].size>=payload){slot=i;break;}
   if(slot<0){if(!g->intent_requested){g->intent_requested=1;return sg_emit(g,7,1,payload,0,0);}return 1;}
   uint8_t b[8+32+sizeof(sg_lookup_qmi)];for(unsigned i=0;i<sizeof(b);i++)b[i]=0;
   sg32(b,payload);sg32(b+8,1);sg32(b+12,1);sg32(b+16,1);sg32(b+20,SG_SEE_PORT);
   sg32(b+28,sizeof(sg_lookup_qmi));sg32(b+32,service->node);sg32(b+36,service->port);
   const uint8_t*query=g->lookup==2?sg_lookup_all_qmi:sg_lookup_qmi;
   for(unsigned i=0;i<sizeof(sg_lookup_qmi);i++)b[40+i]=query[i];
   if(!sg_emit(g,9,1,g->peer[slot].id,b,sizeof(b)))return 0;
   g->peer[slot].used=1;g->lookup_sent=1;g->lookup_node=service->node;g->lookup_port=service->port;
   g->note(g->ctx,23,service->node,service->port,sizeof(sg_lookup_qmi),0);
  }
 }
 if(!g->qcount)return 1;int intent=-1;
 for(unsigned i=0;i<64;i++)if(g->peer[i].present&&!g->peer[i].used&&g->peer[i].size>=52){intent=i;break;}
 if(intent<0){if(!g->intent_requested){g->intent_requested=1;return sg_emit(g,7,1,52,0,0);}return 1;}
 struct sg_control*c=&g->pending[g->qhead];uint8_t data[60];sg32(data,52);sg32(data+4,0);sg32(data+8,1);sg32(data+12,c->type);sg32(data+16,1);sg32(data+20,c->from_port);sg32(data+24,0);sg32(data+28,20);sg32(data+32,c->node);sg32(data+36,c->to_port);for(unsigned i=0;i<5;i++)sg32(data+40+4*i,c->words[i]);
 if(!sg_emit(g,9,1,g->peer[intent].id,data,60))return 0;g->peer[intent].used=1;g->qhead=(g->qhead+1)%32;g->qcount--;return 1;
}
static int sg_receive(struct sg_core*g,const uint8_t*b,unsigned n){
 if(n<8||g->broken||++g->received>4096)return sg_fail(g);int framed=sg_frame_length(b,n,65536);if(framed<0||(unsigned)framed!=n)return sg_fail(g);
 unsigned c=slpi_u16(b),p1=slpi_u16(b+2);uint32_t p2=slpi_u32(b+4);
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
