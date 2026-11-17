#ifndef LX_AWDL_ND6_OBSERVE_H
#define LX_AWDL_ND6_OBSERVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Only observations of direct ICMPv6 from the configured link-local peer.
 * The caller has already verified the received 802.11/Ethernet source
 * and destination. No category establishes reachability or changes a cache. */
enum lx_awdl_nd6_observation {
    LX_AWDL_ND6_OTHER = 0,
    LX_AWDL_ND6_PEER_NS_FOR_LOCAL = 1,
    LX_AWDL_ND6_PEER_NA_FOR_LOCAL = 2,
    LX_AWDL_ND6_PEER_NA_FOR_PEER = 3
};

static inline bool lx_nd6_bytes_equal(const uint8_t *a, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i]) return false;
    return true;
}

static inline bool lx_nd6_checksum_ok(const uint8_t *ip, size_t payload)
{
    uint32_t sum = (uint32_t)payload + 58;
    for (size_t i = 8; i < 40 + payload; i += 2)
        sum += ((uint16_t)ip[i] << 8) | ip[i + 1];
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return sum == 0xffff;
}

static inline enum lx_awdl_nd6_observation
lx_awdl_nd6_observe(const uint8_t *ip, size_t len, const uint8_t local[16],
                    const uint8_t peer[16], const uint8_t peer_mac[6])
{
    static const uint8_t all_nodes[16] = {0xff, 2, [15] = 1};
    static const uint8_t solicited_prefix[13] = {0xff, 2, [11] = 1, [12] = 0xff};
    static const uint8_t zero_mac[6] = {0};
    if (!ip || len < 64 || !local || !peer || !peer_mac ||
        local[0] != 0xfe || (local[1] & 0xc0) != 0x80 ||
        peer[0] != 0xfe || (peer[1] & 0xc0) != 0x80 ||
        lx_nd6_bytes_equal(local, peer, 16) || (peer_mac[0] & 1) ||
        lx_nd6_bytes_equal(peer_mac, zero_mac, 6))
        return LX_AWDL_ND6_OTHER;
    size_t payload = ((size_t)ip[4] << 8) | ip[5];
    if ((ip[0] >> 4) != 6 || ip[6] != 58 || ip[7] != 255 ||
        payload != len - 40 || (payload & 7) || ip[41] != 0 ||
        (ip[40] != 135 && ip[40] != 136) || !lx_nd6_bytes_equal(ip + 8, peer, 16))
        return LX_AWDL_ND6_OTHER;

    bool solicitation = ip[40] == 135;
    bool to_local = lx_nd6_bytes_equal(ip + 24, local, 16);
    bool for_local = lx_nd6_bytes_equal(ip + 48, local, 16);
    if (solicitation) {
        bool to_solicited_group = lx_nd6_bytes_equal(ip + 24, solicited_prefix, 13) &&
                                  lx_nd6_bytes_equal(ip + 37, local + 13, 3);
        if (!for_local || (!to_local && !to_solicited_group))
            return LX_AWDL_ND6_OTHER;
    } else {
        bool to_all_nodes = lx_nd6_bytes_equal(ip + 24, all_nodes, 16);
        if ((!for_local && !lx_nd6_bytes_equal(ip + 48, peer, 16)) ||
            (!to_local && !to_all_nodes) || (to_all_nodes && (ip[44] & 0x40)))
            return LX_AWDL_ND6_OTHER;
    }

    bool has_peer_option = false;
    size_t offset = 64;
    while (offset < len) {
        if (len - offset < 2)
            return LX_AWDL_ND6_OTHER;
        size_t bytes = (size_t)ip[offset + 1] * 8;
        if (!bytes || bytes > len - offset)
            return LX_AWDL_ND6_OTHER;
        uint8_t kind = ip[offset];
        if (kind == 1 || kind == 2) {
            if (kind != (solicitation ? 1 : 2) || bytes != 8 ||
                !lx_nd6_bytes_equal(ip + offset + 2, peer_mac, 6))
                return LX_AWDL_ND6_OTHER;
            has_peer_option = true;
        }
        offset += bytes;
    }
    /* RFC 4861 4.3 requires the source LLA in a multicast NS on Ethernet
     * if the source is specified, as it must be for this observer. */
    if ((solicitation && !to_local && !has_peer_option) || !lx_nd6_checksum_ok(ip, payload))
        return LX_AWDL_ND6_OTHER;
    if (solicitation)
        return LX_AWDL_ND6_PEER_NS_FOR_LOCAL;
    return for_local ? LX_AWDL_ND6_PEER_NA_FOR_LOCAL : LX_AWDL_ND6_PEER_NA_FOR_PEER;
}
#endif
