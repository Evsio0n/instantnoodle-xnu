#include <assert.h>
#include <string.h>
#include "mdns_unicast.h"
static const uint8_t local_mac[6]={2,6,7,8,9,10},peer_mac[6]={2,1,2,3,4,5};
static const uint8_t local_ip[16]={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0x11};
static const uint8_t peer_ip[16]={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,0x22};
static size_t question(uint8_t p[100]){
 memset(p,0,100);memcpy(p,hk_mdns_mac,6);memcpy(p+6,local_mac,6);p[12]=0x86;p[13]=0xdd;
 uint8_t *ip=p+14,*u=ip+40,*d=u+8;ip[0]=0x60;ip[5]=30;ip[6]=17;ip[7]=255;
 memcpy(ip+8,local_ip,16);memcpy(ip+24,hk_mdns_ip,16);u[0]=u[2]=0x14;u[1]=u[3]=0xe9;u[5]=30;
 d[5]=1;const uint8_t q[]={4,'t','e','s','t',0,0,12,0,1};memcpy(d+12,q,sizeof(q));
 hk_mdns_checksum(p,84);assert(awdl_wire_checksum(ip,70)==1);return 84;
}
int main(void){
 uint8_t p[100],original[100],reply[100],before[100];size_t n=question(p);memcpy(original,p,100);
 assert(hk_mdns_query_unicast(p,n,peer_mac,peer_ip));
 assert(!memcmp(p,peer_mac,6) && !memcmp(p+38,peer_ip,16));
 assert((((unsigned)p[54]<<8)|p[55])==HK_MDNS_CARRIER_PORT);
 assert(!memcmp(p+62,original+62,n-62));assert(awdl_wire_checksum(p+14,n-14)==1);
 assert(!hk_mdns_query_unicast(p,n,peer_mac,peer_ip));
 memcpy(reply,p,100);memcpy(reply,local_mac,6);memcpy(reply+6,peer_mac,6);
 reply[54]=0x14;reply[55]=0xe9;reply[56]=(uint8_t)(HK_MDNS_CARRIER_PORT>>8);reply[57]=(uint8_t)HK_MDNS_CARRIER_PORT;
 memcpy(reply+22,peer_ip,16);memcpy(reply+38,local_ip,16);reply[64]=0x84;reply[67]=0;reply[69]=1;hk_mdns_checksum(reply,n);memcpy(before,reply,100);
 assert(!hk_mdns_reply_multicast(reply,n,local_ip,false));assert(!memcmp(reply,before,100));
 assert(hk_mdns_reply_multicast(reply,n,local_ip,true));assert(!memcmp(reply,hk_mdns_mac,6));
 assert(!memcmp(reply+38,hk_mdns_ip,16));assert(reply[56]==0x14 && reply[57]==0xe9);assert(!memcmp(reply+62,before+62,n-62));assert(awdl_wire_checksum(reply+14,n-14)==1);
 assert(!hk_mdns_reply_multicast(reply,n,local_ip,true));
 for(size_t short_n=0;short_n<74;short_n++){
  memcpy(p,original,100);assert(!hk_mdns_query_unicast(p,short_n,peer_mac,peer_ip));assert(!memcmp(p,original,100));
 }
 /* Rejection preserves every byte, including invalid checksums, non-mDNS
  * ports, responses, legacy IDs, wrong hop limits and extension headers. */
 const unsigned positions[]={61,54,64,62,21,20};const uint8_t values[]={0,0,0x80,1,64,0};
 for(unsigned i=0;i<sizeof(positions)/sizeof(positions[0]);i++){
  memcpy(p,original,100);p[positions[i]]=values[i];if(i)hk_mdns_checksum(p,n);memcpy(before,p,100);
  assert(!hk_mdns_query_unicast(p,n,peer_mac,peer_ip));assert(!memcmp(p,before,100));
 }
 memcpy(p,original,100);assert(!hk_mdns_query_unicast(p,n,hk_mdns_mac,peer_ip));assert(!memcmp(p,original,100));
 return 0;
}
