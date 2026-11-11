#ifndef _SYS_HK_QMUX_OWNER_H_
#define _SYS_HK_QMUX_OWNER_H_
#include <stdbool.h>
/* Cold-boot diagnostic/driver owner barrier. The poller acknowledges STOP
 * only after its last transport callback and the owner-side detach. Only a
 * DRAINED acquire-load permits reuse. An unstarted worker can be certified
 * separately if kernel_thread_start itself failed. A timed-out quarantine
 * cannot subsequently be converted to clean by a late acknowledgement. */
enum hk_qmux_phase { HK_QO_OFF, HK_QO_ARMED, HK_QO_READY, HK_QO_STOP, HK_QO_DRAINED, HK_QO_HELD };
struct hk_qmux_owner { unsigned phase; };
static inline unsigned hk_qo_phase(const struct hk_qmux_owner *g) {
    return __atomic_load_n(&g->phase,__ATOMIC_ACQUIRE);
}
static inline bool hk_qo_move(struct hk_qmux_owner *g,unsigned from,unsigned to) {
    return __atomic_compare_exchange_n(&g->phase,&from,to,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static inline bool hk_qo_arm(struct hk_qmux_owner *g) {return hk_qo_move(g,HK_QO_OFF,HK_QO_ARMED);}
static inline bool hk_qo_ready(struct hk_qmux_owner *g) {return hk_qo_move(g,HK_QO_ARMED,HK_QO_READY);}
static inline void hk_qo_stop(struct hk_qmux_owner *g) {
    unsigned phase;
    while((phase=hk_qo_phase(g))==HK_QO_ARMED || phase==HK_QO_READY)
        if(hk_qo_move(g,phase,HK_QO_STOP))return;
}
static inline bool hk_qo_ack(struct hk_qmux_owner *g) {return hk_qo_move(g,HK_QO_STOP,HK_QO_DRAINED);}
static inline bool hk_qo_no_worker(struct hk_qmux_owner *g) {
    /* Only after the sole kernel_thread_start returned failure. */
    hk_qo_stop(g);return hk_qo_ack(g);
}
static inline unsigned hk_qo_timeout(struct hk_qmux_owner *g) {
    hk_qo_stop(g);(void)hk_qo_move(g,HK_QO_STOP,HK_QO_HELD);return hk_qo_phase(g);
}
#endif
