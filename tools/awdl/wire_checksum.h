#ifndef HK_AWDL_WIRE_CHECKSUM_H
#define HK_AWDL_WIRE_CHECKSUM_H
#include <stddef.h>
#include <stdint.h>
/* Inspect the actual copied native packet. -1 means an unsupported header
 * chain; 0 is invalid and 1 is a complete valid transport checksum. */
static inline int awdl_wire_checksum(const uint8_t *ip,size_t n)
{
    if(!ip || n<40 || (ip[0]>>4)!=6 || n-40!=((size_t)ip[4]<<8|ip[5]))return 0;
    size_t at=40;unsigned next=ip[6];
    for(unsigned headers=0;headers<8 && (next==0 || next==60);headers++) {
        if(n-at<8)return 0;
        size_t bytes=8u*((size_t)ip[at+1]+1);
        if(bytes>n-at)return 0;
        next=ip[at];at+=bytes;
    }
    if(next!=6 && next!=17 && next!=58)return -1;
    size_t bytes=n-at;
    if((next==17 && (bytes<8 || ((size_t)ip[at+4]<<8|ip[at+5])!=bytes || !(ip[at+6]|ip[at+7]))) ||
       (next==6 && bytes<20) || (next==58 && bytes<4))return 0;
    uint32_t sum=(uint32_t)(bytes>>16)+(uint32_t)(bytes&65535)+next;
    for(unsigned i=8;i<40;i+=2)sum+=((uint16_t)ip[i]<<8)|ip[i+1];
    for(size_t i=0;i<bytes;i+=2)sum+=((uint16_t)ip[at+i]<<8)|(i+1<bytes?ip[at+i+1]:0);
    while(sum>>16)sum=(sum&65535)+(sum>>16);
    return sum==65535;
}
#endif
