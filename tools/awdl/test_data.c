#include "../../wifi/lx/src/lx_awdl_data.h"
#include <assert.h>
#include <string.h>

int main(void)
{
	uint8_t wire[16 + 48] = {0};
	uint8_t *ip = wire + 16;
	ip[0] = 0x60; ip[5] = 8; ip[6] = 59; ip[7] = 64;
	struct lx_awdl_ipv6 packet;
	assert(lx_awdl_ipv6_encap(wire,16,0x2356,ip,48)==16);
	assert(lx_awdl_ipv6_decap(wire,sizeof(wire),&packet));
	assert(packet.sequence==0x2356 && packet.packet==ip && packet.length==48);
	for (size_t n=0;n<sizeof(wire);n++) assert(!lx_awdl_ipv6_decap(wire,n,&packet));
	wire[6]=0x86;
	assert(!lx_awdl_ipv6_decap(wire,sizeof(wire),&packet));
	wire[6]=8;
	uint8_t tagged[sizeof(wire)+7];
	memcpy(tagged,wire,12);
	const uint8_t tags[]={3,0, 5,3,0,0,0, 3,0};
	memcpy(tagged+12,tags,sizeof(tags));
	memcpy(tagged+12+sizeof(tags),wire+14,sizeof(wire)-14);
	assert(lx_awdl_ipv6_decap(tagged,sizeof(tagged),&packet));
	assert(!memcmp(packet.packet,ip,48));
	tagged[15]=0xff;
	assert(!lx_awdl_ipv6_decap(tagged,sizeof(tagged),&packet));
	assert(!lx_awdl_ipv6_encap(wire,15,0,ip,48));
	return 0;
}
