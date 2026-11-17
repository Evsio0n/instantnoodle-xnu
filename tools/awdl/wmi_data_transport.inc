/* Dedicated WMI raw-data frames use the driver's management IDR/DMA owner,
 * but have their own command and completion event. The standard queue owns
 * the skb through completion or purge; live_flight never unmaps/frees it. */
#define LIVE_WMI_DATA_RING 0xffffu
int lx_awdl_wmi_data_owned(struct ieee80211_hw *hw,struct sk_buff *skb)
{
    return live_find_skb(hw,skb)!=NULL;
}
unsigned lx_awdl_wmi_data_frequency(struct ieee80211_hw *hw)
{
    if(!EG(live.armed) || EG(live.stop) || echo.hw!=hw)return 0;
    if(lx_awdl_home_basis(hw)==3)return air.mhz;
    return lx_awdl_data_foreign(hw)?air.listen_mhz:0;
}
static int live_wmi_data_prepare(struct ieee80211_hw *hw,struct sk_buff *skb,
    unsigned vdev,unsigned desc,unsigned frequency)
{
    struct live_flight *f=live_find_skb(hw,skb);
    if(!f || !EG(live.armed) || EG(live.stop) || !EG(live.accept) ||
       f->lease!=live.lease || vdev!=echo.vdev || !auto_active_fresh())return -ECANCELED;
    bool home=frequency==air.mhz && lx_awdl_home_basis(hw)==3;
    if(!home && (frequency!=air.listen_mhz || !lx_awdl_data_foreign(hw)))return -ECANCELED;
    live_bound(hw,skb,vdev,LIVE_WMI_DATA_RING,desc,0);
    lx_tell("awdl-wmi-data: submit ticket=%u desc=%u frequency=%u home=%u\n",f->tag,desc,frequency,home);
    return 1;
}
static bool live_wmi_data_complete(struct ieee80211_hw *hw,struct sk_buff *skb,
    unsigned desc,unsigned status)
{
    struct live_flight *f=live_find_skb(hw,skb);
    if(!f)return false;
    if(EG(f->bound) && f->ring==LIVE_WMI_DATA_RING && f->desc==desc) {
        /* Source3 is the firmware completion namespace, not a TQM ACK. */
        live_wbm(hw,skb,LIVE_WMI_DATA_RING,desc,3,status,0,0);
        lx_tell("awdl-wmi-data: complete ticket=%u desc=%u status=%u\n",f->tag,desc,status);
    }
    return true;
}
