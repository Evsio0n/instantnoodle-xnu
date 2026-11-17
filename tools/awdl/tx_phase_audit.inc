/* Passive receipts at actual deferred WMI preparation. CPU enqueue time
 * alone does not establish that the command still lies in the peer's AW.
 * Never log addresses, payloads or change a grant/ownership decision here. */
    if (f->tag<=96) {
        uint64_t audit_now=lx_os_nanoseconds()/1000ull;
        lx_os_lock();struct lx_awdl_peer audit_peer=air.table.peers[air.active_slot];lx_os_unlock();
        struct awdl_window audit_window={0};
        bool audit_projected=awdl_peer_project(&audit_peer,audit_now,8,&audit_window);
        lx_tell("awdl-phase: ticket=%u proto=%u peer=%llu deferred_us=%llu clock_age_us=%llu master_self=%u sync_self=%u distance=%u master_counter=%u master_metric=%u physical=%u projected=%u predicted=%u guarded=%u window_left_us=%llu\n",
            f->tag,f->proto,audit_peer.serial,audit_now>=f->send_us?audit_now-f->send_us:0,
            audit_now>=audit_peer.last_rx_us?audit_now-audit_peer.last_rx_us:0,
            !memcmp(audit_peer.master,air.local,6),!memcmp(audit_peer.sync_master,air.local,6),
            audit_peer.distance,audit_peer.master_counter,audit_peer.master_metric,
            channel_of(frequency),audit_projected,audit_window.channel,audit_window.in_guarded_interval,
            audit_window.end_us>audit_now?audit_window.end_us-audit_now:0);
    }
