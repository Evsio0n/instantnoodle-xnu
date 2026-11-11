/* QRTR transport adapter for an MHI/IPCR channel. */
#include <sys/hk_qrtr_mhi.h>

#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

static int
mhi_emit(void *context, const void *packet, size_t length)
{
	struct hk_qrtr_mhi *mhi = context;

	if (mhi == NULL || !mhi->up || mhi->send == NULL) {
		return ENOTCONN;
	}
	return mhi->send(mhi->send_context, packet, length);
}

static struct hk_qrtr_mhi *active_mhi;

void
hk_qrtr_mhi_init(struct hk_qrtr_mhi *mhi, uint32_t local_node,
	 hk_qrtr_mhi_send_t send, void *send_context)
{
	if (mhi == NULL) {
		return;
	}
	hk_qrtr_core_init(&mhi->core, local_node, mhi_emit, mhi);
	mhi->send = send;
	mhi->send_context = send_context;
	mhi->up = false;
	mhi->dropped = 0;
	for (unsigned i = 0; i < HK_QRTR_CORE_MAX_PORTS; i++) {
		mhi->ports[i].core_port = NULL;
		mhi->ports[i].owner = mhi;
	}
}

void
hk_qrtr_mhi_set_up(struct hk_qrtr_mhi *mhi, bool up)
{
	if (mhi == NULL) {
		return;
	}
	if (!up && mhi->up) {
		hk_qrtr_core_link_down(&mhi->core);
	}
	mhi->up = up;
}

void
hk_qrtr_mhi_set_active(struct hk_qrtr_mhi *mhi)
{
	active_mhi = mhi;
}

int
hk_qrtr_mhi_input(struct hk_qrtr_mhi *mhi, const void *packet, size_t length)
{
	if (mhi == NULL || !mhi->up) {
		return ENOTCONN;
	}
	if (hk_qrtr_core_input(&mhi->core, packet, length) != 0) {
		mhi->dropped++;
		return EPROTO;
	}
	/* QMI lookup callbacks are asynchronous from the transport's point of
	 * view.  Deliver the initial live-service snapshot after each datagram. */
	hk_qrtr_core_fresh(&mhi->core);
	return 0;
}

void
hk_qrtr_mhi_fresh(struct hk_qrtr_mhi *mhi)
{
	if (mhi != NULL && mhi->up) {
		hk_qrtr_core_fresh(&mhi->core);
	}
}

static struct hk_qrtr_port *
mhi_open(hk_qrtr_receive_t receive, hk_qrtr_server_t server, void *context)
{
	struct hk_qrtr_mhi_port *slot;
	struct hk_qrtr_core_port *core_port;

	if (active_mhi == NULL || !active_mhi->up) {
		return NULL;
	}
	core_port = hk_qrtr_core_open(&active_mhi->core, receive, server, context);
	if (core_port == NULL) {
		return NULL;
	}
	for (unsigned i = 0; i < HK_QRTR_CORE_MAX_PORTS; i++) {
		slot = &active_mhi->ports[i];
		if (slot->core_port == NULL) {
			slot->core_port = core_port;
			return (struct hk_qrtr_port *)(void *)slot;
		}
	}
	hk_qrtr_core_close(&active_mhi->core, core_port);
	return NULL;
}

static int
mhi_send(struct hk_qrtr_port *port, uint32_t node, uint32_t to_port,
	const void *data, size_t length)
{
	struct hk_qrtr_mhi_port *p = (struct hk_qrtr_mhi_port *)(void *)port;
	return p == NULL || p->owner == NULL || p->core_port == NULL ? ENOTCONN :
	    hk_qrtr_core_send(&p->owner->core, p->core_port, node, to_port, data, length);
}

static int
mhi_lookup(struct hk_qrtr_port *port, uint32_t service, uint32_t version,
	uint32_t instance)
{
	struct hk_qrtr_mhi_port *p = (struct hk_qrtr_mhi_port *)(void *)port;
	return p == NULL || p->owner == NULL || p->core_port == NULL ? ENOTCONN :
	    hk_qrtr_core_lookup(&p->owner->core, p->core_port, service, version, instance);
}

static void
mhi_close(struct hk_qrtr_port *port)
{
	struct hk_qrtr_mhi_port *p = (struct hk_qrtr_mhi_port *)(void *)port;
	if (p != NULL && p->owner != NULL) {
		if (p->core_port != NULL) {
			hk_qrtr_core_close(&p->owner->core, p->core_port);
			p->core_port = NULL;
		}
	}
}

static const struct hk_qrtr_ops mhi_ops = {
	.open = mhi_open,
	.send = mhi_send,
	.lookup = mhi_lookup,
	.close = mhi_close,
};

const struct hk_qrtr_ops *
hk_qrtr_mhi_ops(void)
{
	return &mhi_ops;
}

#include "hk_qmux_nas.inc"
