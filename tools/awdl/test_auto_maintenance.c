/* Exercise the actual owner maintenance against a kernel-like route set:
 * a second withdrawal returns ENOENT; accepted absence is not retried. */
#include "peer_table.h"
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define EG(f) (f)
#define EA(f) ((f)++)
enum {HK_AWDL_PEER_OVER_AIR_VERIFIED=2,HK_AWDL_PEER_OVER_AIR_L2=4};
static struct {
    struct lx_awdl_peer_table table;
    uint64_t last_maint_us,lease_seen,active_serial;
    unsigned active,expired;
    struct {uint64_t serial;unsigned l2,ipv6;uint8_t mac[6],ip[16];} notice[8];
} air;
static struct {uint64_t generation,lease;unsigned accept;} live;
static uint64_t tick;
static unsigned kernel_sources,absences,ip_absences,fail_once;
static const uint8_t mac[6]={2,0,0,0,0,9},ip[16]={0xfe,0x80,0,0,0,0,0,0,0,0,0,0,0,0,0,9};
static void lx_os_lock(void) {}
static void lx_os_unlock(void) {}
static uint64_t lx_os_nanoseconds(void) {return tick*1000;}
static void lx_tell(const char *s,...) {(void)s;}
static int lx_os_awdl_peer_absent(uint64_t g,uint64_t lease,unsigned source,const uint8_t *address,const uint8_t *v6)
{
    assert(g==42 && lease==7 && !memcmp(address,mac,6));
    assert(source==4?!v6:(v6 && !memcmp(v6,ip,16)));
    absences++;ip_absences+=source==2;
    if(fail_once==source) {fail_once=0;return EBUSY;}
    if(!(kernel_sources&source))return ENOENT;
    kernel_sources&=~source;return 0;
}
static int lx_os_awdl_peer_present(uint64_t g,uint64_t lease,unsigned source,const uint8_t *address,const uint8_t *v6)
{
    assert(g==42 && lease==7 && !memcmp(address,mac,6));(void)v6;
    kernel_sources|=source;return 0;
}
#include "auto_maintenance.inc"

static void seed(void)
{
    memset(&air,0,sizeof(air));live.generation=42;live.lease=7;live.accept=1;
    tick=100000+LX_AWDL_PEER_TTL_US;air.lease_seen=7;
    struct lx_awdl_peer *p=air.table.peers;
    p->serial=1;p->last_rx_us=100000;p->has_observed_ipv6=true;
    memcpy(p->mac,mac,6);memcpy(p->data_mac,mac,6);memcpy(p->observed_ipv6,ip,16);
    air.notice[0].serial=1;air.notice[0].l2=air.notice[0].ipv6=1;
    memcpy(air.notice[0].mac,mac,6);memcpy(air.notice[0].ip,ip,16);
    kernel_sources=6;absences=ip_absences=fail_once=0;
}
int main(void)
{
    seed();air.active=1;air.active_serial=1;
    auto_maintain();
    assert(!kernel_sources && absences==2 && !air.notice[0].l2 && !air.notice[0].ipv6);
    assert(air.table.peers[0].serial==1);
    air.active=0;tick+=25000;auto_maintain();
    assert(!air.table.peers[0].serial && air.expired==1 && absences==2);

    seed();fail_once=4;auto_maintain();
    assert(air.table.peers[0].serial && !air.notice[0].ipv6 && air.notice[0].l2 && kernel_sources==4);
    tick+=25000;auto_maintain();
    assert(!air.table.peers[0].serial && absences==3 && ip_absences==1 && !kernel_sources);

    seed();kernel_sources=0;auto_maintain();
    assert(!air.table.peers[0].serial && air.expired==1 && absences==2);

    seed();air.table.peers[1]=air.table.peers[0];air.table.peers[1].serial=2;
    air.table.peers[1].last_rx_us=tick;
    air.notice[1]=air.notice[0];air.notice[1].serial=2;
    auto_maintain();
    assert(!air.table.peers[0].serial && air.table.peers[1].serial==2 && kernel_sources==6 && !absences);
    tick+=LX_AWDL_PEER_TTL_US;auto_maintain();
    assert(!air.table.peers[1].serial && absences==2 && !kernel_sources);
    puts("PASS acknowledged withdrawal, active retirement, partial retry and shared-route ownership");
}
