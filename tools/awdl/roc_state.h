#ifndef LX_AWDL_ROC_STATE_H
#define LX_AWDL_ROC_STATE_H

struct ieee80211_hw;
struct ieee80211_vif;
enum lx_awdl_roc_event { LX_ROC_START, LX_ROC_FOREIGN, LX_ROC_BSS, LX_ROC_DONE, LX_ROC_DEQUEUED };
struct lx_awdl_roc_state {
	unsigned choice, attempted, active;
	unsigned home_mhz, target_mhz, vdev, scan, request;
	unsigned started, foreign, bss, done, dequeued, reason, ready, expired;
	unsigned target_rx, target_psf, target_mif, home_beacon, home_data;
};
void lx_awdl_roc_wmi(struct ieee80211_hw *, unsigned vdev, unsigned scan, unsigned request,
    enum lx_awdl_roc_event, unsigned frequency, unsigned reason);
void lx_awdl_roc_notify(struct ieee80211_hw *, int ready);
int lx_awdl_roc_driver_idle(struct ieee80211_hw *);
unsigned lx_awdl_roc_vdev(struct ieee80211_vif *);

#endif
