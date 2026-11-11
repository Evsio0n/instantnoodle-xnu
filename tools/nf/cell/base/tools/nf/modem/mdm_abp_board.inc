/* IN2010 IPCR producer and sole QRTR owner for the modem QMUX frontend.
 * The board, not userspace, owns the MHI allocation throughout this boot.
 * Keep the callback binding stable: it is installed before MHI can receive
 * and never reused in this kernel. Failed drain retains the entire arena. */
#include <sys/hk_abp_nas_worker.h>
#include <sys/hk_qmux_frontend.h>
#define HK_MDM_HAS_ABP_POLLER 1
static LCK_MTX_DECLARE(mdm_abp_inbox_lock, &mdm_lock_group);
static struct hk_abp_nas_worker mdm_abp_worker;
static struct mdm_abp_binding {
	uint64_t epoch;
} mdm_abp_binding;
static thread_t mdm_abp_thread;
static unsigned mdm_abp_exited;
#include "mdm_abp_read.inc"
#include "mdm_abp_events.inc"

static void mdm_abp_lock(__unused void *p) { lck_mtx_lock(&mdm_abp_inbox_lock); }
static void mdm_abp_unlock(__unused void *p) { lck_mtx_unlock(&mdm_abp_inbox_lock); }
static bool mdm_abp_is_owner(__unused void *p)
{
	return current_thread()==__atomic_load_n(&mdm_abp_thread,__ATOMIC_ACQUIRE);
}
static bool mdm_abp_watch_frame(uint32_t epoch,uint64_t *harvest_ms)
{
    if (!harvest_ms) return false;
    *harvest_ms=0;
    if (!epoch || !mdm_abp_is_owner(NULL) || !mdm_abp_worker.busy ||
        hk_qo_phase(&mdm_abp_worker.gate)!=HK_QO_READY) return false;
    const struct hk_abp_rx_stamp *s=&mdm_abp_worker.dispatch_stamp;
    if (!s->harvest_ms || s->generation!=hk_abp_worker_generation(&mdm_abp_worker)) return false;
    for(unsigned i=0;i<4;i++) if (s->epoch[i]==epoch && mdm_mhi_watch_posted(&mdm_mhi,epoch)) {
        *harvest_ms=s->harvest_ms;
        return true;
    }
    return false;
}
static bool pro_active,pro_transport_hold;
static uint64_t pro_owner_began;
#include "mdm_family_owner.inc"
#include "mdm_rfs_board.inc"
#include "mdm_rfs_uim.inc"
/* Poll-owner-only observation. Endpoint discovery, family management and
 * remote file service remain on their existing QRTR owner. Four one-shot
 * local addresses scope the two HW/SW pairs. The first is kept only as a
 * discovery listener between the pairs so a DEL/NEW on the same peer invalidates
 * the entire run. Each transaction inspects only fields projected by the wire
 * parser; ID data never enters this state or the logs. */
enum { PCS_QUERIES=4, PCS_STABLE_MS=15000, PCS_WAIT_MS=6000 };
struct pcs_trial {
    struct hk_qrtr_core_port *client;
    uint32_t local,watch,token,packets,unqualified,late,other_source;
    uint64_t issued_ms,deadline;
    struct pn_reply reply;
    int watch_error,send_error;
    bool issued,sent,watching,done;
};
static struct {
    struct pcs_trial trials[PCS_QUERIES];
    uint64_t owner,gate_ms,stable_ms,waiting_since;
    uint32_t first_files,read_node,node,port,service_epoch;
    unsigned step,retired;
    bool gated,found,lost,done;
} pcs;

static void
pcs_service(void *ctx,bool arrived,uint32_t service,uint32_t version,uint32_t instance,uint32_t node,uint32_t port)
{
    if (ctx!=&pcs.trials[0] || service!=36 || version!=1 || instance!=0 || !port ||
        node!=pcs.read_node || pcs.done || pcs.lost) return;
    if (arrived && !pcs.found) {
        pcs.node=node;pcs.port=port;pcs.found=true;pcs.service_epoch++;
        say("modem: mcfg-pdc endpoint owner=%llu gen=%u node=%u port=%u service=36 version=1 instance=0\n",
            (unsigned long long)pcs.owner,pcs.service_epoch,node,port);
    } else if ((!arrived && pcs.found && port==pcs.port) || (arrived && pcs.found && port!=pcs.port)) {
        pcs.lost=true;pcs.service_epoch++;
    }
}
static void
pcs_rx(void *ctx,uint32_t node,uint32_t port,const void *wire,size_t n)
{
    struct pcs_trial *t=ctx;
    if (!t->client || !t->issued || t->done || !t->sent || !pcs.found || pcs.lost || pcs.done ||
        t->client->number!=t->local || hk_abp_worker_generation(&mdm_abp_worker)!=pcs.owner) return;
    if (node!=pcs.node || port!=pcs.port) {++t->other_source;return;}
    uint64_t harvest=0;
    if (!mdm_abp_watch_frame(t->watch,&harvest) || harvest<t->issued_ms) {++t->unqualified;return;}
    if (harvest>=t->deadline) {++t->late;return;}
    ++t->packets;
    pn_decode(&t->reply,1,t->token,wire,n);
}
static bool pcs_room(void)
{
    unsigned free=0,reserve=0;
    for(unsigned j=0;j<HK_QRTR_CORE_MAX_PORTS;j++) if(!mdm_qrtr_mhi.core.ports[j].number) free++;
    for(unsigned j=0;j<MDM_RFS_SESSIONS;j++) if(!mdm_rfs_board.server.sessions[j].local) reserve++;
    return free>reserve;
}
static void pcs_retire(struct pcs_trial *t)
{
    if (!t->client) return;
    hk_qrtr_core_close(&mdm_qrtr_mhi.core,t->client);
    t->client=NULL;pcs.retired++;
}
static void pcs_end(const char *why)
{
    for(unsigned j=0;j<PCS_QUERIES;j++) pcs_retire(&pcs.trials[j]);
    pcs.done=true;
    say("modem: mcfg-pdc end reason=%s gated=%u steps=%u retired=%u owner=%llu files=%u\n",why,pcs.gated,pcs.step,pcs.retired,
        (unsigned long long)pcs.owner,mdm_rfs_board.server.reads.mcfg_gate.completed);
}
static void pcs_result(unsigned i,const char *forced,uint64_t now)
{
    struct pcs_trial *t=&pcs.trials[i];struct pn_reply *r=&t->reply;
    bool full=t->issued && now>=t->deadline;
    const char *status=forced;
    if (!status) {
        if(t->watch_error)status="watch_error";
        else if(t->send_error)status="send_error";
        else if(r->ind_candidates>1 || r->ack_conflicts) status="ambiguous";
        else if(r->bad_ack || r->bad_ind || r->bad_length) status="malformed";
        else if(r->ack && r->result) status="ack_error";
        else if(!r->ack || !r->ind) status="timeout";
        else status=r->ind_error?"ind_error":"complete";
    }
    bool fields=full && !strcmp(status,"complete");
    say("modem: mcfg-pdc reply sample=%u type=%u watch=%u local=%u txn=1 status=%s ack=%u result=%u error=%u ind=%u ind_error=%u ind_first=%u full=%u\n",
        i/2+1,i%2,t->watch,t->local,status,r->ack,r->result,r->error,r->ind,r->ind_error,r->ind_first,full);
    say("modem: mcfg-pdc fields sample=%u type=%u valid=%u active=%u active_len=%u pending=%u pending_len=%u ack_count=%u ind_count=%u ack_conflicts=%u ind_conflicts=%u\n",
        i/2+1,i%2,fields,fields && r->active,fields?r->active_length:0,fields && r->pending,fields?r->pending_length:0,
        r->ack_candidates,r->ind_candidates,r->ack_conflicts,r->ind_summary_conflicts);
    say("modem: mcfg-pdc route sample=%u type=%u packets=%u denied_stamp=%u late=%u wrong_source=%u bad_ack=%u bad_ind=%u bad_length=%u token_reject=%u sent=%u watch_rc=%d send_rc=%d\n",
        i/2+1,i%2,t->packets,t->unqualified,t->late,t->other_source,r->bad_ack,r->bad_ind,r->bad_length,r->unexpected_token,t->sent,t->watch_error,t->send_error);
    t->done=true;
    if(t->watching){mdm_mhi_watch_end(&mdm_mhi,t->watch);t->watching=false;}
    if(i!=0)pcs_retire(t);
    pcs.step++;pcs.waiting_since=0;
    if(i==1)pcs.stable_ms=now+PCS_STABLE_MS;
}
static void pcs_send(unsigned i,uint64_t now)
{
    struct pcs_trial *t=&pcs.trials[i];
    const uint32_t window=i%2?10000:1500;
    uint8_t wire[21];
    t->token=i%2?0:i/2+1;t->watch=0x2850u+i+1;
    int n=i%2?pn_get_sw_sub0(1,wire,sizeof(wire)):pcs_hw_request(1,t->token,wire,sizeof(wire));
    t->local=t->client->number;t->issued=true;t->issued_ms=now;t->deadline=now+window;
    if(n!=21)t->send_error=EINVAL;
    else {
        t->watch_error=mdm_mhi_watch_begin(&mdm_mhi,t->watch,1,0x22,t->local,pcs.node,pcs.port);
        if(!t->watch_error){
            t->watching=true;
            t->send_error=hk_qrtr_core_send(&mdm_qrtr_mhi.core,t->client,pcs.node,pcs.port,wire,(size_t)n);
            t->sent=t->send_error==0;
        }
    }
    say("modem: mcfg-pdc issue sample=%u type=%u watch=%u owner=%llu gen=%u node=%u port=%u local=%u txn=1 token=%u logical_sub_present=%u logical_sub=0 files=%u issued_ms=%llu window_ms=%u\n",
        i/2+1,i%2,t->watch,(unsigned long long)pcs.owner,pcs.service_epoch,pcs.node,pcs.port,t->local,t->token,i%2,
        mdm_rfs_board.server.reads.mcfg_gate.completed,(unsigned long long)now,window);
}
static void pcs_poll(struct hk_abp_nas_worker *w)
{
    if(pcs.done || hk_qo_phase(&w->gate)!=HK_QO_READY) return;
    uint64_t now=op_nanoseconds(NULL)/NSEC_PER_MSEC;
    const typeof(mdm_rfs_board.server.reads.mcfg_gate) *fw=&mdm_rfs_board.server.reads.mcfg_gate;
    if(!pcs.gated) {
        if(!fw->completed || !fw->acked_bytes || !fw->node || !fw->last_ms || fw->last_ms>now)return;
        pcs.gated=true;pcs.first_files=fw->completed;pcs.read_node=fw->node;
        pcs.owner=hk_abp_worker_generation(w);pcs.gate_ms=fw->last_ms;
        say("modem: mcfg-pdc gate owner=%llu files=%u acked_bytes=%llu file_node=%u ack_ms=%llu observed_ms=%llu\n",
            (unsigned long long)pcs.owner,pcs.first_files,(unsigned long long)fw->acked_bytes,pcs.read_node,
            (unsigned long long)pcs.gate_ms,(unsigned long long)now);
    }
    if(!pcs.owner || pcs.owner!=hk_abp_worker_generation(w) || fw->completed<pcs.first_files ||
        fw->node!=pcs.read_node || pcs.lost) {
        if(pcs.step<PCS_QUERIES && pcs.trials[pcs.step].issued)pcs_result(pcs.step,"stale",now);
        pcs_end("stale");return;
    }
    if(pcs.step==PCS_QUERIES){pcs_end("finished");return;}
    struct pcs_trial *t=&pcs.trials[pcs.step];
    if(t->issued) {
        if(t->send_error || t->watch_error || now>=t->deadline)pcs_result(pcs.step,NULL,now);
        return;
    }
    if(pcs.step>=2 && now<pcs.stable_ms)return;
    if(!pcs.waiting_since)pcs.waiting_since=now;
    if(!t->client && pcs_room()) {
        t->client=hk_qrtr_core_open(&mdm_qrtr_mhi.core,pcs_rx,pcs.step==0?pcs_service:NULL,t);
        if(t->client && pcs.step==0) {
            if(hk_qrtr_core_lookup(&mdm_qrtr_mhi.core,t->client,36,1,0)) {pcs_end("lookup_failed");return;}
            hk_qrtr_core_fresh(&mdm_qrtr_mhi.core);
        }
    }
    if(t->client && pcs.found && !pcs.lost)pcs_send(pcs.step,now);
    else if(now-pcs.waiting_since>=PCS_WAIT_MS)pcs_end(t->client?"service_absent":"capacity");
}
static void pcs_stop(void)
{
    if(pcs.done)return;
    uint64_t now=op_nanoseconds(NULL)/NSEC_PER_MSEC;
    if(pcs.step<PCS_QUERIES && pcs.trials[pcs.step].issued)pcs_result(pcs.step,"closed",now);
    pcs_end("closed");
}

static const struct hk_abp_nas_worker_ops mdm_abp_worker_ops={
	.lock=mdm_abp_lock,.unlock=mdm_abp_unlock,.is_owner=mdm_abp_is_owner
};
static __attribute__((noreturn)) void mdm_abp_hold(void)
{
	(void)hk_abp_worker_timeout(&mdm_abp_worker);
	const char *msg="modem: UNCLEAN PCIe DMA OWNERSHIP; ABP worker did not drain; loader return halted\n";
	for (const char *p=msg; *p; ++p) loader_log_putc(*p);
	for (;;) __asm__ volatile("wfe");
}
static bool mdm_abp_receive(void *context,const void *data,size_t length)
{
	if (context!=&mdm_abp_binding) return false;
	/* A stale or closed callback may drop data; it can never fall back to
	 * dispatching this datagram directly into the shared QRTR state. */
	struct hk_abp_rx_stamp stamp={0};
    mdm_mhi_watch_capture(&mdm_mhi,stamp.epoch,&stamp.harvest_ms);
    (void)hk_abp_worker_rx_tagged(&mdm_abp_worker,mdm_abp_binding.epoch,data,length,&stamp);
	return true;
}
static int mdm_abp_arm(void)
{
	int rc=hk_abp_worker_arm(&mdm_abp_worker,&mdm_qrtr_mhi,&mdm_abp_worker_ops,NULL);
	if (!rc) mdm_abp_binding.epoch=hk_abp_worker_generation(&mdm_abp_worker);
	say("modem: abp-board: arm epoch=%llu rc=%d\n",(unsigned long long)mdm_abp_binding.epoch,rc);
	return rc;
}
static void mdm_abp_run(void)
{
	struct hk_abp_nas_worker *w=&mdm_abp_worker;
	uint32_t previous_node=0,previous_port=0;
	__atomic_store_n(&mdm_abp_thread,current_thread(),__ATOMIC_RELEASE);
	say("modem: abp-board: poller entered epoch=%llu\n",(unsigned long long)mdm_abp_binding.epoch);
	for (;;) {
		if (hk_qo_phase(&w->gate)>=HK_QO_STOP) break;
		modem_mhi_host_interrupt(&mdm_mhi,-1);
		if (hk_qo_phase(&w->gate)>=HK_QO_STOP) break;
		if (mdm_rfs_requested && hk_qo_phase(&w->gate) == HK_QO_READY &&
		    !mdm_rfs_board.listener && !mdm_rfs_board_start()) {
			hk_abp_worker_stop(w);
			break;
		}
		int rc = mdm_rfs_requested ? hk_abp_worker_poll_observed(w, mdm_rfs_board_observe) :
		    hk_abp_worker_poll(w);
		if (!rc) {
			uint32_t node=w->backend.node,port=w->backend.port;
			if (node!=previous_node || port!=previous_port) {
				say("modem: abp-board: NAS epoch=%llu node=%u port=%u\n",
				    (unsigned long long)mdm_abp_binding.epoch,node,port);
				previous_node=node;
				previous_port=port;
			}
			if (mdm_rfs_requested) {
				mdm_rfs_board_tick();
				mdm_rfs_uim_poll(w);
                if(pro_active && ru.armed && (ru.c[RUC_UIM].subscribed || op_nanoseconds(NULL)-ru.began>=45ull*NSEC_PER_SEC)){
                    pro_owner_began=ru.began;
                    pro_transport_hold=cold.stage>=COLD_FRESH && cold.stage<=COLD_WATCH;
                    mdm_family_run();
                }
                if (pro_active) pcs_poll(w);
				delay_for_interval(1, NSEC_PER_MSEC);
				continue;
			}
			if (mdm_family.enabled) {
				mdm_family_run();
				delay_for_interval(1,NSEC_PER_MSEC);
				continue;
			}
			hk_qmux_frontend_poll(w);
			mdm_abp_read_tick();
			mdm_events_tick();
		} else if (rc!=EAGAIN) {
			hk_abp_worker_stop(w);
			break;
		}
		delay_for_interval(1,NSEC_PER_MSEC);
	}
	/* Ingress admission is closed before disconnecting any frontend.
	 * A previously admitted producer can still finish its bounded copy. */
	hk_abp_worker_stop(w);
	if (mdm_rfs_requested) {
        if (pro_active) pcs_stop();
		mdm_rfs_uim_stop(w);
		mdm_rfs_board_stop();
	}
	if (mdm_family.enabled) mdm_family_stop();
	while (!mdm_family.enabled && !mdm_rfs_requested && !hk_qmux_frontend_stop(w))
		delay_for_interval(1,NSEC_PER_MSEC);
	int rc;
	do {
		rc=hk_abp_worker_finish(w);
		if (rc==EAGAIN) delay_for_interval(1,NSEC_PER_MSEC);
	} while (rc==EAGAIN);
	if (rc || !hk_abp_worker_drained(w)) mdm_abp_hold();
	say("modem: donor qrtr_v2 decoded=%u with_options=%u max_option_bytes=%u\n",w->v2_decoded,w->v2_options,w->v2_max_options);
	say("modem: abp-board: owner drained epoch=%llu accepted=%u retired=%u discarded=%u overflow=%u rejected=%u\n",
	    (unsigned long long)mdm_abp_binding.epoch,w->inbox.accepted,w->inbox.retired,w->discarded,
	    w->inbox.overflow,w->inbox.rejected);
	__atomic_store_n(&mdm_abp_thread,NULL,__ATOMIC_RELEASE);
	__atomic_store_n(&mdm_abp_exited,1,__ATOMIC_RELEASE); /* last access by this owner */
}
static bool mdm_abp_finished(void)
{
	return hk_abp_worker_drained(&mdm_abp_worker) && __atomic_load_n(&mdm_abp_exited,__ATOMIC_ACQUIRE);
}
static void mdm_abp_drain(void)
{
	if (hk_qo_phase(&mdm_abp_worker.gate)==HK_QO_OFF) return;
	hk_abp_worker_stop(&mdm_abp_worker);
	uint64_t until=op_nanoseconds(NULL)+2000ull*NSEC_PER_MSEC;
	while (!mdm_abp_finished() && op_nanoseconds(NULL)<until)
		delay_for_interval(1,NSEC_PER_MSEC);
	if (!mdm_abp_finished()) mdm_abp_hold();
	say("modem: abp-board: mission confirmed owner exit before quiesce epoch=%llu\n",
	    (unsigned long long)mdm_abp_binding.epoch);
}
static void mdm_abp_quiesce_guard(void)
{
	if (hk_qo_phase(&mdm_abp_worker.gate)!=HK_QO_OFF && !mdm_abp_finished()) mdm_abp_hold();
}
