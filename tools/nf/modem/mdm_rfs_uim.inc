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
	if (c && s && c->live && c->node==s->node && c->port==s->port) {c->live=false;ru.changed=true;}
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
static void ru_begin(struct ruc *c,enum ruc_task task,uint32_t arg,uint64_t now)
{
	int rc;unsigned msg;
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
	c->task=RU_WAIT;
	if (!c->live || ru.changed) return;
	switch(task){
	case RU_SUBSCRIBE: if(c->subscribed)ru_begin(c,RU_PHYSICAL,0,now);else c->finished=true;break;
	case RU_PHYSICAL: ru_begin(c,RU_EXT,0,now);break;
	case RU_EXT: ru_begin(c,RU_CARD,0,now);break;
	case RU_CARD: ru_begin(c,RU_ATR,1,now);break;
	case RU_ATR: if(c->late)ru_begin(c,RU_CLEAR,0,now);break;
	case RU_CLEAR: c->finished=true;break;
	default:break;
	}
}
static void mdm_rfs_uim_poll(struct hk_abp_nas_worker *w)
{
	if (!mdm_abp_is_owner(NULL) || hk_qo_phase(&w->gate)!=HK_QO_READY) return;
	uint64_t now=op_nanoseconds(NULL);
	if (!ru.armed) {
		ru.armed=true;ru.began=now;
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
	if (u->subscribed && !u->finished && !u->pending && u->task==RU_WAIT && !ru.changed) {
		bool late=now/NSEC_PER_MSEC>=ru.subscribed_ms+90000;
		bool file=mdm_rfs_board_completed()>ru.refreshed_files && !ru.refreshed_files;
		bool event=ru.card_ind+ru.slot_ind>ru.refreshed_ind && ru.refreshed_ind<2;
		if (late || file || event) {
			ru.round++;u->round=ru.round;u->late=late;ru.late=late;
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
