#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gnss_fix.h"
#define UTC UINT64_C(1800000000000)
struct message {uint8_t buf[160];size_t n;};
static void put(struct message *m,uint8_t type,uint64_t v,unsigned bytes)
{
    assert(m->n+bytes+3<=sizeof(m->buf));m->buf[m->n++]=type;m->buf[m->n++]=bytes;m->buf[m->n++]=0;
    for(unsigned i=0;i<bytes;i++)m->buf[m->n++]=(uint8_t)(v>>(8*i));
    m->buf[5]=(uint8_t)(m->n-7);m->buf[6]=0;
}
static uint64_t d(double x){uint64_t bits;memcpy(&bits,&x,sizeof(bits));return bits;}
static uint32_t f(float x){uint32_t bits;memcpy(&bits,&x,sizeof(bits));return bits;}
enum fields {STAT=1,SID=2,LAT=4,LON=8,UNC=16,TECH=32,TIME=64,ALL=127};
static struct message packet(unsigned fields,unsigned status,unsigned session,double lat,double lon,float acc,unsigned tech,uint64_t stamp)
{
    struct message m={{4,0,0,0x24,0,0,0},7};
    if(fields&STAT)put(&m,1,status,4);if(fields&SID)put(&m,2,session,1);
    if(fields&LAT)put(&m,0x10,d(lat),8);if(fields&LON)put(&m,0x11,d(lon),8);if(fields&UNC)put(&m,0x12,f(acc),4);
    if(fields&TECH)put(&m,0x23,tech,4);if(fields&TIME)put(&m,0x25,stamp,8);return m;
}
static struct hk_fix_session ready(void)
{
    struct hk_fix_session s={0};hk_fix_start(&s,7,90,401,3);return s;
}
static unsigned inspect(struct message m)
{
    struct hk_fix_session s=ready();hk_fix_observe(&s,m.buf,m.n,1000,UTC);return s.state;
}
int main(void)
{
    struct message good=packet(ALL,0,3,0,0,0,1,UTC);
    assert(inspect(good)==HK_FIX_READY);
    assert(inspect(packet(ALL,0,3,-90,180,2,1,UTC))==HK_FIX_READY);
    assert(inspect(packet(ALL,0,3,90,-180,4,1,UTC))==HK_FIX_READY);
    for(unsigned missing=STAT;missing<=TIME;missing<<=1){
        unsigned expected=missing==TECH?HK_FIX_SOURCE_UNKNOWN:HK_FIX_MISSING_FIELDS;
        assert(inspect(packet(ALL&~missing,0,3,0,0,1,1,UTC))==expected);
    }
    assert(inspect(packet(ALL,1,3,0,0,1,1,UTC))==HK_FIX_PROGRESS);
    assert(inspect(packet(ALL,4,3,0,0,1,1,UTC))==HK_FIX_MODEM_ERROR);
    assert(inspect(packet(ALL,0,3,91,0,1,1,UTC))==HK_FIX_BAD_REPORT);
    assert(inspect(packet(ALL,0,3,0,-181,1,1,UTC))==HK_FIX_BAD_REPORT);
    assert(inspect(packet(ALL,0,3,NAN,0,1,1,UTC))==HK_FIX_BAD_REPORT);
    assert(inspect(packet(ALL,0,3,0,0,INFINITY,1,UTC))==HK_FIX_BAD_REPORT);
    assert(inspect(packet(ALL,0,3,0,0,-1,1,UTC))==HK_FIX_BAD_REPORT);
    assert(inspect(packet(ALL,0,3,0,0,-0.0,1,UTC))==HK_FIX_READY);
    for(unsigned used=16;used<=32;used+=16)assert(inspect(packet(ALL,0,3,0,0,1,used|1,UTC))==HK_FIX_SOURCE_UNKNOWN);
    assert(inspect(packet(ALL,0,3,0,0,1,2,UTC))==HK_FIX_SOURCE_UNKNOWN);
    assert(inspect(packet(ALL,0,3,0,0,1,1,UTC-15001))==HK_FIX_OLD);
    assert(inspect(packet(ALL,0,3,0,0,1,1,UTC+2001))==HK_FIX_OLD);
    assert(inspect(packet(ALL,0,3,0,0,1,1,0))==HK_FIX_CLOCK_UNKNOWN);
    struct message bad=good;put(&bad,0x10,0,8);assert(inspect(bad)==HK_FIX_BAD_REPORT);
    bad=good;bad.n--;assert(inspect(bad)==HK_FIX_BAD_REPORT);
    struct hk_fix_session s=ready();assert(hk_fix_matches(&s,7,90,401)&&!hk_fix_matches(&s,7,90,402)&&!hk_fix_matches(&s,7,91,401));
    hk_fix_observe(&s,good.buf,good.n,1000,UTC+10000);assert(s.state==HK_FIX_READY&&s.accepted==1);
    hk_fix_observe(&s,good.buf,good.n,1001,UTC+10001);assert(s.accepted==1&&s.value.observed_ms==1000);
    struct message other=packet(ALL,1,6,0,0,3,1,UTC);hk_fix_observe(&s,other.buf,other.n,1001,UTC);assert(s.state==HK_FIX_READY);
    hk_fix_expire(&s,6000);assert(s.state==HK_FIX_READY);hk_fix_expire(&s,6001);assert(s.state==HK_FIX_OLD&&!s.value.utc_ms);
    hk_fix_observe(&s,good.buf,good.n,7000,UTC);assert(s.state==HK_FIX_OLD&&s.accepted==1);
    hk_fix_end(&s);assert(!s.active&&!s.value.utc_ms&&!s.last_utc_ms&&!s.accepted&&!hk_fix_matches(&s,7,90,401));
    puts("PASS genuine-source/accepted-session/UTC/range/expiry/duplicate validation and volatile teardown");
}
