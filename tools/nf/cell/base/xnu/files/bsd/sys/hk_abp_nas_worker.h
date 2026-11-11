#ifndef _SYS_HK_ABP_NAS_WORKER_H_
#define _SYS_HK_ABP_NAS_WORKER_H_
#include <sys/hk_abp_nas.h>
#include <sys/hk_qmux_ingress.h>
#include <sys/hk_qmux_owner.h>

/* Backend for the existing serial MHI poll worker. Long-lived, zeroed storage
 * must not be on a stack. lock/unlock protect just a copy into or out of the
 * ingress queue; is_owner must check the caller's actual execution context.
 * Do not select this path while another generic QMI client directly drives
 * the same QRTR core. Disconnect all sources before freeing this structure. */
struct hk_abp_nas_worker_ops {
    void (*lock)(void *);
    void (*unlock)(void *);
    bool (*is_owner)(void *);
};
struct hk_abp_rx_stamp {
    uint32_t epoch[4];
    uint64_t harvest_ms, generation;
};
struct hk_abp_nas_worker {
    struct hk_qmux_owner gate;
    struct hk_qmux_ingress inbox;
    struct hk_abp_rx_stamp inbox_stamp[HK_QI_SLOTS], dispatch_stamp;

    struct hk_abp_nas backend;
    struct hk_qrtr_mhi *mhi;
    const struct hk_abp_nas_worker_ops *ops;
    void *context;
    uint8_t packet[HK_QI_MAX];
    unsigned admission, discarded;
    unsigned v2_decoded, v2_options, v2_max_options;
    uint64_t generation;
    bool online, attached, busy;
};

/* Setup before starting the physical worker. READY follows a real mission
 * transition. RX may arrive earlier from synchronous MHI waits and later
 * reentrantly from send. It is copied only, never dispatched here. */
int hk_abp_worker_arm(struct hk_abp_nas_worker *,struct hk_qrtr_mhi *,
    const struct hk_abp_nas_worker_ops *,void *);
/* Capture generation once when registering the producer callback. Do not look
 * it up again for each packet. Rearm is exclusive and only legal after the
 * previous board callbacks have been disconnected/joined, its owner stopped
 * and its drain acknowledged. Outstanding leases never survive rearm. */
int hk_abp_worker_rearm_after_disconnect(struct hk_abp_nas_worker *,struct hk_qrtr_mhi *,
    const struct hk_abp_nas_worker_ops *,void *);
uint64_t hk_abp_worker_generation(const struct hk_abp_nas_worker *);
int hk_abp_worker_ready(struct hk_abp_nas_worker *);
int hk_abp_worker_rx(struct hk_abp_nas_worker *,uint64_t generation,const void *,size_t);
int hk_abp_worker_rx_tagged(struct hk_abp_nas_worker *,uint64_t generation,const void *,size_t,
    const struct hk_abp_rx_stamp *);


/* On the poll thread, after returning from the outermost MHI dispatch.
 * EAGAIN means not ready; ECANCELED requests final drain. Poll never issues
 * a NAS service request on its own. Client operations are explicit and may
 * be scheduled only on this same owner, outside their completion callback. */
int hk_abp_worker_poll(struct hk_abp_nas_worker *);
struct hk_qrtr_wire_datagram;
/* Optional name-service consumer on the same owner, after QRTR has accepted
 * the control datagram. The datagram is borrowed for this callback only. */
int hk_abp_worker_poll_observed(struct hk_abp_nas_worker *,
    void (*control)(const struct hk_qrtr_wire_datagram *));
int hk_abp_worker_open(struct hk_abp_nas_worker *,const char *,size_t,
    hk_qmux_emit_t,void *,uint64_t *);
int hk_abp_worker_write(struct hk_abp_nas_worker *,uint64_t,const uint8_t *,size_t);
int hk_abp_worker_close(struct hk_abp_nas_worker *,uint64_t);

/* The lifecycle coordinator requests STOP, the poller finishes exactly once
 * after its last callback, and the coordinator checks the acquire-load ACK.
 * Finish returns EAGAIN if an admitted producer has not dropped its reference,
 * including producers waiting to take the inbox mutex. Retry on the owner.
 * On expiry, quarantine is sticky. In that case keep all storage and DMA
 * mappings reserved; the board owner must take its hardware recovery path. */
void hk_abp_worker_stop(struct hk_abp_nas_worker *);
int hk_abp_worker_finish(struct hk_abp_nas_worker *);
bool hk_abp_worker_drained(const struct hk_abp_nas_worker *);
bool hk_abp_worker_timeout(struct hk_abp_nas_worker *);

/* Only if the single attempted creation of the poll thread failed. Closes
 * the inbox without publishing an open channel. */
bool hk_abp_worker_never_started(struct hk_abp_nas_worker *);
#endif
