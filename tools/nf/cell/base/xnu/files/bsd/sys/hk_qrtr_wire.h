/*
 * Strict, allocation-free QRTR wire decoding shared by the GLINK and MHI
 * transports.  This file knows the packet header only; service tables,
 * ports, callbacks and QMI remain above it.
 */
#ifndef _SYS_HK_QRTR_WIRE_H_
#define _SYS_HK_QRTR_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HK_QRTR_WIRE_VERSION_1       1u
#define HK_QRTR_WIRE_VERSION_2       3u
#define HK_QRTR_WIRE_PORT_CONTROL    0xfffffffeu
#define HK_QRTR_WIRE_PORT_CONTROL_16 0xfffeu
#define HK_QRTR_WIRE_PORT_LEGACY     0xffffu

enum hk_qrtr_wire_type {
	HK_QRTR_WIRE_DATA        = 1,
	HK_QRTR_WIRE_HELLO       = 2,
	HK_QRTR_WIRE_BYE         = 3,
	HK_QRTR_WIRE_NEW_SERVER  = 4,
	HK_QRTR_WIRE_DEL_SERVER  = 5,
	HK_QRTR_WIRE_DEL_CLIENT  = 6,
	HK_QRTR_WIRE_RESUME_TX   = 7,
	HK_QRTR_WIRE_NEW_LOOKUP  = 10,
	HK_QRTR_WIRE_DEL_LOOKUP  = 11,
};

struct hk_qrtr_wire_datagram {
	uint32_t version;
	uint32_t type;
	uint32_t from_node;
	uint32_t from_port;
	uint32_t to_node;
	uint32_t to_port;
	bool confirm;
	size_t header_length;
	size_t payload_length;
	const uint8_t *payload;
};

struct hk_qrtr_wire_control {
	uint32_t command;
	uint32_t words[4];
};

static inline uint16_t
hk_qrtr_wire_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static inline uint32_t
hk_qrtr_wire_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint32_t
hk_qrtr_wire_port(uint32_t port)
{
	return port == HK_QRTR_WIRE_PORT_CONTROL || port == HK_QRTR_WIRE_PORT_CONTROL_16 ||
	    port == HK_QRTR_WIRE_PORT_LEGACY ?
	    HK_QRTR_WIRE_PORT_CONTROL : port;
}

/*
 * Decode one complete datagram. QRTR v2 counts optional header bytes in
 * optlen. The payload size excludes the fixed header and options. The wire packet must contain
 * exactly the four-byte-aligned payload; a truncated or trailing packet is
 * rejected.  max_payload is a caller-owned bound, normally 8192.
 */
static inline int
hk_qrtr_wire_decode(const void *packet, size_t length, size_t max_payload,
    struct hk_qrtr_wire_datagram *out)
{
	const uint8_t *p = packet;
	size_t header_length, padded;
	uint32_t size;

	if (p == NULL || out == NULL || length == 0 || (length & 3u) != 0) {
		return -1;
	}
	if (p[0] == HK_QRTR_WIRE_VERSION_1) {
		if (length < 32) {
			return -1;
		}
		out->version = hk_qrtr_wire_le32(p);
		if (out->version != HK_QRTR_WIRE_VERSION_1) {
			return -1;
		}
		out->type = hk_qrtr_wire_le32(p + 4);
		out->from_node = hk_qrtr_wire_le32(p + 8);
		out->from_port = hk_qrtr_wire_port(hk_qrtr_wire_le32(p + 12));
		out->confirm = hk_qrtr_wire_le32(p + 16) != 0;
		size = hk_qrtr_wire_le32(p + 20);
		out->to_node = hk_qrtr_wire_le32(p + 24);
		out->to_port = hk_qrtr_wire_port(hk_qrtr_wire_le32(p + 28));
		header_length = 32;
	} else if (p[0] == HK_QRTR_WIRE_VERSION_2) {
		uint32_t from_port, to_port;

		if (length < 16) {
			return -1;
		}
		header_length = 16u + (size_t)p[3];
		if (header_length > length) {
			return -1;
		}
		out->version = p[0];
		out->type = p[1];
		out->confirm = (p[2] & 1u) != 0;
		size = hk_qrtr_wire_le32(p + 4);
		out->from_node = hk_qrtr_wire_le16(p + 8);
		from_port = hk_qrtr_wire_le16(p + 10);
		out->to_node = hk_qrtr_wire_le16(p + 12);
		to_port = hk_qrtr_wire_le16(p + 14);
		out->from_port = hk_qrtr_wire_port(from_port);
		out->to_port = hk_qrtr_wire_port(to_port);
	} else {
		return -1;
	}

	if (size == 0 || size > max_payload || size > length - header_length) {
		return -1;
	}
	padded = (size + 3u) & ~(size_t)3u;
	if (padded > length - header_length || length != header_length + padded) {
		return -1;
	}
	out->header_length = header_length;
	out->payload_length = size;
	out->payload = p + header_length;
	return 0;
}

/* Decode the command and up to four little-endian control words. */
static inline int
hk_qrtr_wire_control_decode(const struct hk_qrtr_wire_datagram *datagram,
    struct hk_qrtr_wire_control *out)
{
	if (datagram == NULL || out == NULL || datagram->payload == NULL || datagram->payload_length < 4) {
		return -1;
	}
	out->command = hk_qrtr_wire_le32(datagram->payload);
	for (unsigned i = 0; i < 4; i++) {
		out->words[i] = datagram->payload_length >= 8 + (size_t)i * 4 ?
		    hk_qrtr_wire_le32(datagram->payload + 4 + (size_t)i * 4) : 0;
	}
	return 0;
}

#endif /* _SYS_HK_QRTR_WIRE_H_ */
