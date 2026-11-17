#ifndef HK_AWDL_IFNET_TRANSPORT_H
#define HK_AWDL_IFNET_TRANSPORT_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifndef LX_AWDL_PORTABLE_VIEW
#include <sys/hk_awdl_provider.h>
#endif

struct hk_awdl_link_config {
    uint8_t address[6]; /* live local unicast address of this AWDL vdev */
    uint32_t mtu;      /* supported IPv6 payload MTU, excluding Ethernet */
};

/* No queue or peer is manufactured by the adapter. The transport must own
 * the vdev, synchronization clock, authorized channel windows and peer
 * validation. Input is delivered only from a schedulable, bounded worker. */
struct hk_awdl_transport_ops {
    /* A real VDEV plus functioning discovery/multicast path. A successful
     * assign_chanctx by itself is insufficient; an existing unicast peer
     * is not required. */
    int (*open)(void *ctx, uint64_t generation, uint64_t lease, struct hk_awdl_link_config *config);
    /* Nonblocking enqueue. Copy the frame before returning 0; otherwise
     * return errno. The temporary Ethernet frame is always owned by XNU. */
    int (*transmit)(void *ctx, uint64_t generation, uint64_t lease, const uint8_t *frame, size_t bytes);
    int (*channels)(void *ctx, uint64_t generation, uint64_t lease, struct hk_awdl_channels *channels);
    /* Stop producers and drain/join accepted TX plus queued RX for this
     * exact lease. Handle partial opens. Zero certifies quiescence. */
    int (*quiesce)(void *ctx, uint64_t generation, uint64_t lease);
    /* Idempotent until acknowledged. Release real hardware ownership
     * only after quiesce succeeded and both OS publications retired. */
    int (*close)(void *ctx, uint64_t generation, uint64_t lease);
};

/* Both sides of this arm64 callback boundary assert every shared field.
 * The portable view uses no XNU kernel headers. */
#ifdef __cplusplus
#define HK_AWDL_ABI_ASSERT static_assert
extern "C" {
#else
#define HK_AWDL_ABI_ASSERT _Static_assert
#endif
HK_AWDL_ABI_ASSERT(sizeof(uint64_t) == 8 && sizeof(size_t) == 8 && sizeof(bool) == 1,
                   "AWDL arm64 scalars");
HK_AWDL_ABI_ASSERT(sizeof(struct hk_awdl_link_config) == 12 &&
                   offsetof(struct hk_awdl_link_config, address) == 0 &&
                   offsetof(struct hk_awdl_link_config, mtu) == 8, "AWDL link ABI");
HK_AWDL_ABI_ASSERT(sizeof(struct hk_awdl_channels) == 6 &&
                   offsetof(struct hk_awdl_channels, preferred) == 0 &&
                   offsetof(struct hk_awdl_channels, peer_master) == 2 &&
                   offsetof(struct hk_awdl_channels, extension) == 4, "AWDL channels ABI");
HK_AWDL_ABI_ASSERT(sizeof(struct hk_awdl_transport_ops) == 40 &&
                   offsetof(struct hk_awdl_transport_ops, open) == 0 &&
                   offsetof(struct hk_awdl_transport_ops, transmit) == 8 &&
                   offsetof(struct hk_awdl_transport_ops, channels) == 16 &&
                   offsetof(struct hk_awdl_transport_ops, quiesce) == 24 &&
                   offsetof(struct hk_awdl_transport_ops, close) == 32, "AWDL callbacks ABI");
#undef HK_AWDL_ABI_ASSERT

#ifndef LX_AWDL_PORTABLE_VIEW
int hk_awdl_transport_register(const struct hk_awdl_transport_ops *, void *ctx, uint64_t *generation);
int hk_awdl_transport_set_ready(uint64_t generation, bool ready);
int hk_awdl_transport_unregister(uint64_t generation);
/* Caller retains the frame and may reclaim it after return on all paths.
 * Pass only decapsulated, peer-validated Ethernet/IPv6 for the current AW.
 * Calls from interrupt/filter context are not supported. */
int hk_awdl_transport_input(uint64_t generation, uint64_t lease, const uint8_t *frame, size_t bytes);
#endif

/* Explicit radio-owner calls only. All status codes, including errors from
 * callbacks, use the positive Darwin errno convention. An unregister error
 * leaves the registration and callback context pinned for retry. */
int lx_os_awdl_register(const struct hk_awdl_transport_ops *ops, void *context, uint64_t *generation);
int lx_os_awdl_set_ready(uint64_t generation, bool ready);
int lx_os_awdl_unregister(uint64_t generation);
int lx_os_awdl_input(uint64_t generation, uint64_t lease, const uint8_t *frame, size_t bytes);
enum hk_awdl_peer_source {
    HK_AWDL_PEER_PROVISIONED = 1,
    HK_AWDL_PEER_OVER_AIR_VERIFIED = 2,
    HK_AWDL_PEER_OVER_AIR_L2 = 4
};
/* Up to eight live MACs per lease. The serial radio worker supplies
 * OVER_AIR_L2 with NULL IPv6 after a qualified action; XNU derives the
 * EUI-64 route. VERIFIED supplies the current observed scoped LLA as its
 * 16 unembedded wire bytes. PROVISIONED requires the configured EUI-64
 * pair. An old address's absence cannot delete a current observed LLA.
 * Report a rotated MAC separately, and withdraw the old identity only
 * on an actual departure/expiry. Retry errors without inventing peers. */
int lx_os_awdl_peer_present(uint64_t generation, uint64_t lease, unsigned source,
                            const uint8_t mac[6], const uint8_t ipv6[16]);
int lx_os_awdl_peer_absent(uint64_t generation, uint64_t lease, unsigned source,
                           const uint8_t mac[6], const uint8_t ipv6[16]);

#ifdef __cplusplus
}
#endif
#endif
