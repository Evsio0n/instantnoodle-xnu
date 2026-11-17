#include "peer_table.h"
#include <assert.h>
#include <string.h>
static struct {struct lx_awdl_peer_table table; uint8_t local[6]; unsigned active_slot;} air;
#include "announcement_sync.inc"
int main(void){
    air.local[0]=2;
    for(unsigned i=0;i<2;i++){
        struct lx_awdl_peer *p=&air.table.peers[i];
        p->serial=i+1;p->seen_mif=true;p->version=1;p->device_class=2;
        p->clock.from_peer=true;p->clock.period_tu=16;p->clock.presence=4;
        p->mac[0]=p->master[0]=p->sync_master[0]=4+i*2;p->master_metric=100+i;
        p->last_rx_us=1;
    }
    assert(announcement_sync_locked(10)==1);
    air.active_slot=1;assert(announcement_sync_locked(10)==1);
    memcpy(air.table.peers[1].sync_master,air.local,6);
    assert(announcement_sync_locked(10)==0);
    assert(announcement_sync_locked(1+LX_AWDL_PEER_TTL_US)==-1);
    return 0;
}
