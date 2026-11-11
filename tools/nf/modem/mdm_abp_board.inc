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
#include "mdm_family_owner.inc"
#include "mdm_rfs_board.inc"
#include "mdm_rfs_uim.inc"
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
	(void)hk_abp_worker_rx(&mdm_abp_worker,mdm_abp_binding.epoch,data,length);
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
