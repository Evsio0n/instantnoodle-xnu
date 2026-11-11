static struct hk_cellular_selection selection = {
    .version = HK_CELLULAR_SELECTION_VERSION,
    .size = sizeof(struct hk_cellular_selection)
};

static void
selection_phase_locked(unsigned phase)
{
    if (phase == HK_OBS_LIVE && selection.phase != phase) {
        selection.platform = (struct hk_selection_report){0};
        selection.software_sub0 = (struct hk_selection_report){0};
    }
    selection.phase = phase;
}

/* Called only at completion of the existing PDC owner's qualified window.
 * A later incomplete or conflicting window supersedes an earlier selection. */
static void
selection_window(unsigned kind, uint64_t checked_ms, bool paired,
                 const struct pn_reply *reply)
{
    if (kind > 1 || !checked_ms)
        return;

    struct hk_selection_report report = {.checked_ms = checked_ms};
    if (paired && reply->ack && reply->ind && !reply->result && !reply->error &&
        reply->ack_candidates && reply->ind_candidates == 1 && !reply->ack_conflicts &&
        !reply->ind_summary_conflicts && !reply->bad_ack && !reply->bad_ind && !reply->bad_length) {
        if (reply->ind_error) {
            report.status = HK_SELECTION_INDICATION_ERROR;
            report.indication_error = reply->ind_error;
        } else if ((!reply->active || reply->active_length) &&
                   (!reply->pending || reply->pending_length)) {
            report.status = HK_SELECTION_COMPLETE;
            report.active_present = reply->active;
            report.pending_present = reply->pending;
        }
    }
    lck_mtx_lock(&obs_lock);
    if (selection.phase == HK_OBS_LIVE) {
        struct hk_selection_report *target = kind ? &selection.software_sub0 : &selection.platform;
        if (target->checked_ms <= checked_ms)
            *target = report;
    }
    lck_mtx_unlock(&obs_lock);
}

static int selection_sysctl SYSCTL_HANDLER_ARGS
{
    struct hk_cellular_selection copy;
    lck_mtx_lock(&obs_lock);
    copy = selection;
    lck_mtx_unlock(&obs_lock);
    copy.now_ms = op_nanoseconds(NULL) / NSEC_PER_MSEC;
    return SYSCTL_OUT(req, &copy, sizeof(copy));
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, cellular_selection, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, selection_sysctl, "S,hk_cellular_selection", "Latest carrier configuration query this boot");
