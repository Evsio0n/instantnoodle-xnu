#include "wire_checksum.h"
#include "echo6.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    /* Independently assembled IPv6 UDP vector, payload "test". */
    uint8_t p[52]={0x60,0,0,0,0,12,17,255,
        0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,1,
        0xff,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0xfb,
        0x14,0xe9,0x14,0xe9,0,12,0xef,0xaa,'t','e','s','t'};
    assert(awdl_wire_checksum(p,sizeof(p))==1);
    p[51]^=1;assert(awdl_wire_checksum(p,sizeof(p))==0);p[51]^=1;
    p[46]=p[47]=0;assert(awdl_wire_checksum(p,sizeof(p))==0);p[46]=0xef;p[47]=0xaa;
    uint8_t options[60]={0};memcpy(options,p,40);options[5]=20;options[6]=0;options[40]=17;
    memcpy(options+48,p+40,12);assert(awdl_wire_checksum(options,sizeof(options))==1);
    options[41]=255;assert(awdl_wire_checksum(options,sizeof(options))==0);
    p[6]=44;assert(awdl_wire_checksum(p,sizeof(p))==-1);
    uint8_t echo[64],from[16]={0xfe,0x80},to[16]={0xfe,0x80},nonce[16]={1,2,3};
    from[15]=1;to[15]=2;
    assert(lx_echo_request(echo,sizeof(echo),from,to,nonce));
    assert(awdl_wire_checksum(echo,sizeof(echo))==1);
    echo[62]^=1;assert(awdl_wire_checksum(echo,sizeof(echo))==0);
    assert(awdl_wire_checksum(NULL,0)==0);
    puts("PASS native IPv6 checksum inspection, UDP, ICMPv6, options, corruption and bounds");
}
