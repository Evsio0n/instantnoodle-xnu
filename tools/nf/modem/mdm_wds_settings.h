/* Passive projection shared by NF's unbound WDS observer and the separately
 * family-bound raw-IP adapter. Only the latter may qualify a bearer. */
#ifndef MDM_WDS_SETTINGS_H
#define MDM_WDS_SETTINGS_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#ifdef KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif
#ifndef HK_CELL_HOST_TEST
#include <sys/hk_qmi_radio.h>
#include <sys/hk_qrtr_wire.h>
#endif

enum { MDM_WDS_CURRENT = 0x002d, MDM_WDS_MRU = 32768 };
struct mdm_wds_settings {
	uint8_t family, prefix, address[16];
	uint32_t mtu;
};

static inline int
mdm_wds_validate_ip(const uint8_t *addr, unsigned family, unsigned prefix, uint32_t mtu)
{
	if (family != 4 && family != 6)
		return EAFNOSUPPORT;
	if (mtu > MDM_WDS_MRU || mtu < (family == 6 ? 1280u : 68u))
		return EMSGSIZE;
	if (family == 4)
		return addr[0] == 0 || addr[0] == 127 || addr[0] >= 224 ? EADDRNOTAVAIL : 0;
	static const uint8_t zero[16] = {0}, loopback[16] = {[15] = 1};
	if (prefix > 128 || !memcmp(addr, zero, 16) || !memcmp(addr, loopback, 16) || addr[0] == 0xff)
		return EADDRNOTAVAIL;
	return 0;
}

/* Wire order from libqmi's data/qmi-service-wds.json. Family 0 observes the
 * wire-reported family, 4/6 require an independently family-bound client. */
static inline int
mdm_wds_decode_current(const void *bytes, size_t length, uint16_t transaction, unsigned expected_family,
                       struct mdm_wds_settings *out)
{
	enum { RESULT = 1, FAMILY = 2, MTU = 4, V4 = 8, V6 = 16 };
	const uint8_t *wire = bytes, *v4 = NULL, *v6 = NULL;
	struct mdm_wds_settings settings = {0};
	unsigned seen = 0;
	if (!out)
		return EINVAL;
	memset(out, 0, sizeof(*out));
	if (!wire || length < QMI_HEADER_SIZE || wire[0] != QMI_RESPONSE ||
	    hk_qrtr_wire_le16(wire + 3) != MDM_WDS_CURRENT ||
	    hk_qrtr_wire_le16(wire + 5) != length - QMI_HEADER_SIZE)
		return EBADMSG;
	if (hk_qrtr_wire_le16(wire + 1) != transaction)
		return ESTALE;

	for (size_t pos = QMI_HEADER_SIZE; pos < length;) {
		if (length - pos < 3)
			return EBADMSG;
		unsigned tag = wire[pos], size = hk_qrtr_wire_le16(wire + pos + 1);
		pos += 3;
		if (size > length - pos)
			return EBADMSG;
		const uint8_t *value = wire + pos;
		pos += size;
		unsigned field;
		switch (tag) {
		case 0x02:
			if (size != 4)
				return EBADMSG;
			if (hk_qrtr_wire_le16(value) == 1 && hk_qrtr_wire_le16(value + 2))
				return ENETDOWN;
			if (hk_qrtr_wire_le32(value))
				return EBADMSG;
			field = RESULT;
			break;
		case 0x1e:
			if (size != 4)
				return EBADMSG;
			v4 = value;
			field = V4;
			break;
		case 0x25:
			if (size != 17 || value[16] > 128)
				return EBADMSG;
			v6 = value;
			field = V6;
			break;
		case 0x29:
			if (size != 4)
				return EBADMSG;
			settings.mtu = hk_qrtr_wire_le32(value);
			field = MTU;
			break;
		case 0x2b:
			if (size != 1)
				return EBADMSG;
			settings.family = value[0];
			if ((expected_family && expected_family != settings.family) ||
			    (settings.family != 4 && settings.family != 6))
				return EAFNOSUPPORT;
			field = FAMILY;
			break;
		default:
			if (tag < 0x10)
				return EBADMSG;
			continue;
		}
		if (seen & field)
			return EBADMSG;
		seen |= field;
	}
	if ((seen & (RESULT | FAMILY | MTU)) != (RESULT | FAMILY | MTU))
		return EBADMSG;
	if (settings.family == 4) {
		if (!v4)
			return EBADMSG;
		for (unsigned i = 0; i < 4; i++)
			settings.address[i] = v4[3 - i];
	} else {
		if (!v6)
			return EBADMSG;
		memcpy(settings.address, v6, 16);
		settings.prefix = v6[16];
	}
	int err = mdm_wds_validate_ip(settings.address, settings.family, settings.prefix, settings.mtu);
	if (err)
		return err;
	*out = settings;
	return 0;
}
#endif
