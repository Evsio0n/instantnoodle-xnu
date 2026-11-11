/* Read-only RFS on the IN2010 SDX55's IPCR transport. Every operation,
 * including FAT access and port close, runs on the single ABP poll owner. */
#include "mdm_rfs.inc"

#define MDM_RFS_SERVICE_ID 4096u
#define MDM_RFS_INSTANCE_RAW 3u

static bool mdm_rfs_requested;
static struct mdm_rfs_endpoint {
	struct hk_qrtr_core_port *port;
} mdm_rfs_endpoints[HK_QRTR_CORE_MAX_PORTS];

struct mdm_rfs_peer {
	uint32_t node;
	bool discovered, published, announce, end_lookup;
	uint64_t retry_ms;
};

static struct {
	struct mdm_rfs server;
	uint64_t epoch;
	uint32_t listener;
	uint32_t hellos, routes, lookups;
	uint64_t listener_ms;
	struct mdm_rfs_peer peers[2];
} mdm_rfs_board;

static uint64_t
mdm_rfs_now(void)
{
	return op_nanoseconds(NULL) / NSEC_PER_MSEC;
}

static struct mdm_rfs_peer *
mdm_rfs_peer(uint32_t node)
{
	/* Node 2 is the SDX's router/AP, node 3 is MPSS. Neither destination is
	 * contacted until it is seen on this physical link in the current epoch. */
	for (unsigned i = 0; i < 2; i++)
		if (node && mdm_rfs_board.peers[i].node == node)
			return &mdm_rfs_board.peers[i];
	return NULL;
}

static bool
mdm_rfs_live(void)
{
	return mdm_rfs_board.epoch && mdm_rfs_board.epoch == mdm_abp_binding.epoch &&
	    hk_qo_phase(&mdm_abp_worker.gate) == HK_QO_READY && mdm_abp_is_owner(NULL);
}

static void
mdm_rfs_port_input(void *context, uint32_t node, uint32_t port, const void *data, size_t length)
{
	struct mdm_rfs_endpoint *endpoint = context;
	struct mdm_rfs_peer *peer = mdm_rfs_peer(node);
	if (!mdm_rfs_live() || !endpoint->port || !peer || !peer->published ||
	    !port || port == MDM_QRTR_CONTROL)
		return;
	uint32_t requests = mdm_rfs_board.server.stats.rrq;
	uint32_t writes=mdm_rfs_board.server.stats.wrq;
	mdm_rfs_receive(&mdm_rfs_board.server, endpoint->port->number, node, port,
	    data, length, mdm_rfs_now());
	if(mdm_rfs_board.server.stats.wrq>writes && writes<32) {
        const struct mdm_rfs_wrq_stats *w=&mdm_rfs_board.server.writes;
        say("modem: rfs wrq event=%u epoch=%llu node=%u clock_ms=%llu listener_age_ms=%llu delta_ms=%llu root=%u options=%u valid=%u same_peer=%u same_shape=%u public_check=%u reject_submitted=%u\n",
            writes+1,(unsigned long long)mdm_rfs_board.epoch,node,(unsigned long long)w->last_ms,
            (unsigned long long)(w->last_ms-mdm_rfs_board.listener_ms),(unsigned long long)w->delta_ms,
            w->last_root,w->last_options,(unsigned)w->last_valid,(unsigned)w->last_same_peer,
            (unsigned)w->last_same_shape,(unsigned)w->last_public_server_check,(unsigned)w->last_submitted);
    }
    if (!requests && mdm_rfs_board.server.stats.rrq)
		say("modem: rfs first RRQ epoch=%llu peer=%u listener=%u\n",
		    (unsigned long long)mdm_rfs_board.epoch, node, mdm_rfs_board.listener);
}

static uint32_t
mdm_rfs_port_open(__unused void *context)
{
	if (!mdm_rfs_live())
		return 0;
	for (unsigned i = 0; i < HK_QRTR_CORE_MAX_PORTS; i++) {
		struct mdm_rfs_endpoint *ep = &mdm_rfs_endpoints[i];
		if (!ep->port) {
			ep->port = hk_qrtr_core_open(&mdm_qrtr_mhi.core, mdm_rfs_port_input, NULL, ep);
			return ep->port ? ep->port->number : 0;
		}
	}
	return 0;
}

static struct mdm_rfs_endpoint *
mdm_rfs_endpoint(uint32_t local)
{
	for (unsigned i = 0; i < HK_QRTR_CORE_MAX_PORTS; i++) {
		struct mdm_rfs_endpoint *ep = &mdm_rfs_endpoints[i];
		if (ep->port && ep->port->number == local)
			return ep;
	}
	return NULL;
}

static void
mdm_rfs_port_close(__unused void *context, uint32_t local)
{
	struct mdm_rfs_endpoint *ep = mdm_rfs_endpoint(local);
	if (ep && mdm_abp_is_owner(NULL)) {
		hk_qrtr_core_close(&mdm_qrtr_mhi.core, ep->port);
		ep->port = NULL;
	}
}

static int
mdm_rfs_data_send(__unused void *context, uint32_t local, uint32_t node, uint32_t port,
    const void *data, size_t length)
{
	struct mdm_rfs_endpoint *ep = mdm_rfs_endpoint(local);
	struct mdm_rfs_peer *peer = mdm_rfs_peer(node);
	if (!mdm_rfs_live() || !mdm_qrtr_mhi.up || !ep || !peer || !peer->published ||
	    !port || port == MDM_QRTR_CONTROL)
		return ENOTCONN;
	return hk_qrtr_core_send(&mdm_qrtr_mhi.core, ep->port, node, port, data, length);
}

static const struct mdm_rfs_ops mdm_rfs_board_ops = {
	.open_port = mdm_rfs_port_open,
	.close_port = mdm_rfs_port_close,
	.send = mdm_rfs_data_send,
};

static uint32_t
mdm_rfs_board_completed(void)
{
	return mdm_rfs_board.server.stats.completed;
}

/* See AP's native tqftpserv: the instance is the raw wire field 3. A port
 * in an announcement is the allocated listener, never the nameserver port. */
static int
mdm_rfs_announce(uint32_t command, uint32_t node, bool end)
{
	uint32_t words[5] = { command, 0, 0, 0, 0 };
	if (!end) {
		words[1] = MDM_RFS_SERVICE_ID;
		words[2] = MDM_RFS_INSTANCE_RAW;
		words[3] = MDM_QRTR_US;
		words[4] = mdm_rfs_board.listener;
	}
	return mdm_qrtr_send(command, MDM_QRTR_CONTROL, node, MDM_QRTR_CONTROL, words, sizeof(words));
}

static void
mdm_rfs_board_observe(const struct hk_qrtr_wire_datagram *d)
{
	struct hk_qrtr_wire_control ctl;
	struct mdm_rfs_peer *from = mdm_rfs_peer(d->from_node);
	if (!mdm_rfs_live() || !mdm_rfs_board.listener || !from ||
	    (d->to_node != MDM_QRTR_US && d->to_node != UINT32_MAX &&
	     !(d->version == HK_QRTR_WIRE_VERSION_2 && d->to_node == UINT16_MAX)) ||
	    hk_qrtr_wire_control_decode(d, &ctl) ||
	    (d->type != ctl.command && d->type != MDM_QRTR_DATA))
		return;
	if (ctl.command != MDM_QRTR_NEW_LOOKUP && d->from_port != MDM_QRTR_CONTROL)
		return;
	switch (ctl.command) {
	case MDM_QRTR_HELLO:
		mdm_rfs_board.hellos++;
		from->discovered = true;
		from->announce = true;
		break;
	case MDM_QRTR_NEW_SERVER: {
		/* The router advertises RMTFS from its own node 2 and may advertise
		 * DMS, NAS or UIM from MPSS node 3 (seen in IN2010 discovery). */
		struct mdm_rfs_peer *owner = mdm_rfs_peer(ctl.words[2]);
		if (d->payload_length < 20 || !owner || !ctl.words[3] ||
		    (ctl.words[0] != 2 && ctl.words[0] != 3 && ctl.words[0] != 11 &&
		     !(owner->node == 2 && ctl.words[0] == 14)))
			break;
		mdm_rfs_board.routes++;
		from->discovered = owner->discovered = true;
		if (!from->published) from->announce = true;
		if (!owner->published) owner->announce = true;
		break;
	}
	case MDM_QRTR_NEW_LOOKUP:
		if (d->payload_length >= 12 &&
		    (ctl.words[0] == 0 || ctl.words[0] == MDM_RFS_SERVICE_ID) &&
		    (ctl.words[1] == 0 || ctl.words[1] == MDM_RFS_INSTANCE_RAW)) {
			mdm_rfs_board.lookups++;
			from->discovered = true;
			from->announce = true;
			from->end_lookup = true;
		}
		break;
	case MDM_QRTR_DEL_CLIENT:
		if (d->payload_length >= 12 && ctl.words[2] == 0 && ctl.words[3] == 0 &&
		    ctl.words[0] == from->node && ctl.words[1])
			mdm_rfs_drop_peer(&mdm_rfs_board.server, from->node, ctl.words[1]);
		break;
	case MDM_QRTR_BYE:
		mdm_rfs_drop_peer(&mdm_rfs_board.server, from->node, 0);
		*from = (struct mdm_rfs_peer){ .node = d->from_node };
		break;
	}
}

static bool
mdm_rfs_board_start(void)
{
	mdm_rfs_board.epoch = mdm_abp_binding.epoch;
	mdm_rfs_board.peers[0].node = 2;
	mdm_rfs_board.peers[1].node = 3;
	mdm_rfs_board.listener = mdm_rfs_port_open(NULL);
	if (!mdm_rfs_board.listener) {
		say("modem: rfs listener unavailable\n");
		return false;
	}
	mdm_rfs_board.listener_ms=mdm_rfs_now();
	mdm_rfs_start(&mdm_rfs_board.server, &mdm_fat, &mdm_rfs_board_ops, NULL, mdm_rfs_board.listener);
    say("modem: rfs timebase clock_ms=%llu epoch=%llu\n",(unsigned long long)mdm_rfs_board.listener_ms,(unsigned long long)mdm_rfs_board.epoch);
    say("modem: rfs listener epoch=%llu port=%u service=%u raw_instance=%u\n",
	    (unsigned long long)mdm_rfs_board.epoch, mdm_rfs_board.listener, MDM_RFS_SERVICE_ID, MDM_RFS_INSTANCE_RAW);
	return true;
}

static void
mdm_rfs_board_tick(void)
{
	if (!mdm_rfs_live() || !mdm_rfs_board.listener)
		return;
	uint64_t now = mdm_rfs_now();
	for (unsigned i = 0; i < 2; i++) {
		struct mdm_rfs_peer *p = &mdm_rfs_board.peers[i];
		if (!p->discovered || now < p->retry_ms)
			continue;
		if (p->announce) {
			int rc = mdm_rfs_announce(MDM_QRTR_NEW_SERVER, p->node, false);
			if (!p->published || rc)
				say("modem: rfs publish node=%u port=%u rc=%d\n", p->node, mdm_rfs_board.listener, rc);
			if (rc) { p->retry_ms = now + 1000; continue; }
			p->announce = false;
			p->published = true;
		}
		if (p->end_lookup) {
			if (mdm_rfs_announce(MDM_QRTR_NEW_SERVER, p->node, true))
				p->retry_ms = now + 1000;
			else
				p->end_lookup = false;
		}
	}
	mdm_rfs_tick(&mdm_rfs_board.server, now);
}

static void
mdm_rfs_board_stop(void)
{
	if (!mdm_rfs_board.listener)
		return;
	say("modem: rfs namespace hellos=%u routes=%u lookups=%u\n",
	    mdm_rfs_board.hellos, mdm_rfs_board.routes, mdm_rfs_board.lookups);
	for (unsigned i = 0; i < 2; i++) {
		struct mdm_rfs_peer *p = &mdm_rfs_board.peers[i];
		if (p->published) {
			int rc = mdm_rfs_announce(MDM_QRTR_DEL_SERVER, p->node, false);
			say("modem: rfs withdraw node=%u port=%u rc=%d\n", p->node, mdm_rfs_board.listener, rc);
		}
	}
	const struct mdm_rfs_wrq_stats *w=&mdm_rfs_board.server.writes;
    for(unsigned i=0;i<MDM_RFS_ROOTS;i++)
        say("modem: rfs wrq_root root=%u count=%u\n",i,w->roots[i]);
    say("modem: rfs wrq_summary submitted=%u failed=%u same_peer=%u same_shape=%u public_check=%u opt_count=%u first_ms=%llu last_ms=%llu\n",
        w->submitted,w->submit_failed,w->same_peer_adjacent,w->same_shape_adjacent,w->public_server_check,w->options_seen,
        (unsigned long long)w->first_ms,(unsigned long long)w->last_ms);
    mdm_rfs_stop(&mdm_rfs_board.server);
	const struct mdm_rfs_stats *s = &mdm_rfs_board.server.stats;
	say("modem: rfs requests rrq=%u wrq=%u denied=%u missing=%u malformed=%u opened=%u\n",
	    s->rrq, s->wrq, s->denied, s->missing, s->malformed, s->opened);
	say("modem: rfs transfer oack=%u tsize=%u blocks=%u sent_bytes=%llu acked_bytes=%llu completed=%u eot=%u expired=%u errors=%u retransmits=%u\n",
	    s->oack, s->tsize_oack, s->data_blocks, (unsigned long long)s->data_bytes,
	    (unsigned long long)s->acked_bytes, s->completed, s->eot, s->expired, s->errors, s->retransmits);
	say("modem: rfs close acked_blocks=%u metadata_eot=%u namespace_denied=%u revoked=%u\n",
	    s->acked_blocks, s->metadata_eot, s->namespace_denied, s->revoked);
	mdm_rfs_port_close(NULL, mdm_rfs_board.listener);
	bzero(&mdm_rfs_board, sizeof(mdm_rfs_board));
}
