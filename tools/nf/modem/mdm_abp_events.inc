/* Opt-in, one published real NAS client, one baseline GET, one enable and
 * a bounded observation window before one explicit clear. All operations
 * are typed kernel frontend calls on the board's existing poll thread. */
enum mdm_event_step { MDM_NAS_EV_DISCOVER, MDM_NAS_EV_GET, MDM_NAS_EV_ENABLE, MDM_NAS_EV_OBSERVE, MDM_NAS_EV_CLEAR, MDM_NAS_EV_DONE };
static struct {
    bool enabled;
    enum mdm_event_step step;
    uint64_t started, deadline, get_token, token, seq, first_seq;
    struct hk_qmux_channel_info channel;
    unsigned samples;
    bool del_submitted;
} mdm_events;

/* Called after the board's real IPCR_OUT MHI sender returns. DEL_CLIENT
 * submission and removal of a local port are independent of any firmware ACK. */
static void mdm_events_tx(const void *packet, size_t n, int rc)
{
    const struct hk_qmux_channel_info *c=&mdm_events.channel;
    if (!mdm_events.enabled || !c->generation || mdm_events.step==MDM_NAS_EV_DONE) return;
    struct hk_qrtr_wire_datagram d;
    if (hk_qrtr_wire_decode(packet,n,HK_QRTR_CORE_MAX_PAYLOAD,&d) || d.from_port!=c->local_port) return;
    if (d.type==HK_QRTR_WIRE_DEL_CLIENT && d.to_port==HK_QRTR_WIRE_PORT_CONTROL) {
        struct hk_qrtr_wire_control ctl;
        if (hk_qrtr_wire_control_decode(&d,&ctl) || ctl.command!=HK_QRTR_WIRE_DEL_CLIENT ||
            ctl.words[0]!=d.from_node || ctl.words[1]!=c->local_port) return;
        mdm_events.del_submitted = !rc;
        say("modem: nas-events: del_client generation=%llu node=%u local=%u rc=%d\n",
            (unsigned long long)c->generation,d.from_node,d.from_port,rc);
    } else if (d.type==HK_QRTR_WIRE_DATA && d.to_node==c->node && d.to_port==c->remote_port &&
               d.payload_length>=7 && !d.payload[0]) {
        const uint8_t *q=d.payload;
        unsigned msg=hk_qrtr_wire_le16(q+3), value=0;
        if (msg==3 && d.payload_length==11 && q[7]==0x13 && hk_qrtr_wire_le16(q+8)==1 && q[10]<=1) value=q[10];
        else if (msg!=0x24 || d.payload_length!=7) return;
        say("modem: nas-events: wire generation=%llu node=%u peer=%u local=%u cid=%u msg=%u tid=%u value=%u rc=%d\n",
            (unsigned long long)c->generation,d.to_node,d.to_port,d.from_port,c->cid,msg,hk_qrtr_wire_le16(q+1),value,rc);
    }
}

static void mdm_events_tick(void)
{
    if (!mdm_events.enabled || mdm_events.step == MDM_NAS_EV_DONE) return;
    uint64_t now = op_nanoseconds(NULL);
    struct hk_qmux_channel_info *c = &mdm_events.channel;
    struct hk_qmux_nas_subscription sub = {0};
    if (!mdm_events.started) mdm_events.started = now;
    int rc;
    switch (mdm_events.step) {
    case MDM_NAS_EV_DISCOVER:
        if (now - mdm_events.started < 35ull*NSEC_PER_SEC) return;
        if (now - mdm_events.started >= 55ull*NSEC_PER_SEC) {
            say("modem: nas-events: discovery timeout\n");
            mdm_events.step = MDM_NAS_EV_DONE;
            return;
        }
        if (hk_qmux_frontend_info(1,c)) return;
        rc = hk_qmux_frontend_request(1,c->generation,&mdm_events.get_token);
        say("modem: nas-events: baseline request generation=%llu index=%u node=%u peer=%u local=%u cid=%u token=%llu rc=%d\n",
            (unsigned long long)c->generation,c->index,c->node,c->remote_port,c->local_port,c->cid,
            (unsigned long long)mdm_events.get_token,rc);
        mdm_events.step = rc ? MDM_NAS_EV_DONE : MDM_NAS_EV_GET;
        return;
    case MDM_NAS_EV_GET: {
        struct hk_qmux_nas_answer a = {0};
        rc = hk_qmux_frontend_take(1,c->generation,mdm_events.get_token,&a);
        if (rc == EAGAIN) return;
        say("modem: nas-events: baseline answer generation=%llu rc=%d tid=%u bytes=%u result=%u error=%u valid=%u reg=%u cs=%u ps=%u\n",
            (unsigned long long)c->generation,rc,a.transaction,a.response_bytes,a.firmware_result,a.firmware_error,
            a.serving_valid,a.registration,a.cs_attach,a.ps_attach);
        if (rc) { mdm_events.step=MDM_NAS_EV_DONE; return; }
        rc = hk_qmux_frontend_subscribe(1,c->generation,&mdm_events.token);
        say("modem: nas-events: enable request generation=%llu token=%llu rc=%d\n",
            (unsigned long long)c->generation,(unsigned long long)mdm_events.token,rc);
        mdm_events.step = rc ? MDM_NAS_EV_DONE : MDM_NAS_EV_ENABLE;
        return;
    }
    case MDM_NAS_EV_ENABLE:
        rc = hk_qmux_frontend_subscription(1,c->generation,mdm_events.token,&sub);
        if (!rc && sub.phase == HK_QMUX_SUB_STARTING) return;
        say("modem: nas-events: enable outcome generation=%llu rc=%d phase=%u tid=%u bytes=%u result=%u error=%u terminal=%d cursor=%llu\n",
            (unsigned long long)c->generation,rc,(unsigned)sub.phase,sub.enable.transaction,sub.enable.response_bytes,
            sub.enable.firmware_result,sub.enable.firmware_error,sub.terminal_error,(unsigned long long)sub.sequence_at_ack);
        if (rc || sub.phase != HK_QMUX_SUB_ACTIVE) {
            if (!rc && sub.phase==HK_QMUX_SUB_REFUSED)
                (void)hk_qmux_frontend_unsubscribe(1,c->generation,mdm_events.token);
            mdm_events.step=MDM_NAS_EV_DONE; return;
        }
        mdm_events.first_seq = mdm_events.seq = sub.sequence_at_ack;
        mdm_events.deadline = now + 12ull*NSEC_PER_SEC;
        mdm_events.step = MDM_NAS_EV_OBSERVE;
        return;
    case MDM_NAS_EV_OBSERVE: {
        struct hk_qmux_nas_event e = {0};
        rc = hk_qmux_frontend_read_subscription_event(1,c->generation,mdm_events.token,mdm_events.seq,&e);
        if (!rc) {
            mdm_events.seq=e.sequence;
            if (mdm_events.samples++ < 3)
                say("modem: nas-events: indication generation=%llu index=%u node=%u peer=%u local=%u cid=%u seq=%llu stamp=%llu reg=%u cs=%u ps=%u\n",
                    (unsigned long long)e.channel.generation,e.channel.index,e.channel.node,e.channel.remote_port,e.channel.local_port,
                    e.channel.cid,(unsigned long long)e.sequence,(unsigned long long)e.received_ns,e.registration,e.cs_attach,e.ps_attach);
        }
        if ((!rc || rc == EAGAIN) && now < mdm_events.deadline) return;
        say("modem: nas-events: window generation=%llu elapsed=%u reports=%llu reads=%u rc=%d\n",
            (unsigned long long)c->generation,now>=mdm_events.deadline,
            (unsigned long long)(mdm_events.seq-mdm_events.first_seq),mdm_events.samples,rc);
        rc=hk_qmux_frontend_unsubscribe(1,c->generation,mdm_events.token);
        say("modem: nas-events: disable request generation=%llu rc=%d\n",(unsigned long long)c->generation,rc);
        mdm_events.step = rc ? MDM_NAS_EV_DONE : MDM_NAS_EV_CLEAR;
        return;
    }
    case MDM_NAS_EV_CLEAR: {
        rc=hk_qmux_frontend_subscription(1,c->generation,mdm_events.token,&sub);
        if (!rc && sub.phase==HK_QMUX_SUB_STOPPING) return;
        struct hk_qmux_channel_info live={0};
        int info=hk_qmux_frontend_info(1,&live);
        bool retired=info==ENOTCONN || (!info && c->generation!=live.generation);
        bool released=true;
        for (unsigned i=0;i<HK_QRTR_CORE_MAX_PORTS;i++)
            if (mdm_abp_worker.mhi->core.ports[i].number==c->local_port) released=false;
        say("modem: nas-events: disable outcome generation=%llu rc=%d phase=%u tid=%u bytes=%u result=%u error=%u terminal=%d withdrawn=%u port_released=%u del_submitted=%u\n",
            (unsigned long long)c->generation,rc,(unsigned)sub.phase,sub.disable.transaction,sub.disable.response_bytes,
            sub.disable.firmware_result,sub.disable.firmware_error,sub.terminal_error,retired,released,mdm_events.del_submitted);
        mdm_events.step=MDM_NAS_EV_DONE;
        return;
    }
    case MDM_NAS_EV_DONE: return;
    }
}
