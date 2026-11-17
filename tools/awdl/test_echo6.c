/* Offline synthetic RFC 4443 vectors. Addresses and nonce are local constants. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "echo6.h"

int main(void)
{
    const uint8_t local[16] = {0xfe,0x80,[15]=1};
    const uint8_t remote[16] = {0xfe,0x80,[15]=2};
    const uint8_t global[16] = {0x20,1,[15]=1};
    uint8_t nonce[16], different[16], packet[64], original[64];
    for (unsigned i = 0; i < sizeof(nonce); i++)
        nonce[i] = (uint8_t)i;
    memcpy(different, nonce, sizeof(nonce));
    different[15]++;
    memset(packet, 0x5a, sizeof(packet));
    memcpy(original, packet, sizeof(packet));
    assert(!lx_echo_request(packet,63,local,remote,nonce));
    assert(!lx_echo_request(packet,64,local,global,nonce));
    assert(!lx_echo_request(packet,64,local,local,nonce));
    assert(!memcmp(packet,original,64));
    assert(lx_echo_request(packet,64,local,remote,nonce)==LX_ECHO_SIZE);
    /* Independently calculated with a 40-byte IPv6 pseudo-header including
     * a network-order uint32 payload length and final next-header octet. */
    assert(packet[42]==0x48 && packet[43]==0x64);
    assert(!memcmp(packet+48,nonce,16) && !memcmp(packet+44,nonce,4));
    assert(!lx_echo_reply(packet,64,local,remote,nonce));
    memcpy(packet+8,remote,16);
    memcpy(packet+24,local,16);
    packet[40]=129;
    packet[42]=0x47;
    assert(lx_echo_reply(packet,64,local,remote,nonce));
    assert(!lx_echo_reply(packet,63,local,remote,nonce));
    assert(!lx_echo_reply(packet,64,remote,local,nonce));
    assert(!lx_echo_reply(packet,64,local,remote,different));
    packet[43]^=1;
    assert(!lx_echo_reply(packet,64,local,remote,nonce));
    packet[43]^=1;
    packet[7]=0;
    assert(!lx_echo_reply(packet,64,local,remote,nonce));
    puts("passed: fixed Echo Request/Reply checksums, exact nonce and endpoints, bounded length, link-local isolation");
}
