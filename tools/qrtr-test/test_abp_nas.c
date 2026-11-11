/* Synthetic wire input delivered through the production MHI/QRTR entry point. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/hk_abp_nas.h>
#include <sys/hk_qrtr_wire.h>

static void nas_reply(struct hk_qrtr_mhi *, uint32_t);
struct bus {
    struct hk_qrtr_mhi *mhi;
    uint32_t late_on_close;
};

static int
transmit(void *context, const void *packet, size_t n)
{
    struct bus *bus = context;
    if (bus->late_on_close && n >= 32 &&
        hk_qrtr_wire_le32((const uint8_t *)packet + 4) == HK_QRTR_WIRE_DEL_CLIENT) {
        uint32_t local = bus->late_on_close;
        bus->late_on_close = 0;
        nas_reply(bus->mhi, local);
    }
    return 0;
}

struct counts {
    unsigned ctl, nas;
};

static void
collect(void *context, const uint8_t header[6], const uint8_t *qmi, size_t n)
{
    struct counts *counts = context;
    if (header[4] == 0) {
        assert(n >= 6 && qmi[0] == 1);
        ++counts->ctl;
    } else {
        assert(header[4] == 3 && header[5] == 1 && n == 14 && qmi[0] == 2);
        assert(qmi[1] == 42 && qmi[3] == 0x24 && qmi[7] == 2 && qmi[12] == 52);
        ++counts->nas;
    }
}

static void
nas_reply(struct hk_qrtr_mhi *m, uint32_t local)
{
    const uint8_t payload[] = {2,42,0,0x24,0,7,0, 2,4,0,1,0,52,0};
    uint32_t packet[] = {1,HK_QRTR_WIRE_DATA,3,58,0,sizeof(payload),1,local,0,0,0,0};
    memcpy(packet + 8, payload, sizeof(payload));
    assert(hk_qrtr_mhi_input(m, packet, sizeof(packet)) == 0);
}

static void
announce(struct hk_qrtr_mhi *mhi, uint32_t event, uint32_t service, uint32_t instance, uint32_t port)
{
    const uint32_t control = HK_QRTR_WIRE_PORT_CONTROL;
    uint32_t packet[] = { 1,event,3,control,0,20,1,control, event,service,instance,3,port };
    assert(hk_qrtr_mhi_input(mhi, packet, sizeof(packet)) == 0);
}

int main(void)
{
    struct hk_qrtr_mhi mhi;
    struct bus bus = {.mhi = &mhi};
    struct hk_abp_nas backend = {0};
    const uint8_t alloc[] = {1,15,0,0,0,0,0,7,0x22,0,4,0,1,1,0,3};
    const uint8_t serving[] = {1,12,0,0,3,1,0,42,0,0x24,0,0,0};
    const uint32_t add = HK_QRTR_WIRE_NEW_SERVER, del = HK_QRTR_WIRE_DEL_SERVER;
    uint64_t a, b, spare, old;
    struct counts first = {0}, second = {0};
    hk_qrtr_mhi_init(&mhi, 1, transmit, &bus);
    hk_qrtr_mhi_set_active(&mhi);
    assert(hk_abp_nas_attach(&backend, &mhi) == ENOTCONN);
    hk_qrtr_mhi_set_up(&mhi, true);
    assert(hk_abp_nas_attach(&backend, &mhi) == 0);
    assert(hk_abp_nas_attach(&backend, &mhi) == EBUSY);
    announce(&mhi, add, 1, 1, 45);         /* WDS, wrong service */
    announce(&mhi, add, 3, 2, 46);         /* NAS, wrong version */
    announce(&mhi, add, 3, 0x101, 47);     /* NAS, wrong instance */
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &a) == EAGAIN);
    announce(&mhi, add, 3, 1, HK_QRTR_WIRE_PORT_CONTROL);
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &a) == EAGAIN);
    announce(&mhi, del, 3, 1, HK_QRTR_WIRE_PORT_CONTROL);
    announce(&mhi, add, 3, 1, 58);
    const char *invalid[] = {"sahara", "pbl", "QMUX1", "qmux3", "qmux1x", "qmux"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); ++i)
        assert(hk_abp_nas_open(&backend, invalid[i], strlen(invalid[i]), collect, &first, &a) == ENOTSUP);
    assert(hk_abp_nas_open(&backend, "qmux1\0junk", 10, collect, &first, &a) == ENOTSUP);
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &a) == 0);
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &spare) == EBUSY);
    assert(hk_abp_nas_open(&backend, "qmux2", 5, collect, &second, &b) == 0 && a != b);
    assert(hk_abp_nas_write(&backend, a, alloc, sizeof(alloc)) == 0);
    assert(hk_abp_nas_write(&backend, b, alloc, sizeof(alloc)) == 0);
    assert(first.ctl == 1 && second.ctl == 1);
    uint32_t p1 = hk_qmux_nas_local_port(&backend.channels[0].router, 1);
    uint32_t p2 = hk_qmux_nas_local_port(&backend.channels[1].router, 1);
    assert(p1 && p2 && p1 != p2);
    hk_qrtr_mhi_fresh(&mhi);
    assert(hk_abp_nas_write(&backend, a, serving, sizeof(serving)) == 0);
    assert(hk_abp_nas_write(&backend, b, serving, sizeof(serving)) == 0);
    nas_reply(&mhi, p2);
    assert(!first.nas && second.nas == 1);
    announce(&mhi, add, 3, 1, 58); /* exact re-announcements preserve a lease */
    assert(hk_abp_nas_write(&backend, a, alloc, sizeof(alloc)) == 0 && first.ctl == 2);
    announce(&mhi, add, 3, 1, 59);
    assert(hk_abp_nas_write(&backend, a, alloc, sizeof(alloc)) == ENOTCONN);
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &spare) == EAGAIN);
    announce(&mhi, del, 3, 1, 59); /* ready again, same peer, new lease required */
    nas_reply(&mhi, p1);
    assert(!first.nas && second.nas == 1);
    assert(hk_abp_nas_write(&backend, b, alloc, sizeof(alloc)) == ENOTCONN);
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &spare) == 0 && a != spare);
    assert(hk_abp_nas_close(&backend, a) == ENOENT);
    assert(hk_abp_nas_close(&backend, b) == 0);
    old = spare;
    hk_qrtr_mhi_set_up(&mhi, false);
    assert(hk_abp_nas_write(&backend, old, alloc, sizeof(alloc)) == ENOTCONN);
    hk_qrtr_mhi_set_up(&mhi, true);
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &spare) == EAGAIN);
    announce(&mhi, add, 3, 1, 58);
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &spare) == 0 && spare != old);
    hk_abp_nas_detach(&backend);
    hk_abp_nas_detach(&backend);
    assert(hk_abp_nas_attach(&backend, &mhi) == 0);  /* existing service snapshot */
    assert(hk_abp_nas_open(&backend, "qmux2", 5, collect, &second, &b) == 0 && b != spare);
    assert(hk_abp_nas_close(&backend, spare) == ENOENT);
    assert(hk_abp_nas_open(&backend, "qmux1", 5, collect, &first, &a) == 0);
    assert(hk_abp_nas_write(&backend, a, alloc, sizeof(alloc)) == 0);
    assert(hk_abp_nas_write(&backend, b, alloc, sizeof(alloc)) == 0);
    hk_qrtr_mhi_fresh(&mhi);
    assert(hk_abp_nas_write(&backend, b, serving, sizeof(serving)) == 0);
    bus.late_on_close = hk_qmux_nas_local_port(&backend.channels[1].router, 1);
    assert(bus.late_on_close);
    hk_abp_nas_detach(&backend);
    assert(!bus.late_on_close && second.nas == 1);
    for (unsigned i = 0; i < HK_QRTR_CORE_MAX_PORTS; ++i)
        assert(mhi.core.ports[i].number == 0);
    puts("PASS native NAS backend exact names, unique peer, two channels, revoke, rebind and detach");
    return 0;
}
