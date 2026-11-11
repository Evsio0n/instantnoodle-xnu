/* Render kernel-retained observations only in the explicitly historical section. */
- (void)renderHistory:(const struct hk_cellular_history *)h available:(BOOL)available live:(BOOL)live {
    self.liveRows.hidden=!live;
    self.historyRows.hidden=live;
    if (!available || h->version!=HK_CELLULAR_HISTORY_VERSION || h->size!=sizeof(*h)) {
        self.historyStamp.text=@"No recorded sample";
        self.historyCard.text=Unknown;self.historyBinding.text=Unknown;self.historyRadio.text=Unknown;
        self.historyOnline.text=@"No command record available";self.historyFault.text=@"No stop record available";return;
    }
    const struct hk_cellular_observation *s=&h->last;
    BOOL have=h->retained && h->latest_field_ms && h->now_ms>=h->latest_field_ms;
    self.historyStamp.text=have?[NSString stringWithFormat:@"Latest field at %.3f s on owner clock · %@",h->latest_field_ms/1000.0,age(h->now_ms,h->latest_field_ms)]:@"No valid field recorded";
    NSString *card=Unknown,*gw=Unknown,*physical=Unknown,*atr=Unknown;
    if(have && (s->valid_mask&HK_OBS_PHYSICAL) && s->physical_ms && s->physical_ms<=h->now_ms)
        physical=[NSString stringWithFormat:@"slot %u → logical %u · %@ · %@",s->physical_slot,s->logical_slot,s->card_present?@"present":@"absent",age(h->now_ms,s->physical_ms)];
    if(have && (s->valid_mask&HK_OBS_ATR) && s->atr_ms && s->atr_ms<=h->now_ms)
        atr=[NSString stringWithFormat:@"%@ · %@",s->atr_present?@"present":@"absent",age(h->now_ms,s->atr_ms)];
    if(have && (s->valid_mask&HK_OBS_CARD) && s->card_ms && s->card_ms<=h->now_ms)
        card=[NSString stringWithFormat:@"USIM %@ · %@",states(s->usim_states),age(h->now_ms,s->card_ms)];
    if(have && (s->valid_mask&HK_OBS_GW) && s->card_ms && s->card_ms<=h->now_ms) {
        NSString *mapping=choice(s->gw_status,@[@"Unassigned",@"Mapped",@"Invalid mapping",@"Outside reported range"]);
        if(s->gw_status==1 && s->gw_slot && s->gw_app)
            mapping=[NSString stringWithFormat:@"Mapped · slot %u, app %u · %@ %@",s->gw_slot,s->gw_app,choice(s->gw_type,@[@"unknown",@"SIM",@"USIM",@"RUIM",@"CSIM",@"ISIM"]),appState(s->gw_state)];
        gw=[NSString stringWithFormat:@"%@ · %@",mapping,age(h->now_ms,s->card_ms)];
    }
    self.historyCard.text=[NSString stringWithFormat:@"%@\n%@ · ATR %@",physical,card,atr];
    NSString *uimAck=s->ack_seen?[NSString stringWithFormat:@"%u/%u",s->ack_result,s->ack_error]:@"unknown";
    self.historyBinding.text=[NSString stringWithFormat:@"%@\nUIM ACK %@ · %u reads · %@",gw,uimAck,s->post_reads,s->verified?@"verified":@"unconfirmed"];
    NSString *dms=Unknown,*nas=Unknown,*wds=Unknown;
    if(have && (s->valid_mask&HK_OBS_DMS) && s->dms_ms && s->dms_ms<=h->now_ms)
        dms=[NSString stringWithFormat:@"%@ · %@",choice(s->dms_mode,@[@"online",@"low power",@"factory test",@"offline",@"resetting",@"shutting down",@"persistent low power",@"mode-only low power"]),age(h->now_ms,s->dms_ms)];
    if(have && (s->valid_mask&HK_OBS_NAS) && s->nas_ms && s->nas_ms<=h->now_ms) {
        NSArray *attach=@[@"unknown",@"attached",@"detached"];
        nas=[NSString stringWithFormat:@"%@ · CS %@ · PS %@ · %@",choice(s->nas_registration,@[@"unregistered",@"registered",@"searching",@"denied",@"unknown"]),choice(s->cs_attach,attach),choice(s->ps_attach,attach),age(h->now_ms,s->nas_ms)];
    }
    if(have && (s->valid_mask&HK_OBS_WDS) && s->wds_ms && s->wds_ms<=h->now_ms)
        wds=[NSString stringWithFormat:@"%@ · %@",choice(s->wds_state,@[@"unknown",@"disconnected",@"connected",@"suspended",@"authenticating"]),age(h->now_ms,s->wds_ms)];
    self.historyRadio.text=[NSString stringWithFormat:@"DMS %@\nNAS %@\nWDS %@",dms,nas,wds];
    if (h->online_ack_seen && h->online_ack_ms && h->online_ack_ms<=h->now_ms) {
        NSString *answer=h->online_result==0 && h->online_error==0?@"Acknowledged":@"Refused";
        self.historyOnline.text=[NSString stringWithFormat:@"%@ · result %u, error %u%@\n%.3f s on owner clock · %@",answer,h->online_result,h->online_error,
            h->online_error==52?@" (DEVICE_NOT_READY)":@"",h->online_ack_ms/1000.0,age(h->now_ms,h->online_ack_ms)];
    } else if(h->online_attempts && h->online_attempt_ms && h->online_attempt_ms<=h->now_ms) {
        self.historyOnline.text=[NSString stringWithFormat:@"%@ · acknowledgement unknown · %@",h->online_submitted?@"Submitted":@"Attempted",age(h->now_ms,h->online_attempt_ms)];
    } else self.historyOnline.text=@"No attempt recorded this boot";
    if(h->errfatal_ms && h->errfatal_ms<=h->now_ms)
        self.historyFault.text=[NSString stringWithFormat:@"ERRFATAL at %.3f s · %@",h->errfatal_ms/1000.0,age(h->now_ms,h->errfatal_ms)];
    else if(h->stopped_ms && h->stopped_ms<=h->now_ms)
        self.historyFault.text=[NSString stringWithFormat:@"Stopped at %.3f s · %@",h->stopped_ms/1000.0,age(h->now_ms,h->stopped_ms)];
    else self.historyFault.text=@"No stop or fault recorded";
}
