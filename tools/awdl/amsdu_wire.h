/* An HT A-MSDU separates the MPDU receiver from the logical MSDU DA.
 * One subframe is enough for multicast-to-unicast delivery. Only native
 * IPv6 group traffic is accepted; the caller requires a live HT peer and
 * actual unicast presence/channel grant. The packet bytes are unchanged. */
#ifndef HK_AWDL_AMSDU_WIRE_H
#define HK_AWDL_AMSDU_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include "lx_awdl_data.h"
static inline size_t lx_awdl_amsdu_ipv6(uint8_t *out,size_t capacity,
    const uint8_t *eth,size_t n,const uint8_t self[6],const uint8_t receiver[6],uint16_t seq)
{
    if(!out || !eth || !self || !receiver || n<54 || eth[12]!=0x86 || eth[13]!=0xdd ||
       !lx_awdl_valid_ipv6(eth+14,n-14) || capacity<n+42 || (receiver[0]&1) ||
       !(receiver[0]|receiver[1]|receiver[2]|receiver[3]|receiver[4]|receiver[5]) ||
       __builtin_memcmp(eth+6,self,6) || eth[0]!=0x33 || eth[1]!=0x33 ||
       eth[38]!=0xff || __builtin_memcmp(eth+2,eth+50,4) || n-14+16>3839)return 0;
    static const uint8_t bssid[6]={0,0x25,0,0xff,0x94,0x73};
    __builtin_memset(out,0,40);out[0]=0x88;
    __builtin_memcpy(out+4,receiver,6);__builtin_memcpy(out+10,self,6);__builtin_memcpy(out+16,bssid,6);
    out[22]=(uint8_t)(seq<<4);out[23]=(uint8_t)(seq>>4);
    out[24]=0x80; /* QoS TID0, A-MSDU present; NoDS remains clear. */
    __builtin_memcpy(out+26,eth,6);__builtin_memcpy(out+32,self,6);
    unsigned length=(unsigned)(n-14+16);out[38]=(uint8_t)(length>>8);out[39]=(uint8_t)length;
    __builtin_memcpy(out+56,eth+14,n-14);
    if(!lx_awdl_ipv6_encap(out+40,16,seq,out+56,n-14))return 0;
    return n+42;
}
#endif
