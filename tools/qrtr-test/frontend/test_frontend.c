/* Real worker, named leases and QRTR wire path. The host supplies only the
 * modem's reply bytes and an in-memory stand-in for the IOKit registry. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/hk_abp_nas_worker.h>
#include <sys/hk_qmux_frontend.h>
#include <sys/hk_qrtr_wire.h>
static struct hk_abp_nas_worker w;
static struct hk_qrtr_mhi m;
static struct hk_qmux_channel_info visible[2];
static pthread_t owner;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static unsigned sent,retired,deny_withdraw_once,deny_publish_once;
static uint64_t timer_ns,worker_gen;
static struct {uint32_t port;uint16_t txn;} requests[32];
static uint64_t cancel_gen,cancel_token;
static unsigned enables,disables;
uint64_t hk_qmux_frontend_test_time(void){return timer_ns;}
static void lock(void *arg){(void)arg;pthread_mutex_lock(&mutex);}
static void unlock(void *arg){(void)arg;pthread_mutex_unlock(&mutex);}
static bool on_owner(void *arg){(void)arg;return pthread_equal(owner,pthread_self())!=0;}
static const struct hk_abp_nas_worker_ops ops={lock,unlock,on_owner};
bool hk_qmux_registry_publish(const struct hk_qmux_channel_info *v){
    assert(on_owner(NULL) && !visible[v->index-1].generation && v->cid && v->local_port);
    if(v->index==1 && deny_publish_once){deny_publish_once--;return false;}
    visible[v->index-1]=*v;return true;
}
bool hk_qmux_registry_withdraw(unsigned i,uint64_t gen){
    assert(on_owner(NULL) && visible[i-1].generation==gen);
    if(deny_withdraw_once){deny_withdraw_once--;return false;}
    visible[i-1]=(struct hk_qmux_channel_info){0};retired++;return true;
}
static int send(void *ct,const void *buf,size_t n){
    (void)ct;const uint8_t *p=buf;
    if(n>=39 && p[4]==1 && p[32]==0){
        assert(on_owner(NULL) && p[36]==0 && sent<32);
        if (p[35] == 3) {
            assert(n==44 && p[37]==4 && !p[38] && p[39]==0x13 &&
                   p[40]==1 && p[41]==0 && p[42]<=1);
            if (p[42]) enables++; else disables++;
        } else assert(p[35]==0x24 && n==40);
        memcpy(&requests[sent].port,p+12,4);requests[sent].txn=(uint16_t)(p[33]|p[34]<<8);sent++;
    }
    return 0;
}
static void input_from(uint32_t type,uint32_t node,uint32_t srcport,uint32_t dstnode,uint32_t dstport,const void *bytes,size_t n) {
    uint32_t header[8]={1,type,node,srcport,0,(uint32_t)n,dstnode,dstport};uint8_t pkt[128]={0};
    assert(n<sizeof(pkt)-32);memcpy(pkt,header,32);memcpy(pkt+32,bytes,n);
    assert(!hk_abp_worker_rx(&w,worker_gen,pkt,32+((n+3)&~3u)));
}
static void input(uint32_t type,uint32_t srcport,uint32_t dstport,const void *bytes,size_t n) {
    input_from(type,3,srcport,1,dstport,bytes,n);
}
static void announce(bool up){
    uint32_t type=up?HK_QRTR_WIRE_NEW_SERVER:HK_QRTR_WIRE_DEL_SERVER;
    uint32_t ns[]={type,3,1,3,58};input(type,HK_QRTR_WIRE_PORT_CONTROL,HK_QRTR_WIRE_PORT_CONTROL,ns,sizeof(ns));
}
static void reply(uint32_t local,uint16_t txn,bool error) {
    uint8_t q[]={2,(uint8_t)txn,(uint8_t)(txn>>8),0x24,0,15,0,2,4,0,0,0,0,0,1,5,0,0,2,2,0,0};
    if(error){q[10]=1;q[12]=74;}
    input(1,58,local,q,sizeof(q));
}
static void tick(void){assert(!hk_abp_worker_poll(&w));hk_qmux_frontend_poll(&w);}
static void settle(void){for(unsigned i=0;i<5;i++)tick();}
/* Model wire bytes only. The source node, source port and addressed local
 * port all go through the ordinary QRTR worker/adapter. */
static void serving_events(struct hk_qmux_channel_info a, struct hk_qmux_channel_info b)
{
    struct hk_qmux_nas_event e = {0};
    struct hk_qmux_nas_answer r = {0};
    uint64_t token;
    unsigned initial_sent = sent;
    uint8_t ind[] = {
        4, 0x77, 0x76, 0x24, 0, 16, 0,
        1, 6, 0, 0, 2, 2, 0, 1, 8,         /* unregistered, detached */
        0xe0, 4, 0, 0xa5, 0xa5, 0xa5, 0xa5  /* optional, not exported */
    };
    assert(hk_qmux_frontend_read_event(a.index, a.generation, 0, &e) == EAGAIN);
    assert(!e.sequence && !e.channel.generation);
    input_from(1, 4, a.remote_port, 1, a.local_port, ind, sizeof(ind));
    input_from(1, a.node, a.remote_port, 9, a.local_port, ind, sizeof(ind));
    input(1, a.remote_port + 1, a.local_port, ind, sizeof(ind));
    input(1, a.remote_port, 0x12345678, ind, sizeof(ind));
    tick();
    assert(hk_qmux_frontend_read_event(a.index, a.generation, 0, &e) == EAGAIN);

    ++timer_ns;
    input(1, a.remote_port, a.local_port, ind, sizeof(ind));
    tick();
    assert(!hk_qmux_frontend_read_event(a.index, a.generation, 0, &e));
    assert(e.channel.generation == a.generation && e.channel.index == a.index &&
           e.channel.node == a.node && e.channel.remote_port == a.remote_port &&
           e.channel.local_port == a.local_port && e.channel.cid == a.cid &&
           e.sequence == 1 && e.received_ns == timer_ns &&
           !e.registration && e.cs_attach == 2 && e.ps_attach == 2);
    assert(hk_qmux_frontend_read_event(b.index, b.generation, 0, &e) == EAGAIN && !e.sequence);
    assert(hk_qmux_frontend_read_event(a.index, b.generation, 0, &e) == ESTALE);
    assert(sent == initial_sent);

    /* A malformed count, invalid enum, result TLV, duplicate serving TLV,
     * and wrong operation are all rejected. Silence creates no new event. */
    ind[14] = 2;
    input(1, a.remote_port, a.local_port, ind, sizeof(ind));
    ind[14] = 1;
    for (unsigned off = 10; off <= 12; ++off) {
        uint8_t saved = ind[off];
        ind[off] = 0xff;
        input(1, a.remote_port, a.local_port, ind, sizeof(ind));
        ind[off] = saved;
    }
    ind[16] = 2;
    input(1, a.remote_port, a.local_port, ind, sizeof(ind));
    ind[16] = 0xe0;
    uint8_t duplicate[sizeof(ind) + 9];
    memcpy(duplicate, ind, sizeof(ind));
    memcpy(duplicate + sizeof(ind), ind + 7, 9);
    duplicate[5] += 9;
    input(1, a.remote_port, a.local_port, duplicate, sizeof(duplicate));
    ind[3] = 0x25;
    input(1, a.remote_port, a.local_port, ind, sizeof(ind));
    ind[3] = 0x24;
    timer_ns += HK_QMUX_FRONTEND_TIMEOUT_NS;
    tick();
    assert(hk_qmux_frontend_read_event(a.index, a.generation, 1, &e) == EAGAIN && !e.sequence);
    assert(sent == initial_sent);

    /* Two readers can see the latest value independently. An intervening
     * indication with the pending request's TID cannot complete its token. */
    assert(!hk_qmux_frontend_request(a.index, a.generation, &token));
    tick();
    ind[1] = (uint8_t)requests[initial_sent].txn;
    ind[2] = (uint8_t)(requests[initial_sent].txn >> 8);
    ind[10] = 2;
    input(1, a.remote_port, a.local_port, ind, sizeof(ind));
    ind[10] = 1;
    ind[11] = ind[12] = 1;
    input(1, a.remote_port, a.local_port, ind, sizeof(ind));
    tick();
    assert(hk_qmux_frontend_take(a.index, a.generation, token, &r) == EAGAIN && !r.serving_valid);
    assert(!hk_qmux_frontend_read_event(a.index, a.generation, 1, &e));
    assert(e.sequence == 3 && e.registration == 1 && e.cs_attach == 1 && e.ps_attach == 1);
    assert(!hk_qmux_frontend_read_event(a.index, a.generation, 0, &e) && e.sequence == 3);
    reply(a.local_port, requests[initial_sent].txn, true);
    tick();
    assert(!hk_qmux_frontend_take(a.index, a.generation, token, &r));
    assert(!r.serving_valid && r.firmware_error == 74 && !r.registration && !r.cs_attach && !r.ps_attach);
    assert(hk_qmux_frontend_read_event(a.index, a.generation, 3, &e) == EAGAIN);
    assert(!hk_qmux_frontend_read_event(a.index, a.generation, 0, &e) &&
           e.sequence == 3 && e.registration == 1);

    assert(!hk_qmux_frontend_request(b.index, b.generation, &token));
    tick();
    reply(b.local_port, requests[initial_sent + 1].txn, false);
    tick();
    assert(!hk_qmux_frontend_take(b.index, b.generation, token, &r) && r.serving_valid);
    assert(hk_qmux_frontend_read_event(b.index, b.generation, 0, &e) == EAGAIN);
    assert(hk_qmux_frontend_take(b.index, b.generation, token, &r) == ENOENT && !r.serving_valid);
    assert(sent == initial_sent + 2);
}

static void register_ack(uint32_t port, uint16_t tid, bool error)
{
    uint8_t p[]={2,(uint8_t)tid,(uint8_t)(tid>>8),3,0,7,0,2,4,0,(uint8_t)error,0,(uint8_t)(error?74:0),0};
    input(1,58,port,p,sizeof(p));
}

static void subscription_round(void)
{
    struct hk_qmux_channel_info a, b, fresh;
    struct hk_qmux_nas_subscription status;
    struct hk_qmux_nas_event event;
    struct hk_qmux_nas_answer answer;
    const uint8_t ind[]={4,0,0,0x24,0,8,0,1,5,0,1,2,2,0,0};
    uint64_t token,other;
    assert(!hk_qmux_frontend_info(1,&a) && !hk_qmux_frontend_info(2,&b));
    assert(!enables && !disables);
    assert(!hk_qmux_frontend_subscribe(1,a.generation,&token));
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && status.phase==HK_QMUX_SUB_STARTING);
    assert(hk_qmux_frontend_subscribe(1,a.generation,&other)==EALREADY);
    assert(hk_qmux_frontend_request(1,a.generation,&other)==EBUSY);
    tick(); unsigned i=sent-1;
    assert(enables==1 && !disables && requests[i].port==a.local_port);
    input(1,58,a.local_port,ind,sizeof(ind));           /* Before enable ACK */
    register_ack(a.local_port,requests[i].txn+1,false); /* Wrong transaction */
    register_ack(b.local_port,requests[i].txn,false);   /* Other channel */
    reply(a.local_port,requests[i].txn,false);          /* Same TX, wrong operation */
    const uint8_t wrongpeer[]={2,(uint8_t)requests[i].txn,0,3,0,7,0,2,4,0,0,0,0,0};
    input(1,59,a.local_port,wrongpeer,sizeof(wrongpeer));
    tick();
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && !status.enable.response_bytes);
    assert(hk_qmux_frontend_read_subscription_event(1,a.generation,token,0,&event)==EAGAIN);
    uint8_t duplicate[]={2,(uint8_t)requests[i].txn,0,3,0,14,0,2,4,0,0,0,0,0,2,4,0,0,0,0,0};
    input(1,58,a.local_port,duplicate,sizeof(duplicate));tick();
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && status.phase==HK_QMUX_SUB_STARTING);
    uint8_t extra[]={2,(uint8_t)requests[i].txn,0,3,0,11,0,0xe0,1,0,0xff,2,4,0,0,0,0,0};
    input(1,58,a.local_port,extra,sizeof(extra));tick();
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && status.phase==HK_QMUX_SUB_ACTIVE);
    assert(status.enable.transaction==requests[i].txn && status.enable.response_bytes==sizeof(extra) && status.sequence_at_ack);
    assert(hk_qmux_frontend_read_subscription_event(1,a.generation,token,0,&event)==EAGAIN);
    assert(!hk_qmux_frontend_request(1,a.generation,&other));tick();
    register_ack(a.local_port,requests[sent-1].txn,false);tick();
    assert(hk_qmux_frontend_take(1,a.generation,other,&answer)==EAGAIN);
    reply(a.local_port,requests[sent-1].txn,false);tick();
    assert(hk_qmux_frontend_unsubscribe(1,a.generation,token)==EBUSY);
    assert(!hk_qmux_frontend_take(1,a.generation,other,&answer) && answer.serving_valid);
    input(1,58,a.local_port,ind,sizeof(ind));tick();
    assert(!hk_qmux_frontend_read_subscription_event(1,a.generation,token,0,&event));
    assert(event.sequence==status.sequence_at_ack+1 && event.registration==1);
    assert(!hk_qmux_frontend_unsubscribe(1,a.generation,token));
    assert(hk_qmux_frontend_read_subscription_event(1,a.generation,token,0,&event)==ENOTCONN && !event.sequence);
    assert(hk_qmux_frontend_unsubscribe(1,a.generation,token)==EALREADY);
    tick();assert(disables==1);
    input(1,58,a.local_port,ind,sizeof(ind));register_ack(a.local_port,requests[sent-1].txn,false);
    settle();
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && status.phase==HK_QMUX_SUB_CLOSED);
    assert(status.disable.transaction==requests[sent-1].txn && status.disable.response_bytes==14 && !status.terminal_error);
    assert(!hk_qmux_frontend_info(1,&fresh) && fresh.generation!=a.generation && fresh.local_port!=a.local_port);
    input(1,58,a.local_port,ind,sizeof(ind));tick();
    assert(hk_qmux_frontend_read_event(1,fresh.generation,0,&event)==EAGAIN);
    assert(hk_qmux_frontend_read_subscription_event(1,a.generation,token,0,&event)==ESTALE);

    assert(!hk_qmux_frontend_subscribe(1,fresh.generation,&token));tick();
    register_ack(fresh.local_port,requests[sent-1].txn,true);tick();
    assert(!hk_qmux_frontend_subscription(1,fresh.generation,token,&status) && status.phase==HK_QMUX_SUB_REFUSED);
    assert(status.enable.firmware_error==74 && !status.disable.response_bytes);
    assert(!hk_qmux_frontend_unsubscribe(1,fresh.generation,token));settle();
    assert(disables==1 && enables==2);
    assert(!hk_qmux_frontend_info(1,&a) && !hk_qmux_frontend_subscribe(1,a.generation,&token));
    tick();timer_ns+=HK_QMUX_FRONTEND_TIMEOUT_NS;settle();
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && status.terminal_error==ETIMEDOUT);
    register_ack(a.local_port,requests[sent-1].txn,false);tick();
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && !status.enable.response_bytes);
    assert(!hk_qmux_frontend_info(1,&a) && !hk_qmux_frontend_subscribe(1,a.generation,&token));
    assert(!hk_qmux_frontend_unsubscribe(1,a.generation,token));settle();
    assert(enables==3 && disables==1); /* cancelled before write */
    assert(!hk_qmux_frontend_info(1,&a) && !hk_qmux_frontend_subscribe(1,a.generation,&token));tick();
    register_ack(a.local_port,requests[sent-1].txn,false);tick();
    assert(!hk_qmux_frontend_unsubscribe(1,a.generation,token));tick();
    timer_ns+=HK_QMUX_FRONTEND_TIMEOUT_NS;settle();
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && status.terminal_error==ETIMEDOUT &&
           status.enable.response_bytes==14 && !status.disable.response_bytes);
    assert(enables==4 && disables==2);
    assert(!hk_qmux_frontend_info(1,&a) && !hk_qmux_frontend_subscribe(1,a.generation,&token));tick();
    announce(false);tick();announce(true);settle();
    assert(!hk_qmux_frontend_subscription(1,a.generation,token,&status) && status.terminal_error==ESTALE);
    puts("PASS explicit NAS enable/clear ACKs, interleaved GET, indication after ACK, bounded retirement, failure and isolation");
}

static void *foreign_cancel(void *arg){
    (void)arg;assert(!hk_qmux_frontend_cancel(1,cancel_gen,cancel_token));return NULL;
}
int main(void){
    owner=pthread_self();hk_qrtr_mhi_init(&m,1,send,NULL);hk_qrtr_mhi_set_active(&m);
    assert(!hk_abp_worker_arm(&w,&m,&ops,NULL));worker_gen=hk_abp_worker_generation(&w);
    assert(!hk_abp_worker_ready(&w));settle();assert(!sent&&!visible[0].generation&&!visible[1].generation);
    /* On this NAS owner, broadcast name-service payloads addressed to a
     * control-port alias still take the ordinary discovery path. */
    uint32_t ns[]={HK_QRTR_WIRE_NEW_SERVER,3,1,3,58};
    input_from(HK_QRTR_WIRE_DATA,3,HK_QRTR_WIRE_PORT_CONTROL_16,UINT32_MAX,
               HK_QRTR_WIRE_PORT_LEGACY,ns,sizeof(ns));settle();
    struct hk_qmux_channel_info a,b,newa,newb;
    assert(!hk_qmux_frontend_info(1,&a)&&!hk_qmux_frontend_info(2,&b));
    assert(a.generation!=b.generation && a.local_port!=b.local_port && a.node==3 && a.remote_port==58 && !sent);
    uint64_t token,ta,tb;struct hk_qmux_nas_answer result;
    assert(hk_qmux_frontend_request(2,a.generation,&token)==ESTALE);
    assert(!hk_qmux_frontend_request(1,a.generation,&token));
    assert(!hk_qmux_frontend_cancel(1,a.generation,token));tick();assert(!sent);
    assert(hk_qmux_frontend_take(1,a.generation,token,&result)==ECANCELED);
    assert(!hk_qmux_frontend_request(1,a.generation,&ta)&&!hk_qmux_frontend_request(2,b.generation,&tb));
    tick();assert(sent==2 && requests[0].port==a.local_port && requests[1].port==b.local_port && requests[0].txn==requests[1].txn);
    assert(hk_qmux_frontend_take(1,a.generation,ta,&result)==EAGAIN);
    cancel_gen=a.generation;cancel_token=ta;pthread_t ct;assert(!pthread_create(&ct,NULL,foreign_cancel,NULL));pthread_join(ct,NULL);
    deny_withdraw_once=1;deny_publish_once=1;settle();settle();
    assert(!hk_qmux_frontend_info(1,&newa)&&!hk_qmux_frontend_info(2,&newb));
    assert(newa.generation!=a.generation && newa.local_port!=a.local_port && b.generation==newb.generation && b.local_port==newb.local_port);
    assert(hk_qmux_frontend_take(1,a.generation,ta,&result)==ECANCELED);
    reply(b.local_port,requests[1].txn,false);tick();
    assert(!hk_qmux_frontend_take(2,b.generation,tb,&result) && result.serving_valid && result.cs_attach==2);
    assert(!hk_qmux_frontend_request(1,newa.generation,&ta));tick();assert(sent==3 && requests[2].port==newa.local_port);
    reply(a.local_port,requests[2].txn,false);tick();assert(hk_qmux_frontend_take(1,newa.generation,ta,&result)==EAGAIN);
    reply(newa.local_port,requests[2].txn,true);tick();
    assert(!hk_qmux_frontend_take(1,newa.generation,ta,&result) && !result.serving_valid && result.firmware_error==74);
    assert(!hk_qmux_frontend_request(2,b.generation,&tb));tick();timer_ns+=HK_QMUX_FRONTEND_TIMEOUT_NS;
    assert(hk_qmux_frontend_take(2,b.generation,tb,&result)==ETIMEDOUT);settle();
    assert(!hk_qmux_frontend_info(1,&a)&&a.generation==newa.generation);
    assert(!hk_qmux_frontend_info(2,&newb)&&newb.generation!=b.generation);
    /* A second advertised copy of the same service is ambiguous, even
     * when the original endpoint stays online. Revoke both until unique. */
    uint32_t duplicate[]={HK_QRTR_WIRE_NEW_SERVER,3,1,3,59};
    input(HK_QRTR_WIRE_NEW_SERVER,HK_QRTR_WIRE_PORT_CONTROL,HK_QRTR_WIRE_PORT_CONTROL,duplicate,sizeof(duplicate));
    tick();assert(!visible[0].generation && !visible[1].generation);
    duplicate[0]=HK_QRTR_WIRE_DEL_SERVER;
    input(HK_QRTR_WIRE_DEL_SERVER,HK_QRTR_WIRE_PORT_CONTROL,HK_QRTR_WIRE_PORT_CONTROL,duplicate,sizeof(duplicate));
    settle();assert(visible[0].generation && visible[1].generation);
    assert(!hk_qmux_frontend_info(1,&a) && !hk_qmux_frontend_request(1,a.generation,&ta));
    unsigned wire_before_expiry=sent;timer_ns+=HK_QMUX_FRONTEND_TIMEOUT_NS;
    assert(hk_qmux_frontend_take(1,a.generation,ta,&result)==ETIMEDOUT);tick();assert(sent==wire_before_expiry);
    announce(false);tick();assert(!visible[0].generation&&!visible[1].generation && hk_qmux_frontend_info(1,&a)==ENOTCONN);
    announce(true);settle();assert(!hk_qmux_frontend_info(1,&a)&&a.generation!=newa.generation);
    assert(!hk_qmux_frontend_info(2,&b));
    serving_events(a,b);
    uint64_t old_gen=a.generation; uint32_t old_local=a.local_port;
    struct hk_qmux_nas_event e;
    announce(false);tick();
    assert(hk_qmux_frontend_read_event(1,old_gen,0,&e)==ESTALE && !e.channel.generation);
    announce(true);settle();
    assert(!hk_qmux_frontend_info(1,&a) && a.generation!=old_gen && a.local_port!=old_local);
    const uint8_t delayed[]={4,0,0,0x24,0,8,0,1,5,0,0,2,2,0,0};
    input(1,58,old_local,delayed,sizeof(delayed));tick();
    assert(hk_qmux_frontend_read_event(1,a.generation,0,&e)==EAGAIN && !e.sequence);
    subscription_round();
    assert(!hk_qmux_frontend_info(1,&a));
    assert(!hk_qmux_frontend_request(1,a.generation,&ta));unsigned before=sent;
    hk_abp_worker_stop(&w);assert(hk_qmux_frontend_stop(&w));
    assert(!hk_abp_worker_finish(&w)&&hk_abp_worker_drained(&w)&&sent==before&&!visible[0].generation&&!visible[1].generation);
    assert(hk_qmux_frontend_take(1,a.generation,ta,&result)==ESTALE);
    assert(hk_qmux_frontend_read_event(1,a.generation,0,&e)==ESTALE && !e.sequence);
    puts("PASS passive NAS events: peer, channel, generation, timestamp, coalescing, invalid frames, reply independence and lifetime");
    printf("PASS typed QMUX publication, distinct generations, zero unsolicited requests, pending/queued cancellation, delayed old response, sibling isolation, real errors, timeout, remove/reappear, stop (%u withdrawals)\n",retired);
}
