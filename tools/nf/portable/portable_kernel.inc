/* Private IN2010 test kernel only. One root-owned loopback helper controls
 * a single run. Nothing spawns the modem worker before an explicit Start. */
enum { P_OFF, P_WAIT_PAGE, P_READY, P_START, P_MEASURING, P_FINISHED };
static LCK_MTX_DECLARE(portable_lock, &mdm_lock_group);
static unsigned portable_state, portable_reports, portable_fix;
static unsigned portable_end_published;
static bool portable_reset_ok;
static bool portable_stop, portable_started, portable_stopped, portable_safe;
static char portable_id[34];
static uint64_t portable_latest_start;
static int portable_cable_seen = -2;
/* instantnoodle_battery.c uses this exact locked PM8150B USBIN status byte.
 * Its charge-current fallback is inappropriate for authorizing Start. */
static int portable_plugged(void)
{
	uint8_t status = 0;
	if (hk_spmi_read(0x213, 0x10, &status, 1) != 0) return -1;
	return (status & (1u << 4)) != 0;
}
extern uint64_t portable_usb_state(void);
static void modem_thread(void *, wait_result_t);

static void
portable_receipt(void)
{
	say("portable-gnss-v1 id=%s started=%u complete=%u reports=%u fix=%u end\n",
	    portable_id, portable_started, portable_safe && portable_stopped, portable_reports, portable_fix);
	if (portable_safe && portable_stopped)
		say("portable-teardown-v1 id=%s loc_stop=1 polling_exited=1 warm_reset=1 pins_low=1 end\n", portable_id);
}

static int
portable_control SYSCTL_HANDLER_ARGS
{
	int action = 0, err = 0;
	thread_t worker;
	if (req->newptr) {
		if (req->newlen != sizeof(action)) return EINVAL;
		err = SYSCTL_IN(req, &action, sizeof(action));
		if (err) return err;
	}
	lck_mtx_lock(&portable_lock);
	if (!portable_state) err = ENXIO;
	else {
		int plugged = (portable_state == P_WAIT_PAGE || portable_state == P_READY) ? portable_plugged() : -1;
		uint64_t usb = portable_usb_state();
		if (portable_state == P_READY && plugged != portable_cable_seen) {
			portable_cable_seen = plugged;
			say("portable-usb-v2 id=%s physical=%d usb_event=%u end\n", portable_id, plugged, (unsigned)(usb & 1));
		}
		if (!req->newptr) { /* a status read never admits a measurement */ }
		else
		if (action == 1 && portable_state == P_WAIT_PAGE && plugged == 1 && (usb & 1)) {
			portable_state = P_READY;
			say("portable-ready-v1 id=%s end\n", portable_id);
		} else if (action == 2 && portable_state == P_READY && plugged == 0 && mdm_fix_now() < portable_latest_start) {
			portable_state = P_START;
			if (modem_level == 0) {
				/* Rehearsal: zero modem thread, pins, or QMI requests. */
				__atomic_store_n(&portable_end_published, 1, __ATOMIC_RELEASE);
				portable_state = P_FINISHED;
				say("portable-rehearsal-v1 id=%s completed=1 end\n", portable_id);
			} else if (kernel_thread_start(modem_thread, NULL, &worker) == KERN_SUCCESS) {
				thread_deallocate(worker);
			} else {
				portable_state = P_FINISHED;
				portable_receipt();
				err = EIO;
			}
		} else if (action == 3 && (portable_state == P_START || portable_state == P_MEASURING)) {
			portable_stop = true;
		} else {
			err = (action == 2 && mdm_fix_now() >= portable_latest_start) ? ETIMEDOUT :
			      (action == 2 && plugged == 1) ? EAGAIN : (action == 2 && plugged < 0) ? ENXIO : EBUSY;
			say("portable-control-v2 id=%s action=%d state=%u physical=%d event=%u errno=%d end\n",
			    portable_id, action, portable_state, plugged, (unsigned)(usb & 1), err);
		}
	}
	unsigned state = portable_state;
	/* UI states 6/7/8 mean detached and ready, unreadable, and expired. */
	if (state == P_READY) {
		if (mdm_fix_now() >= portable_latest_start) state = 8;
		else if (portable_cable_seen == 0) state = 6;
		else if (portable_cable_seen == -1) state = 7;
	}
	lck_mtx_unlock(&portable_lock);
	if (err || !req->oldptr) return err;
	return SYSCTL_OUT(req, &state, sizeof(state));
}
SYSCTL_DECL(_kern_hackios);
SYSCTL_PROC(_kern_hackios, OID_AUTO, portable_gnss, CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_LOCKED,
    0, 0, portable_control, "I", "One foreground portable GNSS run");

static int
portable_summary SYSCTL_HANDLER_ARGS
{
	unsigned data[7] = { 1, 0, 0, 0, 0, 0, 0 };
	lck_mtx_lock(&portable_lock);
	data[1] = modem_level == 0;
	data[2] = portable_state == P_FINISHED;
	if (data[2]) {
		data[3] = portable_started;
		data[4] = portable_stopped && portable_safe;
		data[5] = portable_reports;
		data[6] = portable_fix;
	}
	lck_mtx_unlock(&portable_lock);
	return SYSCTL_OUT(req, data, sizeof(data));
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, portable_result, CTLTYPE_OPAQUE|CTLFLAG_RD|CTLFLAG_LOCKED,
    0, 0, portable_summary, "S,portable_result", "Coordinate-free portable outcome");
