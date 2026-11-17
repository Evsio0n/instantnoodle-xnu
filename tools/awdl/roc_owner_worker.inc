/* The compatibility driver reserves one LinuxKPI task slot per XNU
 * thread. One station owner therefore talks to one persistent ROC worker
 * regardless of the number of windows. All job fields use lx_os_lock. */
static struct {
	bool started,requested,busy,quitting,ended;
	unsigned submitted,retired;
	struct lx_radio *radio;
	struct ieee80211_vif *vif;
} window_owner;

static void lx_awdl_roc_owner_loop(void *unused)
{
	(void)unused;
	lx_os_lock();
	for (;;) {
		while (!window_owner.requested && !window_owner.quitting)
			lx_os_sleep(&window_owner,0);
		if (!window_owner.requested && window_owner.quitting) break;
		struct lx_radio *r=window_owner.radio;
		struct ieee80211_vif *v=window_owner.vif;
		window_owner.requested=false;window_owner.busy=true;
		lx_os_unlock();
		bool ok=lx_awdl_listen_channel(r,v,6);
		lx_os_lock();
		window_owner.radio=NULL;window_owner.vif=NULL;
		window_owner.busy=false;window_owner.retired++;
		ES(bw.roc_ok,ok);ES(bw.finished,1);EA(bw.signal);
		lx_os_wakeup(&bw.signal);lx_os_wakeup(&window_owner);
	}
	window_owner.ended=true;lx_os_wakeup(&window_owner);
	lx_os_unlock();
}
static int lx_awdl_dispatch_roc(struct lx_radio *r)
{
	if (!r->persistent_awdl) return lx_os_thread("wifi-awdl-roc",lx_awdl_window_worker,NULL);
	/* Exactly one radio owner can start this worker and post its jobs. */
	if (!window_owner.started) {
		int e=lx_os_thread("wifi-awdl-roc",lx_awdl_roc_owner_loop,NULL);
		if (e) return e;
		window_owner.started=true;
	}
	lx_os_lock();
	int e=0;
	if (window_owner.quitting || window_owner.busy || window_owner.requested) e=EBUSY;
	else {
		window_owner.radio=r;window_owner.vif=bw.vif;
		window_owner.requested=true;window_owner.submitted++;
		lx_os_wakeup(&window_owner);
	}
	lx_os_unlock();
	return e;
}
static void lx_awdl_end_roc_worker(void)
{
	if (!window_owner.started) return;
	lx_os_lock();
	window_owner.quitting=true;lx_os_wakeup(&window_owner);
	while (!window_owner.ended) lx_os_sleep(&window_owner,50000000ull);
	unsigned submitted=window_owner.submitted,retired=window_owner.retired;
	lx_os_unlock();
	lx_tell("awdl-live: roc_workers=1 roc_jobs=%u roc_retired=%u joined=1\n",submitted,retired);
}
