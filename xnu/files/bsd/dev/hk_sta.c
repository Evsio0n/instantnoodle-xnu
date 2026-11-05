/*
 * Darwin-on-redwood: what stands between the two sides of the station boundary
 * (sys/hk_sta.h).
 *
 * Whoever wants a network asks from wherever it happens to be: an ioctl of wifid's, the thread
 * that brings the radio up. The station side wants to be called by one thread, one call at a
 * time, and a scan or a join takes seconds. So what is asked for is noted here and done by a
 * thread of this file's; the asker returns at once and hears the outcome as an event. One
 * request of each kind can wait: a second scan asked for while the first has not begun
 * replaces it, as does a second join.
 *
 * What the scans heard is kept here, so that it can be read without entering the station
 * side, and so is the list of channels. Not the last scan's alone: wifid scans the bands one at a
 * time (2.4 GHz in 0.6 s, then 5 GHz in 3-4 s, 2026-11-04) and a scan hears a different few each
 * time, so an access point heard in the last wifi_scan_keep seconds (120; 0: the last scan only)
 * stays in what is read, with the age it has -- as a driver's scan cache does: Settings' list had
 * networks missing, and the 5 GHz ones came seconds after the rest.
 *
 * The network the kernel was given in a file is a standing request: joined at the start, and
 * again whenever the radio is turned back on (Settings' switch), until somebody asks for
 * another network -- as iOS itself rejoins the networks it knows. If that other network cannot
 * be joined (a wrong passphrase), the standing one is joined again.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hk_input.h>
#include <sys/hk_sta.h>
#include <sys/systm.h>

extern void thread_set_thread_name(thread_t thread, const char *name);

#define HEARD_MOST      128
#define CHANNELS_MOST   96

static LCK_GRP_DECLARE(sta_lock_group, "hk-sta");
static LCK_MTX_DECLARE(sta_lock, &sta_lock_group);

static const struct hk_sta_ops *sta_ops;   /* the station side, while there is one */
static bool sta_thread_made;
static bool sta_calls;                          /* the thread is in a call of the station side's */
static unsigned sta_reads;                      /* ... and so many others are in its state() */
static bool sta_on;                             /* the radio's power, as last asked for */
static bool sta_held;                           /* the system sleeps: no call is begun (hk_sta_suspend()) */

/* what has been asked for and not begun */
static bool asked_power, asked_power_on, asked_scan, asked_join, asked_leave;
static struct hk_sta_scan asked_scan_what;
static struct hk_sta_join asked_join_what;
static struct hk_sta_join standing;        /* the network joined whenever the radio comes on (ssid_length 0: none) */
static struct hk_sta_join fallback;        /* ... as it was before a join that has not come through yet */

static struct hk_sta_bss *heard;           /* by the scans that came to an end, the last wifi_scan_keep seconds */
static uint64_t heard_at[HEARD_MOST];      /* when each was last heard (ns of uptime) */
static unsigned heard_count;
static uint64_t heard_keep_ns = 120ull * NSEC_PER_SEC;

static uint64_t
sta_now_ns(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}
static struct hk_sta_channel channels[CHANNELS_MOST];
static unsigned channel_count;

static void
heard_one(const struct hk_sta_bss *bss, void *context)
{
	uint64_t now = *(const uint64_t *)context, at = now - (uint64_t)bss->age_ms * NSEC_PER_MSEC;
	unsigned i, oldest = 0;

	/* (only the thread writes the list, and it reads the count without the lock) */
	for (i = 0; i < heard_count && memcmp(heard[i].bssid, bss->bssid, sizeof(bss->bssid)) != 0; i++) {
		oldest = heard_at[i] < heard_at[oldest] ? i : oldest;
	}
	if (i == heard_count && heard_count == HEARD_MOST) {
		i = oldest;                     /* (full: the one heard longest ago gives way) */
	}
	lck_mtx_lock(&sta_lock);
	heard[i] = *bss;
	heard_at[i] = at;
	heard_count += i == heard_count;
	lck_mtx_unlock(&sta_lock);
}

/* Before a scan's are added: those heard longer ago than heard_keep_ns go (all of them with wifi_scan_keep=0). */
static void
heard_forget(uint64_t now)
{
	unsigned kept = 0;

	lck_mtx_lock(&sta_lock);
	for (unsigned i = 0; i < heard_count; i++) {
		if (heard_keep_ns != 0 && now - heard_at[i] <= heard_keep_ns) {
			if (kept != i) {
				heard[kept] = heard[i];
				heard_at[kept] = heard_at[i];
			}
			kept++;
		}
	}
	heard_count = kept;
	lck_mtx_unlock(&sta_lock);
}

static void
sta_runs(__unused void *parameter, __unused wait_result_t result)
{
	static struct hk_sta_scan scan;
	static struct hk_sta_join join;

	for (;;) {
		const struct hk_sta_ops *ops;
		bool power = false, power_on = false, leave = false, do_join = false, do_scan = false;
		int error = 0;

		lck_mtx_lock(&sta_lock);
		while (sta_ops == NULL || sta_held || !(asked_power || asked_leave || asked_join || asked_scan)) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_ops, THREAD_UNINT);
		}
		ops = sta_ops;
		/* one thing at a time, in the order that makes sense when several wait */
		if (asked_power) {
			power = true;
			power_on = asked_power_on;
			asked_power = false;
		} else if (asked_leave) {
			leave = true;
			asked_leave = false;
		} else if (asked_join) {
			do_join = true;
			join = asked_join_what;
			bzero(&asked_join_what, sizeof(asked_join_what));
			asked_join = false;
		} else {
			do_scan = true;
			scan = asked_scan_what;
			asked_scan = false;
		}
		sta_calls = true;
		lck_mtx_unlock(&sta_lock);

		if (power) {
			struct hk_sta_state *state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO);
			bool was_joined = false;

			if (state != NULL) {
				ops->state(state);
				was_joined = state->phase == HK_STA_JOINED;
				kfree_data(state, sizeof(*state));
			}
			error = ops->power(power_on);
			if (!power_on && was_joined) {
				hk_apple80211_event(HK_STA_LINK, 0);
			}
			hk_apple80211_event(HK_STA_POWER, error == 0 ? power_on : !power_on);
			lck_mtx_lock(&sta_lock);
			if (error == 0 && power_on && standing.ssid_length != 0 && !asked_join) {
				asked_join_what = standing;
				asked_join = true;
			}
			lck_mtx_unlock(&sta_lock);
		} else if (leave) {
			ops->leave();
			hk_apple80211_event(HK_STA_LINK, 0);
		} else if (do_join) {
			error = ops->join(&join);
			bzero(&join, sizeof(join));
			hk_apple80211_event(HK_STA_JOIN_DONE, error);
			if (error == 0) {
				hk_apple80211_event(HK_STA_LINK, 1);
			}
			/* a join that did not come through (a wrong passphrase) leaves things as they were: the standing network again */
			lck_mtx_lock(&sta_lock);
			if (error != 0 && fallback.ssid_length != 0 && !asked_join && !asked_leave && !asked_power) {
				standing = asked_join_what = fallback;
				asked_join = true;
			}
			bzero(&fallback, sizeof(fallback));
			lck_mtx_unlock(&sta_lock);
		} else if (do_scan) {
			static unsigned said;
			uint64_t now;
			unsigned kept;

			error = ops->scan(&scan);
			now = sta_now_ns();
			heard_forget(now);
			kept = heard_count;
			ops->heard(heard_one, &now);
			if (said < 30) {
				unsigned low = 0, high = 0, dfs = 0;

				said++;
				for (unsigned i = 0; i < heard_count; i++) {
					low += heard[i].frequency < 4000;
					high += heard[i].frequency >= 4000;
					dfs += heard[i].frequency >= 5250 && heard[i].frequency <= 5730;      /* (channels 52-144) */
				}
				hk_wifi_tell("wifi: a scan of %u channels is over (%d): %u access points to read, %u on 2.4 GHz and %u on 5 GHz "
				    "(%u of them on DFS channels), %u of them kept from the last %llu s\n", scan.frequency_count, error, heard_count,
				    low, high, dfs, kept, heard_keep_ns / NSEC_PER_SEC);
			}
			hk_apple80211_event(HK_STA_SCAN_DONE, error);
		}

		lck_mtx_lock(&sta_lock);
		sta_calls = false;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

/* ---- the station side ---------------------------------------------------------------------------------- */

void
hk_sta_attach(const struct hk_sta_ops *ops)
{
	thread_t thread;

	if (ops == NULL) {
		lck_mtx_lock(&sta_lock);
		sta_ops = NULL;
		asked_power = asked_scan = asked_join = asked_leave = false;
		while (sta_calls || sta_reads != 0) {
			lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_calls, THREAD_UNINT);
		}
		heard_count = channel_count = 0;
		lck_mtx_unlock(&sta_lock);
		return;
	}
	if (heard == NULL) {
		uint32_t keep;

		heard = kalloc_data(HEARD_MOST * sizeof(*heard), Z_WAITOK | Z_ZERO);
		if (PE_parse_boot_argn("wifi_scan_keep", &keep, sizeof(keep))) {
			heard_keep_ns = (uint64_t)keep * NSEC_PER_SEC;
		}
	}
	if (heard == NULL) {
		hk_wifi_tell("wifi: no memory for what a scan hears: nobody can ask the radio for a network\n");
		return;
	}
	if (!sta_thread_made) {
		if (kernel_thread_start(sta_runs, NULL, &thread) != KERN_SUCCESS) {
			hk_wifi_tell("wifi: no thread for the station: nobody can ask the radio for a network\n");
			return;
		}
		thread_set_thread_name(thread, "wifi-station");
		thread_deallocate(thread);
		sta_thread_made = true;
	}
	channel_count = ops->channels(channels, CHANNELS_MOST);
	lck_mtx_lock(&sta_lock);
	sta_ops = ops;
	sta_on = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	hk_apple80211_event(HK_STA_POWER, 1);
}

void
hk_sta_lost(int error)
{
	hk_apple80211_event(HK_STA_LINK, error != 0 ? error : ECONNRESET);
}

/* ---- the system's sleep --------------------------------------------------------------------------------- */

bool
hk_sta_suspend(void)
{
	const struct hk_sta_ops *ops;
	struct hk_sta_state *state;
	bool on, joined = false;

	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return false;
	}
	sta_held = true;
	while (sta_calls) {
		lck_mtx_sleep(&sta_lock, LCK_SLEEP_DEFAULT, (event_t)&sta_calls, THREAD_UNINT);
	}
	ops = sta_ops;
	on = sta_on;
	sta_calls = true;                       /* (the station side is ours until resume: hk_sta_attach(NULL) waits) */
	lck_mtx_unlock(&sta_lock);
	if (on && ops != NULL) {
		if ((state = kalloc_data(sizeof(*state), Z_WAITOK | Z_ZERO)) != NULL) {
			ops->state(state);
			joined = state->phase == HK_STA_JOINED;
			kfree_data(state, sizeof(*state));
		}
		ops->power(false);
		if (joined) {
			hk_apple80211_event(HK_STA_LINK, ENETDOWN);
		}
	}
	lck_mtx_lock(&sta_lock);
	sta_calls = false;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_calls);
	return on;
}

void
hk_sta_resume(bool was_on)
{
	lck_mtx_lock(&sta_lock);
	sta_held = false;
	if (was_on && sta_ops != NULL && sta_on && !asked_power) {
		/* (the power-on joins the standing network again, as after Settings' switch) */
		asked_power = true;
		asked_power_on = true;
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
}

/* ---- who wants a network ------------------------------------------------------------------------------- */

bool
hk_sta_present(void)
{
	return sta_ops != NULL;
}

bool
hk_sta_powered(void)
{
	return sta_ops != NULL && sta_on;
}

bool
hk_sta_busy(void)
{
	bool busy;

	lck_mtx_lock(&sta_lock);
	busy = sta_ops != NULL && (sta_calls || asked_power || asked_leave || asked_join || asked_scan);
	lck_mtx_unlock(&sta_lock);
	return busy;
}

int
hk_sta_ask_power(bool on)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	sta_on = on;
	asked_power = true;
	asked_power_on = on;
	if (!on) {
		asked_scan = asked_join = asked_leave = false;
	}
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hk_sta_ask_scan(const struct hk_sta_scan *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	asked_scan_what = *what;
	asked_scan = true;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hk_sta_ask_join_standing(const struct hk_sta_join *what)
{
	int error = hk_sta_ask_join(what);

	lck_mtx_lock(&sta_lock);
	if (error == 0) {
		standing = *what;
	}
	lck_mtx_unlock(&sta_lock);
	return error;
}

int
hk_sta_ask_join(const struct hk_sta_join *what)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL || !sta_on) {
		lck_mtx_unlock(&sta_lock);
		return sta_ops == NULL ? ENXIO : ENETDOWN;
	}
	if (standing.ssid_length != 0) {
		fallback = standing;
	}
	bzero(&standing, sizeof(standing));     /* (whoever asks for a network now has the say) */
	asked_join_what = *what;
	asked_join = true;
	asked_leave = false;
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

int
hk_sta_ask_leave(void)
{
	lck_mtx_lock(&sta_lock);
	if (sta_ops == NULL) {
		lck_mtx_unlock(&sta_lock);
		return ENXIO;
	}
	asked_leave = true;
	asked_join = false;
	bzero(&asked_join_what, sizeof(asked_join_what));
	bzero(&fallback, sizeof(fallback));
	lck_mtx_unlock(&sta_lock);
	thread_wakeup((event_t)&sta_ops);
	return 0;
}

bool
hk_sta_heard(unsigned index, struct hk_sta_bss *bss)
{
	uint64_t now = sta_now_ns();
	bool there;

	lck_mtx_lock(&sta_lock);
	if ((there = index < heard_count)) {
		*bss = heard[index];
		bss->age_ms = (uint32_t)((now - heard_at[index]) / NSEC_PER_MSEC);
	}
	lck_mtx_unlock(&sta_lock);
	return there;
}

void
hk_sta_state(struct hk_sta_state *state)
{
	const struct hk_sta_ops *ops;

	bzero(state, sizeof(*state));
	lck_mtx_lock(&sta_lock);
	if ((ops = sta_ops) != NULL) {
		sta_reads++;
	}
	lck_mtx_unlock(&sta_lock);
	if (ops != NULL) {
		ops->state(state);
		lck_mtx_lock(&sta_lock);
		sta_reads--;
		lck_mtx_unlock(&sta_lock);
		thread_wakeup((event_t)&sta_calls);
	}
}

unsigned
hk_sta_channels(struct hk_sta_channel *out, unsigned most)
{
	unsigned count;

	lck_mtx_lock(&sta_lock);
	count = channel_count < most ? channel_count : most;
	memcpy(out, channels, count * sizeof(*out));
	lck_mtx_unlock(&sta_lock);
	return count;
}

bool
hk_sta_address(uint8_t *address)
{
	const struct hk_sta_ops *ops = sta_ops;

	if (ops != NULL) {
		memcpy(address, ops->address, 6);
	}
	return ops != NULL;
}
