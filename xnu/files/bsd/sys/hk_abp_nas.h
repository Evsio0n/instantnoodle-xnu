#ifndef _SYS_HK_ABP_NAS_H_
#define _SYS_HK_ABP_NAS_H_

#include <sys/hk_qmux_nas.h>
#include <sys/hk_qrtr_mhi.h>

/* Internal NAS transport only. All entry points and QRTR receive run on the
 * same serialized modem owner. Attach after mission, detach before replacing
 * or reinitializing the adapter. No IOKit service or user ABI is published. */
struct hk_abp_nas;
struct hk_abp_nas_channel {
    struct hk_qmux_nas router;
    struct hk_abp_nas *backend;
    hk_qmux_emit_t emit;
    void *context;
    uint64_t lease;
    bool revoked;
};
struct hk_abp_nas {
    struct hk_qrtr_mhi *owner;
    struct hk_qrtr_core_port *watch;
    uint64_t serial;
    uint32_t node, port;
    struct hk_abp_nas_channel channels[2];
};

/* Start with zeroed storage, retain it until detach, preserve it across restarts.
 * The opaque nonzero lease names one open during this backend's lifetime.
 * emit consumes each frame before returning. It may not call this API, block
 * waiting on the modem, or retain the borrowed bytes. Close/detach occur on
 * the owner outside receive callbacks; worker teardown must wait for detach
 * before clearing the adapter or unmapping MHI. Open returns EAGAIN until
 * there is one exact NAS v1/instance-0 endpoint. Revoked I/O returns
 * ENOTCONN. Closing a lease twice or after its replacement returns ENOENT. */
int hk_abp_nas_attach(struct hk_abp_nas *, struct hk_qrtr_mhi *);
void hk_abp_nas_detach(struct hk_abp_nas *);
int hk_abp_nas_open(struct hk_abp_nas *, const char *protocol, size_t name_length,
    hk_qmux_emit_t, void *context, uint64_t *lease);
int hk_abp_nas_write(struct hk_abp_nas *, uint64_t lease, const uint8_t *, size_t);
int hk_abp_nas_close(struct hk_abp_nas *, uint64_t lease);

#endif
