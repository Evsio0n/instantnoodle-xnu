#ifndef HK_CELL_RAWIP_H
#define HK_CELL_RAWIP_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#ifndef HK_CELL_HOST_TEST
#include <net/kpi_interface.h>
#include <sys/kpi_mbuf.h>
#include <sys/hk_qmi_radio.h>
#endif

enum { HK_CELL_IPV4 = 1, HK_CELL_IPV6 = 2, HK_CELL_MRU = 32768 };

/* QRTR carries the peer and the receiving QMI handle out of band. Capture
 * these for each reply while the family-bound client is still live. Its
 * owner must change the epoch on bearer change, handle reuse or revocation. */
struct qmi_handle;

struct hk_cell_wds_client {
	uint64_t epoch;
	const struct qmi_handle *handle;
	uint32_t service, node, port;
	uint8_t family;
};

/* Supplied by validated WDS Get Current Settings for the same WDS client and
 * family as Get Packet Service Status. A valid zeroed address is never usable. */
struct hk_cell_bearer {
	uint64_t epoch;
	uint8_t family;             /* IP version, 4 or 6 */
	uint8_t ipv6_prefix_length; /* From the local IPv6 address TLV, valid for family 6. */
	bool local_present, mtu_present;
	uint8_t local[16];
	uint32_t mtu;
	struct hk_qmi_wds_packet_service_status_resp status;
	struct hk_cell_wds_client source; /* Family-bound source checked with the settings transaction. */
};
struct hk_cell_wds_status {
	struct hk_cell_wds_client source;
	struct hk_qmi_wds_packet_service_status_resp status; /* Validated 0x0022 response. */
};

/* Passive only. Match a checked status and a complete 0x002d QMI reply
 * against the expected transaction. Clears out on every error. */
int hk_cell_decode_wds_settings(const struct hk_cell_wds_status *status,
                                const struct hk_cell_wds_client *source, uint16_t transaction,
                                const void *reply, size_t length, struct hk_cell_bearer *out);

struct hk_cell_format {
	uint64_t epoch;
	uint32_t endpoint_type, endpoint_id;
	struct hk_qmi_wda_format wda;
};

struct mhi_host;

struct hk_cell_transport {
	uint64_t epoch;
	uint32_t ul_channel, ul_er, dl_channel, dl_er, max_datagram;
	bool ul_ready, dl_ready;
	void *ctx;
	/* Synchronously accept/copy one complete raw packet before returning 0.
	 * The mbuf is borrowed for this call, even on failure. No callbacks into
	 * open/close from this function. Transport's own DMA retirement is separate. */
	int (*copy_tx)(void *ctx, uint64_t epoch, mbuf_t packet, size_t bytes);
	/* Optional concrete receiving endpoint. Only this MHI host may feed its
	 * stamped ER8 transfer units into this binding. */
	struct mhi_host *rx_host;
};

/* Lifecycle calls run in thread context. Each consecutive transport/bearer
 * binding requires a new nonzero monotonically increasing epoch. */
int hk_cell_open(uint64_t epoch, const struct hk_cell_format *format, const struct hk_cell_bearer *v4,
                 const struct hk_cell_bearer *v6, const struct hk_cell_transport *transport);
/* WDS bearer indications, settings changes and transport lifecycle events
 * must revalidate the existing epoch or call close on loss. Recheck never
 * creates an interface. A changed family/address/MTU needs a fresh epoch. */
int hk_cell_recheck(uint64_t epoch, const struct hk_cell_format *format, const struct hk_cell_bearer *v4,
                    const struct hk_cell_bearer *v6, const struct hk_cell_transport *transport);
/* Open only with this WDS client's actual result and family-bound settings.
 * An unbound client can supply negative evidence but cannot authorize an ifnet.
 * Ready channels, capacity and the XNU output producer come from the MHI owner. */
int hk_cell_mhi_open(struct mhi_host *host, uint64_t epoch, const struct hk_cell_format *format,
    const struct hk_cell_wds_status *observed, const struct hk_cell_bearer *v4,
    const struct hk_cell_bearer *v6);
int hk_cell_mhi_recheck(struct mhi_host *host, uint64_t epoch, const struct hk_cell_format *format,
    const struct hk_cell_wds_status *observed, const struct hk_cell_bearer *v4,
    const struct hk_cell_bearer *v6);
int hk_cell_close(uint64_t epoch);
/* Owns and frees/forwards all non-NULL packets, including rejected input. */
int hk_cell_input(uint64_t epoch, mbuf_t packet);
int hk_cell_mhi_ingress(struct mhi_host *host, uint64_t epoch);
void hk_cell_mhi_revoke(struct mhi_host *host, uint64_t epoch);
#endif
