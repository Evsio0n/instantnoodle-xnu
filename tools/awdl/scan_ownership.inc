/* The driver takes ar->data_lock at each real transition from IDLE into
 * a scan or ROC. The token is zero for every other requester, including
 * an immediately following scan that reuses the same VDEV and WMI ID. */
static struct ath11k *awdl_claim_ar;
static unsigned awdl_claim_seq;
static void awdl_scan_acquired(struct ath11k *ar,struct ieee80211_vif *vif,unsigned freq)
{
	lockdep_assert_held(&ar->data_lock);
	awdl_claim_ar=ar;
	awdl_claim_seq=freq?lx_awdl_request_token(ar->hw,vif,freq):0;
}
int lx_awdl_scan_state(struct ieee80211_hw *hw,unsigned token,unsigned vdev)
{
	struct ath11k *ar=hw->priv;
	spin_lock_bh(&ar->data_lock);
	int state=LX_AWDL_SCAN_IDLE;
	if (ar->scan.state!=ATH11K_SCAN_IDLE) {
		if (ar->scan.is_roc && token && awdl_claim_ar==ar && token==awdl_claim_seq)
			state=LX_AWDL_SCAN_OURS;
		else if (ar->scan.vdev_id==vdev) state=LX_AWDL_SCAN_OTHER_SAME_VDEV;
		else state=ar->scan.is_roc?LX_AWDL_SCAN_OTHER_ROC:LX_AWDL_SCAN_OTHER;
	}
	spin_unlock_bh(&ar->data_lock);
	return state;
}
