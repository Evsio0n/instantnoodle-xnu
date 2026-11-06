/*
 * The SDX55's MHI host: wifi/lx/src/mhi_host.c under other names (the Wi-Fi's copy is in the driver's object), with a boot of its own for a flashless modem. See modem_boot.inc.
 */
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <stdbool.h>

#define _KERNEL 1
#include "modem_mhi.h"
#if __has_include("mhi_host.c")
#include "mhi_host.c"
#else
#include "../../../../wifi/lx/src/mhi_host.c"
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
	setup_mmio(h);
	for (unsigned e = 0; e < h->config.event_count; e++) {
		doorbell(h, h->event_doorbells + 8 * e, element_address(&h->events[e], h->events[e].wp));
	}
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
	for (unsigned e = 0; e < h->config.event_count; e++) {
		doorbell(h, h->event_doorbells + 8 * e, element_address(&h->events[e], h->events[e].wp));
	}
	for (unsigned c = 0; c < h->config.channel_count && error == 0; c++) {
		if (h->channels[c].config->ee_mask & 1u << MHI_HOST_EE_AMSS) {
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
	if (ch == NULL || ch->config->inbound || !ch->running || h->failed) {
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
	put64(channel_context(h, number) + CTX_WP, element_address(&ch->ring, ch->ring.wp));
	h->ops->unlock(h->context);
	if ((error = mhi_host_wake_get(h)) == 0) {
		h->ops->lock(h->context);
		doorbell(h, h->channel_doorbells + 8 * number, element_address(&ch->ring, ch->ring.wp));
		h->ops->unlock(h->context);
	}
	mhi_host_wake_put(h);
	return error;
}

