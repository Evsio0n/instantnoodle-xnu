"""Compose one private GNSS modem object. Shared project sources and build trees are read-only."""
from pathlib import Path
import shutil
from build_loader import ROOT, HERE, need


def one(source, before, after):
    need(source.count(before) == 1, 'source anchor changed: '+before[:70])
    return source.replace(before, after)


def prepare(directory):
    dev = ROOT/'xnu-src/nf-tree/xnu-8796.121.2/bsd/dev'
    src = (dev/'instantnoodle_modem.c').read_text()
    # Include the capture-vetted GNSS source, keeping other modem experiments isolated.
    original = ROOT.parent/'oneplus8-audit/gnss-location-next/candidate/tools/nf/modem'
    boot = (original/'modem_boot.inc').read_text()
    src = one(src,'extern void loader_log_putc(char c);','extern void loader_log_line(const char *line);')
    src = one(src,"for (const char *c = line; *c != 0; c++) {\n\t\tloader_log_putc(*c);\n\t}", "loader_log_line(line);")
    src = one(src,'stopped = modem_stop();','stopped = modem_stop();\n\t\tportable_reset_ok = stopped;')
    marker = "\n}\n\nvoid\nhk_modem_init"
    src = one(src,marker,"""
	if (__atomic_load_n(&portable_end_published, __ATOMIC_ACQUIRE)) {
		say("portable-ready-v1 id=%s end\\n", portable_id);
		if (modem_level == 0) say("portable-rehearsal-v1 id=%s completed=1 end\\n", portable_id);
		else portable_receipt();
	}"""+marker)
    # Only the generic, acknowledged LOC client may initiate one session.
    a = boot.index('say("modem: qmi loc: generic GNSS start unavailable;')
    b = boot.index('\n}', a)
    boot = boot[:a]+'''say("modem: portable LOC Start was not acknowledged\\n");
	}
	return false;'''+boot[b:]
    a = boot.index('static bool\nmdm_gnss_start(void)')
    b = boot.index('if (mdm_qmi_server)', a)
    boot = boot[:a]+'''static bool
mdm_gnss_start(void)
{
	uint64_t mask = 1u << 0 | 1u << 1 | 1u << 7;
	uint32_t uncertainty_ms = 5000, client_type = 1;
	clock_sec_t seconds;
	clock_usec_t microseconds;
	mdm_gnss_stage = MDM_GNSS_STARTING;
	'''+boot[b:]
    boot = one(boot, 'mdm_qmi_generic = true;', '''mdm_qmi_generic = true;
			mdm_gnss.started_ms = mdm_gnss.last_said_ms = mdm_gnss_ms();
			lck_mtx_lock(&portable_lock);
			portable_started = true;
			portable_state = P_MEASURING;
			lck_mtx_unlock(&portable_lock);''')
    boot = one(boot, 'say("modem: qmi loc: generic GNSS stop accepted', '''portable_stopped = true;
			say("modem: qmi loc: generic GNSS stop accepted''')
    boot = one(boot, 'if (tenths % 10 == 0) mdm_fix_tick();', '''lck_mtx_lock(&portable_lock);
		bool finish = portable_stop;
		lck_mtx_unlock(&portable_lock);
		if (finish) break;
		if (tenths % 10 == 0) mdm_fix_tick();''')
    boot = one(boot, '''if (!generic_started) {
				mdm_loc_ask(MDM_LOC_GET_ENGINE_LOCK);
				mdm_loc_ask(MDM_LOC_GET_OP_MODE);
			}''', 'if (!generic_started) break;')
    boot = one(boot, 'static volatile bool mdm_polling;', 'static volatile bool mdm_polling;\nstatic unsigned portable_poll_exited;')
    boot = one(boot,'\tthread_terminate(current_thread());', '\t__atomic_store_n(&portable_poll_exited, 1, __ATOMIC_RELEASE);\n\tthread_terminate(current_thread());')
    boot = one(boot, 'delay_for_interval(20, NSEC_PER_MSEC);  /* the poll thread out of the device\'s window */',
               'for (unsigned i = 0; i < 100 && !__atomic_load_n(&portable_poll_exited, __ATOMIC_ACQUIRE); i++) delay_for_interval(1, NSEC_PER_MSEC);')
    boot = one(boot, 'if (mdm_gnss.started_ms != 0) {\n\t\tmdm_gnss_stop();', '''if (mdm_gnss.started_ms != 0) {
		lck_mtx_lock(&mdm_fix_lock);
		portable_fix = mdm_fix.accepted > 0;
		portable_reports = mdm_fix.position_reports;
		lck_mtx_unlock(&mdm_fix_lock);
		mdm_gnss_stop();''')
    # Count reports matching the active typed LOC position path only.
    boot = one(boot, 'static void\nmdm_fix_close(void)', (HERE/'portable_kernel.inc').read_text()+'\nstatic void\nmdm_fix_close(void)')
    boot = one(boot, 'say("modem: level 3 is done\\n");', '''portable_safe = mdm_gnss_failure == MDM_GNSS_ERROR_NONE && portable_reset_ok && __atomic_load_n(&portable_poll_exited, __ATOMIC_ACQUIRE) && pcie.quiesced && !instantnoodle_gpio_level(PIN_MDM2AP_STATUS) &&
	    !instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL) && !instantnoodle_gpio_level(PIN_AP2MDM_STATUS) &&
	    !instantnoodle_gpio_level(PIN_PERST);
	lck_mtx_lock(&portable_lock);
	portable_state = P_FINISHED;
	__atomic_store_n(&portable_end_published, 1, __ATOMIC_RELEASE);
	portable_receipt();
	lck_mtx_unlock(&portable_lock);
	say("modem: level 3 is done\\n");''')
    a = src.index('\tthread_t thread;',src.index('\nhk_modem_init(void)'))
    src = src[:a]+'''	uint32_t limit = 0, wifi = 1, radio = 1, gnss = 0, loc = 0, mission = 0;
	if (!instantnoodle_board() || !PE_parse_boot_arg_str("portable_id", portable_id, sizeof(portable_id)) ||
	    strnlen(portable_id, sizeof(portable_id)) != 32 ||
	    !PE_parse_boot_argn("loader_return_after", &limit, sizeof(limit)) ||
	    !PE_parse_boot_argn("modem", &modem_level, sizeof(modem_level)) ||
	    !PE_parse_boot_argn("wifi", &wifi, sizeof(wifi)) || wifi ||
	    !PE_parse_boot_argn("modem_radio", &radio, sizeof(radio)) || radio) return;
	for (unsigned i = 0; i < 32; i++)
		if (!(portable_id[i] >= '0' && portable_id[i] <= '9') &&
		    !(portable_id[i] >= 'a' && portable_id[i] <= 'f')) return;
	PE_parse_boot_argn("modem_gnss", &gnss, sizeof(gnss));
	PE_parse_boot_argn("modem_loc", &loc, sizeof(loc));
	PE_parse_boot_argn("modem_mission", &mission, sizeof(mission));
	if (!((modem_level == 0 && !gnss && !loc && !mission && limit == 300) ||
	      (modem_level == 3 && gnss == 2 && loc == 1 && mission == 1 && limit == 1200))) return;
	/* Absolute boot-relative deadlines reserve time for a full diagnostic and teardown. */
	portable_latest_start = mdm_fix_now() + (limit - (modem_level ? 420u : 30u)) * 1000ULL;
	portable_state = P_WAIT_PAGE;
}
'''
    live=(dev.parents[1]/'osfmk/arm64/loader_return.c').read_text()
    live=one(live,"const void *agent;              /* the toolkit's mailbox (bsd/dev/iphont.c, struct iphont_box) */",
        "const void *agent;\n\tuint64_t portable_magic;\n\tvolatile uint64_t portable_usb;")
    live=one(live,'.agent = &iphont_box,','.agent = &iphont_box, .portable_magic = 0x3154524f504b4848ULL,')
    live=one(live,'void loader_live_beat(void);','''uint64_t portable_usb_state(void);
uint64_t portable_usb_state(void)
{
	return __atomic_load_n(&loader_live.portable_usb, __ATOMIC_ACQUIRE);
}
void loader_live_beat(void);''')
    (directory/'loader_return.c').write_text(live)
    (directory/'instantnoodle_modem.c').write_text(src)
    (directory/'modem_boot.inc').write_text(boot)
    for name in ('gnss_fix.h','gnss_sv_observe.h','mdm_radio_status.h'):
        path = original/name
        shutil.copyfile(path if path.exists() else dev/name,directory/name)
    header=(directory/'gnss_fix.h').read_text()
    header=one(header,'node, port, local, state, accepted;', 'node, port, local, state, accepted, position_reports;')
    header=one(header,'if(status){hk_fix_invalidate', 'if(status <= 1 && s->position_reports < 2147483647U)s->position_reports++;\n    if(status){hk_fix_invalidate')
    (directory/'gnss_fix.h').write_text(header)
