/* Opt-in failure-path verification for an isolated device. The poller owns the
 * four actual QRTR ports and all QMI state; synchronous MHI waits enqueue IPCR
 * into the same generation's normal worker. Never send a user-plane packet. */
#include <net/kpi_interface.h>
#include "hk_cell_rawip.h"

enum family_key { FAM_WDA, FAM_V4, FAM_V6, FAM_NAS, FAM_COUNT };
enum family_step { FAM_DISCOVERY, FAM_INITIAL, FAM_STATUS, FAM_SETTINGS, FAM_DONE };
struct mdm_family_client {
	struct qmi_handle handle;
	struct hk_qmi_family_op op;
	struct hk_cell_wds_status source_status;
	struct hk_cell_bearer settings;
	unsigned service, requested_family, local, node, port, incarnation;
	enum family_step step;
	uint64_t deadline, cookie;
	bool open, live, pending, bound, validated_status, validated_settings;
};
static struct {
	struct mdm_family_client clients[FAM_COUNT];
	struct hk_cell_format format;
	uint64_t start, epoch;
	bool enabled, armed, hardware, done, invalidated, format_valid, nas_valid, released;
	unsigned nas_registration,nas_ps,ports;
} mdm_family;

static struct mdm_family_client *mdm_family_lookup(struct qmi_handle *handle)
{
	for (unsigned i=0;i<FAM_COUNT;i++)
		if (&mdm_family.clients[i].handle==handle) return &mdm_family.clients[i];
	return NULL;
}
static int mdm_family_arrived(struct qmi_handle *handle,struct qmi_service *s)
{
	struct mdm_family_client *c=mdm_family_lookup(handle);
	if (!mdm_abp_is_owner(NULL) || !c || !s || s->service!=c->service || s->version!=1 ||
	    s->instance || !s->node || !s->port || c->live || mdm_family.invalidated) return ENXIO;
	int rc=hk_qmi_connect(handle->sock,s->node,s->port);
	if (!rc) {
		c->live=true;c->node=s->node;c->port=s->port;c->incarnation++;
		say("modem: family endpoint epoch=%llu service=%u wanted=%u node=%u port=%u local=%u version=1 instance=0 incarnation=%u\n",
		    (unsigned long long)mdm_family.epoch,c->service,c->requested_family,c->node,c->port,c->local,c->incarnation);
	}
	return rc;
}
static void mdm_family_departed(struct qmi_handle *handle,struct qmi_service *s)
{
	struct mdm_family_client *c=mdm_family_lookup(handle);
	if (!mdm_abp_is_owner(NULL) || !s || !c || !c->live || c->node!=s->node || c->port!=s->port) return;
	c->live=false;mdm_family.invalidated=true;
}
static const struct qmi_ops mdm_family_ops={.new_server=mdm_family_arrived,.del_server=mdm_family_departed};
static void mdm_family_endpoint_close(void)
{
	for (unsigned i=0;i<FAM_COUNT;i++) {
		struct mdm_family_client *c=&mdm_family.clients[i];
		if (!c->open) continue;
		if (c->pending) {qmi_txn_cancel(&c->op.txn);c->pending=false;}
		qmi_handle_release(&c->handle);c->open=false;
		bool found=false;
		for(unsigned n=0;n<HK_QRTR_CORE_MAX_PORTS;n++)
			if (mdm_qrtr_mhi.core.ports[n].number==c->local) found=true;
		say("modem: family release epoch=%llu service=%u wanted=%u local=%u local_gone=%u\n",
		    (unsigned long long)mdm_family.epoch,c->service,c->requested_family,c->local,(unsigned)!found);
	}
}
static bool mdm_family_open_clients(void)
{
	static const unsigned service[FAM_COUNT]={HK_QMI_WDA_SERVICE,HK_QMI_WDS_SERVICE,HK_QMI_WDS_SERVICE,HK_QMI_NAS_SERVICE};
	static const unsigned family[FAM_COUNT]={0,4,6,0};
	for (unsigned i=0;i<FAM_COUNT;i++) {
		struct mdm_family_client *c=&mdm_family.clients[i];
		if(pro_active && i==FAM_NAS){c->step=FAM_DONE;continue;}
		c->service=service[i];c->requested_family=family[i];
		if(qmi_handle_init_transport(&c->handle,8192,&mdm_family_ops,NULL,hk_qrtr_mhi_ops()))return false;
		c->open=true;
		for(unsigned n=0;n<HK_QRTR_CORE_MAX_PORTS;n++) {
			struct hk_qrtr_core_port *p=&mdm_qrtr_mhi.core.ports[n];
			if (p->context==(void *)c->handle.sock && p->receive) {c->local=p->number;break;}
		}
		if (!c->local || qmi_add_lookup(&c->handle,c->service,1,0)<0)return false;
		mdm_family.ports++;
	}
	return true;
}
static int mdm_family_settings_reply(const void *packet,size_t length,void *output)
{
	struct mdm_family_client *c=output;
	/* qmi_receive already matched this handle, node, port, message and txn. */
	if (!mdm_abp_is_owner(NULL) || !c->live || mdm_family.invalidated || !c->bound ||
	    !c->validated_status || c->source_status.status.connection_status!=2) return -ENOTCONN;
	int rc=hk_cell_decode_wds_settings(&c->source_status,&c->source_status.source,c->op.txn.id,packet,length,&c->settings);
	return rc?-rc:(int)length;
}
static int mdm_family_settings_begin(struct mdm_family_client *c)
{
	static const struct qmi_elem_info mask_ei[]={
		{QMI_UNSIGNED_4_BYTE,1,4,NO_ARRAY,0x10,0,NULL},{QMI_EOTI,0,0,NO_ARRAY,0,0,NULL}};
	const uint32_t mask=0xa100;
	c->op.projection=(struct hk_qmi_projection){mdm_family_settings_reply,c};
	int rc=hk_qmi_txn_init_projected(&c->handle,&c->op.txn,&c->op.projection);
	if (rc<0) return rc;
	rc=qmi_send_request(&c->handle,NULL,&c->op.txn,0x2d,7,mask_ei,&mask);
	if (rc) qmi_txn_cancel(&c->op.txn);
	return rc;
}
static unsigned mdm_family_msg(struct mdm_family_client *c)
{
	if (c->service==HK_QMI_WDA_SERVICE) return HK_QMI_WDA_GET_DATA_FORMAT;
	if (c->service==HK_QMI_NAS_SERVICE) return HK_QMI_NAS_GET_SERVING_SYSTEM;
	return c->step==FAM_STATUS?0x22:c->step==FAM_SETTINGS?0x2d:0x4d;
}
static void mdm_family_start(struct mdm_family_client *c,uint64_t now)
{
	unsigned msg=mdm_family_msg(c);
	int rc;
	if (c->step==FAM_SETTINGS) rc=mdm_family_settings_begin(c);
	else {
		enum hk_qmi_family_query q=c->service==HK_QMI_WDA_SERVICE?HK_FQ_FORMAT:
		    c->service==HK_QMI_NAS_SERVICE?HK_FQ_SERVING:c->step==FAM_STATUS?HK_FQ_STATUS:HK_FQ_SET_FAMILY;
		rc=hk_qmi_family_begin(&c->handle,&c->op,q,q==HK_FQ_SET_FAMILY?c->requested_family:0);
	}
	c->pending=rc==0;
	c->deadline=now+5ull*NSEC_PER_SEC;
	if (rc) c->step=FAM_DONE;
	say("modem: family issue epoch=%llu wanted=%u service=%u node=%u port=%u local=%u msg=%u tid=%u rc=%d\n",
	    (unsigned long long)mdm_family.epoch,c->requested_family,c->service,c->node,c->port,c->local,msg,c->op.txn.id,rc);
}
static void mdm_family_reply(struct mdm_family_client *c,int rc,uint64_t now)
{
	unsigned msg=mdm_family_msg(c);
	struct qmi_response_type_v01 response={1,0};
	bool valid=false;
	unsigned value=0;
	enum family_step next=FAM_DONE;
	if (rc>0 && c->live && !mdm_family.invalidated) {
		switch(msg) {
		case HK_QMI_WDA_GET_DATA_FORMAT: {
			struct hk_qmi_wda_format *q=&c->op.value.format;response=q->response;
			valid=!response.result && q->present==15;mdm_family.format_valid=valid;
			mdm_family.format=(struct hk_cell_format){.epoch=mdm_family.epoch,.endpoint_type=3,.endpoint_id=4,.wda=*q};
			say("modem: family WDA epoch=%llu endpoint=3 interface=4 present=%u qos=%u link=%u ul=%u dl=%u\n",
			    (unsigned long long)mdm_family.epoch,q->present,q->value[0],q->value[1],q->value[2],q->value[3]);break;}
		case HK_QMI_NAS_GET_SERVING_SYSTEM: {
			struct hk_qmi_nas_serving_status *s=&c->op.value.serving;response=s->response;
			mdm_family.nas_valid=valid=!response.result && s->valid;
			if (valid) {mdm_family.nas_registration=s->registration;mdm_family.nas_ps=s->ps_attach;}
			say("modem: family NAS epoch=%llu valid=%u reg=%u cs=%u ps=%u\n",
			    (unsigned long long)mdm_family.epoch,(unsigned)valid,s->registration,s->cs_attach,s->ps_attach);break;}
		case 0x4d:
			response=c->op.value.family_ack;
			c->bound=valid=!response.result && !response.error;
			if (c->bound) next=FAM_STATUS;
			break;
		case 0x22:
			c->source_status.source=(struct hk_cell_wds_client){
			    .epoch=mdm_family.epoch,.handle=&c->handle,.service=c->service,.node=c->node,.port=c->port,.family=c->requested_family};
			c->source_status.status=c->op.value.status;response=c->op.value.status.response;
			c->validated_status=valid=c->bound && !response.result && c->op.value.status.connection_status_valid;
			if (valid) {value=c->op.value.status.connection_status;if(value==2)next=FAM_SETTINGS;}
			break;
		case 0x2d:
			c->validated_settings=valid=c->settings.local_present && c->settings.mtu_present;
			if(valid) response=(struct qmi_response_type_v01){0,0};
			break;
		}
	}
	say("modem: family reply epoch=%llu wanted=%u service=%u node=%u port=%u local=%u msg=%u tid=%u rc=%d result=%u error=%u valid=%u value=%u\n",
	    (unsigned long long)mdm_family.epoch,c->requested_family,c->service,c->node,c->port,c->local,msg,c->op.txn.id,rc,response.result,response.error,(unsigned)valid,value);
	c->step=next;
	if (next!=FAM_DONE) mdm_family_start(c,now);
}
static bool mdm_family_tick_client(struct mdm_family_client *c,uint64_t now)
{
	if (c->step==FAM_DONE) return true;
	if (mdm_family.invalidated || now-mdm_family.start>80ull*NSEC_PER_SEC) {
		if(c->pending){qmi_txn_cancel(&c->op.txn);c->pending=false;}
		c->step=FAM_DONE;return true;
	}
	if (!c->live || now-mdm_family.start<35ull*NSEC_PER_SEC) return false;
	if (!c->pending) {c->step=FAM_INITIAL;mdm_family_start(c,now);return c->step==FAM_DONE;}
	int rc=0;
	int collected=hk_qmi_txn_poll(&c->op.txn,&rc);
	if (!collected && now<c->deadline) return false;
	if (!collected) {qmi_txn_cancel(&c->op.txn);rc=-ETIMEDOUT;}
	if (collected<0) rc=collected;
	c->pending=false;
	mdm_family_reply(c,rc,now);return c->step==FAM_DONE;
}

static void mdm_family_tx_view(unsigned phase)
{
	struct mdm_mhi_tx_status s={0};
	int rc=mdm_mhi_tx_status(&mdm_mhi,mdm_family.epoch,&s);
	say("modem: family TX epoch=%llu phase=%u rc=%d capacity=%u buffers=%u pending=%u ready=%u bound=%u retired=%u quarantined=%u published=%llu completed=%llu cancelled=%llu doorbells=%llu\n",
	    (unsigned long long)mdm_family.epoch,phase,rc,s.capacity,s.buffers,s.pending,
	    (unsigned)s.ready,(unsigned)s.bound,(unsigned)s.retired,(unsigned)s.quarantined,(unsigned long long)s.published,
	    (unsigned long long)s.completed,(unsigned long long)s.cancelled,(unsigned long long)s.doorbells);
}
static int mdm_family_find(const char *name)
{
	ifnet_t interface=NULL;
	int rc=ifnet_find_by_name(name,&interface);
	if (!rc && interface) ifnet_release(interface);
	return rc;
}
static void mdm_family_admit(void)
{
	struct mdm_family_client *v4=&mdm_family.clients[FAM_V4],*v6=&mdm_family.clients[FAM_V6];
	bool disconnected=v4->bound && v6->bound && v4->validated_status && v6->validated_status &&
	    v4->source_status.status.connection_status==1 && v6->source_status.status.connection_status==1;
	const struct hk_qmi_wda_format *f=&mdm_family.format.wda;
	bool format=mdm_family.format_valid && f->value[HK_WDA_QOS]==0 && f->value[HK_WDA_LINK]==2 &&
	    f->value[HK_WDA_UPLINK]==0 && f->value[HK_WDA_DOWNLINK]==0;
	int control=mdm_family_find("lo0"),before=mdm_family_find("pdp_ip0");
	/* No positive attach is part of this experiment, including if the modem
	 * unexpectedly reports a connected bearer. A current-settings read alone
	 * cannot publish an interface, assign an address, install a route or send. */
	bool eligible_negative=mdm_family.hardware && !mdm_family.invalidated && disconnected && format &&
	    v4->node==v6->node && v4->port==v6->port && v4->local!=v6->local && !control && before!=0;
	say("modem: family admission epoch=%llu disconnected=%u raw_format=%u attempt=%u lo_lookup=%d pdp_before=%d\n",
	    (unsigned long long)mdm_family.epoch,(unsigned)disconnected,(unsigned)format,(unsigned)eligible_negative,control,before);
	if (eligible_negative) {
		for (unsigned i=FAM_V4;i<=FAM_V6;i++) {
			struct mdm_family_client *c=&mdm_family.clients[i];
			/* Pass both missing family contexts as NULL, independently of
			 * the checked disconnected status. There is no eligible address. */
			int rc=hk_cell_mhi_open(&mdm_mhi,mdm_family.epoch,&mdm_family.format,&c->source_status,NULL,NULL);
			say("modem: family open epoch=%llu family=%u local=%u rc=%d\n",
			    (unsigned long long)mdm_family.epoch,c->requested_family,c->local,rc);
		}
	}
	int after=mdm_family_find("pdp_ip0");
	say("modem: family ifnet epoch=%llu pdp_after=%d\n",(unsigned long long)mdm_family.epoch,after);
}
static bool pro_data_begin(void)
{
		int rc=mdm_mhi_rx_begin(&mdm_mhi,mdm_family.epoch);
		mdm_family.hardware=!rc;
		say("modem: family transport epoch=%llu begin=%d ul=100 dl=101 er_ul=7 er_dl=8 pool_used=%llu pool_limit=%u\n",
		    (unsigned long long)mdm_family.epoch,rc,(unsigned long long)mdm.pool_next,MDM_POOL_SIZE);
		if(rc)return false;
		mdm_family_tx_view(0);
    return true;
}
static void mdm_family_stop(void);
static void mdm_family_run(void)
{
	if (mdm_family.done) return;
	uint64_t now=op_nanoseconds(NULL);
	if (!mdm_family.armed) {
		mdm_family.armed=true;mdm_family.start=now;mdm_family.epoch=mdm_abp_binding.epoch;
if(pro_active)mdm_family.start=pro_owner_began;
        else if(!pro_data_begin()){mdm_family.done=true;return;}
		if (!mdm_family_open_clients()) {mdm_family_endpoint_close();mdm_family.done=true;}
		return;
	}
	bool all=true;
	for(unsigned i=0;i<FAM_COUNT;i++)
		if (!mdm_family_tick_client(&mdm_family.clients[i],now)) all=false;
	if (all) {
        if(pro_active){
            if(pro_transport_hold)return;
            if(mdm_family.invalidated || !mdm_family.format_valid){
                say("modem: donor transport skipped source_invalid=%u format_valid=%u\n",
                    (unsigned)mdm_family.invalidated,(unsigned)mdm_family.format_valid);
                mdm_family.done=true;mdm_family_stop();return;
            }
            if(!pro_data_begin()){mdm_family.done=true;mdm_family_stop();return;}
        }
        mdm_family_admit();
		mdm_family_tx_view(1);
		mdm_family.done=true;
        if(pro_active)mdm_family_stop();
	}
}
static void mdm_family_stop(void)
{
	if (!mdm_family.armed || mdm_family.released) return;
    mdm_family.released=true;
	/* Stop any interface owner before closing its WDS handles and retiring
	 * its DMA pool. This exercise never requests a positive network binding. */
	if (mdm_family.hardware) hk_cell_mhi_revoke(&mdm_mhi,mdm_family.epoch);
	mdm_family_endpoint_close();
	if (mdm_family.hardware) {
		int rc=mdm_mhi_rx_close(&mdm_mhi,mdm_family.epoch);
		mdm_family_tx_view(2);
		/* Controller configuration fixed by mdm_mhi_tx_config: channel
		 * configs 4/5 are exactly IP_HW0 UL100/DL101. No RX post occurs. */
		mdm_mhi.ops->lock(mdm_mhi.context);
		struct mhi_rx_credit *rx=&mdm_mhi.channels[5].rx;
		unsigned start=rx->start_acks,reset=rx->reset_acks,issued=rx->issued;
		bool drained=rx->drained;
		mdm_mhi.ops->unlock(mdm_mhi.context);
		say("modem: family retire epoch=%llu rc=%d start_ack=%u reset_ack=%u rx_credits=%u er_drained=%u clients=%u\n",
		    (unsigned long long)mdm_family.epoch,rc,start,reset,issued,(unsigned)drained,mdm_family.ports);
	}
}
