/* Internal binding between a named QMUX channel and the real mission QRTR
 * NAS endpoint. Register no Apple class until the kernel supports its full
 * client ABI. The owner may call emit only while consuming a router frame. */
#include <sys/hk_abp_nas.h>
#include <sys/hk_qrtr_wire.h>

#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

static void
nas_changed(void *context, bool arrived, uint32_t service, uint32_t version,
    uint32_t instance, uint32_t node, uint32_t port)
{
    struct hk_abp_nas *b = context;
    (void)arrived;
    (void)node;
    (void)port;
    if (service != 3 || version != 1 || instance != 0) return;

    /* QRTR has updated its catalogue before calling our lookup. Ambiguity
     * must revoke the old route even if one of the two endpoints stays. */
    const struct hk_qrtr_core_server *only = NULL;
    for (unsigned i = 0; i < HK_QRTR_CORE_MAX_SERVERS; ++i) {
        const struct hk_qrtr_core_server *s = &b->owner->core.servers[i];
        if (!s->there || s->service != 3 || s->instance != 1) continue;
        if (only) {
            only = NULL;
            break;
        }
        only = s;
    }
    if (only && (!only->node || only->node == UINT32_MAX ||
        only->node == b->owner->core.local_node || !only->port ||
        hk_qrtr_wire_port(only->port) == HK_QRTR_WIRE_PORT_CONTROL))
        only = NULL;
    uint32_t next_node = only ? only->node : 0;
    uint32_t next_port = only ? only->port : 0;
    if (next_node == b->node && next_port == b->port) return;
    b->node = next_node;
    b->port = next_port;
    for (unsigned i = 0; i < 2; ++i)
        if (b->channels[i].lease) b->channels[i].revoked = true;
}

static void
forward_current(void *context, const uint8_t qmux[6], const uint8_t *qmi, size_t n)
{
    struct hk_abp_nas_channel *ch = context;
    if (!ch->revoked && ch->backend->owner->up)
        ch->emit(ch->context, qmux, qmi, n);
}

static void
close_channel(struct hk_abp_nas_channel *ch)
{
    ch->revoked = true;
    hk_qmux_nas_close(&ch->router);
    *ch = (struct hk_abp_nas_channel){0};
}

int
hk_abp_nas_attach(struct hk_abp_nas *b, struct hk_qrtr_mhi *owner)
{
    if (!b || !owner) return EINVAL;
    if (b->owner) return EBUSY;
    if (!owner->up) return ENOTCONN;
    struct hk_qrtr_core_port *watch = hk_qrtr_core_open(&owner->core, NULL, nas_changed, b);
    if (!watch) return ENOSPC;
    int error = hk_qrtr_core_lookup(&owner->core, watch, 3, 1, 0);
    if (error) {
        hk_qrtr_core_close(&owner->core, watch);
        return error;
    }
    b->owner = owner;
    b->watch = watch;
    hk_qrtr_mhi_fresh(owner);
    return 0;
}

void
hk_abp_nas_detach(struct hk_abp_nas *b)
{
    if (!b || !b->owner) return;
    /* Closing a QRTR port sends DEL_CLIENT. Under TX back-pressure MHI
     * can poll a pending RX completion on this same thread before returning. */
    for (unsigned i = 0; i < 2; ++i) b->channels[i].revoked = true;
    for (unsigned i = 0; i < 2; ++i) close_channel(&b->channels[i]);
    hk_qrtr_core_close(&b->owner->core, b->watch);
    *b = (struct hk_abp_nas){ .serial = b->serial };
}

int
hk_abp_nas_open(struct hk_abp_nas *b, const char *protocol, size_t n,
    hk_qmux_emit_t emit, void *context, uint64_t *lease)
{
    if (!b || !protocol || !emit || !lease) return EINVAL;
    *lease = 0;
    if (n != 5 || protocol[0] != 'q' || protocol[1] != 'm' ||
        protocol[2] != 'u' || protocol[3] != 'x' ||
        (protocol[4] != '1' && protocol[4] != '2')) return ENOTSUP;
    if (!b->owner || !b->owner->up) return ENOTCONN;
    if (!b->node || !b->port) return EAGAIN;
    struct hk_abp_nas_channel *ch = &b->channels[protocol[4] - '1'];
    if (ch->lease && !ch->revoked) return EBUSY;
    if (b->serial == UINT64_MAX) return EOVERFLOW;
    if (ch->revoked) {
        close_channel(ch);
        if (!b->owner->up) return ENOTCONN;
        if (!b->node || !b->port) return EAGAIN;
    }
    int error = hk_qrtr_mhi_nas_open(&ch->router, forward_current, ch);
    if (error) return error;
    if (ch->router.owner != b->owner) {
        close_channel(ch);
        return ENOTCONN;
    }
    ch->backend = b;
    ch->emit = emit;
    ch->context = context;
    *lease = ch->lease = ++b->serial;
    return 0;
}

static struct hk_abp_nas_channel *
find_channel(struct hk_abp_nas *b, uint64_t lease)
{
    if (b && lease)
        for (unsigned i = 0; i < 2; ++i)
            if (b->channels[i].lease == lease) return &b->channels[i];
    return NULL;
}

int
hk_abp_nas_write(struct hk_abp_nas *b, uint64_t lease, const uint8_t *frame, size_t n)
{
    struct hk_abp_nas_channel *ch = find_channel(b, lease);
    if (!ch || ch->revoked || !b->owner->up) return ENOTCONN;
    return hk_qmux_nas_write(&ch->router, frame, n);
}

int
hk_abp_nas_close(struct hk_abp_nas *b, uint64_t lease)
{
    struct hk_abp_nas_channel *ch = find_channel(b, lease);
    if (!ch) return ENOENT;
    close_channel(ch);
    return 0;
}
