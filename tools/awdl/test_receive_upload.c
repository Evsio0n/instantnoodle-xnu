#include <stdbool.h>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define LX_AWDL_PEER_SLOTS 8
static struct {uint64_t lease;} live={42};
static struct {struct {struct {uint64_t serial;} peers[8];} table;} air;
static unsigned live_wire_u32(const uint8_t *p){
    return ((unsigned)p[0]<<24)|((unsigned)p[1]<<16)|((unsigned)p[2]<<8)|p[3];
}
#include "receive_upload.inc"
int main(void){
    uint8_t ip[80]={[5]=40,[6]=6,[40]=32,[42]=0x22,[43]=0x42,[52]=0x50,[53]=16};
    air.table.peers[2].serial=7;
    receive_note(2,ip,79,100); assert(receive_choice(100)<0);
    receive_note(2,ip,80,100); assert(receive_choice(100)==2);
    receive_note(2,ip,80,900000); assert(receive_choice(2000100)<0);
    ip[47]++; receive_note(2,ip,80,3000000); assert(receive_choice(3000000)==2);
    live.lease++;assert(receive_choice(3000001)<0);
    receive_note(2,ip,80,3000002);air.table.peers[2].serial++;assert(receive_choice(3000003)<0);
    ip[5]=20;receive_note(2,ip,80,3000004);assert(receive_choice(3000004)<0);
    ip[53]=2;receive_note(2,ip,80,3000005);assert(receive_choice(3000005)==2);
    uint8_t bulk[1060]={[4]=3,[5]=252,[6]=6,[40]=40,[42]=0x22,[43]=0x42,[52]=0x50,[53]=16};
    for(unsigned i=0;i<70;i++){
        unsigned seq=1000*i;
        bulk[44]=seq>>24;bulk[45]=seq>>16;bulk[46]=seq>>8;bulk[47]=seq;
        receive_note(2,bulk,sizeof(bulk),4000000+i);
    }
    air.table.peers[1].serial=33;
    receive_note(1,ip,80,5000000);
    assert(receive_choice(5000000)==2);
    receive_note(2,ip,80,6000000);
    assert(receive_peer[2].port==40*256 && receive_choice(14000000)==2);
    uint64_t deadline=receive_peer[2].until;
    receive_note(2,bulk,sizeof(bulk),15000000);assert(receive_peer[2].until==deadline);
    assert(receive_choice(deadline)<0);
    bulk[53]=17;receive_note(2,bulk,sizeof(bulk),deadline);assert(receive_peer[2].until==0);
    return 0;
}
