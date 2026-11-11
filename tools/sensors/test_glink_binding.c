#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <stdarg.h>
#include "slpi_smem_ro.h"
#define SLPI_LINK_READ_BARRIER() __atomic_thread_fence(__ATOMIC_ACQUIRE)
#define os_atomic_load(p,o) (*(p))
#ifndef __offsetof
#define __offsetof(s,m) __builtin_offsetof(s,m)
#endif
#define SMEM_CANARY 0xa5a5
#define LOCK_OWNER 0
#define LOCK_US 1
#define IPCC_SEND 12
#define IPCC_ENABLE 20
#define IPCC_DISABLE 24
#define IPCC_SLPI 4
#define IPCC_GLINK 0
#define DSP_FATAL 1
struct smem_partition {uint8_t magic[4];uint16_t host0,host1;uint32_t size,free_uncached,free_cached,reserved[3];};
struct smp2p {uint32_t state;};
static uint8_t memory[SLPI_SMEM_BYTES];static const volatile uint8_t*smem=memory;
static struct{uintptr_t base;}smem_lock={0x10000000},ipcc={0x20000000};
static int hk_smem_alloc_lock,mutex_depth,force_busy;static uint32_t owner;
static unsigned smem_writes,publications,zero_before_publish,doorbells_sent;static uint64_t clock_us,fatal_at,cancel_at;
static unsigned defer_rx_creation,created_after_version;
static void maybe_create_remote(void);
static bool boot_cancelled;static struct{unsigned owned_running,ready;}boot_once;
static char logs[32768];static unsigned loglen;
static void tell(const char*f,...){va_list a;va_start(a,f);int n=vsnprintf(logs+loglen,sizeof(logs)-loglen,f,a);va_end(a);if(n>0&&n<(int)(sizeof(logs)-loglen))loglen+=n;}
static void need(void*b){}
static void lck_mtx_lock(int*p){assert(!mutex_depth);mutex_depth=1;}
static void lck_mtx_unlock(int*p){assert(mutex_depth);mutex_depth=0;}
static void put16(uint32_t o,unsigned v){memory[o]=v;memory[o+1]=v>>8;}
static void put32(uint32_t o,uint32_t v){for(unsigned i=0;i<4;i++)memory[o+i]=v>>(8*i);}
static uint32_t smem_word(uint32_t o){assert(o<=sizeof(memory)-4);return slpi_u32(memory+o);}
static int smem_peek(uint32_t o,void*d,uint32_t n){assert(o<=sizeof(memory)&&n<=sizeof(memory)-o);memcpy(d,memory+o,n);return 1;}
static int ro_reader(void*c,uint32_t o,void*d,uint32_t n){return smem_peek(o,d,n);}
static void smem_poke(uint32_t o,uint32_t v){assert(o<=sizeof(memory)-4);smem_writes++;
 if(o==0x17f00c){uint32_t at=0x17f000+smem_word(o),bytes=smem_word(at+4);publications++;unsigned zero=1;for(unsigned i=0;i<bytes;i++)if(memory[at+16+i])zero=0;zero_before_publish+=zero;}
 put32(o,v);
}
static void wr(uintptr_t a,uint32_t v){if(a==smem_lock.base){owner=force_busy?2:v;return;}assert(a==ipcc.base+IPCC_SEND||a==ipcc.base+IPCC_ENABLE||a==ipcc.base+IPCC_DISABLE);if(a==ipcc.base+IPCC_SEND){doorbells_sent++;maybe_create_remote();}}
static uint32_t rd(uintptr_t a){assert(a==smem_lock.base);return force_busy?2:owner;}
static void settle(void){}
static uint64_t now_us(void){return clock_us;}
static void sleep_us(unsigned us){clock_us+=us;}
static void boot_wait_ms(void*c,unsigned ms){clock_us+=(uint64_t)ms*1000;if(cancel_at&&clock_us>=cancel_at)boot_cancelled=true;}
static int dsp_words(struct smp2p*w){w->state=fatal_at&&clock_us>=fatal_at?1:0;return 1;}
static uint32_t dsp_state(const struct smp2p*w){return w->state;}
static void dsp_listen(const struct smp2p*w){}
static void doorbells(void){}
#include "slpi_glink_probe.inc"
static void reset(void){memset(memory,0,sizeof(memory));memset(&sensor_link,0,sizeof(sensor_link));memset(logs,0,sizeof(logs));loglen=0;link_descriptor=link_tx=link_rx=link_tx_size=link_rx_size=0;link_irq_enabled=false;clock_us=fatal_at=cancel_at=0;defer_rx_creation=created_after_version=0;boot_cancelled=false;boot_once.owned_running=boot_once.ready=1;owner=force_busy=mutex_depth=0;smem_writes=publications=zero_before_publish=doorbells_sent=0;
 memcpy(memory+SLPI_SMEM_TOC,"$TOC",4);put32(SLPI_SMEM_TOC+4,1);put32(SLPI_SMEM_TOC+8,1);uint32_t t=SLPI_SMEM_TOC+32;put32(t,0x17f000);put32(t+4,0x30000);put16(t+12,0);put16(t+14,3);put32(t+16,32);memcpy(memory+0x17f000,"$PRT",4);put16(0x17f006,3);put32(0x17f008,0x30000);put32(0x17f00c,32);put32(0x17f010,0x30000);memset(memory+0x17f020,0xa5,0x30000-32);
}
static uint32_t remote_item(unsigned id,uint32_t bytes){uint32_t at=0x17f000+smem_word(0x17f00c);put16(at,0xa5a5);put16(at+2,id);put32(at+4,bytes);put32(at+8,0);put32(at+12,0);memset(memory+at+16,0,bytes);put32(0x17f00c,smem_word(0x17f00c)+16+bytes);return at+16;}
static void rings(void){assert(link_make(478,32));assert(link_make(479,16384));remote_item(480,1024);struct slpi_report r;assert(link_inventory(&r));uint32_t size;assert(link_peer_item(&r,3,&link_descriptor,&size));assert(link_peer_item(&r,4,&link_tx,&link_tx_size));assert(link_peer_item(&r,5,&link_rx,&link_rx_size));}
static unsigned append_at(uint32_t rxbase,uint32_t off,unsigned c,unsigned a,uint32_t v,const void*p,unsigned n){unsigned bytes=(8+n+7)&~7u;uint8_t*b=memory+rxbase+off;memset(b,0,bytes);sg16(b,c);sg16(b+2,a);sg32(b+4,v);if(n)memcpy(b+8,p,n);return off+bytes;}
static void rx_handshake(uint32_t rxbase){unsigned o=0;uint8_t intent[8];sg32(intent,512);sg32(intent+4,9);o=append_at(rxbase,o,0,1,1,0,0);o=append_at(rxbase,o,1,1,1,0,0);o=append_at(rxbase,o,2,7,7,"IPCRTR",7);o=append_at(rxbase,o,4,1,0,0,0);o=append_at(rxbase,o,5,7,1,intent,8);uint8_t body[60]={0};sg32(body,52);sg32(body+8,1);sg32(body+12,2);sg32(body+16,9);sg32(body+20,SG_CTRL);sg32(body+28,20);sg32(body+32,1);sg32(body+36,SG_CTRL);sg32(body+40,2);o=append_at(rxbase,o,9,7,1,body,60);put32(link_descriptor+12,o);}
static void handshake(void){rings();rx_handshake(link_rx);}
static void maybe_create_remote(void){
 if(!defer_rx_creation||created_after_version||!link_tx)return;
 if(smem_word(link_descriptor+4)>=8 && slpi_u16(memory+link_tx)==0 && slpi_u16(memory+link_tx+2)==1){
  uint32_t rx=remote_item(480,1024);created_after_version=1;rx_handshake(rx);
 }
}
int main(void){reset();assert(link_make(478,32));assert(publications==1&&zero_before_publish==1);struct slpi_report r;assert(link_inventory(&r));uint32_t off,n;assert(link_peer_item(&r,3,&off,&n));memset(memory+off,0x77,32);unsigned writes=smem_writes;assert(link_make(478,32)&&smem_writes==writes);assert(memory[off]==0x77);assert(!link_make(478,64));
 for(unsigned foreign=0;foreign<2;foreign++){reset();put32(SLPI_SMEM_TOC+8,2);uint32_t t=SLPI_SMEM_TOC+32+48;put32(t,0x1000);put32(t+4,0x1000);put16(t+12,foreign?0:0xfffe);put16(t+14,foreign?2:0xfffe);put32(t+16,32);memcpy(memory+0x1000,"$PRT",4);put16(0x1004,foreign?0:0xfffe);put16(0x1006,foreign?2:0xfffe);put32(0x1008,0x1000);put32(0x100c,80);put32(0x1010,0x1000);put16(0x1020,0xa5a5);put16(0x1022,478);put32(0x1024,32);memset(memory+0x1030,0x66,32);assert(link_make(478,32));assert(link_inventory(&r)&&link_peer_item(&r,3,&off,&n)&&off!=0x1030);assert(memory[0x1030]==0x66);}
 reset();force_busy=1;assert(!link_make(478,32)&&!smem_writes&&!mutex_depth&&clock_us==1000000);reset();boot_cancelled=1;assert(!link_make(478,32)&&!smem_writes&&!mutex_depth);reset();put32(0x17f010,48);assert(!link_make(479,16384)&&!smem_writes);
 reset();rings();put32(link_descriptor+4,16376);put32(link_descriptor,64);uint8_t b[16];for(unsigned i=0;i<16;i++)b[i]=i+1;writes=smem_writes;assert(link_emit(0,b,16));assert(smem_word(link_descriptor+4)==8&&smem_word(link_descriptor)==64&&smem_writes==writes+1);assert(!memcmp(memory+link_tx+16376,b,8)&&!memcmp(memory+link_tx,b+8,8));put32(link_descriptor+4,16384);writes=smem_writes;assert(!link_emit(0,b,16)&&smem_writes==writes);
 reset();handshake();fatal_at=10000;assert(!slpi_glink_probe(0)&&sensor_link.open&&sensor_link.hello_received&&sensor_link.broken&&!link_irq_enabled);assert(strstr(logs,"DSP FATAL"));
 reset();handshake();cancel_at=10000;assert(!slpi_glink_probe(0)&&sensor_link.open&&sensor_link.hello_received&&!link_irq_enabled);assert(strstr(logs,"cancelled=1"));
 reset();handshake();assert(slpi_glink_probe(0)&&sensor_link.hello_received&&!link_irq_enabled&&clock_us==10000000);
 reset();defer_rx_creation=1;assert(slpi_glink_probe(0)&&created_after_version&&sensor_link.hello_received&&!link_irq_enabled);assert(sensor_link.sent>1);
 reset();assert(!slpi_glink_probe(0)&&clock_us==3000000&&link_rx==0&&sensor_link.sent==1&&!link_irq_enabled); /* no480: oneVERSION, bounded wait, noRXbytes */
 puts("PASS actual bindings: zero-before-publish/existing untouched/lock timeout/cancel/bounds/wrappedTX/invalidindex/fatal/cancel/success");}
