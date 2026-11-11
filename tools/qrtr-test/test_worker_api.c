#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/hk_abp_nas_worker.h>
#include <sys/hk_qrtr_wire.h>

static struct hk_abp_nas_worker service;
static struct hk_qrtr_mhi mhi;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_t owner;
static unsigned sending,received,release_callback,callback_active,finished;
static bool timeout_case;
static unsigned park_before_lock,parked,continue_producer;
static uint64_t lease, generation;
static const uint8_t allocate[]={1,15,0,0,0,0,0,7,0x22,0,4,0,1,1,0,3};
static const uint8_t serving[]={1,12,0,0,3,1,0,6,0,0x24,0,0,0};
static void lock(void *ctx){
    (void)ctx;
    if(__atomic_exchange_n(&park_before_lock,0,__ATOMIC_ACQ_REL)){
        __atomic_store_n(&parked,1,__ATOMIC_RELEASE);
        while(!__atomic_load_n(&continue_producer,__ATOMIC_ACQUIRE))sched_yield();
    }
    assert(!pthread_mutex_lock(&mutex));
}
static void unlock(void *ctx){(void)ctx;assert(!pthread_mutex_unlock(&mutex));}
static bool is_owner(void *ctx){(void)ctx;return !!pthread_equal(pthread_self(),owner);}
static const struct hk_abp_nas_worker_ops ops={lock,unlock,is_owner};
static int receive(uint32_t type,uint32_t remote,uint32_t local,const void *bytes,size_t n){
    uint32_t header[8]={1,type,3,remote,0,(uint32_t)n,1,local};uint8_t p[128]={0};
    assert(n<sizeof(p)-32);memcpy(p,header,sizeof(header));memcpy(p+32,bytes,n);
    return hk_abp_worker_rx(&service,generation,p,32+((n+3)&~3u));
}
static int send(void *ctx,const void *data,size_t n){
    (void)ctx;const uint8_t *p=data;
    if(n>=39 && p[4]==1 && p[32]==0 && p[35]==0x24){
        uint32_t local;memcpy(&local,p+12,4);sending++;
        uint8_t reply[]={2,p[33],p[34],0x24,0,7,0,2,4,0,1,0,74,0};
        assert(!receive(1,58,local,reply,sizeof(reply)));
    }
    return 0;
}
static void deliver(void *ctx,const uint8_t h[6],const uint8_t *qmi,size_t n){
    (void)ctx;assert(is_owner(NULL));
    assert(hk_abp_worker_close(&service,lease)==EBUSY);
    if(!h[4])return;
    assert(h[4]==3 && h[5]==1 && n==14 && qmi[0]==2 && qmi[12]==74);
    received++;
    __atomic_store_n(&callback_active,1,__ATOMIC_RELEASE);
    while(!__atomic_load_n(&release_callback,__ATOMIC_ACQUIRE))sched_yield();
}
static void *run(void *arg){
    (void)arg;owner=pthread_self();
    assert(!hk_abp_worker_poll(&service) && !sending);
    assert(hk_abp_worker_open(&service,"pbl",3,deliver,NULL,&lease)==ENOTSUP);
    assert(!hk_abp_worker_open(&service,"qmux1",5,deliver,NULL,&lease) && lease && !sending);
    assert(!hk_abp_worker_write(&service,lease,allocate,sizeof(allocate)) && !sending);
    assert(!hk_abp_worker_poll(&service) && !sending);
    assert(!hk_abp_worker_write(&service,lease,serving,sizeof(serving)) && sending==1 && !received);
    assert(hk_abp_worker_poll(&service)==ECANCELED);
    assert(hk_abp_worker_write(&service,lease,serving,sizeof(serving))==ECANCELED);
    assert(hk_abp_worker_finish(&service)==(timeout_case?ETIMEDOUT:0));finished++;
    return NULL;
}
static void *waiting_producer(void *unused){
    (void)unused;uint8_t byte=0;
    assert(hk_abp_worker_rx(&service,generation,&byte,1)==ECANCELED);
    return NULL;
}
int main(int argc,char **argv){
    assert(argc==2);timeout_case=!strcmp(argv[1],"timeout");
    hk_qrtr_mhi_init(&mhi,1,send,NULL);hk_qrtr_mhi_set_active(&mhi);
    assert(!hk_abp_worker_arm(&service,&mhi,&ops,NULL));
    generation=hk_abp_worker_generation(&service);
    if(!strcmp(argv[1],"failed-start")){
        hk_abp_worker_stop(&service);
        assert(hk_abp_worker_never_started(&service)&&hk_abp_worker_drained(&service));return 0;
    }
    uint32_t query[]={HK_QRTR_WIRE_NEW_SERVER,3,1,3,58};
    assert(!receive(HK_QRTR_WIRE_NEW_SERVER,HK_QRTR_WIRE_PORT_CONTROL,HK_QRTR_WIRE_PORT_CONTROL,query,sizeof(query)));
    assert(!hk_abp_worker_ready(&service));
    if(!strcmp(argv[1],"producer-before-lock")){
        owner=pthread_self();
        assert(!hk_abp_worker_poll(&service));
        uint64_t old_lease=0;
        assert(!hk_abp_worker_open(&service,"qmux1",5,deliver,NULL,&old_lease));
        __atomic_store_n(&park_before_lock,1,__ATOMIC_RELEASE);
        pthread_t producer;assert(!pthread_create(&producer,NULL,waiting_producer,NULL));
        while(!__atomic_load_n(&parked,__ATOMIC_ACQUIRE))sched_yield();
        hk_abp_worker_stop(&service);
        assert(hk_abp_worker_finish(&service)==EAGAIN && !hk_abp_worker_drained(&service));
        assert(service.backend.watch!=NULL && mhi.up);
        __atomic_store_n(&continue_producer,1,__ATOMIC_RELEASE);pthread_join(producer,NULL);
        assert(!hk_abp_worker_finish(&service) && hk_abp_worker_drained(&service));
        uint64_t old_generation=generation;
        assert(!hk_abp_worker_rearm_after_disconnect(&service,&mhi,&ops,NULL));
        generation=hk_abp_worker_generation(&service);assert(generation==old_generation+1);
        uint8_t stale=0;
        assert(hk_abp_worker_rx(&service,old_generation,&stale,1)==ESTALE && !service.inbox.accepted);
        assert(!receive(HK_QRTR_WIRE_NEW_SERVER,HK_QRTR_WIRE_PORT_CONTROL,HK_QRTR_WIRE_PORT_CONTROL,query,sizeof(query)));
        assert(!hk_abp_worker_ready(&service) && !hk_abp_worker_poll(&service));
        uint64_t new_lease=0;
        assert(!hk_abp_worker_open(&service,"qmux1",5,deliver,NULL,&new_lease) && new_lease!=old_lease);
        assert(hk_abp_worker_write(&service,old_lease,serving,sizeof(serving))==ENOTCONN);
        hk_abp_worker_stop(&service);assert(!hk_abp_worker_finish(&service) && !sending);
        puts("PASS producer counted before acquiring mutex; delayed release blocks ACK; stale generation and lease rejected after rearm");
        return 0;
    }
    if(!strcmp(argv[1],"overflow")){
        owner=pthread_self();
        for(unsigned i=1;i<HK_QI_SLOTS;i++)assert(!hk_abp_worker_rx(&service,generation,query,sizeof(query)));
        assert(hk_abp_worker_rx(&service,generation,query,sizeof(query))==ENOBUFS);
        assert(hk_abp_worker_poll(&service)==ENOBUFS && !sending && !mhi.up);
        assert(!hk_abp_worker_finish(&service) && hk_abp_worker_drained(&service));
        assert(service.inbox.accepted==HK_QI_SLOTS && service.discarded==HK_QI_SLOTS);
        return 0;
    }
    pthread_t t;assert(!pthread_create(&t,NULL,run,NULL));
    while(!__atomic_load_n(&callback_active,__ATOMIC_ACQUIRE))sched_yield();
    assert(hk_abp_worker_write(&service,lease,serving,sizeof(serving))==EPERM);
    assert(hk_abp_worker_finish(&service)==EPERM);
    hk_abp_worker_stop(&service);
    assert(!hk_abp_worker_drained(&service));
    assert(hk_abp_worker_rx(&service,generation,query,sizeof(query))==ECANCELED);
    if(timeout_case)assert(!hk_abp_worker_timeout(&service));
    __atomic_store_n(&release_callback,1,__ATOMIC_RELEASE);pthread_join(t,NULL);
    assert(hk_abp_worker_drained(&service)==!timeout_case);
    assert(finished==1 && sending==1 && received==1 && !mhi.up && !service.backend.watch);
    assert(service.inbox.accepted==service.inbox.retired && !service.discarded);
    for(unsigned i=0;i<HK_QRTR_CORE_MAX_PORTS;i++)assert(!mhi.core.ports[i].number);
    puts("PASS maintained worker API: idle without requests, nested real receive, owner-only lease, reentry refusal, stop during callback, late-input rejection, final ownership");
}
