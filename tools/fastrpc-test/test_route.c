#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../xnu/files/bsd/dev/slpi_dma_route.h"

struct mock {
    uint32_t reg[0x87000/4];
    unsigned writes, drop_write, flushes, fail_flush;
    bool emulate_format, sticky_cbar, stuck;
};
static struct mock m;
static uint32_t initial[sizeof(m.reg)/4];
static uint32_t get(void *c,unsigned off){return ((struct mock *)c)->reg[off/4];}
static void put(void *c,unsigned off,uint32_t v){
    struct mock *s=c;
    if (++s->writes==s->drop_write) return;
    if(s->stuck && off==0x800 && v==0) return;
    if(off==0x86000 && v==0x1127)v=0x127;
    if(s->sticky_cbar && off==0x1018 && v==0)return;
    if(s->emulate_format && off==0x1018 && v==0 && get(c,0x1818)==1)return;
    s->reg[off/4]=v;
}
static bool flush(void *c){struct mock *s=c;return ++s->flushes!=s->fail_flush;}
static const struct slpi_route_io io={&m,get,put,flush};
static void reset(void){
    memset(&m,0,sizeof(m));m.reg[0]=0x2a0406;m.reg[0x20/4]=0x4c017e57;
    m.reg[0x24/4]=0x6000004a;m.reg[0xc00/4]=0x200ff;
    m.reg[0x86000/4]=0x1cdd6718;m.reg[0x86030/4]=0xaa01;
    memcpy(initial,m.reg,sizeof(initial));
}
static bool peer_still_active(void *unused)
{
    (void)unused;
    assert(get(&m,0x800)==0x80000541 && (get(&m,0x86000)&1));
    return false;
}
int main(void){
    reset();
    struct slpi_route_result r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(r.attempted&&r.mapped&&r.restored&&!memcmp(m.reg,initial,sizeof(initial)));
    unsigned writes=m.writes;
    reset();r=slpi_route_trial(&io,4,0x100200000ULL,false);
    assert(r.enabled&&!r.mapped&&r.restored&&!memcmp(m.reg,initial,sizeof(initial)));
    for(unsigned i=1;i<=writes;i++){
        reset();m.drop_write=i;r=slpi_route_trial(&io,4,0x100200000ULL,true);
        /* If a failed rollback leaves a changed register, caller must retain arena. */
        if(r.restored)assert(!memcmp(m.reg,initial,sizeof(initial)));
    }
    reset();m.emulate_format=true;m.reg[0x86058/4]=0x400;
    r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(r.mapped&&r.restored&&m.reg[0x86058/4]==0x400);
    reset();m.reg[0x86058/4]=0x402;r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(!r.attempted&&!m.writes);
    reset();m.sticky_cbar=true;r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(r.mapped&&r.reclaimable&&!r.restored&&r.rollback_at==0x1018);
    assert(get(&m,0x86020)==0 && get(&m,0x86024)==0 && !(get(&m,0x86000)&1) && !get(&m,0x800));
    reset();m.fail_flush=1;r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(!r.mapped&&r.restored&&!memcmp(m.reg,initial,sizeof(initial)));
    reset();m.fail_flush=2;r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(r.mapped&&!r.restored&&!r.reclaimable);
    reset();m.stuck=true;r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(r.mapped&&!r.restored&&!r.reclaimable);
    reset();m.reg[0x804/4]=0x80010540;r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(!r.attempted&&r.restored&&!m.writes);
    reset();r=slpi_route_trial(&io,6,0x100200000ULL,true);assert(!r.attempted&&!m.writes);
    reset();m.reg[0x86000/4]|=1;r=slpi_route_trial(&io,4,0x100200000ULL,true);
    assert(!r.attempted&&!m.writes);
    reset();r=slpi_route_run(&io,4,0x100200000ULL,true,peer_still_active,NULL);
    assert(r.mapped && !r.reclaimable && !r.restored && get(&m,0x800)==0x80000541);
    puts("PASS SMMU route/restore, failed readbacks, TLB timeouts, stuck route, busy bank and masked SID");
}
