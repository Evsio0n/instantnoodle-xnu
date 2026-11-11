/* Fixed-size read-only kernel -> iOS diagnostic ABI. Times are monotonic ms
 * since boot. The producer serializes updates; query completion uses a token
 * invalidated on endpoint loss or subsystem shutdown. */
#ifndef MDM_RADIO_STATUS_H
#define MDM_RADIO_STATUS_H
#include <stdint.h>
#define HK_RADIO_VERSION 1u
enum hk_radio_phase { HK_RADIO_OFF, HK_RADIO_STARTING, HK_RADIO_ACTIVE,
    HK_RADIO_INIT_FAILURE, HK_RADIO_STOPPED };
enum hk_radio_state { HK_RADIO_ABSENT, HK_RADIO_ENDPOINT, HK_RADIO_QUERYING,
    HK_RADIO_VALUE, HK_RADIO_TRANSPORT_FAILURE, HK_RADIO_QMI_FAILURE, HK_RADIO_NO_FIELD };
enum hk_radio_kind { HK_RADIO_MODE, HK_RADIO_SIGNAL, HK_RADIO_PACKET, HK_RADIO_COUNT };
/* For MODE, detail carries only the two optional Get Operating Mode TLVs.
 * Zero remains compatible with kernels that did not retain either field. */
#define HK_RADIO_REASON_PRESENT (1u << 16)
#define HK_RADIO_RESTRICTION_PRESENT (1u << 17)
#define HK_RADIO_RESTRICTED (1u << 18)
static inline int32_t hk_radio_mode_detail(int has_reason, uint16_t reason,
    int has_restriction, int restriction)
{
    unsigned packed = has_reason ? HK_RADIO_REASON_PRESENT | reason : 0;
    if (has_restriction)
        packed |= HK_RADIO_RESTRICTION_PRESENT | (restriction ? HK_RADIO_RESTRICTED : 0);
    return (int32_t)packed;
}
struct hk_radio_field {
    uint64_t observed_ms;
    uint32_t generation, state;
    int32_t value, detail, error;
    uint32_t reserved;
};
struct hk_radio_snapshot {
    uint32_t version, size, phase, reserved;
    uint64_t read_ms;
    struct hk_radio_field fields[HK_RADIO_COUNT];
};
_Static_assert(sizeof(struct hk_radio_field)==32, "radio field ABI");
_Static_assert(sizeof(struct hk_radio_snapshot)==120, "radio status ABI");

static inline void hk_radio_invalidate(struct hk_radio_snapshot *s, unsigned kind, unsigned state)
{
    if (kind>=HK_RADIO_COUNT) return;
    struct hk_radio_field *f=&s->fields[kind];
    uint32_t next=f->generation+1u;
    *f=(struct hk_radio_field){.generation=next?next:1u,.state=state};
}
static inline void hk_radio_reset(struct hk_radio_snapshot *s,unsigned phase)
{
    for (unsigned i=0;i<HK_RADIO_COUNT;i++) hk_radio_invalidate(s,i,HK_RADIO_ABSENT);
    s->phase=phase;
}
static inline uint32_t hk_radio_begin(struct hk_radio_snapshot *s,unsigned kind)
{
    if(s->phase!=HK_RADIO_ACTIVE || kind>=HK_RADIO_COUNT ||
       s->fields[kind].state!=HK_RADIO_ENDPOINT)return 0;
    s->fields[kind].state=HK_RADIO_QUERYING;
    return s->fields[kind].generation;
}
static inline void hk_radio_finish(struct hk_radio_snapshot *s,unsigned kind,uint32_t token,
    int bytes, unsigned modem_result, unsigned modem_error,int valid,int value,int detail,uint64_t ms)
{
    if(kind>=HK_RADIO_COUNT || !token || s->phase!=HK_RADIO_ACTIVE)return;
    struct hk_radio_field *f=&s->fields[kind];
    if(f->generation!=token || f->state!=HK_RADIO_QUERYING)return;
    f->observed_ms=ms;
    if(bytes<=0){ f->state=HK_RADIO_TRANSPORT_FAILURE;f->error=bytes; }
    else if(modem_result){ f->state=HK_RADIO_QMI_FAILURE;f->error=(int32_t)modem_error; }
    else if(!valid){ f->state=HK_RADIO_NO_FIELD; }
    else { f->state=HK_RADIO_VALUE;f->value=value;f->detail=detail; }
}
#endif
