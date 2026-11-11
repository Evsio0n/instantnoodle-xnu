/* Host-only MHI/IPCR adapter test.  The fake MHI channel only captures bytes. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/hk_qrtr_mhi.h>
#include <sys/hk_qrtr_wire.h>

static unsigned checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

struct capture { uint8_t data[256]; size_t length; unsigned sends; };
struct events { unsigned arrived, gone, received; };

static int send_packet(void *context, const void *packet, size_t length)
{
	struct capture *c = context;
	if (length > sizeof(c->data)) return 1;
	memcpy(c->data, packet, length); c->length = length; c->sends++; return 0;
}

static void server(void *context, bool arrived, uint32_t service, uint32_t version,
    uint32_t instance, uint32_t node, uint32_t port)
{
	struct events *e = context;
	(void)service; (void)version; (void)instance; (void)node; (void)port;
	if (arrived) e->arrived++; else e->gone++;
}

static void receive(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct events *e = context;
	(void)node; (void)port; (void)data; (void)length; e->received++;
}

static size_t v1(uint8_t *p, uint32_t type, uint32_t from_node, uint32_t from_port,
    uint32_t confirm, uint32_t to_node, uint32_t to_port, const void *payload, size_t length)
{
	uint32_t words[8] = { 1, type, from_node, from_port, confirm, length, to_node, to_port };
	size_t padded = (length + 3) & ~3u;
	memcpy(p, words, sizeof(words)); memset(p + sizeof(words), 0, padded); memcpy(p + sizeof(words), payload, length);
	return sizeof(words) + padded;
}

static void ctl(uint32_t *p, uint32_t command, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{ p[0] = command; p[1] = a; p[2] = b; p[3] = c; p[4] = d; }

int main(void)
{
	struct hk_qrtr_mhi mhi; struct capture cap = { 0 }; struct events e = { 0 };
	struct hk_qrtr_port *port; uint8_t packet[128];
	uint32_t words[5]; size_t n;
	const struct hk_qrtr_ops *ops = hk_qrtr_mhi_ops();

	hk_qrtr_mhi_init(&mhi, 1, send_packet, &cap);
	hk_qrtr_mhi_set_active(&mhi);
	CHECK(ops->open(receive, server, &e) == NULL);
	hk_qrtr_mhi_set_up(&mhi, true);
	/* The modem may announce LOC before the generic QMI handle opens. */
	ctl(words, HK_QRTR_WIRE_HELLO, 0, 0, 0, 0);
	n = v1(packet, HK_QRTR_WIRE_HELLO, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_mhi_input(&mhi, packet, n) == 0 && cap.sends == 1);
	ctl(words, HK_QRTR_WIRE_NEW_SERVER, 16, 1, 3, 84);
	n = v1(packet, HK_QRTR_WIRE_NEW_SERVER, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_mhi_input(&mhi, packet, n) == 0);
	port = ops->open(receive, server, &e);
	CHECK(port != NULL);
	CHECK(ops->lookup(port, 16, 1, 0) == 0);
	hk_qrtr_mhi_fresh(&mhi);
	CHECK(e.arrived == 1);
	CHECK(ops->send(port, 3, 84, "x", 1) == 0 && cap.sends == 2);
	n = v1(packet, HK_QRTR_WIRE_DATA, 3, 84, 0, 1, 0x4000, "ok", 2);
	CHECK(hk_qrtr_mhi_input(&mhi, packet, n) == 0 && e.received == 1);
	packet[0] = 0;
	CHECK(hk_qrtr_mhi_input(&mhi, packet, n) != 0 && mhi.dropped == 1);
	hk_qrtr_mhi_set_up(&mhi, false);
	CHECK(e.gone == 1);
	CHECK(hk_qrtr_mhi_input(&mhi, packet, n) != 0);
	CHECK(ops->send(port, 3, 84, "x", 1) != 0);
	/* The same QMI handle/port survives a link reset and rediscovers LOC. */
	hk_qrtr_mhi_set_up(&mhi, true);
	ctl(words, HK_QRTR_WIRE_HELLO, 0, 0, 0, 0);
	n = v1(packet, HK_QRTR_WIRE_HELLO, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_mhi_input(&mhi, packet, n) == 0);
	ctl(words, HK_QRTR_WIRE_NEW_SERVER, 16, 1, 3, 84);
	n = v1(packet, HK_QRTR_WIRE_NEW_SERVER, 3, HK_QRTR_WIRE_PORT_CONTROL, 0, 1, HK_QRTR_WIRE_PORT_CONTROL, words, sizeof(words));
	CHECK(hk_qrtr_mhi_input(&mhi, packet, n) == 0 && e.arrived == 2);
	printf("qrtr-mhi-test: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
