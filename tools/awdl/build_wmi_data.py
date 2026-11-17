"""Dedicated WMI_OFFCHAN_DATA_TX_SEND path, from the native country control.

Raw-data command/params/completion layouts follow Qualcomm's coral Android11
wlan-fw-api wmi_unified.h. Service123/144 are checked before any data command.
Reuse the existing management queue/IDR/DMA lifecycle without extra peers.
"""
import json,subprocess
from build_roc import BUS,HERE,OUT,read,write,once,sha
BASE='/tmp/awdl-country-idvVy7'
BASE_SHA='44ef2c448c6c966d40c585ef0cbf55bf234e420fe91ce5a78cdc0bfdac772226'
def prepend_body(s,name,code):
    a=s.index(name+'(');b=s.index('{',a)+1
    return s[:b]+'\n'+code+'\n'+s[b:]
def build():
    obj=BASE+'/xnu-deps/build/ath11k/out/ath11k.o'
    actual=subprocess.check_output(['docker','exec',BUS,'sha256sum',obj],text=True).split()[0]
    if actual!=BASE_SHA:raise ValueError('native country-layout base changed')
    stage=subprocess.check_output(['docker','exec',BUS,'mktemp','-d','/tmp/awdl-wmi-data-XXXXXX'],text=True).strip()
    subprocess.run(['docker','exec',BUS,'cp','-a',BASE+'/.',stage+'/'],check=True)
    src=stage+'/wifi/lx/src/';work=stage+'/xnu-deps/build/ath11k/';driver=work+'src/ath11k/'
    live=read(src+'live_transport.inc')
    live=once(live,'ti->flags=IEEE80211_TX_CTL_REQ_TX_STATUS|((frame[4]&1)?IEEE80211_TX_CTL_NO_ACK:0);',
        'ti->flags=IEEE80211_TX_CTL_REQ_TX_STATUS|(basis==1?IEEE80211_TX_CTL_TX_OFFCHAN:0)|((frame[4]&1)?IEEE80211_TX_CTL_NO_ACK:0);')
    live=once(live,'if (!f->wbm || f->source!=0 || f->reason!=0) live.dropped++;',
        'if (!f->wbm || f->reason!=0 || (f->source!=0 && !(f->ring==0xffffu && f->source==3))) live.dropped++;')
    live+='\n'+(HERE/'wmi_data_transport.inc').read_text();write(src+'live_transport.inc',live)
    roc=read(src+'roc.inc')
    roc=prepend_body(roc,'lx_awdl_psf_prepare','    if(lx_awdl_wmi_data_owned(hw,skb))return live_wmi_data_prepare(hw,skb,vdev,desc,frequency);')
    roc=prepend_body(roc,'lx_awdl_psf_fw','    if(live_wmi_data_complete(hw,skb,desc,status))return;')
    roc=prepend_body(roc,'lx_awdl_psf_release','    if(lx_awdl_wmi_data_owned(hw,skb)){live_freed(hw,skb,4);return;}')
    write(src+'roc.inc',roc)
    header=read(src+'roc_state.h');header=once(header,'#endif','int lx_awdl_wmi_data_owned(struct ieee80211_hw *,struct sk_buff *);\nunsigned lx_awdl_wmi_data_frequency(struct ieee80211_hw *);\n#endif')
    write(src+'roc_state.h',header);write(driver+'roc_state.h',header)
    mac=read(driver+'mac.c');mac=once(mac,'} else if (ieee80211_is_mgmt(hdr->frame_control)) {',
        '} else if (ieee80211_is_mgmt(hdr->frame_control) || lx_awdl_wmi_data_owned(hw,skb)) {')
    write(driver+'mac.c',mac)
    wmi=read(driver+'wmi.c')
    wmi=once(wmi,'int ath11k_wmi_mgmt_send(', 'extern void lx_tell(const char *,...);\nint ath11k_wmi_mgmt_send(')
    anchor='\t[WMI_TAG_MGMT_TX_COMPL_EVENT]\n\t\t= { .min_len = sizeof(struct wmi_mgmt_tx_compl_event) },'
    wmi=once(wmi,anchor,anchor+'\n\t[WMI_TAG_OFFCHAN_DATA_TX_COMPL_EVENT] = { .min_len = 3*sizeof(u32) },')
    marker='\tbuf_len = frame->len < WMI_MGMT_SEND_DOWNLD_LEN ?'
    wmi=once(wmi,marker,'''    bool raw_data=lx_awdl_wmi_data_owned(ar->hw,frame);
    u32 data_frequency=0;
    if(raw_data) {
        bool offchan=test_bit(WMI_TLV_SERVICE_OFFCHAN_TX_WMI,ar->ab->wmi_ab.svc_map);
        bool nonpause=test_bit(WMI_TLV_SERVICE_OFFCHAN_DATA_TID_SUPPORT,ar->ab->wmi_ab.svc_map);
        static unsigned announced;
        if(!__atomic_exchange_n(&announced,1,__ATOMIC_RELAXED))
            lx_tell("awdl-wmi-data: service123=%u service144=%u\\n",offchan,nonpause);
        if(!offchan && !nonpause)return -EOPNOTSUPP;
        tx_params_valid=nonpause;
        data_frequency=lx_awdl_wmi_data_frequency(ar->hw);
        if(!data_frequency)return -ECANCELED;
    }
'''+marker)
    wmi=once(wmi,'cmd->chanfreq = ath11k_wmi_mgmt_get_freq(ar, info);','cmd->chanfreq = raw_data?data_frequency:ath11k_wmi_mgmt_get_freq(ar, info);')
    wmi=once(wmi,'if (tx_params_valid)\n\t\tlen += sizeof(*params);','if (tx_params_valid || raw_data)\n\t\tlen += sizeof(*params);')
    wmi=once(wmi,'if (tx_params_valid) {\n\t\tparams =','if (tx_params_valid || raw_data) {\n\t\tparams =')
    wmi=once(wmi,'FIELD_PREP(WMI_TLV_TAG, WMI_TAG_MGMT_TX_SEND_CMD) |',
        'FIELD_PREP(WMI_TLV_TAG, raw_data?WMI_TAG_OFFCHAN_DATA_TX_SEND_CMD:WMI_TAG_MGMT_TX_SEND_CMD) |')
    wmi=once(wmi,'params->tx_params_dword1 |= WMI_TX_PARAMS_DWORD1_CFR_CAPTURE;',
        '''if(raw_data) {
            /* Qualcomm ABI: 0dBm, OFDM6Mbps, one stream, four attempts;
             * bandwidth20MHz, data frame, no CFR request. Radio policy is
             * unchanged and can further constrain these requested values. */
            params->tx_params_dword0=tx_params_valid?0x40101000u:0;
            params->tx_params_dword1=tx_params_valid?0x00108400u:0x00100000u;
        }else params->tx_params_dword1 |= WMI_TX_PARAMS_DWORD1_CFR_CAPTURE;''')
    wmi=once(wmi,'ath11k_wmi_cmd_send(wmi, skb, WMI_MGMT_TX_SEND_CMDID);',
        'ath11k_wmi_cmd_send(wmi, skb, raw_data?WMI_OFFCHAN_DATA_TX_SEND_CMDID:WMI_MGMT_TX_SEND_CMDID);')
    wmi=once(wmi,'if (tracked>0) lx_awdl_psf_submit_result(ar->hw,buf_id,ret);',
        'if (tracked>0 && !raw_data) lx_awdl_psf_submit_result(ar->hw,buf_id,ret);')
    handler='''static void ath11k_offchan_data_tx_compl_event(struct ath11k_base *ab,struct sk_buff *skb)
{
    lx_tell("awdl-wmi-data: event bytes=%u\\n",(unsigned)skb->len);
    const void **tb=ath11k_wmi_tlv_parse_alloc(ab,skb,GFP_ATOMIC);
    if(IS_ERR(tb)){lx_tell("awdl-wmi-data: event parse failed=%ld\\n",(long)PTR_ERR(tb));return;}
    const u32 *ev=tb[WMI_TAG_OFFCHAN_DATA_TX_COMPL_EVENT];
    if(!ev){lx_tell("awdl-wmi-data: event missing expected tag\\n");kfree(tb);return;}
    struct wmi_mgmt_tx_compl_event param={.desc_id=ev[0],.status=ev[1],.pdev_id=ev[2]};
    lx_tell("awdl-wmi-data: event desc=%u status=%u pdev=%u\\n",param.desc_id,param.status,param.pdev_id);
    rcu_read_lock();
    struct ath11k *ar=ath11k_mac_get_ar_by_pdev_id(ab,param.pdev_id);
    if(ar) {
        spin_lock_bh(&ar->txmgmt_idr_lock);
        struct sk_buff *frame=idr_find(&ar->txmgmt_idr,param.desc_id);
        bool owned=frame && lx_awdl_wmi_data_owned(ar->hw,frame);
        spin_unlock_bh(&ar->txmgmt_idr_lock);
        if(owned)wmi_process_mgmt_tx_comp(ar,&param);
    }
    rcu_read_unlock();kfree(tb);
}

'''
    wmi=once(wmi,'static struct ath11k *ath11k_get_ar_on_scan_state(',handler+'static struct ath11k *ath11k_get_ar_on_scan_state(')
    wmi=once(wmi,'\tcase WMI_MGMT_TX_COMPLETION_EVENTID:\n\t\tath11k_mgmt_tx_compl_event(ab, skb);\n\t\tbreak;',
        '\tcase WMI_OFFCHAN_DATA_TX_COMPLETION_EVENTID:\n\t\tath11k_offchan_data_tx_compl_event(ab,skb);\n\t\tbreak;\n\tcase WMI_MGMT_TX_COMPLETION_EVENTID:\n\t\tath11k_mgmt_tx_compl_event(ab, skb);\n\t\tbreak;')
    write(driver+'wmi.c',wmi)
    subprocess.run(['docker','exec',BUS,'rm',work+'obj/lx_lx_80211.o',work+'obj/ath11k_mac.o',work+'obj/ath11k_wmi.o'],check=True)
    with (OUT/'wmi-data-v2-build.log').open('w') as log:
        subprocess.run(['docker','exec',BUS,'bash',stage+'/tools/mk-ath11k.sh'],check=True,stdout=log,stderr=subprocess.STDOUT)
    dest=OUT/'ath11k-wmi-data-v2.o';subprocess.run(['docker','cp',BUS+':'+work+'out/ath11k.o',str(dest)],check=True);dest.chmod(0o600)
    r=dict(object=str(dest),sha256=sha(dest),stage=stage,source_radio_sha256=BASE_SHA,offchan_data_wmi=True,additional_peer=False,fixed_endpoint=False,explicit_country=True,lifetime_seconds=420)
    (OUT/'wmi-data-v2-build.json').write_text(json.dumps(r,indent=2)+'\n');print(json.dumps(r))
if __name__=='__main__':build()
