/* Passive observations at the host SKB boundary, not an RF receive counter.
 * No addresses/payloads retained; no packet decision or contents changed. */
#ifndef HK_AWDL_RX_BOUNDARY_AUDIT_H
#define HK_AWDL_RX_BOUNDARY_AUDIT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum { RXA_EARLY,RXA_ETH_ADMISSION,RXA_ETH_SHAPE,RXA_RAW_ADMISSION,
       RXA_SHORT,RXA_AMSDU,RXA_DECAP,RXA_DROP_COUNT };
static struct {
    unsigned host_eth,host_raw_data,raw_awdl,raw_awdl_bad_header;
    unsigned vendor_eth,length_vendor_eth,drop[RXA_DROP_COUNT];
    unsigned parsed,post_rejected;
} rx_boundary;
#define RXA_INC(v) ((void)__atomic_fetch_add(&(v),1,__ATOMIC_RELAXED))
static inline bool rx_boundary_equal(const uint8_t *a,const uint8_t *b,size_t n)
{
    for(size_t i=0;i<n;i++)if(a[i]!=b[i])return false;
    return true;
}
static inline bool rx_boundary_begin(const uint8_t *p,size_t n,bool ethernet)
{
    if(!p)return false;
    static const uint8_t network[6]={0,0x25,0,0xff,0x94,0x73};
    static const uint8_t llc[8]={0xaa,0xaa,3,0,0x17,0xf2,8,0};
    if(ethernet) {
        RXA_INC(rx_boundary.host_eth);
        if(n>=16 && p[12]==8 && !p[13] && p[14]==3 && p[15]==4)
            RXA_INC(rx_boundary.vendor_eth);
        if(n>=22 && (((unsigned)p[12]<<8)|p[13])<=1500 && rx_boundary_equal(p+14,llc,8))
            RXA_INC(rx_boundary.length_vendor_eth);
        return true;
    }
    if(n<2 || (p[0]&0x0c)!=8)return false;
    RXA_INC(rx_boundary.host_raw_data);
    if(n>=24 && rx_boundary_equal(p+16,network,6)) {
        RXA_INC(rx_boundary.raw_awdl);
        if((p[0]!=8 && p[0]!=0x88) || (p[1]&0x47) || (p[22]&15))
            RXA_INC(rx_boundary.raw_awdl_bad_header);
    }
    return true;
}
static inline bool rx_boundary_drop(unsigned reason,bool data)
{
    if(data && reason<RXA_DROP_COUNT)RXA_INC(rx_boundary.drop[reason]);
    return false;
}
#endif
