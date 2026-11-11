/* Opt-in harness in the privately rebuilt #267 modem worker. No changes
 * to UIM/WDS. Both clients issue exactly the same read-only NAS request;
 * each QRTR destination port must route its reply to the right QMUX CID. */
#include <sys/hk_qmux_nas.h>
static struct {
    struct hk_qmux_nas q;
    unsigned enabled, checked, begun, done, attempts, replies, good;
    unsigned cid[2];
    uint64_t epoch;
} qn;
static unsigned qn_u16(const uint8_t *p) { return (unsigned)p[0] | ((unsigned)p[1]<<8); }
static void qn_emit(void *ctx,const uint8_t h[6],const uint8_t *p,size_t n)
{
    (void)ctx;
    if (h[4]==0 && n==18 && p[2]==0x22 && p[1]>=1 && p[1]<=2) {
        qn.cid[p[1]-1]=p[17];return;
    }
    if(h[4]!=3 || !h[5] || n<7 || p[0]!=2 || qn_u16(p+1)!=0x75a3 || qn_u16(p+3)!=0x24)return;
    unsigned index=h[5]==qn.cid[0]?0:h[5]==qn.cid[1]?1:2;
    if(index>1 || (qn.replies & (1u<<index)))return;
    int result=-1,error=-1,reg=-1,cs=-1,ps=-1;
    for(size_t off=7;off+3<=n;){
        unsigned type=p[off],len=qn_u16(p+off+1);off+=3;
        if(len>n-off)break;
        if(type==2 && len==4){result=(int)qn_u16(p+off);error=(int)qn_u16(p+off+2);}
        if(type==1 && len>=5 && (size_t)p[off+4]==len-5){reg=p[off];cs=p[off+1];ps=p[off+2];}
        off+=len;
    }
    qn.replies|=1u<<index;
    if(result==0 && error==0 && reg>=0 && cs>=0 && ps>=0)qn.good|=1u<<index;
    say("modem: ui_qmux: reply epoch=%llu cid=%u local=%u node=%u peer=%u tid=30115 msg=36 bytes=%u result=%d error=%d reg=%d cs=%d ps=%d\n",
        (unsigned long long)qn.epoch,h[5],hk_qmux_nas_local_port(&qn.q,h[5]),qn.q.slots[h[5]-1].node,
        qn.q.slots[h[5]-1].server,(unsigned)n,result,error,reg,cs,ps);
}
static void qn_close(const char *why)
{
    if(!qn.begun || qn.done)return;
    hk_qmux_nas_close(&qn.q);qn.done=1;
    say("modem: ui_qmux: end epoch=%llu reason=%s attempts=%u replies=%u valid=%u released=%u\n",
        (unsigned long long)qn.epoch,why,qn.attempts,qn.replies,qn.good,qn.q.ops==NULL);
}
static void qn_tick(unsigned tenths)
{
    if(!qn.checked){PE_parse_boot_argn("ui_qmux",&qn.enabled,sizeof(qn.enabled));qn.checked=1;}
    if(qn.enabled!=1 || qn.done || tenths<350u)return;
    if(!qn.begun){
        qn.begun=1;qn.epoch=mach_absolute_time();
        int rc=hk_qrtr_mhi_nas_open(&qn.q,qn_emit,NULL);
        for(unsigned i=0;!rc && i<2;i++){
            uint8_t ctl[]={1,15,0,0,0,0,0,(uint8_t)(i+1),0x22,0,4,0,1,1,0,3};
            rc=hk_qmux_nas_write(&qn.q,ctl,sizeof(ctl));if(!qn.cid[i] && !rc)rc=EINVAL;
        }
        if(rc || qn.cid[0]==qn.cid[1]){qn_close("allocation_failed");return;}
        say("modem: ui_qmux: allocate epoch=%llu cid1=%u local1=%u cid2=%u local2=%u\n",
            (unsigned long long)qn.epoch,qn.cid[0],hk_qmux_nas_local_port(&qn.q,qn.cid[0]),
            qn.cid[1],hk_qmux_nas_local_port(&qn.q,qn.cid[1]));
    }
    if(qn.replies==3){qn_close("both_replies");return;}
    if(tenths>=800u){qn_close("deadline");return;}
    if(qn.attempts)return;
    if(!qn.q.slots[qn.cid[0]-1].ready || !qn.q.slots[qn.cid[1]-1].ready)return;
    for(unsigned i=0;i<2;i++){
        uint8_t get[]={1,12,0,0,3,(uint8_t)qn.cid[i],0,0xa3,0x75,0x24,0,0,0};
        qn.attempts++;
        int rc=hk_qmux_nas_write(&qn.q,get,sizeof(get));
        say("modem: ui_qmux: submit epoch=%llu cid=%u local=%u node=%u peer=%u tid=30115 msg=36 rc=%d\n",
            (unsigned long long)qn.epoch,qn.cid[i],hk_qmux_nas_local_port(&qn.q,qn.cid[i]),qn.q.slots[qn.cid[i]-1].node,
            qn.q.slots[qn.cid[i]-1].server,rc);
        if(rc){qn_close("send_failed");return;}
    }
}
