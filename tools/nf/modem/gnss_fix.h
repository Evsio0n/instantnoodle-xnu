#ifndef HK_GNSS_FIX_H
#define HK_GNSS_FIX_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HK_FIX_MAX_AGE_MS 15000u
enum hk_fix_state { HK_FIX_OFF, HK_FIX_WAITING, HK_FIX_READY, HK_FIX_PROGRESS, HK_FIX_MODEM_ERROR,
    HK_FIX_MISSING_FIELDS, HK_FIX_BAD_REPORT, HK_FIX_SOURCE_UNKNOWN, HK_FIX_OLD, HK_FIX_CLOCK_UNKNOWN };
struct hk_fix {
    uint64_t latitude_bits, longitude_bits, utc_ms, observed_ms;
    uint32_t horizontal_uncertainty_bits, technologies, age_at_arrival_ms;
};
struct hk_fix_session {
    uint32_t node, port, local, state, accepted;
    uint64_t last_utc_ms;
    uint8_t session;
    bool active;
    /* Explicit volatile retention bounds this one device-local record. No coordinate accessor is exported. */
    volatile struct hk_fix value;
};
static inline void hk_fix_invalidate(struct hk_fix_session *s, uint32_t state) {
    s->value=(struct hk_fix){0};
    s->state=state;
}
static inline void hk_fix_end(struct hk_fix_session *s) {
    hk_fix_invalidate(s,HK_FIX_OFF);s->active=false;
    s->node=s->port=s->local=s->accepted=0;s->session=0;s->last_utc_ms=0;
}
static inline void hk_fix_start(struct hk_fix_session *s,uint32_t node,uint32_t port,uint32_t local,uint8_t id) {
    hk_fix_end(s);
    if(!node||!port||!local)return;
    s->node=node;s->port=port;s->local=local;s->session=id;s->active=true;s->state=HK_FIX_WAITING;
}
static inline uint64_t hk_fix_le(const uint8_t *v,unsigned n) {
    uint64_t value=0;
    for(unsigned i=0;i<n;i++)value|=(uint64_t)v[i]<<(8*i);
    return value;
}
static inline bool hk_fix_matches(const struct hk_fix_session *s,uint32_t node,uint32_t port,uint32_t local) {
    return s->active && node==s->node && port==s->port && local==s->local;
}
static inline void hk_fix_expire(struct hk_fix_session *s,uint64_t now) {
    if(s->state!=HK_FIX_READY)return;
    if(now<s->value.observed_ms||now-s->value.observed_ms>HK_FIX_MAX_AGE_MS-s->value.age_at_arrival_ms)hk_fix_invalidate(s,HK_FIX_OLD);
}
/* QMI LOC 0x0024. Each TLV of interest has an exact size and may appear once.
 * Coordinates are checked against IEEE-754 magnitudes without kernel FP. */
static inline void hk_fix_observe(struct hk_fix_session *s,const uint8_t *q,size_t n,uint64_t now,uint64_t utc_now) {
    if(!s->active||!q||n<7||q[0]!=4||hk_fix_le(q+3,2)!=0x24)return;
    if(hk_fix_le(q+5,2)!=n-7){hk_fix_invalidate(s,HK_FIX_BAD_REPORT);return;}
    struct hk_fix value={0};
    enum { STATUS=1, SID=2, LAT=4, LON=8, UNC=16, TECH=32, UTC=64 };
    unsigned mask=0,id=0;uint32_t status=0;
    q+=7;n-=7;
    while(n) {
        if(n<3)goto bad;
        unsigned type=q[0],bytes=(unsigned)hk_fix_le(q+1,2),bit=0,wanted=0;q+=3;n-=3;
        if(bytes>n)goto bad;
        switch(type) {
        case 1: bit=STATUS;wanted=4;break; case 2: bit=SID;wanted=1;break;
        case 0x10:bit=LAT;wanted=8;break;case 0x11:bit=LON;wanted=8;break;
        case 0x12:bit=UNC;wanted=4;break;case 0x23:bit=TECH;wanted=4;break;
        case 0x25:bit=UTC;wanted=8;break;
        default:break;
        }
        if(bit) {
            if(bytes!=wanted||(mask&bit))goto bad;
            uint64_t x=hk_fix_le(q,bytes);mask|=bit;
            switch(bit) {
            case STATUS:status=(uint32_t)x;break;case SID:id=(unsigned)x;break;
            case LAT:value.latitude_bits=x;break;case LON:value.longitude_bits=x;break;
            case UNC:value.horizontal_uncertainty_bits=(uint32_t)x;break;
            case TECH:value.technologies=(uint32_t)x;break;case UTC:value.utc_ms=x;break;
            }
        }
        q+=bytes;n-=bytes;
    }
    if((mask&SID)&&id!=s->session)return;
    if((mask&(STATUS|SID))!=(STATUS|SID)){hk_fix_invalidate(s,HK_FIX_MISSING_FIELDS);return;}
    if(status){hk_fix_invalidate(s,status==1?HK_FIX_PROGRESS:HK_FIX_MODEM_ERROR);return;}
    if((mask&(LAT|LON|UNC|UTC))!=(LAT|LON|UNC|UTC)){hk_fix_invalidate(s,HK_FIX_MISSING_FIELDS);return;}
    if((value.latitude_bits&UINT64_C(0x7fffffffffffffff))>UINT64_C(0x4056800000000000) ||
       (value.longitude_bits&UINT64_C(0x7fffffffffffffff))>UINT64_C(0x4066800000000000) ||
       (value.horizontal_uncertainty_bits&0x7fffffffu)>=0x7f800000u ||
       (value.horizontal_uncertainty_bits>0x80000000u))goto bad;
    if(!(value.technologies&1u)||(value.technologies&0x30u)){hk_fix_invalidate(s,HK_FIX_SOURCE_UNKNOWN);return;}
    if(!utc_now||!value.utc_ms){hk_fix_invalidate(s,HK_FIX_CLOCK_UNKNOWN);return;}
    if(value.utc_ms>utc_now ? value.utc_ms-utc_now>2000 : utc_now-value.utc_ms>HK_FIX_MAX_AGE_MS) {
        hk_fix_invalidate(s,HK_FIX_OLD);return;
    }
    if(value.utc_ms<=s->last_utc_ms){hk_fix_expire(s,now);return;}
    value.observed_ms=now;value.age_at_arrival_ms=utc_now>value.utc_ms?(uint32_t)(utc_now-value.utc_ms):0;
    s->last_utc_ms=value.utc_ms;s->value=value;s->state=HK_FIX_READY;s->accepted++;
    return;
bad:
    hk_fix_invalidate(s,HK_FIX_BAD_REPORT);
}
#endif
