/* AWDL time is expressed in 1024-us TUs. Each state belongs to one peer
 * and one owner epoch. The caller supplies RX time in the same host monotonic
 * domain as its local scheduler, as OWL does in src/rx.c:474. */
#ifndef HK_AWDL_PEER_WINDOWS_H
#define HK_AWDL_PEER_WINDOWS_H
#include "awdl_rx_action.h"

struct awdl_window_clock {
	uint64_t anchor_us, observed_us, valid_until_us;
	uint32_t eaw_us;
	uint16_t base_aw, period_tu;
	uint8_t presence, slots[16];
	bool from_peer;
};
struct awdl_window {
	uint64_t start_us, end_us;
	uint16_t aw;
	uint8_t channel;
	bool in_guarded_interval;
};

static inline bool
awdl_clock_local(struct awdl_window_clock *out, uint64_t now_us, uint8_t channel)
{
	if (!out || !now_us || !channel || now_us > UINT64_MAX - 16u*65536u) return false;
	struct awdl_window_clock s={0};
	s.anchor_us=s.observed_us=now_us;
	s.eaw_us=65536; s.period_tu=16; s.presence=4;
	s.valid_until_us=now_us+16ull*s.eaw_us;
	for (unsigned i=0;i<16;i++) s.slots[i]=channel;
	*out=s;
	return true;
}

/* Form an estimate from a validated sync TLV and its own sequence. Election
 * and peer admission are independent of this calculation. No clock is set
 * in the radio by this function. An invalid sample leaves out untouched. */
static inline bool
awdl_clock_from_action(const struct awdl_rx_action *a,uint64_t rx_us,struct awdl_window_clock *out)
{
	if (!a || !out || !rx_us || !(a->fields & AWDL_RX_HAS_SYNC)) return false;
	const struct awdl_rx_sync *sync=&a->sync;
	const struct awdl_rx_channel_sequence *seq=(a->fields & AWDL_RX_HAS_SEQUENCE)?
	    &a->sequence:&sync->sequence;
	unsigned presence=sync->presence_mode;
	if (!presence || (presence&(presence-1)) || !sync->aw_period_tu ||
	    !seq->entries_decoded || seq->count!=16 || seq->duplicate_count ||
	    seq->step_count+1u!=presence || seq->fill_channel!=0xffff) return false;
	uint32_t period=(uint32_t)sync->aw_period_tu*presence;
	if (period>65535u || !sync->tx_down_counter_tu || sync->tx_down_counter_tu>period) return false;
	uint64_t elapsed=(uint64_t)(period-sync->tx_down_counter_tu)*1024;
	if (rx_us<elapsed || rx_us>UINT64_MAX-16ull*period*1024) return false;
	struct awdl_window_clock s={0};
	s.observed_us=rx_us; s.anchor_us=rx_us-elapsed;
	s.base_aw=(uint16_t)(sync->next_aw_sequence & ~(presence-1));
	s.period_tu=sync->aw_period_tu; s.presence=(uint8_t)presence;
	s.eaw_us=period*1024; s.valid_until_us=rx_us+16ull*s.eaw_us;
	s.from_peer=true;
	for (unsigned i=0;i<16;i++) s.slots[i]=seq->entries[i].number;
	*out=s;
	return true;
}

static inline bool
awdl_window_at(const struct awdl_window_clock *s,uint64_t now_us,unsigned guard_tu,
    struct awdl_window *out)
{
	if (!s || !out || !s->eaw_us || !s->presence || !s->period_tu ||
	    now_us<s->observed_us || now_us>=s->valid_until_us) return false;
	uint64_t delta=now_us-s->anchor_us;
	uint64_t periods=delta/s->eaw_us;
	uint32_t phase=(uint32_t)(delta%s->eaw_us);
	uint16_t base=(uint16_t)(s->base_aw + periods*s->presence);
	struct awdl_window w={0};
	w.aw=(uint16_t)(base+phase/(1024u*s->period_tu));
	w.channel=s->slots[(base/s->presence)&15];
	w.start_us=now_us-phase;
	if (w.start_us > UINT64_MAX - s->eaw_us) return false;
	w.end_us=w.start_us+s->eaw_us;
	uint64_t guard=(uint64_t)guard_tu*1024;
	w.in_guarded_interval=w.channel && phase>=guard && (uint64_t)(s->eaw_us-phase)>guard;
	*out=w;
	return true;
}
#endif
