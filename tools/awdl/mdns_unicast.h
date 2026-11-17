/* Carry a native mDNS multicast exchange over a real peer's unicast link.
 * RFC6762 section5.5 permits direct queries to5353. DNS payloads, IDs and
 * questions/answers are immutable. The receiving virtual NIC restores the
 * original logical multicast destination only for a validated peer with a
 * recently carried native query. It never creates a DNS record or service. */
#ifndef HK_AWDL_MDNS_UNICAST_H
#define HK_AWDL_MDNS_UNICAST_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "wire_checksum.h"
/* Diagnostic transport port; UDP mDNS remains5353 on the native ifnet. */
#define HK_MDNS_CARRIER_PORT 61743u
static const uint8_t hk_mdns_ip[16]={0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0xfb};
static const uint8_t hk_mdns_mac[6]={0x33,0x33,0,0,0,0xfb};
static inline bool hk_mdns_equal(const uint8_t *a,const uint8_t *b,size_t n)
{for(size_t i=0;i<n;i++)if(a[i]!=b[i])return false;return true;}
static inline bool hk_mdns_shape(const uint8_t *eth,size_t n,bool reply)
{
 if(!eth || n<14+40+8+12 || eth[12]!=0x86 || eth[13]!=0xdd)return false;
 const uint8_t *ip=eth+14,*udp=ip+40,*dns=udp+8;
 if((ip[0]>>4)!=6 || ip[6]!=17 || ip[7]!=255 || ip[8]!=0xfe || (ip[9]&0xc0)!=0x80 ||
    (size_t)(((unsigned)ip[4]<<8)|ip[5])!=n-54 ||
    udp[0]!=0x14 || udp[1]!=0xe9 ||
    (((unsigned)udp[2]<<8)|udp[3])!=(reply?HK_MDNS_CARRIER_PORT:5353u) ||
    (size_t)(((unsigned)udp[4]<<8)|udp[5])!=n-54 ||
    dns[0] || dns[1] || !!(dns[2]&0x80)!=reply || (dns[2]&0x78) ||
    awdl_wire_checksum(ip,n-14)!=1)return false;
 if(!reply && !(dns[4]|dns[5]))return false;
 return true;
}
static inline void hk_mdns_checksum(uint8_t *eth,size_t n)
{
 uint8_t *ip=eth+14,*udp=ip+40;size_t len=n-54;udp[6]=udp[7]=0;
 uint32_t sum=(uint32_t)len+17;
 for(unsigned i=8;i<40;i+=2)sum+=((unsigned)ip[i]<<8)|ip[i+1];
 for(size_t i=0;i<len;i+=2)sum+=((unsigned)udp[i]<<8)|(i+1<len?udp[i+1]:0);
 while(sum>>16)sum=(sum&65535)+(sum>>16);
 uint16_t value=(uint16_t)~sum;if(!value)value=65535;
 udp[6]=(uint8_t)(value>>8);udp[7]=(uint8_t)value;
}
static inline bool hk_mdns_query_unicast(uint8_t *eth,size_t n,
                                        const uint8_t peer_mac[6],const uint8_t peer_ip[16])
{
 if(!peer_mac || !peer_ip || (peer_mac[0]&1) ||
    !(peer_mac[0]|peer_mac[1]|peer_mac[2]|peer_mac[3]|peer_mac[4]|peer_mac[5]) ||
    peer_ip[0]!=0xfe || (peer_ip[1]&0xc0)!=0x80 || !hk_mdns_shape(eth,n,false) ||
    !hk_mdns_equal(eth,hk_mdns_mac,6) || !hk_mdns_equal(eth+38,hk_mdns_ip,16))return false;
 for(unsigned i=0;i<6;i++)eth[i]=peer_mac[i];
 for(unsigned i=0;i<16;i++)eth[38+i]=peer_ip[i];
 eth[54]=(uint8_t)(HK_MDNS_CARRIER_PORT>>8);eth[55]=(uint8_t)HK_MDNS_CARRIER_PORT;
 hk_mdns_checksum(eth,n);return true;
}
static inline bool hk_mdns_reply_multicast(uint8_t *eth,size_t n,
                                          const uint8_t local_ip[16],bool query_pending)
{
 if(!query_pending || !local_ip || !hk_mdns_shape(eth,n,true) ||
    (eth[0]&1) || !hk_mdns_equal(eth+38,local_ip,16))return false;
 for(unsigned i=0;i<6;i++)eth[i]=hk_mdns_mac[i];
 for(unsigned i=0;i<16;i++)eth[38+i]=hk_mdns_ip[i];
 eth[56]=0x14;eth[57]=0xe9;
 hk_mdns_checksum(eth,n);return true;
}
#endif
