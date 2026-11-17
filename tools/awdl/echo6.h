#ifndef LX_AWDL_ECHO6_H
#define LX_AWDL_ECHO6_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LX_ECHO_SIZE 64u

/* One fixed IPv6 ICMP Echo Request, addressed to the configured link-local
 * test peer. Request and Reply both carry the caller's entire 16-byte nonce.
 * The owner admits one transmission and consumes a match once per trial. */
static inline bool lx_echo_equal(const uint8_t *a, const uint8_t *b, size_t count)
{
    for (size_t i = 0; i < count; i++)
        if (a[i] != b[i]) return false;
    return true;
}

static inline bool lx_echo_endpoints(const uint8_t *local, const uint8_t *remote)
{
    return local && remote && local[0] == 0xfe && (local[1] & 0xc0) == 0x80 &&
        remote[0] == 0xfe && (remote[1] & 0xc0) == 0x80 && !lx_echo_equal(local, remote, 16);
}

static inline uint16_t lx_echo_checksum(const uint8_t *packet)
{
    /* The pseudo-header adds both IPv6 addresses, length 24, protocol 58.
     * The ICMP header and data are the 24 bytes immediately following them. */
    uint32_t sum = 24 + 58;
    for (unsigned i = 8; i < LX_ECHO_SIZE; i += 2)
        sum += ((uint16_t)packet[i] << 8) | packet[i + 1];
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)~sum;
}

static inline size_t lx_echo_request(uint8_t *out, size_t capacity, const uint8_t local[16],
                                    const uint8_t remote[16], const uint8_t nonce[16])
{
    if (!out || !nonce || capacity < LX_ECHO_SIZE || !lx_echo_endpoints(local, remote))
        return 0;
    /* Stage the packet so input addresses/nonce may alias the output. */
    uint8_t p[LX_ECHO_SIZE] = {0};
    p[0] = 0x60;
    p[5] = 24;
    p[6] = 58;
    p[7] = 64;
    p[40] = 128;
    for (unsigned i = 0; i < 16; i++) {
        p[8 + i] = local[i];
        p[24 + i] = remote[i];
        p[48 + i] = nonce[i];
    }
    for (unsigned i = 0; i < 4; i++)
        p[44 + i] = nonce[i]; /* ID and sequence in network byte order */
    uint16_t checksum = lx_echo_checksum(p);
    p[42] = (uint8_t)(checksum >> 8);
    p[43] = (uint8_t)checksum;
    for (unsigned i = 0; i < LX_ECHO_SIZE; i++)
        out[i] = p[i];
    return LX_ECHO_SIZE;
}

static inline bool lx_echo_reply(const uint8_t *p, size_t size, const uint8_t local[16],
                                 const uint8_t remote[16], const uint8_t nonce[16])
{
    return p && nonce && size == LX_ECHO_SIZE && lx_echo_endpoints(local, remote) &&
        (p[0] >> 4) == 6 && p[4] == 0 && p[5] == 24 && p[6] == 58 && p[7] != 0 &&
        p[40] == 129 && p[41] == 0 && lx_echo_equal(p + 8, remote, 16) &&
        lx_echo_equal(p + 24, local, 16) && lx_echo_equal(p + 44, nonce, 4) &&
        lx_echo_equal(p + 48, nonce, 16) && lx_echo_checksum(p) == 0;
}

/* The current board candidate already uses the hk-prefixed spellings. */
#define hk_awdl_echo6_request lx_echo_request
#define hk_awdl_echo6_reply lx_echo_reply
#endif
