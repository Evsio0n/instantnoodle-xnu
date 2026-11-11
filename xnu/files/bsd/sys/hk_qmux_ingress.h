#ifndef _SYS_HK_QMUX_INGRESS_H_
#define _SYS_HK_QMUX_INGRESS_H_
#include <sys/hk_qrtr_core.h>
/* Call push/pop/close under the same short mutex. No transport call, packet
 * decode, or client callback may occur under that mutex. The owner uses a
 * separate buffer for pop; a synchronous MHI send may reenter push. */
#define HK_QI_SLOTS 64u
#define HK_QI_MAX (HK_QRTR_CORE_MAX_PAYLOAD+32u)
struct hk_qmux_ingress {
    unsigned head,tail,accepted,retired,overflow,rejected,late;
    bool closed;
    uint16_t sizes[HK_QI_SLOTS];
    uint8_t data[HK_QI_SLOTS][HK_QI_MAX];
};
static inline bool hk_qi_push(struct hk_qmux_ingress *q,const void *data,size_t len) {
    if(q->closed){q->late++;return false;}
    if(!data || !len || len>HK_QI_MAX){q->rejected++;return false;}
    if(q->head-q->tail==HK_QI_SLOTS){q->overflow++;return false;}
    unsigned slot=q->head++%HK_QI_SLOTS;
    __builtin_memcpy(q->data[slot],data,len);
    q->sizes[slot]=(uint16_t)len;q->accepted++;return true;
}
static inline size_t hk_qi_pop(struct hk_qmux_ingress *q,uint8_t dst[HK_QI_MAX]) {
    if(q->head==q->tail)return 0;
    unsigned slot=q->tail++%HK_QI_SLOTS;size_t len=q->sizes[slot];
    __builtin_memcpy(dst,q->data[slot],len);q->retired++;return len;
}
static inline void hk_qi_close(struct hk_qmux_ingress *q) {q->closed=true;}
#endif
