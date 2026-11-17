/* AWDL data adaptation. These functions operate on one LLC MSDU, after the
 * radio has selected the link and stripped its 802.11 header (or split an
 * A-MSDU). They neither transmit frames nor maintain trust/peer state. */
#ifndef LX_AWDL_DATA_H
#define LX_AWDL_DATA_H

#include <stddef.h>
#include <stdint.h>

struct lx_awdl_ipv6 {
	const uint8_t *packet;
	size_t length;
	uint16_t sequence;
};

static inline int
lx_awdl_valid_ipv6(const uint8_t *ip, size_t length)
{
	if (!ip || length < 40 || (ip[0] >> 4) != 6) return 0;
	size_t payload = ((size_t)ip[4] << 8) | ip[5];
	return payload == length - 40;
}

/* OWL's interoperable short data format uses a 16-byte encapsulation.
 * The caller supplies an mbuf/skb with reserved headroom. The IPv6 bytes
 * remain in place and the caller retains ownership if validation fails. */
static inline size_t
lx_awdl_ipv6_encap(uint8_t *head, size_t capacity, uint16_t sequence,
    const uint8_t *ip, size_t length)
{
	if (!head || capacity < 16 || !lx_awdl_valid_ipv6(ip, length)) return 0;
	static const uint8_t prefix[] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0, 3, 4};
	for (size_t i=0; i<sizeof(prefix); i++) head[i]=prefix[i];
	head[10]=(uint8_t)sequence;
	head[11]=(uint8_t)(sequence >> 8);
	head[12]=0;
	head[13]=0;
	head[14]=0x86;
	head[15]=0xdd;
	return 16;
}

/* Decode the short envelope and the variable-length tagged envelope in the
 * Wireshark AWDL dissector. Skip unknown tag contents by their lengths. */
static inline int
lx_awdl_ipv6_decap(const uint8_t *msdu, size_t length, struct lx_awdl_ipv6 *out)
{
	static const uint8_t llc[] = {0xaa, 0xaa, 3, 0, 0x17, 0xf2, 8, 0};
	if (!out) return 0;
	*out=(struct lx_awdl_ipv6){0};
	if (!msdu || length < 16) return 0;
	for (size_t i=0; i<sizeof(llc); i++) if (msdu[i] != llc[i]) return 0;
	size_t at=12;
	if (msdu[at] == 3) {
		/* Initial descriptor, zero or more short TLVs, closing descriptor. */
		do {
			if (length-at < 2) return 0;
			size_t field=2u+msdu[at+1];
			if (field > length-at) return 0;
			at+=field;
			if (length-at < 2) return 0;
		} while (msdu[at] != 3);
		size_t end_field=2u+msdu[at+1];
		if (end_field > length-at) return 0;
		at+=end_field;
	} else {
		if (msdu[at] || msdu[at+1]) return 0;
		at+=2;
	}
	if (length-at < 2 || msdu[at]!=0x86 || msdu[at+1]!=0xdd) return 0;
	at+=2;
	if (!lx_awdl_valid_ipv6(msdu+at,length-at)) return 0;
	out->sequence=(uint16_t)((uint16_t)msdu[11] << 8 | msdu[10]);
	out->packet=msdu+at;
	out->length=length-at;
	return 1;
}
#endif
