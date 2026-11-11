/* This optional, kernel-originating client uses the same public request/take
 * interface as any permitted kernel consumer. Normal frontend operation sends
 * no test reads. Never reuse a request or token after a new publication. */
static struct {
	bool enabled, started, done;
	unsigned submitted, taken, good;
	uint64_t token[2], since, deadline;
	struct hk_qmux_channel_info info[2];
} mdm_abp_read;

static void mdm_abp_read_tick(void)
{
	if (!mdm_abp_read.enabled || mdm_abp_read.done) return;
	uint64_t now=op_nanoseconds(NULL);
	if (!mdm_abp_read.since) mdm_abp_read.since=now;
	if (!mdm_abp_read.started) {
		if (now-mdm_abp_read.since<35ull*NSEC_PER_SEC) return;
		struct hk_qmux_channel_info *a=&mdm_abp_read.info[0],*b=&mdm_abp_read.info[1];
		if (hk_qmux_frontend_info(1,a) || hk_qmux_frontend_info(2,b)) return;
		mdm_abp_read.started=true;
		if (!a->generation || !b->generation || a->index!=1 || b->index!=2 ||
		    !a->local_port || !b->local_port || a->local_port==b->local_port ||
		    a->node!=b->node || a->remote_port!=b->remote_port || !a->cid || !b->cid) {
			mdm_abp_read.done=true;
			say("modem: abp-read: inconsistent channel publication\n");
			return;
		}
		mdm_abp_read.deadline=now+8ull*NSEC_PER_SEC;
		uint64_t abandoned=0;
		int request=hk_qmux_frontend_request(1,a->generation,&abandoned);
		int cancel=request?request:hk_qmux_frontend_cancel(1,a->generation,abandoned);
		struct hk_qmux_nas_answer ignored={0};
		int stale=cancel?cancel:hk_qmux_frontend_take(1,a->generation,abandoned,&ignored);
		say("modem: abp-read: cancel owner=%llu generation=%llu request=%d cancel=%d take_after_cancel=%d\n",
		    (unsigned long long)mdm_abp_binding.epoch,(unsigned long long)a->generation,request,cancel,stale);
		if (request || cancel || (stale!=ENOENT && stale!=ECANCELED)) {mdm_abp_read.done=true;return;}
		for(unsigned i=0;i<2;i++) {
			struct hk_qmux_channel_info *v=&mdm_abp_read.info[i];
			int rc=hk_qmux_frontend_request(i+1,v->generation,&mdm_abp_read.token[i]);
			if (!rc) mdm_abp_read.submitted|=1u<<i;
			say("modem: abp-read: request owner=%llu generation=%llu index=%u node=%u peer=%u local=%u cid=%u token=%llu rc=%d\n",
			    (unsigned long long)mdm_abp_binding.epoch,(unsigned long long)v->generation,i+1,v->node,v->remote_port,v->local_port,v->cid,
			    (unsigned long long)mdm_abp_read.token[i],rc);
		}
	}
	for(unsigned i=0;i<2;i++) {
		unsigned bit=1u<<i;
		if (!(mdm_abp_read.submitted&bit) || (mdm_abp_read.taken&bit)) continue;
		struct hk_qmux_nas_answer a={0};
		int rc=hk_qmux_frontend_take(i+1,mdm_abp_read.info[i].generation,mdm_abp_read.token[i],&a);
		if (rc==EAGAIN && now<mdm_abp_read.deadline) continue;
		if (rc==EAGAIN) (void)hk_qmux_frontend_cancel(i+1,mdm_abp_read.info[i].generation,mdm_abp_read.token[i]);
		mdm_abp_read.taken|=bit;
		if (!rc && !a.firmware_result && !a.firmware_error && a.serving_valid) mdm_abp_read.good|=bit;
		say("modem: abp-read: answer owner=%llu generation=%llu index=%u token=%llu take=%d tid=%u bytes=%u result=%u error=%u valid=%u reg=%u cs=%u ps=%u\n",
		    (unsigned long long)mdm_abp_binding.epoch,(unsigned long long)mdm_abp_read.info[i].generation,i+1,(unsigned long long)mdm_abp_read.token[i],rc,a.transaction,
		    a.response_bytes,a.firmware_result,a.firmware_error,(unsigned)a.serving_valid,a.registration,a.cs_attach,a.ps_attach);
	}
	if (mdm_abp_read.taken==mdm_abp_read.submitted) {
		mdm_abp_read.done=true;
		say("modem: abp-read: end owner=%llu gen1=%llu gen2=%llu submitted=%u taken=%u serving=%u\n",
		    (unsigned long long)mdm_abp_binding.epoch,(unsigned long long)mdm_abp_read.info[0].generation,
		    (unsigned long long)mdm_abp_read.info[1].generation,mdm_abp_read.submitted,mdm_abp_read.taken,mdm_abp_read.good);
	}
}
