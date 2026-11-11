#ifndef _SYS_HK_QMUX_NAS_H_
#define _SYS_HK_QMUX_NAS_H_

#include <sys/hk_qrtr.h>

/* One QMUX channel, operated only on the serial modem/IPCR worker. A QMI CTL
 * client is backed by an actual QRTR port. Only NAS Get Serving System and
 * an explicit Serving System Events boolean subscription are admitted. A future ABI-matched IOUserClient can copy
 * both pieces into its own bounded async delivery queue. emit() must consume
 * the bytes before returning and must not reenter or destroy this channel. */
#define HK_QMUX_NAS_CLIENTS 4
struct hk_qmux_nas;
struct hk_qrtr_mhi;
typedef void (*hk_qmux_emit_t)(void *context, const uint8_t qmux[6],
    const uint8_t *qmi, size_t qmi_size);

struct hk_qmux_nas_slot {
    struct hk_qmux_nas *channel;
    struct hk_qrtr_port *port;
    uint32_t node, server;
    uint16_t transaction, message;
    uint8_t cid;
    bool ready, lost, pending, enable_attempted, enabled, disable_attempted;
};
struct hk_qmux_nas {
    const struct hk_qrtr_ops *ops;
    struct hk_qrtr_mhi *owner;
    hk_qmux_emit_t emit;
    void *context;
    struct hk_qmux_nas_slot slots[HK_QMUX_NAS_CLIENTS];
};

/* QMI responses may add other optional TLVs in any order. Return the
 * unique four-byte Operation Result only after the entire frame validates. */
static inline const uint8_t *
hk_qmux_nas_result_tlv(const uint8_t *p, size_t size)
{
    const uint8_t *result = NULL;
    while (size) {
        if (size < 3) return NULL;
        size_t len = (size_t)p[1] | ((size_t)p[2] << 8);
        if (len > size - 3) return NULL;
        if (p[0] == 2) {
            if (result || len != 4) return NULL;
            result = p + 3;
        }
        p += 3 + len;
        size -= 3 + len;
    }
    return result;
}

/* Zero-init a channel before its first open. Close it before reusing storage.
 * All error returns use errno. Unsupported or stale requests emit no success
 * frame. Opening alone neither allocates ports nor sends a modem request. */
int hk_qrtr_mhi_nas_open(struct hk_qmux_nas *, hk_qmux_emit_t, void *);
int hk_qmux_nas_write(struct hk_qmux_nas *, const uint8_t *frame, size_t length);
/* Positive only for an allocated local client. Returns its actual QRTR port,
 * useful for attributing independently routed replies. */
uint32_t hk_qmux_nas_local_port(const struct hk_qmux_nas *, uint8_t cid);
void hk_qmux_nas_close(struct hk_qmux_nas *);

#endif
