/* Host-only tests for sys/hk_qrtr_wire.h. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <sys/hk_qrtr_wire.h>

static unsigned checks, failures;

#define CHECK(condition) do { \
	checks++; \
	if (!(condition)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
	} \
} while (0)

static void
put16(uint8_t *p, uint16_t value)
{
	p[0] = (uint8_t)value;
	p[1] = (uint8_t)(value >> 8);
}

static void
put32(uint8_t *p, uint32_t value)
{
	for (unsigned i = 0; i < 4; i++) {
		p[i] = (uint8_t)(value >> (8 * i));
	}
}

static size_t
v2(uint8_t *packet, uint8_t option_bytes, uint32_t type, uint16_t from_port,
    uint16_t to_port, uint32_t declared_size, const uint8_t *payload,
    size_t payload_size)
{
	size_t header = 16u + (size_t)option_bytes;
	memset(packet, 0, header + ((payload_size + 3) & ~3u));
	packet[0] = 3;
	packet[1] = (uint8_t)type;
	packet[3] = option_bytes;
	put32(packet + 4, declared_size);
	put16(packet + 8, 3);
	put16(packet + 10, from_port);
	put16(packet + 12, 1);
	put16(packet + 14, to_port);
	memcpy(packet + header, payload, payload_size);
	return header + ((payload_size + 3) & ~3u);
}

static void
test_v2(void)
{
	uint8_t payload[20], packet[64];
	struct hk_qrtr_wire_datagram d;
	struct hk_qrtr_wire_control c;

	memset(payload, 0, sizeof(payload));
	put32(payload, HK_QRTR_WIRE_NEW_SERVER);
	put32(payload + 4, 16);
	put32(payload + 8, 1);
	put32(payload + 12, 3);
	put32(payload + 16, 84);

	size_t length = v2(packet, 0, HK_QRTR_WIRE_NEW_SERVER, 0xfffe, 0xffff,
	    sizeof(payload), payload, sizeof(payload));
	CHECK(hk_qrtr_wire_decode(packet, length, 8192, &d) == 0);
	CHECK(d.header_length == 16 && d.payload_length == 20);
	CHECK(d.from_port == HK_QRTR_WIRE_PORT_CONTROL);
	CHECK(d.to_port == HK_QRTR_WIRE_PORT_CONTROL);
	CHECK(hk_qrtr_wire_control_decode(&d, &c) == 0 && c.command == HK_QRTR_WIRE_NEW_SERVER);
	CHECK(c.words[0] == 16 && c.words[2] == 3 && c.words[3] == 84);

	length = v2(packet, 4, HK_QRTR_WIRE_NEW_SERVER, 0x4000, 0x4001,
	    sizeof(payload), payload, sizeof(payload));
	CHECK(hk_qrtr_wire_decode(packet, length, 8192, &d) == 0);
	CHECK(d.header_length == 20 && d.payload == packet + 20);
	CHECK(hk_qrtr_wire_control_decode(&d, &c) == 0 && c.words[3] == 84);

	length = v2(packet, 0, HK_QRTR_WIRE_NEW_SERVER, 0x4000, 0x4001,
	    64, payload, sizeof(payload));
	CHECK(hk_qrtr_wire_decode(packet, length, 8192, &d) != 0);
	length = v2(packet, 0, HK_QRTR_WIRE_NEW_SERVER, 0x4000, 0x4001,
	    4, payload, sizeof(payload));
	CHECK(hk_qrtr_wire_decode(packet, length, 8192, &d) != 0);
}

static void
test_v1(void)
{
	uint8_t packet[64] = { 0 }, payload[4] = { 2, 0, 0, 0 };
	struct hk_qrtr_wire_datagram d;

	put32(packet, 1);
	put32(packet + 4, HK_QRTR_WIRE_HELLO);
	put32(packet + 8, 9);
	put32(packet + 12, 0xfffffffeu);
	put32(packet + 16, 1);
	put32(packet + 20, sizeof(payload));
	put32(packet + 24, 1);
	put32(packet + 28, 0xfffffffeu);
	memcpy(packet + 32, payload, sizeof(payload));
	CHECK(hk_qrtr_wire_decode(packet, 36, 8192, &d) == 0);
	CHECK(d.version == 1 && d.type == HK_QRTR_WIRE_HELLO && d.confirm);
	CHECK(d.from_node == 9 && d.from_port == HK_QRTR_WIRE_PORT_CONTROL);
	CHECK(d.to_port == HK_QRTR_WIRE_PORT_CONTROL);
	CHECK(d.payload_length == 4 && d.payload[0] == 2);
	put32(packet, 0x00010001u);
	CHECK(hk_qrtr_wire_decode(packet, 36, 8192, &d) != 0);
	put32(packet, HK_QRTR_WIRE_VERSION_1);
	CHECK(hk_qrtr_wire_decode(packet, 37, 8192, &d) != 0);
}

int
main(void)
{
	test_v2();
	test_v1();
	printf("qrtr-wire-test: %u checks, %u failures\n", checks, failures);
	return failures != 0;
}
