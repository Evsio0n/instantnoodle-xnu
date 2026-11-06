/*
 * modem=3: the SDX55's firmware (included by bsd/dev/instantnoodle_modem.c; drafted in tools/nf/modem while the tree was another session's to build).
 *
 *   1. what modem=2 does: the reset, PCIe 2 up, the device in its primary boot loader;
 *   2. the device's stream 0x1d01 (dt iommu-map, BDF 0x100; qcom,smmu-sid-base 0x1d00) through a context with tables of ours, as hk_wifi.c does for 0x1c01, under
 *      hk_smmu_lock, on the addresses the tree gives it (mhi_0_iommu_group: 0x20000000, 512 MB). A refusal by the hypervisor reads back 0 and ends the run [observed
 *      for camera streams]; only then may the device be bus master;
 *   3. wifi/lx/src/mhi_host.c, under other names, with a boot of its own at the end of this file: Android's for this device has no BHIe (mhi_qcom.c: sdx55m/sbl1.mbn
 *      through BHI), and in its secondary boot loader the device asks for everything else by Sahara on channels 2 and 3 (dt mhi,ee = 2);
 *   4. the files come from where the phone keeps them: the FAT of modem_a, image/sdx55m (modem_fat.c), and the EFS copies from their partitions, READ ONLY, through
 *      /dev/rufs_NAME. Nothing of the phone's storage is written. No firmware, no IMEI, nothing of a SIM's goes into the log: ids and sizes;
 *   5. the device in its mission (EE AMSS), MDM2AP status high: the second milestone, and the run stays there to say what the modem does.
 *
 * Which Sahara id is which file: closed in Android (ks's table of the old names, mdm_helper's -s for the EFS). The ones below come from ks's strings, in the order
 * Qualcomm's kickstart tool has them, and from Linux's table for the same family (qaic's aic200_image_table) [inferred]. An id with no file ends the run, and says which.
 */
#include "modem_mhi.h"
#include "modem_fat.h"
#include "modem_sahara.h"

#include <sys/disk.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>
#include <kern/kalloc.h>
#include <kern/sched_prim.h>
#include <kern/locks.h>
#include <kern/clock.h>

extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern lck_mtx_t hk_smmu_lock;
extern uint32_t hk_display_context;
void hk_modem_quiesce(void);

/* ---- memory the device can reach ---------------------------------------------------------------------------- */

#define MDM_ADDRESSES           0x20000000ull   /* dt: mhi_0_iommu_group, qcom,iommu-dma-addr-pool */
#define MDM_ADDRESSES_SIZE      0x1fffffffull
#define MDM_POOL_SIZE           (24u * 1024 * 1024)
#define MDM_TABLES              16              /* of 4K: the root, one of the second level, 2 MB of addresses each other */
#define MDM_STREAM              0x1d01
#define PTE_PAGE                (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)        /* as hk_wifi.c's RADIO_PTE: Linux's for a device */
#define APPS_SMMU_PHYS          0x15000000ul
#define SMMU_SCR0               0x000
#define SMMU_IDR0               0x020
#define SMMU_IDR1               0x024
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR0           0x050
#define SMMU_SGFSYNR1           0x054
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
#define SMMU_CBAR(n)            (4 * (n))
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CBA2R_64_BIT       1
#define SMMU_CB_SCTLR           0x00
#define SMMU_CB_TCR2            0x10
#define SMMU_CB_TTBR0           0x20
#define SMMU_CB_TTBR1           0x28
#define SMMU_CB_TCR             0x30
#define SMMU_CB_MAIR0           0x38
#define SMMU_CB_MAIR1           0x3c
#define SMMU_CB_FSR             0x58
#define SMMU_CB_FAR             0x60
#define SMMU_CB_FSYNR0          0x68
#define SMMU_CB_TLBIASID        0x610
#define SMMU_CB_TLBSYNC         0x7f0
#define SMMU_CB_TLBSTATUS       0x7f4
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe

static struct {
	vm_offset_t global, second, context_page;
	uint32_t match, context, old_smr, old_s2cr, old_sctlr;
	bool on;
	vm_offset_t tables;
	uint32_t table_count;
	uint8_t *pool;
	uint64_t pool_physical, pool_next;
} mdm;

static uint64_t *
mdm_table(uint32_t n)
{
	return (uint64_t *)(mdm.tables + 4096 * n);
}

static uint64_t *
mdm_next(uint64_t *entry)
{
	uint64_t *table;

	if (*entry & 1) {
		for (uint32_t i = 0; i < mdm.table_count; i++) {
			if ((*entry & 0xfffffffff000ull) == kvtophys(mdm.tables + 4096 * i)) {
				return mdm_table(i);
			}
		}
		return NULL;
	}
	if (mdm.table_count == MDM_TABLES) {
		return NULL;
	}
	table = mdm_table(mdm.table_count++);
	*entry = kvtophys((vm_offset_t)table) | 3;
	return table;
}

/* The pool, and tables of ours with all of it behind MDM_ADDRESSES. Nothing of the SMMU's yet. */
static bool
mdm_pool(void)
{
	vm_offset_t cached = 0;

	if (kmem_alloc_contig(kernel_map, &cached, MDM_POOL_SIZE, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &mdm.tables, 4096 * MDM_TABLES, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		say("modem: no %u MB of memory in one piece\n", MDM_POOL_SIZE >> 20);
		return false;
	}
	mdm.pool_physical = kvtophys(cached);
	bzero((void *)cached, MDM_POOL_SIZE);
	for (vm_offset_t line = cached; line < cached + MDM_POOL_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	mdm.pool = (uint8_t *)ml_io_map_unmappable(mdm.pool_physical, MDM_POOL_SIZE, VM_WIMG_WCOMB);
	if (mdm.pool == NULL) {
		return false;
	}
	mdm.table_count = 1;
	for (uint64_t offset = 0; offset < MDM_POOL_SIZE; offset += 4096) {
		uint64_t at = MDM_ADDRESSES + offset, *level2 = mdm_next(&mdm_table(0)[at >> 30 & 0x1ff]);
		uint64_t *level3 = level2 == NULL ? NULL : mdm_next(&level2[at >> 21 & 0x1ff]);

		if (level3 == NULL) {
			say("modem: no page table left at %#llx\n", at);
			return false;
		}
		level3[at >> 12 & 0x1ff] = (mdm.pool_physical + offset) | PTE_PAGE;
	}
	for (vm_offset_t line = mdm.tables; line < mdm.tables + 4096 * MDM_TABLES; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	say("modem: %u MB at %#llx are the device's %#llx on\n", MDM_POOL_SIZE >> 20, mdm.pool_physical, MDM_ADDRESSES);
	return true;
}

/* The stream through a free context, as hk_wifi.c's radio_space_find() and radio_space_on(). The global registers are the hypervisor's to grant: read back. */
static bool
mdm_smmu_on(void)
{
	bool used[256] = { false }, ok;
	uint32_t idr0, idr1, page, pages, matches, contexts, sees = 1u << 31 | MDM_STREAM;
	uint64_t root;

	mdm.global = ml_io_map(APPS_SMMU_PHYS, 0x1000);
	idr0 = rd(mdm.global + SMMU_IDR0);
	idr1 = rd(mdm.global + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	page = idr1 >> 31 ? 0x10000 : 0x1000;
	pages = 2u << (idr1 >> 28 & 7);
	if ((rd(mdm.global + SMMU_SCR0) & 1u << 3) || 2 * pages * page != 0x100000) {
		say("modem: the SMMU is not as hk_wifi.c knows it (idr1 %#x)\n", idr1);
		return false;
	}
	lck_mtx_lock(&hk_smmu_lock);
	mdm.match = ~0u;
	used[4] = true;                 /* (never read: hardware-qualcomm.md 1) */
	used[hk_display_context] = true;
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(mdm.global + SMMU_SMR(n)), s2cr = rd(mdm.global + SMMU_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
		} else if (mdm.match == ~0u) {
			mdm.match = n;
			mdm.old_smr = smr;
			mdm.old_s2cr = s2cr;
		}
	}
	for (mdm.context = 0; mdm.context < contexts && used[mdm.context]; mdm.context++) {
	}
	if (mdm.match == ~0u || mdm.context >= contexts) {
		lck_mtx_unlock(&hk_smmu_lock);
		say("modem: the SMMU has no match register or no context left\n");
		return false;
	}
	mdm.second = ml_io_map(APPS_SMMU_PHYS + page, 0x1000);
	mdm.context_page = ml_io_map(APPS_SMMU_PHYS + (pages + mdm.context) * page, 0x1000);
	mdm.old_sctlr = rd(mdm.context_page + SMMU_CB_SCTLR);
	if (mdm.old_sctlr & 1) {
		lck_mtx_unlock(&hk_smmu_lock);
		say("modem: context %u is switched on though no stream goes through it: somebody's, left alone\n", mdm.context);
		return false;
	}
	root = kvtophys(mdm.tables) | (uint64_t)mdm.context << 48;
	say("modem: THE SMMU IS WRITTEN: context %u gets tables of ours, match register %u sends stream %#x through it\n", mdm.context, mdm.match, MDM_STREAM);
	wr_sync(mdm.context_page + SMMU_CB_SCTLR, 0);
	wr_sync(mdm.second + SMMU_CBA2R(mdm.context), SMMU_CBA2R_64_BIT);
	wr_sync(mdm.second + SMMU_CBAR(mdm.context), SMMU_CBAR_STAGE_1);
	wr_sync(mdm.context_page + SMMU_CB_TCR2, SMMU_TCR2);
	wr_sync(mdm.context_page + SMMU_CB_TCR, SMMU_TCR_39_BITS);
	wr_sync(mdm.context_page + SMMU_CB_TTBR0, (uint32_t)root);
	wr_sync(mdm.context_page + SMMU_CB_TTBR0 + 4, (uint32_t)(root >> 32));
	wr_sync(mdm.context_page + SMMU_CB_TTBR1, 0);
	wr_sync(mdm.context_page + SMMU_CB_TTBR1 + 4, 0);
	wr_sync(mdm.context_page + SMMU_CB_MAIR0, SMMU_MAIR0);
	wr_sync(mdm.context_page + SMMU_CB_MAIR1, 0);
	wr_sync(mdm.context_page + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr_sync(mdm.context_page + SMMU_CB_TLBIASID, mdm.context);
	wr_sync(mdm.context_page + SMMU_CB_TLBSYNC, 1);
	for (unsigned ms = 0; rd(mdm.context_page + SMMU_CB_TLBSTATUS) & 1; ms++) {
		if (ms == 100) {
			lck_mtx_unlock(&hk_smmu_lock);
			say("modem: the SMMU does not finish forgetting old translations\n");
			return false;
		}
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	wr_sync(mdm.context_page + SMMU_CB_SCTLR, SMMU_SCTLR_ON);
	mdm.on = true;
	wr_sync(mdm.global + SMMU_S2CR(mdm.match), mdm.context);
	wr_sync(mdm.global + SMMU_SMR(mdm.match), sees);
	ok = rd(mdm.global + SMMU_SMR(mdm.match)) == sees && (rd(mdm.global + SMMU_S2CR(mdm.match)) & 0x300ff) == mdm.context && (rd(mdm.context_page + SMMU_CB_SCTLR) & 1);
	lck_mtx_unlock(&hk_smmu_lock);
	if (!ok) {
		say("modem: THE HYPERVISOR REFUSES THE STREAM: smr %#x, s2cr %#x, sctlr %#x\n", rd(mdm.global + SMMU_SMR(mdm.match)), rd(mdm.global + SMMU_S2CR(mdm.match)),
		    rd(mdm.context_page + SMMU_CB_SCTLR));
	}
	return ok;
}

static void
mdm_smmu_say_faults(void)
{
	uint32_t status, global;

	if (!mdm.on) {
		return;
	}
	status = rd(mdm.context_page + SMMU_CB_FSR);
	global = rd(mdm.global + SMMU_SGFSR);
	if ((status & SMMU_FSR_ALL) != 0 || global != 0) {
		say("modem: THE SMMU HAS REFUSED A REQUEST: status %#x, address %#llx, syndrome %#x; global %#x, %#x %#x\n", status,
		    (uint64_t)rd(mdm.context_page + SMMU_CB_FAR + 4) << 32 | rd(mdm.context_page + SMMU_CB_FAR), rd(mdm.context_page + SMMU_CB_FSYNR0), global,
		    rd(mdm.global + SMMU_SGFSYNR0), rd(mdm.global + SMMU_SGFSYNR1));
	}
}

/* The stream goes nowhere again: on the way out, PERST low first. */
static void
mdm_smmu_off(void)
{
	if (mdm.on) {
		mdm_smmu_say_faults();
		wr_sync(mdm.global + SMMU_SMR(mdm.match), mdm.old_smr);
		wr_sync(mdm.global + SMMU_S2CR(mdm.match), mdm.old_s2cr);
		wr_sync(mdm.context_page + SMMU_CB_SCTLR, mdm.old_sctlr);
		mdm.on = false;
	}
}

/* The controller's table from a BDF to a stream: the bridge's and the device's, as instantnoodle_wifi.c's sid_table() has them. */
static uint8_t
bdf_hash(uint16_t bdf)
{
	uint8_t bytes[2] = { (uint8_t)(bdf >> 8), (uint8_t)bdf }, crc = 0;

	for (unsigned b = 0; b < 2; b++) {
		crc ^= bytes[b];
		for (unsigned i = 0; i < 8; i++) {
			crc = (uint8_t)(crc & 0x80 ? crc << 1 ^ 0x07 : crc << 1);
		}
	}
	return crc;
}

#define PARF_BDF_TO_SID         0x2000

static void
sid_table(void)
{
	static const uint16_t bdfs[2] = { 0x0000, EP_BDF };

	for (unsigned i = 0; i < 256; i++) {
		wr_sync(pcie.parf + PARF_BDF_TO_SID + 4 * i, 0);
	}
	for (unsigned i = 0; i < 2; i++) {
		uint8_t hash = bdf_hash(bdfs[i]);

		while (rd(pcie.parf + PARF_BDF_TO_SID + 4u * hash) != 0) {
			uint32_t entry = rd(pcie.parf + PARF_BDF_TO_SID + 4u * hash);
			uint8_t following = (uint8_t)(hash + 1);

			if ((entry & 0xff) == 0) {
				wr_sync(pcie.parf + PARF_BDF_TO_SID + 4u * hash, entry | following);
			}
			hash = following;
		}
		wr_sync(pcie.parf + PARF_BDF_TO_SID + 4u * hash, (uint32_t)bdfs[i] << 16 | (uint32_t)i << 8);
	}
}

/* ---- what MHI wants of a kernel ------------------------------------------------------------------------------ */

static LCK_GRP_DECLARE(mdm_lock_group, "instantnoodle-modem");
static LCK_MTX_DECLARE(mdm_mhi_lock, &mdm_lock_group);

#define SAHARA_OUT              2
#define SAHARA_IN               3
#define IPCR_OUT                20
#define IPCR_IN                 21
#define MDM_PACKETS             16

/* what has come in on SAHARA_IN, for the thread that answers it */
static struct {
	uint8_t data[MDM_PACKETS][64];
	uint32_t length[MDM_PACKETS];
	volatile uint32_t head, tail;
} mdm_in;

/*
 * The device's window only while the link is up: a modem that crashes or resets may take the link down, and what a read through a downed link does here is not known
 * (a read of PCIe 0's before its clocks is a CrashDump). The controller's own status is safe to read. Reads as all ones then, as a device that has gone does.
 */
static bool
mdm_link_up(void)
{
	static bool said;
	bool up = (rd(pcie.dbi + DBI_LINK_STATUS) & 1u << 29) != 0;

	if (!up && !said) {
		said = true;
		say("modem: THE LINK IS DOWN (LTSSM %#x): the device's window is not touched any more\n", rd(pcie.dbi + ELBI_SYS_STTS) >> 12 & 0x3f);
	}
	return up;
}

static uint32_t
op_read(void *c, uint32_t offset)
{
	(void)c;
	return mdm_link_up() ? rd(pcie.bar + offset) : 0xffffffffu;
}

static void
op_write(void *c, uint32_t offset, uint32_t value)
{
	(void)c;
	if (mdm_link_up()) {
		wr_sync(pcie.bar + offset, value);
	}
}

static void *
op_dma_alloc(void *c, size_t size, size_t align, uint64_t *address)
{
	uint64_t at = (mdm.pool_next + align - 1) & ~((uint64_t)align - 1);

	(void)c;
	if (at + size > MDM_POOL_SIZE) {
		say("modem: the pool is out of room for %zu bytes\n", size);
		return NULL;
	}
	mdm.pool_next = at + size;
	*address = MDM_ADDRESSES + at;
	bzero(mdm.pool + at, size);
	return mdm.pool + at;
}

static void op_dma_free(void *c, void *m, size_t s) { (void)c; (void)m; (void)s; }

static uint64_t
op_nanoseconds(void *c)
{
	uint64_t ns;

	(void)c;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns;
}

static void op_lock(void *c) { (void)c; lck_mtx_lock(&mdm_mhi_lock); }
static void op_unlock(void *c) { (void)c; lck_mtx_unlock(&mdm_mhi_lock); }
static void op_pause(void *c, unsigned us) { (void)c; delay_for_interval(us, NSEC_PER_USEC); }

static void
op_random(void *c, void *bytes, size_t length)
{
	uint64_t t = mach_absolute_time();

	(void)c;
	memcpy(bytes, &t, length < sizeof(t) ? length : sizeof(t));     /* (a transaction number: only not 0 and not the last) */
}

static void op_say(void *c, const char *line) { (void)c; say("modem: %s\n", line); }

static void
op_status(void *c, enum mhi_host_status what)
{
	(void)c;
	say("modem: mhi says: %s\n", what == MHI_HOST_MISSION ? "the device is in its mission" : what == MHI_HOST_RDDM ? "the device has crashed (RDDM)" : "the device reports an error");
}

/*
 * QRTR on IPCR, in its mission: hello both ways, and what it says it offers and looks for (hk_adsp.c's, over MHI instead of GLINK). No service is spoken to: nothing of a SIM, no
 * registration, no RF is asked for. Data for a port of ours has nobody to take it, and only its size is said.
 */
#define MDM_QRTR_US             1u
#define MDM_QRTR_CONTROL        0xfffffffeu
enum { MDM_QRTR_DATA = 1, MDM_QRTR_HELLO = 2, MDM_QRTR_BYE = 3, MDM_QRTR_NEW_SERVER = 4, MDM_QRTR_DEL_SERVER = 5, MDM_QRTR_DEL_CLIENT = 6, MDM_QRTR_RESUME_TX = 7, MDM_QRTR_NEW_LOOKUP = 10, MDM_QRTR_DEL_LOOKUP = 11 };

struct mdm_qrtr_header {
	uint32_t version, type, from_node, from_port, confirm, size, to_node, to_port;
};

static volatile bool mdm_qrtr_hello_due;
static uint32_t mdm_qrtr_peer, mdm_qrtr_in, mdm_qrtr_servers, mdm_qrtr_lookups;

/*
 * QMI LOC, service 16 (libqmi's qmi-service-loc.json has the ids), with modem_loc=1: for now two questions that start nothing, the engine's lock and its operation mode. Each is answered
 * twice, a response and then an indication. Only these answers' fields are said, and of no other message but its id and size: a position must never reach the log.
 */
#define MDM_LOC_SERVICE         16
#define MDM_LOC_GET_ENGINE_LOCK 0x003b
#define MDM_LOC_GET_OP_MODE     0x004b
#define MDM_QRTR_OUR_PORT       0x4000u
#define MDM_LOC_REGISTER_EVENTS 0x0021
#define MDM_LOC_START           0x0022
#define MDM_LOC_STOP            0x0023
#define MDM_LOC_POSITION_REPORT 0x0024
#define MDM_LOC_SV_INFO         0x0025
#define MDM_LOC_ENGINE_STATE    0x002b
#define MDM_LOC_INJECT_UTC      0x0038
#define MDM_SV_ENTRY            28      /* libqmi's list element, packed: masks, system, id, health, status, data, elevation, azimuth, signal */
static uint32_t mdm_loc_enabled;
static volatile uint32_t mdm_loc_node, mdm_loc_port;
static volatile bool mdm_resume_due;
static volatile uint32_t mdm_resume_node, mdm_resume_port;

/*
 * modem_gnss=1, the one try the owner allowed by a window (2026-11-06): what the engine reports is counted, and of a position only its uncertainty, status and the number of satellites
 * used. Coordinates, altitude, speed, heading, time of week, satellites' ids and directions are never read out of the packet, let alone kept or printed.
 */
static uint32_t mdm_gnss_enabled;
static struct {
	uint64_t started_ms, first_signal_ms, first_fix_ms, last_said_ms;
	uint32_t sv_reports, position_reports, fixes;
	uint32_t most_listed, most_with_signal, best_signal, best_uncertainty;
	bool shape_said;
} mdm_gnss;

static uint32_t
mdm_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* An IEEE float's bits as tenths, in whole numbers: no floating point in the kernel. Negative, huge or not a number: ~0u. */
static uint32_t
mdm_tenths(uint32_t bits)
{
	uint32_t exponent = bits >> 23 & 0xff;
	uint64_t value = (uint64_t)(1u << 23 | (bits & 0x7fffffu)) * 10;
	int shift = (int)exponent - 127 - 23;

	if (bits >> 31 || exponent == 255) {
		return ~0u;
	}
	if (exponent == 0) {
		return 0;
	}
	if (shift >= 8) {
		return ~0u;
	}
	value = shift >= 0 ? value << shift : (shift <= -63 ? 0 : value >> -shift);
	return value > 0xfffffffeull ? ~0u : (uint32_t)value;
}

static uint64_t
mdm_gnss_ms(void)
{
	return op_nanoseconds(NULL) / NSEC_PER_MSEC;
}

/* Satellites: how many are listed, how many with a signal, the best signal. Not which, nor where. */
static void
mdm_gnss_sv_info(const uint8_t *q, size_t length, uint32_t size)
{
	for (size_t at = 7; at + 3 <= length && at + 3 <= 7u + size;) {
		uint32_t tlv = q[at], bytes = (uint32_t)q[at + 1] | (uint32_t)q[at + 2] << 8;

		if (at + 3 + bytes > length) {
			return;
		}
		if (tlv == 0x10 && bytes >= 1) {
			uint32_t listed = q[at + 3], with_signal = 0, best = 0;
			uint64_t now = mdm_gnss_ms();

			if (bytes != 1 + listed * MDM_SV_ENTRY) {
				if (!mdm_gnss.shape_said) {
					mdm_gnss.shape_said = true;
					say("modem: gnss: the satellites' list is %u bytes for %u of them, not %u each: its signals are not read\n", bytes, listed, MDM_SV_ENTRY);
				}
			} else {
				for (uint32_t i = 0; i < listed; i++) {
					uint32_t signal = mdm_tenths(mdm_le32(q + at + 4 + i * MDM_SV_ENTRY + 24));

					if (signal != ~0u && signal > 0) {
						with_signal++;
						best = signal > best ? signal : best;
					}
				}
			}
			mdm_gnss.sv_reports++;
			mdm_gnss.most_listed = listed > mdm_gnss.most_listed ? listed : mdm_gnss.most_listed;
			mdm_gnss.most_with_signal = with_signal > mdm_gnss.most_with_signal ? with_signal : mdm_gnss.most_with_signal;
			mdm_gnss.best_signal = best > mdm_gnss.best_signal ? best : mdm_gnss.best_signal;
			if (with_signal != 0 && mdm_gnss.first_signal_ms == 0) {
				mdm_gnss.first_signal_ms = now;
				say("modem: gnss: FIRST SIGNAL %llu ms after the start\n", now - mdm_gnss.started_ms);
			}
			if (now - mdm_gnss.last_said_ms >= 20000) {
				mdm_gnss.last_said_ms = now;
				say("modem: gnss: +%llu s: %u satellites listed, %u with a signal, the best %u.%u dB-Hz\n", (now - mdm_gnss.started_ms) / 1000, listed, with_signal, best / 10, best % 10);
			}
		}
		at += 3 + bytes;
	}
}

/* A position report: its status, its uncertainty, how many satellites it used. Every other field, the coordinates among them, is stepped over unread. */
static void
mdm_gnss_position(const uint8_t *q, size_t length, uint32_t size)
{
	uint32_t status = ~0u, uncertainty = ~0u, used = 0;

	for (size_t at = 7; at + 3 <= length && at + 3 <= 7u + size;) {
		uint32_t tlv = q[at], bytes = (uint32_t)q[at + 1] | (uint32_t)q[at + 2] << 8;

		if (at + 3 + bytes > length) {
			break;
		}
		if (tlv == 0x01 && bytes == 4) {
			status = mdm_le32(q + at + 3);
		} else if (tlv == 0x12 && bytes == 4) {
			uncertainty = mdm_tenths(mdm_le32(q + at + 3));
		} else if (tlv == 0x2c && bytes >= 1) {
			used = q[at + 3];
		}
		at += 3 + bytes;
	}
	mdm_gnss.position_reports++;
	if (uncertainty != ~0u && (mdm_gnss.best_uncertainty == 0 || uncertainty < mdm_gnss.best_uncertainty)) {
		mdm_gnss.best_uncertainty = uncertainty;
	}
	if (status == 0) {      /* (libqmi: 0, success; 1, in progress) */
		mdm_gnss.fixes++;
		if (mdm_gnss.first_fix_ms == 0) {
			mdm_gnss.first_fix_ms = mdm_gnss_ms();
			say("modem: gnss: FIRST FIX %llu ms after the start: uncertainty %u.%u m, %u satellites used\n", mdm_gnss.first_fix_ms - mdm_gnss.started_ms, uncertainty / 10, uncertainty % 10, used);
		}
	}
	if (mdm_gnss.position_reports == 1 || mdm_gnss.position_reports % 30 == 0) {
		say("modem: gnss: report %u: status %u, %s%u.%u m, %u satellites used\n", mdm_gnss.position_reports, status, uncertainty == ~0u ? "no uncertainty, " : "", uncertainty == ~0u ? 0 : uncertainty / 10,
		    uncertainty == ~0u ? 0 : uncertainty % 10, used);
	}
}

static void
mdm_loc_input(const uint8_t *q, size_t length)
{
	static const char *const kinds[] = { "request", "?", "response", "?", "indication" };
	uint16_t message, size;

	if (length < 7) {
		say("modem: loc: %zu bytes\n", length);
		return;
	}
	message = (uint16_t)(q[3] | q[4] << 8);
	size = (uint16_t)(q[5] | q[6] << 8);
	if (message == MDM_LOC_SV_INFO) {
		mdm_gnss_sv_info(q, length, size);
		return;
	}
	if (message == MDM_LOC_POSITION_REPORT) {
		mdm_gnss_position(q, length, size);
		return;
	}
	say("modem: loc: %s %#x, %u bytes\n", q[0] < 5 ? kinds[q[0]] : "?", message, size);
	if (message != MDM_LOC_GET_ENGINE_LOCK && message != MDM_LOC_GET_OP_MODE && message != MDM_LOC_REGISTER_EVENTS && message != MDM_LOC_START && message != MDM_LOC_STOP &&
	    message != MDM_LOC_INJECT_UTC && message != MDM_LOC_ENGINE_STATE) {
		return;
	}
	for (size_t at = 7; at + 3 <= length && at + 3 <= 7u + size;) {
		uint32_t tlv = q[at], bytes = (uint32_t)q[at + 1] | (uint32_t)q[at + 2] << 8, value = 0;

		if (at + 3 + bytes > length) {
			break;
		}
		if (message == MDM_LOC_GET_ENGINE_LOCK && tlv == 0x12 && bytes == 8) {
			/*
			 * Qualcomm's public location_service_v02.h: lockClient, a 64-bit mask of the clients locked (run 102633 sent it in place of libqmi's Lock Type). Should this firmware mean
			 * something else by it, 8 bytes could be an IMSI or an IMEI: so printed only if both halves are small, and of anything else not a byte, hashed or not.
			 */
			uint32_t low = (uint32_t)q[at + 3] | (uint32_t)q[at + 4] << 8 | (uint32_t)q[at + 5] << 16 | (uint32_t)q[at + 6] << 24;
			uint32_t high = (uint32_t)q[at + 7] | (uint32_t)q[at + 8] << 8 | (uint32_t)q[at + 9] << 16 | (uint32_t)q[at + 10] << 24;

			if (low <= 255 && high <= 255) {
				say("modem: loc:   %#x: field 0x12: %u %u\n", message, low, high);
			} else {
				say("modem: loc:   %#x: field 0x12: not small\n", message);
			}
		} else if (bytes > 4) {
			say("modem: loc:   %#x: field %#x, %u bytes\n", message, tlv, bytes);     /* (longer than a result or a state: its size only, whatever it holds) */
		} else {
			for (uint32_t i = 0; i < bytes; i++) {
				value |= (uint32_t)q[at + 3 + i] << (8 * i);
			}
			say("modem: loc:   %#x: field %#x, %u bytes: %#x\n", message, tlv, bytes, value);
		}
		at += 3 + bytes;
	}
}

static void
mdm_qrtr_input(const uint8_t *data, size_t length)
{
	struct mdm_qrtr_header header;
	uint32_t words[5] = { 0 };
	size_t at, payload;

	/*
	 * The second header is what it speaks, and its number is 3 (Linux's QRTR_PROTO_VER_2; run 095805): a byte each of version, type, flags and options, the size, then 16 bits each of the
	 * two ends (run 073746: node 2, port 0xfffe, to node 0xffff).
	 */
	if (length >= 16 && data[0] == 3) {
		memset(&header, 0, sizeof(header));
		header.version = 3;
		header.type = data[1];
		header.confirm = data[2] & 1;
		memcpy(&header.size, data + 4, 4);
		header.from_node = (uint32_t)data[8] | (uint32_t)data[9] << 8;
		header.from_port = (uint32_t)data[10] | (uint32_t)data[11] << 8;
		header.to_node = (uint32_t)data[12] | (uint32_t)data[13] << 8;
		header.to_port = (uint32_t)data[14] | (uint32_t)data[15] << 8;
		at = 16 + (size_t)data[3] * 4;
	} else if (length >= sizeof(header) && data[0] == 1) {
		memcpy(&header, data, sizeof(header));
		at = sizeof(header);
	} else {
		say("modem: qrtr: a packet of %zu bytes, version %u\n", length, length > 0 ? data[0] : 0);
		return;
	}
	if (at > length) {
		say("modem: qrtr: a packet of %zu bytes whose header is %zu\n", length, at);
		return;
	}
	payload = length - at < sizeof(words) ? length - at : sizeof(words);
	memcpy(words, data + at, payload);
	mdm_qrtr_in++;
	switch (header.type) {
	case MDM_QRTR_HELLO:
		say("modem: qrtr: HELLO FROM NODE %u (version %u, %zu bytes)\n", header.from_node, header.version, length);
		mdm_qrtr_peer = header.from_node;
		mdm_qrtr_hello_due = true;
		break;
	case MDM_QRTR_NEW_SERVER:
		mdm_qrtr_servers++;
		say("modem: qrtr: it offers service %u (%#x), version %u, instance %u, at node %u port %u\n", words[1], words[1], words[2] & 0xff, words[2] >> 8, words[3], words[4]);
		if (words[1] == MDM_LOC_SERVICE) {
			mdm_loc_node = words[3];
			mdm_loc_port = words[4];
		}
		break;
	case MDM_QRTR_DEL_SERVER:
		say("modem: qrtr: service %u at node %u port %u has gone\n", words[1], words[3], words[4]);
		break;
	case MDM_QRTR_NEW_LOOKUP:
		mdm_qrtr_lookups++;
		say("modem: qrtr: it looks for service %u (%#x), instance %u: nobody here offers it\n", words[1], words[1], words[2]);
		break;
	case MDM_QRTR_DATA:
		if (header.from_node == mdm_loc_node && header.from_port == mdm_loc_port && header.to_port == MDM_QRTR_OUR_PORT) {
			mdm_loc_input(data + at, length - at < header.size ? length - at : header.size);
			if (header.confirm) {
				mdm_resume_node = header.from_node;
				mdm_resume_port = header.from_port;
				mdm_resume_due = true;
			}
			break;
		}
		say("modem: qrtr: data, %u bytes, from %u:%u to %u:%u: nobody takes it\n", header.size, header.from_node, header.from_port, header.to_node, header.to_port);
		break;
	default:
		say("modem: qrtr: type %u from %u:%u, %zu bytes\n", header.type, header.from_node, header.from_port, length);
		break;
	}
}

/*
 * The modem's file sync, the owner's option A (2026-11-06, run 113829's reason: remotefs_sahara.c:850): in the mission it offers its file system over Sahara, and the host pulls what it wants
 * (Replicant's sahara.c, another chip). Here the host wants nothing: HELLO answered once, then RESET at the next thing it says, and never one MEMORY_READ, so none of its contents is sent to
 * us at all. Which channel it uses is not known: EFS, 10 and 11, is the tree's name for it [inferred]. Of what comes, only the command and its length are said, and a table's size, not its address.
 */
#define MDM_EFS_OUT             10
#define MDM_EFS_IN              11
enum { MDM_EFS_HELLO_DUE = 1, MDM_EFS_RESET_DUE = 2 };
static volatile uint32_t mdm_efs_due;
static uint32_t mdm_efs_mode, mdm_efs_state, mdm_efs_packets;
#define MDM_EFS_QUIET_AFTER     32

static void
mdm_efs_input(const uint8_t *p, size_t length)
{
	uint32_t command = length >= 8 ? mdm_le32(p) : 0, declared = length >= 8 ? mdm_le32(p + 4) : 0;

	if (++mdm_efs_packets > MDM_EFS_QUIET_AFTER) {
		return;
	}
	say("modem: efs: channel %u: command %#x, says %u bytes, %zu came\n", MDM_EFS_IN, command, declared, length);
	if (command == 0x1 && length >= 24) {
		say("modem: efs:   hello, mode %u\n", mdm_le32(p + 20));
		/* (Replicant's: the modem does not like a second hello answered) */
		if (mdm_efs_state == 0) {
			mdm_efs_mode = mdm_le32(p + 20);
			mdm_efs_state = 1;
			mdm_efs_due = MDM_EFS_HELLO_DUE;
		}
		return;
	}
	if (command == 0x8) {
		say("modem: efs:   the reset is answered\n");
		return;
	}
	if (command == 0x9 && length >= 16) {
		say("modem: efs:   its table is %u bytes\n", mdm_le32(p + 12));    /* (the address, at 8, is not said) */
	} else if (command == 0x10 && length >= 24) {
		say("modem: efs:   its table is %u bytes\n", mdm_le32(p + 16));    /* (64-bit: the address, at 8, is not said) */
	}
	if (mdm_efs_state == 1) {
		mdm_efs_state = 2;
		mdm_efs_due = MDM_EFS_RESET_DUE;
	}
}

static void
op_receive(void *c, unsigned channel, const void *data, size_t length)
{
	(void)c;
	if (channel == SAHARA_IN) {
		uint32_t at = mdm_in.head % MDM_PACKETS;

		if (mdm_in.head - mdm_in.tail == MDM_PACKETS) {
			say("modem: sahara's queue is full: a packet of %zu bytes is lost\n", length);
			return;
		}
		length = length < sizeof(mdm_in.data[0]) ? length : sizeof(mdm_in.data[0]);
		memcpy(mdm_in.data[at], data, length);
		mdm_in.length[at] = (uint32_t)length;
		__asm__ volatile ("dmb ish");
		mdm_in.head++;
		thread_wakeup((event_t)&mdm_in);
	} else if (channel == IPCR_IN) {
		mdm_qrtr_input(data, length);
	} else if (channel == MDM_EFS_IN) {
		mdm_efs_input(data, length);
	} else {
		say("modem: channel %u: %zu bytes (nothing takes them yet)\n", channel, length);
	}
}

static const struct mhi_host_ops mdm_ops = {
	.read = op_read, .write = op_write, .dma_alloc = op_dma_alloc, .dma_free = op_dma_free, .nanoseconds = op_nanoseconds, .lock = op_lock, .unlock = op_unlock,
	.pause = op_pause, .random = op_random, .say = op_say, .status = op_status, .receive = op_receive,
};

static struct mhi_host mdm_mhi;

/* ---- the files: the phone's storage, read only --------------------------------------------------------------- */

/* A partition, read through its raw device: whole 4K blocks, by a small cache. */
struct mdm_part {
	vnode_t vp;
	uint64_t cached_at;
	uint8_t *cache;
	bool have;
};

#define MDM_BLOCK               4096u
#define MDM_CACHE               (256u * 1024)

static int
part_open(struct mdm_part *p, const char *name)
{
	char path[64];
	int error;

	snprintf(path, sizeof(path), "/dev/rufs_%s", name);
	p->have = false;
	p->cache = kalloc_data(MDM_CACHE, Z_WAITOK | Z_ZERO);
	if (p->cache == NULL) {
		return ENOMEM;
	}
	for (int tries = 0;; tries++) {
		error = vnode_open(path, FREAD, 0, 0, &p->vp, vfs_context_kernel());
		if (error == 0 || tries == 60) {
			return error;
		}
		delay_for_interval(1, NSEC_PER_SEC);    /* (devfs may not be there yet) */
	}
}

static int
part_read(void *context, uint64_t offset, void *buffer, size_t length)
{
	struct mdm_part *p = context;
	uint8_t *to = buffer;

	while (length > 0) {
		uint64_t base = offset & ~((uint64_t)MDM_CACHE - 1);
		size_t within = (size_t)(offset - base), take = MDM_CACHE - within;

		if (!p->have || p->cached_at != base) {
			int resid = 0, error = vn_rdwr(UIO_READ, p->vp, (caddr_t)p->cache, MDM_CACHE, (off_t)base, UIO_SYSSPACE, IO_NOAUTH | IO_NOCACHE, kauth_cred_get(), &resid, current_proc());

			if (error != 0 || resid == MDM_CACHE) {
				p->have = false;
				say("modem: a read at %llu of the storage: error %d, %d left\n", base, error, resid);
				return error != 0 ? error : EIO;
			}
			p->have = true;
			p->cached_at = base;
		}
		take = take < length ? take : length;
		memcpy(to, p->cache + within, take);
		to += take;
		offset += take;
		length -= take;
	}
	return 0;
}

static void *fat_alloc(void *c, size_t s) { (void)c; return kalloc_data(s, Z_WAITOK | Z_ZERO); }
static void fat_free(void *c, void *m, size_t s) { (void)c; kfree_data(m, s); }
static const struct modem_fat_ops mdm_fat_ops = { part_read, fat_alloc, fat_free };

static const struct {
	uint32_t id;
	const char *file;               /* in image/sdx55m of modem_a's FAT */
	const char *partition;          /* or a partition, whole */
} mdm_images[] = {
	{ 2, "qdsp6sw.mbn", NULL }, { 6, "apps.mbn", NULL }, { 12, "qdsp6sw.mbn", NULL }, { 23, "aop.mbn", NULL }, { 25, "tz.mbn", NULL }, { 29, "acdb.mbn", NULL },
	{ 32, "tz.mbn", NULL }, { 33, "hyp.mbn", NULL }, { 38, "xbl_cfg.elf", NULL }, { 40, "apdp.mbn", NULL }, { 41, "devcfg.mbn", NULL }, { 42, "sec.elf", NULL },
	{ 16, NULL, "mdm1m9kefs1" }, { 17, NULL, "mdm1m9kefs2" }, { 20, NULL, "mdm1m9kefs3" },
	{ 37, "multi_image.mbn", NULL },        /* the first it asks for, 52 bytes: an ELF32 not in the table above [inferred; run 064537, and taken, status 0, run 065313] */
	{ 34, "mdmddr.mbn", NULL },             /* the next, 40 bytes at 0, which is that file's size [inferred; run 065313] */
	{ 36, "multi_image_qti.mbn", NULL },    /* after apdp (40, taken): an ELF32, and its load address follows apdp's [inferred; run 065933] */
	{ 8, "qdsp6sw.mbn", NULL },             /* after all the rest: an ELF32, Qualcomm's old table's DSP1 [inferred; run 070540] */
};

static struct mdm_part mdm_modem_a;
static struct modem_fat mdm_fat;
static struct modem_fat_file mdm_file;
static bool mdm_file_open;
static struct mdm_part mdm_efs;
static bool mdm_efs_open;

/*
 * Image 8 is the modem's own firmware, and with it the secondary boot loader goes on to start it. Not sent without modem_mission=1 (the supervisor, 2026-11-06: no mission until what
 * stops it is shown; a kernel that hangs leaves it running, with nothing watching its heat): everything up to there is checked and nothing of it runs.
 */
#define MDM_FIRMWARE_ID         8
static uint32_t mdm_mission;

static int
image_open(void *context, uint32_t id, uint64_t *size, void **image)
{
	(void)context;
	if (id == MDM_FIRMWARE_ID && !mdm_mission) {
		say("modem: image 8 would take it to its mission: not sent, there is no modem_mission=1\n");
		return EPERM;
	}
	if (mdm_file_open) {
		modem_fat_close(&mdm_fat, &mdm_file);
		mdm_file_open = false;
	}
	for (size_t i = 0; i < sizeof(mdm_images) / sizeof(mdm_images[0]); i++) {
		char path[64];
		int error;

		if (mdm_images[i].id != id) {
			continue;
		}
		if (mdm_images[i].partition != NULL) {
			if (mdm_efs_open) {
				vnode_close(mdm_efs.vp, FREAD, vfs_context_kernel());
				kfree_data(mdm_efs.cache, MDM_CACHE);
				mdm_efs_open = false;
			}
			if ((error = part_open(&mdm_efs, mdm_images[i].partition)) != 0) {
				return error;
			}
			mdm_efs_open = true;
			*size = 2 * 1024 * 1024;        /* (2 MB each where read: partitions.txt) */
			*image = &mdm_efs;
			say("modem: image %u is partition %s, read only\n", id, mdm_images[i].partition);
			return 0;
		}
		snprintf(path, sizeof(path), "image/sdx55m/%s", mdm_images[i].file);
		if ((error = modem_fat_open(&mdm_fat, path, &mdm_file)) != 0) {
			return error;
		}
		mdm_file_open = true;
		*size = mdm_file.size;
		*image = &mdm_file;
		say("modem: image %u is %s\n", id, mdm_images[i].file);
		return 0;
	}
	return ENOENT;
}

static int
image_read(void *context, void *image, uint64_t offset, void *buffer, size_t length)
{
	(void)context;
	if (image == &mdm_efs) {
		return part_read(&mdm_efs, offset, buffer, length);
	}
	return modem_fat_read(&mdm_fat, image, offset, buffer, length);
}

static int
sahara_send(void *context, const void *data, size_t length, bool more)
{
	(void)context;
	return mdm_mhi_send(&mdm_mhi, SAHARA_OUT, data, length, more);
}

static void sahara_say(void *context, const char *format, ...) __printflike(2, 3);

static void
sahara_say(void *context, const char *format, ...)
{
	char line[240];
	va_list list;

	(void)context;
	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say("modem: %s", line);
}

static const struct modem_sahara_ops mdm_sahara_ops = { sahara_send, image_open, image_read, sahara_say };

/* Ours to it, from the thread that watches it. */
static int
mdm_qrtr_send(uint32_t type, uint32_t from_port, uint32_t to_node, uint32_t to_port, const void *data, uint32_t length)
{
	static uint8_t packet[sizeof(struct mdm_qrtr_header) + 64];
	struct mdm_qrtr_header *header = (struct mdm_qrtr_header *)packet;
	uint32_t padded = (length + 3) & ~3u;

	if (padded > sizeof(packet) - sizeof(*header)) {
		return EMSGSIZE;
	}
	memset(packet, 0, sizeof(packet));
	header->version = 1;
	header->type = type;
	header->from_node = MDM_QRTR_US;
	header->from_port = from_port;
	header->size = length;
	header->to_node = to_node;
	header->to_port = to_port;
	memcpy(header + 1, data, length);
	return mdm_mhi_send(&mdm_mhi, IPCR_OUT, packet, sizeof(*header) + padded, false);
}

static void
mdm_qrtr_hello(void)
{
	uint32_t words[5] = { MDM_QRTR_HELLO };

	say("modem: qrtr: our hello to node %u: %d\n", mdm_qrtr_peer, mdm_qrtr_send(MDM_QRTR_HELLO, MDM_QRTR_CONTROL, mdm_qrtr_peer, MDM_QRTR_CONTROL, words, sizeof(words)));
}

/* From the thread that watches: the two things this host ever says on EFS. */
static void
mdm_efs_answer(void)
{
	uint32_t due = mdm_efs_due;
	int error;

	mdm_efs_due = 0;
	if (due == MDM_EFS_HELLO_DUE) {
		uint32_t hello[12] = { 0x2, 0x30, 2, 2, 0, mdm_efs_mode };

		error = mdm_mhi_send(&mdm_mhi, MDM_EFS_OUT, hello, sizeof(hello), false);
		say("modem: efs: hello answered, mode %u: %d\n", mdm_efs_mode, error);
	} else if (due == MDM_EFS_RESET_DUE) {
		uint32_t reset[2] = { 0x7, 8 };

		error = mdm_mhi_send(&mdm_mhi, MDM_EFS_OUT, reset, sizeof(reset), false);
		say("modem: efs: RESET sent, nothing read: %d\n", error);
	}
}

/* A request: type 0, a transaction, the message, the length of its fields, the fields. */
static void
mdm_loc_send(uint16_t message, const uint8_t *fields, uint16_t length)
{
	static uint16_t transaction;
	uint8_t q[7 + 40] = { 0 };

	if (length > sizeof(q) - 7) {
		return;
	}
	transaction++;
	q[1] = (uint8_t)transaction;
	q[2] = (uint8_t)(transaction >> 8);
	q[3] = (uint8_t)message;
	q[4] = (uint8_t)(message >> 8);
	q[5] = (uint8_t)length;
	q[6] = (uint8_t)(length >> 8);
	if (length != 0) {
		memcpy(q + 7, fields, length);
	}
	say("modem: loc: asking %#x: %d\n", message, mdm_qrtr_send(MDM_QRTR_DATA, MDM_QRTR_OUR_PORT, mdm_loc_node, mdm_loc_port, q, (uint32_t)(7 + length)));
}

static void
mdm_loc_ask(uint16_t message)
{
	mdm_loc_send(message, NULL, 0);
}

static uint16_t
mdm_put_field(uint8_t *to, uint8_t id, const void *value, uint16_t length)
{
	to[0] = id;
	to[1] = (uint8_t)length;
	to[2] = (uint8_t)(length >> 8);
	memcpy(to + 3, value, length);
	return (uint16_t)(3 + length);
}

/*
 * The one try: the time of day, which needs no network; the three events it needs, not NMEA; a session in standalone mode, which it is in already (run 102633). The operation mode is not set.
 * All little-endian, as this processor is.
 */
static void
mdm_gnss_start(void)
{
	uint8_t fields[40], session = 1;
	uint64_t mask = 1u << 0 | 1u << 1 | 1u << 7;    /* position report, satellites, engine state */
	uint32_t uncertainty_ms = 5000, recurrence = 1, intermediate = 1, interval_ms = 1000, client_type = 1;
	clock_sec_t seconds;
	clock_usec_t microseconds;
	uint64_t utc_ms;
	uint16_t length;

	clock_get_calendar_microtime(&seconds, &microseconds);
	utc_ms = (uint64_t)seconds * 1000 + microseconds / 1000;
	length = mdm_put_field(fields, 0x01, &utc_ms, sizeof(utc_ms));
	length = (uint16_t)(length + mdm_put_field(fields + length, 0x02, &uncertainty_ms, sizeof(uncertainty_ms)));
	mdm_loc_send(MDM_LOC_INJECT_UTC, fields, length);
	length = mdm_put_field(fields, 0x01, &mask, sizeof(mask));
	/* Client Type, 1: the application framework (libqmi field 0x11; Qualcomm's header). Left out it defaults to the non-framework kind, which is locked here: status 7, run 110304. */
	length = (uint16_t)(length + mdm_put_field(fields + length, 0x11, &client_type, sizeof(client_type)));
	mdm_loc_send(MDM_LOC_REGISTER_EVENTS, fields, length);
	mdm_gnss.started_ms = mdm_gnss.last_said_ms = mdm_gnss_ms();
	length = mdm_put_field(fields, 0x01, &session, sizeof(session));
	length = (uint16_t)(length + mdm_put_field(fields + length, 0x10, &recurrence, sizeof(recurrence)));
	length = (uint16_t)(length + mdm_put_field(fields + length, 0x12, &intermediate, sizeof(intermediate)));
	length = (uint16_t)(length + mdm_put_field(fields + length, 0x13, &interval_ms, sizeof(interval_ms)));
	mdm_loc_send(MDM_LOC_START, fields, length);
}

static void
mdm_gnss_stop(void)
{
	uint8_t fields[8], session = 1;

	mdm_loc_send(MDM_LOC_STOP, fields, mdm_put_field(fields, 0x01, &session, sizeof(session)));
}

/* What the sender asks for when it wants to know we have room for more (Linux's qrtr_tx_resume). */
static void
mdm_qrtr_resume(void)
{
	uint32_t words[5] = { MDM_QRTR_RESUME_TX, MDM_QRTR_US, MDM_QRTR_OUR_PORT };

	(void)mdm_qrtr_send(MDM_QRTR_RESUME_TX, MDM_QRTR_OUR_PORT, mdm_resume_node, mdm_resume_port, words, sizeof(words));
}

/* ---- modem=3 ------------------------------------------------------------------------------------------------ */

static volatile bool mdm_polling;

/* No MSI yet: the host looks at the device's rings a thousand times a second. */
static void
poll_thread(__unused void *parameter, __unused wait_result_t result)
{
	while (mdm_polling) {
		modem_mhi_host_interrupt(&mdm_mhi, -1);
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	thread_terminate(current_thread());
}

static bool
mdm_load_sbl(uint8_t **image, size_t *length)
{
	struct modem_fat_file sbl;
	int error;

	if ((error = part_open(&mdm_modem_a, "modem_a")) != 0) {
		say("modem: /dev/rufs_modem_a does not open: %d\n", error);
		return false;
	}
	if ((error = modem_fat_mount(&mdm_fat, &mdm_fat_ops, &mdm_modem_a)) != 0) {
		say("modem: modem_a's FAT: %d\n", error);
		return false;
	}
	say("modem: modem_a is FAT%u, %u bytes a cluster\n", mdm_fat.fat_type, mdm_fat.bytes_per_cluster);
	if ((error = modem_fat_open(&mdm_fat, "image/sdx55m/sbl1.mbn", &sbl)) != 0) {
		say("modem: sbl1.mbn: %d\n", error);
		return false;
	}
	*length = (size_t)sbl.size;
	*image = kalloc_data(*length, Z_WAITOK);
	if (*image == NULL || modem_fat_read(&mdm_fat, &sbl, 0, *image, *length) != 0) {
		say("modem: sbl1.mbn could not be read\n");
		return false;
	}
	modem_fat_close(&mdm_fat, &sbl);
	say("modem: sbl1.mbn: %zu bytes\n", *length);
	return true;
}

static uint64_t
mdm_fnv(const volatile uint8_t *bytes, size_t length)
{
	uint64_t hash = 0xcbf29ce484222325ull;

	for (size_t i = 0; i < length; i++) {
		hash = (hash ^ bytes[i]) * 0x100000001b3ull;
	}
	return hash;
}

/*
 * What the device will read, against what the file is (no firmware in the log: a hash, to hold beside firmware-reader's). modem_gap=OFFSET is a test: that page of the image
 * has no translation, so whether the device reaches it, and asks for that address, is the SMMU's to say.
 */
static void
mdm_check_image(const uint8_t *sbl, size_t length)
{
	uint32_t gap;

	say("modem: the image: FNV-1a %#llx read from the file, %#llx where the device reads it\n", mdm_fnv(sbl, length), mdm_fnv((const volatile uint8_t *)mdm_mhi.image, length));
	if (PE_parse_boot_argn("modem_gap", &gap, sizeof(gap)) && gap < length) {
		uint64_t at = mdm_mhi.image_address + (gap & ~0xfffu);
		uint64_t *level2 = mdm_next(&mdm_table(0)[at >> 30 & 0x1ff]), *level3 = level2 == NULL ? NULL : mdm_next(&level2[at >> 21 & 0x1ff]);

		if (level3 != NULL) {
			level3[at >> 12 & 0x1ff] = 0;
			__asm__ volatile ("dc civac, %0\n dsb sy" : : "r" (&level3[at >> 12 & 0x1ff]));
			wr_sync(mdm.context_page + SMMU_CB_TLBIASID, mdm.context);
			wr_sync(mdm.context_page + SMMU_CB_TLBSYNC, 1);
			say("modem: a test: the device's %#llx has no translation\n", at);
		}
	}
}

/*
 * The device's MSI. Android's host has it on before BHI, this one did not, and the device's error comes 9 ms after the image's last page is read (runs 063433, 063927): it may want to
 * say it is done [hypothesis]. An MSI is a write to an address of the host's choosing, so here it is a page of the pool: nothing takes an interrupt from it (the poll thread looks), and
 * what is in the page says the device asked.
 */
#define MDM_MSI_DATA    0x1230
static volatile uint32_t *mdm_msi_page;

static void
mdm_msi_on(void)
{
	uint32_t cap = rd(pcie.conf + 0x34) & 0xff, control = 0;
	uint64_t address;

	for (unsigned hops = 0; cap != 0 && hops < 32; hops++) {
		control = rd(pcie.conf + cap);
		if ((control & 0xff) == 5) {
			break;
		}
		cap = control >> 8 & 0xff;
	}
	if (cap == 0 || (control & 0xff) != 5) {
		say("modem: the device has no MSI capability\n");
		return;
	}
	mdm_msi_page = op_dma_alloc(NULL, 4096, 4096, &address);
	if (mdm_msi_page == NULL) {
		return;
	}
	wr_sync(pcie.conf + cap + 4, (uint32_t)address);
	if (control & 1u << 23) {
		wr_sync(pcie.conf + cap + 8, (uint32_t)(address >> 32));
		wr_sync(pcie.conf + cap + 0xc, MDM_MSI_DATA);
	} else {
		wr_sync(pcie.conf + cap + 8, MDM_MSI_DATA);
	}
	wr_sync(pcie.conf + cap, (control & ~(7u << 20)) | (control >> 17 & 7) << 20 | 1u << 16);
	say("modem: MSI on: capability at %#x, control %#x, now %#x; its writes go to the device's %#llx\n", cap, control >> 16, rd(pcie.conf + cap) >> 16, address);
}

/*
 * The modem's failure reason (the owner, 2026-11-06, 「先查 modem 崩潰」). Normally a source file and a line, but the buffer is the modem's to fill, so its text is cleaned before anything
 * sees it: printable ASCII only, at most 160 bytes, any run of 7 or more digits a #, anything like a decimal coordinate a #. The raw bytes stay nowhere: the copy here and the buffer are cleared.
 */
#define MDM_SFR_LENGTH          256     /* Android's MHI_MAX_SFR_LEN */
#define MDM_SFR_SAID            160
static volatile uint8_t *mdm_sfr;

static bool
mdm_digit(char c)
{
	return c >= '0' && c <= '9';
}

/* In to out, as above. Returns out's length; out has room for said + 1. */
static size_t
mdm_clean_reason(const char *in, size_t length, char *out, size_t said)
{
	size_t used = 0;

	for (size_t i = 0; i < length && in[i] != 0 && used < said;) {
		size_t run = i, fraction;
		bool sign = (in[i] == '-' || in[i] == '+') && i + 1 < length && mdm_digit(in[i + 1]);

		if (sign) {
			run++;
		}
		if (run < length && mdm_digit(in[run])) {
			size_t digits = run;

			while (digits < length && mdm_digit(in[digits])) {
				digits++;
			}
			fraction = digits;
			if (digits + 1 < length && in[digits] == '.' && mdm_digit(in[digits + 1])) {
				fraction = digits + 1;
				while (fraction < length && mdm_digit(in[fraction])) {
					fraction++;
				}
			}
			/* 7 or more digits in a row, or a number with two or more decimals */
			if (digits - run >= 7 || (fraction > digits && fraction - digits - 1 >= 2)) {
				out[used++] = '#';
				i = fraction;
				continue;
			}
			while (i < digits && used < said) {
				out[used++] = in[i++];
			}
			continue;
		}
		if (in[i] >= 0x20 && in[i] < 0x7f) {
			out[used++] = in[i];
		}
		i++;
	}
	out[used] = 0;
	return used;
}

static void
mdm_say_reason(void)
{
	char raw[MDM_SFR_LENGTH], clean[MDM_SFR_SAID + 1];
	size_t length;

	if (mdm_sfr == NULL) {
		return;
	}
	for (unsigned i = 0; i < MDM_SFR_LENGTH; i++) {
		raw[i] = (char)mdm_sfr[i];
	}
	length = mdm_clean_reason(raw, MDM_SFR_LENGTH - 1, clean, MDM_SFR_SAID);
	if (length == 0) {
		say("modem: its failure reason: none written\n");
	} else {
		say("modem: its failure reason (cleaned, %zu bytes): %s\n", length, clean);
	}
	bzero(raw, sizeof(raw));
	for (unsigned i = 0; i < MDM_SFR_LENGTH; i++) {
		mdm_sfr[i] = 0;
	}
}

static void
level3(void)
{
	static uint8_t chunk[MODEM_SAHARA_CHUNK];
	static struct modem_sahara sahara;
	static struct mhi_host_config config = {
		.max_channels = 111,    /* dt mhi,max-channels */
		.timeout_ms = 8000,     /* (dt mhi,timeout is 2000; Linux's for this family, pci_generic.c, 8000; only how long a stuck device is waited for) */
		.buffer_length = MODEM_SAHARA_CHUNK,
		.channel_count = 4,
		.event_count = 3,
		.channels = {
			{ SAHARA_OUT, 128, 1, false, 1u << MHI_HOST_EE_SBL }, { SAHARA_IN, 8, 1, true, 1u << MHI_HOST_EE_SBL },
			{ IPCR_OUT, 16, 2, false, 1u << MHI_HOST_EE_AMSS }, { IPCR_IN, 32, 2, true, 1u << MHI_HOST_EE_AMSS },
			{ MDM_EFS_OUT, 16, 1, false, 1u << MHI_HOST_EE_AMSS }, { MDM_EFS_IN, 16, 1, true, 1u << MHI_HOST_EE_AMSS },     /* (counted in only with modem_gnss=1) */
		},
		.events = { { 32, 0, 0, true }, { 256, 1, 0, false }, { 256, 2, 0, false } },
		.segment_length = 512 * 1024,
		.iova_start = MDM_ADDRESSES,
		.iova_stop = MDM_ADDRESSES + MDM_ADDRESSES_SIZE,
	};
	uint8_t *sbl = NULL;
	size_t sbl_length = 0;
	thread_t thread;
	uint64_t until, ns;
	bool done = false, loc_asked = false;
	int error;

	say("modem: modem=3: its firmware\n");
	(void)PE_parse_boot_argn("modem_mission", &mdm_mission, sizeof(mdm_mission));
	(void)PE_parse_boot_argn("modem_loc", &mdm_loc_enabled, sizeof(mdm_loc_enabled));
	(void)PE_parse_boot_argn("modem_gnss", &mdm_gnss_enabled, sizeof(mdm_gnss_enabled));
	mdm_gnss_enabled = mdm_gnss_enabled == 1 && mdm_loc_enabled == 1 && mdm_mission == 1;
	config.channel_count = mdm_gnss_enabled ? 6 : 4;
	if (!mdm_load_sbl(&sbl, &sbl_length) || !modem_reset() || !pcie_on()) {
		pins_say("no link");
		goto end;
	}
	mhi_look();
	sid_table();
	if (!mdm_pool() || !mdm_smmu_on()) {
		goto end;
	}
	config.image = sbl;
	config.image_length = config.sbl_length = sbl_length;
	if ((error = modem_mhi_host_init(&mdm_mhi, &mdm_ops, NULL, &config)) != 0) {
		say("modem: mhi: %d\n", error);
		goto end;
	}
	mdm_check_image(sbl, sbl_length);
	set_bits(pcie.dbi + 0x04, 0, 0x4);      /* the bridge: bus master */
	set_bits(pcie.conf + 0x04, 0, 0x4);     /* the device: the SMMU has it now */
	mdm_msi_on();
	mdm_polling = true;
	if (kernel_thread_start(poll_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	error = mdm_mhi_to_sbl(&mdm_mhi);
	if (mdm_msi_page != NULL) {
		say("modem: the MSI page reads %#x %#x\n", mdm_msi_page[0], mdm_msi_page[1]);
	}
	if (error != 0) {
		say("modem: to its boot loader: %d\n", error);
		mdm_smmu_say_faults();
		goto end;
	}
	modem_sahara_init(&sahara, &mdm_sahara_ops, NULL, chunk);
	nanoseconds_to_absolutetime(120 * NSEC_PER_SEC, &until);
	until += mach_absolute_time();
	while (!done && mach_absolute_time() < until) {
		if (instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL)) {
			say("modem: MDM2AP ERRFATAL is high: the modem has crashed\n");
			mdm_smmu_say_faults();
			goto end;
		}
		if (mdm_in.tail == mdm_in.head) {
			assert_wait_timeout((event_t)&mdm_in, THREAD_UNINT, 100, NSEC_PER_MSEC);
			if (mdm_in.tail != mdm_in.head) {
				clear_wait(current_thread(), THREAD_AWAKENED);
			} else {
				thread_block(THREAD_CONTINUE_NULL);
			}
			continue;
		}
		{
			uint32_t at = mdm_in.tail % MDM_PACKETS;
			enum modem_sahara_result r = modem_sahara_input(&sahara, mdm_in.data[at], mdm_in.length[at]);

			mdm_in.tail++;
			if (r == MODEM_SAHARA_FAILED) {
				say("modem: Sahara stops here\n");
				mdm_smmu_say_faults();
				goto end;
			}
			done = r == MODEM_SAHARA_DONE;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	say("modem: Sahara %s: %llu MB sent in %u images\n", done ? "is done" : "TIMED OUT", sahara.total >> 20, sahara.images);
	for (unsigned i = 0; i < sahara.images; i++) {
		say("modem:   image %u: %llu KB\n", sahara.ids[i], sahara.sent[i] >> 10);
	}
	if (!done || (error = mdm_mhi_to_mission(&mdm_mhi, 30000)) != 0) {
		goto end;
	}
	if (mdm_gnss_enabled) {
		uint64_t address;
		uint8_t *buffer = op_dma_alloc(NULL, MDM_SFR_LENGTH, 4096, &address);

		if (buffer != NULL) {
			error = mdm_mhi_sfr_config(&mdm_mhi, address, MDM_SFR_LENGTH);
			say("modem: the failure reason's buffer, %u bytes, given: %d\n", MDM_SFR_LENGTH, error);
			mdm_sfr = error == 0 ? (volatile uint8_t *)buffer : NULL;
		}
	}
	/* About a minute; three with the one GNSS try. A hard limit either way: this loop's end is what stops the firmware. */
	for (unsigned tenths = 0; tenths < (mdm_gnss_enabled ? 1800u : 600u); tenths++) {
		if (tenths % 100 == 0) {
			say("modem: %u s in its mission: MDM2AP status %u, errfatal %u, EE %u, state %#x\n", tenths / 10, instantnoodle_gpio_level(PIN_MDM2AP_STATUS),
			    instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL), mdm_mhi_ee(&mdm_mhi), mdm_mhi_state(&mdm_mhi));
		}
		if (instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL)) {
			say("modem: MDM2AP ERRFATAL is high: the modem has crashed\n");
			delay_for_interval(2, NSEC_PER_SEC);    /* (the text may come after the pin) */
			mdm_say_reason();
			break;
		}
		if (mdm_qrtr_hello_due) {
			mdm_qrtr_hello_due = false;
			mdm_qrtr_hello();
		}
		if (mdm_loc_enabled && mdm_loc_port != 0 && !loc_asked) {
			loc_asked = true;
			mdm_loc_ask(MDM_LOC_GET_ENGINE_LOCK);
			mdm_loc_ask(MDM_LOC_GET_OP_MODE);
			if (mdm_gnss_enabled) {
				mdm_gnss_start();
			}
		}
		if (mdm_resume_due) {
			mdm_resume_due = false;
			mdm_qrtr_resume();
		}
		if (mdm_efs_due != 0) {
			mdm_efs_answer();
		}
		delay_for_interval(100, NSEC_PER_MSEC);
	}
	if (mdm_gnss.started_ms != 0) {
		mdm_gnss_stop();
		delay_for_interval(500, NSEC_PER_MSEC);
		say("modem: gnss: the end, %llu s after the start: %u satellite reports, at most %u listed and %u with a signal, the best %u.%u dB-Hz; first signal %llu ms; %u position reports, %u fixes, first fix %llu ms, the best uncertainty %u.%u m\n",
		    (mdm_gnss_ms() - mdm_gnss.started_ms) / 1000, mdm_gnss.sv_reports, mdm_gnss.most_listed, mdm_gnss.most_with_signal, mdm_gnss.best_signal / 10, mdm_gnss.best_signal % 10,
		    mdm_gnss.first_signal_ms != 0 ? mdm_gnss.first_signal_ms - mdm_gnss.started_ms : 0, mdm_gnss.position_reports, mdm_gnss.fixes,
		    mdm_gnss.first_fix_ms != 0 ? mdm_gnss.first_fix_ms - mdm_gnss.started_ms : 0, mdm_gnss.best_uncertainty / 10, mdm_gnss.best_uncertainty % 10);
	}
	say("modem: qrtr: %u packets in, %u services offered, %u looked for\n", mdm_qrtr_in, mdm_qrtr_servers, mdm_qrtr_lookups);
	mdm_smmu_say_faults();
end:
	mdm_polling = false;
	delay_for_interval(20, NSEC_PER_MSEC);  /* the poll thread out of the device's window */
	hk_modem_quiesce();                     /* no link and no firmware now, however long the run goes on */
	delay_for_interval(1, NSEC_PER_SEC);
	pins_say("a second after the last reset");
	say("modem: level 3 is done\n");
}
