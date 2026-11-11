/* The SDX55's MHI host: see modem_mhi.c. */
#ifndef _MODEM_MHI_H_
#define _MODEM_MHI_H_

/* The existing NF mission may also request EFS in addition to Sahara/IPCR. */
#define MHI_HOST_CHANNELS       6
#define MHI_HOST_EVENT_RINGS    17
#define mhi_host_init           modem_mhi_host_init
#define mhi_host_power_up       modem_mhi_host_power_up
#define mhi_host_power_down     modem_mhi_host_power_down
#define mhi_host_ownership_clean modem_mhi_host_ownership_clean
#define mhi_host_interrupt      modem_mhi_host_interrupt
#define mhi_host_poll_event     modem_mhi_host_poll_event
#define mhi_host_send           modem_mhi_host_send
#define mhi_host_wake_get       modem_mhi_host_wake_get
#define mhi_host_wake_put       modem_mhi_host_wake_put
#define mhi_host_suspend        modem_mhi_host_suspend
#define mhi_host_resume         modem_mhi_host_resume
#define mhi_host_force_rddm     modem_mhi_host_force_rddm
#if __has_include("mhi_host.h")
#include "mhi_host.h"
#else
#include "../../../wifi/lx/src/mhi_host.h"
#endif

int mdm_mhi_to_sbl(struct mhi_host *h);
int mdm_mhi_to_mission(struct mhi_host *h, uint32_t ms);
int mdm_mhi_send(struct mhi_host *h, unsigned number, const void *data, size_t length, bool more);
int mdm_mhi_sfr_config(struct mhi_host *h, uint64_t address, uint32_t length);
uint32_t mdm_mhi_ee(struct mhi_host *h);
uint32_t mdm_mhi_state(struct mhi_host *h);

enum {
	MDM_IP_HW0_UL = 100, MDM_IP_HW0_DL = 101,
	MDM_IP_HW0_UL_ER = 7, MDM_IP_HW0_DL_ER = 8,
	MDM_IP_HW0_MRU = 32768
};
struct mdm_ip_hw0_receipt {
	unsigned start_attempted, start_ack, reset_attempted, reset_ack; /* bit 0 UL, bit 1 DL */
	unsigned er8_events;
	unsigned ner, nhwer; /* MHICFG read-back before START. */
	unsigned empty, buffers, ipcr_running, ipcr_ul_state, ipcr_dl_state;
	uint32_t ee, state;
	bool quarantined;
};
int mdm_mhi_ip_hw0_config(struct mhi_host_config *);
int mdm_mhi_ip_hw0_trial(struct mhi_host *, struct mdm_ip_hw0_receipt *);

/* Private preparatory provider. A fresh controller allocation is required
 * for each generation. Caller serializes begin/close against each other.
 * post/copy may race close; valid data is copied under the MHI lock.
 * A short destination leaves the complete transfer unit owned by the provider for
 * retry. No API configures IP, sends UL or exposes the private DMA pointer. */
int mdm_mhi_rx_config(struct mhi_host_config *);
/* The producer is the specific attached interface for this bearer generation.
 * The copy callback runs synchronously under the MHI mutex and must not sleep
 * or call MHI. It never receives ownership of the destination. */
struct mdm_mhi_tx_status {
	unsigned capacity, buffers, pending;
	uint64_t published, completed, cancelled, doorbells;
	bool ready, bound, retired, quarantined;
};
int mdm_mhi_tx_config(struct mhi_host_config *);
int mdm_mhi_tx_status(struct mhi_host *, uint64_t epoch, struct mdm_mhi_tx_status *);
int mdm_mhi_tx_bind(struct mhi_host *, uint64_t epoch, const void *producer, uint32_t mtu);
void mdm_mhi_tx_unbind(struct mhi_host *, uint64_t epoch, const void *producer);
int mdm_mhi_tx_submit(struct mhi_host *, uint64_t epoch, const void *producer, size_t n,
    int (*copy)(void *source, void *dst, size_t n), void *source);
int mdm_mhi_rx_begin(struct mhi_host *, uint64_t epoch);
int mdm_mhi_rx_post(struct mhi_host *, uint64_t epoch);
int mdm_mhi_rx_copy(struct mhi_host *, uint64_t epoch, void *, size_t, size_t *, unsigned *code);
int mdm_mhi_rx_close(struct mhi_host *, uint64_t epoch);
/* Sleepable ER8 owner. The return code describes event processing. If
 * supplied, input_error reports the independent ifnet handoff status.
 * A call with no fresh event may retry a completed slot after ENOBUFS. */
int mdm_mhi_rx_poll(struct mhi_host *, uint64_t epoch, unsigned budget,
    unsigned *events, int *input_error);
struct mdm_rx_receipt {
	uint64_t epoch;
	unsigned posted, completed, completion_code, late, wait_expired, start_ack, reset_ack, er8_drained;
	unsigned slot, pending_before_reset, pending, retired, quarantined, ul_wp, ipcr_running;
	unsigned ipcr_ul_state, ipcr_dl_state;
	uint32_t capacity, length, ee, state;
};
int mdm_mhi_rx_trial(struct mhi_host *, uint64_t epoch, unsigned window_ms, struct mdm_rx_receipt *);

#endif
