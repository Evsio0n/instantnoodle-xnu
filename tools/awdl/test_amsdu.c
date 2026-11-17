#include "amsdu_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
 const uint8_t self[6]={2,0,0,0,0,1},peer[6]={2,0,0,0,0,2};
 uint8_t eth[82]={0x33,0x33,0,0,0,0xfb};memcpy(eth+6,self,6);eth[12]=0x86;eth[13]=0xdd;
 uint8_t *ip=eth+14;ip[0]=0x60;ip[5]=28;ip[6]=17;ip[7]=255;ip[8]=0xfe;ip[9]=0x80;ip[23]=1;ip[24]=0xff;ip[25]=2;ip[39]=0xfb;
 uint8_t wire[128];memset(wire,0xcc,sizeof(wire));uint8_t old[82];memcpy(old,eth,sizeof(old));
 assert(lx_awdl_amsdu_ipv6(wire,124,eth,sizeof(eth),self,peer,0xabc)==124);
 assert(wire[0]==0x88 && wire[1]==0 && wire[24]==0x80 && wire[25]==0 && wire[22]==0xc0 && wire[23]==0xab);
 assert(!memcmp(wire+4,peer,6)&&!memcmp(wire+10,self,6)&&!memcmp(wire+26,eth,6)&&!memcmp(wire+32,self,6));
 assert(wire[38]==0&&wire[39]==84&&wire[124]==0xcc&&!memcmp(old,eth,sizeof(old)));
 struct lx_awdl_ipv6 decoded;assert(lx_awdl_ipv6_decap(wire+40,84,&decoded));
 assert(decoded.packet==wire+56&&decoded.length==68&&decoded.sequence==0xabc&&!memcmp(decoded.packet,ip,68));
 uint8_t saved[128];memcpy(saved,wire,sizeof(saved));
 assert(!lx_awdl_amsdu_ipv6(wire,123,eth,sizeof(eth),self,peer,1));
 uint8_t group[6]={0x33,0x33,0,0,0,0xfb};assert(!lx_awdl_amsdu_ipv6(wire,128,eth,sizeof(eth),self,group,1));
 eth[38]=0xfe;assert(!lx_awdl_amsdu_ipv6(wire,128,eth,sizeof(eth),self,peer,1));assert(!memcmp(saved,wire,sizeof(saved)));
 puts("PASS single HT A-MSDU: physical receiver distinct from logical group DA; unchanged IPv6 bytes/sequence; length and rejection bounds");
}
