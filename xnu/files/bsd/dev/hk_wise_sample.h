/* Vendor wise_light trial. Native handler decodes the standard sensor event
 * as data[0]=reported lux, data[1]=C channel, data[2]=period in ms. */
#ifndef HK_WISE_SAMPLE_H
#define HK_WISE_SAMPLE_H
#include "hk_vendor_attributes.h"
enum { VW_TXN=0x4500,VW_MAX=6 };
struct vw_sample {uint64_t ticks;uint32_t status,lux_bits,c_bits,period_bits;unsigned fields;};
struct vw_state {
 struct sl_config config;unsigned sent,accepted,rejected,invalid,count,bad_events;
 struct vw_sample samples[VW_MAX];
};
static bool vw_mode(const struct va_state *v,struct sl_config *out)
{
 const struct sl_attrs *a=&v->attrs[0];
 const uint32_t need=sl_bit(SL_NAME)|sl_bit(SL_TYPE)|sl_bit(SL_AVAILABLE)|sl_bit(SL_STREAM);
 if(!(v->ack&v->received&1)||(v->invalid&1)||v->count[0]!=1||
    !sl_nonzero(v->uid[0])||!sl_equal(a->source,v->uid[0])||
    (a->present&need)!=need||!a->available||strcmp(a->type,"wise_light"))return false;
 struct sl_config cfg={.source=a->source};
 if(a->stream==1)cfg.mode=SL_ON_CHANGE;
 else if(a->stream==0){
  uint32_t selected=0;
  for(unsigned i=0;i<a->rate_count;i++){
   uint32_t n=a->rates[i];
   if(n>=UINT32_C(0x3f800000)&&n<=UINT32_C(0x41a00000)&&(!selected||n<selected))selected=n;
  }
  if(!selected)return false;
  cfg.mode=SL_STREAMING;cfg.rate_binary32=selected;
 }else return false;
 *out=cfg;return true;
}
static size_t vw_request(struct vw_state *s,const struct va_state *v,const struct sr_client *reg,uint8_t *out,size_t cap)
{
 if(s->sent||s->invalid||reg->invalidated||!sl_nonzero(reg->registry)||!vw_mode(v,&s->config))return 0;
 return sl_config_request(out,cap,VW_TXN,&s->config);
}
static bool vw_ack(struct vw_state *s,const struct sr_client *reg,const uint8_t *p,size_t n)
{
 if(reg->invalidated||!s->sent||s->accepted||s->rejected||n<7||sl_u16(p+1)!=VW_TXN)return false;
 int r=sr_ack_result(p,n,reg->client_id);if(!r)return false;
 s->accepted=r>0;s->rejected=r<0;return true;
}
static bool vw_decode(const struct sl_event *e,struct vw_sample *out)
{
 if(e->id!=1025||!e->ticks)return false;
 struct sl_span pb=e->payload;uint32_t first[3]={0};unsigned total=0;
 bool status=false;struct vw_sample r={.ticks=e->ticks};
 while(pb.n){
  struct sl_field f;if(!sl_field(&pb,&f))return false;
  if(f.number==1){
   if((f.wire!=2&&f.wire!=5)||!f.bytes.n||f.bytes.n%4||f.bytes.n/4>16u-total)return false;
   for(size_t k=0;k<f.bytes.n;k+=4){
    uint32_t bits=sl_u32(f.bytes.p+k);
    if((bits&UINT32_C(0x7fffffff))>=UINT32_C(0x7f800000))return false;
    if(total<3)first[total]=bits;total++;
   }
  }else if(f.number==2){if(status||f.wire!=0||f.word>3)return false;status=true;r.status=(uint32_t)f.word;}
 }
 if(!status||total<3)return false;
 r.fields=total;r.lux_bits=first[0];r.c_bits=first[1];r.period_bits=first[2];*out=r;
 return true;
}
static void vw_report(struct vw_state *s,const struct va_state *v,const struct sr_client *reg,const uint8_t *p,size_t n)
{
 if(!s->sent||s->invalid||s->rejected)return;
 struct sl_config current={0};
 if(reg->invalidated||!vw_mode(v,&current)||!sl_equal(current.source,s->config.source)||
    current.mode!=s->config.mode||current.rate_binary32!=s->config.rate_binary32){s->invalid=1;return;}
 struct sl_events events;
 if(!sl_indication(p,n,&events)||events.client!=reg->client_id||!sl_equal(events.source,s->config.source))return;
 for(unsigned i=0;i<events.count && s->count<VW_MAX;i++){
  if(events.event[i].id!=1025)continue;
  struct vw_sample m;
  if(!vw_decode(&events.event[i],&m)){s->bad_events++;continue;}
  if(!s->count||m.ticks>s->samples[s->count-1].ticks)s->samples[s->count++]=m;
 }
}
#endif
