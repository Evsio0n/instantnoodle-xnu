/* Transport-independent QRTR core; calls are serialized by its adapter. */
#include <sys/hk_qrtr_core.h>
#include <sys/hk_qrtr_wire.h>

#ifdef KERNEL
#include <sys/errno.h>
#include <libkern/libkern.h>
#else
#include <errno.h>
#include <string.h>
#endif

static void
core_zero(void *p, size_t n)
{
#ifdef KERNEL
	bzero(p, n);
#else
	memset(p, 0, n);
#endif
}

static void
core_copy(void *dst, const void *src, size_t n)
{
#ifdef KERNEL
	bcopy(src, dst, n);
#else
	memcpy(dst, src, n);
#endif
}

static void
put32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* The candidate issues UIM Change Provisioning Session as its only
 * request with message ID 0x38. This touches CPU-owned temporary storage
 * only. The modem owns its DMA copy until completion or transport teardown. */
static void
uim_wipe_temp(void *memory, size_t length)
{
	volatile uint8_t *bytes = memory;
	while (length--)
		*bytes++ = 0;
}

static int
core_emit(struct hk_qrtr_core *c, uint32_t type, uint32_t from_port,
	uint32_t to_node, uint32_t to_port, const void *payload, size_t length)
{
	uint8_t packet[32 + HK_QRTR_CORE_MAX_PAYLOAD + 3];
	size_t padded;

	if (c == NULL || c->emit == NULL || payload == NULL || length == 0 ||
	    length > HK_QRTR_CORE_MAX_PAYLOAD) {
		return EINVAL;
	}
	padded = (length + 3u) & ~(size_t)3u;
	core_zero(packet, 32 + padded);
	put32(packet, 1);
	put32(packet + 4, type);
	put32(packet + 8, c->local_node);
	put32(packet + 12, from_port);
	put32(packet + 16, 0);
	put32(packet + 20, (uint32_t)length);
	put32(packet + 24, to_node);
	put32(packet + 28, to_port);
	core_copy(packet + 32, payload, length);
	int error = c->emit(c->emit_context, packet, 32 + padded);
	/* The MHI send callback has finished copying this stack payload.
	 * The QMI request byte and little-endian message ID are in its header. */
	if (type == HK_QRTR_WIRE_DATA && length >= 7 &&
	    packet[32] == 0 && packet[35] == 0x38 && packet[36] == 0)
		uim_wipe_temp(packet + 32, padded);
	if (error == 0) {
		c->packets_out++;
	}
	return error;
}

static int
core_emit_control(struct hk_qrtr_core *c, uint32_t command, uint32_t from_port,
	uint32_t to_node, uint32_t to_port, const uint32_t *words)
{
	uint32_t payload[5] = { command, 0, 0, 0, 0 };
	for (unsigned i = 0; i < 4; i++) {
		if (words != NULL) {
			payload[i + 1] = words[i];
		}
	}
	return core_emit(c, command, from_port, to_node,
	    to_port, payload, sizeof(payload));
}

static bool
server_matches(const struct hk_qrtr_core_server *s, uint32_t service,
	uint32_t instance)
{
	return s->there && s->service == service &&
	    (instance == 0 || s->instance == instance);
}

static void
tell(struct hk_qrtr_core *c, uint32_t service, uint32_t instance,
	uint32_t node, uint32_t port, bool arrived)
{
	for (unsigned p = 0; p < HK_QRTR_CORE_MAX_PORTS; p++) {
		struct hk_qrtr_core_port *q = &c->ports[p];
		if (q->number == 0 || q->server == NULL) {
			continue;
		}
		for (unsigned l = 0; l < HK_QRTR_CORE_MAX_LOOKUPS; l++) {
			if (!q->lookups[l].asked || q->lookups[l].fresh ||
			    q->lookups[l].service != service ||
			    (q->lookups[l].instance != 0 && q->lookups[l].instance != instance)) {
				continue;
			}
			q->server(q->context, arrived, service, instance & 0xff,
			    instance >> 8, node, port);
		}
	}
}

static void
drop_server(struct hk_qrtr_core *c, unsigned i)
{
	struct hk_qrtr_core_server *s = &c->servers[i];
	uint32_t service = s->service, instance = s->instance;
	uint32_t node = s->node, port = s->port;
	s->there = false;
	tell(c, service, instance, node, port, false);
}

void
hk_qrtr_core_init(struct hk_qrtr_core *c, uint32_t local_node,
	hk_qrtr_core_emit_t emit, void *emit_context)
{
	core_zero(c, sizeof(*c));
	c->emit = emit;
	c->emit_context = emit_context;
	c->local_node = local_node != 0 ? local_node : HK_QRTR_CORE_NODE_US;
	c->next_port = HK_QRTR_CORE_FIRST_PORT;
}

void
hk_qrtr_core_reset(struct hk_qrtr_core *c)
{
	if (c == NULL) {
		return;
	}
	core_zero(c->ports, sizeof(c->ports));
	core_zero(c->servers, sizeof(c->servers));
	c->peer_node = 0;
	c->next_port = HK_QRTR_CORE_FIRST_PORT;
	c->hello_sent = false;
	c->packets_in = c->packets_out = 0;
}

void
hk_qrtr_core_link_down(struct hk_qrtr_core *c)
{
	if (c == NULL) {
		return;
	}
	/* QMI keeps the handle and its local port across an MHI reset.  Tell it
	 * about every service it had observed before dropping the peer table. */
	for (unsigned i = 0; i < HK_QRTR_CORE_MAX_SERVERS; i++) {
		if (c->servers[i].there) {
			drop_server(c, i);
		}
	}
	core_zero(c->servers, sizeof(c->servers));
	c->peer_node = 0;
	c->hello_sent = false;
	for (unsigned p = 0; p < HK_QRTR_CORE_MAX_PORTS; p++) {
		struct hk_qrtr_core_port *q = &c->ports[p];
		if (q->number == 0) {
			continue;
		}
		for (unsigned l = 0; l < HK_QRTR_CORE_MAX_LOOKUPS; l++) {
			if (q->lookups[l].asked) {
				q->lookups[l].fresh = true;
			}
		}
	}
}

struct hk_qrtr_core_port *
hk_qrtr_core_open(struct hk_qrtr_core *c, hk_qrtr_receive_t receive,
	hk_qrtr_server_t server, void *context)
{
	if (c == NULL) {
		return NULL;
	}
	for (unsigned i = 0; i < HK_QRTR_CORE_MAX_PORTS; i++) {
		struct hk_qrtr_core_port *p = &c->ports[i];
		if (p->number == 0) {
			core_zero(p, sizeof(*p));
			p->number = c->next_port++;
			if (c->next_port == 0) {
				c->next_port = HK_QRTR_CORE_FIRST_PORT;
			}
			p->receive = receive;
			p->server = server;
			p->context = context;
			return p;
		}
	}
	return NULL;
}

int
hk_qrtr_core_send(struct hk_qrtr_core *c, struct hk_qrtr_core_port *p,
	uint32_t node, uint32_t to_port, const void *data, size_t length)
{
	if (c == NULL || p == NULL || p->number == 0 || data == NULL ||
	    length == 0 || length > HK_QRTR_CORE_MAX_PAYLOAD) {
		return EINVAL;
	}
	return core_emit(c, HK_QRTR_WIRE_DATA, p->number, node, to_port, data, length);
}

int
hk_qrtr_core_lookup(struct hk_qrtr_core *c, struct hk_qrtr_core_port *p,
	uint32_t service, uint32_t version, uint32_t instance)
{
	if (c == NULL || p == NULL || p->number == 0) {
		return EINVAL;
	}
	for (unsigned i = 0; i < HK_QRTR_CORE_MAX_LOOKUPS; i++) {
		if (!p->lookups[i].asked) {
			p->lookups[i].service = service;
			p->lookups[i].instance = instance != 0 ? (version & 0xffu) | (instance << 8) : 0;
			p->lookups[i].asked = true;
			p->lookups[i].fresh = true;
			return 0;
		}
	}
	return ENOSPC;
}

void
hk_qrtr_core_close(struct hk_qrtr_core *c, struct hk_qrtr_core_port *p)
{
	uint32_t words[4];
	uint32_t number;
	if (c == NULL || p == NULL || p->number == 0) {
		return;
	}
	number = p->number;
	words[0] = c->local_node;
	words[1] = number;
	words[2] = words[3] = 0;
	p->number = 0;
	if (c->peer_node != 0) {
		(void)core_emit_control(c, HK_QRTR_WIRE_DEL_CLIENT, number,
		    c->peer_node, HK_QRTR_WIRE_PORT_CONTROL, words);
	}
}

void
hk_qrtr_core_fresh(struct hk_qrtr_core *c)
{
	if (c == NULL) {
		return;
	}
	for (unsigned p = 0; p < HK_QRTR_CORE_MAX_PORTS; p++) {
		struct hk_qrtr_core_port *q = &c->ports[p];
		if (q->number == 0 || q->server == NULL) {
			continue;
		}
		for (unsigned l = 0; l < HK_QRTR_CORE_MAX_LOOKUPS; l++) {
			if (!q->lookups[l].asked || !q->lookups[l].fresh) {
				continue;
			}
			q->lookups[l].fresh = false;
			for (unsigned i = 0; i < HK_QRTR_CORE_MAX_SERVERS; i++) {
				struct hk_qrtr_core_server *s = &c->servers[i];
				if (server_matches(s, q->lookups[l].service, q->lookups[l].instance)) {
					q->server(q->context, true, s->service, s->instance & 0xff,
					    s->instance >> 8, s->node, s->port);
				}
			}
		}
	}
}

static void
handle_control(struct hk_qrtr_core *c, const struct hk_qrtr_wire_datagram *d,
	const struct hk_qrtr_wire_control *ctl)
{
	if (ctl->command == HK_QRTR_WIRE_HELLO) {
		c->peer_node = d->from_node;
		if (!c->hello_sent) {
			if (core_emit_control(c, HK_QRTR_WIRE_HELLO, HK_QRTR_WIRE_PORT_CONTROL,
			    d->from_node, HK_QRTR_WIRE_PORT_CONTROL, NULL) == 0) {
				c->hello_sent = true;
			}
		}
		return;
	}
	if (ctl->command == HK_QRTR_WIRE_NEW_SERVER) {
		int free_slot = -1;
		for (unsigned i = 0; i < HK_QRTR_CORE_MAX_SERVERS; i++) {
			struct hk_qrtr_core_server *s = &c->servers[i];
			if (s->there && s->node == ctl->words[2] && s->port == ctl->words[3]) {
				/* Re-announcements are idempotent. */
				if (s->service == ctl->words[0] && s->instance == ctl->words[1]) {
					return;
				}
				drop_server(c, i);
			}
			if (!s->there && free_slot < 0) {
				free_slot = (int)i;
			}
		}
		if (free_slot >= 0) {
			struct hk_qrtr_core_server *s = &c->servers[free_slot];
			s->there = true;
			s->service = ctl->words[0];
			s->instance = ctl->words[1];
			s->node = ctl->words[2];
			s->port = ctl->words[3];
			tell(c, s->service, s->instance, s->node, s->port, true);
		}
		return;
	}
	if (ctl->command == HK_QRTR_WIRE_DEL_SERVER) {
		for (unsigned i = 0; i < HK_QRTR_CORE_MAX_SERVERS; i++) {
			struct hk_qrtr_core_server *s = &c->servers[i];
			if (s->there && s->node == ctl->words[2] && s->port == ctl->words[3]) {
				drop_server(c, i);
			}
		}
		return;
	}
	if (ctl->command == HK_QRTR_WIRE_BYE) {
		for (unsigned i = 0; i < HK_QRTR_CORE_MAX_SERVERS; i++) {
			if (c->servers[i].there && c->servers[i].node == d->from_node) {
				drop_server(c, i);
			}
		}
	}
}

int
hk_qrtr_core_input(struct hk_qrtr_core *c, const void *packet, size_t length)
{
	struct hk_qrtr_wire_datagram d;
	if (c == NULL || hk_qrtr_wire_decode(packet, length, HK_QRTR_CORE_MAX_PAYLOAD, &d) != 0) {
		return EINVAL;
	}
	c->packets_in++;
	if (c->peer_node == 0) {
		c->peer_node = d.from_node;
	}
	if (d.confirm) {
		uint32_t words[4] = { d.to_node, d.to_port, 0, 0 };
		(void)core_emit_control(c, HK_QRTR_WIRE_RESUME_TX, d.to_port,
		    d.from_node, d.from_port, words);
	}
	if (d.to_port == HK_QRTR_WIRE_PORT_CONTROL) {
		struct hk_qrtr_wire_control ctl;
		if (hk_qrtr_wire_control_decode(&d, &ctl) != 0) {
			return EPROTO;
		}
		handle_control(c, &d, &ctl);
		return 0;
	}
	if (d.type == HK_QRTR_WIRE_DATA) {
		for (unsigned i = 0; i < HK_QRTR_CORE_MAX_PORTS; i++) {
			struct hk_qrtr_core_port *p = &c->ports[i];
			if (p->number == d.to_port && p->receive != NULL) {
				p->receive(p->context, d.from_node, d.from_port, d.payload, d.payload_length);
				break;
			}
		}
	}
	return 0;
}

unsigned
hk_qrtr_core_live_services(const struct hk_qrtr_core *c)
{
	unsigned n = 0;
	if (c != NULL) {
		for (unsigned i = 0; i < HK_QRTR_CORE_MAX_SERVERS; i++) {
			n += c->servers[i].there;
		}
	}
	return n;
}
