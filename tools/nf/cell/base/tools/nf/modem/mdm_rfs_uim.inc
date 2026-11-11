/* Native UIM startup sequence and independent read-only radio queries on the
 * same serial owner as the read-only firmware server. A notification merely
 * triggers a fresh card read; its argument bytes never leave the decoder. */
enum ruc_client { RUC_UIM,RUC_DMS,RUC_NAS,RUC_WDS,RUC_COUNT };
enum ruc_task { RU_SUBSCRIBE,RU_PHYSICAL,RU_EXT,RU_CARD,RU_ATR,RU_RADIO,RU_CLEAR,RU_WAIT };
struct ruc {
	struct qmi_handle handle;
	union {struct hk_uim_obs_op u;struct hk_qmi_family_op f;} op;
	uint32_t service,node,port,local,generation,round,arg;
	uint64_t deadline;
	enum ruc_task task;
	bool open,live,pending,subscribed,ready,finished,queried,late;
};
static struct {
	struct ruc c[RUC_COUNT];
	uint64_t began,subscribed_ms;
	unsigned card_ind,slot_ind,refreshed_ind,refreshed_files,round,extra_servers;
	bool armed,changed,late;
} ru;
static unsigned cold_modem_stop;
static uint64_t cold_wds_when;
static bool cold_refused_offline;
static void cold_record_radio(struct ruc *, enum ruc_task, int, struct qmi_response_type_v01,bool,uint64_t);
static struct ruc *ru_slot(struct qmi_handle *h)
{
	for (unsigned i=0;i<RUC_COUNT;i++) if (&ru.c[i].handle==h) return &ru.c[i];
	return NULL;
}
static int ru_arrive(struct qmi_handle *h,struct qmi_service *s)
{
	struct ruc *c=ru_slot(h);
	if (!c || !s || !mdm_abp_is_owner(NULL) || !s->node || !s->port || s->service!=c->service ||
	    s->version!=1 || s->instance!=0) return ENXIO;
	if (c->live) {
		if (c->node!=s->node || c->port!=s->port) ru.extra_servers++;
		return ENXIO;
	}
	if (ru.changed) return ENXIO;
	int rc=hk_qmi_connect(h->sock,s->node,s->port);
	if (!rc) {
		c->node=s->node;c->port=s->port;c->generation++;c->live=true;
		say("modem: rfs-state endpoint service=%u version=1 instance=0 node=%u port=%u local=%u gen=%u\n",
		    c->service,c->node,c->port,c->local,c->generation);
	}
	return rc;
}
static void ru_depart(struct qmi_handle *h,struct qmi_service *s)
{
	struct ruc *c=ru_slot(h);
	if (c && s && c->live && c->node==s->node && c->port==s->port) {say("modem: donor endpoint_lost service=%u node=%u port=%u gen=%u round=%u clock_ms=%llu\n",c->service,c->node,c->port,c->generation,c->round,(unsigned long long)(op_nanoseconds(NULL)/NSEC_PER_MSEC)); c->live=false;ru.changed=true;}
}
static const struct qmi_ops ru_ops={.new_server=ru_arrive,.del_server=ru_depart};
struct ru_cue {uint8_t card,first,extended,ext_first;};
static const struct qmi_elem_info ru_cue_ei[]={
	{QMI_OPT_FLAG,1,1,NO_ARRAY,0x10,offsetof(struct ru_cue,card),NULL},
	{QMI_UNSIGNED_1_BYTE,1,1,NO_ARRAY,0x10,offsetof(struct ru_cue,first),NULL},
	{QMI_OPT_FLAG,1,1,NO_ARRAY,0x12,offsetof(struct ru_cue,extended),NULL},
	{QMI_UNSIGNED_1_BYTE,1,1,NO_ARRAY,0x12,offsetof(struct ru_cue,ext_first),NULL},
	{QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}};
static bool ru_ours(struct qmi_handle *h,struct sockaddr_qrtr *p)
{
	struct ruc *c=&ru.c[RUC_UIM];
	return mdm_abp_is_owner(NULL) && h==&c->handle && p && c->live && !ru.changed &&
	    p->sq_node==c->node && p->sq_port==c->port && !c->finished;
}
static void ru_card_cue(struct qmi_handle *h,struct sockaddr_qrtr *s,__unused struct qmi_txn *t,const void *d)
{
	const struct ru_cue *v=d;
	if (v && (v->card||v->extended) && ru_ours(h,s)) ru.card_ind++;
}
static void ru_slot_cue(struct qmi_handle *h,struct sockaddr_qrtr *s,__unused struct qmi_txn *t,const void *d)
{
	const struct ru_cue *v=d;
	if (v && v->card && ru_ours(h,s)) ru.slot_ind++;
}
static const struct qmi_msg_handler ru_handlers[]={
	{QMI_INDICATION,0x32,ru_cue_ei,sizeof(struct ru_cue),ru_card_cue},
	{QMI_INDICATION,0x48,ru_cue_ei,sizeof(struct ru_cue),ru_slot_cue},{}};
static struct qmi_txn *ru_txn(struct ruc *c)
{
	return c==&ru.c[RUC_NAS] || c==&ru.c[RUC_WDS]?&c->op.f.txn:&c->op.u.txn;
}
#include "hk_cellular_observation.h"
static LCK_MTX_DECLARE(obs_lock,&mdm_lock_group);
static struct hk_cellular_observation obs={.version=HK_CELLULAR_OBSERVATION_VERSION,.size=sizeof(obs),.phase=HK_OBS_OFF};
#ifndef HK_CELLULAR_HISTORY_H
#define HK_CELLULAR_HISTORY_H
/* v1 ABI included above */

#define HK_CELLULAR_HISTORY_NAME "kern.hackios.cellular_history"
#define HK_CELLULAR_HISTORY_VERSION 1u

/* This boot's in-memory observations only. last.valid_mask is historical;
 * only the existing cellular_observation sysctl can identify live values.
 * Each group keeps its own source timestamp, so these are not represented
 * as simultaneous measurements. now_ms is read from the same boot clock. */
struct hk_cellular_history {
    uint32_t version,size,phase,retained;
    uint64_t now_ms,latest_field_ms,stopped_ms,errfatal_ms;
    uint32_t online_attempts,online_submitted,online_generation,online_tid;
    uint32_t online_ack_seen,online_result,online_error,reserved;
    uint64_t online_attempt_ms,online_ack_ms;
    struct hk_cellular_observation last;
};
#endif

/* Owner-side display telemetry. Each caller passes only already-decoded
 * data. There are no QMI requests, file writes or device accesses here. */
static struct hk_cellular_history hist = {
    .version=HK_CELLULAR_HISTORY_VERSION,.size=sizeof(struct hk_cellular_history)
};

static int history_sysctl SYSCTL_HANDLER_ARGS
{
    struct hk_cellular_history copy;
    lck_mtx_lock(&obs_lock);
    copy=hist;
    lck_mtx_unlock(&obs_lock);
    copy.now_ms=op_nanoseconds(NULL)/NSEC_PER_MSEC;
    return SYSCTL_OUT(req,&copy,sizeof(copy));
}
SYSCTL_PROC(_kern_hackios,OID_AUTO,cellular_history,CTLTYPE_OPAQUE|CTLFLAG_RD|CTLFLAG_LOCKED,
    0,0,history_sysctl,"S,hk_cellular_history","Current boot's last valid cellular observations");

static void hist_field_locked(unsigned bits,uint64_t stamp)
{
    if(obs.phase!=HK_OBS_LIVE || !stamp) return;
    bits&=obs.valid_mask;
    if(!bits)return;
    struct hk_cellular_observation *p=&hist.last;
    if(bits&(HK_OBS_CARD|HK_OBS_GW)){
        if(p->uim_generation && p->uim_generation!=obs.uim_generation){
            p->valid_mask&=~(HK_OBS_CARD|HK_OBS_GW|HK_OBS_PHYSICAL|HK_OBS_ATR);
            p->verified=0;p->verified_ms=0;p->post_reads=0;
        }
        p->uim_generation=obs.uim_generation;p->round=obs.round;
    }
    if(bits&HK_OBS_PHYSICAL){p->physical_slot=obs.physical_slot;p->logical_slot=obs.logical_slot;
        p->card_present=obs.card_present;p->slot2_empty=obs.slot2_empty;p->physical_ms=obs.physical_ms;}
    if(bits&HK_OBS_CARD){p->card_state=obs.card_state;p->card_validity=obs.card_validity;p->apps=obs.apps;
        p->sim_states=obs.sim_states;p->usim_states=obs.usim_states;p->ready_gw=obs.ready_gw;p->card_ms=obs.card_ms;}
    if(bits&HK_OBS_ATR){p->atr_present=obs.atr_present;p->atr_length=obs.atr_length;p->atr_ms=obs.atr_ms;}
    if(bits&HK_OBS_GW){p->gw_status=obs.gw_status;p->gw_slot=obs.gw_slot;p->gw_app=obs.gw_app;
        p->gw_type=obs.gw_type;p->gw_state=obs.gw_state;p->card_ms=obs.card_ms;}
    if(bits&HK_OBS_DMS){p->dms_mode=obs.dms_mode;p->dms_ms=obs.dms_ms;}
    if(bits&HK_OBS_NAS){p->nas_registration=obs.nas_registration;p->cs_attach=obs.cs_attach;
        p->ps_attach=obs.ps_attach;p->nas_ms=obs.nas_ms;}
    if(bits&HK_OBS_WDS){p->wds_state=obs.wds_state;p->wds_ms=obs.wds_ms;}
    p->valid_mask|=bits;p->sequence=obs.sequence;
    p->size=sizeof(*p);p->version=HK_CELLULAR_OBSERVATION_VERSION;
    hist.retained=1;
    if(stamp>hist.latest_field_ms)hist.latest_field_ms=stamp;
}
static void hist_uim_locked(void)
{
    if(obs.phase!=HK_OBS_LIVE || !hist.retained)return;
    struct hk_cellular_observation *p=&hist.last;
    p->activation=obs.activation;p->attempted=obs.attempted;p->submitted=obs.submitted;
    p->ack_seen=obs.ack_seen;p->ack_result=obs.ack_result;p->ack_error=obs.ack_error;
    p->decision_ms=obs.decision_ms;p->post_reads=obs.post_reads;p->verified=obs.verified;
    p->verified_ms=obs.verified_ms;
}
static void hist_phase_locked(unsigned phase)
{
    if(phase==HK_OBS_LIVE && hist.phase!=HK_OBS_LIVE)
        hist=(struct hk_cellular_history){.version=HK_CELLULAR_HISTORY_VERSION,.size=sizeof(hist)};
    hist.phase=phase;
    if(phase==HK_OBS_STOPPED && !hist.stopped_ms)
        hist.stopped_ms=op_nanoseconds(NULL)/NSEC_PER_MSEC;
}
static void hist_online_submit(unsigned gen,unsigned tid,bool attempted,bool submitted,uint64_t now_ms)
{
    if(!attempted)return;
    lck_mtx_lock(&obs_lock);
    if(hist.phase==HK_OBS_LIVE){
        hist.online_attempts++;hist.online_generation=gen;hist.online_tid=tid;
        hist.online_submitted=submitted;hist.online_attempt_ms=now_ms;
        hist.online_ack_seen=0;hist.online_result=0;hist.online_error=0;hist.online_ack_ms=0;
    }
    lck_mtx_unlock(&obs_lock);
}
static void hist_online_ack(unsigned gen,unsigned tid,bool seen,int bytes,unsigned result,unsigned error,uint64_t now_ms)
{
    if(!seen || bytes<=0)return;
    lck_mtx_lock(&obs_lock);
    if(hist.phase==HK_OBS_LIVE && hist.online_submitted && hist.online_generation==gen &&
       hist.online_tid==tid && now_ms>=hist.online_attempt_ms){
        hist.online_ack_seen=1;hist.online_result=result;hist.online_error=error;hist.online_ack_ms=now_ms;
    }
    lck_mtx_unlock(&obs_lock);
}
static void hist_errfatal(void)
{
    lck_mtx_lock(&obs_lock);
    if(!hist.errfatal_ms)hist.errfatal_ms=op_nanoseconds(NULL)/NSEC_PER_MSEC;
    lck_mtx_unlock(&obs_lock);
}

static int ua_observer_sysctl SYSCTL_HANDLER_ARGS
{
	struct hk_cellular_observation copy;
	lck_mtx_lock(&obs_lock);copy=obs;lck_mtx_unlock(&obs_lock);
	copy.now_ms=op_nanoseconds(NULL)/NSEC_PER_MSEC;
	return SYSCTL_OUT(req,&copy,sizeof(copy));
}
SYSCTL_PROC(_kern_hackios,OID_AUTO,cellular_observation,CTLTYPE_OPAQUE|CTLFLAG_RD|CTLFLAG_LOCKED,
    0,0,ua_observer_sysctl,"S,hk_cellular_observation","Live nonidentifying modem observations");
static void obs_phase(unsigned phase)
{
	lck_mtx_lock(&obs_lock);hist_phase_locked(phase);obs.phase=phase;if(phase!=HK_OBS_LIVE)obs.valid_mask=0;obs.sequence++;lck_mtx_unlock(&obs_lock);
}
static void obs_card(struct ruc *u,const struct hk_qmi_uim_status *s,bool extended)
{
	if(!s->logical_count)return;
	const struct hk_uim_logical_slot *v=&s->logical[0];
	if(!extended && (v->validity==HK_UIM_VALIDITY_INVALID || v->validity==HK_UIM_VALIDITY_OTHER))return;
	const struct hk_uim_gw_binding *g=&s->gw[0];bool mapped=g->status==HK_UIM_GW_MAPPED;
	lck_mtx_lock(&obs_lock);
	obs.uim_generation=u->generation;obs.round=u->round;obs.card_state=v->card_state;obs.apps=v->app_count;
	obs.card_validity=v->validity;obs.sim_states=v->sim_states;obs.usim_states=v->usim_states;obs.ready_gw=v->ready_gw_apps;
	obs.gw_status=g->status;obs.gw_slot=mapped?1u+(g->index>>8):0;obs.gw_app=mapped?1u+(g->index&255):0;
	obs.gw_type=mapped?g->app_type:0;obs.gw_state=mapped?g->app_state:0;
	obs.card_ms=op_nanoseconds(NULL)/NSEC_PER_MSEC;obs.valid_mask|=HK_OBS_CARD|HK_OBS_GW;obs.sequence++;
	hist_field_locked(HK_OBS_CARD|HK_OBS_GW,obs.card_ms);
	lck_mtx_unlock(&obs_lock);
}
static void obs_answer(struct ruc *c,enum ruc_task task,int bytes,struct qmi_response_type_v01 reply,bool has,uint64_t now)
{
	uint32_t bit=0;uint64_t stamp=now/NSEC_PER_MSEC;
	bool good=bytes>0 && !reply.result && !reply.error && has && !ru.changed && c->live;
	lck_mtx_lock(&obs_lock);
	if(task==RU_PHYSICAL){
		bit=HK_OBS_PHYSICAL;
		struct hk_uim_physical_status *s=&c->op.u.value.slots;
		if(good && s->count==2 && s->info_count==2){
			obs.physical_slot=1;obs.logical_slot=s->slots[0].logical_slot;obs.card_present=s->slots[0].card==2;
			obs.slot2_empty=s->slots[1].card==1;obs.physical_ms=stamp;
		}else good=false;
	}else if(task==RU_EXT && !good)bit=HK_OBS_CARD|HK_OBS_GW;
	else if(task==RU_ATR) {
		bit=HK_OBS_ATR;if(good){obs.atr_length=c->op.u.value.scalar.atr_length;obs.atr_present=obs.atr_length>0;obs.atr_ms=stamp;}
	}else if(task==RU_RADIO && c==&ru.c[RUC_DMS]) {
		bit=HK_OBS_DMS;if(good){obs.dms_mode=c->op.u.value.dms.mode;obs.dms_ms=stamp;}
	}else if(task==RU_RADIO && c==&ru.c[RUC_NAS]){
		bit=HK_OBS_NAS;if(good){obs.nas_registration=c->op.f.value.serving.registration;obs.cs_attach=c->op.f.value.serving.cs_attach;
			obs.ps_attach=c->op.f.value.serving.ps_attach;obs.nas_ms=stamp;}
	}else if(task==RU_RADIO && c==&ru.c[RUC_WDS]){
		bit=HK_OBS_WDS;if(good){obs.wds_state=c->op.f.value.status.connection_status;obs.wds_ms=stamp;}
	}
	if(good)obs.valid_mask|=bit;else obs.valid_mask&=~bit;
	if(bit)obs.sequence++;
	if(good && bit)hist_field_locked(bit,stamp);
	lck_mtx_unlock(&obs_lock);
}

/* Private RAM-only activation fixture; no replay. The one physical-owner
 * authorization is reserved and consumed on the host before this boot. */
static struct {
	struct hk_uim_act_once once;
	struct hk_uim_act_stamp physical_stamp;
	struct hk_uim_physical_status physical;
	uint64_t start_ns,deadline,post_at;
	bool primary_latched,authorized,physical_valid,extended_ok,legacy_ok,primary_seen,decision,already;
	unsigned pending,post_rounds,target_mask,ready_mask,mapped_app[2],expected_app;
} ua_board;

static struct hk_uim_act_stamp ua_stamp(struct ruc *u)
{
	return (struct hk_uim_act_stamp){.generation=u->generation,.epoch=1+ru.card_ind+ru.slot_ind,
	    .round=u->round,.node=u->node,.port=u->port,.local=u->local};
}
static bool ua_round_valid(struct ruc *u,uint64_t now)
{
	struct hk_uim_act_stamp s=ua_stamp(u);
	return u->live && u->subscribed && !ru.changed && !ru.extra_servers && ua_board.physical_valid &&
	    now>=ua_board.start_ns && now-ua_board.start_ns<=5ull*NSEC_PER_SEC &&
	    !memcmp(&s,&ua_board.physical_stamp,sizeof(s));
}
static bool ua_application(const struct hk_qmi_uim_status *r)
{
	if (r->slots!=2 || r->logical_count!=2 || r->logical[0].card_state!=1 ||
	    r->logical[0].validity==HK_UIM_VALIDITY_INVALID || r->logical[0].validity==HK_UIM_VALIDITY_OTHER ||
	    r->logical[1].card_state==1 || r->logical[1].app_count!=0) return false;
	const struct hk_uim_logical_slot *v=&r->logical[0];
	return v->app_count>=1 && v->gw_apps==1 && v->sim_states==0 &&
	    (v->usim_states==(1u<<1) || v->usim_states==(1u<<7)) &&
	    r->gw[0].status==HK_UIM_GW_UNASSIGNED && r->gw[1].status==HK_UIM_GW_UNASSIGNED;
}
static void ua_card_evidence(struct ruc *u,const struct hk_qmi_uim_status *r,bool ext)
{
	const struct hk_uim_gw_binding *g=&r->gw[0];
	bool mapped=g->status==HK_UIM_GW_MAPPED;
	if (ext) ua_board.extended_ok=ua_application(r);
	else ua_board.legacy_ok=ua_application(r);
	if (mapped) {
		ua_board.primary_seen=true;ua_board.primary_latched=true;
		if ((g->index>>8)==0 && g->app_type==2 && r->logical_count==2 && r->logical[1].card_state!=1 && !r->logical[1].app_count && r->logical[0].card_state==1 &&
		    r->logical[0].gw_apps==1 && r->logical[0].validity!=HK_UIM_VALIDITY_INVALID &&
		    r->logical[0].validity!=HK_UIM_VALIDITY_OTHER) {
            unsigned ordinal=1u+(g->index&255u), bit=ext?1u:2u;
            ua_board.mapped_app[ext?0:1]=ordinal;
            if(!ua_board.expected_app || ua_board.expected_app==ordinal){
                ua_board.target_mask|=bit;
                if(hk_online_usim_primary_ready(r))ua_board.ready_mask|=bit;
            }
        }
	}
	obs_card(u,r,ext);
	say("modem: uim-once binding gen=%u round=%u ext=%u status=%u slot=%u app=%u app_type=%u app_state=%u\n",
	    u->generation,u->round,(unsigned)ext,g->status,mapped?1u+(g->index>>8):0,
	    mapped?1u+(g->index&255):0,mapped?g->app_type:0,mapped?g->app_state:0);
}
static void ua_snapshot_start(struct ruc *u,uint64_t now)
{
	ua_board.physical_valid=false;ua_board.extended_ok=false;ua_board.legacy_ok=false;
	ua_board.primary_seen=false;ua_board.ready_mask=0;ua_board.mapped_app[0]=ua_board.mapped_app[1]=0;ua_board.target_mask=0;ua_board.start_ns=now;ua_board.physical_stamp=ua_stamp(u);
}
static void ua_after_atr(struct ruc *u,int bytes,uint64_t now)
{
	if (!ua_board.authorized || ua_board.decision || ua_board.pending) return;
	struct hk_uim_obs_scalar *v=&u->op.u.value.scalar;
	bool atr=bytes>0 && !v->result && !v->error && v->present && v->atr_length>0 && u->arg==1;
	bool recent=ua_round_valid(u,now);
	say("modem: uim-once eligibility gen=%u card_epoch=%u round=%u recent=%u physical=%u extended=%u legacy=%u direct_atr=%u primary_seen=%u\n",
	    u->generation,ua_board.physical_stamp.epoch,u->round,(unsigned)recent,(unsigned)ua_board.physical_valid,
	    (unsigned)ua_board.extended_ok,(unsigned)ua_board.legacy_ok,(unsigned)atr,(unsigned)ua_board.primary_seen);
	if (ua_board.primary_seen) {
		if(!recent || !atr || ua_board.target_mask!=3 || ua_board.mapped_app[0]!=ua_board.mapped_app[1])return;
        ua_board.already=true;ua_board.decision=true;ua_board.expected_app=ua_board.mapped_app[0];ua_board.post_at=now;
		return;
	}
	if (ua_board.primary_latched || !recent || !atr || !ua_board.extended_ok || !ua_board.legacy_ok) return;
	int rc=hk_uim_act_choose(&u->handle,&ua_board.once,&ua_board.physical_stamp,&ua_board.physical,v->atr_length);
	say("modem: uim-once choose gen=%u card_epoch=%u round=%u tid=%u rc=%d\n",
	    u->generation,ua_board.physical_stamp.epoch,u->round,ua_board.once.txn.id,rc);
	if (!rc) {ua_board.pending=1;ua_board.deadline=now+1500ull*NSEC_PER_MSEC;}
}
static void ua_progress(struct ruc *u,uint64_t now)
{
	unsigned pending=ua_board.pending;
	if (!pending) return;
	int bytes=0,done=hk_qmi_txn_poll(&ua_board.once.txn,&bytes);
	if (!done && now<ua_board.deadline && !ru.changed && u->live) return;
	if (!done) {qmi_txn_cancel(&ua_board.once.txn);bytes=-ETIMEDOUT;}
	if (done<0) bytes=done;
	ua_board.pending=0;
	if (pending==2) {
		say("modem: uim-once ack gen=%u tid=%u bytes=%d seen=%u result=%u error=%u\n",
		    u->generation,ua_board.once.txn.id,bytes,(unsigned)ua_board.once.ack_seen,
		    ua_board.once.result,ua_board.once.error);
		hk_uim_act_forget(&ua_board.once);ua_board.post_at=now;
		return;
	}
	unsigned choice=ua_board.once.choice;
	bool fresh=ua_round_valid(u,now);
	say("modem: uim-once choice gen=%u card_epoch=%u round=%u bytes=%d type=%u fresh=%u\n",
	    u->generation,ua_board.physical_stamp.epoch,u->round,bytes,choice,(unsigned)fresh);
	if (bytes<=0 || !fresh || choice==HK_UA_NONE) {hk_uim_act_forget(&ua_board.once);return;}
	ua_board.decision=true;ua_board.expected_app=1u+ua_board.once.app_index;
	if (choice==HK_UA_ALREADY) {
		ua_board.already=true;hk_uim_act_forget(&ua_board.once);ua_board.post_at=now;return;
	}
	struct hk_uim_act_stamp s=ua_stamp(u);
	int rc=hk_uim_act_send(&u->handle,&ua_board.once,&s);
	say("modem: uim-once submit msg=56 gen=%u card_epoch=%u round=%u node=%u peer=%u local=%u tid=%u slot=1 primary_gw=1 attempted=%u submitted=%u rc=%d\n",
	    s.generation,s.epoch,s.round,s.node,s.port,s.local,ua_board.once.txn.id,
	    ua_board.once.attempted,ua_board.once.submitted,rc);
	if (!rc) {ua_board.pending=2;ua_board.deadline=now+10ull*NSEC_PER_SEC;}
	else ua_board.post_at=now;
}

static void obs_decision(uint64_t now)
{
	lck_mtx_lock(&obs_lock);
	obs.attempted=ua_board.once.attempted;obs.submitted=ua_board.once.submitted;
	obs.ack_seen=ua_board.once.ack_seen;obs.ack_result=obs.ack_seen?ua_board.once.result:0;obs.ack_error=obs.ack_seen?ua_board.once.error:0;
	obs.post_reads=ua_board.post_rounds;
	unsigned value=HK_OBS_NOT_SUBMITTED;
	if(ua_board.already)value=HK_OBS_ALREADY_BOUND;
	else if(obs.attempted)value=HK_OBS_ATTEMPT_UNCERTAIN;
	if(obs.submitted)value=HK_OBS_SUBMITTED;
	if(obs.ack_seen)value=obs.ack_result?HK_OBS_MODEM_ERROR:HK_OBS_MODEM_ACK;
	if(obs.verified && obs.attempted)value=HK_OBS_READBACK_BOUND;
	if(value!=obs.activation){obs.activation=value;obs.decision_ms=now/NSEC_PER_MSEC;obs.sequence++;}
	hist_uim_locked();
	lck_mtx_unlock(&obs_lock);
}
static void obs_readback(struct ruc *c,uint64_t now)
{
	if (!ua_board.decision || !ua_board.post_rounds || c->task!=RU_WAIT) return;
	/* Confirm by both current solicited logical projections plus same-epoch
	 * physical status. A mode-change ACK alone never sets verified. */
	if (ua_board.target_mask==3 && ua_board.ready_mask==3 && ua_board.expected_app && ua_board.expected_app==ua_board.mapped_app[0] && ua_board.expected_app==ua_board.mapped_app[1] && ua_round_valid(c,now) &&
	    c->op.u.value.scalar.present && !c->op.u.value.scalar.result && c->op.u.value.scalar.atr_length) {
		lck_mtx_lock(&obs_lock);
		obs.verified=1;obs.verified_ms=now/NSEC_PER_MSEC;obs.post_reads=ua_board.post_rounds;obs.sequence++;
		lck_mtx_unlock(&obs_lock);
		say("modem: uim-once independent verified=1 round=%u primary_slot=1 primary_app=%u usim=1\n",c->round,ua_board.expected_app);
	} else {
		lck_mtx_lock(&obs_lock);obs.verified=0;obs.verified_ms=0;obs.sequence++;lck_mtx_unlock(&obs_lock);
	}
	obs_decision(now);
}

static void ru_begin(struct ruc *c,enum ruc_task task,uint32_t arg,uint64_t now)
{
	int rc;unsigned msg;
	if (task==RU_PHYSICAL) ua_snapshot_start(c,now);
	c->task=task;c->arg=arg;c->queried=true;
	if (task==RU_RADIO) {
		if (c==&ru.c[RUC_DMS]) {msg=0x2d;rc=hk_qmi_uim_obs_begin(&c->handle,&c->op.u,HK_UO_DMS,0);}
		else if (c==&ru.c[RUC_NAS]) {msg=0x24;rc=hk_qmi_family_begin(&c->handle,&c->op.f,HK_FQ_SERVING,0);}
		else {msg=0x22;rc=hk_qmi_family_begin(&c->handle,&c->op.f,HK_FQ_STATUS,0);}
	} else {
		static const enum hk_uim_obs_query kinds[]={HK_UO_REGISTER,HK_UO_SLOTS,HK_UO_EXTENDED,HK_UO_LEGACY,HK_UO_ATR,HK_UO_DMS,HK_UO_REGISTER};
		static const unsigned ids[]={0x2e,0x47,0x2f,0x2f,0x41,0,0x2e};
		msg=ids[task];rc=hk_qmi_uim_obs_begin(&c->handle,&c->op.u,kinds[task],arg);
	}
	c->pending=rc==0;
	c->deadline=now+1500000000ull;
	if (rc) {c->finished=true;c->task=RU_WAIT;}
	say("modem: rfs-state issue service=%u local=%u gen=%u round=%u task=%u msg=%u tid=%u arg=%u rc=%d uptime_ms=%llu\n",
	    c->service,c->local,c->generation,c->round,task,msg,ru_txn(c)->id,arg,rc,(unsigned long long)(now/NSEC_PER_MSEC));
}
static void ru_cards(struct ruc *c,const struct hk_qmi_uim_status *s,bool ext)
{
	ua_card_evidence(c,s,ext);
	for(unsigned i=0;i<s->logical_count;i++){
		const struct hk_uim_logical_slot *v=&s->logical[i];
		say("modem: rfs-state card round=%u ext=%u gen=%u slot=%u state=%u validity=%u error_valid=%u error=%u apps=%u sim=%u usim=%u ruim=%u csim=%u isim=%u ready_gw=%u\n",
		    c->round,(unsigned)ext,c->generation,i+1,v->card_state,v->validity,v->error_valid,v->error,
		    v->app_count,v->type_states[1],v->type_states[2],v->type_states[3],v->type_states[4],v->type_states[5],v->ready_gw_apps);
	}
	say("modem: rfs-state GW round=%u ext=%u gen=%u primary=%u secondary=%u confirmed=%u\n",
	    c->round,(unsigned)ext,c->generation,s->gw[0].status,s->gw[1].status,s->confirmed_ready_apps);
}
static void ru_answer(struct ruc *c,int bytes,uint64_t now)
{
	enum ruc_task task=c->task;
	struct qmi_response_type_v01 reply={1,0};bool has=false;
	if (bytes>0 && c->live && !ru.changed) {
		if (task==RU_SUBSCRIBE || task==RU_CLEAR || task==RU_ATR) {
			struct hk_uim_obs_scalar *v=&c->op.u.value.scalar;
			reply=(struct qmi_response_type_v01){v->result,v->error};has=v->present;
			say("modem: rfs-state scalar gen=%u round=%u task=%u request=%u present=%u mask=%u atr_length=%u\n",
			    c->generation,c->round,task,c->arg,v->present,v->mask,v->atr_length);
			if (task==RU_SUBSCRIBE && !v->result && !v->error && v->present && (v->mask&0x15)==0x15) {
				c->subscribed=true;ru.subscribed_ms=now/NSEC_PER_MSEC;
			}
			if(task==RU_CLEAR && !v->result && !v->error && v->present && !v->mask)c->subscribed=false;
		} else if (task==RU_PHYSICAL) {
			struct hk_uim_physical_status *v=&c->op.u.value.slots;reply=v->response;has=v->valid&&v->info_valid;
			if (!reply.result && has) {ua_board.physical=*v;ua_board.physical_valid=true;}
			if(!reply.result && has)for(unsigned i=0;i<v->count && i<2;i++)
				say("modem: rfs-state physical round=%u gen=%u slot=%u card=%u active=%u logical=%u protocol=%u atr=%u\n",
				    c->round,c->generation,i+1,v->slots[i].card,v->slots[i].activity,v->slots[i].logical_slot,v->slots[i].protocol,v->slots[i].atr_present);
		} else if(task==RU_EXT) {
			struct hk_uim_obs_extended *v=&c->op.u.value.ext;reply=(struct qmi_response_type_v01){v->result,v->error};has=v->extended;
			if(!reply.result && has) ru_cards(c,&v->card,true);
		} else if(task==RU_CARD) {
			struct hk_qmi_uim_status *v=&c->op.u.value.card;reply=v->response;has=v->valid;
			if(!reply.result && has) ru_cards(c,v,false);
		} else if(c==&ru.c[RUC_DMS]) {
			struct hk_qmi_dms_operating_mode_resp *v=&c->op.u.value.dms;reply=v->response;has=v->mode_valid;
			if(!reply.result && has)say("modem: rfs-state DMS round=%u mode=%u\n",c->round,v->mode);
		} else if(c==&ru.c[RUC_NAS]) {
			struct hk_qmi_nas_serving_status *v=&c->op.f.value.serving;reply=v->response;has=v->valid;
			if(!reply.result && has)say("modem: rfs-state NAS round=%u reg=%u cs=%u ps=%u\n",c->round,v->registration,v->cs_attach,v->ps_attach);
		} else {
			struct hk_qmi_wds_packet_service_status_resp *v=&c->op.f.value.status;reply=v->response;has=v->connection_status_valid;
			if(!reply.result && has)say("modem: rfs-state WDS round=%u state=%u\n",c->round,v->connection_status);
		}
	}
	say("modem: rfs-state reply service=%u local=%u gen=%u round=%u task=%u tid=%u bytes=%d result=%u error=%u has=%u uptime_ms=%llu\n",
	    c->service,c->local,c->generation,c->round,task,ru_txn(c)->id,bytes,reply.result,reply.error,(unsigned)has,
	    (unsigned long long)(now/NSEC_PER_MSEC));
	obs_answer(c,task,bytes,reply,has,now);
	cold_record_radio(c,task,bytes,reply,has,now);
	c->task=RU_WAIT;
	if (!c->live || ru.changed) return;
	switch(task){
	case RU_SUBSCRIBE: if(c->subscribed)ru_begin(c,RU_PHYSICAL,0,now);else c->finished=true;break;
	case RU_PHYSICAL: ru_begin(c,RU_EXT,0,now);break;
	case RU_EXT: ru_begin(c,RU_CARD,0,now);break;
	case RU_CARD: ru_begin(c,RU_ATR,1,now);break;
	case RU_ATR:
		ua_after_atr(c,bytes,now);obs_decision(now);obs_readback(c,now);
		if (!ua_board.authorized && c->late && !ua_board.pending && (!ua_board.decision || ua_board.post_rounds>=2)) ru_begin(c,RU_CLEAR,0,now);
		break;
	case RU_CLEAR: c->finished=true;break;
	default:break;
	}
}
enum cold_stage { COLD_IDLE,COLD_FRESH,COLD_TS_LIST,COLD_TS_CURRENT,COLD_LOW_ACK,COLD_LOW_WAIT,COLD_LOW_READ,COLD_MODE1,COLD_MODE2,COLD_ON,COLD_WATCH,COLD_DONE };
static struct {
	struct qmi_handle ts;
	struct hk_online_ts_op list,temp;
	struct hk_online_once cmd;
	struct hk_uim_obs_op mode[2];
	uint32_t tsnode,tsport,tslocal,tsgen,seq;
	uint64_t until,issued_ms,last_safe,next_temp,fresh_round_ms;
	unsigned stage,round,sweep,pending_mode,slot_epoch;
	bool listing,open,attached,ended,started,polled,ts_busy,ts_ack,mode_busy,cmd_busy,attempt_called,already,rf_possible;
} cold;
static unsigned pro_issued,pro_last_firmware,pro_last_nas,pro_last_mode;
static bool pro_have_nas,pro_have_mode;
static uint64_t pro_retry_after;

static uint64_t cold_ms(void){return op_nanoseconds(NULL)/NSEC_PER_MSEC;}
static struct hk_online_scope cold_scope(void)
{
	struct ruc *u=&ru.c[RUC_UIM],*d=&ru.c[RUC_DMS];struct hk_online_scope s={0};
	s.uim=ua_stamp(u);s.dms=s.uim;s.ts=s.uim;
	s.dms.generation=d->generation;s.dms.node=d->node;s.dms.port=d->port;s.dms.local=d->local;
	s.ts.generation=cold.tsgen;s.ts.node=cold.tsnode;s.ts.port=cold.tsport;s.ts.local=cold.tslocal;
	return s;
}
static int cold_ts_arrive(struct qmi_handle *q,struct qmi_service *s)
{
	if(!mdm_abp_is_owner(NULL)||q!=&cold.ts||!s||s->service!=HK_QMI_TS_SERVICE||s->version!=1||s->instance!=100||cold.attached||!s->node||!s->port)return ENXIO;
	int rc=hk_qmi_connect(q->sock,s->node,s->port);
	if(!rc){cold.tsgen++;cold.tsnode=s->node;cold.tsport=s->port;cold.attached=true;}
	return rc;
}
static void cold_ts_depart(struct qmi_handle *q,struct qmi_service *s)
{
	if(q==&cold.ts && s && s->node==cold.tsnode && s->port==cold.tsport){cold.attached=false;cold.tsgen++;}
}
static const struct qmi_ops cold_ts_ops={.new_server=cold_ts_arrive,.del_server=cold_ts_depart};
static void cold_ts_ind(struct qmi_handle *q,struct sockaddr_qrtr *p,__unused struct qmi_txn *t,const void *d)
{
	if(!mdm_abp_is_owner(NULL)||!cold.attached)return;
	struct hk_uim_act_stamp stamp=cold.temp.stamp;
	stamp.generation=cold.tsgen;stamp.node=cold.tsnode;stamp.port=cold.tsport;stamp.local=cold.tslocal;
	hk_online_ts_ind(&cold.temp,q,&stamp,p,d,cold_ms());
}
static const struct qmi_msg_handler cold_handlers[]={
	{QMI_INDICATION,HK_QMI_TS_TEMP_REPORT,hk_qmi_ts_report_ei,sizeof(struct hk_qmi_ts_report),cold_ts_ind},{}};
static void cold_stop(const char *why)
{
	if(cold.stage==COLD_DONE)return;
	say("modem: gw-online stop reason=%s attempted=%u submitted=%u ack=%u rf_possible=%u\n",why,cold.cmd.attempted,cold.cmd.submitted,cold.cmd.ack_seen,(unsigned)cold.rf_possible);
	cold.stage=COLD_DONE;
	if(cold.rf_possible)__atomic_store_n(&cold_modem_stop,1,__ATOMIC_RELEASE);
}
static bool cold_card_ok(uint64_t now, unsigned round)
{
	struct ruc *u=&ru.c[RUC_UIM];
	const struct hk_uim_physical_status *p=&ua_board.physical;
	return ua_board.authorized && ua_board.decision && !ua_board.pending && !u->pending &&
	    !u->finished && u->task==RU_WAIT && !ru.changed && !ru.extra_servers && u->subscribed &&
	    ua_round_valid(u,now*NSEC_PER_MSEC) && u->round==round && ua_board.ready_mask==3 && ua_board.target_mask==3 &&
	    ua_board.mapped_app[0] && ua_board.mapped_app[0]==ua_board.mapped_app[1] &&
	    ua_board.expected_app==ua_board.mapped_app[0] &&
	    p->count==2 && p->info_count==2 && p->slots[0].card==2 && p->slots[0].activity==1 && p->slots[0].logical_slot==1 &&
	    p->slots[0].protocol==2 && p->slots[0].atr_present && p->slots[1].card==1 && !p->slots[1].atr_present &&
	    p->slots[1].logical_slot!=1 && u->op.u.value.scalar.present && !u->op.u.value.scalar.result && u->op.u.value.scalar.atr_length;
}
static bool cold_wds_ok(uint64_t now)
{
	struct ruc *c=&ru.c[RUC_WDS];
	return c->live && !c->pending && c->round==cold.round && cold_wds_when && now>=cold_wds_when && now-cold_wds_when<=3000 &&
	    c->op.f.txn.done && c->op.f.txn.result>0 && !c->op.f.value.status.response.result &&
	    c->op.f.value.status.connection_status_valid && c->op.f.value.status.connection_status==1;
}
static int cold_query_ts(bool listing,uint64_t now)
{
	struct hk_online_ts_op *o=listing?&cold.list:&cold.temp;
	*o=(struct hk_online_ts_op){0};
	struct hk_online_scope scope=cold_scope();
	uint32_t seq=listing?0:++cold.seq;
	int rc=hk_online_ts_begin(&cold.ts,o,&scope.ts,seq,now);
	cold.listing=listing;cold.ts_busy=!rc;cold.ts_ack=false;cold.until=now+1300;
	return rc;
}
static int cold_ts_poll(bool listing,uint64_t now)
{
	struct hk_online_ts_op *o=listing?&cold.list:&cold.temp;
	if(!cold.attached)return -ENOTCONN;
	if(cold.ts_busy){
		int bytes=0,rc=hk_qmi_txn_poll(&o->txn,&bytes);
		if(!rc && now<cold.until)return 0;
		if(!rc){qmi_txn_cancel(&o->txn);bytes=-ETIMEDOUT;}
		cold.ts_busy=false;
		if(rc<0||bytes<=0)return -EIO;
		cold.ts_ack=true;
	}
	if(listing)return cold.ts_ack && !o->list.response.result && o->list.valid && o->list.has_modem_tsens?1:-EIO;
	if(!cold.ts_ack || o->ack.result || o->ack.error || o->conflict)return -EIO;
	if(!o->sampled)return now<cold.until?0:-ETIMEDOUT;
	if(now<o->sample_ms || now-o->sample_ms>3000 || o->sample_mc<0 || o->sample_mc>=50000)return -ERANGE;
	cold.last_safe=o->sample_ms;
	say("modem: gw-online thermal seq=%u local=%u node=%u port=%u mc=%d sampled_ms=%llu\n",o->seq,cold.tslocal,cold.tsnode,cold.tsport,o->sample_mc,(unsigned long long)o->sample_ms);
	return 1;
}
static int cold_begin_mode(unsigned i,uint64_t now)
{
	struct ruc *d=&ru.c[RUC_DMS];if(!d->live)return -ENOTCONN;
	int rc=hk_qmi_uim_obs_begin(&d->handle,&cold.mode[i],HK_UO_DMS,0);
	cold.pending_mode=i;cold.mode_busy=!rc;cold.until=now+1000;
	return rc;
}
static int cold_mode_poll(unsigned i,uint64_t now)
{
	struct hk_uim_obs_op *op=&cold.mode[i];int bytes=0,rc=hk_qmi_txn_poll(&op->txn,&bytes);
	if(!rc && now<cold.until)return 0;
	if(!rc){qmi_txn_cancel(&op->txn);bytes=-ETIMEDOUT;}
	cold.mode_busy=false;
	struct hk_qmi_dms_operating_mode_resp *s=&op->value.dms;
	bool valid=rc>0 && bytes>0 && !s->response.result && !s->response.error && s->mode_valid;
	say("modem: gw-online pre_mode sample=%u gen=%u tid=%u bytes=%d valid=%u mode=%u reason_present=%u reason=%u restriction_present=%u restricted=%u\n",
	    i+1,ru.c[RUC_DMS].generation,op->txn.id,bytes,(unsigned)valid,s->mode,s->offline_reason_valid,s->offline_reason,s->hardware_restricted_valid,s->hardware_restricted);
	return valid?1:-EIO;
}
static struct {
	struct hk_online_once once;
	struct hk_uim_obs_op read;
	uint64_t next,deadline;
	unsigned reads,expected_mode;
	bool busy,read_busy,recheck,skipped;
} donor_low;

static bool donor_low_continuity(const struct hk_online_scope *scope,unsigned mode)
{
	struct hk_online_scope expected=donor_low.once.scope;
	expected.uim.round=scope->uim.round;
	expected.dms.round=scope->dms.round;
	expected.ts.round=scope->ts.round;
	return mode==donor_low.expected_mode && !memcmp(&expected,scope,sizeof(expected));
}

static void donor_low_begin(uint64_t now,const struct hk_online_scope *scope)
{
	struct ruc *d=&ru.c[RUC_DMS];
	int rc=hk_donor_low_send(&d->handle,&donor_low.once,scope,&cold.mode[1],&cold.list,&cold.temp,
	    cold_card_ok(now,cold.round),cold_wds_ok(now),now);
	say("modem: donor-low submit target=1 from=%u gen=%u local=%u node=%u port=%u tid=%u attempted=%u submitted=%u rc=%d\n",
	    cold.mode[1].value.dms.mode,d->generation,d->local,d->node,d->port,donor_low.once.txn.id,
	    donor_low.once.attempted,donor_low.once.submitted,rc);
	if(rc || !donor_low.once.submitted){cold_stop("low_submission_failed");return;}
	donor_low.busy=true;donor_low.deadline=now+1500;cold.stage=COLD_LOW_ACK;
}

static bool donor_low_tick(uint64_t now)
{
	if(cold.stage!=COLD_LOW_ACK && cold.stage!=COLD_LOW_WAIT && cold.stage!=COLD_LOW_READ)return false;
	struct ruc *d=&ru.c[RUC_DMS];
	struct hk_online_scope scope=cold_scope();
	if(!d->live || !cold.attached || memcmp(&scope,&donor_low.once.scope,sizeof(scope)) ||
	    now<cold.last_safe || now-cold.last_safe>3000 || cold.temp.conflict ||
	    cold.temp.sample_mc<0 || cold.temp.sample_mc>=45000) {
		cold_stop("low_scope_or_thermal_changed");return true;
	}
	if(cold.stage==COLD_LOW_ACK){
		int bytes=0,rc=hk_qmi_txn_poll(&donor_low.once.txn,&bytes);
		if(!rc && now<donor_low.deadline)return true;
		if(!rc){qmi_txn_cancel(&donor_low.once.txn);bytes=-ETIMEDOUT;}
		donor_low.busy=false;
		bool paired=rc>0 && bytes>0 && hk_donor_low_ack(&d->handle,&donor_low.once,&scope);
		say("modem: donor-low ack target=1 gen=%u tid=%u bytes=%d matched=%u result=%u error=%u\n",
		    d->generation,donor_low.once.txn.id,bytes,(unsigned)paired,donor_low.once.result,donor_low.once.error);
		if(!paired){cold_stop("low_unmatched_or_missing_ack");return true;}
		donor_low.next=now;cold.stage=COLD_LOW_WAIT;
	}
	if(cold.stage==COLD_LOW_WAIT){
		if(now<donor_low.next)return true;
		if(hk_qmi_uim_obs_begin(&d->handle,&donor_low.read,HK_UO_DMS,0)){
			cold_stop("low_readback_request");return true;
		}
		donor_low.reads++;donor_low.read_busy=true;donor_low.deadline=now+1000;cold.stage=COLD_LOW_READ;
	}
	int bytes=0,rc=hk_qmi_txn_poll(&donor_low.read.txn,&bytes);
	if(!rc && now<donor_low.deadline)return true;
	if(!rc){qmi_txn_cancel(&donor_low.read.txn);bytes=-ETIMEDOUT;}
	donor_low.read_busy=false;
	bool valid=rc>0 && bytes>0 && hk_donor_low_read(&d->handle,&donor_low.once,&scope,&donor_low.read);
	unsigned mode=valid?donor_low.read.value.dms.mode:255;
	say("modem: donor-low read sample=%u gen=%u tid=%u set_tid=%u valid=%u mode=%u bytes=%d\n",
	    donor_low.reads,d->generation,donor_low.read.txn.id,donor_low.once.txn.id,(unsigned)valid,mode,bytes);
	if(!valid){cold_stop("low_invalid_or_restricted_readback");return true;}
	if(!mode){
		cold.slot_epoch=ru.slot_ind;cold.already=true;cold.rf_possible=true;
		cold.stage=COLD_WATCH;cold.issued_ms=now;cold.next_temp=now;
		say("modem: donor-low decision mode=0 confirmed_low=0 action=already_online\n");
		return true;
	}
	if(mode==3 && !donor_low.once.result){
		if(donor_low.reads>=4){cold_stop("low_ack_without_transition");return true;}
		donor_low.next=now+150*donor_low.reads;cold.stage=COLD_LOW_WAIT;return true;
	}
	/* Recollect CARD/extended/slots/ATR, WDS, and the TS list/current evidence;
	 * the two ordinary DMS guard reads are also new. Keep the LOW_POWER latch. */
	donor_low.expected_mode=mode;donor_low.recheck=true;
	say("modem: donor-low decision mode=%u confirmed_low=%u action=fresh_online_gates\n",
	    mode,(unsigned)(mode==1 && !donor_low.once.result));
	struct ruc *u=&ru.c[RUC_UIM];
	u->round=++ru.round;cold.stage=COLD_IDLE;
	ru_begin(u,RU_PHYSICAL,0,now*NSEC_PER_MSEC);
	return true;
}

static void cold_watch(uint64_t now)
{
	static const unsigned offsets[]={0,5000,15000,30000,60000,89000};
	if(ru.changed || ru.slot_ind!=cold.slot_epoch || !ru.c[RUC_UIM].live || !cold.attached || !ru.c[RUC_DMS].live || now<cold.last_safe || now-cold.last_safe>3000){
		cold_stop("monitor_lost");return;
	}
	if(cold.cmd_busy){
		int bytes=0,rc=hk_qmi_txn_poll(&cold.cmd.txn,&bytes);
		if(rc || now>=cold.issued_ms+2000) {
			if(!rc){qmi_txn_cancel(&cold.cmd.txn);bytes=-ETIMEDOUT;}
			cold.cmd_busy=false;
			hist_online_ack(ru.c[RUC_DMS].generation,cold.cmd.txn.id,cold.cmd.ack_seen,bytes,cold.cmd.result,cold.cmd.error,now);
			say("modem: gw-online ACK tid=%u bytes=%d seen=%u result=%u error=%u\n",cold.cmd.txn.id,bytes,cold.cmd.ack_seen,cold.cmd.result,cold.cmd.error);
		}
	}
	if(cold.polled){
		int rc=cold_ts_poll(false,now);
		if(rc<0){cold_stop("thermal_unavailable");return;}
		if(rc>0){cold.polled=false;cold.next_temp=now+1000;}
	}else if(now>=cold.next_temp){
		if(cold_query_ts(false,now)){cold_stop("thermal_request_failed");return;}
		cold.polled=true;
	}
	unsigned elapsed=(unsigned)(now-cold.issued_ms);
	if(cold.sweep<sizeof(offsets)/sizeof(offsets[0]) && elapsed>=offsets[cold.sweep] &&
	    !ru.c[RUC_DMS].pending && !ru.c[RUC_NAS].pending && !ru.c[RUC_WDS].pending) {
		say("modem: gw-online sweep sample=%u elapsed_ms=%u\n",cold.sweep,elapsed);cold.sweep++;
		for(unsigned i=RUC_DMS;i<RUC_COUNT;i++)if(ru.c[i].live)ru_begin(&ru.c[i],RU_RADIO,0,now*NSEC_PER_MSEC);
	}
	if(cold_refused_offline && !cold.cmd_busy && elapsed>=5000) {
		cold.rf_possible=false;cold_stop("refused_original_mode_confirmed");return;
	}
	if(elapsed>=90000)cold_stop("window_90s");
}
/* The current completed round itself supplies the two independent UIM
 * projections and the direct ATR. The display is an independent reader. */
static void cold_consider_ready(struct ruc *u,uint64_t now)
{
    if(cold.stage!=COLD_IDLE || !ua_board.post_rounds || !cold_card_ok(now,u->round))return;
    cold.round=u->round;
    cold.fresh_round_ms=now;
    cold.stage=COLD_FRESH;
    say("modem: gw-online fresh round=%u uptime_ms=%llu trigger=independent_ready\n",
        cold.round,(unsigned long long)cold.fresh_round_ms);
}
static void cold_tick(void)
{
	uint64_t now=cold_ms();
	if(!cold.started){
		cold.started=true;
		int rc=qmi_handle_init_transport(&cold.ts,8192,&cold_ts_ops,cold_handlers,hk_qrtr_mhi_ops());
		if(!rc){
			cold.open=true;
			for(unsigned i=0;i<HK_QRTR_CORE_MAX_PORTS;i++){
				struct hk_qrtr_core_port *p=&mdm_qrtr_mhi.core.ports[i];
				if(p->context==(void*)cold.ts.sock && p->receive){cold.tslocal=p->number;break;}
			}
			rc=cold.tslocal?qmi_add_lookup(&cold.ts,HK_QMI_TS_SERVICE,1,100):-ENXIO;
		}
		if(rc)cold_stop("thermal_endpoint_failed");
		return;
	}
	if(cold.stage==COLD_DONE || cold.stage==COLD_IDLE)return;
	if(cold.stage==COLD_WATCH){cold_watch(now);return;}
	if(cold.stage==COLD_FRESH){
		if(now>cold.fresh_round_ms+6000){cold_stop("ready_mapping_or_status_not_confirmed");return;}
		if(!cold_card_ok(now,cold.round) || !cold_wds_ok(now) || !cold.attached)return;
		if(cold_query_ts(true,now)){cold_stop("list_request");return;}
		cold.stage=COLD_TS_LIST;return;
	}
	if(!cold_card_ok(now,cold.round) || !cold_wds_ok(now)){cold_stop("prerequisite_changed");return;}
	if(donor_low_tick(now)) return;
	if(cold.stage==COLD_TS_LIST || cold.stage==COLD_TS_CURRENT){
		bool list=cold.stage==COLD_TS_LIST;
		int rc=cold_ts_poll(list,now);
		if(!rc)return;
		if(rc<0){cold_stop(list?"list_failed":"sample_failed");return;}
		if(list){if(cold_query_ts(false,now)){cold_stop("sample_request");return;}cold.stage=COLD_TS_CURRENT;}
		else{
			if(cold.temp.sample_mc>=45000 || cold_begin_mode(0,now)){cold_stop("initial_temperature_or_mode");return;}
			cold.stage=COLD_MODE1;
		}
		return;
	}
	unsigned i=cold.stage==COLD_MODE1?0:1;int rc=cold_mode_poll(i,now);
	if(!rc)return;
	if(rc<0){cold_stop("invalid_mode");return;}
	struct hk_online_scope scope=cold_scope();struct ruc *d=&ru.c[RUC_DMS];
	if(!i){
		if(cold.mode[0].value.dms.mode==0){
			cold.slot_epoch=ru.slot_ind;cold.already=true;cold.rf_possible=true;cold.stage=COLD_WATCH;cold.issued_ms=now;cold.next_temp=now;
			say("modem: gw-online already_online=1 command=0\n");return;
		}
		bool prelude=pro_active && !donor_low.once.arm_checked && !donor_low.skipped &&
		    cold.mode[0].value.dms.mode==3;
		if (pro_active && !prelude && !donor_low.once.arm_checked && !donor_low.skipped) {
			donor_low.skipped=true;
			say("modem: donor-low skip mode=%u target=1 attempted=0\n",cold.mode[0].value.dms.mode);
		}
		if (donor_low.recheck && !donor_low_continuity(&scope,cold.mode[0].value.dms.mode)) {
			cold_stop("post_low_scope_or_mode_changed");return;
		}
		rc=hk_online_arm(&d->handle,prelude?&donor_low.once:&cold.cmd,&scope,&cold.mode[0],now);
		if(rc || cold_begin_mode(1,now)){cold_stop("original_mode_restricted");return;}
		cold.stage=COLD_MODE2;return;
	}
	if(donor_low.once.armed && !donor_low.once.attempted) {
		donor_low_begin(now,&scope);
		return;
	}
	donor_low.recheck=false;
	cold.slot_epoch=ru.slot_ind;cold.attempt_called=true;
    pro_last_firmware=mdm_rfs_board_completed();
    struct ruc *nas=&ru.c[RUC_NAS];
    pro_have_nas=nas->live && !nas->pending && nas->round==cold.round && nas->op.f.txn.done &&
        nas->op.f.txn.result>0 && !nas->op.f.value.serving.response.result && nas->op.f.value.serving.valid;
    if(pro_have_nas)pro_last_nas=nas->op.f.value.serving.registration;
    pro_have_mode=true;pro_last_mode=cold.mode[1].value.dms.mode;
	rc=hk_online_send(&d->handle,&cold.cmd,&scope,&cold.mode[1],&cold.list,&cold.temp,cold_card_ok(now,cold.round),cold_wds_ok(now),now);
	hist_online_submit(d->generation,cold.cmd.txn.id,cold.cmd.attempted,cold.cmd.submitted,now);
	say("modem: gw-online submit msg=46 target=0 gen=%u local=%u node=%u peer=%u tid=%u attempted=%u submitted=%u rc=%d\n",
	    d->generation,d->local,d->node,d->port,cold.cmd.txn.id,cold.cmd.attempted,cold.cmd.submitted,rc);
	if(!cold.cmd.attempted){cold_stop("arm_rejected");return;}
	pro_issued++;
    const unsigned backoff_ms=pro_issued==1?15000u:pro_issued==2?30000u:60000u;
    pro_retry_after=now+backoff_ms;
    say("modem: donor next_evaluation attempt=%u wait_ms=%u earliest_ms=%llu\n",pro_issued,backoff_ms,(unsigned long long)pro_retry_after);
    cold.rf_possible=true;cold.issued_ms=now;cold.next_temp=now+750;cold.cmd_busy=!rc;cold.stage=COLD_WATCH;
}
static void cold_end(void)
{
	if(donor_low.busy)qmi_txn_cancel(&donor_low.once.txn);
	if(donor_low.read_busy)qmi_txn_cancel(&donor_low.read.txn);
	if(cold.cmd_busy)qmi_txn_cancel(&cold.cmd.txn);
	if(cold.ts_busy)qmi_txn_cancel(cold.listing?&cold.list.txn:&cold.temp.txn);
	if(cold.mode_busy)qmi_txn_cancel(&cold.mode[cold.pending_mode].txn);
	if(cold.open){qmi_handle_release(&cold.ts);cold.open=false;}
	say("modem: gw-online end attempted=%u submitted=%u ack=%u result=%u error=%u already=%u sweeps=%u thermal_samples=%u\n",
	    cold.cmd.attempted,cold.cmd.submitted,cold.cmd.ack_seen,cold.cmd.result,cold.cmd.error,(unsigned)cold.already,cold.sweep,cold.seq);
}


static void cold_record_radio(struct ruc *c,enum ruc_task task,int bytes,struct qmi_response_type_v01 reply,bool has,uint64_t ns)
{
    if(task!=RU_RADIO)return;
    uint64_t now=ns/NSEC_PER_MSEC;
    bool valid=bytes>0 && has && !reply.result && !reply.error && c->live && !ru.changed;
    if(c==&ru.c[RUC_WDS]) cold_wds_when=valid?now:0;
    if(c==&ru.c[RUC_NAS] && valid) {
        const struct hk_qmi_nas_serving_status *s=&c->op.f.value.serving;
        say("modem: gw-online nas round=%u gen=%u tid=%u reg=%u cs=%u ps=%u roam_valid=%u roam=%u uptime_ms=%llu\n",
            c->round,c->generation,c->op.f.txn.id,s->registration,s->cs_attach,s->ps_attach,s->roaming_valid,s->roaming,(unsigned long long)now);
    }
    if(cold.stage!=COLD_WATCH || now<cold.issued_ms)return;
    if(c==&ru.c[RUC_DMS] && valid && cold.cmd.ack_seen && cold.cmd.result && c->op.u.value.dms.mode==cold.cmd.original.mode)
        cold_refused_offline=true;
    say("modem: gw-online observe service=%u gen=%u tid=%u valid=%u elapsed_ms=%llu\n",
        c->service,c->generation,ru_txn(c)->id,(unsigned)valid,(unsigned long long)(now-cold.issued_ms));
}

/* After a confirmed temporary firmware refusal, recheck all real inputs on the backed-off owner thread. */
static bool pro_retry_ready(uint64_t now_ms)
{
    struct ruc *d=&ru.c[RUC_DMS], *n=&ru.c[RUC_NAS], *u=&ru.c[RUC_UIM];
    if(!pro_active || !ua_board.authorized || cold.stage!=COLD_DONE || !cold_refused_offline ||
        !cold.cmd.ack_seen || cold.cmd.result!=1 || cold.cmd.error!=52 || cold.rf_possible ||
        now_ms<pro_retry_after || ru.changed || ru.slot_ind!=cold.slot_epoch || !u->live ||
        !cold.attached || !d->live || !n->live || !u->subscribed || cold.cmd_busy || cold.ts_busy || cold.mode_busy ||
        d->generation!=cold.cmd.scope.dms.generation || cold.tsgen!=cold.cmd.scope.ts.generation ||
        u->generation!=cold.cmd.scope.uim.generation || ru.extra_servers)
        return false;
    bool firmware=mdm_rfs_board_completed()>pro_last_firmware;
    bool mode=pro_have_mode && !d->pending && d->round==u->round && d->op.u.txn.done &&
        d->op.u.txn.result>0 && !d->op.u.value.dms.response.result && d->op.u.value.dms.mode_valid &&
        d->op.u.value.dms.mode!=pro_last_mode;
    bool registration=pro_have_nas && !n->pending && n->round==u->round && n->op.f.txn.done &&
        n->op.f.txn.result>0 && !n->op.f.value.serving.response.result && n->op.f.value.serving.valid &&
        n->op.f.value.serving.registration!=pro_last_nas;
    /* Firmware may become ready while its exported observations stay
                 * unchanged. A proven temporary refusal, the same live endpoints
                 * and elapsed backoff permit a fresh full readiness evaluation. */
                if(now_ms>=ru.began/NSEC_PER_MSEC+218000ull) return false;
    say("modem: donor reevaluate prior_error=52 backoff_elapsed=1 firmware_delta=%u mode_delta=%u registration_delta=%u attempts=%u\n",
        (unsigned)firmware,(unsigned)mode,(unsigned)registration,pro_issued);
    /* Every former transaction was collected. Keep the actual socket, sensor
     * sequence and immutable prior receipt; get a completely new evidence round. */
    cold.cmd=(struct hk_online_once){0};cold.stage=COLD_IDLE;cold.sweep=0;
    cold.polled=false;cold_refused_offline=false;
    return true;
}
static void mdm_rfs_uim_poll(struct hk_abp_nas_worker *w)
{
	if (!mdm_abp_is_owner(NULL) || hk_qo_phase(&w->gate)!=HK_QO_READY) return;
	uint64_t now=op_nanoseconds(NULL);
	if (!ru.armed) {
		uint32_t once_ticket=0;
		ua_board.authorized=pro_active && PE_parse_boot_argn("pro_rf",&once_ticket,sizeof(once_ticket)) && once_ticket!=0;
		say("modem: uim-once permit enabled=%u ticket=%u\n",(unsigned)ua_board.authorized,once_ticket);
		ru.armed=true;ru.began=now;obs_phase(HK_OBS_LIVE);
		static const unsigned svc[]={HK_QMI_UIM_SERVICE,HK_QMI_DMS_SERVICE,HK_QMI_NAS_SERVICE,HK_QMI_WDS_SERVICE};
		for (unsigned i=0;i<RUC_COUNT;i++){
			struct ruc *c=&ru.c[i];c->service=svc[i];c->task=RU_WAIT;
			int rc=qmi_handle_init_transport(&c->handle,8192,&ru_ops,i==RUC_UIM?ru_handlers:NULL,hk_qrtr_mhi_ops());
			if (!rc) {
				c->open=true;
				for(unsigned n=0;n<HK_QRTR_CORE_MAX_PORTS;n++)
					if(w->mhi->core.ports[n].context==(void*)c->handle.sock && w->mhi->core.ports[n].receive) {c->local=w->mhi->core.ports[n].number;break;}
				rc=c->local?qmi_add_lookup(&c->handle,c->service,1,0):-ENXIO;
			}
			if(rc){ru.changed=true;say("modem: rfs-state unavailable service=%u rc=%d\n",c->service,rc);}
		}
	}
	struct ruc *u=&ru.c[RUC_UIM];
	ua_progress(u,now);obs_decision(now);
    if(ua_board.authorized){
        cold_consider_ready(u,now/NSEC_PER_MSEC);
        cold_tick();
    }
    if (ua_board.authorized && ((cold.stage==COLD_DONE && (!pro_active || now>=ru.began+218ull*NSEC_PER_SEC)) ||
        (cold.stage==COLD_WATCH && cold_ms()>=cold.issued_ms+89500)) &&
        u->subscribed && !u->finished && !u->pending && !ua_board.pending && u->task==RU_WAIT)
        ru_begin(u,RU_CLEAR,0,now);
	if (u->subscribed && !u->finished && !ua_board.pending && !u->pending && u->task==RU_WAIT && !ru.changed) {
		bool late=!ru.late && now/NSEC_PER_MSEC>=ru.subscribed_ms+90000;
		bool post=ua_board.post_at && ua_board.post_rounds<2 &&
		    now>=ua_board.post_at+(ua_board.post_rounds?8000ull*NSEC_PER_MSEC:0);
		bool file=mdm_rfs_board_completed()>ru.refreshed_files && !ru.refreshed_files;
		bool event=ru.card_ind+ru.slot_ind>ru.refreshed_ind && ru.refreshed_ind<2;
		bool renewed=pro_retry_ready(now/NSEC_PER_MSEC);
        bool evaluating=cold.stage>=COLD_FRESH && cold.stage<=COLD_MODE2;
        if (!evaluating && (renewed || late || file || event || post)) {
			if (post) {
				ua_board.post_rounds++;
				say("modem: uim-once verify ordinal=%u round=%u after_ms=%llu\n",ua_board.post_rounds,ru.round+1,
				    (unsigned long long)((now-ua_board.post_at)/NSEC_PER_MSEC));
			}
			ru.round++;u->round=ru.round;u->late|=late;ru.late|=late;
			ru.refreshed_ind=ru.card_ind+ru.slot_ind;
			ru.refreshed_files=mdm_rfs_board_completed();
			say("modem: rfs-state refresh round=%u after_ms=%llu late=%u event=%u file=%u completed=%u\n",
			    ru.round,(unsigned long long)(now/NSEC_PER_MSEC-ru.subscribed_ms),(unsigned)late,(unsigned)event,(unsigned)file,ru.refreshed_files);
			ru_begin(u,RU_PHYSICAL,0,now);
		}
	}
	for(unsigned i=0;i<RUC_COUNT;i++) {
		struct ruc *c=&ru.c[i];
		if (!c->open || !c->live || c->finished || ru.changed) continue;
		if (c->pending) {
			int decoded=0;int ready=hk_qmi_txn_poll(ru_txn(c),&decoded);
			if (!ready && now<c->deadline) continue;
			if (!ready) {qmi_txn_cancel(ru_txn(c));decoded=-ETIMEDOUT;}
			if (ready<0) decoded=ready;
			c->pending=false;ru_answer(c,decoded,now);
		} else if (!c->queried) {
			if(i==RUC_UIM) ru_begin(c,RU_SUBSCRIBE,0x15,now);else ru_begin(c,RU_RADIO,0,now);
		} else if (i!=RUC_UIM && c->round!=ru.round) {
			c->round=ru.round;ru_begin(c,RU_RADIO,0,now);
		}
	}
}
static void mdm_rfs_uim_stop(struct hk_abp_nas_worker *w)
{
	if (!ru.armed || !mdm_abp_is_owner(NULL)) return;
	if(ua_board.authorized)cold_end();
    say("modem: donor rf_total submissions_attempted=%u primary_attempt=%u primary_submitted=%u policy_on=%u\n",
        pro_issued,(unsigned)ua_board.once.attempted,(unsigned)ua_board.once.submitted,(unsigned)ua_board.authorized);
	obs_phase(HK_OBS_STOPPED);
	if (ua_board.pending) qmi_txn_cancel(&ua_board.once.txn);
	hk_uim_act_forget(&ua_board.once);
	say("modem: uim-once end attempted=%u submitted=%u ack_seen=%u already=%u post_rounds=%u pending_at_close=%u\n",
	    ua_board.once.attempted,ua_board.once.submitted,ua_board.once.ack_seen,(unsigned)ua_board.already,
	    ua_board.post_rounds,ua_board.pending);
	for(unsigned i=0;i<RUC_COUNT;i++){
		struct ruc *c=&ru.c[i];if(!c->open)continue;
		if(c->pending){qmi_txn_cancel(ru_txn(c));c->pending=false;}
		qmi_handle_release(&c->handle);c->open=false;
		bool gone=true;for(unsigned n=0;n<HK_QRTR_CORE_MAX_PORTS;n++)if(w->mhi->core.ports[n].number==c->local)gone=false;
		say("modem: rfs-state release service=%u gen=%u local=%u gone=%u\n",c->service,c->generation,c->local,(unsigned)gone);
	}
	say("modem: rfs-state closed card_ind=%u slot_ind=%u late=%u endpoint_change=%u extra_servers=%u completed=%u\n",
	    ru.card_ind,ru.slot_ind,(unsigned)ru.late,(unsigned)ru.changed,ru.extra_servers,mdm_rfs_board_completed());
}
