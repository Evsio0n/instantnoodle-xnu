/*
 * The OnePlus 8's panel in high brightness mode (HBM), for HDR's room above the user's maximum (backlog item 17,
 * 2026-11-04). The panel's own commands are sent by the display's driver (iokit/Kernel/MiniIOMobileFramebuffer.cpp,
 * hk_display_hbm(): the AMB655UV01's qcom,mdss-dsi-hbm-on-command / -off-command from Android's device tree); this
 * file decides when, and for how long. An OLED driven at its peak ages and heats, and here nothing but this file
 * watches it -- no ambient light sensor, no panel temperature, no CoreBrightness thermal input -- so:
 *
 *   - nothing at all unless boot-arg hbm=1 (default 0)
 *   - one period lasts HBM_PERIOD_MAX_S at most, and is followed by HBM_COOLDOWN_S off
 *   - HBM_WINDOW_BUDGET_S of HBM in any HBM_WINDOW_S at most
 *   - refused, and ended at once, when the battery reads HBM_BATTERY_MAX_C or more (or is not known), when the thermal
 *     service's hottest zone reads HBM_HOTTEST_MAX_TENTHS or more (or it does not run), or when its pressure is above
 *     nominal; looked at every HBM_TICK_MS
 *   - ended whenever the screen goes dark, iOS sets a brightness, the system sleeps, and on the way to the loader or
 *     a panic (the display's driver: hk_display_quiesce())
 *
 * Asked for by a test, kern.hackios.hbm (write N: a period of N seconds; 0: off; read: 1 while on), from power_test
 * hbm N; and with boot-arg edr=1 by the brightness itself (hk_hbm_hold(), from MiniIOMobileFramebuffer.cpp's
 * hk_display_set_nits(): CoreBrightness asks for more than the user's maximum while EDR is engaged). A hold that
 * outlasts a period is given another after the cooldown, while the limits allow it (looked at again every
 * HBM_RETRY_S); meanwhile the panel is at the user's maximum, and what CoreAnimation dimmed for the headroom looks
 * dimmer. Every period's start and end is in the log with its reason and the temperatures.
 */
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/hk_input.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

SYSCTL_DECL(_kern_hackios);

#define HBM_PERIOD_MAX_S        30
#define HBM_COOLDOWN_S          60
#define HBM_WINDOW_S            600
#define HBM_WINDOW_BUDGET_S     180
#define HBM_BATTERY_MAX_C       38
#define HBM_HOTTEST_MAX_TENTHS  600
#define HBM_TICK_MS             250
#define HBM_RETRY_S             5

extern void loader_log_putc(char c);
extern bool hk_display_hbm(bool on, bool locked);       /* (MiniIOMobileFramebuffer.cpp) */
extern bool hk_display_hbm_on(void);
extern bool hk_display_on(void);
extern uint32_t hk_display_te_us(void);
extern int hk_thermal_now(int *hottest_tenths);         /* (hk_thermal.c) pressure 0..4; -1: the service is not running */

static TUNABLE(uint32_t, hbm_allowed, "hbm", 0);
static LCK_GRP_DECLARE(hbm_grp, "hk-hbm");
static LCK_MTX_DECLARE(hbm_mtx, &hbm_grp);
static thread_call_t hbm_call, hbm_after_call, hbm_retry_call;
static bool hbm_held;                   /* the brightness asks for it (hk_hbm_hold) */
static unsigned hbm_refusals_said;
static bool hbm_said_during;
static uint64_t hbm_since_ns, hbm_until_ns, hbm_cool_until_ns, hbm_window_ns, hbm_window_used_ns, hbm_periods;

static void
hbm_say(const char *format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

static uint64_t
hbm_now_ns(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

/* Why it may not be on now; NULL: it may. With hbm_mtx held. */
static const char *
hbm_refusal(uint64_t now, int *battery, int *hottest)
{
	int pressure = hk_thermal_now(hottest);

	*battery = hk_battery_temperature();
	if (!hbm_allowed) {
		return "not allowed (boot-arg hbm=1)";
	}
	if (!hk_display_on()) {
		return "the screen is dark";
	}
	if (pressure < 0) {
		return "the thermal service does not run";
	}
	if (pressure > 0) {
		return "the thermal pressure is above nominal";
	}
	if (*hottest >= HBM_HOTTEST_MAX_TENTHS) {
		return "the hottest zone is too hot";
	}
	if (*battery <= -273 || *battery >= HBM_BATTERY_MAX_C) {
		return *battery <= -273 ? "the battery's temperature is not known" : "the battery is too warm";
	}
	if (now < hbm_cool_until_ns) {
		return "cooling down after the last period";
	}
	if (now - hbm_window_ns >= (uint64_t)HBM_WINDOW_S * NSEC_PER_SEC) {
		hbm_window_ns = now;
		hbm_window_used_ns = 0;
	}
	if (hbm_window_used_ns >= (uint64_t)HBM_WINDOW_BUDGET_S * NSEC_PER_SEC) {
		return "the ten minutes' budget is used";
	}
	return NULL;
}

/* With hbm_mtx held: the period ends, for `why'. */
static void
hbm_end(const char *why, int battery, int hottest)
{
	uint64_t now = hbm_now_ns(), held = now - hbm_since_ns;
	bool sent = hk_display_hbm(false, true);

	hbm_window_used_ns += held;
	hbm_cool_until_ns = now + (uint64_t)HBM_COOLDOWN_S * NSEC_PER_SEC;
	hbm_since_ns = 0;
	thread_call_cancel(hbm_call);
	hbm_say("hbm: OFF after %llu ms (%s; the command %s); battery %d C, hottest %d.%d C; TE %u us; %llu s of the ten "
	    "minutes' %u used; cooling down %u s\n", held / NSEC_PER_MSEC, why, sent ? "sent" : "NOT sent", battery, hottest / 10,
	    hottest < 0 ? 0 : hottest % 10, hk_display_te_us(), hbm_window_used_ns / NSEC_PER_SEC, HBM_WINDOW_BUDGET_S,
	    HBM_COOLDOWN_S);
	if (hbm_after_call != NULL) {
		uint64_t deadline;

		clock_interval_to_deadline(2000, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(hbm_after_call, deadline);
	}
	if (hbm_held && hbm_retry_call != NULL) {
		uint64_t deadline;

		clock_interval_to_deadline(HBM_COOLDOWN_S * 1000 + 100, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(hbm_retry_call, deadline);
	}
}

static int hbm_request(int seconds);

/* The hold's next period, once the cooldown is over (or HBM_RETRY_S after a refusal), while it is still asked for */
static void
hbm_retry(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	bool again;

	lck_mtx_lock(&hbm_mtx);
	again = hbm_held && hbm_since_ns == 0;
	lck_mtx_unlock(&hbm_mtx);
	if (again && hbm_request(HBM_PERIOD_MAX_S) != 0) {
		uint64_t deadline;

		lck_mtx_lock(&hbm_mtx);
		if (hbm_held) {
			clock_interval_to_deadline(HBM_RETRY_S * 1000, NSEC_PER_MSEC, &deadline);
			thread_call_enter_delayed(hbm_retry_call, deadline);
		}
		lck_mtx_unlock(&hbm_mtx);
	}
}

/* 2 s after a period: the panel's timing as it was (the supervisor's check that HBM does not disturb it) */
static void
hbm_after(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	hbm_say("hbm: 2 s after: TE %u us, high brightness %s\n", hk_display_te_us(), hk_display_hbm_on() ? "ON" : "off");
}

static void
hbm_tick(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t now = hbm_now_ns(), deadline;
	int battery, hottest;
	const char *why;

	lck_mtx_lock(&hbm_mtx);
	if (hbm_since_ns == 0) {
		lck_mtx_unlock(&hbm_mtx);
		return;
	}
	if (!hk_display_hbm_on()) {
		/* (the display's driver ended it itself: the screen dark, iOS's brightness, the way to the loader) */
		why = "the display ended it";
	} else if (now >= hbm_until_ns) {
		why = "its time is over";
	} else {
		uint64_t keep = hbm_cool_until_ns;

		hbm_cool_until_ns = 0;          /* (the cooldown is the last period's; this one is on) */
		why = hbm_refusal(now, &battery, &hottest);
		hbm_cool_until_ns = keep;
		if (why == NULL && hbm_window_used_ns + (now - hbm_since_ns) >= (uint64_t)HBM_WINDOW_BUDGET_S * NSEC_PER_SEC) {
			why = "the ten minutes' budget is used";
		}
	}
	if (why != NULL) {
		hk_thermal_now(&hottest);
		hbm_end(why, hk_battery_temperature(), hottest);
	} else {
		if (!hbm_said_during && now - hbm_since_ns >= NSEC_PER_SEC) {
			hbm_said_during = true;
			hbm_say("hbm: 1 s in: TE %u us, battery %d C, hottest %d.%d C\n", hk_display_te_us(), battery, hottest / 10,
			    hottest < 0 ? 0 : hottest % 10);
		}
		clock_interval_to_deadline(HBM_TICK_MS, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(hbm_call, deadline);
	}
	lck_mtx_unlock(&hbm_mtx);
}

/* A period of `seconds' (HBM_PERIOD_MAX_S at most); 0: off now. Returns 0, or why not. */
static int
hbm_request(int seconds)
{
	uint64_t now = hbm_now_ns(), deadline;
	int battery = 0, hottest = 0;
	const char *why;

	lck_mtx_lock(&hbm_mtx);
	if (hbm_call == NULL) {
		hbm_call = thread_call_allocate_with_options(hbm_tick, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
		hbm_after_call = thread_call_allocate_with_options(hbm_after, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
		hbm_retry_call = thread_call_allocate_with_options(hbm_retry, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
	}
	if (seconds <= 0) {
		if (hbm_since_ns != 0) {
			hk_thermal_now(&hottest);
			hbm_end("asked off", hk_battery_temperature(), hottest);
		}
		lck_mtx_unlock(&hbm_mtx);
		return 0;
	}
	if (hbm_since_ns != 0) {
		lck_mtx_unlock(&hbm_mtx);
		return EBUSY;
	}
	why = hbm_refusal(now, &battery, &hottest);
	if (why != NULL) {
		if (hbm_refusals_said++ < 20) {
			hbm_say("hbm: refused (%s); battery %d C, hottest %d.%d C\n", why, battery, hottest / 10,
			    hottest < 0 ? 0 : hottest % 10);
		}
		lck_mtx_unlock(&hbm_mtx);
		return EPERM;
	}
	if (!hk_display_hbm(true, true)) {
		hbm_say("hbm: the panel did not take the command\n");
		lck_mtx_unlock(&hbm_mtx);
		return EIO;
	}
	seconds = seconds < HBM_PERIOD_MAX_S ? seconds : HBM_PERIOD_MAX_S;
	hbm_since_ns = hbm_now_ns();
	hbm_until_ns = hbm_since_ns + (uint64_t)seconds * NSEC_PER_SEC;
	hbm_periods++;
	hbm_said_during = false;
	hbm_say("hbm: ON for %d s at most (period %llu); battery %d C, hottest %d.%d C; TE %u us\n", seconds, hbm_periods,
	    battery, hottest / 10, hottest < 0 ? 0 : hottest % 10, hk_display_te_us());
	clock_interval_to_deadline(HBM_TICK_MS, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(hbm_call, deadline);
	lck_mtx_unlock(&hbm_mtx);
	return 0;
}

static int
sysctl_hbm(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = hk_display_hbm_on() ? 1 : 0, error;

	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	return hbm_request(value);
}
SYSCTL_PROC(_kern_hackios, OID_AUTO, hbm, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_hbm, "I",
    "the panel's high brightness mode: write N for a period of N s (30 at most; boot-arg hbm=1, limits in hk_hbm.c), 0 off");

/* The brightness asks for high brightness (on), or no longer (off): MiniIOMobileFramebuffer.cpp, with edr=1. Thread context. */
void
hk_hbm_hold(bool on)
{
	bool changed;

	if (!hbm_allowed) {
		return;
	}
	lck_mtx_lock(&hbm_mtx);
	if (hbm_call == NULL) {
		hbm_call = thread_call_allocate_with_options(hbm_tick, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
		hbm_after_call = thread_call_allocate_with_options(hbm_after, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
		hbm_retry_call = thread_call_allocate_with_options(hbm_retry, NULL, THREAD_CALL_PRIORITY_KERNEL, 0);
	}
	changed = on != hbm_held;
	hbm_held = on;
	lck_mtx_unlock(&hbm_mtx);
	if (!changed) {
		return;
	}
	hbm_say("hbm: the brightness %s\n", on ? "asks for high brightness (EDR)" : "no longer asks for high brightness");
	if (!on) {
		thread_call_cancel(hbm_retry_call);
		hbm_request(0);
	} else if (hbm_request(HBM_PERIOD_MAX_S) != 0) {
		uint64_t deadline;

		clock_interval_to_deadline(HBM_RETRY_S * 1000, NSEC_PER_MSEC, &deadline);
		thread_call_enter_delayed(hbm_retry_call, deadline);
	}
}
