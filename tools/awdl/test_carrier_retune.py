"""Check the actual owner retune decision before any hardware reconfiguration."""
import subprocess,tempfile
from pathlib import Path
HERE=Path(__file__).resolve().parent
code=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define LX_AWDL_PEER_TTL_US 1000000ull
struct lx_awdl_peer {uint64_t serial,last_rx_us;struct {bool from_peer;uint64_t valid_until_us;} clock;unsigned desired;};
static struct {bool active,prepeer;unsigned active_slot,wanted,channel;uint64_t active_serial,wanted_serial;struct {struct lx_awdl_peer peers[8];}table;} air;
static uint64_t now=2000000;
static uint64_t lx_os_nanoseconds(void){return now*1000;}
static void lx_os_lock(void){}
static void lx_os_unlock(void){}
static void lx_tell(const char *format,...){(void)format;}
static unsigned auto_phy_chan(const struct lx_awdl_peer *p,uint64_t t){(void)t;return p->desired;}
'''+(HERE/'carrier_retune.inc').read_text()+r'''
static void reset(void){
 air.active=true;air.prepeer=false;air.active_slot=2;air.active_serial=7;air.channel=48;air.wanted_serial=0;
 air.table.peers[2]=(struct lx_awdl_peer){.serial=7,.last_rx_us=now-100,.clock={true,now+100000},.desired=149};
}
int main(void){
 reset();assert(auto_follow_channel());assert(air.wanted==2 && air.wanted_serial==7);
 /* Selection is deferred to the ordinary owner; never relabel the old
  * channel while hardware/DMA still belongs to its current carrier. */
 assert(air.channel==48);air.channel=149;assert(!auto_follow_channel());
 reset();air.wanted_serial=9;assert(!auto_follow_channel());assert(air.wanted_serial==9);
 reset();air.table.peers[2].serial=8;assert(!auto_follow_channel());assert(!air.wanted_serial);
 reset();air.table.peers[2].clock.from_peer=false;assert(!auto_follow_channel());
 reset();air.table.peers[2].clock.valid_until_us=now;assert(!auto_follow_channel());
 reset();air.table.peers[2].last_rx_us=now+1;assert(!auto_follow_channel());
 reset();air.table.peers[2].last_rx_us=now-LX_AWDL_PEER_TTL_US;assert(!auto_follow_channel());
 reset();air.table.peers[2].desired=0;assert(!auto_follow_channel());
 reset();air.prepeer=true;assert(!auto_follow_channel());
 reset();air.active=false;assert(!auto_follow_channel());
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='awdl-carrier-retune-') as folder:
    c=Path(folder)/'test.c';binary=Path(folder)/'test';c.write_text(code)
    subprocess.run(['clang','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS actual retune decision: preserve active hardware channel; request same live identity; respect competing peer and stale-clock rejection')
