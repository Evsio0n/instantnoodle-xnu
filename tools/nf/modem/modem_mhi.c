/*
 * The SDX55's MHI host: wifi/lx/src/mhi_host.c under other names (the Wi-Fi's copy is in the driver's object), with a boot of its own for a flashless modem. See modem_boot.inc.
 */
#ifdef KERNEL
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <stdbool.h>

#define _KERNEL 1
#endif
#include "modem_mhi.h"
#if defined(KERNEL) || defined(HK_CELL_MHI_TEST)
#include "hk_cell_rawip.h"
#endif
#if __has_include("mhi_host.c")
#include "mhi_host.c"
#else
#include "../../../wifi/lx/src/mhi_host.c"
#endif

uint32_t
mdm_mhi_ee(struct mhi_host *h)
{
	return device_ee(h);
}

uint32_t
mdm_mhi_state(struct mhi_host *h)
{
	return device_state(h);
}

/* The device tree configures 512 TREs in either direction and ER 7/8 in a
 * 17-entry event context table. Linux allocates the downlink pages in the
 * netdev after both START completions. This bounded test stops before that
 * point: these two channels own empty TRE rings and no payload buffers. */
int
mdm_mhi_ip_hw0_config(struct mhi_host_config *config)
{
	if (config == NULL || config->channel_count != 4 || config->event_count != 3 ||
	    config->max_channels <= MDM_IP_HW0_DL || config->hardware_event_count != 0)
		return EINVAL;
	config->channels[4] = (struct mhi_host_channel_config){
		.number = MDM_IP_HW0_UL, .elements = 512, .event_ring = MDM_IP_HW0_UL_ER,
		.ee_mask = 1u << MHI_HOST_EE_AMSS, .command_only = true
	};
	config->channels[5] = (struct mhi_host_channel_config){
		.number = MDM_IP_HW0_DL, .elements = 512, .event_ring = MDM_IP_HW0_DL_ER, .inbound = true,
		.ee_mask = 1u << MHI_HOST_EE_AMSS, .command_only = true
	};
	config->events[MDM_IP_HW0_UL_ER] = (struct mhi_host_event_config){
		.elements = 1024, .vector = 5, .moderation_ms = 5, .burst = true
	};
	config->events[MDM_IP_HW0_DL_ER] = (struct mhi_host_event_config){
		.elements = 2048, .vector = 6, .moderation_ms = 5, .burst = true,
		.client_managed = true, .uncached = true
	};
	config->channel_count = 6;
	config->event_count = 17;
	config->hardware_event_count = 10;
	return 0;
}

/*
 * This device's boot: as mhi_host_power_up() up to the secondary boot loader, but with no BHIe: its channels for that environment started, and back to the caller,
 * who talks Sahara on them. (BHIe's offset may read 0 in the PBL, which mhi_host_power_up() would refuse.)
 */
int
mdm_mhi_to_sbl(struct mhi_host *h)
{
	uint32_t ee, status;
	uint64_t began;
	int error = 0;

	h->ops->lock(h->context);
	/* The SDX55 board description sets burst mode on both IP_HW0 transfer
	 * channels and db-mode-switch only on the uplink. Apply this to channels
	 * the caller has configured; Sahara, IPCR and EFS use ordinary doorbells. */
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		struct mhi_host_channel *ch = &h->channels[c];

		if (ch->config->number == 100) {
			ch->transfer_db = MHI_HOST_DB_BURST_SWITCH;
		} else if (ch->config->number == 101) {
			ch->transfer_db = MHI_HOST_DB_BURST;
		}
	}
	h->powered = true;
	h->mission = h->failed = h->mission_told = false;
	h->wakes = 0;
	h->channel_doorbells = h->event_doorbells = 0;
	h->bhi = rd(h, MHI_REG_BHIOFF);
	h->bhie = rd(h, MHI_REG_BHIEOFF);
	wr(h, h->bhi + MHI_BHI_INTVEC, 0);
	ee = device_ee(h);
	status = rd(h, MHI_REG_MHISTATUS);
	h->ee = ee;
	h->state = status >> 8 & 0xff;
	say(h, "mhi: BHI at %#x, BHIe at %#x: EE %u, state %#x (MHISTATUS %#x)", h->bhi, h->bhie, ee, h->state, status);
	if (ee != MHI_HOST_EE_PBL) {
		error = EIO;
		goto out;
	}
	/* (MHI's own state 0xff is SYS_ERR, and it reads so after the warm reset; run 060451's BHI did not move from there: as mhi_host_power_up(), and Linux, a soft reset first, which clears INTVEC) */
	if (h->state == MHI_HOST_STATE_SYS_ERR || (status & MHI_STATUS_SYSERR)) {
		error = device_reset(h);
		say(h, "mhi: soft reset: %d, MHICTRL %#x, MHISTATUS %#x, EE %u", error, rd(h, MHI_REG_MHICTRL), rd(h, MHI_REG_MHISTATUS), device_ee(h));
		if (error != 0) {
			goto out;
		}
		h->state = device_state(h);
		h->failed = false;
		wr(h, h->bhi + MHI_BHI_INTVEC, 0);
	}
	wr(h, h->bhi + MHI_BHI_STATUS, 0);
	wr(h, h->bhi + MHI_BHI_IMGADDR_HIGH, (uint32_t)(h->image_address >> 32));
	wr(h, h->bhi + MHI_BHI_IMGADDR_LOW, (uint32_t)h->image_address);
	wr(h, h->bhi + MHI_BHI_IMGSIZE, (uint32_t)h->image_size);
	device_fence();
	began = now_ms(h);
	wr(h, h->bhi + MHI_BHI_IMGTXDB, transaction(h));
	if (wait_for(h, bhi_done, h->config.timeout_ms) != 0 || (rd(h, h->bhi + MHI_BHI_STATUS) >> 30) != 2) {
		say(h, "mhi: BHI: the boot loader was not taken after %llu ms (BHI_STATUS %#x, error %#x, EE %u)", (unsigned long long)(now_ms(h) - began), rd(h, h->bhi + MHI_BHI_STATUS),
		    rd(h, h->bhi + MHI_BHI_ERRCODE), device_ee(h));
		say(h, "mhi: BHI reads: image at %#x, %u bytes; debug %#x %#x %#x; MHISTATUS %#x", rd(h, h->bhi + MHI_BHI_IMGADDR_LOW), rd(h, h->bhi + MHI_BHI_IMGSIZE),
		    rd(h, h->bhi + 0x34), rd(h, h->bhi + 0x38), rd(h, h->bhi + 0x3c), rd(h, MHI_REG_MHISTATUS));
		error = EIO;
		goto out;
	}
	say(h, "mhi: BHI: the boot loader (%zu bytes) is taken", h->image_size);
	if (wait_for(h, ready, h->config.timeout_ms) != 0) {
		say(h, "mhi: the device does not say READY (MHICTRL %#x, MHISTATUS %#x, EE %u)", rd(h, MHI_REG_MHICTRL), rd(h, MHI_REG_MHISTATUS), device_ee(h));
		error = ETIMEDOUT;
		goto out;
	}
	if ((error = setup_mmio(h)) != 0 || (error = activate_event_rings(h, false)) != 0)
		goto out;
	set_state(h, MHI_HOST_STATE_M0);
	if (wait_for(h, in_sbl, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the device does not come to its secondary boot loader (EE %u, state %#x)", h->ee, h->state);
		error = h->failed ? EIO : ETIMEDOUT;
		goto out;
	}
	if (wait_for(h, device_awake, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the device is not in M0 (state %#x)", h->state);
		error = EIO;
		goto out;
	}
	for (unsigned c = 0; c < h->config.channel_count && error == 0; c++) {
		if (h->channels[c].config->ee_mask & 1u << MHI_HOST_EE_SBL) {
			error = channel_start(h, &h->channels[c]);
		}
	}
	if (error == 0) {
		say(h, "mhi: in its secondary boot loader (EE %u), Sahara's channels started", h->ee);
	}
out:
	if (error != 0) {
		h->powered = false;
	}
	h->ops->unlock(h->context);
	return error;
}

/* After Sahara: the mission; what Linux's mhi_pm_mission_mode_transition() does, the event rings rung again, the channels for it started. */
int
mdm_mhi_to_mission(struct mhi_host *h, uint32_t ms)
{
	int error = 0;

	h->ops->lock(h->context);
	if (wait_for(h, mission, ms) != 0 || h->failed) {
		say(h, "mhi: the image does not come to its mission (EE %u, state %#x)", device_ee(h), device_state(h));
		error = h->failed ? EIO : ETIMEDOUT;
		goto out;
	}
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		if (!(h->channels[c].config->ee_mask & 1u << MHI_HOST_EE_AMSS)) {
			h->channels[c].running = false;
		}
	}
	if (wait_for(h, mission_told, 1000) != 0) {
		say(h, "mhi: no event of the device's for its mission in 1 s: its channels are started all the same");
	}
	if (wait_for(h, device_awake, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the device is not in M0 (state %#x)", device_state(h));
		error = EIO;
		goto out;
	}
	ring_active_events(h);
	if ((error = activate_event_rings(h, true)) != 0)
		goto out;
	for (unsigned c = 0; c < h->config.channel_count && error == 0; c++) {
		if (!h->channels[c].config->command_only && !h->channels[c].config->client_rx && !h->channels[c].config->client_tx &&
		    (h->channels[c].config->ee_mask & 1u << MHI_HOST_EE_AMSS)) {
			error = channel_start(h, &h->channels[c]);
		}
	}
	if (error == 0) {
		say(h, "mhi: THE DEVICE RUNS ITS IMAGE (EE %u), its channels are started", h->ee);
	}
out:
	h->ops->unlock(h->context);
	return error;
}

/* The sole board owner runs one UL/DL START followed by DL/UL RESET, as in
 * mhi_prepare_for_transfer and mhi_unprepare_from_transfer. No transfer TRE
 * is published. A missing command completion leaves device ownership
 * uncertain: retain the entire allocation and let the board quiesce. */
int
mdm_mhi_ip_hw0_trial(struct mhi_host *h, struct mdm_ip_hw0_receipt *r)
{
	struct mhi_host_channel *channels[2];
	int error;

	if (r == NULL)
		return EINVAL;
	*r = (struct mdm_ip_hw0_receipt){ 0 };
	if (h == NULL || !h->allocated)
		return ENXIO;
	if (!mhi_host_ownership_clean(h)) {
		r->quarantined = true;
		return EIO;
	}
	if ((error = mhi_host_wake_get(h)) != 0) {
		mhi_host_wake_put(h);
		return error;
	}
	h->ops->lock(h->context);
	channels[0] = channel_numbered(h, MDM_IP_HW0_UL);
	channels[1] = channel_numbered(h, MDM_IP_HW0_DL);
	if (!h->powered || !h->mission_told || h->ee != MHI_HOST_EE_AMSS || h->state != MHI_HOST_STATE_M0 ||
	    h->failed || !mhi_host_ownership_clean(h) ||
	    !h->event_active[MDM_IP_HW0_UL_ER] || !h->event_active[MDM_IP_HW0_DL_ER]) {
		error = ENOTCONN;
		goto out;
	}
	uint32_t cfg = rd(h, MHI_REG_MHICFG);
	r->ner = (cfg >> 16) & 0xff;
	r->nhwer = cfg >> 24;
	if (r->ner != 17 || r->nhwer != 10) {
		error = EIO;
		goto out;
	}
	for (unsigned i = 0; i < 2; i++) {
		struct mhi_host_channel *ch = channels[i];
		if (!ch || !ch->config->command_only || ch->buffers != NULL || ch->ring.rp != ch->ring.wp ||
		    ch->running || ch->config->inbound != (i == 1) ||
		    ch->config->event_ring != (i == 0 ? MDM_IP_HW0_UL_ER : MDM_IP_HW0_DL_ER)) {
			error = EINVAL;
			goto out;
		}
	}
	for (unsigned i = 0; i < 2; i++) {
		r->start_attempted |= 1u << i;
		if ((error = channel_start(h, channels[i])) != 0 || h->failed)
			goto uncertain;
		r->start_ack |= 1u << i;
	}
	/* ER8 belongs to the client. A quiet ring is expected: no receive
	 * credit was supplied. This call never runs a transmitter. */
	h->ops->unlock(h->context);
	error = mhi_host_poll_event(h, MDM_IP_HW0_DL_ER, 32, &r->er8_events);
	h->ops->lock(h->context);
	if (error || h->failed || r->er8_events >= 32)
		goto uncertain;
	for (unsigned n = 2; n != 0; n--) {
		unsigned i = n - 1;
		struct mhi_host_channel *ch = channels[i];
		if (ch->ring.rp != ch->ring.wp)
			goto uncertain;
		r->reset_attempted |= 1u << i;
		if ((error = command(h, ch, MHI_TYPE_RESET_CHAN)) != 0 || h->failed)
			goto uncertain;
		r->reset_ack |= 1u << i;
		ch->running = false;
	}
	device_fence();
	for (unsigned i = 0; i < 2; i++) {
		struct mhi_host_channel *ch = channels[i];
		uint8_t *ctx = channel_context(h, ch->config->number);
		uint64_t rp = get64(ctx + CTX_RP), wp = get64(ctx + CTX_WP);
		if (rp == wp && (rp == ch->ring.address || (rp == 0 && ctx[CTX_CFG] == 0)) &&
		    ch->ring.rp == ch->ring.wp)
			r->empty |= 1u << i;
		if (ch->buffers != NULL)
			r->buffers |= 1u << i;
	}
	struct mhi_host_channel *ipcr_ul = channel_numbered(h, 20), *ipcr_dl = channel_numbered(h, 21);
	r->ipcr_ul_state = channel_context(h, 20)[CTX_CFG];
	r->ipcr_dl_state = channel_context(h, 21)[CTX_CFG];
	r->ipcr_running = (ipcr_ul && ipcr_ul->running ? 1u : 0u) | (ipcr_dl && ipcr_dl->running ? 2u : 0u);
	r->ee = device_ee(h);
	r->state = device_state(h);
	if (r->empty != 3 || r->buffers || r->ipcr_running != 3 || r->ee != MHI_HOST_EE_AMSS ||
	    r->state != MHI_HOST_STATE_M0)
		goto uncertain;
	goto out;
uncertain:
	__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
	r->quarantined = true;
	if (!error)
		error = EIO;
out:
	if (!mhi_host_ownership_clean(h))
		r->quarantined = true;
	h->ops->unlock(h->context);
	mhi_host_wake_put(h);
	return error;
}

/*
 * Where the device writes its failure reason when it crashes: Android's mhi_init_sfr() (drivers/bus/mhi/core/mhi_init.c, for dt mhi,sfr-support), sent in the same place, on the way into the
 * mission: a command of type 73, the buffer's address, its length less one. The shared host keeps a command's result by channel and this one has none, so "done" is all that is known here.
 */
int
mdm_mhi_sfr_config(struct mhi_host *h, uint64_t address, uint32_t length)
{
	uint32_t at;
	uint64_t until;
	int error = 0;

	h->ops->lock(h->context);
	at = h->commands.wp;
	if (next(&h->commands, at) == h->commands.rp) {
		h->ops->unlock(h->context);
		return EBUSY;
	}
	write_element(element(&h->commands, at), address, length - 1, 73u << 16);
	h->commands.wp = next(&h->commands, at);
	put64(h->command_context + CTX_WP, element_address(&h->commands, h->commands.wp));
	doorbell(h, MHI_REG_CRDB, element_address(&h->commands, h->commands.wp));
	for (until = now_ms(h) + h->config.timeout_ms; h->commands.rp != h->commands.wp;) {
		service_all(h);
		if (now_ms(h) > until) {
			error = ETIMEDOUT;
			break;
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
	h->ops->unlock(h->context);
	return error;
}

/* The cached ARM64 OEM mhi_netbuf occupies 88 trailing bytes in its 32-KiB
 * page (checked from its two source declarations by the host test). Keep the
 * same programmed maximum. Our ownership metadata stays in CPU-only state. */
enum { MDM_RX_CAPACITY = MDM_IP_HW0_MRU - 88, MDM_RX_BEI = 1u << 10 };

int
mdm_mhi_rx_config(struct mhi_host_config *config)
{
	int rc = mdm_mhi_ip_hw0_config(config);
	if (!rc) {
		config->channels[5].command_only = false;
		config->channels[5].client_rx = true;
	}
	return rc;
}

static int
mdm_rx_quarantine(struct mhi_host *h, struct mhi_rx_credit *rx, int error)
{
	rx->owner = MHI_RX_QUARANTINED;
	h->failed = true;
	__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
	return error ? error : EIO;
}

int
mdm_mhi_tx_config(struct mhi_host_config *config)
{
	int rc = mdm_mhi_rx_config(config);
	if (!rc) {
		config->channels[4].command_only = false;
		config->channels[4].client_tx = true;
	}
	return rc;
}

static bool
mdm_tx_ready(struct mhi_host *h, struct mhi_host_channel *ul)
{
	struct mhi_host_channel *dl = channel_numbered(h, MDM_IP_HW0_DL);
	struct mhi_tx_pool *tx = &ul->tx;
	return tx->base && tx->epoch && !tx->closing && !tx->retired && !tx->quarantined &&
	    dl && dl->running && dl->rx.epoch == tx->epoch && !dl->rx.closing &&
	    (dl->rx.owner == MHI_RX_IDLE || dl->rx.owner == MHI_RX_POSTED || dl->rx.owner == MHI_RX_COMPLETE) && ul->running &&
	    h->powered && !h->failed && h->mission_told && mhi_host_ownership_clean(h) &&
	    h->event_active[MDM_IP_HW0_UL_ER] && h->event_active[MDM_IP_HW0_DL_ER] &&
	    h->state == MHI_HOST_STATE_M0 && device_state(h) == MHI_HOST_STATE_M0 &&
	    device_ee(h) == MHI_HOST_EE_AMSS;
}

static int
mdm_tx_lookup(struct mhi_host *h, uint64_t epoch, struct mhi_host_channel **out)
{
	struct mhi_host_channel *ul = channel_numbered(h,MDM_IP_HW0_UL);
	if (!ul || !ul->config->client_tx || !ul->tx.base) return ENOTCONN;
	if (ul->tx.epoch != epoch) return ESTALE;
	*out = ul;
	return 0;
}

int
mdm_mhi_tx_status(struct mhi_host *h, uint64_t epoch, struct mdm_mhi_tx_status *out)
{
	if (!out) return EINVAL;
	*out = (struct mdm_mhi_tx_status){0};
	if (!h || !h->allocated || !epoch) return EINVAL;
	h->ops->lock(h->context);
	struct mhi_host_channel *ul;
	int rc = mdm_tx_lookup(h,epoch,&ul);
	if (!rc) {
		struct mhi_tx_pool *tx = &ul->tx;
		*out = (struct mdm_mhi_tx_status){
			.capacity=MHI_TX_CAPACITY, .buffers=MHI_TX_BUFFERS, .pending=tx->pending,
			.published=tx->submitted, .completed=tx->completed, .cancelled=tx->cancelled, .doorbells=tx->doorbells,
			.ready=mdm_tx_ready(h,ul), .bound=tx->producer != NULL, .retired=tx->retired,
			.quarantined=tx->quarantined || h->failed || !mhi_host_ownership_clean(h)
		};
	}
	h->ops->unlock(h->context);
	return rc;
}

int
mdm_mhi_tx_bind(struct mhi_host *h, uint64_t epoch, const void *producer, uint32_t mtu)
{
	if (!h || !h->allocated || !epoch || !producer || !mtu || mtu > MHI_TX_CAPACITY) return EINVAL;
	h->ops->lock(h->context);
	struct mhi_host_channel *ul;
	int rc = mdm_tx_lookup(h,epoch,&ul);
	if (!rc) {
		struct mhi_tx_pool *tx = &ul->tx;
		if (!mdm_tx_ready(h,ul)) rc = ENOTCONN;
		else if (tx->bound_once && (tx->producer != producer || tx->mtu != mtu)) rc = ESTALE;
		else { tx->producer=producer; tx->mtu=mtu; tx->bound_once=true; }
	}
	h->ops->unlock(h->context);
	return rc;
}

void
mdm_mhi_tx_unbind(struct mhi_host *h, uint64_t epoch, const void *producer)
{
	if (!h || !h->allocated || !epoch || !producer) return;
	h->ops->lock(h->context);
	struct mhi_host_channel *ul;
	if (!mdm_tx_lookup(h,epoch,&ul) && ul->tx.producer == producer) {
		ul->tx.producer = NULL;
		ul->tx.closing = true;
	}
	h->ops->unlock(h->context);
}

int
mdm_mhi_tx_submit(struct mhi_host *h, uint64_t epoch, const void *producer, size_t n,
    int (*copy)(void *,void *,size_t), void *source)
{
	if (!h || !h->allocated || !epoch || !producer || !copy || !source) return EINVAL;
	h->ops->lock(h->context);
	struct mhi_host_channel *ul;
	int rc = mdm_tx_lookup(h,epoch,&ul);
	if (rc) goto out;
	struct mhi_tx_pool *tx = &ul->tx;
	if (!tx->producer || !mdm_tx_ready(h,ul)) { rc = ENOTCONN; goto out; }
	if (tx->producer != producer) { rc = ESTALE; goto out; }
	if (!n || n > tx->mtu || n > MHI_TX_CAPACITY) { rc = EMSGSIZE; goto out; }
	if (tx->pending == MHI_TX_BUFFERS) { rc = EWOULDBLOCK; goto out; }
	uint32_t slot = ul->ring.wp, index = slot % MHI_TX_BUFFERS;
	if (tx->entry[index].live || next(&ul->ring,slot) == ul->ring.rp) {
		rc = tx_quarantine(h,ul);
		goto out;
	}
	uint8_t *dst = tx->base + (size_t)index * MHI_TX_CAPACITY;
	rc = copy(source,dst,n);
	if (rc || !mdm_tx_ready(h,ul)) {
		memset(dst,0,n);
		if (!rc) rc = ENOTCONN;
		goto out;
	}
	write_element(element(&ul->ring,slot),tx->iova + (uint64_t)index * MHI_TX_CAPACITY,(uint32_t)n,
	    MHI_TYPE_TRANSFER << 16 | MHI_TRE_IEOT | MDM_RX_BEI);
	tx->entry[index].slot = slot;
	tx->entry[index].length = (unsigned)n;
	tx->entry[index].live = true;
	tx->pending++;
	tx->submitted++;
	ul->ring.wp = next(&ul->ring,slot);
	publish_transfer_wp(h,ul);
	ring_channel(h,ul);
out:
	h->ops->unlock(h->context);
	return rc;
}

/* Single owner per freshly allocated controller: stale wire events have no
 * generation field. Do not recycle a transfer-ring slot in this generation
 * or reopen the same physical ring under a new generation. */
int
mdm_mhi_rx_begin(struct mhi_host *h, uint64_t epoch)
{
	if (!h || !h->allocated || !epoch)
		return EINVAL;
	h->ops->lock(h->context);
	struct mhi_host_channel *existing = channel_numbered(h, MDM_IP_HW0_DL);
	bool stale = existing && existing->rx.epoch != 0;
	h->ops->unlock(h->context);
	if (stale)
		return ESTALE;
	int rc = mhi_host_wake_get(h);
	if (rc) {
		mhi_host_wake_put(h);
		return rc;
	}
	h->ops->lock(h->context);
	struct mhi_host_channel *ul = channel_numbered(h, MDM_IP_HW0_UL);
	struct mhi_host_channel *dl = channel_numbered(h, MDM_IP_HW0_DL);
	if (!ul || !dl || (!ul->config->command_only && !ul->config->client_tx) || ul->config->inbound ||
	    !dl->config->client_rx || !dl->config->inbound ||
	    ul->config->event_ring != MDM_IP_HW0_UL_ER || dl->config->event_ring != MDM_IP_HW0_DL_ER ||
	    h->config.event_count != 17 || h->config.hardware_event_count != 10 ||
	    !h->event_active[MDM_IP_HW0_UL_ER] || !h->event_active[MDM_IP_HW0_DL_ER] ||
	    !h->powered || h->failed || !h->mission_told || !mhi_host_ownership_clean(h) ||
	    device_ee(h) != MHI_HOST_EE_AMSS || device_state(h) != MHI_HOST_STATE_M0) {
		rc = ENOTCONN;
		goto unavailable;
	}
	if (ul->running || dl->running || dl->rx.epoch || dl->rx.owner != MHI_RX_NEW) {
		rc = ESTALE;
		goto unavailable;
	}
	struct mhi_rx_credit *rx = &dl->rx;
	if (!h->ops->dma_alloc_uncached) {
		rc = ENOTSUP;
		goto unavailable;
	}
	rx->buffer = h->ops->dma_alloc_uncached(h->context, MDM_IP_HW0_MRU, MDM_IP_HW0_MRU, &rx->iova);
	if (!rx->buffer) {
		rc = ENOMEM;
		goto unavailable;
	}
	if ((rx->iova & (MDM_IP_HW0_MRU - 1)) || rx->iova < h->config.iova_start ||
	    rx->iova > h->config.iova_stop || h->config.iova_stop - rx->iova < MDM_IP_HW0_MRU - 1) {
		rc = mdm_rx_quarantine(h, rx, EFAULT);
		goto unavailable;
	}
	rx->capacity = MDM_RX_CAPACITY;
	rx->epoch = epoch;
	if (ul->config->client_tx) {
		struct mhi_tx_pool *tx = &ul->tx;
		size_t bytes = MHI_TX_BUFFERS * MHI_TX_CAPACITY;
		tx->base = h->ops->dma_alloc_uncached(h->context,bytes,MHI_TX_CAPACITY,&tx->iova);
		if (!tx->base) { rc=ENOMEM; goto unavailable; }
		tx->epoch = epoch;
		if ((tx->iova & (MHI_TX_CAPACITY-1)) || tx->iova < h->config.iova_start ||
		    tx->iova > h->config.iova_stop || bytes-1 > h->config.iova_stop-tx->iova) {
			rc = tx_quarantine(h,ul);
			goto unavailable;
		}
	}
	if ((rc = channel_start(h, ul)) != 0 || h->failed) {
		if (ul->config->client_tx) (void)tx_quarantine(h,ul);
		rc = mdm_rx_quarantine(h, rx, rc);
		goto unavailable;
	}
	rx->start_acks = 1;
	if ((rc = channel_start(h, dl)) != 0 || h->failed) {
		if (ul->config->client_tx) (void)tx_quarantine(h,ul);
		rc = mdm_rx_quarantine(h, rx, rc);
		goto unavailable;
	}
	rx->start_acks |= 2;
	rx->owner = MHI_RX_IDLE;
	h->ops->unlock(h->context);
	return 0; /* Keep the wake vote until a confirmed retirement. */
unavailable:
	h->ops->unlock(h->context);
	mhi_host_wake_put(h);
	return rc;
}

/* Returns with the host mutex held on success; no pointer or state escapes
 * a failed generation check. External RX polling uses mhi_host_poll_event. */
static int
mdm_rx_acquire(struct mhi_host *h, uint64_t epoch, struct mhi_host_channel **out)
{
	if (!h || !h->allocated || !epoch)
		return EINVAL;
	h->ops->lock(h->context);
	struct mhi_host_channel *ch = channel_numbered(h, MDM_IP_HW0_DL);
	int rc = ESTALE;
	if (ch && ch->config->client_rx && ch->rx.epoch == epoch &&
	    ch->rx.owner != MHI_RX_NEW && ch->rx.owner != MHI_RX_RETIRED) {
		if (!h->powered || h->failed || !mhi_host_ownership_clean(h) || ch->rx.owner == MHI_RX_QUARANTINED)
			rc = mdm_rx_quarantine(h, &ch->rx, EIO);
		else if (ch->rx.closing || !ch->running)
			rc = EBUSY;
		else {
			*out = ch;
			return 0;
		}
	}
	h->ops->unlock(h->context);
	return rc;
}

int
mdm_mhi_rx_post(struct mhi_host *h, uint64_t epoch)
{
	struct mhi_host_channel *ch;
	int rc = mdm_rx_acquire(h, epoch, &ch);
	if (rc) return rc;
	struct mhi_rx_credit *rx = &ch->rx;
	if (rx->owner != MHI_RX_IDLE || ch->ring.rp != ch->ring.wp)
		rc = EBUSY;
	else if (rx->issued >= ch->ring.elements)
		rc = EOVERFLOW;
	else if (device_ee(h) != MHI_HOST_EE_AMSS || device_state(h) != MHI_HOST_STATE_M0)
		rc = mdm_rx_quarantine(h, rx, EIO);
	else {
		rx->received = 0;
		rx->completion_code = 0;
		rx->slot = ch->ring.wp;
		rx->issued++;
		rx->pending = true;
		rx->owner = MHI_RX_POSTED;
		write_element(element(&ch->ring, rx->slot), rx->iova, rx->capacity,
		    MHI_TYPE_TRANSFER << 16 | MDM_RX_BEI | MHI_TRE_IEOT);
		ch->ring.wp = next(&ch->ring, rx->slot);
		publish_transfer_wp(h, ch);
		ring_channel(h, ch);
	}
	h->ops->unlock(h->context);
	return rc;
}

int
mdm_mhi_rx_copy(struct mhi_host *h, uint64_t epoch, void *dst, size_t limit, size_t *bytes, unsigned *code)
{
	if (!bytes || !code || (!dst && limit))
		return EINVAL;
	*bytes = 0;
	*code = 0;
	struct mhi_host_channel *ch;
	int rc = mdm_rx_acquire(h, epoch, &ch);
	if (rc) return rc;
	struct mhi_rx_credit *rx = &ch->rx;
	if (rx->owner != MHI_RX_COMPLETE)
		rc = EAGAIN;
	else if (rx->received > limit) {
		*bytes = rx->received;
		*code = rx->completion_code;
		rc = EMSGSIZE; /* No truncation; caller may retry. */
	} else {
		device_fence();
		if (rx->received)
			memcpy(dst, rx->buffer, rx->received);
		*bytes = rx->received;
		*code = rx->completion_code;
		memset(rx->buffer, 0, rx->capacity);
		rx->received = 0;
		rx->owner = MHI_RX_IDLE;
	}
	h->ops->unlock(h->context);
	return rc;
}

/* Silence is not a completion. When a posted credit has seen no data, only
 * acknowledged RESET plus retiring its pending ER8 entries returns it to CPU
 * ownership. The pool allocation itself remains pinned for the controller. */
int
mdm_mhi_rx_close(struct mhi_host *h, uint64_t epoch)
{
	struct mhi_host_channel *dl;
	int rc = mdm_rx_acquire(h, epoch, &dl);
	if (rc) {
#if defined(KERNEL) || defined(HK_CELL_MHI_TEST)
		hk_cell_mhi_revoke(h, epoch);
#endif
		return rc;
	}
	struct mhi_rx_credit *rx = &dl->rx;
	struct mhi_host_channel *ul = channel_numbered(h, MDM_IP_HW0_UL);
	rx->closing = true;
	if (ul->config->client_tx) {
		ul->tx.closing = true;
		ul->tx.producer = NULL;
	}
#if defined(KERNEL) || defined(HK_CELL_MHI_TEST)
	/* First prevent a new binding from qualifying. Then drain any existing
	 * stack users outside the MHI mutex, before issuing either RESET. */
	h->ops->unlock(h->context);
	hk_cell_mhi_revoke(h, epoch);
	h->ops->lock(h->context);
	if (h->failed || !mhi_host_ownership_clean(h) || !h->powered)
		goto uncertain;
#endif
	if ((rc = command(h, dl, MHI_TYPE_RESET_CHAN)) != 0 || h->failed)
		goto uncertain;
	rx->reset_acks = 2;
	unsigned retired = 0;
	rc = service_ring(h, MDM_IP_HW0_DL_ER, 32, &retired);
	struct mhi_host_ring *er = &h->events[MDM_IP_HW0_DL_ER];
	device_fence();
	if (rc || h->failed || retired == 32 || h->event_busy[MDM_IP_HW0_DL_ER] ||
	    get64(event_context(h, MDM_IP_HW0_DL_ER) + CTX_RP) != element_address(er, er->rp))
		goto uncertain;
	dl->running = false;
	if ((rc = command(h, ul, MHI_TYPE_RESET_CHAN)) != 0 || h->failed)
		goto uncertain;
	rx->reset_acks |= 1;
	if (ul->config->client_tx) {
		struct mhi_host_ring *tx_er = &h->events[MDM_IP_HW0_UL_ER];
		unsigned consumed = 0;
		rc = service_ring(h,MDM_IP_HW0_UL_ER,32,&consumed);
		device_fence();
		if (rc || h->failed || consumed == 32 ||
		    get64(event_context(h,MDM_IP_HW0_UL_ER)+CTX_RP) != element_address(tx_er,tx_er->rp))
			goto uncertain;
	}
	ul->running = false;
	device_fence();
	/* Any new post-RESET write is an ownership violation; never recycle it. */
	if (get64(event_context(h, MDM_IP_HW0_DL_ER) + CTX_RP) != element_address(er, er->rp))
		goto uncertain;
	if (ul->config->client_tx) {
		struct mhi_tx_pool *tx = &ul->tx;
		tx->cancelled += tx->pending; /* returned by RESET, without an observed EOT */
		tx->pending = 0;
		ul->ring.rp = ul->ring.wp;
		memset(tx->entry,0,sizeof(tx->entry));
		memset(tx->base,0,MHI_TX_CAPACITY * MHI_TX_BUFFERS);
		tx->retired = true;
	}
	dl->ring.rp = dl->ring.wp;
	memset(rx->buffer, 0, rx->capacity);
	rx->received = 0;
	rx->pending = false;
	rx->drained = true;
	rx->owner = MHI_RX_RETIRED;
	h->ops->unlock(h->context);
	mhi_host_wake_put(h);
	return 0;
uncertain:
	if (ul->config->client_tx) (void)tx_quarantine(h,ul);
	rc = mdm_rx_quarantine(h, rx, rc);
	h->ops->unlock(h->context);
	return rc;
}

int
mdm_mhi_rx_poll(struct mhi_host *h, uint64_t epoch, unsigned budget, unsigned *events, int *input_error)
{
	if (!events || !budget)
		return EINVAL;
	*events = 0;
	if (input_error)
		*input_error = EAGAIN;
	struct mhi_host_channel *dl;
	int rc = mdm_rx_acquire(h, epoch, &dl);
	if (rc)
		return rc;
	h->ops->unlock(h->context);
	rc = mhi_host_poll_event(h, MDM_IP_HW0_DL_ER, budget, events);
	if (rc)
		return rc;
#if defined(KERNEL) || defined(HK_CELL_MHI_TEST)
	rc = mdm_rx_acquire(h, epoch, &dl);
	if (rc)
		return rc;
	bool complete = dl->rx.owner == MHI_RX_COMPLETE;
	h->ops->unlock(h->context);
	if (complete) {
		int input = hk_cell_mhi_ingress(h, epoch);
		if (input_error)
			*input_error = input;
	}
#endif
	return 0;
}

/* The standalone diagnostic has no WDS bearer or payload-capable UL.
 * Its shared poll worker only hands data to an independently bound ifnet. */
int
mdm_mhi_rx_trial(struct mhi_host *h, uint64_t epoch, unsigned window_ms, struct mdm_rx_receipt *r)
{
	if (!r || !h || !h->allocated || window_ms == 0 || window_ms > 2000)
		return EINVAL;
	*r = (struct mdm_rx_receipt){ .epoch = epoch };
	struct mhi_host_channel *dl = channel_numbered(h, MDM_IP_HW0_DL);
	struct mhi_host_channel *ul = channel_numbered(h, MDM_IP_HW0_UL);
	if (!ul || !dl || !dl->config->client_rx || !ul->config->command_only)
		return EINVAL;
	struct mhi_rx_credit *rx = &dl->rx;
	unsigned in_window = 0;
	int rc = mdm_mhi_rx_begin(h, epoch);
	if (rc) goto summary;
	rc = mdm_mhi_rx_post(h, epoch);
	if (!rc) {
		uint64_t deadline = now_ms(h) + window_ms;
		for (;;) {
			unsigned done;
			rc = mdm_mhi_rx_poll(h, epoch, 32, &done, NULL);
			h->ops->lock(h->context);
			in_window = rx->completions;
			h->ops->unlock(h->context);
			if (rc || in_window) break;
			if (now_ms(h) >= deadline) {
				r->wait_expired = 1;
				break;
			}
			h->ops->pause(h->context, 1000);
		}
	}
	h->ops->lock(h->context);
	r->pending_before_reset = rx->pending;
	h->ops->unlock(h->context);
	int close_rc = mdm_mhi_rx_close(h, epoch);
	if (close_rc) rc = close_rc;
summary:
	h->ops->lock(h->context);
	if (rx->epoch == epoch) {
		r->posted = rx->issued;
		r->slot = rx->slot;
		r->capacity = rx->capacity;
		r->completed = rx->completions;
		r->late = rx->completions - in_window;
		r->completion_code = rx->completion_code;
		r->length = rx->last_length;
		r->reset_ack = rx->reset_acks;
		r->start_ack = rx->start_acks;
		r->er8_drained = rx->drained;
		r->pending = rx->pending;
		r->retired = rx->owner == MHI_RX_RETIRED;
	}
	r->quarantined = !mhi_host_ownership_clean(h);
	r->ul_wp = ul->ring.wp;
	struct mhi_host_channel *cu = channel_numbered(h, 20), *cd = channel_numbered(h, 21);
	r->ipcr_running = (cu && cu->running ? 1u : 0u) | (cd && cd->running ? 2u : 0u);
	r->ipcr_ul_state = channel_context(h, 20)[CTX_CFG];
	r->ipcr_dl_state = channel_context(h, 21)[CTX_CFG];
	r->ee = device_ee(h);
	r->state = device_state(h);
	h->ops->unlock(h->context);
	return rc;
}

/* As mhi_host_send(), with `more': a piece of a transfer, its element chained to the next (Linux's MHI_CHAIN), and no end of transfer until the last. */
int
mdm_mhi_send(struct mhi_host *h, unsigned number, const void *data, size_t length, bool more)
{
	struct mhi_host_channel *ch;
	uint64_t until;
	uint32_t at;
	int error;

	h->ops->lock(h->context);
	ch = channel_numbered(h, number);
	if (ch == NULL || ch->config->inbound || ch->config->command_only || ch->config->client_tx || !ch->running || h->failed) {
		h->ops->unlock(h->context);
		return ENOTCONN;
	}
	if (length == 0 || length > ch->buffer_length) {
		h->ops->unlock(h->context);
		return EMSGSIZE;
	}
	for (until = now_ms(h) + h->config.timeout_ms; next(&ch->ring, ch->ring.wp) == ch->ring.rp;) {
		service_all(h);
		if (now_ms(h) > until || !ch->running) {
			h->ops->unlock(h->context);
			return ETIMEDOUT;
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
	at = ch->ring.wp;
	memcpy(ch->buffers + (size_t)at * ch->buffer_length, data, length);
	write_element(element(&ch->ring, at), ch->buffers_address + (uint64_t)at * ch->buffer_length, (uint32_t)length,
	    (more ? 1u /* chain */ | MHI_TRE_IEOB : MHI_TRE_IEOT) | MHI_TYPE_TRANSFER << 16);	/* (an event for each piece: a transfer longer than the ring has none of its own to free it, run 071137) */
	ch->ring.wp = next(&ch->ring, at);
	publish_transfer_wp(h, ch);
	h->ops->unlock(h->context);
	if ((error = mhi_host_wake_get(h)) == 0) {
		h->ops->lock(h->context);
		ring_channel(h, ch);
		h->ops->unlock(h->context);
	}
	mhi_host_wake_put(h);
	return error;
}

#if defined(KERNEL) || defined(HK_CELL_MHI_TEST)
/* Keep this adapter in the same NF translation unit as the board RX owner;
 * its short, lock-held transfer copy is private to this implementation. */
#include "hk_cell_rawip.inc"
#endif

