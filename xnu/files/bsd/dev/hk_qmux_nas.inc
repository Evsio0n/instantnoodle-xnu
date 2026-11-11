/* Included by hk_qrtr_mhi.c so the bridge shares the running transport and
 * its existing callbacks. No device node, Apple service, or radio status is
 * synthesized by this adapter. The NAS response is the modem's QMI frame. */
#include <sys/hk_qmux_nas.h>

#define HK_QMUX_NAS_SERVICE 3
#define HK_QMUX_ALLOCATE_CID 0x22
#define HK_QMUX_RELEASE_CID 0x23
#define HK_QMUX_GET_SERVING_SYSTEM 0x24
#define HK_QMUX_REGISTER_INDICATIONS 0x03

static uint16_t
qmux_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void
qmux_emit(struct hk_qmux_nas *q, uint8_t service, uint8_t cid,
    const uint8_t *qmi, size_t n)
{
    uint16_t wire_length = (uint16_t)(n + 5);
    uint8_t header[6] = { 1, (uint8_t)wire_length, (uint8_t)(wire_length >> 8), 0x80, service, cid };
    q->emit(q->context, header, qmi, n);
}

static void
qmux_server(void *context, bool arrived, uint32_t service, uint32_t version,
    uint32_t instance, uint32_t node, uint32_t port)
{
    struct hk_qmux_nas_slot *s = context;
    if (service != HK_QMUX_NAS_SERVICE || version != 1 || instance != 0 || s->lost) return;
    if (arrived && !s->ready) {
        s->node = node;
        s->server = port;
        s->ready = true;
        return;
    }
    bool same = s->ready && s->node == node && s->server == port;
    if ((arrived && !same) || (!arrived && same)) {
        /* A new peer must not satisfy a transaction issued to an old peer.
         * Release/reallocate uses a new local QRTR port even if the CID,
         * remote endpoint and QMI transaction number are later reused. */
        s->lost = true;
        s->ready = false;
        s->pending = false;
    }
}

static bool
qmux_serving_indication(const uint8_t *p, size_t size)
{
    bool serving = false;
    while (size) {
        if (size < 3) return false;
        size_t len = qmux_u16(p + 1);
        if (len > size - 3) return false;
        if (p[0] == 1) {
            if (serving || len < 5 || (size_t)p[7] != len - 5) return false;
            serving = true;
        }
        if (p[0] == 2) return false;
        size -= 3 + len;
        p += 3 + len;
    }
    return serving;
}

static void
qmux_receive(void *context, uint32_t node, uint32_t port, const void *data, size_t size)
{
    struct hk_qmux_nas_slot *s = context;
    const uint8_t *p = data;
    if (!s->ready || s->lost || s->node != node || s->server != port) return;
    if (!p || size < 7 || size > HK_QRTR_CORE_MAX_PAYLOAD ||
        qmux_u16(p + 5) != size - 7) return;
    uint16_t message = qmux_u16(p + 3);
    bool indication = p[0] == 4;
    if (indication && message != HK_QMUX_GET_SERVING_SYSTEM) return;
    if (!indication && (p[0] != 2 || !s->pending || message != s->message ||
        qmux_u16(p + 1) != s->transaction)) return;
    if (indication) {
        if (!qmux_serving_indication(p + 7, size - 7)) return;
    } else {
        const uint8_t *result = hk_qmux_nas_result_tlv(p + 7, size - 7);
        if (!result) return;
        s->pending = false;
        if (message == HK_QMUX_REGISTER_INDICATIONS)
            s->enabled = !s->disable_attempted && !qmux_u16(result) && !qmux_u16(result + 2);
    }
    qmux_emit(s->channel, HK_QMUX_NAS_SERVICE, s->cid, p, size);
}

static void
qmux_drop(struct hk_qmux_nas_slot *s)
{
    if (s->port) s->channel->ops->close(s->port);
    *s = (struct hk_qmux_nas_slot){0};
}

int
hk_qrtr_mhi_nas_open(struct hk_qmux_nas *q, hk_qmux_emit_t emit, void *context)
{
    if (!q || !emit) return EINVAL;
    if (q->ops) return EBUSY;
    if (!active_mhi || !active_mhi->up) return ENOTCONN;
    *q = (struct hk_qmux_nas){ .ops=hk_qrtr_mhi_ops(), .owner=active_mhi, .emit=emit, .context=context };
    return 0;
}

static int
qmux_allocate(struct hk_qmux_nas *q, uint8_t *cid)
{
    if (!active_mhi || active_mhi != q->owner || !active_mhi->up) return ENOTCONN;
    for (unsigned i = 0; i < HK_QMUX_NAS_CLIENTS; i++) {
        struct hk_qmux_nas_slot *s = &q->slots[i];
        if (s->port) continue;
        s->channel = q; s->cid = (uint8_t)(i + 1);
        s->port = q->ops->open(qmux_receive, qmux_server, s);
        if (!s->port) { *s = (struct hk_qmux_nas_slot){0}; return ENOSPC; }
        int err = q->ops->lookup(s->port, HK_QMUX_NAS_SERVICE, 1, 0);
        if (err) { qmux_drop(s); return err; }
        *cid = s->cid;
        return 0;
    }
    return ENOSPC;
}

static int
qmux_ctl(struct hk_qmux_nas *q, const uint8_t *p, size_t n)
{
    if (n < 6 || p[0] != 0 || qmux_u16(p + 4) != n - 6) return EPROTO;
    uint16_t message = qmux_u16(p + 2);
    if (message != HK_QMUX_ALLOCATE_CID && message != HK_QMUX_RELEASE_CID) return ENOTSUP;
    bool allocate = message == HK_QMUX_ALLOCATE_CID;
    size_t expected = allocate ? 1 : 2;
    if (n != 9 + expected || p[6] != 1 || qmux_u16(p + 7) != expected) return EPROTO;
    if (p[9] != HK_QMUX_NAS_SERVICE) return ENOTSUP;
    uint8_t cid;
    if (allocate) {
        int error = qmux_allocate(q, &cid);
        if (error) return error;
    } else {
        cid = p[10];
        if (!cid || cid > HK_QMUX_NAS_CLIENTS || !q->slots[cid - 1].port) return ENOENT;
        qmux_drop(&q->slots[cid - 1]);
    }
    /* CTL allocations are local transport operations, unlike the NAS service
     * reply, which always comes through qmux_receive from the discovered peer. */
    uint8_t reply[] = { 1, p[1], (uint8_t)message, 0, 12, 0, 2, 4, 0, 0, 0, 0, 0,
                        1, 2, 0, HK_QMUX_NAS_SERVICE, cid };
    qmux_emit(q, 0, 0, reply, sizeof(reply));
    return 0;
}

int
hk_qmux_nas_write(struct hk_qmux_nas *q, const uint8_t *frame, size_t n)
{
    if (!q || !q->ops || !frame || n < 6 || n > 6 + HK_QRTR_CORE_MAX_PAYLOAD) return EINVAL;
    if (frame[0] != 1 || qmux_u16(frame + 1) != n - 1 || frame[3] != 0) return EPROTO;
    if (frame[4] == 0 && frame[5] == 0) return qmux_ctl(q, frame + 6, n - 6);
    if (frame[4] != HK_QMUX_NAS_SERVICE) return ENOTSUP;
    uint8_t cid = frame[5];
    if (!cid || cid > HK_QMUX_NAS_CLIENTS || !q->slots[cid - 1].port) return ENOENT;
    const uint8_t *p = frame + 6;
    if (n < 13 || p[0] != 0 || qmux_u16(p + 5) != n - 13) return EPROTO;
    uint16_t message = qmux_u16(p + 3);
    bool subscribe = message == HK_QMUX_REGISTER_INDICATIONS;
    if (subscribe) {
        if (n != 17 || p[7] != 0x13 || qmux_u16(p + 8) != 1 || p[10] > 1) return EPROTO;
    } else {
        if (message != HK_QMUX_GET_SERVING_SYSTEM) return ENOTSUP;
        if (n != 13) return EPROTO;
    }
    struct hk_qmux_nas_slot *s = &q->slots[cid - 1];
    if (s->lost || active_mhi != q->owner || !q->owner->up) return ENOTCONN;
    if (!s->ready) return EAGAIN;
    if (s->pending) return EBUSY;
    if (subscribe) {
        if (p[10]) {
            if (s->enable_attempted) return EALREADY;
            s->enable_attempted = true;
        } else {
            if (!s->enabled || s->disable_attempted) return EALREADY;
            s->disable_attempted = true;
        }
    }
    s->message = message;
    s->transaction = qmux_u16(p + 1);
    s->pending = true;
    int error = q->ops->send(s->port, s->node, s->server, p, n - 6);
    if (error) s->pending = false;
    return error;
}

uint32_t
hk_qmux_nas_local_port(const struct hk_qmux_nas *q, uint8_t cid)
{
    if (!q || !q->ops || !cid || cid > HK_QMUX_NAS_CLIENTS) return 0;
    const struct hk_qrtr_mhi_port *p = (const void *)q->slots[cid - 1].port;
    return p && p->owner == q->owner && p->core_port ? p->core_port->number : 0;
}

void
hk_qmux_nas_close(struct hk_qmux_nas *q)
{
    if (!q || !q->ops) return;
    for (unsigned i = 0; i < HK_QMUX_NAS_CLIENTS; i++) qmux_drop(&q->slots[i]);
    *q = (struct hk_qmux_nas){0};
}
