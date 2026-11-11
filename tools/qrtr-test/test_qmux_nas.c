/* Exercise the maintained MHI+QRTR adapter end to end. Test input contains no
 * subscriber identities. These are model frames, not captures from the phone. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/hk_qrtr_mhi.h>
#include <sys/hk_qrtr_wire.h>
#include <sys/hk_qmux_nas.h>

struct bus { uint8_t frame[160]; size_t size; int fail; };
struct sink { uint8_t data[128]; size_t size; unsigned count; };
static int transmit(void *ctx,const void *packet,size_t n) {
    struct bus *b=ctx;
    assert(n<=sizeof(b->frame));memcpy(b->frame,packet,n);b->size=n;return b->fail;
}
static void collect(void *ctx,const uint8_t head[6],const uint8_t *data,size_t n) {
    struct sink *s=ctx;assert(n+6<=sizeof(s->data));
    memcpy(s->data,head,6);memcpy(s->data+6,data,n);s->size=n+6;s->count++;
}
static uint32_t word(const uint8_t *bytes,unsigned i) {
    uint32_t n;memcpy(&n,bytes+i*4,4);return n;
}
static void inject(struct hk_qrtr_mhi *m,uint32_t type,uint32_t node,uint32_t peer,uint32_t local,const void *data,size_t n) {
    uint8_t p[180]={0};uint32_t h[]={1,type,node,peer,0,(uint32_t)n,1,local};
    assert(n<=sizeof(p)-sizeof(h));memcpy(p,h,sizeof(h));memcpy(p+sizeof(h),data,n);
    assert(!hk_qrtr_mhi_input(m,p,sizeof(h)+((n+3)&~3u)));
}
static int allocate(struct hk_qmux_nas *q,struct sink *s) {
    const uint8_t request[]={1,15,0,0,0,0,0,7,0x22,0,4,0,1,1,0,3};
    unsigned before=s->count;
    int rc=hk_qmux_nas_write(q,request,sizeof(request));
    if(rc)return -rc;
    assert(s->count==before+1 && s->size==24 && s->data[3]==0x80 && s->data[4]==0 &&
           s->data[6]==1 && s->data[7]==7 && s->data[8]==0x22 && s->data[22]==3);
    return s->data[23];
}
static int release(struct hk_qmux_nas *q,uint8_t id) {
    uint8_t request[]={1,16,0,0,0,0,0,8,0x23,0,5,0,1,2,0,3,id};
    return hk_qmux_nas_write(q,request,sizeof(request));
}
int main(void) {
    struct hk_qrtr_mhi m;struct bus bus={0};struct hk_qmux_nas chan={0},other={0};struct sink a={0},b={0};
    hk_qrtr_mhi_init(&m,1,transmit,&bus);hk_qrtr_mhi_set_active(&m);
    assert(hk_qrtr_mhi_nas_open(&chan,collect,&a)==ENOTCONN);
    hk_qrtr_mhi_set_up(&m,true);
    assert(!hk_qrtr_mhi_nas_open(&chan,collect,&a) && !hk_qrtr_mhi_nas_open(&other,collect,&b));
    assert(hk_qrtr_mhi_nas_open(&chan,collect,&a)==EBUSY);
    int c1=allocate(&chan,&a),c2=allocate(&chan,&a),c3=allocate(&other,&b);
    assert(c1==1 && c2==2 && c3==1);
    uint32_t p1=hk_qmux_nas_local_port(&chan,c1),p2=hk_qmux_nas_local_port(&chan,c2),p3=hk_qmux_nas_local_port(&other,c3);
    assert(p1 && p2 && p3 && p1!=p2 && p2!=p3 && p1!=p3);
    uint8_t request[]={1,12,0,0,3,1,0,42,0,0x24,0,0,0};
    assert(hk_qmux_nas_write(&chan,request,sizeof(request))==EAGAIN);
    const uint32_t ctl=HK_QRTR_WIRE_PORT_CONTROL;
    uint32_t svc[]={HK_QRTR_WIRE_NEW_SERVER,3,1,3,58};
    inject(&m,HK_QRTR_WIRE_NEW_SERVER,3,ctl,ctl,svc,sizeof(svc));
    /* The only new operation is a client-scoped, single boolean TLV on NAS.
     * Unrecognized events, values and trailing TLVs never reach the wire. */
    uint8_t reg[]={1,16,0,0,3,1,0,71,0,3,0,4,0,0x13,1,0,0};
    assert(hk_qmux_nas_write(&chan,reg,sizeof(reg))==EALREADY);
    reg[16]=2;assert(hk_qmux_nas_write(&chan,reg,sizeof(reg))==EPROTO);
    reg[16]=1;reg[13]=0x10;assert(hk_qmux_nas_write(&chan,reg,sizeof(reg))==EPROTO);
    reg[13]=0x13;reg[14]=2;assert(hk_qmux_nas_write(&chan,reg,sizeof(reg))==EPROTO);
    reg[14]=1;reg[4]=5;assert(hk_qmux_nas_write(&chan,reg,sizeof(reg))==ENOTSUP);
    reg[4]=3;
    uint8_t extra[sizeof(reg)+4];memcpy(extra,reg,sizeof(reg));extra[1]+=4;extra[11]+=4;
    memcpy(extra+sizeof(reg),reg+13,4);
    assert(hk_qmux_nas_write(&chan,extra,sizeof(extra))==EPROTO);
    request[9]=0x25;assert(hk_qmux_nas_write(&chan,request,sizeof(request))==ENOTSUP);request[9]=0x24;
    request[1]--;assert(hk_qmux_nas_write(&chan,request,sizeof(request))==EPROTO);request[1]++;
    bus.fail=EIO;assert(hk_qmux_nas_write(&chan,request,sizeof(request))==EIO);bus.fail=0;
    assert(!hk_qmux_nas_write(&chan,request,sizeof(request)) && word(bus.frame,3)==p1 && word(bus.frame,7)==58);
    assert(!memcmp(bus.frame+32,request+6,7));
    assert(hk_qmux_nas_write(&chan,request,sizeof(request))==EBUSY);
    request[5]=2;assert(!hk_qmux_nas_write(&chan,request,sizeof(request)) && word(bus.frame,3)==p2);
    request[5]=1;assert(!hk_qmux_nas_write(&other,request,sizeof(request)) && word(bus.frame,3)==p3);
    uint8_t response[]={2,42,0,0x24,0,7,0,2,4,0,0,0,0,0};
    uint8_t indication[]={4,0,0,0x24,0,8,0,1,5,0,0,2,2,0,0};
    unsigned ac=a.count,bc=b.count;
    inject(&m,1,9,58,p1,response,sizeof(response));inject(&m,1,3,57,p1,response,sizeof(response));
    response[1]++;inject(&m,1,3,58,p1,response,sizeof(response));response[1]--;
    response[8]++;inject(&m,1,3,58,p1,response,sizeof(response));response[8]--;
    assert(a.count==ac && b.count==bc && chan.slots[0].pending);
    inject(&m,1,3,58,p2,indication,sizeof(indication));assert(a.count==ac+1 && a.data[5]==2 && chan.slots[1].pending && b.count==bc);
    indication[14]=1;inject(&m,1,3,58,p2,indication,sizeof(indication));assert(a.count==ac+1);
    inject(&m,1,3,58,p3,response,sizeof(response));assert(b.count==bc+1 && a.count==ac+1 && b.data[5]==1);
    response[10]=1;response[12]=52;inject(&m,1,3,58,p2,response,sizeof(response));
    assert(a.data[5]==2 && !memcmp(a.data+6,response,sizeof(response)) && chan.slots[0].pending);
    inject(&m,1,3,58,p1,response,sizeof(response));assert(a.data[5]==1 && !chan.slots[0].pending);
    assert(allocate(&chan,&a)==3 && allocate(&chan,&a)==4 && allocate(&chan,&a)==-ENOSPC);
    hk_qrtr_mhi_set_up(&m,false);assert(hk_qmux_nas_write(&chan,request,sizeof(request))==ENOTCONN);
    hk_qrtr_mhi_set_up(&m,true);inject(&m,HK_QRTR_WIRE_NEW_SERVER,3,ctl,ctl,svc,sizeof(svc));
    assert(hk_qmux_nas_write(&chan,request,sizeof(request))==ENOTCONN);
    assert(!release(&chan,1) && release(&chan,1)==ENOENT && allocate(&chan,&a)==1);
    uint32_t fresh=hk_qmux_nas_local_port(&chan,1);assert(fresh && fresh!=p1);
    hk_qrtr_mhi_fresh(&m);assert(!hk_qmux_nas_write(&chan,request,sizeof(request)));
    ac=a.count;inject(&m,1,3,58,p1,response,sizeof(response));assert(a.count==ac);
    inject(&m,1,3,58,fresh,response,sizeof(response));assert(a.count==ac+1);
    /* No peer reset: owner times out an outstanding request, releases the CID,
     * then issues the identical transaction on the newly allocated local port. */
    assert(!hk_qmux_nas_write(&chan,request,sizeof(request)) && chan.slots[0].pending);
    uint32_t expired=fresh;
    assert(!release(&chan,1) && allocate(&chan,&a)==1);hk_qrtr_mhi_fresh(&m);
    fresh=hk_qmux_nas_local_port(&chan,1);assert(fresh && fresh!=expired);
    assert(!hk_qmux_nas_write(&chan,request,sizeof(request)));
    ac=a.count;inject(&m,1,3,58,expired,response,sizeof(response));
    assert(a.count==ac && chan.slots[0].pending);
    inject(&m,1,3,58,fresh,response,sizeof(response));assert(a.count==ac+1 && !chan.slots[0].pending);
    hk_qmux_nas_close(&chan);hk_qmux_nas_close(&other);
    for(unsigned i=0;i<HK_QRTR_CORE_MAX_PORTS;i++) assert(!m.core.ports[i].number);
    puts("PASS MHI/QRTR QMUX CID and local-port allocation, same-TID isolation, peer filtering, indication ownership, error passthrough, exhaustion, release and link reset");
}
