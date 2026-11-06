/* The SDX55's MHI host: see modem_mhi.c. */
#ifndef _MODEM_MHI_H_
#define _MODEM_MHI_H_

#define mhi_host_init           modem_mhi_host_init
#define mhi_host_power_up       modem_mhi_host_power_up
#define mhi_host_power_down     modem_mhi_host_power_down
#define mhi_host_interrupt      modem_mhi_host_interrupt
#define mhi_host_send           modem_mhi_host_send
#define mhi_host_wake_get       modem_mhi_host_wake_get
#define mhi_host_wake_put       modem_mhi_host_wake_put
#define mhi_host_suspend        modem_mhi_host_suspend
#define mhi_host_resume         modem_mhi_host_resume
#define mhi_host_force_rddm     modem_mhi_host_force_rddm
#if __has_include("mhi_host.h")
#include "mhi_host.h"
#else
#include "../../../../wifi/lx/src/mhi_host.h"
#endif

int mdm_mhi_to_sbl(struct mhi_host *h);
int mdm_mhi_to_mission(struct mhi_host *h, uint32_t ms);
int mdm_mhi_send(struct mhi_host *h, unsigned number, const void *data, size_t length, bool more);
int mdm_mhi_sfr_config(struct mhi_host *h, uint64_t address, uint32_t length);
uint32_t mdm_mhi_ee(struct mhi_host *h);
uint32_t mdm_mhi_state(struct mhi_host *h);

#endif
