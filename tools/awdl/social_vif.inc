/* A live secondary VDEV transmits its own AWDL management frames. Grant
 * its social-channel ROC to that same owner, as for its data ROCs. Before
 * the secondary carrier is up, retain the primary's passive discovery path. */
static struct ieee80211_vif *auto_social_vif(struct lx_radio *r,struct ieee80211_vif *v)
{
    if (v && EG(echo.active) && echo.hw==&r->hw && EG(echo.start) &&
        !EG(echo.start_status) && !EG(echo.stop) && echo.vdev==lx_awdl_roc_vdev(v))
        return v;
    return r->vif;
}
