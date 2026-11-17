#include "rx_boundary_audit.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    /* Exact-sized allocations exercise every short-header read with ASan.
     * A normal IPv4 Ethernet packet must not look like Apple vendor data. */
    for(size_t n=0;n<32;n++) {
        uint8_t *p=calloc(n?n:1,1);assert(p);
        if(n>0)p[0]=8;
        if(n>12)p[12]=8;
        if(n>14)p[14]=0x45;
        rx_boundary_begin(p,n,true);rx_boundary_begin(p,n,false);free(p);
    }
    assert(!rx_boundary.vendor_eth && !rx_boundary.length_vendor_eth);
    uint8_t ethernet[30]={0};ethernet[12]=8;ethernet[14]=3;ethernet[15]=4;
    assert(rx_boundary_begin(ethernet,sizeof(ethernet),true));assert(rx_boundary.vendor_eth==1);
    ethernet[12]=0;ethernet[13]=16;
    memcpy(ethernet+14,(uint8_t[]){0xaa,0xaa,3,0,0x17,0xf2,8,0},8);
    assert(rx_boundary_begin(ethernet,sizeof(ethernet),true));assert(rx_boundary.length_vendor_eth==1);
    uint8_t raw[24]={8};memcpy(raw+16,(uint8_t[]){0,0x25,0,0xff,0x94,0x73},6);
    assert(rx_boundary_begin(raw,sizeof(raw),false));assert(rx_boundary.raw_awdl==1);
    raw[1]=0x40;assert(rx_boundary_begin(raw,sizeof(raw),false));assert(rx_boundary.raw_awdl_bad_header==1);
    assert(!rx_boundary_begin((uint8_t[]){0xd0,0},2,false));
    for(unsigned i=0;i<RXA_DROP_COUNT;i++)assert(!rx_boundary_drop(i,true));
    assert(!rx_boundary_drop(RXA_DROP_COUNT,true));
    puts("PASS passive host-frame classification, short buffers, distinct vendor envelopes and unchanged false-return contract");
}
