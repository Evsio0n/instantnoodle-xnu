/* SDX55 raw-IP ifnet. Built into the NF board translation unit. */
#ifndef HK_CELL_HOST_TEST
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <sys/errno.h>
#include <sys/kern_event.h>
#include <sys/hk_qrtr_wire.h>
#include <sys/sockio.h>
#include <sys/systm.h>
#endif
#include "hk_cell_rawip.h"
#include "mdm_wds_settings.h"

static LCK_GRP_DECLARE(cell_group, "hk-cell");
static LCK_MTX_DECLARE(cell_control, &cell_group);
static LCK_MTX_DECLARE(cell_lock, &cell_group);
static LCK_MTX_DECLARE(cell_rx_lock, &cell_group);

static struct {
	ifnet_t ifp;
	uint64_t epoch, last_epoch;
	uint32_t families, mtu, inflight;
	uint8_t ipv4[4], ipv6[16], ipv6_prefix_length;
	uint32_t ipv4_mtu, ipv6_mtu;
	struct hk_cell_wds_client wds4, wds6;
	uint32_t next_rx_slot;
	bool discard_to_eot;
	bool active;
	struct hk_cell_transport transport;
} binding;

struct cell_lease {
	ifnet_t ifp;
	uint64_t epoch;
	uint32_t families, mtu;
	struct hk_cell_transport transport;
};

static unsigned
family_bit(protocol_family_t p)
{
	return p == PF_INET ? HK_CELL_IPV4 : p == PF_INET6 ? HK_CELL_IPV6 : 0;
}

static bool
wds_connected(const struct hk_qmi_wds_packet_service_status_resp *status)
{
	return !status->response.result && !status->response.error && status->connection_status_valid &&
	       status->connection_status == 2;
}

static bool
same_wds_client(const struct hk_cell_wds_client *a, const struct hk_cell_wds_client *b)
{
	return a->epoch == b->epoch && a->handle == b->handle && a->node == b->node &&
	    a->port == b->port && a->service == b->service && a->family == b->family;
}

static bool
family_client(const struct hk_cell_bearer *bearer, unsigned family, uint64_t epoch)
{
	const struct hk_cell_wds_client *s = &bearer->source;
	return s->epoch == epoch && s->handle != NULL && s->service == HK_QMI_WDS_SERVICE && s->family == family;
}

static int
bearer_error(const struct hk_cell_bearer *b, unsigned family, uint64_t epoch)
{
	if (!b || b->epoch != epoch || b->family != family || !b->local_present || !b->mtu_present ||
	    !wds_connected(&b->status))
		return ENETDOWN;
	return mdm_wds_validate_ip(b->local, family, b->ipv6_prefix_length, b->mtu);
}

/* WDS Get Current Settings (libqmi data/qmi-service-wds.json). A QMI
 * response carries neither its QRTR port nor the IP family of the client
 * that requested it. Verify the source before looking at optional fields. */
int
hk_cell_decode_wds_settings(const struct hk_cell_wds_status *status,
                            const struct hk_cell_wds_client *source, uint16_t transaction,
                            const void *reply, size_t length, struct hk_cell_bearer *out)
{
	struct mdm_wds_settings settings;
	if (!out)
		return EINVAL;
	memset(out, 0, sizeof(*out));
	if (!status || !source || !source->epoch || !source->handle ||
	    source->service != HK_QMI_WDS_SERVICE || (source->family != 4 && source->family != 6))
		return EINVAL;
	const struct hk_cell_wds_client *prior = &status->source;
	if (!same_wds_client(prior, source))
		return ESTALE;
	if (!wds_connected(&status->status))
		return ENETDOWN;
	int error = mdm_wds_decode_current(reply, length, transaction, source->family, &settings);
	if (error)
		return error;
	out->epoch = source->epoch;
	out->family = settings.family;
	out->ipv6_prefix_length = settings.prefix;
	memcpy(out->local, settings.address, sizeof(out->local));
	out->mtu = settings.mtu;
	out->status = status->status;
	out->source = *source;
	out->mtu_present = out->local_present = true;
	return 0;
}

static int
qualify(uint64_t epoch, const struct hk_cell_format *f, const struct hk_cell_bearer *v4,
        const struct hk_cell_bearer *v6, const struct hk_cell_transport *t, unsigned *families,
        unsigned *mtu)
{
	if (!epoch || !f || !t || (!t->rx_host && !t->copy_tx) || (!v4 && !v6))
		return EINVAL;
	if (f->epoch != epoch || t->epoch != epoch)
		return ESTALE;
	if (f->endpoint_type != 3 || f->endpoint_id != 4 || f->wda.response.result ||
	    f->wda.response.error || f->wda.present != ((1u << HK_WDA_FIELD_COUNT) - 1) ||
	    f->wda.value[HK_WDA_LINK] != 2 || f->wda.value[HK_WDA_QOS] || f->wda.value[HK_WDA_UPLINK] ||
	    f->wda.value[HK_WDA_DOWNLINK])
		return EPROTONOSUPPORT;
	if (!t->ul_ready || !t->dl_ready || t->ul_channel != 100 || t->ul_er != 7 || t->dl_channel != 101 ||
	    t->dl_er != 8)
		return ENETDOWN;
	if ((v4 && bearer_error(v4, 4, epoch)) || (v6 && bearer_error(v6, 6, epoch)))
		return ENETDOWN;
	*families = (v4 ? HK_CELL_IPV4 : 0) | (v6 ? HK_CELL_IPV6 : 0);
	*mtu = v4 && v6 ? (v4->mtu < v6->mtu ? v4->mtu : v6->mtu) : v4 ? v4->mtu : v6->mtu;
	if (*mtu > t->max_datagram || (v6 && *mtu < 1280))
		return EMSGSIZE;
	if (t->rx_host) {
		if ((v4 && !family_client(v4, 4, epoch)) || (v6 && !family_client(v6, 6, epoch)) ||
		    (v4 && v6 && v4->source.handle == v6->source.handle))
			return ESTALE;
		struct mdm_mhi_tx_status status;
		int rc = mdm_mhi_tx_status(t->rx_host, epoch, &status);
		if (rc || !status.ready)
			return rc ? rc : ENETDOWN;
		struct mhi_host_channel *dl;
		rc = mdm_rx_acquire(t->rx_host, epoch, &dl);
		if (rc)
			return rc;
		if (dl->config->event_ring != t->dl_er)
			rc = ENETDOWN;
		else if (t->max_datagram > dl->rx.capacity || t->max_datagram > status.capacity)
			rc = EMSGSIZE;
		t->rx_host->ops->unlock(t->rx_host->context);
		if (rc)
			return rc;
	}
	return 0;
}

static bool
enter(ifnet_t ifp, uint64_t epoch, struct cell_lease *l)
{
	lck_mtx_lock(&cell_lock);
	bool ok = binding.active && (ifp ? ifp == binding.ifp : epoch == binding.epoch) &&
	          (ifnet_flags(binding.ifp) & IFF_UP) != 0;
	if (ok) {
		binding.inflight++;
		*l = (struct cell_lease){binding.ifp, binding.epoch, binding.families, binding.mtu,
		                         binding.transport};
	}
	lck_mtx_unlock(&cell_lock);
	return ok;
}

static void
leave(void)
{
	lck_mtx_lock(&cell_lock);
	if (!--binding.inflight)
		wakeup(&binding.inflight);
	lck_mtx_unlock(&cell_lock);
}

static void
drain_locked(void)
{
	binding.active = false;
	if (binding.transport.rx_host) {
		struct mhi_host *host = binding.transport.rx_host;
		ifnet_t producer = binding.ifp;
		uint64_t epoch = binding.epoch;
		/* cell_control holds the binding stable. Revoke the DMA producer
		 * before waiting on output callbacks that already entered. */
		lck_mtx_unlock(&cell_lock);
		mdm_mhi_tx_unbind(host, epoch, producer);
		lck_mtx_lock(&cell_lock);
	}
	while (binding.inflight)
		msleep(&binding.inflight, &cell_lock, PZERO, "hk-cell-drain", NULL);
}

static int
raw_family(mbuf_t packet, size_t mtu, protocol_family_t *family)
{
	uint8_t h[6];
	if (!packet || !(mbuf_flags(packet) & MBUF_PKTHDR) || mbuf_nextpkt(packet))
		return EINVAL;
	size_t n = mbuf_pkthdr_len(packet), sum = 0;
	if (n < sizeof(h) || n > mtu)
		return EMSGSIZE;
	for (mbuf_t m = packet; m; m = mbuf_next(m)) {
		size_t len = mbuf_len(m);
		if (len > n - sum)
			return EMSGSIZE;
		sum += len;
	}
	if (sum != n || mbuf_copydata(packet, 0, sizeof(h), h))
		return EINVAL;
	switch (h[0] >> 4) {
	case 4: {
		size_t hdr = (h[0] & 15u) * 4u, declared = ((size_t)h[2] << 8) | h[3];
		if (hdr < 20 || hdr > n || declared != n)
			return EINVAL;
		*family = PF_INET;
		return 0;
	}
	case 6:
		if (n < 40 || (((size_t)h[4] << 8) | h[5]) != n - 40)
			return EINVAL;
		*family = PF_INET6;
		return 0;
	default:
		return EAFNOSUPPORT;
	}
}

static void
free_input(mbuf_t m)
{
	if (m)
		mbuf_freem_list(m);
}

static int
output_family(const struct cell_lease *l, mbuf_t packet, protocol_family_t *family)
{
	int rc = raw_family(packet, l->mtu, family);
	if (rc)
		return rc;
	if (!(l->families & family_bit(*family)))
		return EAFNOSUPPORT;
	/* XNU owns source selection, including scoped and temporary IPv6
	 * addresses and the unspecified DAD source. The WDS client, family,
	 * ifnet identity and MHI epoch bind the transfer to its bearer. */
	return 0;
}

static int
copy_tx_mbuf(void *source, void *destination, size_t length)
{
	return mbuf_copydata((mbuf_t)source, 0, length, destination);
}

static errno_t
cell_output(ifnet_t ifp, mbuf_t packet)
{
	struct cell_lease l;
	protocol_family_t family = 0;
	int rc = ENETDOWN;
	if (enter(ifp, 0, &l)) {
		rc = output_family(&l, packet, &family);
		if (!rc) {
			size_t n = mbuf_pkthdr_len(packet);
			if (l.transport.rx_host) {
				rc = mdm_mhi_tx_submit(l.transport.rx_host, l.epoch, ifp, n, copy_tx_mbuf, packet);
				/* No packet was enqueued. Legacy DLIL consumes the mbuf;
				 * a later stack retry supplies a new one. */
				if (rc == EWOULDBLOCK)
					rc = ENOBUFS;
			} else
				rc = l.transport.copy_tx(l.transport.ctx, l.epoch, packet, n);
		}
		if (!rc)
			ifnet_stat_increment_out(ifp, 1, (uint32_t)mbuf_pkthdr_len(packet), 0);
		else
			ifnet_stat_increment_out(ifp, 0, 0, 1);
		leave();
	}
	free_input(packet);
	return rc;
}

int
hk_cell_input(uint64_t epoch, mbuf_t packet)
{
	struct cell_lease l;
	protocol_family_t family = 0;
	if (!enter(NULL, epoch, &l)) {
		free_input(packet);
		return ENETDOWN;
	}
	int rc = raw_family(packet, l.mtu, &family);
	if (!rc && !(l.families & family_bit(family)))
		rc = EAFNOSUPPORT;
	if (!rc)
		rc = ifnet_reference(l.ifp);
	if (rc) {
		ifnet_stat_increment_in(l.ifp, 0, 0, 1);
		free_input(packet);
		leave();
		return rc;
	}
	struct ifnet_stat_increment_param stats = {.packets_in = 1,
	                                           .bytes_in = (uint32_t)mbuf_pkthdr_len(packet)};
	mbuf_pkthdr_setrcvif(packet, l.ifp);
	mbuf_pkthdr_setheader(packet, NULL);
	/* Pin the actual ifnet object, not the provider. DLIL may reenter or
	 * detach from here. demux checks this exact ifp if input was queued. */
	leave();
	rc = ifnet_input(l.ifp, packet, &stats); /* DLIL consumes input on all outcomes. */
	(void)ifnet_release(l.ifp);
	return rc;
}

static errno_t
demux(ifnet_t ifp, mbuf_t packet, char *header, protocol_family_t *proto)
{
	(void)header;
	struct cell_lease l;
	if (!enter(ifp, 0, &l))
		return ENETDOWN;
	int rc = raw_family(packet, l.mtu, proto);
	if (!rc && !(l.families & family_bit(*proto)))
		rc = EAFNOSUPPORT;
	leave();
	return rc;
}

static errno_t
protocol_input(ifnet_t ifp, protocol_family_t p, mbuf_t m, char *header)
{
	(void)header;
	struct cell_lease l;
	if (!enter(ifp, 0, &l)) {
		free_input(m);
		return 0;
	}
	bool allowed = (l.families & family_bit(p)) != 0;
	leave();
	int rc = allowed ? proto_input(p, m) : EAFNOSUPPORT;
	if (rc) {
		free_input(m);
		ifnet_stat_increment_in(ifp, 0, 0, 1);
	}
	return 0; /* Owner has consumed the mbuf even on rejection. */
}

static errno_t
pre_output(ifnet_t ifp, protocol_family_t p, mbuf_t *m, const struct sockaddr *dst, void *route, char *type,
           char *lladdr)
{
	(void)route;
	(void)type;
	(void)lladdr;
	struct cell_lease l;
	protocol_family_t actual = 0;
	if (!enter(ifp, 0, &l))
		return ENETDOWN;
	int rc = m ? output_family(&l, *m, &actual) : EINVAL;
	if (!rc && (actual != p || (dst && dst->sa_family != p)))
		rc = EAFNOSUPPORT;
	leave();
	return rc;
}

static errno_t
add_proto(ifnet_t ifp, protocol_family_t p, const struct ifnet_demux_desc *desc, uint32_t count)
{
	(void)desc;
	(void)count;
	lck_mtx_lock(&cell_lock);
	bool ok = ifp == binding.ifp && (binding.families & family_bit(p));
	lck_mtx_unlock(&cell_lock);
	return ok ? 0 : EPROTONOSUPPORT;
}

static errno_t
del_proto(ifnet_t ifp, protocol_family_t p)
{
	(void)ifp;
	(void)p;
	return 0;
}

static errno_t
cell_ioctl(ifnet_t ifp, u_long cmd, void *arg)
{
	lck_mtx_lock(&cell_lock);
	bool active = binding.active && ifp == binding.ifp;
	uint32_t mtu = binding.mtu;
	lck_mtx_unlock(&cell_lock);
	switch (cmd) {
	case SIOCSIFFLAGS:
		return 0; /* DLIL owns IFF_UP. It cannot reconnect an offline transport. */
	case SIOCSIFADDR:
		return active ? ifnet_set_flags(ifp, IFF_UP, IFF_UP) : ENETDOWN;
	case SIOCSIFMTU:
		return active && arg && ((struct ifreq *)arg)->ifr_mtu == (int)mtu ? 0 : EINVAL;
	default:
		return EOPNOTSUPP;
	}
}

static void
link_event(ifnet_t ifp, uint32_t kind)
{
	_Alignas(struct kern_event_msg) char bytes[KEV_MSG_HEADER_SIZE + sizeof(struct net_event_data)] = {
	    0};
	struct kern_event_msg *msg = (void *)bytes;
	struct net_event_data *net = (void *)(bytes + KEV_MSG_HEADER_SIZE);
	msg->total_size = sizeof(bytes);
	msg->vendor_code = KEV_VENDOR_APPLE;
	msg->kev_class = KEV_NETWORK_CLASS;
	msg->kev_subclass = KEV_DL_SUBCLASS;
	msg->event_code = kind;
	net->if_family = ifnet_family(ifp);
	net->if_unit = ifnet_unit(ifp);
	strlcpy(net->if_name, ifnet_name(ifp), sizeof(net->if_name));
	ifnet_event(ifp, msg);
}

static void
detached(ifnet_t ifp)
{
	/* ifnet_detach is asynchronous. Serialize final release with start/stop,
	 * including an externally initiated stack detach. */
	lck_mtx_lock(&cell_control);
	lck_mtx_lock(&cell_lock);
	if (binding.ifp == ifp) {
		drain_locked();
		binding.ifp = NULL;
		binding.transport = (struct hk_cell_transport){0};
		binding.families = 0;
		binding.mtu = 0;
	}
	lck_mtx_unlock(&cell_lock);
	(void)ifnet_release(ifp);
	lck_mtx_unlock(&cell_control);
}

int
hk_cell_open(uint64_t epoch, const struct hk_cell_format *f, const struct hk_cell_bearer *v4,
             const struct hk_cell_bearer *v6, const struct hk_cell_transport *t)
{
	unsigned families, mtu;
	lck_mtx_lock(&cell_control);
	int rc = qualify(epoch, f, v4, v6, t, &families, &mtu);
	if (rc)
		goto done;
	lck_mtx_lock(&cell_lock);
	if (binding.ifp || epoch <= binding.last_epoch) {
		rc = binding.ifp ? EBUSY : ESTALE;
		lck_mtx_unlock(&cell_lock);
		goto done;
	}
	lck_mtx_unlock(&cell_lock);
	struct ifnet_init_eparams init = {.ver = IFNET_INIT_CURRENT_VERSION,
	                                  .len = sizeof(init),
	                                  .flags = IFNET_INIT_LEGACY,
	                                  .name = "pdp_ip",
	                                  .unit = 0,
	                                  .family = IFNET_FAMILY_CELLULAR,
	                                  .type = IFT_CELLULAR,
	                                  .output = cell_output,
	                                  .demux = demux,
	                                  .add_proto = add_proto,
	                                  .del_proto = del_proto,
	                                  .ioctl = cell_ioctl,
	                                  .detach = detached};
	ifnet_t ifp = NULL;
	rc = ifnet_allocate_extended(&init, &ifp);
	if (rc)
		goto done;
	lck_mtx_lock(&cell_lock);
	binding.ifp = ifp;
	binding.epoch = epoch;
	binding.mtu = mtu;
	binding.families = families;
	lck_mtx_unlock(&cell_lock);
	rc = ifnet_set_flags(ifp, IFF_POINTOPOINT | IFF_MULTICAST, 0xffff);
	if (!rc)
		rc = ifnet_set_mtu(ifp, mtu);
	if (!rc)
		rc = ifnet_attach(ifp, NULL);
	if (rc) {
		lck_mtx_lock(&cell_lock);
		binding.ifp = NULL;
		lck_mtx_unlock(&cell_lock);
		ifnet_release(ifp);
		goto done;
	}
	struct ifnet_attach_proto_param proto = {.input = protocol_input, .pre_output = pre_output};
	if (families & HK_CELL_IPV4)
		rc = ifnet_attach_protocol(ifp, PF_INET, &proto);
	if (!rc && (families & HK_CELL_IPV6))
		rc = ifnet_attach_protocol(ifp, PF_INET6, &proto);
	if (!rc)
		rc = ifnet_set_flags(ifp, IFF_RUNNING, IFF_RUNNING);
	if (!rc && t->rx_host)
		rc = mdm_mhi_tx_bind(t->rx_host, epoch, ifp, mtu);
	if (rc) {
		(void)ifnet_detach(ifp);
		goto done;
	}
	lck_mtx_lock(&cell_lock);
	if (v4) {
		memcpy(binding.ipv4, v4->local, sizeof(binding.ipv4));
		binding.ipv4_mtu = v4->mtu;
		binding.wds4 = v4->source;
	}
	if (v6) {
		memcpy(binding.ipv6, v6->local, sizeof(binding.ipv6));
		binding.ipv6_mtu = v6->mtu;
		binding.wds6 = v6->source;
		binding.ipv6_prefix_length = v6->ipv6_prefix_length;
	}
	binding.next_rx_slot = 0;
	binding.discard_to_eot = false;
	binding.transport = *t;
	binding.last_epoch = epoch;
	binding.active = true;
	lck_mtx_unlock(&cell_lock);
	link_event(ifp, KEV_DL_LINK_ON);
done:
	lck_mtx_unlock(&cell_control);
	return rc;
}

static int
close_locked(uint64_t epoch)
{
	lck_mtx_lock(&cell_lock);
	ifnet_t ifp = binding.ifp;
	bool live = binding.active;
	if (!ifp || epoch != binding.epoch) {
		lck_mtx_unlock(&cell_lock);
		return ESTALE;
	}
	drain_locked();
	binding.transport = (struct hk_cell_transport){0};
	lck_mtx_unlock(&cell_lock);
	if (live) {
		ifnet_set_flags(ifp, 0, IFF_RUNNING | IFF_UP);
		link_event(ifp, KEV_DL_LINK_OFF);
	}
	return ifnet_detach(ifp);
}

int
hk_cell_close(uint64_t epoch)
{
	lck_mtx_lock(&cell_control);
	int rc = close_locked(epoch);
	lck_mtx_unlock(&cell_control);
	return rc;
}

int
hk_cell_recheck(uint64_t epoch, const struct hk_cell_format *f, const struct hk_cell_bearer *v4,
                const struct hk_cell_bearer *v6, const struct hk_cell_transport *t)
{
	unsigned families = 0, mtu = 0;
	int why = qualify(epoch, f, v4, v6, t, &families, &mtu);
	lck_mtx_lock(&cell_control);
	lck_mtx_lock(&cell_lock);
	if (!binding.active || epoch != binding.epoch) {
		lck_mtx_unlock(&cell_lock);
		lck_mtx_unlock(&cell_control);
		return ESTALE;
	}
	if (!why &&
	    (families != binding.families || mtu != binding.mtu || t->ctx != binding.transport.ctx ||
	     t->copy_tx != binding.transport.copy_tx || t->rx_host != binding.transport.rx_host ||
	     (t->rx_host && ((v4 && !same_wds_client(&binding.wds4, &v4->source)) ||
	                     (v6 && !same_wds_client(&binding.wds6, &v6->source)))) ||
	     (v4 &&
	      (v4->mtu != binding.ipv4_mtu || memcmp(v4->local, binding.ipv4, sizeof(binding.ipv4)))) ||
	     (v6 && (v6->mtu != binding.ipv6_mtu || v6->ipv6_prefix_length != binding.ipv6_prefix_length ||
	             memcmp(v6->local, binding.ipv6, sizeof(binding.ipv6))))))
		why = ENETRESET;
	lck_mtx_unlock(&cell_lock);
	if (why) {
		int rc = close_locked(epoch);
		if (rc && rc != ENXIO)
			why = rc;
	}
	lck_mtx_unlock(&cell_control);
	return why;
}

/* Materialize an MHI transport only from the live duplex owner. WDS status
 * from an unbound handle can explain a refusal; only an independently
 * configured family client with matching address/MTU can admit a bearer. */
static int
mhi_evidence(struct mhi_host *host, uint64_t epoch, const struct hk_cell_wds_status *observed,
    const struct hk_cell_bearer *v4, const struct hk_cell_bearer *v6, struct hk_cell_transport *t)
{
	if (!host || !epoch || !observed)
		return EINVAL;
	const struct hk_cell_wds_client *s = &observed->source;
	if (!s->handle || s->service != HK_QMI_WDS_SERVICE || s->epoch != epoch)
		return ESTALE;
	if (!wds_connected(&observed->status))
		return ENETDOWN;
	const struct hk_cell_bearer *family = s->family == 4 ? v4 : s->family == 6 ? v6 : NULL;
	if (!family)
		return EADDRNOTAVAIL;
	if (!same_wds_client(s, &family->source))
		return ESTALE;
	struct mdm_mhi_tx_status tx;
	int rc = mdm_mhi_tx_status(host, epoch, &tx);
	if (rc || !tx.ready)
		return rc ? rc : ENETDOWN;
	struct mhi_host_channel *dl;
	rc = mdm_rx_acquire(host, epoch, &dl);
	if (rc)
		return rc;
	uint32_t capacity = tx.capacity < dl->rx.capacity ? tx.capacity : dl->rx.capacity;
	*t = (struct hk_cell_transport){.epoch = epoch, .ul_channel = MDM_IP_HW0_UL, .ul_er = MDM_IP_HW0_UL_ER,
	    .dl_channel = dl->config->number, .dl_er = dl->config->event_ring, .max_datagram = capacity,
	    .ul_ready = true, .dl_ready = true, .rx_host = host};
	host->ops->unlock(host->context);
	return 0;
}

int
hk_cell_mhi_open(struct mhi_host *host, uint64_t epoch, const struct hk_cell_format *f,
    const struct hk_cell_wds_status *observed, const struct hk_cell_bearer *v4,
    const struct hk_cell_bearer *v6)
{
	struct hk_cell_transport t;
	int rc = mhi_evidence(host, epoch, observed, v4, v6, &t);
	return rc ? rc : hk_cell_open(epoch, f, v4, v6, &t);
}

int
hk_cell_mhi_recheck(struct mhi_host *host, uint64_t epoch, const struct hk_cell_format *f,
    const struct hk_cell_wds_status *observed, const struct hk_cell_bearer *v4,
    const struct hk_cell_bearer *v6)
{
	if (!host || !epoch || !observed)
		return EINVAL;
	lck_mtx_lock(&cell_lock);
	const struct hk_cell_wds_client *s = &observed->source;
	bool relevant = binding.active && binding.transport.rx_host == host && binding.epoch == epoch &&
	    ((s->family == 4 && (binding.families & HK_CELL_IPV4) && same_wds_client(s, &binding.wds4)) ||
	     (s->family == 6 && (binding.families & HK_CELL_IPV6) && same_wds_client(s, &binding.wds6)));
	lck_mtx_unlock(&cell_lock);
	if (!relevant)
		return ESTALE;
	struct hk_cell_transport t;
	int rc = mhi_evidence(host, epoch, observed, v4, v6, &t);
	if (rc) {
		hk_cell_mhi_revoke(host, epoch);
		return rc;
	}
	return hk_cell_recheck(epoch, f, v4, v6, &t);
}

/* The MHI recv lock protects ownership, while the RX mutex orders slots for
 * this ifnet. The stack may reenter after both have been released. A slot
 * number is never reused within this provider's generation. */
static void
cell_consume_transfer(struct mhi_rx_credit *rx)
{
	memset(rx->buffer, 0, rx->capacity);
	rx->received = 0;
	rx->owner = MHI_RX_IDLE;
}

int
hk_cell_mhi_ingress(struct mhi_host *host, uint64_t epoch)
{
	struct cell_lease lease;
	struct mhi_host_channel *dl;
	mbuf_t packet = NULL;
	int error;

	if (!host || !enter(NULL, epoch, &lease))
		return ENETDOWN;
	if (lease.transport.rx_host != host) {
		leave();
		return ESTALE;
	}

	lck_mtx_lock(&cell_rx_lock);
	error = mdm_rx_acquire(host, epoch, &dl);
	if (error)
		goto done;
	struct mhi_rx_credit *rx = &dl->rx;
	if (rx->owner != MHI_RX_COMPLETE) {
		error = EAGAIN;
		goto unlock_host;
	}
	uint32_t slot = rx->slot;
	size_t length = rx->received;
	unsigned code = rx->completion_code;
	bool gap = slot != binding.next_rx_slot;

	if (code != MHI_CODE_EOT || gap || binding.discard_to_eot || length == 0 || length > lease.mtu) {
		binding.discard_to_eot = code != MHI_CODE_EOT;
		binding.next_rx_slot = slot + 1;
		cell_consume_transfer(rx);
		error = (length == 0 || length > lease.mtu) ? EMSGSIZE : EPROTO;
		goto unlock_host;
	}
	host->ops->unlock(host->context);

	/* XNU may return a chain. Allocate outside the MHI lock, then verify
	 * the same slot still owns the same bytes before copying anything. */
	error = mbuf_allocpacket(MBUF_DONTWAIT, length, NULL, &packet);
	if (error)
		goto done;
	error = mdm_rx_acquire(host, epoch, &dl);
	if (error)
		goto done;
	rx = &dl->rx;
	if (rx->owner != MHI_RX_COMPLETE || rx->slot != slot || rx->received != length ||
	    rx->completion_code != code) {
		error = ESTALE;
		goto unlock_host;
	}
	device_fence();
	size_t copied = 0;
	for (mbuf_t m = packet; m != NULL; m = mbuf_next(m)) {
		size_t capacity = mbuf_len(m) + mbuf_trailingspace(m);
		size_t amount = length - copied < capacity ? length - copied : capacity;
		if (amount)
			memcpy(mbuf_data(m), rx->buffer + copied, amount);
		mbuf_setlen(m, amount);
		copied += amount;
	}
	if (copied != length) {
		error = ENOBUFS;
		goto unlock_host; /* caller can retry the same provider slot */
	}
	mbuf_pkthdr_setlen(packet, length);
	binding.next_rx_slot = slot + 1;
	cell_consume_transfer(rx);
unlock_host:
	host->ops->unlock(host->context);
done:
	lck_mtx_unlock(&cell_rx_lock);
	leave();
	if (error) {
		free_input(packet);
		return error;
	}
	/* Recheck the binding after allocation/copy, validate the IP header and
	 * exact datagram length, then transfer mbuf ownership to DLIL. */
	return hk_cell_input(epoch, packet);
}

void
hk_cell_mhi_revoke(struct mhi_host *host, uint64_t epoch)
{
	if (!host || !epoch)
		return;
	lck_mtx_lock(&cell_control);
	lck_mtx_lock(&cell_lock);
	bool ours = binding.active && binding.epoch == epoch && binding.transport.rx_host == host;
	lck_mtx_unlock(&cell_lock);
	if (ours)
		(void)close_locked(epoch);
	lck_mtx_unlock(&cell_control);
}
