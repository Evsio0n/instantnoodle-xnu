/*
 * The MHI host (mhi_host.h): a device's boot over BHI and BHIe, its states, its rings.
 *
 * The device is told where four kinds of things are in the host's memory, which it reads and writes
 * by itself: a context per channel, per event ring and for the command ring (44 bytes each: a
 * ring's base, length, read and write pointers, and what kind of ring it is), and the rings, of
 * 16-byte elements. On a transfer or command ring the host writes elements and moves the write
 * pointer, the device reads and moves the read pointer; on an event ring it is the other way
 * round. After moving a pointer the side that moved it rings the other's doorbell (a register of
 * the window, 64 bits wide, the new pointer in it); the device's doorbell for the host is an MSI.
 *
 * A boot from the primary boot loader (EE PBL), as Linux's host does it with "fbc_download":
 *   1. BHI: the first sbl_length bytes of the image are pointed to, and a transaction number
 *      written; BHI_STATUS says when they are taken;
 *   2. the secondary boot loader runs, and says READY: the host writes where its contexts are
 *      (the MMIO setup), gives the event rings their elements and asks for M0;
 *   3. in EE SBL, BHIe: a table of the whole image's pieces, and another transaction number;
 *   4. the image runs (EE AMSS, "mission mode"): the channels whose environments that is are
 *      started with a command each, and the inbound ones given buffers.
 * Waiting is done by looking -- the registers, the event rings -- every 200 us, whether or not
 * interrupts come; with them, a waiter is done sooner.
 *
 * The device may go to M1 by itself when it has nothing to do; the host then sends it to M2
 * unless somebody wants it awake (mhi_host_wake_get(): the wake doorbell held). Doorbells other
 * than that one are rung only in M0 (the QCA6390: "mhi,m2-no-db-access" in Android's tree).
 */
#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#else
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#endif

#include "mhi_host.h"

#define PAUSE_US	200
#define CHANNEL_STATE_ENABLED	1
#define BURST_DISABLED		2u
#define BURST_ENABLED		3u
#define EVENT_RING_VALID	1

static void
say(struct mhi_host *h, const char *format, ...)
{
	char line[240];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	h->ops->say(h->context, line);
}

static uint32_t
rd(struct mhi_host *h, uint32_t offset)
{
	return (h->ops->read(h->context, offset));
}

static void
wr(struct mhi_host *h, uint32_t offset, uint32_t value)
{
	h->ops->write(h->context, offset, value);
}

/* a 64-bit doorbell: high word first, the low word rings it */
/*
 * Order against the device: it is no observer of this processor's inner domain, so a "dmb ish"
 * (a C fence) does not order the rings' memory against a doorbell, nor the device's writes
 * against our reads of them (Linux: wmb() and rmb(), dsb).
 */
static inline void
device_fence(void)
{
#if defined(__aarch64__)
	__asm__ volatile ("dsb sy" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static void
doorbell(struct mhi_host *h, uint32_t offset, uint64_t value)
{
	device_fence();				/* (what the pointer covers is in memory first) */
	wr(h, offset + 4, (uint32_t)(value >> 32));
	wr(h, offset, (uint32_t)value);
}

static uint64_t
now_ms(struct mhi_host *h)
{
	return (h->ops->nanoseconds(h->context) / 1000000u);
}

/* ---- contexts: little-endian words at fixed offsets ------------------------------------------------ */

static void
put32(uint8_t *at, uint32_t value)
{
	memcpy(at, &value, 4);
}

static void
put64(uint8_t *at, uint64_t value)
{
	memcpy(at, &value, 8);
}

/* (what the device writes: two loads of 32 bits, low first; its pointers stay below 4 GB here) */
static uint64_t
get64(const uint8_t *at)
{
	uint32_t low = *(const volatile uint32_t *)at, high = *(const volatile uint32_t *)(at + 4);

	return ((uint64_t)high << 32 | low);
}

#define CTX_CFG		0
#define CTX_TYPE	4
#define CTX_INDEX	8
#define CTX_RBASE	12
#define CTX_RLEN	20
#define CTX_RP		28
#define CTX_WP		36

static uint8_t *
channel_context(struct mhi_host *h, uint32_t number)
{
	return (h->channel_contexts + (size_t)number * MHI_CONTEXT);
}

static uint8_t *
event_context(struct mhi_host *h, unsigned ring)
{
	return (h->event_contexts + (size_t)ring * MHI_CONTEXT);
}

/* ---- rings ---------------------------------------------------------------------------------------- */

static uint8_t *
element(struct mhi_host_ring *r, uint32_t index)
{
	return (r->base + (size_t)index * MHI_ELEMENT);
}

static uint64_t
element_address(struct mhi_host_ring *r, uint32_t index)
{
	return (r->address + (uint64_t)index * MHI_ELEMENT);
}

static uint32_t
next(struct mhi_host_ring *r, uint32_t index)
{
	return (index + 1 == r->elements ? 0 : index + 1);
}

/* the element the device's pointer names; ~0u if it names none of the ring's */
static uint32_t
index_of(struct mhi_host_ring *r, uint64_t address)
{
	if (address < r->address || address >= r->address + (uint64_t)r->elements * MHI_ELEMENT ||
	    (address - r->address) % MHI_ELEMENT != 0) {
		return (~0u);
	}
	return ((uint32_t)((address - r->address) / MHI_ELEMENT));
}

/* The endpoint can complete only descriptors posted in [rp, wp), including
 * across the end of the ring. wp always names the first unused element. */
static bool
outstanding(struct mhi_host_ring *r, uint32_t index)
{
	if (index >= r->elements) {
		return false;
	}
	if (r->rp <= r->wp) {
		return index >= r->rp && index < r->wp;
	}
	return index >= r->rp || index < r->wp;
}

static void
write_element(uint8_t *e, uint64_t pointer, uint32_t word0, uint32_t word1)
{
	put64(e, pointer);
	put32(e + 8, word0);
	put32(e + 12, word1);
}

static bool
ring_alloc(struct mhi_host *h, struct mhi_host_ring *r, uint32_t elements, bool uncached)
{
	size_t size = (size_t)elements * MHI_ELEMENT;

	/* a ring is aligned to its size, rounded up to a power of two (the device's address arithmetic) */
	size_t align = MHI_ELEMENT;

	while (align < size) {
		align <<= 1;
	}
	r->base = (uncached ? h->ops->dma_alloc_uncached : h->ops->dma_alloc)(h->context, size, align, &r->address);
	r->elements = elements;
	r->rp = r->wp = 0;
	return (r->base != NULL);
}

/* ---- memory, once --------------------------------------------------------------------------------- */

int
mhi_host_init(struct mhi_host *h, const struct mhi_host_ops *ops, void *context, const struct mhi_host_config *config)
{
	size_t vectors;
	bool control = false;

	if (h->allocated) {
		return (0);
	}
	memset(h, 0, sizeof(*h));
	h->ops = ops;
	h->context = context;
	h->config = *config;
	if (config->channel_count > MHI_HOST_CHANNELS || config->event_count == 0 || config->event_count > MHI_HOST_EVENT_RINGS ||
	    config->hardware_event_count > config->event_count || config->event_count > 255 ||
	    config->image == NULL || config->image_length == 0 || config->segment_length == 0 || config->max_channels == 0) {
		say(h, "mhi: the controller's description is not one this host takes");
		return (EINVAL);
	}
	for (unsigned e = 0; e < config->event_count; e++) {
		const struct mhi_host_event_config *ev = &config->events[e];
		if (ev->elements == 0) {
			continue;
		}
		if (ev->elements < 2 || ev->moderation_ms > 0xffffu || (ev->control &&
		    (ev->client_managed || e >= config->event_count - config->hardware_event_count))) {
			return EINVAL;
		}
		if (ev->uncached && ops->dma_alloc_uncached == NULL) {
			return ENOTSUP;
		}
		control |= ev->control;
	}
	if (!control) {
		return EINVAL;
	}
	if (config->hardware_event_count && config->register_window_size < MHI_REG_SOC_RESET + 4) {
		return EINVAL;
	}
	for (unsigned c = 0; c < config->channel_count; c++) {
		const struct mhi_host_channel_config *cc = &config->channels[c];
		if (cc->client_tx && (cc->inbound || cc->command_only || cc->client_rx ||
		    cc->elements <= MHI_TX_BUFFERS || cc->elements % MHI_TX_BUFFERS ||
		    cc->event_ring >= config->event_count || config->events[cc->event_ring].client_managed ||
		    config->events[cc->event_ring].control || !ops->dma_alloc_uncached))
			return EINVAL;
		if (config->channels[c].number >= config->max_channels || config->channels[c].event_ring >= config->event_count ||
		    config->events[config->channels[c].event_ring].elements == 0 || config->channels[c].elements < 2) {
			say(h, "mhi: channel %u is not one this host takes", config->channels[c].number);
			return (EINVAL);
		}
		const struct mhi_host_channel_config *ch = &config->channels[c];
		if (ch->client_rx && (!ch->inbound || ch->command_only ||
		    !config->events[ch->event_ring].client_managed))
			return EINVAL;
	}
	h->channel_contexts = ops->dma_alloc(context, (size_t)config->max_channels * MHI_CONTEXT, 64, &h->channel_contexts_address);
	h->event_contexts = ops->dma_alloc(context, (size_t)config->event_count * MHI_CONTEXT, 64, &h->event_contexts_address);
	h->command_context = ops->dma_alloc(context, MHI_CONTEXT, 64, &h->command_context_address);
	if (h->channel_contexts == NULL || h->event_contexts == NULL || h->command_context == NULL ||
	    !ring_alloc(h, &h->commands, 32, false)) {
		return (ENOMEM);
	}
	for (unsigned e = 0; e < config->event_count; e++) {
		if (config->events[e].elements != 0 &&
		    !ring_alloc(h, &h->events[e], config->events[e].elements, config->events[e].uncached)) {
			return (ENOMEM);
		}
	}
	for (unsigned c = 0; c < config->channel_count; c++) {
		struct mhi_host_channel *ch = &h->channels[c];

		ch->config = &h->config.channels[c];
		ch->transfer_db = MHI_HOST_DB_ALWAYS;
		ch->buffer_length = config->buffer_length;
		if (!ring_alloc(h, &ch->ring, ch->config->elements, false)) {
			return (ENOMEM);
		}
		if (ch->config->command_only || ch->config->client_rx || ch->config->client_tx) {
			continue;
		}
		ch->buffers = ops->dma_alloc(context, (size_t)ch->buffer_length * ch->config->elements, 64, &ch->buffers_address);
		if (ch->buffers == NULL) {
			return (ENOMEM);
		}
	}
	/* the image where the device reads it, and the table of its pieces: BHIe takes the whole image */
	h->image_size = config->image_length;
	h->image = ops->dma_alloc(context, h->image_size, 4096, &h->image_address);
	vectors = (config->image_length + config->segment_length - 1) / config->segment_length;
	h->vectors_size = vectors * 16;
	h->vectors = ops->dma_alloc(context, h->vectors_size, 4096, &h->vectors_address);
	if (h->image == NULL || h->vectors == NULL) {
		return (ENOMEM);
	}
	memcpy(h->image, config->image, config->image_length);
	for (size_t v = 0; v < vectors; v++) {
		size_t at = v * config->segment_length, length = config->image_length - at;

		put64(h->vectors + 16 * v, h->image_address + at);
		put64(h->vectors + 16 * v + 8, length < config->segment_length ? length : config->segment_length);
	}
	h->allocated = true;
	say(h, "mhi: %u channel and %u event ring(s) set up; the image (%zu bytes, %zu pieces) is where the device reads it",
	    config->channel_count, config->event_count, config->image_length, vectors);
	return (0);
}

/* ---- the device's state -------------------------------------------------------------------------- */

static uint32_t
device_state(struct mhi_host *h)
{
	return (rd(h, MHI_REG_MHISTATUS) >> 8 & 0xff);
}

static uint32_t
device_ee(struct mhi_host *h)
{
	return (h->bhi != 0 ? rd(h, h->bhi + MHI_BHI_EXECENV) : ~0u);
}

static void
set_state(struct mhi_host *h, uint32_t state)
{
	uint32_t control = rd(h, MHI_REG_MHICTRL);

	wr(h, MHI_REG_MHICTRL, (control & ~(0xffu << 8)) | state << 8);
}

static bool
in_mission(uint32_t ee)
{
	return (ee == MHI_HOST_EE_AMSS || ee == MHI_HOST_EE_WFW || ee == MHI_HOST_EE_FP);
}

/* A burst-mode endpoint may already be polling the context. Its WP must never
 * expose an unwritten TRE, including when this submission needs no MMIO write. */
static void
publish_transfer_wp(struct mhi_host *h, struct mhi_host_channel *ch)
{
	device_fence();
	put64(channel_context(h, ch->config->number) + CTX_WP, element_address(&ch->ring, ch->ring.wp));
}

static void
ring_channel(struct mhi_host *h, struct mhi_host_channel *ch)
{
	if (!h->powered || h->failed || !ch->running || (ch->config->client_rx && ch->rx.closing) ||
	    (ch->config->client_tx && (ch->tx.closing || ch->tx.quarantined || !ch->tx.pending)) || h->state != MHI_HOST_STATE_M0 ||
	    ch->ring.wp == ch->ring.rp) {
		return;
	}
	if (ch->transfer_db != MHI_HOST_DB_ALWAYS) {
		if (!ch->db_armed) {
			return;
		}
		ch->db_armed = false;
	}
	doorbell(h, h->channel_doorbells + 8 * ch->config->number, element_address(&ch->ring, ch->ring.wp));
	if (ch->config->client_tx) ch->tx.doorbells++;
}

static void
ring_event(struct mhi_host *h, unsigned e)
{
	if (h->powered && !h->failed && (!h->mission || h->state == MHI_HOST_STATE_M0) &&
	    h->event_active[e] && (!h->config.events[e].burst || !h->event_db_sent[e])) {
		h->event_db_sent[e] = true;
		doorbell(h, h->event_doorbells + 8 * e, element_address(&h->events[e], h->events[e].wp));
	}
}

static void
ring_active_events(struct mhi_host *h)
{
	for (unsigned e = 0; e < h->config.event_count; e++) {
		ring_event(h, e);
	}
}

/* READY gives credits only to software event rings. The mission/M0 transition
 * grants the hardware rings their first credits. A second call never rewinds a live ring. */
static int
activate_event_rings(struct mhi_host *h, bool hardware)
{
	unsigned first_hw = h->config.event_count - h->config.hardware_event_count;
	for (unsigned e = hardware ? first_hw : 0; e < (hardware ? h->config.event_count : first_hw); e++) {
		struct mhi_host_ring *r = &h->events[e];
		if (r->base == NULL || h->event_active[e]) {
			continue;
		}
		if (hardware && (!h->powered || h->failed || !h->mission_told || h->state != MHI_HOST_STATE_M0)) {
			return ENOTCONN;
		}
		r->wp = r->elements - 1;
		put64(event_context(h, e) + CTX_WP, element_address(r, r->wp));
		h->event_active[e] = true;
		ring_event(h, e);
	}
	return 0;
}

static void
ring_doorbells(struct mhi_host *h)
{
	ring_active_events(h);
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		struct mhi_host_channel *ch = &h->channels[c];

		if (ch->transfer_db == MHI_HOST_DB_BURST_SWITCH) {
			ch->db_armed = true;
		}
		ring_channel(h, ch);
	}
}

/* ---- events ---------------------------------------------------------------------------------------- */

static struct mhi_host_channel *
channel_numbered(struct mhi_host *h, uint32_t number)
{
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		if (h->channels[c].config->number == number) {
			return (&h->channels[c]);
		}
	}
	return (NULL);
}

static int
tx_quarantine(struct mhi_host *h, struct mhi_host_channel *ch)
{
	ch->tx.quarantined = true;
	ch->tx.producer = NULL;
	h->failed = true;
	__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
	return EIO;
}

/* An MHI TX event names the last TRE of an ordered completion. Validate the
 * entire batch before returning any payload to the producer. Wire events
 * have no per-lap sequence; the endpoint must follow normal MHI event order. */
static int
complete_client_tx(struct mhi_host *h, struct mhi_host_channel *ch, uint64_t ptr, uint32_t length, uint32_t code)
{
	struct mhi_host_ring *r = &ch->ring;
	struct mhi_tx_pool *tx = &ch->tx;
	uint32_t terminal = index_of(r, ptr);
	if (!ch->running || !tx->base || !tx->epoch || tx->retired || tx->quarantined ||
	    code != MHI_CODE_EOT || !outstanding(r,terminal))
		return tx_quarantine(h,ch);
	unsigned span = (terminal + r->elements - r->rp) % r->elements + 1;
	if (span > tx->pending || tx->pending > MHI_TX_BUFFERS)
		return tx_quarantine(h,ch);
	uint32_t slot = r->rp;
	for (unsigned i = 0; i < span; i++, slot = next(r,slot)) {
		unsigned index = slot % MHI_TX_BUFFERS;
		uint8_t *tre = element(r,slot);
		uint32_t bytes, flags;
		memcpy(&bytes,tre+8,sizeof(bytes));
		memcpy(&flags,tre+12,sizeof(flags));
		if (!tx->entry[index].live || tx->entry[index].slot != slot || !bytes ||
		    bytes > MHI_TX_CAPACITY || bytes != tx->entry[index].length ||
		    get64(tre) != tx->iova + (uint64_t)index * MHI_TX_CAPACITY ||
		    flags != (MHI_TYPE_TRANSFER << 16 | MHI_TRE_IEOT | 1u << 10) ||
		    (slot == terminal && length > bytes))
			return tx_quarantine(h,ch);
	}
	for (unsigned i = 0; i < span; i++) {
		unsigned index = r->rp % MHI_TX_BUFFERS;
		tx->entry[index].live = false;
		r->rp = next(r,r->rp);
	}
	tx->pending -= span;
	tx->completed += span;
	return 0;
}

static void
state_changed(struct mhi_host *h, uint32_t state)
{
	bool changed = h->state != state;

	h->state = state;
	switch (state) {
	case MHI_HOST_STATE_M0:
		if (h->mission && changed) {
			ring_doorbells(h);		/* (what was put on a ring meanwhile) */
		}
		break;
	case MHI_HOST_STATE_M1:
		/* the device is idle: to M2, unless somebody holds it awake (then it comes back to M0 by itself) */
		if (h->wakes == 0) {
			set_state(h, MHI_HOST_STATE_M2);
			h->state = MHI_HOST_STATE_M2;
		}
		break;
	case MHI_HOST_STATE_SYS_ERR:
		h->failed = true;
		__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
		break;
	default:
		break;
	}
}

/*
 * One event ring: everything the device has put there since the last look, handled, and the
 * elements given back. Datagrams are handed on without the lock (an ops->receive() that is under
 * way leaves the ring to its caller, who looks again).
 */
static int
service_ring(struct mhi_host *h, unsigned e, unsigned budget, unsigned *done)
{
	struct mhi_host_ring *r = &h->events[e];
	uint8_t *ctx = event_context(h, e);
	bool told_mission = false, told_error = false, told_rddm = false;
	unsigned consumed = 0;
	int error = 0;

	if (!h->event_active[e]) {
		return ENOTCONN;
	}
	if (h->event_busy[e]) {
		h->event_again[e] = true;
		return EBUSY;
	}
	h->event_busy[e] = true;
	do {
		uint32_t until;

		h->event_again[e] = false;
		until = index_of(r, get64(ctx + CTX_RP));
		if (until == ~0u) {
			if (e >= h->config.event_count - h->config.hardware_event_count) {
				say(h, "mhi: hardware event ring %u: RP outside owned ring", e);
				h->failed = true;
				__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
			}
			error = EIO;
			break;
		}
		if (r->rp == until) break;
		device_fence();				/* (the elements are read after the pointer to them) */
		while (r->rp != until && consumed < budget) {
			uint8_t *ev = element(r, r->rp);
			uint64_t pointer;
			uint32_t word0, word1, type, code, length, number;
			struct mhi_host_channel *ch;

			memcpy(&pointer, ev, 8);
			memcpy(&word0, ev + 8, 4);
			memcpy(&word1, ev + 12, 4);
			type = word1 >> 16 & 0xff;
			code = word0 >> 24;
			length = word0 & 0xffff;
			number = word1 >> 24;
			h->serviced++;
			switch (type) {
			case MHI_TYPE_STATE_CHANGE:
				if (!h->config.events[e].control) break;
				state_changed(h, code);
				told_error |= code == MHI_HOST_STATE_SYS_ERR;
				break;
			case MHI_TYPE_EE:
				if (!h->config.events[e].control) break;
				h->ee = code;
				if (in_mission(code) && !h->mission) {
					h->mission = told_mission = true;
				}
				h->mission_told |= in_mission(code);
				told_rddm |= code == MHI_HOST_EE_RDDM;
				if (code == MHI_HOST_EE_RDDM) {
					h->failed = true;
					__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
				}
				break;
			case MHI_TYPE_CMD_DONE: {
				uint32_t at = index_of(&h->commands, pointer), command;

				if (!h->config.events[e].control || h->commands.rp == h->commands.wp || at != h->commands.rp) {
					say(h, "mhi: a command's completion for no command of ours (%#llx)", (unsigned long long)pointer);
					break;
				}
				memcpy(&command, element(&h->commands, at) + 12, 4);
				if ((ch = channel_numbered(h, command >> 24)) != NULL && ch->command_in_flight && ch->command_slot == at) {
					ch->command_status = code;
				}
				h->commands.rp = next(&h->commands, at);
				break;
			}
			case MHI_TYPE_TX: {
				uint32_t at;

				if ((ch = channel_numbered(h, number)) == NULL) {
					say(h, "mhi: transfer event on unowned channel %u", number);
					break;
				}
				if (ch->config->event_ring != e) {
					if (ch->config->client_tx) error = tx_quarantine(h,ch);
					say(h, "mhi: channel %u: transfer event on unexpected ring %u", number, e);
					break;
				}
				if (ch->config->client_rx && code != MHI_CODE_DB_MODE && code != MHI_CODE_OOB) {
					struct mhi_rx_credit *rx = &ch->rx;
					uint8_t *tre = element(&ch->ring, rx->slot);
					uint32_t posted_length, posted_flags;
					memcpy(&posted_length, tre + 8, sizeof(posted_length));
					memcpy(&posted_flags, tre + 12, sizeof(posted_flags));
					if (!ch->running || rx->owner != MHI_RX_POSTED || ch->ring.rp != rx->slot ||
					    ch->ring.wp != next(&ch->ring, rx->slot) ||
					    pointer != element_address(&ch->ring, rx->slot) ||
					    get64(tre) != rx->iova || posted_length != rx->capacity ||
					    posted_flags != (MHI_TYPE_TRANSFER << 16 | 1u << 10 | MHI_TRE_IEOT) ||
					    length > rx->capacity ||
					    (code != MHI_CODE_EOT && code != MHI_CODE_EOB && code != MHI_CODE_OVERFLOW)) {
						rx->owner = MHI_RX_QUARANTINED;
						h->failed = true;
						__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
						error = EIO;
						break;
					}
					ch->ring.rp = ch->ring.wp;
					rx->completion_code = code;
					rx->received = length;
					rx->last_length = length;
					rx->completions++;
					rx->pending = false;
					rx->owner = MHI_RX_COMPLETE;
					break; /* owner copies and explicitly releases; never auto-repost */
				}
				if (ch->config->command_only && (code == MHI_CODE_EOT || code == MHI_CODE_EOB || code == MHI_CODE_OVERFLOW)) {
					say(h, "mhi: channel %u: payload completion while no payloads were posted", number);
					h->failed = true;
					__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
					error = EIO;
					break;
				}
				if (ch->config->client_tx && code != MHI_CODE_DB_MODE && code != MHI_CODE_OOB) {
					error = complete_client_tx(h,ch,pointer,length,code);
					break;
				}
				if (!ch->running) break;
				if (code == MHI_CODE_DB_MODE || code == MHI_CODE_OOB) {
					ch->db_armed = true;
					ring_channel(h, ch);
					break;
				}
				if (code != MHI_CODE_EOT && code != MHI_CODE_EOB && code != MHI_CODE_OVERFLOW) {
					say(h, "mhi: channel %u: completion code %#x", number, code);
					break;
				}
				at = index_of(&ch->ring, pointer);
				if (!outstanding(&ch->ring, at)) {
					say(h, "mhi: channel %u: a completion for no posted element", number);
					break;
				}
				/* every element up to and with the one named is done; the last holds `length` bytes */
				while (ch->ring.rp != next(&ch->ring, at)) {
					uint32_t done = ch->ring.rp;
					uint32_t got = done == at ? length : ch->buffer_length;
					uint64_t where;
					uint8_t *buffer;

					ch->ring.rp = next(&ch->ring, done);
					if (!ch->config->inbound) {
						continue;
					}
					/* (which buffer: the element says; buffers move round the ring as they are given back) */
					memcpy(&where, element(&ch->ring, done), 8);
					if (where < ch->buffers_address || where - ch->buffers_address >=
					    (uint64_t)ch->buffer_length * ch->config->elements) {
						say(h, "mhi: channel %u: an element names no buffer of ours", number);
						continue;
					}
					buffer = ch->buffers + (where - ch->buffers_address);
					if ((code == MHI_CODE_OVERFLOW && done == at) || got > ch->buffer_length) {
						say(h, "mhi: channel %u: more came than a buffer holds: dropped", number);
					} else if (h->ops->receive != NULL) {
						h->ops->unlock(h->context);
						h->ops->receive(h->context, number, buffer, got);
						h->ops->lock(h->context);
					}
					/* the buffer back on the ring */
					write_element(element(&ch->ring, ch->ring.wp), where, ch->buffer_length,
					    MHI_TRE_IEOT | MHI_TYPE_TRANSFER << 16);
					ch->ring.wp = next(&ch->ring, ch->ring.wp);
				}
				if (ch->config->inbound) {
					publish_transfer_wp(h, ch);
					ring_channel(h, ch);
				}
				break;
			}
			default:
				say(h, "mhi: event ring %u: an event of type %#x", e, type);
				break;
			}
			r->rp = next(r, r->rp);
			r->wp = next(r, r->wp);
			consumed++;
			if (h->failed) break;
		}
		if (h->failed) break;
		device_fence();	/* the device may poll this WP even when no doorbell is due */
		put64(ctx + CTX_WP, element_address(r, r->wp));
		if (!h->failed && (h->state == MHI_HOST_STATE_M0 || !h->mission)) {
			ring_event(h, e);
		}
	} while (h->event_again[e] && consumed < budget);
	h->event_busy[e] = false;

	if (told_mission || told_error || told_rddm) {
		h->ops->unlock(h->context);
		if (told_mission) {
			h->ops->status(h->context, MHI_HOST_MISSION);
		}
		if (told_error) {
			h->ops->status(h->context, MHI_HOST_SYS_ERROR);
		}
		if (told_rddm) {
			h->ops->status(h->context, MHI_HOST_RDDM);
		}
		h->ops->lock(h->context);
	}
	if (done != NULL) {
		*done = consumed;
	}
	return error;
}

/*
 * The BHI vector: what the device's state and environment are now, from its registers (which
 * say what the events say, and are looked at in case an event or its interrupt went missing).
 */
static void
service_bhi(struct mhi_host *h)
{
	uint32_t ee = device_ee(h), state = device_state(h);
	bool error = false, rddm = false, told_mission = false;

	if (ee == ~0u) {
		if (h->powered && h->bhi != 0) {
			h->failed = true;
			__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
		}
		return;				/* (no BHI yet, or the link is down: everything reads all ones) */
	}
	if (ee == MHI_HOST_EE_RDDM && h->ee != MHI_HOST_EE_RDDM) {
		h->ee = ee;
		rddm = h->powered;
		if (rddm) {
			h->failed = true;
			__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
		}
	} else if (h->powered && !h->mission) {
		h->ee = ee;
		if (in_mission(ee) && h->channel_doorbells != 0) {
			h->mission = told_mission = true;
		}
	}
	if (h->powered && h->channel_doorbells != 0 && state != h->state &&
	    (state <= MHI_HOST_STATE_M3 || state == MHI_HOST_STATE_SYS_ERR)) {
		error = state == MHI_HOST_STATE_SYS_ERR;
		state_changed(h, state);
	}
	if (error || rddm || told_mission) {
		h->ops->unlock(h->context);
		h->ops->status(h->context, told_mission ? MHI_HOST_MISSION : rddm ? MHI_HOST_RDDM : MHI_HOST_SYS_ERROR);
		h->ops->lock(h->context);
	}
}

static void
service_all(struct mhi_host *h)
{
	service_bhi(h);
	if (h->channel_doorbells != 0 && h->powered && !h->failed) {
		for (unsigned e = 0; e < h->config.event_count && !h->failed; e++) {
			if (!h->config.events[e].client_managed && h->event_active[e]) {
				(void)service_ring(h, e, ~0u, NULL);
			}
		}
	}
}

void
mhi_host_interrupt(struct mhi_host *h, int vector)
{
	if (!h->allocated) {
		return;
	}
	h->ops->lock(h->context);
	if (vector < 0) {
		service_all(h);
	} else {
		if (vector == 0) service_bhi(h);
		for (unsigned e = 0; e < h->config.event_count && h->channel_doorbells != 0 && h->powered && !h->failed; e++) {
			if (h->config.events[e].vector == (uint32_t)vector && h->event_active[e] && !h->config.events[e].client_managed) {
				(void)service_ring(h, e, ~0u, NULL);
			}
		}
	}
	h->ops->unlock(h->context);
}

int
mhi_host_poll_event(struct mhi_host *h, unsigned e, unsigned budget, unsigned *done)
{
	if (done == NULL) return EINVAL;
	*done = 0;
	if (!h->allocated || e >= h->config.event_count || !h->config.events[e].client_managed || budget == 0) {
		return EINVAL;
	}
	h->ops->lock(h->context);
	int error = ENOTCONN;
	if (h->powered && !h->failed && h->channel_doorbells != 0 && h->state == MHI_HOST_STATE_M0) {
		error = service_ring(h, e, budget, done);
	}
	h->ops->unlock(h->context);
	return error;
}

/*
 * Until `done(h)` or the timeout: looking at everything every PAUSE_US, the lock given up in
 * between. Called and returns with the lock held. 0, or ETIMEDOUT.
 */
static int
wait_for(struct mhi_host *h, bool (*done)(struct mhi_host *), uint32_t ms)
{
	uint64_t until = now_ms(h) + ms;

	for (;;) {
		service_all(h);
		if (done(h)) {
			return (0);
		}
		if (now_ms(h) > until) {
			return (ETIMEDOUT);
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
}

/* ---- commands ------------------------------------------------------------------------------------- */

/*
 * A command's completion waited for; the doorbells (the command ring's, the event rings') rung
 * again every 100 ms meanwhile, as one rung while the device was still on its way can be lost --
 * ringing one again with the same pointer asks nothing new.
 */
static int
command_wait(struct mhi_host *h, struct mhi_host_channel *ch)
{
	uint64_t until = now_ms(h) + h->config.timeout_ms, rung = now_ms(h);

	for (;;) {
		service_all(h);
		if (ch->command_status != 0 || h->failed) {
			return (0);
		}
		if (now_ms(h) > until) {
			return (ETIMEDOUT);
		}
		if (now_ms(h) - rung >= 100) {
			rung = now_ms(h);
			h->rung_again++;
			ring_active_events(h);
			doorbell(h, MHI_REG_CRDB, element_address(&h->commands, h->commands.wp));
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
}

static int
command(struct mhi_host *h, struct mhi_host_channel *ch, uint32_t type)
{
	uint32_t at = h->commands.wp;
	int error;

	if (ch->command_in_flight || next(&h->commands, at) == h->commands.rp) {
		return (EBUSY);
	}
	write_element(element(&h->commands, at), 0, 0, ch->config->number << 24 | type << 16);
	h->commands.wp = next(&h->commands, at);
	put64(h->command_context + CTX_WP, element_address(&h->commands, h->commands.wp));
	ch->command_status = 0;
	ch->command_in_flight = true;
	ch->command_slot = at;
	doorbell(h, MHI_REG_CRDB, element_address(&h->commands, h->commands.wp));
	error = command_wait(h, ch);
	ch->command_in_flight = false;
	if (error == 0 && ch->command_status != MHI_CODE_SUCCESS) {
		say(h, "mhi: channel %u: command %#x completed with %#x", ch->config->number, type, ch->command_status);
		error = EIO;
	}
	return (error);
}

/* ---- channels ------------------------------------------------------------------------------------- */

static int
channel_start(struct mhi_host *h, struct mhi_host_channel *ch)
{
	uint8_t *ctx = channel_context(h, ch->config->number);
	uint32_t cfg;
	int error;

	ch->ring.rp = ch->ring.wp = 0;
	ch->db_armed = true;
	memcpy(&cfg, ctx + CTX_CFG, 4);
	put32(ctx + CTX_CFG, (cfg & ~0xffu) | CHANNEL_STATE_ENABLED);
	put32(ctx + CTX_TYPE, ch->config->inbound ? 2 : 1);
	put32(ctx + CTX_INDEX, ch->config->event_ring);
	put64(ctx + CTX_RBASE, ch->ring.address);
	put64(ctx + CTX_RLEN, (uint64_t)ch->ring.elements * MHI_ELEMENT);
	put64(ctx + CTX_RP, ch->ring.address);
	put64(ctx + CTX_WP, ch->ring.address);
	if ((error = command(h, ch, MHI_TYPE_START_CHAN)) != 0) {
		say(h, "mhi: channel %u does not start (%d)", ch->config->number, error);
		return (error);
	}
	ch->running = true;
	if (ch->config->inbound && !ch->config->command_only && !ch->config->client_rx) {
		/* all elements but one get a buffer (a full ring is one whose write pointer is one short of its read pointer) */
		for (uint32_t i = 0; i + 1 < ch->ring.elements; i++) {
			write_element(element(&ch->ring, i), ch->buffers_address + (uint64_t)i * ch->buffer_length, ch->buffer_length,
			    MHI_TRE_IEOT | MHI_TYPE_TRANSFER << 16);
		}
		ch->ring.wp = ch->ring.elements - 1;
		publish_transfer_wp(h, ch);
		ring_channel(h, ch);
	}
	return (0);
}

/* ---- power ---------------------------------------------------------------------------------------- */

static bool
reset_done(struct mhi_host *h)
{
	return (!(rd(h, MHI_REG_MHICTRL) & MHI_CTRL_RESET));
}

static bool
ready(struct mhi_host *h)
{
	return (!(rd(h, MHI_REG_MHICTRL) & MHI_CTRL_RESET) && (rd(h, MHI_REG_MHISTATUS) & MHI_STATUS_READY));
}

static bool
bhi_done(struct mhi_host *h)
{
	return ((rd(h, h->bhi + MHI_BHI_STATUS) >> 30) != 0 || h->failed);
}

static bool
bhie_done(struct mhi_host *h)
{
	return ((rd(h, h->bhie + MHI_BHIE_TXVECSTATUS) >> 30) >= 2 || h->failed);
}

static bool
in_sbl(struct mhi_host *h)
{
	return (h->ee == MHI_HOST_EE_SBL || in_mission(h->ee) || h->failed);
}

static bool
mission(struct mhi_host *h)
{
	return (h->mission || h->failed);
}

static bool
awake(struct mhi_host *h)
{
	return (h->state == MHI_HOST_STATE_M0 || h->failed);
}

static bool
mission_told(struct mhi_host *h)
{
	return (h->mission_told || h->failed);
}

/* M0 as the device's register has it: h->state can still be the M0 of its boot loader's */
static bool
device_awake(struct mhi_host *h)
{
	return (device_state(h) == MHI_HOST_STATE_M0 || h->failed);
}

static uint32_t
transaction(struct mhi_host *h)
{
	uint32_t number = 0;

	while (number == 0) {
		h->ops->random(h->context, &number, sizeof(number));
		number &= 0x3fffffff;
	}
	return (number);
}

/* The device's contexts and doorbells, given the device in READY. */
static uint64_t
register64(struct mhi_host *h, uint32_t reg)
{
	uint32_t low = rd(h, reg);
	return (uint64_t)rd(h, reg + 4) << 32 | low;
}

static int
setup_mmio(struct mhi_host *h)
{
	uint32_t cfg;

	h->channel_doorbells = rd(h, MHI_REG_CHDBOFF);
	h->event_doorbells = rd(h, MHI_REG_ERDBOFF);
	if (h->config.hardware_event_count) {
		unsigned last_channel = MHI_WAKE_DOORBELL;
		for (unsigned c = 0; c < h->config.channel_count; c++) {
			if (h->channels[c].config->number > last_channel) last_channel = h->channels[c].config->number;
		}
		uint64_t ch_end = h->channel_doorbells + 8ull * (last_channel + 1ull);
		uint64_t er_end = h->event_doorbells + 8ull * h->config.event_count;
		if (h->channel_doorbells < MHI_REG_SOC_RESET + 4 || h->event_doorbells < MHI_REG_SOC_RESET + 4 ||
		    ((h->channel_doorbells | h->event_doorbells) & 7) ||
		    ch_end > h->config.register_window_size || er_end > h->config.register_window_size ||
		    (h->channel_doorbells < er_end && h->event_doorbells < ch_end)) {
			h->channel_doorbells = h->event_doorbells = 0;
			return ERANGE;
		}
	}
	/* the contexts as at the beginning: no channel enabled, the event rings full of room, the command ring empty */
	memset(h->channel_contexts, 0, (size_t)h->config.max_channels * MHI_CONTEXT);
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		uint8_t *ctx = channel_context(h, h->channels[c].config->number);

		put32(ctx + CTX_CFG, (h->channels[c].transfer_db == MHI_HOST_DB_ALWAYS ? BURST_DISABLED : BURST_ENABLED) << 8);
		put32(ctx + CTX_TYPE, h->channels[c].config->inbound ? 2 : 1);
		put32(ctx + CTX_INDEX, h->channels[c].config->event_ring);
		h->channels[c].running = false;
	}
	for (unsigned e = 0; e < h->config.event_count; e++) {
		struct mhi_host_ring *r = &h->events[e];
		uint8_t *ctx = event_context(h, e);

		h->event_active[e] = h->event_db_sent[e] = false;
		h->event_busy[e] = h->event_again[e] = false;
		if (r->base == NULL) continue;
		put32(ctx + CTX_CFG, h->config.events[e].moderation_ms << 16);
		put32(ctx + CTX_TYPE, EVENT_RING_VALID);
		put32(ctx + CTX_INDEX, h->config.events[e].vector);
		put64(ctx + CTX_RBASE, r->address);
		put64(ctx + CTX_RLEN, (uint64_t)r->elements * MHI_ELEMENT);
		put64(ctx + CTX_RP, r->address);
		r->rp = r->wp = 0;
		put64(ctx + CTX_WP, r->address);
	}
	memset(h->command_context, 0, MHI_CONTEXT);
	h->commands.rp = h->commands.wp = 0;
	put64(h->command_context + CTX_RBASE, h->commands.address);
	put64(h->command_context + CTX_RLEN, (uint64_t)h->commands.elements * MHI_ELEMENT);
	put64(h->command_context + CTX_RP, h->commands.address);
	put64(h->command_context + CTX_WP, h->commands.address);
	device_fence();

	wr(h, MHI_REG_CCABAP + 4, (uint32_t)(h->channel_contexts_address >> 32));
	wr(h, MHI_REG_CCABAP, (uint32_t)h->channel_contexts_address);
	wr(h, MHI_REG_ECABAP + 4, (uint32_t)(h->event_contexts_address >> 32));
	wr(h, MHI_REG_ECABAP, (uint32_t)h->event_contexts_address);
	wr(h, MHI_REG_CRCBAP + 4, (uint32_t)(h->command_context_address >> 32));
	wr(h, MHI_REG_CRCBAP, (uint32_t)h->command_context_address);
	wr(h, MHI_REG_CTRLBASE + 4, (uint32_t)(h->config.iova_start >> 32));
	wr(h, MHI_REG_CTRLBASE, (uint32_t)h->config.iova_start);
	wr(h, MHI_REG_CTRLLIMIT + 4, (uint32_t)(h->config.iova_stop >> 32));
	wr(h, MHI_REG_CTRLLIMIT, (uint32_t)h->config.iova_stop);
	wr(h, MHI_REG_DATABASE + 4, (uint32_t)(h->config.iova_start >> 32));
	wr(h, MHI_REG_DATABASE, (uint32_t)h->config.iova_start);
	wr(h, MHI_REG_DATALIMIT + 4, (uint32_t)(h->config.iova_stop >> 32));
	wr(h, MHI_REG_DATALIMIT, (uint32_t)h->config.iova_stop);
	cfg = rd(h, MHI_REG_MHICFG);
	cfg = (cfg & 0xffffu) | h->config.event_count << 16 | h->config.hardware_event_count << 24;
	wr(h, MHI_REG_MHICFG, cfg);
	if (h->config.hardware_event_count && ((rd(h, MHI_REG_MHICFG) >> 16) != (cfg >> 16) ||
	    register64(h, MHI_REG_CCABAP) != h->channel_contexts_address ||
	    register64(h, MHI_REG_ECABAP) != h->event_contexts_address ||
	    register64(h, MHI_REG_CRCBAP) != h->command_context_address)) {
		h->failed = true;
		__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
		return EIO;
	}
	return 0;
}

/* MHICTRL.RESET, and until the device has cleared it. 0, or ETIMEDOUT. */
static int
device_reset(struct mhi_host *h)
{
	wr(h, MHI_REG_MHICTRL, MHI_CTRL_RESET);
	return (wait_for(h, reset_done, h->config.timeout_ms));
}

int
mhi_host_power_up(struct mhi_host *h)
{
	uint32_t ee, status;
	uint64_t began = now_ms(h);
	int error = 0;

	if (!h->allocated) {
		return (ENXIO);
	}
	h->ops->lock(h->context);
	h->powered = true;
	h->mission = h->failed = h->mission_told = false;
	h->wakes = 0;
	h->channel_doorbells = h->event_doorbells = 0;
	h->bhi = rd(h, MHI_REG_BHIOFF);
	h->bhie = rd(h, MHI_REG_BHIEOFF);
	if (h->bhi == 0xffffffffu || h->bhi == 0 || h->bhie == 0) {
		say(h, "mhi: the device has no BHI (BHIOFF %#x, BHIEOFF %#x): it does not answer", h->bhi, h->bhie);
		error = ENODEV;
		goto out;
	}
	for (uint32_t at = MHI_BHIE_RXVECADDR_LOW; at < MHI_BHIE_RXVECSTATUS; at += 4) {
		wr(h, h->bhie + at, 0);			/* (no crash dump is asked for) */
	}
	wr(h, h->bhi + MHI_BHI_INTVEC, 0);
	ee = device_ee(h);
	status = rd(h, MHI_REG_MHISTATUS);
	h->ee = ee;
	h->state = status >> 8 & 0xff;
	say(h, "mhi: power-up: EE %u, state %#x (MHISTATUS %#x)", ee, h->state, status);
	if (h->state == MHI_HOST_STATE_SYS_ERR || (status & MHI_STATUS_SYSERR)) {
		if (device_reset(h) != 0) {
			say(h, "mhi: the device stays in its error state");
			error = EIO;
			goto out;
		}
		h->state = device_state(h);
		h->failed = false;
	}
	if (ee != MHI_HOST_EE_PBL && ee != MHI_HOST_EE_PTHRU && ee != MHI_HOST_EE_EDL) {
		say(h, "mhi: the device is in EE %u, not in its primary boot loader: it is not booted", ee);
		error = EIO;
		goto out;
	}

	/* 1. the secondary boot loader, through BHI */
	wr(h, h->bhi + MHI_BHI_STATUS, 0);
	wr(h, h->bhi + MHI_BHI_IMGADDR_HIGH, (uint32_t)(h->image_address >> 32));
	wr(h, h->bhi + MHI_BHI_IMGADDR_LOW, (uint32_t)h->image_address);
	wr(h, h->bhi + MHI_BHI_IMGSIZE, (uint32_t)(h->config.sbl_length < h->image_size ? h->config.sbl_length : h->image_size));
	device_fence();
	wr(h, h->bhi + MHI_BHI_IMGTXDB, transaction(h));
	if (wait_for(h, bhi_done, h->config.timeout_ms) != 0 || (rd(h, h->bhi + MHI_BHI_STATUS) >> 30) != 2) {
		say(h, "mhi: BHI: the boot loader was not taken (BHI_STATUS %#x, error %#x, EE %u)", rd(h, h->bhi + MHI_BHI_STATUS),
		    rd(h, h->bhi + MHI_BHI_ERRCODE), device_ee(h));
		error = EIO;
		goto out;
	}
	say(h, "mhi: BHI: the boot loader (%zu bytes) is taken, %llu ms in", h->config.sbl_length,
	    (unsigned long long)(now_ms(h) - began));

	/* 2. READY: the MMIO set up, the event rings given their room, M0 */
	if (wait_for(h, ready, h->config.timeout_ms) != 0) {
		say(h, "mhi: the device does not say READY (MHICTRL %#x, MHISTATUS %#x)", rd(h, MHI_REG_MHICTRL),
		    rd(h, MHI_REG_MHISTATUS));
		error = ETIMEDOUT;
		goto out;
	}
	if ((error = setup_mmio(h)) != 0) goto out;
	(void)activate_event_rings(h, false);
	set_state(h, MHI_HOST_STATE_M0);

	/* 3. in SBL: the whole image, through BHIe */
	if (wait_for(h, in_sbl, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the device does not come to its secondary boot loader (EE %u, state %#x)", h->ee, h->state);
		error = h->failed ? EIO : ETIMEDOUT;
		goto out;
	}
	if (!in_mission(h->ee)) {
		uint32_t number = transaction(h);

		wr(h, h->bhie + MHI_BHIE_TXVECADDR_HIGH, (uint32_t)(h->vectors_address >> 32));
		wr(h, h->bhie + MHI_BHIE_TXVECADDR_LOW, (uint32_t)h->vectors_address);
		wr(h, h->bhie + MHI_BHIE_TXVECSIZE, (uint32_t)h->vectors_size);
		device_fence();
		wr(h, h->bhie + MHI_BHIE_TXVECDB, (rd(h, h->bhie + MHI_BHIE_TXVECDB) & ~0x3fffffffu) | number);
		if (wait_for(h, bhie_done, h->config.timeout_ms) != 0 || (rd(h, h->bhie + MHI_BHIE_TXVECSTATUS) >> 30) != 2) {
			say(h, "mhi: BHIe: the image was not taken (TXVECSTATUS %#x)", rd(h, h->bhie + MHI_BHIE_TXVECSTATUS));
			error = EIO;
			goto out;
		}
		say(h, "mhi: BHIe: the image (%zu bytes) is taken, %llu ms in", h->image_size, (unsigned long long)(now_ms(h) - began));
	}

	/* 4. the mission: the channels for it */
	if (wait_for(h, mission, h->config.timeout_ms) != 0 || h->failed) {
		say(h, "mhi: the image does not come to its mission (EE %u, state %#x)", h->ee, h->state);
		error = h->failed ? EIO : ETIMEDOUT;
		goto out;
	}
	/*
	 * The register's EE turns to the mission before the image's MHI takes commands: a channel's
	 * start sent then was lost (no completion, ever). Linux waits for the device's event of the
	 * EE, and for M0, before it starts a channel; so here.
	 */
	if (wait_for(h, mission_told, 1000) != 0) {
		say(h, "mhi: no event of the device's for its mission in 1 s: its channels are started all the same");
	}
	if (wait_for(h, awake, h->config.timeout_ms) != 0 || wait_for(h, device_awake, h->config.timeout_ms) != 0 ||
	    h->failed) {
		say(h, "mhi: the device is not in M0 (state %#x)", h->state);
		error = EIO;
		goto out;
	}
	error = activate_event_rings(h, true);
	for (unsigned c = 0; c < h->config.channel_count && error == 0; c++) {
		if (h->channels[c].config->ee_mask & 1u << h->ee) {
			error = channel_start(h, &h->channels[c]);
		}
	}
	if (error == 0) {
		say(h, "mhi: the device runs its image (EE %u), its channels are started: %llu ms (doorbells rung again: %u)",
		    h->ee, (unsigned long long)(now_ms(h) - began), h->rung_again);
		__atomic_store_n(&h->ownership_dirty, false, __ATOMIC_RELEASE);
	}
out:
	if (error != 0) {
		/* Even a failed BHI/mission attempt can leave DMA requests outstanding.
		 * powered=false stops our poll/send path; it is not a device reset. */
		__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
		h->powered = false;
	}
	h->ops->unlock(h->context);
	return (error);
}

int
mhi_host_power_down(struct mhi_host *h)
{
	int error = 0;

	if (!h->allocated) {
		return (ENXIO);
	}
	h->ops->lock(h->context);
	if (h->powered) {
		/* A receiver may be outside the lock and still own its payload, and
		 * a command waiter may be between polls. They must finish first. */
		for (unsigned e = 0; e < h->config.event_count; e++) {
			if (h->event_busy[e]) {
				h->ops->unlock(h->context);
				return EBUSY;
			}
		}
		for (unsigned c = 0; c < h->config.channel_count; c++) {
			if (h->channels[c].command_in_flight) {
				h->ops->unlock(h->context);
				return EBUSY;
			}
		}
		/* The frontend and both endpoint channels must retire their live owner
		 * first. Recovery of an already failed controller may still issue RESET,
		 * but cannot free or silently adopt its uncertain pool. */
		for (unsigned c = 0; c < h->config.channel_count; c++) {
			struct mhi_host_channel *ch = &h->channels[c];
			if (!ch->config->client_tx || !ch->tx.base || ch->tx.retired) continue;
			if (!h->failed && mhi_host_ownership_clean(h)) {
				h->ops->unlock(h->context);
				return EBUSY;
			}
			ch->tx.closing = true;
			(void)tx_quarantine(h,ch);
		}
		memset(h->event_active, 0, sizeof(h->event_active));
		for (unsigned c = 0; c < h->config.channel_count; c++) h->channels[c].running = false;
		/* An inaccessible register is no more a reset acknowledgement than a
		 * RESET bit which never clears. Do not turn link loss into clean DMA. */
		if (rd(h, MHI_REG_MHICTRL) == 0xffffffffu || device_reset(h) != 0) {
			__atomic_store_n(&h->ownership_dirty, true, __ATOMIC_RELEASE);
			say(h, "mhi: power-down: reset is unproven (MHICTRL %#x); DMA ownership is dirty", rd(h, MHI_REG_MHICTRL));
		}
	}
	/* A second stop after a failed start/reset must preserve the failure.
	 * Only a complete later power-up clears the recovery latch. */
	if (__atomic_load_n(&h->ownership_dirty, __ATOMIC_ACQUIRE)) error = EIO;
	for (unsigned c = 0; c < h->config.channel_count; c++) {
		h->channels[c].running = false;
	}
	h->powered = h->mission = h->mission_told = false;
	h->channel_doorbells = h->event_doorbells = 0;
	h->wakes = 0;
	h->ops->unlock(h->context);
	return (error);
}

bool
mhi_host_ownership_clean(const struct mhi_host *h)
{
	return (h != NULL && h->allocated && !__atomic_load_n(&h->ownership_dirty, __ATOMIC_ACQUIRE));
}

/* ---- sending -------------------------------------------------------------------------------------- */

int
mhi_host_send(struct mhi_host *h, unsigned number, const void *data, size_t length)
{
	struct mhi_host_channel *ch;
	uint64_t until;
	uint32_t at;
	int error;

	h->ops->lock(h->context);
	ch = channel_numbered(h, number);
	if (ch == NULL || ch->config->inbound || ch->config->command_only || ch->config->client_tx || !ch->running || h->failed) {
		h->ops->unlock(h->context);
		return (ENOTCONN);
	}
	if (length == 0 || length > ch->buffer_length) {
		h->ops->unlock(h->context);
		return (EMSGSIZE);
	}
	for (until = now_ms(h) + h->config.timeout_ms; next(&ch->ring, ch->ring.wp) == ch->ring.rp;) {
		service_all(h);
		if (now_ms(h) > until || !ch->running) {
			h->ops->unlock(h->context);
			return (ETIMEDOUT);
		}
		h->ops->unlock(h->context);
		h->ops->pause(h->context, PAUSE_US);
		h->ops->lock(h->context);
	}
	at = ch->ring.wp;
	memcpy(ch->buffers + (size_t)at * ch->buffer_length, data, length);
	write_element(element(&ch->ring, at), ch->buffers_address + (uint64_t)at * ch->buffer_length, (uint32_t)length,
	    MHI_TRE_IEOT | MHI_TYPE_TRANSFER << 16);
	ch->ring.wp = next(&ch->ring, at);
	publish_transfer_wp(h, ch);
	h->ops->unlock(h->context);
	/* (rung in M0 only: the device is woken for it, and left to sleep again) */
	if ((error = mhi_host_wake_get(h)) == 0) {
		h->ops->lock(h->context);
		ring_channel(h, ch);
		h->ops->unlock(h->context);
	}
	mhi_host_wake_put(h);
	return (error);
}

/* ---- wake votes, suspend ---------------------------------------------------------------------------- */

int
mhi_host_wake_get(struct mhi_host *h)
{
	int error = 0;

	h->ops->lock(h->context);
	if (!h->powered || h->channel_doorbells == 0) {
		h->ops->unlock(h->context);
		return (0);
	}
	if (h->wakes++ == 0) {
		doorbell(h, h->channel_doorbells + 8 * MHI_WAKE_DOORBELL, 1);
	}
	if (h->state == MHI_HOST_STATE_M3) {
		;				/* (suspended: mhi_host_resume() is what brings it back) */
	} else if (h->state != MHI_HOST_STATE_M0) {
		error = wait_for(h, awake, h->config.timeout_ms);
	}
	h->ops->unlock(h->context);
	return (error);
}

void
mhi_host_wake_put(struct mhi_host *h)
{
	h->ops->lock(h->context);
	if (h->powered && h->channel_doorbells != 0 && h->wakes != 0 && --h->wakes == 0) {
		doorbell(h, h->channel_doorbells + 8 * MHI_WAKE_DOORBELL, 0);
	}
	h->ops->unlock(h->context);
}

static bool
suspended(struct mhi_host *h)
{
	return (h->state == MHI_HOST_STATE_M3 || h->failed);
}

int
mhi_host_suspend(struct mhi_host *h)
{
	int error;

	if ((error = mhi_host_wake_get(h)) != 0) {
		mhi_host_wake_put(h);
		return (error);
	}
	h->ops->lock(h->context);
	set_state(h, MHI_HOST_STATE_M3);
	error = wait_for(h, suspended, h->config.timeout_ms);
	if (error == 0 && h->failed) {
		error = EIO;
	}
	h->ops->unlock(h->context);
	mhi_host_wake_put(h);
	return (error);
}

int
mhi_host_resume(struct mhi_host *h)
{
	int error;

	h->ops->lock(h->context);
	if (!h->powered) {
		h->ops->unlock(h->context);
		return (ENXIO);
	}
	set_state(h, MHI_HOST_STATE_M0);
	error = wait_for(h, awake, h->config.timeout_ms);
	if (error == 0 && h->failed) {
		error = EIO;
	}
	h->ops->unlock(h->context);
	return (error);
}

static bool
in_rddm(struct mhi_host *h)
{
	return (device_ee(h) == MHI_HOST_EE_RDDM);
}

int
mhi_host_force_rddm(struct mhi_host *h)
{
	int error;

	h->ops->lock(h->context);
	wr(h, MHI_REG_SOC_RESET, 1);
	error = wait_for(h, in_rddm, h->config.timeout_ms);
	h->ops->unlock(h->context);
	return (error);
}
