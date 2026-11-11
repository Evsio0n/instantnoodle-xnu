/*
 * QRTR over an MHI/IPCR channel.
 *
 * The MHI owner supplies one send callback and calls input() for complete
 * inbound datagrams.  The adapter owns the transport-independent QRTR core;
 * callers use ops() as the hk_qmi transport for this modem link.
 */
#ifndef _SYS_HK_QRTR_MHI_H_
#define _SYS_HK_QRTR_MHI_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/hk_qrtr.h>
#include <sys/hk_qrtr_core.h>

typedef int (*hk_qrtr_mhi_send_t)(void *context, const void *packet, size_t length);

struct hk_qrtr_mhi;
struct hk_qrtr_mhi_port {
	struct hk_qrtr_core_port *core_port;
	struct hk_qrtr_mhi *owner;
};

struct hk_qrtr_mhi {
	struct hk_qrtr_core core;
	hk_qrtr_mhi_send_t send;
	void *send_context;
	bool up;
	uint32_t dropped;
	struct hk_qrtr_mhi_port ports[HK_QRTR_CORE_MAX_PORTS];
};

void hk_qrtr_mhi_init(struct hk_qrtr_mhi *, uint32_t local_node,
	 hk_qrtr_mhi_send_t, void *send_context);
void hk_qrtr_mhi_set_up(struct hk_qrtr_mhi *, bool up);
void hk_qrtr_mhi_set_active(struct hk_qrtr_mhi *);
int hk_qrtr_mhi_input(struct hk_qrtr_mhi *, const void *packet, size_t length);
/* Deliver the current service snapshot to lookups made after discovery. */
void hk_qrtr_mhi_fresh(struct hk_qrtr_mhi *);
const struct hk_qrtr_ops *hk_qrtr_mhi_ops(void);

#endif /* _SYS_HK_QRTR_MHI_H_ */
