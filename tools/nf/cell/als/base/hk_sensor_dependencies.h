/* Late, read-only SUID inventory. Only the opt-in FastRPC registry diagnostic
 * starts it, after its four registry reads have terminated on a live endpoint. */
#ifndef HK_SENSOR_DEPENDENCIES_H
#define HK_SENSOR_DEPENDENCIES_H
#include "hk_registry_read.h"

enum { SD_COUNT=3, SD_TXN_BASE=0x4200 };
static const char *const sd_names[SD_COUNT]={"interrupt","resampler","wise_light"};
struct sd_state {
    uint64_t started_us,ticks[SD_COUNT];
    unsigned sent,ack_ok,ack_fail,observed,available;
    unsigned reports[SD_COUNT],count[SD_COUNT];
};
static inline size_t sd_request(unsigned group,uint8_t *out,size_t cap)
{
    const struct sl_uid lookup={UINT64_C(0xabababababababab),UINT64_C(0xabababababababab)};
    if(group>=SD_COUNT)return 0;
    return sr_named_request(lookup,sd_names[group],(uint16_t)(SD_TXN_BASE+group),true,out,cap);
}
static inline bool sd_ack(struct sd_state *d,const struct sr_client *reg,const uint8_t *p,size_t n)
{
    if(!d->started_us || reg->invalidated || !sl_nonzero(reg->registry) || n<7)return false;
    unsigned group=(unsigned)sl_u16(p+1)-SD_TXN_BASE;
    if(group>=SD_COUNT)return false;
    unsigned bit=1u<<group;
    if(!(d->sent&bit)||((d->ack_ok|d->ack_fail)&bit))return false;
    int result=sr_ack_result(p,n,reg->client_id);
    if(!result)return false;
    if(result>0)d->ack_ok|=bit;else d->ack_fail|=bit;
    return true;
}
/* Caller binds the QRTR peer. Indication sequence is independent of QMI txn.
 * Empty and missing reports remain distinct; keep the latest dated report. */
static inline void sd_report(struct sd_state *d,const struct sr_client *reg,const uint8_t *p,size_t n)
{
    struct sl_events events;
    if(!d->started_us||reg->invalidated||!sl_nonzero(reg->registry)||!sl_indication(p,n,&events)||
       events.client!=reg->client_id)return;
    for(unsigned i=0;i<events.count;i++){
        const struct sl_event *e=&events.event[i];
        for(unsigned j=0;j<SD_COUNT;j++){
            unsigned bit=1u<<j;
            struct sl_lookup r;
            if(!(d->sent&bit)||!sl_lookup_named_event(events.source,e,sd_names[j],&r)||
               ((d->observed&bit)&&e->ticks<=d->ticks[j]))continue;
            d->observed|=bit;d->ticks[j]=e->ticks;d->count[j]=r.count;d->reports[j]++;
            if(r.count)d->available|=bit;else d->available&=~bit;
        }
    }
}
static inline bool sd_terminal(const struct sd_state *d)
{
    const unsigned all=(1u<<SD_COUNT)-1;
    return d->sent==all && ((d->ack_ok&d->observed)|d->ack_fail)==all;
}
static inline bool sd_window_done(const struct sd_state *d,uint64_t now_us)
{
    if(!d->started_us||now_us<d->started_us)return false;
    uint64_t elapsed=now_us-d->started_us;
    return elapsed>=UINT64_C(8000000) || (elapsed>=UINT64_C(3000000) && sd_terminal(d));
}
#endif
