/* This entire block is built only in the private owner-checked candidate.
 * All mutable channel data, including its completion callback, belongs to
 * the MHI event thread. Mission/teardown share ONLY the atomic owner gate. */
#include <sys/hk_abp_nas.h>
#include <sys/hk_qmux_owner.h>
#include <sys/hk_qmux_ingress.h>
static struct hk_qmux_owner qno_gate;
static struct hk_qmux_ingress qno_rx;
static uint8_t qno_scratch[HK_QI_MAX];
static LCK_GRP_DECLARE(qno_group,"qmux read-only ingress");
static LCK_MTX_DECLARE(qno_lock,&qno_group);
static void qno_enqueue(const void *p,size_t n) {
    lck_mtx_lock(&qno_lock);(void)hk_qi_push(&qno_rx,p,n);lck_mtx_unlock(&qno_lock);
}
static bool qno_admit_rx(const void *p,size_t n) {
    if(hk_qo_phase(&qno_gate)==HK_QO_OFF)return false;
    qno_enqueue(p,n);return true;
}
static bool qno_lost_input(void) {
    lck_mtx_lock(&qno_lock);bool lost=qno_rx.overflow||qno_rx.rejected;lck_mtx_unlock(&qno_lock);return lost;
}
static size_t qno_take(void) {
    lck_mtx_lock(&qno_lock);size_t n=hk_qi_pop(&qno_rx,qno_scratch);lck_mtx_unlock(&qno_lock);return n;
}

static struct {
    struct hk_abp_nas backend;
    thread_t thread;
    uint64_t started,lease,epoch;
    unsigned attached,up,done,attempts,replies,valid,cid[2];
} qno;
static unsigned qno_u16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1]<<8); }
__attribute__((noreturn)) static void qno_held(void) {
    (void)hk_qo_timeout(&qno_gate);
    const char *msg="modem: UNCLEAN PCIe DMA OWNERSHIP; QMUX poll owner not drained; loader return halted, cold recovery required\n";
    for(const char *p=msg;*p;p++)loader_log_putc(*p);
#ifdef HK_QMUX_OWNER_HOST_TEST
    __builtin_trap();
#else
    for(;;)__asm__ volatile("wfe");
#endif
}
static struct hk_qmux_nas *qno_router(void) {return &qno.backend.channels[0].router;}
static void qno_emit(void *context,const uint8_t h[6],const uint8_t *p,size_t n) {
    (void)context;
    if(current_thread()!=qno.thread)qno_held();
    if(h[4]==0 && n==18 && p[2]==0x22 && p[1]>=1 && p[1]<=2){qno.cid[p[1]-1]=p[17];return;}
    if(h[4]!=3 || n<7 || p[0]!=2 || qno_u16(p+1)!=0x75a3 || qno_u16(p+3)!=0x24)return;
    unsigned index=h[5]&&h[5]==qno.cid[0]?0:h[5]&&h[5]==qno.cid[1]?1:2;
    if(index>1 || (qno.replies&(1u<<index)))return;
    int result=-1,error=-1,reg=-1,cs=-1,ps=-1;
    for(size_t off=7;off+3<=n;){
        unsigned type=p[off],len=qno_u16(p+off+1);off+=3;
        if(len>n-off)break;
        if(type==2 && len==4){result=(int)qno_u16(p+off);error=(int)qno_u16(p+off+2);}
        if(type==1 && len>=5 && (unsigned)p[off+4]==len-5){reg=p[off];cs=p[off+1];ps=p[off+2];}
        off+=len;
    }
    qno.replies|=1u<<index;
    if(!result && !error && reg>=0 && cs>=0 && ps>=0)qno.valid|=1u<<index;
    struct hk_qmux_nas *q=qno_router();
    say("modem: ui_qmux: reply epoch=%llu cid=%u local=%u node=%u peer=%u tid=30115 msg=36 bytes=%u result=%d error=%d reg=%d cs=%d ps=%d\n",
        (unsigned long long)qno.epoch,h[5],hk_qmux_nas_local_port(q,h[5]),q->slots[h[5]-1].node,
        q->slots[h[5]-1].server,(unsigned)n,result,error,reg,cs,ps);
}
static void qno_close(const char *why) {
    if(qno.done)return;
    if(current_thread()!=qno.thread)qno_held();
    if(qno.lease)hk_abp_nas_close(&qno.backend,qno.lease);
    if(qno.attached)hk_abp_nas_detach(&qno.backend);
    qno.done=1;
    say("modem: ui_qmux: end epoch=%llu reason=%s attempts=%u replies=%u valid=%u released=%u\n",
        (unsigned long long)qno.epoch,why,qno.attempts,qno.replies,qno.valid,
        qno.backend.watch==NULL && qno_router()->ops==NULL);
}
static void qno_tick(void) {
    if(current_thread()!=qno.thread)qno_held();
    if(!qno.up){
        hk_qrtr_mhi_set_up(&mdm_qrtr_mhi,true);qno.up=1;
        qno.started=op_nanoseconds(NULL);qno.epoch=mach_absolute_time();
        if(hk_abp_nas_attach(&qno.backend,&mdm_qrtr_mhi)){qno_close("attach_failed");return;}
        qno.attached=1;
        say("modem: qmux-owner: poller attached after mission epoch=%llu\n",(unsigned long long)qno.epoch);
    }
    /* Frames received in the synchronous startup waits were copied to the same
     * queue. Preserve HELLO/NEW_SERVER order even before the first active read.
     * Cap one batch; inner sends may enqueue another RX while we decode. */
    for(unsigned i=0;i<HK_QI_SLOTS;i++){
        size_t n=qno_take();if(!n)break;
        (void)hk_qrtr_mhi_input(&mdm_qrtr_mhi,qno_scratch,n);
    }
    if(qno.done)return;
    if(qno_lost_input()){qno_close("ingress_loss");return;}
    if(qno.replies==3){qno_close("both_replies");return;}
    if(op_nanoseconds(NULL)-qno.started>=50ull*NSEC_PER_SEC){qno_close("deadline");return;}
    hk_qrtr_mhi_fresh(&mdm_qrtr_mhi);
    if(!qno.lease){
        uint64_t lease=0;int rc=hk_abp_nas_open(&qno.backend,"qmux1",5,qno_emit,NULL,&lease);
        if(rc==EAGAIN)return;
        if(rc){qno_close("open_failed");return;}
        qno.lease=lease;
        for(unsigned i=0;i<2;i++){
            uint8_t ctl[]={1,15,0,0,0,0,0,(uint8_t)(i+1),0x22,0,4,0,1,1,0,3};
            if(hk_abp_nas_write(&qno.backend,lease,ctl,sizeof(ctl)) || !qno.cid[i]){qno_close("allocation_failed");return;}
        }
        if(qno.cid[0]==qno.cid[1]){qno_close("duplicate_cid");return;}
        say("modem: ui_qmux: allocate epoch=%llu cid1=%u local1=%u cid2=%u local2=%u\n",(unsigned long long)qno.epoch,
            qno.cid[0],hk_qmux_nas_local_port(qno_router(),qno.cid[0]),qno.cid[1],hk_qmux_nas_local_port(qno_router(),qno.cid[1]));
        hk_qrtr_mhi_fresh(&mdm_qrtr_mhi);
    }
    if(qno.attempts)return;
    struct hk_qmux_nas *q=qno_router();
    for(unsigned i=0;i<2;i++){
        struct hk_qmux_nas_slot *s=&q->slots[qno.cid[i]-1];
        if(s->lost){qno_close("peer_lost");return;}
        if(!s->ready)return;
    }
    for(unsigned i=0;i<2;i++){
        if(qno_lost_input() || hk_qo_phase(&qno_gate)!=HK_QO_READY){qno_close("admission_closed");return;}
        uint8_t get[]={1,12,0,0,3,(uint8_t)qno.cid[i],0,0xa3,0x75,0x24,0,0,0};
        qno.attempts++;
        int rc=hk_abp_nas_write(&qno.backend,qno.lease,get,sizeof(get));
        say("modem: ui_qmux: submit epoch=%llu cid=%u local=%u node=%u peer=%u tid=30115 msg=36 rc=%d\n",
            (unsigned long long)qno.epoch,qno.cid[i],hk_qmux_nas_local_port(q,qno.cid[i]),q->slots[qno.cid[i]-1].node,
            q->slots[qno.cid[i]-1].server,rc);
        if(rc){qno_close("send_failed");return;}
    }
}
static void qno_run_poller(void) {
    qno.thread=current_thread();
    for(;;){
        unsigned phase=hk_qo_phase(&qno_gate);
        if(phase==HK_QO_STOP || phase==HK_QO_HELD)break;
        modem_mhi_host_interrupt(&mdm_mhi,-1);
        if(hk_qo_phase(&qno_gate)==HK_QO_READY)qno_tick();
        delay_for_interval(1,NSEC_PER_MSEC);
    }
    /* No further interrupts or receive callbacks on this owner after here. */
    lck_mtx_lock(&qno_lock);hk_qi_close(&qno_rx);lck_mtx_unlock(&qno_lock);
    /* STOP closes admission, retires queued but undelivered copies, then
     * detaches. New/late callbacks can only observe the closed static queue. */
    unsigned discarded=0;
    while(qno_take())discarded++;
    qno_close("modem_teardown");
    if(qno.up)hk_qrtr_mhi_set_up(&mdm_qrtr_mhi,false);
    lck_mtx_lock(&qno_lock);
    unsigned in=qno_rx.accepted,out=qno_rx.retired,overflow=qno_rx.overflow,rejected=qno_rx.rejected;
    lck_mtx_unlock(&qno_lock);
    say("modem: qmux-owner: ingress closed accepted=%u retired=%u discarded=%u overflow=%u rejected=%u\n",in,out,discarded,overflow,rejected);
    if(in!=out)qno_held();
    say("modem: qmux-owner: callbacks exited, channels detached, link down\n");
    (void)hk_qo_ack(&qno_gate); /* Last access to the transport/owner state. */
}
static void qno_drain(void) {
    if(hk_qo_phase(&qno_gate)==HK_QO_OFF)return;
    hk_qo_stop(&qno_gate);
    uint64_t until=op_nanoseconds(NULL)+2000ull*NSEC_PER_MSEC;
    while(hk_qo_phase(&qno_gate)==HK_QO_STOP && op_nanoseconds(NULL)<until)
        delay_for_interval(1,NSEC_PER_MSEC);
    if(hk_qo_timeout(&qno_gate)!=HK_QO_DRAINED)qno_held();
    say("modem: qmux-owner: mission observed drain ACK before release\n");
}
