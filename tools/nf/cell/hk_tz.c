/*
 * WHAT TRUSTZONE HAS RUNNING, ASKED (boot-arg tz=N; without it nothing in this file calls TrustZone).
 *
 * Why (the owner, 2026-11-11: 「看看truststore吧」, a key store behind TrustZone): iOS keeps its keys behind the Secure
 * Enclave, which this phone has not got, and the stand-in for AppleKeyStore wraps them with something anybody can undo
 * (docs/KNOWN_ISSUES.md K13). The phone does have Qualcomm's secure world, and in it the secure applications Android's
 * key store is made of. This is the first step towards using one: it asks what is there. docs/research/trustzone-keystore.md.
 *
 * What is known, and from where:
 *   - The phone's own device tree has qseecom@82400000 with qcom,appsbl-qseecom-support and
 *     qcom,commonlib64-loaded-by-uefi: the boot loader loads secure applications before any kernel runs. And
 *     qcom,qsee-reentrancy-support = 2: a call may come back "blocked on a listener", which nothing here answers.
 *   - The boot loader's own source (CodeLinaro's edk2, QcomModulePkg/Library/avb/KeymasterClient.c) starts the
 *     application "keymaster" at every boot, to tell it the root of trust, and asks it its version with command
 *     0x200 (KEYMASTER_GET_VERSION): four bytes in, five words back (status, major, minor, the application's two).
 *   - How a kernel asks, from Linux (drivers/firmware/qcom/qcom_scm.c, CONFIG_QCOM_QSEECOM): the secure monitor call
 *     this tree already makes for an image's signature (hk_adsp.c, owner 2), with another owner:
 *       version   owner 2 (SIP), service 6, command 3, one value (the feature, 10): a number back
 *       lookup    owner 50 (the QSEE's OS), service 1 (application manager), command 3, a buffer with the name and
 *                 its length: result 0 and the type 0xEE01 with the application's id, or a failure (not loaded)
 *       send      owner 48 (the applications), service 0, command 1: the id, a request and its size, a response
 *                 and its size; the fourth and fifth of these in a buffer of their own, as every call of more than
 *                 four arguments has them
 *     Linux allows that interface on a list of laptops only, because it does not answer listeners either.
 *
 * The steps:
 *   tz=1   the version, and the lookup of each name of a short list. Asks only: nothing is loaded or started.
 *   tz=2   and the keymaster's own version, by the command the boot loader sends it at every boot: through a shared-memory
 *          bridge made for it over one page and taken down again (THE SEND, below). This one changes TrustZone's state while
 *          it runs: a bridge is a region TrustZone is told it may share.
 *   tz=3   as 2, and if the bridge is refused, bridges are switched on first (command 0x1c, as the maker's kernel does at
 *          its start) and the bridge is asked for again. From then on, in that boot, TrustZone may want every buffer it
 *          is given to lie in a bridge: only in a test boot that starts nothing else through TrustZone afterwards.
 *   tz=4   as 2, and before the bridge is taken down: A KEY THAT PROTECTS NOTHING (below). The key master is asked to make
 *          an AES key, to encrypt 32 bytes with it and to decrypt them again. This is the first step that makes a key; the
 *          key's wrapped form stays in this kernel's memory and is gone with the boot.
 *   tz=5   as 4, with THE SECURE WORLD'S OWN LOG (below) asked for first and printed at the end: what the key master says
 *          while it works. From then on, in that boot, TrustZone writes its applications' log into 64 KB of ours.
 *   tz=6   as 5, with the key's time of creation given in the request and, if the key master still says it cannot get
 *          the time, THE TIME, SERVED (below): the time listener registered as Android's daemon registers it, its
 *          requests answered from this kernel's clock, and the request that failed made once more.
 *   tz=7   as 6, the key master configured first; and A KEY KEPT FROM AN EARLIER BOOT (below): with one in the root
 *          (/hackios/share/tz-kept) that key decrypts what the earlier boot encrypted with it and no key is made; without
 *          one a key is made as in 6 and printed, wrapped, with what it encrypted, for the computer to keep.
 * And apart from the steps, for use and not for asking: THE ROOT KEY (boot-arg tz_keys=1, at this file's end), which the
 * key store's stand-in wraps the keychain's keys under.
 * Steps 1 to 3 make, store or change no key. All run once, the first time kern.hackios.tz is read, and what they found
 * stays as that sysctl's text and is in the kernel's log. Each call is said in the log two and a half seconds before it
 * is made, so that the read takes about twenty seconds (step 4: about a minute).
 *
 * Buffers are whole pages of ours, zeroed, flushed out of this processor's cache before the call as hk_adsp.c does.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <stdarg.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/random.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <libkern/OSAtomic.h>
#include <vm/vm_kern.h>
#include "hk_keywrap.h"

extern uint64_t kvtophys(vm_offset_t va);
extern int vn_rdwr(enum uio_rw rw, struct vnode *vp, caddr_t base, int len, off_t offset, enum uio_seg segflg, int ioflg, kauth_cred_t cred,
    int *aresid, proc_t p);

/*
 * Every line here goes to the loader's log as well as the kernel's, as hk_adreno.c's do, and for its reason: a run that
 * ends in a reset leaves no kernel log, and the computer keeps what the loader's log said until then. The first run of
 * this file (155603) printed with the kernel's printf alone: the phone reset 17.7 s after the read began, and of what
 * TrustZone had answered by then nothing was left.
 */
extern void loader_log_putc(char c);

static int tz_say(const char *format, ...) __printflike(1, 2);

static int
tz_say(const char *format, ...)
{
	static char line[2100];         /* (every caller holds tz_lock; the last line of a read is the whole text) */
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf tz_say

#define TZ_SMC64                (1ull << 30)
#define TZ_OWNER_SIP            2ull
#define TZ_OWNER_APPS           48ull
#define TZ_OWNER_QSEE_OS        50ull
#define TZ_INTERRUPTED          1
#define TZ_VALUE                0u
#define TZ_BUFFER               2u              /* read and written */
#define TZ_ARGS1(a)             (1u | (a) << 4)
#define TZ_ARGS2(a, b)          (2u | (a) << 4 | (b) << 6)
#define TZ_ARGS3(a, b, c)       (3u | (a) << 4 | (b) << 6 | (c) << 8)
#define TZ_ARGS5(a, b, c, d, e) (5u | (a) << 4 | (b) << 6 | (c) << 8 | (d) << 10 | (e) << 12)
#define TZ_RESULT_APP_ID        0xEE01u
#define KEYMASTER_GET_VERSION   0x200u

/* What a send comes back with when the application has not finished (the maker's kernel: include/soc/qcom/qseecomi.h). */
#define TZ_RESULT_INCOMPLETE    1u              /* it wants a listener served: the listener's number is the data */
#define TZ_RESULT_BLOCKED       2u              /* it waits for a listener that is busy with somebody else */
#define TZ_RESULT_FAILURE       0xffffffffu
#define TZ_SERVICE_LISTENER     2ull
#define TZ_LISTENER_ANSWERED    3ull            /* "the listener has answered", with its number and how it went */
/* Inside the bridge's page: a request at its start with the response behind it, and the arguments past the third at its end. */
#define TZ_REST                 (PAGE_SIZE - 64)
#define TZ_ROOM                 (PAGE_SIZE - 128)

/*
 * The key master's own commands and the tags of their parameters. The numbers of the commands are the boot loader's
 * (KeymasterClient.c), the tags are Android's (hardware/keymaster_defs.h): a type in the top four bits, a number below.
 */
#define KM_GENERATE_KEY         0x108u
#define KM_BEGIN                0x10fu
#define KM_FINISH               0x112u
#define KM_UPGRADE_KEY          0x114u
#define KM_CONFIGURE            0x116u
#define KM_ENUM                 (1u << 28)
#define KM_ENUM_REP             (2u << 28)
#define KM_UINT                 (3u << 28)
#define KM_DATE                 (6u << 28)
#define KM_BOOL                 (7u << 28)
#define KM_BYTES                (9u << 28)
#define KM_TAG_PURPOSE          (KM_ENUM_REP | 1)       /* 0 encrypt, 1 decrypt */
#define KM_TAG_ALGORITHM        (KM_ENUM | 2)           /* 32: AES */
#define KM_TAG_KEY_SIZE         (KM_UINT | 3)
#define KM_TAG_BLOCK_MODE       (KM_ENUM_REP | 4)       /* 32: GCM */
#define KM_TAG_PADDING          (KM_ENUM_REP | 6)       /* 1: none */
#define KM_TAG_MIN_MAC_LENGTH   (KM_UINT | 8)
#define KM_TAG_NO_AUTH_REQUIRED (KM_BOOL | 503)
#define KM_TAG_CREATION_DATETIME (KM_DATE | 701)        /* milliseconds since 1970 */
#define KM_TAG_OS_VERSION       (KM_UINT | 705)
#define KM_TAG_OS_PATCHLEVEL    (KM_UINT | 706)
#define KM_TAG_VENDOR_PATCHLEVEL (KM_UINT | 718)
#define KM_TAG_NONCE            (KM_BYTES | 1001)
#define KM_TAG_MAC_LENGTH       (KM_UINT | 1003)
#define KM_NOT_CONFIGURED       (-64)
#define KM_REQUIRES_UPGRADE     (-62)           /* the key was made under an older version or patch level than the key master has now */
#define KM_NO_HARDWARE          (-49)           /* "secure hardware communication failed": what it says when it cannot get the time */
/* What both of our boot images say in their headers and the boot loader hands the key master at every boot: 16.0.0, 2026-09. */
#define KM_OUR_OS_VERSION       160000u
#define KM_OUR_PATCHLEVEL       202609u
#define KM_OUR_VENDOR_PATCHLEVEL 20260901u

static LCK_GRP_DECLARE(tz_lock_group, "hk_tz");
static LCK_MTX_DECLARE(tz_lock, &tz_lock_group);
static char tz_text[2040];             /* (userland/src/sysctl_text.c reads 2047 bytes) */
static uint32_t tz_pause = 2500;        /* ms between saying a call and making it; the root key's calls, proven by then, do not wait */
static bool tz_asked;
static uint8_t tz_blob[2048];           /* the test key as the key master handed it out: wrapped, of use to it alone */

/* One call. x0 comes back as the status, the three after it as what was answered. */
static int32_t
tz_call(uint64_t owner, uint64_t service, uint64_t command, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third,
    uint64_t more, uint64_t answer[3])
{
	const uint64_t function = TZ_SMC64 | owner << 24 | service << 8 | command;
	uint64_t session = 0;

	printf("tz: about to ask TrustZone: smc %#llx, arguments %#llx: %#llx, %#llx, %#llx, more at %#llx\n", function, arguments, first,
	    second, third, more);
	/* (a call that never comes back leaves no log behind it: the computer copies the kernel's log every two seconds or so, and
	 * this is long enough for the line above to be in a copy before the call is made; run 154140 ended with no last line at all) */
	if (tz_pause != 0) {
		delay_for_interval(tz_pause, NSEC_PER_MSEC);
	}
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : TZ_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = more;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != TZ_INTERRUPTED) {
			answer[0] = x1;
			answer[1] = x2;
			answer[2] = x3;
			printf("tz: TrustZone returns %d: %#llx, %#llx, %#llx\n", (int32_t)x0, x1, x2, x3);
			return (int32_t)x0;
		}
	}
	printf("tz: TrustZone keeps saying the call was interrupted\n");
	return -1;
}

static vm_offset_t
tz_page(void)
{
	vm_offset_t page = 0;

	if (kmem_alloc(kernel_map, &page, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return 0;
	}
	return page;
}

static void
tz_out_of_cache(vm_offset_t page)
{
	for (vm_offset_t line = page; line < page + PAGE_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
}

/*
 * THE SECURE WORLD'S OWN LOG (tz=5). Run 165841 made no key: the key master took the request, said -64 (not configured),
 * took the configuration, and then said -49, "secure hardware communication failed", with no listener asked for and no
 * word of what it had tried to reach. Its own words go to the QSEE's log, which the maker's kernel has TrustZone write
 * into a buffer it hands over at its start (OnePlus's drivers/firmware/qcom/tz_log.c, tzdbg_register_qsee_log_buf: 64 KB
 * of contiguous memory, made a bridge, then owner 50, service 1, command 6 with the buffer and its size; Android reads it
 * as /proc/tzdbg/qsee_log). The same here: 64 KB below 4 GB, a bridge over it, registered; after the key's requests what
 * was written is printed, a line each. The buffer: two 16-bit words (how often it wrapped, where the next byte goes),
 * then the text, a ring. Nothing takes the registration back (the maker's kernel has no call for that either): the
 * memory and its bridge stay TrustZone's to write for the rest of the boot.
 */
#define TZ_LOG_SIZE             0x10000u

static vm_offset_t tz_log;

static void
tz_log_start(void)
{
	uint64_t answer[3] = { 0, 0, 0 };
	vm_offset_t memory = 0;
	uint64_t phys;
	int32_t status;

	if (kmem_alloc_contig(kernel_map, &memory, TZ_LOG_SIZE, PAGE_MASK, (ppnum_t)(atop_64(0x100000000ull) - 1), 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("tz: the secure world's log: no 64 KB below 4 GB, not asked for\n");
		return;
	}
	phys = kvtophys(memory);
	for (vm_offset_t page = memory; page < memory + TZ_LOG_SIZE; page += PAGE_SIZE) {
		tz_out_of_cache(page);
	}
	printf("tz: the secure world's log: 64 KB at %#llx\n", phys);
	status = tz_call(TZ_OWNER_SIP, 0x0c, 0x1e, 4, phys | 6, phys | 6, TZ_LOG_SIZE | 1u << 9, 3, answer);
	if (status != 0 || answer[0] != 0) {
		printf("tz: the secure world's log: no bridge (status %d, result %#llx), not asked for\n", status, answer[0]);
		return;
	}
	printf("tz: the secure world's log: its bridge's handle %#llx\n", answer[1]);
	answer[0] = answer[1] = answer[2] = 0;
	status = tz_call(TZ_OWNER_QSEE_OS, 1, 6, TZ_ARGS2(TZ_BUFFER, TZ_VALUE), phys, TZ_LOG_SIZE, 0, 0, answer);
	printf("tz: the secure world's log: registered: status %d, result %#llx\n", status, answer[0]);
	if (status == 0 && answer[0] == 0) {
		tz_log = memory;
	}
}

static void
tz_log_print(void)
{
	static char line[240];
	const uint8_t *text = (const uint8_t *)tz_log + 4;
	const uint32_t room = TZ_LOG_SIZE - 4;
	uint32_t wrap, next, from, count, most = 28 * 1024;
	unsigned used = 0;

	if (tz_log == 0) {
		return;
	}
	for (vm_offset_t page = tz_log; page < tz_log + TZ_LOG_SIZE; page += PAGE_SIZE) {
		tz_out_of_cache(page);
	}
	wrap = *(const uint16_t *)tz_log;
	next = *(const uint16_t *)(tz_log + 2);
	if (next >= room) {
		printf("tz: the secure world's log: its place, %u, is past its end\n", next);
		return;
	}
	from = wrap == 0 ? 0 : (next + 1) % room;
	count = wrap == 0 ? next : room - 1;
	if (count > most) {
		from = (from + count - most) % room;
		count = most;
	}
	printf("tz: the secure world's log: wrapped %u times, at %u; its last %u bytes:\n", wrap, next, count);
	for (uint32_t i = 0; i <= count; i++) {
		const uint8_t c = i < count ? text[(from + i) % room] : '\n';

		if (c == '\n' || c == '\r' || c == 0 || used == sizeof(line) - 1) {
			if (used != 0) {
				line[used] = 0;
				printf("tz: qsee: %s\n", line);
				used = 0;
			}
			if (c == '\n' || c == '\r' || c == 0) {
				continue;
			}
		}
		line[used++] = c >= 0x20 && c < 0x7f ? (char)c : '.';
	}
}

/*
 * A KEY THAT PROTECTS NOTHING (tz=4): what the key master is asked, and how it is written.
 *
 * The layouts are those this phone's own Android sends, read out of its key store library (vendor/lib64/
 * libqtikeymaster4.so with libkeymasterdeviceutils.so, from the copy of the phone's vendor partition; no vendor code is
 * in this file). That library asks the key master's version first and, for one that says 4.0 as this phone's does
 * (run 163458), takes its "legacy" path: the boot loader's command numbers (0x100 + ...) and a layout of Qualcomm's own.
 * It is not the layout of Android's reference key master, which a public client for another phone assumed and never saw
 * answered (github.com/lawik/qsee_keymaster).
 *
 *   A list of parameters: twelve bytes each, the tag and then, by the tag's type, a value of four bytes, of eight, one
 *   byte for a flag, or for bytes where they lie, counted from the list's start, and how many. The bytes themselves come
 *   behind the last entry.
 *   Generate (0x108): the command, where the list starts (12), how many entries; the list. Back: a status, a word, where
 *   the wrapped key lies in the response and how long it is.
 *   Begin (0x10f): the command, the purpose, where the wrapped key lies and its length, where the list starts and its
 *   count; the key, the list. Back: a status, a word, the operation's handle (eight bytes), a word, sixteen bytes for a
 *   nonce the key master chose, and how many of them count.
 *   Finish (0x112): the command, the handle, a list's place and count, the input's place and length, a signature's place
 *   and length. The response is prepared by the sender: where the output may go (16) and how much room there is. Back:
 *   a status, and the output's place and length filled in.
 *   Configure (0x116): as generate, with the system's version and patch levels: Android sends it once at its start.
 *   Here it is only sent if the key master says it has not been configured (-64), with the numbers of our boot images'
 *   headers, which are what the boot loader gave it.
 *
 * LISTENERS. A secure application may stop in the middle and ask this side for something: storage (RPMB 0x2000, a file
 * system 0xa and 0x7000), the time (0xb). On Android a daemon serves those (qseecomd; this phone's starts ten of them)
 * and the key store library waits for it before it sends anything. Here nobody is registered. What the maker's kernel
 * does when a listener is asked for that does not exist (OnePlus's qseecom.c, __qseecom_reentrancy_process_incomplete_cmd:
 * "Listener Svc %d does not exist") is done here too: the request is answered "failed" (owner 50, service 2, command 3:
 * the listener's number, 0xffffffff) and the application goes on or gives up by itself. A send that is left at
 * "incomplete" with no answer keeps the application waiting until the phone restarts, which is what happened to the
 * client above. "Blocked on a listener" cannot be answered here and ends this step; nothing more is then sent.
 */
struct tz_param {
	uint32_t        tag;
	uint64_t        value;          /* for bytes: how many */
	const void      *bytes;
};

static uint32_t
tz_params(uint8_t *at, const struct tz_param *list, uint32_t count)
{
	uint32_t behind = 12 * count;

	for (uint32_t i = 0; i < count; i++) {
		uint8_t *entry = at + 12 * i;
		const uint32_t type = list[i].tag >> 28;

		bzero(entry, 12);
		memcpy(entry, &list[i].tag, 4);
		if (type == 8 || type == 9) {
			const uint32_t length = (uint32_t)list[i].value;

			memcpy(entry + 4, &behind, 4);
			memcpy(entry + 8, &length, 4);
			memcpy(at + behind, list[i].bytes, length);
			behind += length;
		} else if (type == 5 || type == 6 || type == 10) {
			memcpy(entry + 4, &list[i].value, 8);
		} else if (type == 7) {
			entry[4] = list[i].value != 0;
		} else {
			const uint32_t value = (uint32_t)list[i].value;

			memcpy(entry + 4, &value, 4);
		}
	}
	return behind;
}

static uint32_t
tz_word(const uint8_t *at)
{
	uint32_t word;

	memcpy(&word, at, 4);
	return word;
}

static void
tz_put(uint8_t *at, uint32_t word)
{
	memcpy(at, &word, 4);
}

/* Where the response to a request of this many bytes is put. */
static uint32_t
tz_behind(uint32_t request)
{
	return (request + 63) & ~63u;
}

static const char *
tz_listener(uint64_t number)
{
	switch (number) {
	case 0xa: return "the file system";
	case 0xb: return "the time";
	case 0x1100: return "GP request cancellation";
	case 0x2000: return "RPMB";
	case 0x3000: return "SSD";
	case 0x4000: return "the secure UI";
	case 0x7000: return "the GP file system";
	case 0x9000: return "interrupts";
	case 0xb000: return "the secure processor";
	case 0xc000: return "output protection";
	default: return "not one of this phone's daemon";
	}
}

/*
 * THE TIME, SERVED (tz=6). Run 170355 had the secure world's log say why no key was made: the key master read every
 * parameter, then "get_current_time: -1", "add_kmta_origin_tags: -49". It wants the time of day for the key's record,
 * asks the time listener (0xb), and with none registered TrustZone turns the request down inside, without a word to this
 * side. The listener, as this phone's daemon keeps it (libdrmtime.so, read from the vendor partition's copy): a shared
 * buffer of 20480 bytes; a request is a command in its first word, the answer is written over it, its first word left 0:
 *   0x302  the seconds since 1970 at 4, twelve bytes back in all sixteen
 *   0x303  the same as a calendar date: nine words at 4 (second, minute, hour, day, month from 0, year from 1900, weekday,
 *          day of the year, 0), then 0 for "it worked": 44 bytes
 *   0x304  milliseconds since 1970, eight bytes at 4: 12 bytes
 *   0x305  seconds at 4, nanoseconds at 8, 0 for "it worked" at 12: 16 bytes
 *   anything else: -1 at 4
 * Registered as the maker's kernel registers one (qseecom.c, QSEOS_REGISTER_LISTENER: owner 50, service 2, command 6
 * with the number, the buffer and its size, and command 1 if TrustZone does not know 6), over memory made a bridge first.
 * A request arrives as a send that comes back "incomplete" with the listener's number; the answer is written and
 * "the listener has answered" called with 0. The registration is not taken back: it lasts for the boot.
 *
 * Run 170955 (tz=6): the time of creation in the request changes nothing, the key master asks all the same; command 6
 * is taken; one request arrives, 0x302; with it answered the key is made (455 bytes wrapped), 32 bytes come back as 48
 * and those as the 32 again. Begin and finish ask for nothing.
 */
#define TZ_TIME_LISTENER        0xbull
#define TZ_TIME_ROOM            20480u

static vm_offset_t tz_time;
static unsigned tz_time_asked;

static bool
tz_time_start(void)
{
	uint64_t answer[3] = { 0, 0, 0 };
	vm_offset_t memory = 0;
	uint64_t phys;
	int32_t status;

	if (tz_time != 0) {
		return false;                   /* (it is there already: asking again changes nothing) */
	}
	if (kmem_alloc_contig(kernel_map, &memory, 2 * PAGE_SIZE, PAGE_MASK, (ppnum_t)(atop_64(0x100000000ull) - 1), 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("tz: the time listener: no memory below 4 GB, not registered\n");
		return false;
	}
	phys = kvtophys(memory);
	tz_out_of_cache(memory);
	tz_out_of_cache(memory + PAGE_SIZE);
	printf("tz: the time listener: its buffer at %#llx\n", phys);
	status = tz_call(TZ_OWNER_SIP, 0x0c, 0x1e, 4, phys | 6, phys | 6, 2 * PAGE_SIZE | 1u << 9, 3, answer);
	if (status != 0 || answer[0] != 0) {
		printf("tz: the time listener: no bridge (status %d, result %#llx), not registered\n", status, answer[0]);
		return false;
	}
	answer[0] = answer[1] = answer[2] = 0;
	status = tz_call(TZ_OWNER_QSEE_OS, TZ_SERVICE_LISTENER, 6, TZ_ARGS3(TZ_VALUE, TZ_BUFFER, TZ_VALUE), TZ_TIME_LISTENER, phys,
	    TZ_TIME_ROOM, 0, answer);
	if (status != 0) {
		printf("tz: the time listener: command 6 is not known here (status %d), command 1 instead\n", status);
		answer[0] = answer[1] = answer[2] = 0;
		status = tz_call(TZ_OWNER_QSEE_OS, TZ_SERVICE_LISTENER, 1, TZ_ARGS3(TZ_VALUE, TZ_BUFFER, TZ_VALUE), TZ_TIME_LISTENER, phys,
		    TZ_TIME_ROOM, 0, answer);
	}
	printf("tz: the time listener: registered: status %d, result %#llx\n", status, answer[0]);
	if (status != 0 || answer[0] != 0) {
		return false;
	}
	tz_time = memory;
	return true;
}

static void
tz_time_serve(void)
{
	uint8_t *buffer = (uint8_t *)tz_time;
	clock_sec_t seconds = 0;
	clock_usec_t micro = 0;
	uint32_t command;

	tz_out_of_cache(tz_time);
	command = *(const uint32_t *)tz_time;
	clock_get_calendar_microtime(&seconds, &micro);
	printf("tz: the time listener is asked %#x (%08x %08x %08x); it is %lu s since 1970 here\n", command,
	    *(const uint32_t *)(tz_time + 4), *(const uint32_t *)(tz_time + 8), *(const uint32_t *)(tz_time + 12), (unsigned long)seconds);
	tz_time_asked++;
	bzero(buffer, 64);
	if (command == 0x302) {
		*(uint32_t *)(tz_time + 4) = (uint32_t)seconds;
	} else if (command == 0x303) {
		/* (days to a date: Howard Hinnant's civil_from_days, the year starting in March) */
		const uint64_t days = (uint64_t)seconds / 86400, left = (uint64_t)seconds % 86400;
		const uint64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
		const uint64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
		const uint64_t mp = (5 * doy + 2) / 153, day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9;
		const uint64_t year = yoe + era * 400 + (month <= 2);
		const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
		const uint64_t in_year = month > 2 ? doy + 59 + leap : doy - 306;
		uint32_t *words = (uint32_t *)(tz_time + 4);

		words[0] = (uint32_t)(left % 60);
		words[1] = (uint32_t)(left / 60 % 60);
		words[2] = (uint32_t)(left / 3600);
		words[3] = (uint32_t)day;
		words[4] = (uint32_t)(month - 1);
		words[5] = (uint32_t)(year - 1900);
		words[6] = (uint32_t)((days + 4) % 7);
		words[7] = (uint32_t)in_year;
	} else if (command == 0x304) {
		const uint64_t milli = (uint64_t)seconds * 1000;

		memcpy(buffer + 4, &milli, 8);
	} else if (command == 0x305) {
		*(uint32_t *)(tz_time + 4) = (uint32_t)seconds;
		*(uint32_t *)(tz_time + 8) = (uint32_t)micro * 1000;
	} else {
		*(uint32_t *)(tz_time + 4) = 0xffffffffu;
	}
	tz_out_of_cache(tz_time);
}

static bool tz_stuck;           /* an application was left waiting: nothing more is sent in this boot */
static uint64_t tz_wanted[8];   /* the listeners that were asked for, in order */
static unsigned tz_wants;

/*
 * One request to a secure application, the request already written at the start of the bridge's page. True: the
 * application finished, and what it says is in the response (its first word is its own status).
 */
static bool
tz_send(uint32_t application, vm_offset_t shared, uint32_t request, const char *what)
{
	const uint64_t phys = kvtophys(shared);
	const uint32_t response = tz_behind(request);
	uint64_t *rest = (uint64_t *)(shared + TZ_REST);
	uint64_t answer[3] = { 0, 0, 0 };
	int32_t status;

	if (tz_stuck || response + 64 > TZ_ROOM) {
		printf("tz: %s: not sent (%s)\n", what, tz_stuck ? "an application was left waiting" : "no room for it");
		return false;
	}
	rest[0] = phys + response;
	rest[1] = TZ_ROOM - response;
	tz_out_of_cache(shared);
	printf("tz: %s: %u bytes to application %u, its response %u bytes further on\n", what, request, application, response);
	status = tz_call(TZ_OWNER_APPS, 0, 1, TZ_ARGS5(TZ_VALUE, TZ_BUFFER, TZ_VALUE, TZ_BUFFER, TZ_VALUE), application, phys, request,
	    phys + TZ_REST, answer);
	for (int round = 0; status == 0 && answer[0] == TZ_RESULT_INCOMPLETE && round < 16; round++) {
		const uint64_t listener = answer[2];
		const bool served = listener == TZ_TIME_LISTENER && tz_time != 0;

		if (served) {
			tz_time_serve();
		} else {
			printf("tz: %s: the application asks for listener %#llx (%s); nobody serves it here: answered as failed\n", what, listener,
			    tz_listener(listener));
			if (tz_wants < sizeof(tz_wanted) / sizeof(tz_wanted[0])) {
				tz_wanted[tz_wants++] = listener;
			}
		}
		answer[0] = answer[1] = answer[2] = 0;
		status = tz_call(TZ_OWNER_QSEE_OS, TZ_SERVICE_LISTENER, TZ_LISTENER_ANSWERED, TZ_ARGS2(TZ_VALUE, TZ_VALUE), listener,
		    served ? 0 : TZ_RESULT_FAILURE, 0, 0, answer);
	}
	/* (the response was written behind this processor's back: whatever it still holds of the page is dropped) */
	tz_out_of_cache(shared);
	if (status != 0 || answer[0] != 0) {
		tz_stuck = status != 0 || answer[0] == TZ_RESULT_INCOMPLETE || answer[0] == TZ_RESULT_BLOCKED;
		printf("tz: %s: the application did not finish: status %d, result %#llx (%s), type %#llx, data %#llx%s\n", what, status,
		    answer[0], answer[0] == TZ_RESULT_BLOCKED ? "blocked on a listener" : answer[0] == TZ_RESULT_INCOMPLETE ?
		    "still asking for a listener" : "a failure", answer[1], answer[2], tz_stuck ? "; nothing more is sent" : "");
		return false;
	}
	return true;
}

/* Begin and finish in one: the purpose, the parameters of the operation, the input. The output's length, or -1. */
static int
tz_crypt(uint32_t keymaster, vm_offset_t shared, uint32_t key, uint32_t purpose, const struct tz_param *list, uint32_t count,
    const uint8_t *input, uint32_t length, uint8_t *output, uint32_t room, uint8_t nonce[16], uint32_t *nonce_length, int32_t *why)
{
	uint8_t *page = (uint8_t *)shared;
	const char *what = purpose == 0 ? "begin to encrypt" : "begin to decrypt";
	const uint8_t *response;
	uint64_t handle;
	uint32_t request, produced, where;

	bzero(page, PAGE_SIZE);
	tz_put(page, KM_BEGIN);
	tz_put(page + 4, purpose);
	tz_put(page + 8, 24);
	tz_put(page + 12, key);
	memcpy(page + 24, tz_blob, key);
	tz_put(page + 16, 24 + key);
	tz_put(page + 20, count);
	request = 24 + key + tz_params(page + 24 + key, list, count);
	if (!tz_send(keymaster, shared, request, what)) {
		return -1;
	}
	response = page + tz_behind(request);
	memcpy(&handle, response + 8, 8);
	printf("tz: %s: the key master answers status %d, handle %#llx, %u, a nonce of %u bytes\n", what, (int32_t)tz_word(response),
	    handle, tz_word(response + 16), tz_word(response + 36));
	*why = (int32_t)tz_word(response);
	if (tz_word(response) != 0) {
		return -1;
	}
	if (nonce_length != NULL) {
		*nonce_length = tz_word(response + 36) <= 16 ? tz_word(response + 36) : 0;
		memcpy(nonce, response + 20, 16);
	}

	what = purpose == 0 ? "finish encrypting" : "finish decrypting";
	bzero(page, PAGE_SIZE);
	tz_put(page, KM_FINISH);
	memcpy(page + 4, &handle, 8);
	tz_put(page + 20, 36);
	tz_put(page + 24, length);
	memcpy(page + 36, input, length);
	request = 36 + length;
	/* (the sender says where in the response the output may go and how much room it has there) */
	response = page + tz_behind(request);
	tz_put(page + tz_behind(request) + 8, 16);
	tz_put(page + tz_behind(request) + 12, TZ_ROOM - tz_behind(request) - 16);
	if (!tz_send(keymaster, shared, request, what)) {
		return -1;
	}
	where = tz_word(response + 8);
	produced = tz_word(response + 12);
	printf("tz: %s: the key master answers status %d, %u bytes of output at %u\n", what, (int32_t)tz_word(response), produced, where);
	*why = (int32_t)tz_word(response);
	if (tz_word(response) != 0 || produced > room || where > TZ_ROOM || tz_behind(request) + where + produced > TZ_ROOM) {
		return -1;
	}
	memcpy(output, response + where, produced);
	return (int)produced;
}

/*
 * What Android's key store service sends once at its start, with our boot images' numbers. The key master's status.
 * Boot-arg tz_patch=YYYYMM, for a test of the upgrade and nothing else: that patch level instead (and its first day as the
 * vendor's). A real change of these numbers is KM_OUR_* above together with the boot images' headers.
 */
static uint32_t tz_patch;

static int32_t
tz_configure(uint32_t keymaster, vm_offset_t shared)
{
	struct tz_param ours[] = {
		{ KM_TAG_OS_VERSION, KM_OUR_OS_VERSION, NULL }, { KM_TAG_OS_PATCHLEVEL, KM_OUR_PATCHLEVEL, NULL },
		{ KM_TAG_VENDOR_PATCHLEVEL, KM_OUR_VENDOR_PATCHLEVEL, NULL },
	};
	uint8_t *page = (uint8_t *)shared;
	uint32_t request;

	tz_patch = KM_OUR_PATCHLEVEL;
	if (PE_parse_boot_argn("tz_patch", &tz_patch, sizeof(tz_patch)) && tz_patch >= 200001 && tz_patch <= 209912) {
		ours[1].value = tz_patch;
		ours[2].value = (uint64_t)tz_patch * 100 + 1;
		printf("tz: configure: a test's patch level, %u, in place of %u\n", tz_patch, KM_OUR_PATCHLEVEL);
	} else {
		tz_patch = KM_OUR_PATCHLEVEL;
	}
	bzero(page, PAGE_SIZE);
	tz_put(page, KM_CONFIGURE);
	tz_put(page + 4, 12);
	tz_put(page + 8, 3);
	request = 12 + tz_params(page + 12, ours, 3);
	if (!tz_send(keymaster, shared, request, "configure")) {
		return INT32_MIN;
	}
	printf("tz: configure: the key master answers status %d\n", (int32_t)tz_word(page + tz_behind(request)));
	return (int32_t)tz_word(page + tz_behind(request));
}

/*
 * A KEY KEPT FROM AN EARLIER BOOT (tz=7). Run 170955 made a key and used it inside one boot. What a key store is for is
 * the next boot: the wrapped key lies on the disk, and after a restart only this phone's key master can use it. Whether
 * it can is the question (a wrapped key carries the system's version and patch levels, and whatever the boot loader told
 * the key master of the boot's state). Asked without writing to the phone: the boot that makes the key prints it,
 * wrapped, together with the nonce and the 48 bytes its first encryption gave ("tz: keep ..." lines: a record of four
 * words, "TZK1" and the three lengths, then the three pieces); the computer makes a file of those lines
 * (tools/nf/cell/tz_keep.py) and puts it into the next boot's root; that boot reads the file here and asks the key
 * master to decrypt the 48 bytes with the key: the 32 bytes of the first boot, or not.
 * The key protects nothing and its wrapped form is of use to this phone's key master alone; it is in the run's log and
 * in out/ on the computer, and in no repository.
 *
 * Runs 171750 (made and printed) and 172147 (read and used, after the phone had been through its boot loader again): the
 * key master, configured with the same numbers, decrypts the earlier boot's 48 bytes to its 32. No listener is asked
 * for when a key is only used.
 */
#define TZ_KEPT                 "/hackios/share/tz-kept"
#define TZ_KEPT_MAGIC           0x314b5a54u     /* "TZK1" */

static uint8_t tz_record[sizeof(tz_blob) + 96];

static uint32_t
tz_kept_read(void)
{
	vfs_context_t context = vfs_context_current();
	vnode_t file = NULLVP;
	int left = 0, error;

	if (vnode_open(TZ_KEPT, FREAD, 0, 0, &file, context) != 0) {
		return 0;
	}
	error = vn_rdwr(UIO_READ, file, (caddr_t)tz_record, (int)sizeof(tz_record), 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(context),
	    &left, vfs_context_proc(context));
	vnode_close(file, FREAD, context);
	return error == 0 ? (uint32_t)(sizeof(tz_record) - (size_t)left) : 0;
}

static void
tz_keep_print(uint32_t key, const uint8_t *nonce, uint32_t nonce_length, const uint8_t *sealed, uint32_t sealed_length)
{
	static char hex[2 * 40 + 1];
	uint32_t length = 16, sum = 0;

	if (key > sizeof(tz_blob) || nonce_length > 16 || sealed_length > 64) {
		return;
	}
	tz_put(tz_record, TZ_KEPT_MAGIC);
	tz_put(tz_record + 4, key);
	tz_put(tz_record + 8, nonce_length);
	tz_put(tz_record + 12, sealed_length);
	memcpy(tz_record + length, tz_blob, key);
	length += key;
	memcpy(tz_record + length, nonce, nonce_length);
	length += nonce_length;
	memcpy(tz_record + length, sealed, sealed_length);
	length += sealed_length;
	for (uint32_t i = 0; i < length; i += 40) {
		const uint32_t count = length - i < 40 ? length - i : 40;

		for (uint32_t j = 0; j < count; j++) {
			snprintf(hex + 2 * j, 3, "%02x", tz_record[i + j]);
		}
		printf("tz: keep %04u: %s\n", i, hex);
	}
	for (uint32_t i = 0; i < length; i++) {
		sum = sum * 31 + tz_record[i];
	}
	printf("tz: keep end: %u bytes, sum %08x\n", length, sum);
}

static size_t
tz_key(uint32_t level, uint32_t keymaster, vm_offset_t shared, char *text, size_t size)
{
	struct tz_param make[] = {
		{ KM_TAG_ALGORITHM, 32, NULL }, { KM_TAG_KEY_SIZE, 256, NULL }, { KM_TAG_PURPOSE, 0, NULL }, { KM_TAG_PURPOSE, 1, NULL },
		{ KM_TAG_BLOCK_MODE, 32, NULL }, { KM_TAG_PADDING, 1, NULL }, { KM_TAG_MIN_MAC_LENGTH, 128, NULL },
		{ KM_TAG_NO_AUTH_REQUIRED, 1, NULL }, { KM_TAG_CREATION_DATETIME, 0, NULL },      /* (the last from tz=6 on) */
	};
	const uint32_t entries = sizeof(make) / sizeof(make[0]) - (level >= 6 ? 0 : 1);
	clock_sec_t seconds = 0;
	clock_usec_t micro = 0;
	int32_t why = 0;
	uint8_t *page = (uint8_t *)shared;
	uint8_t plain[32], sealed[64], opened[64], nonce[16] = { 0 };
	const uint8_t *response = page;
	uint32_t request = 0, key = 0, where, nonce_length = 0;
	int32_t status = -1;
	int sealed_length, opened_length;
	size_t at = 0;

	for (unsigned i = 0; i < sizeof(plain); i++) {
		plain[i] = (uint8_t)(0xa0 + i);
	}
	if (level >= 7) {
		uint32_t kept;

		status = tz_configure(keymaster, shared);
		at += (size_t)snprintf(text + at, size - at, " configure (%u, %u, %u): status %d;", KM_OUR_OS_VERSION, KM_OUR_PATCHLEVEL,
		    KM_OUR_VENDOR_PATCHLEVEL, status);
		if (status != 0) {
			goto out;
		}
		kept = tz_kept_read();
		if (kept != 0) {
			const uint32_t nonce_kept = tz_word(tz_record + 8), sealed_kept = tz_word(tz_record + 12);

			key = tz_word(tz_record + 4);
			printf("tz: a key kept from an earlier boot: %u bytes read: %08x, a key of %u, a nonce of %u, %u bytes it encrypted\n", kept,
			    tz_word(tz_record), key, nonce_kept, sealed_kept);
			if (kept < 16 || tz_word(tz_record) != TZ_KEPT_MAGIC || key == 0 || key > sizeof(tz_blob) || nonce_kept > 16 ||
			    sealed_kept > sizeof(sealed) || kept != 16 + key + nonce_kept + sealed_kept) {
				at += (size_t)snprintf(text + at, size - at, " a key kept from an earlier boot: %u bytes that are not a record; nothing asked;",
				    kept);
				goto out;
			}
			memcpy(tz_blob, tz_record + 16, key);
			memcpy(nonce, tz_record + 16 + key, nonce_kept);
			memcpy(sealed, tz_record + 16 + key + nonce_kept, sealed_kept);
			{
				const struct tz_param open[] = {
					{ KM_TAG_BLOCK_MODE, 32, NULL }, { KM_TAG_PADDING, 1, NULL }, { KM_TAG_MAC_LENGTH, 128, NULL },
					{ KM_TAG_NONCE, nonce_kept, nonce },
				};

				opened_length = tz_crypt(keymaster, shared, key, 1, open, 4, sealed, sealed_kept, opened, sizeof(opened), NULL, NULL, &why);
				if (opened_length < 0 && why == KM_NO_HARDWARE && tz_time_start()) {
					at += (size_t)snprintf(text + at, size - at, " decrypting: status -49; the time listener registered;");
					opened_length = tz_crypt(keymaster, shared, key, 1, open, 4, sealed, sealed_kept, opened, sizeof(opened), NULL, NULL,
					    &why);
				}
			}
			if (opened_length < 0) {
				at += (size_t)snprintf(text + at, size - at, " a key kept from an earlier boot (%u bytes): decrypting what that boot encrypted "
				    "FAILED (%d);", key, why);
			} else {
				at += (size_t)snprintf(text + at, size - at, " a key kept from an earlier boot (%u bytes): decrypting what that boot encrypted: "
				    "%d bytes out, %s;", key, opened_length, opened_length == 32 && memcmp(opened, plain, 32) == 0 ?
				    "the 32 bytes that boot put in" : "NOT what that boot put in");
			}
			goto out;
		}
		printf("tz: no key kept from an earlier boot (%s): one is made\n", TZ_KEPT);
	}
	clock_get_calendar_microtime(&seconds, &micro);
	make[sizeof(make) / sizeof(make[0]) - 1].value = (uint64_t)seconds * 1000;
	for (int attempt = 0; attempt < 3; attempt++) {
		bzero(page, PAGE_SIZE);
		tz_put(page, KM_GENERATE_KEY);
		tz_put(page + 4, 12);
		tz_put(page + 8, entries);
		request = 12 + tz_params(page + 12, make, entries);
		if (!tz_send(keymaster, shared, request, "make a key")) {
			at += (size_t)snprintf(text + at, size - at, " a key: the key master did not finish;");
			goto out;
		}
		response = page + tz_behind(request);
		status = (int32_t)tz_word(response);
		printf("tz: make a key: the key master answers status %d, %u, the key at %u, %u bytes\n", status, tz_word(response + 4),
		    tz_word(response + 8), tz_word(response + 12));
		if (status == KM_NO_HARDWARE && level >= 6 && tz_time_start()) {
			at += (size_t)snprintf(text + at, size - at, " a key: status -49 with its time of creation given; the time listener registered;");
			continue;
		}
		if (status != KM_NOT_CONFIGURED || attempt != 0) {
			break;
		}
		/* (not configured, and not yet told: tz=4 to 6 configure only when asked to) */
		{
			const int32_t configured = tz_configure(keymaster, shared);

			if (configured == INT32_MIN) {
				at += (size_t)snprintf(text + at, size - at, " a key: not configured, and configure did not finish;");
				goto out;
			}
			at += (size_t)snprintf(text + at, size - at, " configure (%u, %u, %u): status %d;", KM_OUR_OS_VERSION, KM_OUR_PATCHLEVEL,
			    KM_OUR_VENDOR_PATCHLEVEL, configured);
		}
	}
	where = tz_word(response + 8);
	key = tz_word(response + 12);
	if (status != 0 || key == 0 || key > sizeof(tz_blob) || where > TZ_ROOM || tz_behind(request) + where + key > TZ_ROOM) {
		at += (size_t)snprintf(text + at, size - at, " a key: status %d, %u bytes at %u: not made;", status, key, where);
		goto out;
	}
	memcpy(tz_blob, response + where, key);
	/* (its first words say what kind of wrapping it is; the key itself is in there encrypted, for the key master alone) */
	printf("tz: the key as handed out: %u bytes, beginning %08x %08x %08x %08x %08x %08x %08x %08x\n", key, tz_word(tz_blob),
	    tz_word(tz_blob + 4), tz_word(tz_blob + 8), tz_word(tz_blob + 12), tz_word(tz_blob + 16), tz_word(tz_blob + 20),
	    tz_word(tz_blob + 24), tz_word(tz_blob + 28));
	at += (size_t)snprintf(text + at, size - at, " a key: made, %u bytes wrapped;", key);

	{
		const struct tz_param seal[] = {
			{ KM_TAG_BLOCK_MODE, 32, NULL }, { KM_TAG_PADDING, 1, NULL }, { KM_TAG_MAC_LENGTH, 128, NULL },
		};

		sealed_length = tz_crypt(keymaster, shared, key, 0, seal, 3, plain, sizeof(plain), sealed, sizeof(sealed), nonce, &nonce_length,
		    &why);
		if (sealed_length < 0 && why == KM_NO_HARDWARE && level >= 6 && tz_time_start()) {
			at += (size_t)snprintf(text + at, size - at, " encrypting: status -49; the time listener registered;");
			sealed_length = tz_crypt(keymaster, shared, key, 0, seal, 3, plain, sizeof(plain), sealed, sizeof(sealed), nonce,
			    &nonce_length, &why);
		}
	}
	if (sealed_length < 0) {
		at += (size_t)snprintf(text + at, size - at, " encrypting 32 bytes: failed (%d);", why);
		goto out;
	}
	at += (size_t)snprintf(text + at, size - at, " encrypting 32 bytes: %d bytes out, a nonce of %u, %s;", sealed_length, nonce_length,
	    sealed_length >= 32 && memcmp(sealed, plain, 32) != 0 ? "not the input" : "THE INPUT UNCHANGED");
	if (level >= 7) {
		tz_keep_print(key, nonce, nonce_length, sealed, (uint32_t)sealed_length);
		at += (size_t)snprintf(text + at, size - at, " the key printed for keeping;");
	}
	{
		const struct tz_param open[] = {
			{ KM_TAG_BLOCK_MODE, 32, NULL }, { KM_TAG_PADDING, 1, NULL }, { KM_TAG_MAC_LENGTH, 128, NULL },
			{ KM_TAG_NONCE, nonce_length, nonce },
		};

		opened_length = tz_crypt(keymaster, shared, key, 1, open, 4, sealed, (uint32_t)sealed_length, opened, sizeof(opened), NULL,
		    NULL, &why);
	}
	if (opened_length < 0) {
		at += (size_t)snprintf(text + at, size - at, " decrypting them: failed (%d);", why);
		goto out;
	}
	at += (size_t)snprintf(text + at, size - at, " decrypting them: %d bytes out, %s;", opened_length,
	    opened_length == 32 && memcmp(opened, plain, 32) == 0 ? "the 32 bytes that went in" : "NOT what went in");
out:
	if (tz_time != 0 && at < size) {
		at += (size_t)snprintf(text + at, size - at, " the time listener answered %u requests;", tz_time_asked);
	}
	if (tz_wants != 0 && at < size) {
		at += (size_t)snprintf(text + at, size - at, " listeners asked for and refused:");
		for (unsigned i = 0; i < tz_wants && at < size; i++) {
			at += (size_t)snprintf(text + at, size - at, " %#llx (%s)", tz_wanted[i], tz_listener(tz_wanted[i]));
		}
		if (at < size) {
			at += (size_t)snprintf(text + at, size - at, ";");
		}
	}
	bzero(page, PAGE_SIZE);
	return at < size ? at : size - 1;
}

static void
tz_ask(uint32_t level)
{
	static const char *const names[] = { "keymaster", "keymaster64", "gatekeeper", "qcom.tz.uefisecapp", "soter64" };
	uint64_t answer[3] = { 0, 0, 0 };
	uint32_t keymaster = 0;
	size_t at = 0;
	int32_t status;
	vm_offset_t name = tz_page();

	if (name == 0) {
		snprintf(tz_text, sizeof(tz_text), "no memory for a page: nothing asked\n");
		return;
	}
	status = tz_call(TZ_OWNER_SIP, 6, 3, TZ_ARGS1(TZ_VALUE), 10, 0, 0, 0, answer);
	printf("tz: QSEE version: status %d, %#llx\n", status, answer[0]);
	at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, "QSEE version: status %d, %#llx (%llu.%llu.%llu);", status, answer[0],
	    answer[0] >> 22 & 0x3ff, answer[0] >> 12 & 0x3ff, answer[0] & 0xfff);
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]) && at < sizeof(tz_text); i++) {
		const size_t length = strlen(names[i]);

		bzero((void *)name, PAGE_SIZE);
		memcpy((void *)name, names[i], length);
		tz_out_of_cache(name);
		answer[0] = answer[1] = answer[2] = 0;
		status = tz_call(TZ_OWNER_QSEE_OS, 1, 3, TZ_ARGS2(TZ_BUFFER, TZ_VALUE), kvtophys(name), length, 0, 0, answer);
		printf("tz: %s: status %d, result %#llx, type %#llx, data %#llx\n", names[i], status, answer[0], answer[1], answer[2]);
		if (status == 0 && answer[0] == 0 && answer[1] == TZ_RESULT_APP_ID) {
			at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, " %s: loaded, id %llu;", names[i], answer[2]);
			if (keymaster == 0 && strncmp(names[i], "keymaster", 9) == 0) {
				keymaster = (uint32_t)answer[2];
			}
		} else {
			at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, " %s: not loaded (status %d, result %#llx, type %#llx);",
			    names[i], status, answer[0], answer[1]);
		}
	}
	if (level >= 5 && keymaster != 0) {
		tz_log_start();
		at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, " the secure world's log: %s;", tz_log != 0 ? "registered" : "not registered");
	}
	if (level >= 2 && keymaster != 0 && at < sizeof(tz_text)) {
		/*
		 * THE SEND, THROUGH A SHARED-MEMORY BRIDGE. Handed a request and a response in ordinary pages of ours, the key master
		 * never answered: the phone was gone at that call, twice (runs 155603 and 161135). The maker's own kernel for this
		 * phone takes every buffer it gives TrustZone out of a "shm bridge" (OnePlus's qseecom.c: qtee_shmbridge_allocate_shm,
		 * for a look-up's name too), and this phone's UEFI carries the driver that switches the bridge on (ShmBridgeDxe). So:
		 * one page of ours, below 4 GB as the maker's are, is made a bridge first (Linux's qcom_scm_shm_bridge_create: SIP,
		 * service 0x0c, command 0x1e: the address with its permissions for this side and for the secure side, the size with
		 * the number of owners, the owner: 3, HLOS), and only if TrustZone says yes is anything sent: the request at the
		 * page's start, the arguments past the third at 1024, the response at 2048, all inside the bridge. The bridge is
		 * taken down again afterwards (command 0x1d). Nothing is sent if the bridge is refused.
		 */
		vm_offset_t shared = 0;
		bool low = kmem_alloc_contig(kernel_map, &shared, PAGE_SIZE, PAGE_MASK, (ppnum_t)(atop_64(0x100000000ull) - 1), 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS;

		if (!low) {
			shared = tz_page();
		}
		if (shared == 0) {
			at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, " keymaster's version: no page for the bridge, not asked;");
		} else {
			const uint64_t phys = kvtophys(shared);
			uint64_t *rest = (uint64_t *)(shared + 1024);
			const int32_t *words = (const int32_t *)(shared + 2048);
			uint64_t handle;

			printf("tz: the page for the bridge is at %#llx (%s 4 GB), %u bytes\n", phys, low ? "below" : "ABOVE", (unsigned)PAGE_SIZE);
			answer[0] = answer[1] = answer[2] = 0;
			status = tz_call(TZ_OWNER_SIP, 0x0c, 0x1e, 4 /* four values */, phys | 6, phys | 6, PAGE_SIZE | 1u << 9, 3, answer);
			handle = answer[1];
			printf("tz: bridge: status %d, result %#llx, handle %#llx\n", status, answer[0], handle);
			if ((status != 0 || answer[0] != 0) && level == 3) {
				/* (tz=3: refused, so bridges may not be switched on in this boot: the maker's kernel switches them on itself
				 * at its start, command 0x1c with no argument, and makes its bridge after that; the same here, once) */
				uint64_t enabled[3] = { 0, 0, 0 };
				const int32_t on = tz_call(TZ_OWNER_SIP, 0x0c, 0x1c, 0, 0, 0, 0, 0, enabled);

				printf("tz: bridges switched on: status %d, result %#llx\n", on, enabled[0]);
				at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, " (refused: status %d, result %#llx; switched on: status %d, result %#llx)",
				    status, answer[0], on, enabled[0]);
				answer[0] = answer[1] = answer[2] = 0;
				status = tz_call(TZ_OWNER_SIP, 0x0c, 0x1e, 4, phys | 6, phys | 6, PAGE_SIZE | 1u << 9, 3, answer);
				handle = answer[1];
				printf("tz: bridge, again: status %d, result %#llx, handle %#llx\n", status, answer[0], handle);
			}
			at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, " bridge over %#llx: status %d, result %#llx, handle %#llx;", phys, status,
			    answer[0], handle);
			if (status == 0 && answer[0] == 0) {
				*(uint32_t *)shared = KEYMASTER_GET_VERSION;
				rest[0] = phys + 2048;
				rest[1] = 64;
				tz_out_of_cache(shared);
				answer[0] = answer[1] = answer[2] = 0;
				status = tz_call(TZ_OWNER_APPS, 0, 1, TZ_ARGS5(TZ_VALUE, TZ_BUFFER, TZ_VALUE, TZ_BUFFER, TZ_VALUE), keymaster, phys, 64,
				    phys + 1024, answer);
				printf("tz: keymaster's version: call status %d, result %#llx, type %#llx; it answers status %d, %d.%d, application %d.%d\n",
				    status, answer[0], answer[1], words[0], words[1], words[2], words[3], words[4]);
				at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at,
				    " keymaster's version: call status %d, result %#llx, type %#llx; it answers status %d, %d.%d, application %d.%d;", status,
				    answer[0], answer[1], words[0], words[1], words[2], words[3], words[4]);
				if (level >= 4 && status == 0 && answer[0] == 0 && words[0] == 0 && at < sizeof(tz_text)) {
					at += tz_key(level, keymaster, shared, tz_text + at, sizeof(tz_text) - at);
				}
				tz_log_print();
				if (tz_stuck) {
					/* (an application is waiting with this page in its hands: the bridge stays) */
					printf("tz: the bridge stays, an application was left waiting\n");
				} else {
					answer[0] = answer[1] = answer[2] = 0;
					status = tz_call(TZ_OWNER_SIP, 0x0c, 0x1d, TZ_ARGS1(TZ_VALUE), handle, 0, 0, 0, answer);
					printf("tz: bridge taken down: status %d, result %#llx\n", status, answer[0]);
				}
			} else {
				at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, " keymaster's version: not asked, there is no bridge to send it through;");
			}
		}
	} else if (level >= 2 && at < sizeof(tz_text)) {
		at += (size_t)snprintf(tz_text + at, sizeof(tz_text) - at, " keymaster's version: not asked, no keymaster was found;");
	}
	if (at < sizeof(tz_text)) {
		snprintf(tz_text + at, sizeof(tz_text) - at, "\n");
	}
	/* (the pages stay: TrustZone was given their addresses, and two pages are not worth finding out whether it keeps them) */
}

/*
 * THE ROOT KEY (boot-arg tz_keys=1): what the key store's stand-in wraps the keychain's keys under.
 *
 * The stand-in (iokit/Kernel/MiniAppleKeyStore.cpp) had nothing secret to wrap with, and "wrapped" every key of the
 * keychain with a pattern anybody can undo (docs/KNOWN_ISSUES.md K13): a copy of the flash is a copy of the keychain.
 * Runs 170955 to 172750 showed that this phone's key master makes an AES key, uses it, and uses it again after a restart,
 * and that the wrapped form of such a key is of use to this phone's TrustZone alone. So:
 *
 *   - Once, on a volume that has none: a key is made in the key master (it wants the time for that: the time listener),
 *     32 random bytes of this kernel's become the root key, the key master encrypts them with its key, and a record of
 *     it all (as tz=7's: four words, "TZR1" and the three lengths, the key master's wrapped key, the nonce, the 48 bytes)
 *     is written to /private/var/db/hackios-root.tz, after the key master has opened it once to show that it can.
 *   - At every start after that: the record is read and the key master asked to decrypt the 48 bytes. That is one
 *     look-up, a bridge, a configure, a begin and a finish, when the keychain first asks for a key; no listener.
 *   - The root key then stays in this kernel's memory, as an iPhone's class keys stay in its key store once it has been
 *     unlocked, and the stand-in wraps with AES key wrap under a key made of it (hk_keywrap.c). TrustZone is not asked
 *     again in that boot.
 *
 * What this gives: the keychain's database, copied off the flash, cannot be read without this phone's TrustZone and the
 * boot state the key master ties its keys to. What it does not give: there is no passcode, the phone is never locked,
 * and this kernel lets any program ask the key store (K13's other half): whoever can start this phone reads its
 * keychain. A record the key master will not open is LEFT AS IT IS and no new one is made over it: the keys wrapped
 * under its root key would be lost for good, and the cause may be one that goes away (a boot image with other numbers).
 * In such a boot, and in any boot without tz_keys, the stand-in wraps with the old pattern, which it always still reads.
 *
 * THE KEY, UPGRADED. The key master writes the system's version and patch levels into a key and compares them at every
 * use: a key of older numbers than it has now is refused with -62, "requires upgrade", until it has been through the
 * upgrade command (0x114: the wrapped key in, the same key wrapped under the present numbers out; the layout is the
 * phone's own library's, as the others). The numbers change when the boot image's header does, or KM_OUR_* here. So a
 * record whose begin is answered -62 is upgraded: the key master's new wrapping of its key takes the old one's place in
 * the record, the nonce and the 48 bytes stay as they are (the key is the same key), and only after the key master has
 * opened the root key with the upgraded wrapping is anything written: first the record as it was, to
 * /private/var/db/hackios-root.tz.before-upgrade, then the new one over the old. If the record itself is not one (a write
 * that was cut short), the copy from before the upgrade is taken and upgraded again. Numbers that went DOWN are not
 * mended by this, nor should they be: the key master then refuses the key (a newer key under an older system) and the
 * record is left as it is.
 *
 * tz_keys=2, for a test boot that keeps nothing: also prints a new record for the computer ("tz: root ..." lines,
 * tools/nf/cell/tz_keep.py --root) and, when the volume has no record, takes one from the root image
 * (/hackios/share/tz-root). tz_keys=3: as 2, with the secure world's log around it.
 */
#define TZ_ROOT_FILE            "/private/var/db/hackios-root.tz"
#define TZ_ROOT_BEFORE          "/private/var/db/hackios-root.tz.before-upgrade"
#define TZ_ROOT_GIVEN           "/hackios/share/tz-root"
#define TZ_ROOT_MAGIC           0x31525a54u     /* "TZR1" */

static int tz_root_state;               /* 0 not asked yet, 1 there, 2 not to be had in this boot, 3 made and not yet written */
static uint8_t tz_root[32];
static uint8_t tz_root_bytes[sizeof(tz_blob) + 96], tz_root_check[sizeof(tz_blob) + 96];
static uint32_t tz_root_length;
static char tz_root_text[300] = "not asked for yet";
static SInt32 tz_counts[5];

static uint32_t
tz_file_read(const char *path, uint8_t *to, uint32_t room)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t file = NULLVP;
	int left = 0, error;

	if (vnode_open(path, FREAD, 0, 0, &file, context) != 0) {
		return 0;
	}
	error = vn_rdwr(UIO_READ, file, (caddr_t)to, (int)room, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(context), &left,
	    vfs_context_proc(context));
	vnode_close(file, FREAD, context);
	return error == 0 ? room - (uint32_t)left : 0;
}

/* Written, pushed to the disk, and read back the same. */
static bool
tz_file_write(const char *path, const uint8_t *from, uint32_t length)
{
	vfs_context_t context = vfs_context_kernel();
	vnode_t file = NULLVP;
	int left = 0, error;

	error = vnode_open(path, FWRITE | O_CREAT | O_TRUNC, 0600, 0, &file, context);
	if (error != 0) {
		printf("tz: %s cannot be made (%d)\n", path, error);
		return false;
	}
	error = vn_rdwr(UIO_WRITE, file, (caddr_t)(uintptr_t)from, (int)length, 0, UIO_SYSSPACE, IO_NOAUTH | IO_SYNC,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	if (error == 0) {
		error = VNOP_FSYNC(file, MNT_WAIT, context);
	}
	vnode_close(file, FWRITE, context);
	if (error != 0 || left != 0) {
		printf("tz: %s was not written (%d, %d bytes left)\n", path, error, left);
		return false;
	}
	return tz_file_read(path, tz_root_check, sizeof(tz_root_check)) == length && memcmp(tz_root_check, from, length) == 0;
}

/* A new AES-256 key for GCM that asks for no authentication, into tz_blob: its length, or 0. (The steps of tz= make theirs
 * in tz_key, which differs by step in what it sends; this is what step 7 sends.) */
static uint32_t
tz_generate(uint32_t keymaster, vm_offset_t shared)
{
	struct tz_param make[] = {
		{ KM_TAG_ALGORITHM, 32, NULL }, { KM_TAG_KEY_SIZE, 256, NULL }, { KM_TAG_PURPOSE, 0, NULL }, { KM_TAG_PURPOSE, 1, NULL },
		{ KM_TAG_BLOCK_MODE, 32, NULL }, { KM_TAG_PADDING, 1, NULL }, { KM_TAG_MIN_MAC_LENGTH, 128, NULL },
		{ KM_TAG_NO_AUTH_REQUIRED, 1, NULL }, { KM_TAG_CREATION_DATETIME, 0, NULL },
	};
	const uint32_t entries = sizeof(make) / sizeof(make[0]);
	uint8_t *page = (uint8_t *)shared;
	clock_sec_t seconds = 0;
	clock_usec_t micro = 0;

	clock_get_calendar_microtime(&seconds, &micro);
	make[entries - 1].value = (uint64_t)seconds * 1000;
	for (int attempt = 0; attempt < 2; attempt++) {
		const uint8_t *response;
		uint32_t request, where, key;
		int32_t status;

		bzero(page, PAGE_SIZE);
		tz_put(page, KM_GENERATE_KEY);
		tz_put(page + 4, 12);
		tz_put(page + 8, entries);
		request = 12 + tz_params(page + 12, make, entries);
		if (!tz_send(keymaster, shared, request, "make a key")) {
			return 0;
		}
		response = page + tz_behind(request);
		status = (int32_t)tz_word(response);
		where = tz_word(response + 8);
		key = tz_word(response + 12);
		printf("tz: make a key: the key master answers status %d, the key at %u, %u bytes\n", status, where, key);
		if (status == KM_NO_HARDWARE && attempt == 0 && tz_time_start()) {
			continue;               /* (it wants the time, and now there is somebody to ask) */
		}
		if (status != 0 || key == 0 || key > sizeof(tz_blob) || where > TZ_ROOM || tz_behind(request) + where + key > TZ_ROOM) {
			return 0;
		}
		memcpy(tz_blob, response + where, key);
		return key;
	}
	return 0;
}

/* The wrapped key in tz_blob through the key master's upgrade: its new length (the new wrapping is then in tz_blob), or 0. */
static uint32_t
tz_upgrade(uint32_t keymaster, vm_offset_t shared, uint32_t key)
{
	uint8_t *page = (uint8_t *)shared;
	const uint8_t *response;
	uint32_t request, where, upgraded;
	int32_t status;

	bzero(page, PAGE_SIZE);
	tz_put(page, KM_UPGRADE_KEY);
	tz_put(page + 4, 20);
	tz_put(page + 8, key);
	memcpy(page + 20, tz_blob, key);
	request = 20 + key;
	if (!tz_send(keymaster, shared, request, "upgrade the key")) {
		return 0;
	}
	response = page + tz_behind(request);
	status = (int32_t)tz_word(response);
	where = tz_word(response + 8);
	upgraded = tz_word(response + 12);
	printf("tz: upgrade the key: the key master answers status %d, the key at %u, %u bytes\n", status, where, upgraded);
	if (status != 0 || upgraded == 0 || upgraded > sizeof(tz_blob) || where > TZ_ROOM || tz_behind(request) + where + upgraded > TZ_ROOM) {
		return 0;
	}
	memcpy(tz_blob, response + where, upgraded);
	return upgraded;
}

/* Is this a record of a root key: the mark, lengths that fit, and no byte more or less. */
static bool
tz_root_is_record(const uint8_t *bytes, uint32_t length)
{
	uint32_t key, nonce, sealed;

	if (length < 16 || tz_word(bytes) != TZ_ROOT_MAGIC) {
		return false;
	}
	key = tz_word(bytes + 4);
	nonce = tz_word(bytes + 8);
	sealed = tz_word(bytes + 12);
	return key != 0 && key <= sizeof(tz_blob) && nonce != 0 && nonce <= 16 && sealed == 48 && length == 16 + key + nonce + sealed;
}

static void
tz_root_print(uint32_t length)
{
	static char hex[2 * 40 + 1];
	uint32_t sum = 0;

	for (uint32_t i = 0; i < length; i += 40) {
		const uint32_t count = length - i < 40 ? length - i : 40;

		for (uint32_t j = 0; j < count; j++) {
			snprintf(hex + 2 * j, 3, "%02x", tz_root_bytes[i + j]);
		}
		printf("tz: root %04u: %s\n", i, hex);
	}
	for (uint32_t i = 0; i < length; i++) {
		sum = sum * 31 + tz_root_bytes[i];
	}
	printf("tz: root end: %u bytes, sum %08x\n", length, sum);
}

static void
tz_root_start(uint32_t wanted)
{
	static const char name[] = "keymaster64";
	const uint32_t pause = tz_pause;
	vfs_context_t context = vfs_context_kernel();
	uint64_t answer[3] = { 0, 0, 0 };
	uint8_t nonce[16] = { 0 }, sealed[64], opened[64];
	vnode_t directory = NULLVP;
	vm_offset_t page, shared = 0;
	uint64_t phys, handle = 0;
	uint32_t keymaster, key, kept, nonce_length = 0, sealed_length;
	int32_t status, why = 0;
	const char *source = TZ_ROOT_FILE;
	int length;

	if (tz_root_state == 3) {
		/* (made in this boot, and the volume would not take the record then) */
		if (tz_file_write(TZ_ROOT_FILE, tz_root_bytes, tz_root_length)) {
			tz_root_state = 1;
			snprintf(tz_root_text, sizeof(tz_root_text), "made in this boot, its record written at a later try (%u bytes)", tz_root_length);
			printf("tz: the root key: %s\n", tz_root_text);
		}
		return;
	}
	if (vnode_lookup("/private/var/db", 0, &directory, context) != 0) {
		return;                         /* (the volume the record lives on is not there yet: asked again at the next key) */
	}
	vnode_put(directory);
	tz_root_state = 2;
	tz_pause = 0;
	printf("tz: the root key (tz_keys=%u): asking the key master\n", wanted);
	page = tz_page();
	if (page == 0) {
		snprintf(tz_root_text, sizeof(tz_root_text), "none: no memory");
		goto done;
	}
	memcpy((void *)page, name, sizeof(name) - 1);
	tz_out_of_cache(page);
	status = tz_call(TZ_OWNER_QSEE_OS, 1, 3, TZ_ARGS2(TZ_BUFFER, TZ_VALUE), kvtophys(page), sizeof(name) - 1, 0, 0, answer);
	if (status != 0 || answer[0] != 0 || answer[1] != TZ_RESULT_APP_ID) {
		snprintf(tz_root_text, sizeof(tz_root_text), "none: no key master is loaded (status %d, result %#llx)", status, answer[0]);
		goto done;
	}
	keymaster = (uint32_t)answer[2];
	if (wanted >= 3) {
		tz_log_start();
	}
	if (kmem_alloc_contig(kernel_map, &shared, PAGE_SIZE, PAGE_MASK, (ppnum_t)(atop_64(0x100000000ull) - 1), 0,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		snprintf(tz_root_text, sizeof(tz_root_text), "none: no page below 4 GB for the bridge");
		goto done;
	}
	phys = kvtophys(shared);
	answer[0] = answer[1] = answer[2] = 0;
	status = tz_call(TZ_OWNER_SIP, 0x0c, 0x1e, 4, phys | 6, phys | 6, PAGE_SIZE | 1u << 9, 3, answer);
	handle = answer[1];
	if (status != 0 || answer[0] != 0) {
		snprintf(tz_root_text, sizeof(tz_root_text), "none: no bridge (status %d, result %#llx)", status, answer[0]);
		goto done;
	}
	status = tz_configure(keymaster, shared);
	if (status != 0) {
		snprintf(tz_root_text, sizeof(tz_root_text), "none: the key master does not take its configuration (%d)", status);
		goto bridge;
	}
	kept = tz_file_read(TZ_ROOT_FILE, tz_root_bytes, sizeof(tz_root_bytes));
	if (!tz_root_is_record(tz_root_bytes, kept)) {
		/* (none, or bytes that are not one: an upgrade's write may have been cut short, and the record from before it is kept) */
		const uint32_t before = tz_file_read(TZ_ROOT_BEFORE, tz_root_check, sizeof(tz_root_check));

		if (tz_root_is_record(tz_root_check, before)) {
			printf("tz: the root key: %u bytes in %s that are not a record; the one from before an upgrade is taken (%u bytes)\n", kept,
			    TZ_ROOT_FILE, before);
			memcpy(tz_root_bytes, tz_root_check, before);
			kept = before;
			source = TZ_ROOT_BEFORE;
		}
	}
	if (kept == 0 && wanted >= 2) {
		kept = tz_file_read(TZ_ROOT_GIVEN, tz_root_bytes, sizeof(tz_root_bytes));
		if (kept != 0) {
			source = "the root image, a test boot's";
		}
	}
	if (kept != 0) {
		const char *how = "";

		if (!tz_root_is_record(tz_root_bytes, kept)) {
			snprintf(tz_root_text, sizeof(tz_root_text), "none: %u bytes in %s that are not a record: LEFT AS THEY ARE", kept, source);
			goto bridge;
		}
		key = tz_word(tz_root_bytes + 4);
		nonce_length = tz_word(tz_root_bytes + 8);
		sealed_length = tz_word(tz_root_bytes + 12);
		memcpy(tz_blob, tz_root_bytes + 16, key);
		memcpy(nonce, tz_root_bytes + 16 + key, nonce_length);
		memcpy(sealed, tz_root_bytes + 16 + key + nonce_length, sealed_length);
		{
			const struct tz_param open[] = {
				{ KM_TAG_BLOCK_MODE, 32, NULL }, { KM_TAG_PADDING, 1, NULL }, { KM_TAG_MAC_LENGTH, 128, NULL },
				{ KM_TAG_NONCE, nonce_length, nonce },
			};

			length = tz_crypt(keymaster, shared, key, 1, open, 4, sealed, sealed_length, opened, sizeof(opened), NULL, NULL, &why);
			if (length != 32 && why == KM_REQUIRES_UPGRADE) {
				const uint32_t newer = tz_upgrade(keymaster, shared, key);

				printf("tz: the root key: the key master wants its key upgraded (made under older numbers than patch level %u)\n", tz_patch);
				if (newer != 0) {
					length = tz_crypt(keymaster, shared, newer, 1, open, 4, sealed, sealed_length, opened, sizeof(opened), NULL, NULL,
					    &why);
				}
				if (newer != 0 && length == 32) {
					/* (it opens with the new wrapping: the record as it was is put aside, then the new one written) */
					const bool aside = tz_file_write(TZ_ROOT_BEFORE, tz_root_bytes, kept);

					tz_put(tz_root_bytes + 4, newer);
					memcpy(tz_root_bytes + 16, tz_blob, newer);
					memcpy(tz_root_bytes + 16 + newer, nonce, nonce_length);
					memcpy(tz_root_bytes + 16 + newer + nonce_length, sealed, sealed_length);
					tz_root_length = 16 + newer + nonce_length + sealed_length;
					if (aside && tz_file_write(TZ_ROOT_FILE, tz_root_bytes, tz_root_length)) {
						how = ", AFTER IT UPGRADED ITS KEY: the record written anew, the one from before kept beside it";
					} else {
						how = ", AFTER IT UPGRADED ITS KEY: the new record NOT WRITTEN (it is upgraded again at the next start)";
					}
					if (wanted >= 2) {
						tz_root_print(tz_root_length);
					}
					kept = tz_root_length;
				}
			}
		}
		if (length != 32) {
			snprintf(tz_root_text, sizeof(tz_root_text), "none: the key master does not open the record in %s (%d): LEFT AS IT IS, "
			    "what was wrapped under its root key cannot be read in this boot", source, why);
			goto bridge;
		}
		memcpy(tz_root, opened, 32);
		tz_root_state = 1;
		snprintf(tz_root_text, sizeof(tz_root_text), "opened by the key master from the record in %s (%u bytes)%s", source, kept, how);
	} else {
		key = tz_generate(keymaster, shared);
		if (key == 0) {
			snprintf(tz_root_text, sizeof(tz_root_text), "none: the key master made no key");
			goto bridge;
		}
		read_random(tz_root, sizeof(tz_root));
		{
			const struct tz_param seal[] = {
				{ KM_TAG_BLOCK_MODE, 32, NULL }, { KM_TAG_PADDING, 1, NULL }, { KM_TAG_MAC_LENGTH, 128, NULL },
			};

			length = tz_crypt(keymaster, shared, key, 0, seal, 3, tz_root, sizeof(tz_root), sealed, sizeof(sealed), nonce, &nonce_length,
			    &why);
		}
		if (length != 48 || nonce_length == 0 || nonce_length > 16) {
			snprintf(tz_root_text, sizeof(tz_root_text), "none: the key master did not encrypt the new root key (%d, %d bytes)", why, length);
			goto forget;
		}
		{
			/* (before anything is wrapped under it: the key master opens what it has just made) */
			const struct tz_param open[] = {
				{ KM_TAG_BLOCK_MODE, 32, NULL }, { KM_TAG_PADDING, 1, NULL }, { KM_TAG_MAC_LENGTH, 128, NULL },
				{ KM_TAG_NONCE, nonce_length, nonce },
			};

			length = tz_crypt(keymaster, shared, key, 1, open, 4, sealed, 48, opened, sizeof(opened), NULL, NULL, &why);
		}
		if (length != 32 || memcmp(opened, tz_root, 32) != 0) {
			snprintf(tz_root_text, sizeof(tz_root_text), "none: the key master did not open the root key it had just encrypted (%d)", why);
			goto forget;
		}
		tz_put(tz_root_bytes, TZ_ROOT_MAGIC);
		tz_put(tz_root_bytes + 4, key);
		tz_put(tz_root_bytes + 8, nonce_length);
		tz_put(tz_root_bytes + 12, 48);
		memcpy(tz_root_bytes + 16, tz_blob, key);
		memcpy(tz_root_bytes + 16 + key, nonce, nonce_length);
		memcpy(tz_root_bytes + 16 + key + nonce_length, sealed, 48);
		tz_root_length = 16 + key + nonce_length + 48;
		tz_root_state = tz_file_write(TZ_ROOT_FILE, tz_root_bytes, tz_root_length) ? 1 : 3;
		snprintf(tz_root_text, sizeof(tz_root_text), "made in this boot: a key of the key master's (%u bytes wrapped), 32 random bytes "
		    "encrypted with it, the record (%u bytes) %s %s", key, tz_root_length, tz_root_state == 1 ? "written to" : "NOT YET WRITTEN to",
		    TZ_ROOT_FILE);
		if (wanted >= 2) {
			tz_root_print(tz_root_length);
		}
	}
	goto bridge;
forget:
	bzero(tz_root, sizeof(tz_root));
bridge:
	if (wanted >= 3) {
		tz_log_print();
	}
	if (!tz_stuck) {
		answer[0] = answer[1] = answer[2] = 0;
		tz_call(TZ_OWNER_SIP, 0x0c, 0x1d, TZ_ARGS1(TZ_VALUE), handle, 0, 0, 0, answer);
	}
done:
	bzero(opened, sizeof(opened));
	if (shared != 0) {
		bzero((void *)shared, PAGE_SIZE);
	}
	tz_pause = pause;
	printf("tz: the root key: %s\n", tz_root_text);
}

bool
hk_tz_root_key(uint8_t key[32])
{
	static uint32_t wanted = UINT32_MAX;
	bool have;

	if (wanted == UINT32_MAX) {
		uint32_t given = 0;

		PE_parse_boot_argn("tz_keys", &given, sizeof(given));
		wanted = given;
	}
	if (wanted == 0) {
		return false;
	}
	lck_mtx_lock(&tz_lock);
	if (tz_root_state == 0 || tz_root_state == 3) {
		tz_root_start(wanted);
	}
	have = tz_root_state == 1;
	if (have) {
		memcpy(key, tz_root, sizeof(tz_root));
	}
	lck_mtx_unlock(&tz_lock);
	return have;
}

void
hk_tz_count(unsigned which)
{
	if (which < sizeof(tz_counts) / sizeof(tz_counts[0])) {
		OSIncrementAtomic(&tz_counts[which]);
	}
}

/* What the key store's stand-in wraps with, and how often it has: read only, TrustZone is not asked by this. */
static int
tz_keys_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	char text[520];
	uint32_t wanted = 0;

	PE_parse_boot_argn("tz_keys", &wanted, sizeof(wanted));
	lck_mtx_lock(&tz_lock);
	snprintf(text, sizeof(text), "the root key: %s; keys wrapped under it %d, unwrapped %d; wrapped with the old pattern %d, unwrapped "
	    "of it %d; unwraps refused %d\n", wanted == 0 ? "off (boot-arg tz_keys=1 has the key store wrap under a key TrustZone keeps)" :
	    tz_root_text, tz_counts[0], tz_counts[1], tz_counts[2], tz_counts[3], tz_counts[4]);
	lck_mtx_unlock(&tz_lock);
	return SYSCTL_OUT(req, text, strlen(text) + 1);
}

static int
tz_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	uint32_t level = 0;

	PE_parse_boot_argn("tz", &level, sizeof(level));
	lck_mtx_lock(&tz_lock);
	if (level == 0) {
		snprintf(tz_text, sizeof(tz_text),
		    "off: boot-arg tz=1 asks TrustZone what it has loaded, tz=2 also the keymaster's version, tz=4 also makes and uses a test key, tz=5 with the secure world's log, tz=6 with the time served, tz=7 with a key kept across boots\n");
	} else if (!tz_asked) {
		tz_asked = true;
		printf("tz: step %u: asking TrustZone what it has running; nothing is loaded, started or stored%s\n", level,
		    level >= 4 ? "; and a key that protects nothing is made and used once" : "");
		tz_ask(level);
		printf("tz: %s", tz_text);
	}
	lck_mtx_unlock(&tz_lock);
	return SYSCTL_OUT(req, tz_text, strlen(tz_text) + 1);
}
SYSCTL_DECL(_kern_hackios);
SYSCTL_PROC(_kern_hackios, OID_AUTO, tz_keys, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, tz_keys_sysctl, "A",
    "the key store's stand-in: what it wraps the keychain's keys with (boot-arg tz_keys=1: a root key TrustZone's key master keeps)");
SYSCTL_PROC(_kern_hackios, OID_AUTO, tz, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, tz_sysctl, "A",
    "TrustZone: the QSEE's version and which secure applications are loaded (boot-arg tz=1, asked once; tz=4 makes a test key)");
