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
