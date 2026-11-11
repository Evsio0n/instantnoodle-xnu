#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/hk_qrtr_core.h>
#include <sys/hk_qrtr_wire.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

struct capture { uint8_t bytes[16384]; size_t length; unsigned sends; };
struct events { unsigned arrived, gone, received; uint32_t node, port; size_t length; };

static int emit_packet(void *ctx, const void *packet, size_t length)
{
	struct capture *c = ctx;
	if (length > sizeof(c->bytes)) return 1;
	memcpy(c->bytes, packet, length); c->length = length; c->sends++; return 0;
}
static void receive(void *ctx, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct events *e = ctx; e->received++; e->node = node; e->port = port; e->length = length; CHECK(data != NULL);
}
static void server(void *ctx, bool arrived, uint32_t service, uint32_t version, uint32_t instance, uint32_t node, uint32_t port)
{
	struct events *e = ctx; (void)service; (void)version; (void)instance; e->node = node; e->port = port; if (arrived) e->arrived++; else e->gone++;
}
static size_t v1(uint8_t *p, uint32_t type, uint32_t from_node, uint32_t from_port, uint32_t confirm,
    uint32_t to_node, uint32_t to_port, const void *payload, size_t length)
{
	memset(p, 0, 32 + ((length + 3) & ~3u));
	#define P32(o, v) do { uint32_t _v = (v); p[(o)] = _v; p[(o)+1] = _v >> 8; p[(o)+2] = _v >> 16; p[(o)+3] = _v >> 24; } while (0)
	P32(0, 1); P32(4, type); P32(8, from_node); P32(12, from_port); P32(16, confirm); P32(20, length); P32(24, to_node); P32(28, to_port);
	memcpy(p + 32, payload, length); return 32 + ((length + 3) & ~3u);
}
static void ctl(uint32_t *p, uint32_t command, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ p[0] = command; p[1] = a; p[2] = b; p[3] = c; p[4] = d; }

int main(void)
{
	struct hk_qrtr_core core; struct capture cap = { 0 }; struct events e = { 0 };
	struct hk_qrtr_core_port *port; uint8_t packet[128]; uint32_t words[5]; size_t n;
	hk_qrtr_core_init(&core, 1, emit_packet, &cap);
	port = hk_qrtr_core_open(&core, receive, server, &e); CHECK(port != NULL && port->number == 0x4000);
	CHECK(hk_qrtr_core_lookup(&core, port, 16, 1, 0) == 0);
	ctl(words, HK_QRTR_WIRE_HELLO, 0, 0, 0, 0); n = v1(packet, HK_QRTR_WIRE_HELLO, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_core_input(&core, packet, n) == 0 && cap.sends == 1);
	ctl(words, HK_QRTR_WIRE_NEW_SERVER, 16, 1, 3, 84); n = v1(packet, HK_QRTR_WIRE_NEW_SERVER, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_core_input(&core, packet, n) == 0); CHECK(hk_qrtr_core_live_services(&core) == 1); CHECK(e.arrived == 0);
	hk_qrtr_core_fresh(&core); CHECK(e.arrived == 1 && e.node == 3 && e.port == 84);
	CHECK(hk_qrtr_core_input(&core, packet, n) == 0); CHECK(hk_qrtr_core_live_services(&core) == 1 && e.arrived == 1);
	ctl(words, HK_QRTR_WIRE_DEL_SERVER, 16, 1, 3, 84); n = v1(packet, HK_QRTR_WIRE_DEL_SERVER, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_core_input(&core, packet, n) == 0 && hk_qrtr_core_live_services(&core) == 0 && e.gone == 1);
	/* Confirmed data produces RESUME, then dispatches to the local port. */
	/* A second service is delivered after fresh(), and BYE removes all services from its node. */
	ctl(words, HK_QRTR_WIRE_NEW_SERVER, 16, 2, 3, 85); n = v1(packet, HK_QRTR_WIRE_NEW_SERVER, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_core_input(&core, packet, n) == 0 && hk_qrtr_core_live_services(&core) == 1);
	hk_qrtr_core_fresh(&core); CHECK(e.arrived == 2);
	ctl(words, HK_QRTR_WIRE_BYE, 0, 0, 0, 0); n = v1(packet, HK_QRTR_WIRE_BYE, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_core_input(&core, packet, n) == 0 && hk_qrtr_core_live_services(&core) == 0 && e.gone == 2);
	/* A confirmed data packet produces RESUME and is dispatched to the local port. */
	uint8_t data[] = { 9, 8, 7 }; n = v1(packet, HK_QRTR_WIRE_DATA, 3, 0x5000, 0, 1, port->number, data, sizeof(data));
	CHECK(hk_qrtr_core_input(&core, packet, n) == 0 && e.received == 1 && e.length == 3 && cap.sends == 1);
	n = v1(packet, HK_QRTR_WIRE_DATA, 3, 0x5000, 1, 1, port->number, data, sizeof(data)); packet[16] = 1;
	CHECK(hk_qrtr_core_input(&core, packet, n) == 0 && cap.sends == 2);
	struct hk_qrtr_wire_datagram emitted; CHECK(hk_qrtr_wire_decode(cap.bytes, cap.length, 8192, &emitted) == 0 && emitted.type == HK_QRTR_WIRE_RESUME_TX);
	hk_qrtr_core_close(&core, port); CHECK(port->number == 0); CHECK(cap.sends == 3);
	CHECK(hk_qrtr_wire_decode(cap.bytes, cap.length, 8192, &emitted) == 0 &&
	    emitted.type == HK_QRTR_WIRE_DEL_CLIENT && emitted.from_port == 0x4000);
	printf("qrtr-core-test: %u checks, %u failures\n", checks, failures); return failures != 0;
}
