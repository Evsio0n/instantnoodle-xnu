/* Read-only attributes from two actual vendor-board discoveries. No CONFIG
 * encoding, sensor subscription or panel update exists in this diagnostic. */
#ifndef HK_VENDOR_ATTRIBUTES_H
#define HK_VENDOR_ATTRIBUTES_H
#include "hk_registry_read.h"
enum { VA_COUNT=2, VA_TXN=0x4400 };
static const char *const va_names[VA_COUNT]={"wise_light","proximity"};
struct va_state {
    struct sl_uid uid[VA_COUNT];unsigned seen,count[VA_COUNT],sent,ack,failed,received,invalid;
    uint64_t lookup_ticks[VA_COUNT],attr_ticks[VA_COUNT];
    struct sl_attrs attrs[VA_COUNT];
};
static void va_report(struct va_state *v,const struct sr_client *reg,const uint8_t *packet,size_t n)
{
    struct sl_events m;
    if(reg->invalidated||!sl_nonzero(reg->registry)||!sl_indication(packet,n,&m)||m.client!=reg->client_id)return;
    for(unsigned j=0;j<m.count;j++)for(unsigned i=0;i<VA_COUNT;i++){
        struct sl_lookup found;unsigned bit=1u<<i;
        const struct sl_event *e=&m.event[j];
        if(sl_lookup_named_event(m.source,e,va_names[i],&found)){
            if((v->seen&bit)&&e->ticks<=v->lookup_ticks[i])continue;
            v->seen|=bit;v->count[i]=found.count;v->lookup_ticks[i]=e->ticks;
            struct sl_uid id=found.count==1?found.uids[0]:(struct sl_uid){0};
            if((v->sent&bit)&&!sl_equal(id,v->uid[i]))v->invalid|=bit;
            if(!(v->invalid&bit))v->uid[i]=id;
            continue;
        }
        if((v->sent&bit)&&!(v->invalid&bit)&&sl_equal(v->uid[i],m.source)&&e->id==128 &&
           (!(v->received&bit)||e->ticks>v->attr_ticks[i])){
            struct sl_attrs attrs;
            if(sl_attributes(v->uid[i],m.source,e,&attrs)){
                v->attrs[i]=attrs;v->received|=bit;v->attr_ticks[i]=e->ticks;
            }
        }
    }
}
static bool va_terminal(const struct va_state *v)
{
    const unsigned all=(1u<<VA_COUNT)-1;
    return ((v->received&v->ack)|v->failed|v->invalid)==all;
}
static size_t va_next(const struct va_state *v,const struct sr_client *reg,uint16_t *txn,uint8_t *out,size_t cap)
{
    if(reg->invalidated||!sl_nonzero(reg->registry))return 0;
    for(unsigned i=0;i<VA_COUNT;i++)if(sl_nonzero(v->uid[i])&&!((v->invalid|v->sent)&(1u<<i))){
        /* An identical SUID under both names is ambiguous and cannot be
         * attributed to either source independently. */
        if(sl_equal(v->uid[i],v->uid[1-i]))return 0;
        *txn=VA_TXN+i;return sl_attribute_request(out,cap,*txn,v->uid[i]);
    }
    return 0;
}
static bool va_ack(struct va_state *v,const struct sr_client *reg,const uint8_t *p,size_t n)
{
    if(reg->invalidated||!sl_nonzero(reg->registry)||n<7)return false;
    unsigned i=(unsigned)sl_u16(p+1)-VA_TXN;if(i>=VA_COUNT)return false;
    unsigned bit=1u<<i;if(!(v->sent&bit)||((v->ack|v->failed|v->invalid)&bit))return false;
    int result=sr_ack_result(p,n,reg->client_id);if(!result)return false;
    if(result>0)v->ack|=bit;else v->failed|=bit;
    return true;
}
#endif
