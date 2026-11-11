/*
 * Transport-independent QRTR name service and port core.
 *
 * The caller serializes calls to one core instance.  A GLINK or MHI adapter
 * only supplies emit(), which receives a complete v1 QRTR datagram.
 */
#ifndef _SYS_HK_QRTR_CORE_H_
#define _SYS_HK_QRTR_CORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/hk_qrtr.h>

#define HK_QRTR_CORE_MAX_PAYLOAD 8192u
#define HK_QRTR_CORE_MAX_SERVERS 64u
#define HK_QRTR_CORE_MAX_PORTS 16u
#define HK_QRTR_CORE_MAX_LOOKUPS 4u
#define HK_QRTR_CORE_FIRST_PORT 0x4000u
#define HK_QRTR_CORE_NODE_US 1u

typedef int (*hk_qrtr_core_emit_t)(void *context, const void *packet, size_t length);

struct hk_qrtr_core_port {
	uint32_t number;
	hk_qrtr_receive_t receive;
	hk_qrtr_server_t server;
	void *context;
	struct {
		uint32_t service;
		uint32_t instance;
		bool asked;
		bool fresh;
	} lookups[HK_QRTR_CORE_MAX_LOOKUPS];
};

struct hk_qrtr_core_server {
	uint32_t service;
	uint32_t instance;
	uint32_t node;
	uint32_t port;
	bool there;
};

struct hk_qrtr_core {
	hk_qrtr_core_emit_t emit;
	void *emit_context;
	uint32_t local_node;
	uint32_t peer_node;
	uint32_t next_port;
	bool hello_sent;
	uint32_t packets_in;
	uint32_t packets_out;
	struct hk_qrtr_core_port ports[HK_QRTR_CORE_MAX_PORTS];
	struct hk_qrtr_core_server servers[HK_QRTR_CORE_MAX_SERVERS];
};

void hk_qrtr_core_init(struct hk_qrtr_core *, uint32_t local_node,
	 hk_qrtr_core_emit_t, void *emit_context);
void hk_qrtr_core_reset(struct hk_qrtr_core *);
/* The transport went away; keep local ports/lookups so an existing client can
 * rediscover its services after the link comes back. */
void hk_qrtr_core_link_down(struct hk_qrtr_core *);

struct hk_qrtr_core_port *hk_qrtr_core_open(struct hk_qrtr_core *,
	hk_qrtr_receive_t, hk_qrtr_server_t, void *context);
int hk_qrtr_core_send(struct hk_qrtr_core *, struct hk_qrtr_core_port *,
	uint32_t node, uint32_t to_port, const void *, size_t length);
int hk_qrtr_core_lookup(struct hk_qrtr_core *, struct hk_qrtr_core_port *,
	uint32_t service, uint32_t version, uint32_t instance);
void hk_qrtr_core_close(struct hk_qrtr_core *, struct hk_qrtr_core_port *);
void hk_qrtr_core_fresh(struct hk_qrtr_core *);
int hk_qrtr_core_input(struct hk_qrtr_core *, const void *, size_t length);
unsigned hk_qrtr_core_live_services(const struct hk_qrtr_core *);

#endif /* _SYS_HK_QRTR_CORE_H_ */
