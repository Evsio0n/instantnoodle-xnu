#ifndef HK_AWDL_RX_TCP_PROBE_H
#define HK_AWDL_RX_TCP_PROBE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "lx_awdl_data.h"
/* Passive parser before admission, for TCP8770 addressed to our actual IPv6.
 * Unsupported encryption, extensions, A-MSDU and truncated/nonlinear SKBs
 * remain outside coverage. Missing probe rows alone do not prove RF absence.
 * This helper never authorizes or rewrites an SKB. */
struct hk_rx_tcp_probe {bool valid;unsigned src,dst,flags,payload;uint32_t seq,ack;const uint8_t *source;};
static uint32_t hk_rx_u32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static struct hk_rx_tcp_probe hk_probe_tcp(const uint8_t *p,size_t n,bool eth,const uint8_t local[16])
{
    struct hk_rx_tcp_probe t={0};const uint8_t *ip=NULL,*source=NULL;size_t bytes=0;
    if(!p || !local)return t;
    if(eth){
        if(n<14)return t;
        source=p+6;
        if(p[12]==0x86 && p[13]==0xdd){ip=p+14;bytes=n-14;}
        else if(p[12]==8 && !p[13] && n>=22 && p[14]==3 && p[15]==4 && p[20]==0x86 && p[21]==0xdd){ip=p+22;bytes=n-22;}
        else return t;
    }else{
        if(n<24 || (p[0]!=8 && p[0]!=0x88) || (p[1]&0x43))return t;
        size_t h=p[0]==0x88?26:24;
        if(n<h || (h==26 && (p[24]&0x80)))return t;
        if(h==26 && (p[1]&0x80))h+=4;
        struct lx_awdl_ipv6 envelope;
        if(n<h || !lx_awdl_ipv6_decap(p+h,n-h,&envelope))return t;
        source=p+10;ip=envelope.packet;bytes=envelope.length;
    }
    if(bytes<60 || (ip[0]>>4)!=6 || ip[6]!=6 || __builtin_memcmp(ip+24,local,16))return t;
    unsigned length=(unsigned)ip[4]<<8|ip[5],header=(ip[52]>>4)*4;
    if(length<20 || length>bytes-40 || header<20 || header>length)return t;
    t.src=(unsigned)ip[40]<<8|ip[41];t.dst=(unsigned)ip[42]<<8|ip[43];
    if(t.src!=8770 && t.dst!=8770)return (struct hk_rx_tcp_probe){0};
    t.seq=hk_rx_u32(ip+44);t.ack=hk_rx_u32(ip+48);t.flags=ip[53];t.payload=length-header;t.source=source;t.valid=true;
    return t;
}
#endif
