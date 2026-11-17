/* Qualcomm's public coral/android11 WMI ABI describes this as three u32
 * fields: TLV header, pdev id, receive-filter promiscuous enable. This is
 * firmware service145, distinct from an unsupported monitor interface.
 * No command is sent unless that exact service was advertised. */
int lx_awdl_receiver_mode(struct ieee80211_hw *hw,int enable)
{
	struct ath11k *ar=hw->priv;
	if(enable!=0 && enable!=1) return -EINVAL;
	if(!test_bit(WMI_TLV_SERVICE_RX_PROMISC_ENABLE_SUPPORT,ar->ab->wmi_ab.svc_map)) return -EOPNOTSUPP;
	struct {u32 tlv_header,pdev_id,enable;} *cmd;
	struct sk_buff *skb=ath11k_wmi_alloc_skb(ar->wmi->wmi_ab,sizeof(*cmd));
	if(!skb) return -ENOMEM;
	cmd=(void *)skb->data;
	cmd->tlv_header=FIELD_PREP(WMI_TLV_TAG,WMI_TAG_PDEV_SET_RX_FILTER_PROMISCUOUS_CMD)|
	    FIELD_PREP(WMI_TLV_LEN,sizeof(*cmd)-TLV_HDR_SIZE);
	cmd->pdev_id=ar->pdev->pdev_id;cmd->enable=enable;
	mutex_lock(&ar->conf_mutex);
	int rc=ath11k_wmi_cmd_send(ar->wmi,skb,WMI_PDEV_SET_RX_FILTER_PROMISCUOUS_CMDID);
	mutex_unlock(&ar->conf_mutex);
	if(rc)dev_kfree_skb(skb);
	return rc;
}
