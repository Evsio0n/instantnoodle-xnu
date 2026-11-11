/* Real ingress, router, lease and owner loop against a model MHI device.
 * No radio or network I/O. Run once per case in a fresh process. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/hk_qrtr_wire.h>
#include <sys/hk_abp_nas.h>
#define HK_QMUX_OWNER_HOST_TEST 1
#define LCK_GRP_DECLARE(name,why) int name __attribute__((unused))
#define LCK_MTX_DECLARE(name,group) pthread_mutex_t name=PTHREAD_MUTEX_INITIALIZER
#define lck_mtx_lock pthread_mutex_lock
#define lck_mtx_unlock pthread_mutex_unlock
#define NSEC_PER_SEC 1000000000ull
#define NSEC_PER_MSEC 1000000ull
typedef pthread_t thread_t;
#define current_thread pthread_self
static struct hk_qrtr_mhi mdm_qrtr_mhi;
static int mdm_mhi;
static unsigned callback_entered,unblock,block_first,interrupts,wire_sends,end_seen;
static pthread_t expected_owner;
static uint64_t op_nanoseconds(void *unused){
    (void)unused;struct timespec ts;clock_gettime(CLOCK_MONOTONIC,&ts);
    return (uint64_t)ts.tv_sec*NSEC_PER_SEC+(uint64_t)ts.tv_nsec;
}
static uint64_t mach_absolute_time(void){return op_nanoseconds(NULL);}
static void delay_for_interval(unsigned n,unsigned long long unit){usleep((useconds_t)(n*unit/1000));}
static void loader_log_putc(char c){(void)c;}
static void modem_mhi_host_interrupt(void *m,int vec){
    (void)m;(void)vec;__atomic_add_fetch(&interrupts,1,__ATOMIC_RELEASE);
}
static void say(const char *format,...) {
    if(strstr(format,"ui_qmux: end epoch="))__atomic_store_n(&end_seen,1,__ATOMIC_RELEASE);
    if(block_first && strstr(format,"ui_qmux: reply epoch=") && !__atomic_exchange_n(&callback_entered,1,__ATOMIC_ACQ_REL)){
        while(!__atomic_load_n(&unblock,__ATOMIC_ACQUIRE))sched_yield();
    }
}
#include "phone_nas_owner.inc"

static void packet(uint32_t type,uint32_t from,uint32_t to,const void *data,size_t length) {
    uint32_t h[8]={1,type,3,from,0,(uint32_t)length,1,to};
    uint8_t p[160]={0};assert(length<=sizeof(p)-32);
    memcpy(p,h,sizeof(h));memcpy(p+32,data,length);
    assert(qno_admit_rx(p,32+((length+3u)&~3u)));
}
static int nested_transmit(void *unused,const void *raw,size_t len) {
    (void)unused;const uint8_t *p=raw;assert(pthread_equal(pthread_self(),expected_owner));
    if(len>=39 && p[4]==1 && p[32]==0 && p[35]==0x24){
        unsigned port=0;memcpy(&port,p+12,4);wire_sends++;
        uint8_t reply[]={2,p[33],p[34],0x24,0,15,0,2,4,0,0,0,0,0,1,5,0,0,2,2,0,0};
        /* Reenter physical RX while the owner is inside send. It can enqueue
         * but must never immediately dispatch back to these clients. */
        packet(1,58,port,reply,sizeof(reply));
        assert(!__atomic_load_n(&callback_entered,__ATOMIC_ACQUIRE));
    }
    return 0;
}
static void *worker(void *unused){(void)unused;expected_owner=pthread_self();qno_run_poller();return NULL;}
static void wait_flag(unsigned *flag){
    uint64_t end=op_nanoseconds(NULL)+2*NSEC_PER_SEC;
    while(!__atomic_load_n(flag,__ATOMIC_ACQUIRE) && op_nanoseconds(NULL)<end)sched_yield();
    assert(__atomic_load_n(flag,__ATOMIC_ACQUIRE));
}
int main(int argc,char **argv){
    assert(argc==2);const char *test=argv[1];
    hk_qrtr_mhi_init(&mdm_qrtr_mhi,1,nested_transmit,NULL);hk_qrtr_mhi_set_active(&mdm_qrtr_mhi);
    assert(hk_qo_arm(&qno_gate));
    if(!strcmp(test,"start-failed")){assert(hk_qo_no_worker(&qno_gate)&&hk_qo_phase(&qno_gate)==HK_QO_DRAINED);puts("PASS QMUX owner start-failed");return 0;}
    uint32_t ann[]={HK_QRTR_WIRE_NEW_SERVER,3,1,3,58};
    packet(HK_QRTR_WIRE_NEW_SERVER,HK_QRTR_WIRE_PORT_CONTROL,HK_QRTR_WIRE_PORT_CONTROL,ann,sizeof(ann));
    if(!strcmp(test,"overflow")){
        for(unsigned i=0;i<HK_QI_SLOTS;i++)packet(HK_QRTR_WIRE_NEW_SERVER,HK_QRTR_WIRE_PORT_CONTROL,HK_QRTR_WIRE_PORT_CONTROL,ann,sizeof(ann));
    }
    pthread_t th;block_first=!strcmp(test,"stop-in-callback")||!strcmp(test,"timeout");
    assert(!pthread_create(&th,NULL,worker,NULL));wait_flag(&interrupts);
    if(!strcmp(test,"early-stop")){
        qno_drain();pthread_join(th,NULL);
        assert(qno_rx.retired==1 && qno_rx.closed && !mdm_qrtr_mhi.up && !wire_sends);
    }else{
        assert(hk_qo_ready(&qno_gate));
        if(block_first){
            wait_flag(&callback_entered);hk_qo_stop(&qno_gate);
            assert(hk_qo_phase(&qno_gate)==HK_QO_STOP);
            if(!strcmp(test,"timeout"))assert(hk_qo_timeout(&qno_gate)==HK_QO_HELD);
            __atomic_store_n(&unblock,1,__ATOMIC_RELEASE);
        }else{
            /* Wait for the real owner once, without racy access to qno state. */
            wait_flag(&end_seen);hk_qo_stop(&qno_gate);
        }
        pthread_join(th,NULL);
        if(!strcmp(test,"overflow"))assert(qno_rx.overflow==1 && !wire_sends && !qno.replies);
        else assert(wire_sends==2 && qno.valid==3 && qno.cid[0]!=qno.cid[1]);
    }
    assert(hk_qo_phase(&qno_gate)==(!strcmp(test,"timeout")?HK_QO_HELD:HK_QO_DRAINED));
    assert(!qno.backend.owner && !qno_router()->ops && !mdm_qrtr_mhi.up && qno_rx.accepted==qno_rx.retired);
    size_t before=qno_rx.late;
    packet(HK_QRTR_WIRE_NEW_SERVER,HK_QRTR_WIRE_PORT_CONTROL,HK_QRTR_WIRE_PORT_CONTROL,ann,sizeof(ann));
    assert(qno_rx.late==before+1 && qno_rx.head==qno_rx.tail);
    for(unsigned i=0;i<HK_QRTR_CORE_MAX_PORTS;i++)assert(!mdm_qrtr_mhi.core.ports[i].number);
    printf("PASS QMUX owner %s\n",test);
}
