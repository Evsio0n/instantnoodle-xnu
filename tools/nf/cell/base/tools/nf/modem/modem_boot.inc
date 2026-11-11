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
#include "mdm_radio_status.h"
#include "mdm_wds_read.h"
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
#include <sys/sysctl.h>
#include <sys/hk_qmi.h>
#include <sys/hk_qmi_loc.h>
#include <sys/hk_qmi_radio.h>
#include <sys/hk_qrtr_wire.h>
#include <sys/hk_qrtr_mhi.h>
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
static bool mdm_ip_hw0_diagnostic;
static bool mdm_abp_diagnostic;
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
		if (mdm_ip_hw0_diagnostic || mdm_abp_diagnostic) {
			say("modem: %s SMMU fault local=%#x global=%#x\n", mdm_abp_diagnostic?"abp-board:":"IP_HW0", status & SMMU_FSR_ALL, global);
			return;
		}
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
		uint32_t local = 0, global = 0;
		if (mdm_ip_hw0_diagnostic || mdm_abp_diagnostic) {
			local = rd(mdm.context_page + SMMU_CB_FSR) & SMMU_FSR_ALL;
			global = rd(mdm.global + SMMU_SGFSR);
		} else {
			mdm_smmu_say_faults();
		}
		wr_sync(mdm.global + SMMU_SMR(mdm.match), mdm.old_smr);
		wr_sync(mdm.global + SMMU_S2CR(mdm.match), mdm.old_s2cr);
		wr_sync(mdm.context_page + SMMU_CB_SCTLR, mdm.old_sctlr);
		if (mdm_ip_hw0_diagnostic || mdm_abp_diagnostic) {
			bool smr = rd(mdm.global + SMMU_SMR(mdm.match)) == mdm.old_smr;
			bool s2cr = rd(mdm.global + SMMU_S2CR(mdm.match)) == mdm.old_s2cr;
			bool sctlr = rd(mdm.context_page + SMMU_CB_SCTLR) == mdm.old_sctlr;
			say("modem: %s SMMU restore perst=%u smr_match=%u s2cr_match=%u sctlr_match=%u local_fault=%#x global_fault=%#x\n",
			    mdm_abp_diagnostic?"abp-board:":"IP_HW0", instantnoodle_gpio_level(PIN_PERST), smr, s2cr, sctlr, local, global);
		}
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

static struct mhi_host mdm_mhi;

static void
op_write(void *c, uint32_t offset, uint32_t value)
{
	(void)c;
	if (mdm_link_up()) {
		wr_sync(pcie.bar + offset, value);
		mdm_mhi_watch_write(&mdm_mhi,offset,value,true);
	} else {
		mdm_mhi_watch_write(&mdm_mhi,offset,value,false);
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
#define MDM_QRTR_MAX_SERVERS 64u
struct mdm_qrtr_server {
	uint32_t service, instance, node, port;
	bool there;
};
static struct mdm_qrtr_server mdm_qrtr_server_table[MDM_QRTR_MAX_SERVERS];

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
static uint32_t mdm_radio_enabled;
static uint32_t mdm_wds_settings_enabled;
enum mdm_radio_slot_id {
	MDM_RADIO_DMS,
	MDM_RADIO_NAS,
	MDM_RADIO_WDS,
	MDM_RADIO_UIM,
	MDM_RADIO_WDA,
	MDM_RADIO_TS,
	MDM_RADIO_SLOTS,
};
/* Auxiliary endpoint results stay outside the original version-1 iOS ABI. */
static struct {
	uint32_t generation;
	bool present;
} mdm_aux[MDM_RADIO_SLOTS - MDM_RADIO_UIM];
static LCK_MTX_DECLARE(mdm_radio_status_lock, &mdm_lock_group);
static struct sockaddr_qrtr mdm_wds_peer; /* Held under mdm_radio_status_lock. */
static struct hk_radio_snapshot mdm_radio_status = {
    .version = HK_RADIO_VERSION, .size = sizeof(struct hk_radio_snapshot)
};
static void mdm_radio_phase(unsigned phase)
{
    lck_mtx_lock(&mdm_radio_status_lock);
    hk_radio_reset(&mdm_radio_status, phase);
    for (unsigned i = 0; i < sizeof(mdm_aux) / sizeof(mdm_aux[0]); i++)
        mdm_aux[i].present = false;
    lck_mtx_unlock(&mdm_radio_status_lock);
}
static void mdm_radio_endpoint(unsigned kind, unsigned state, uint32_t node, uint32_t port)
{
    lck_mtx_lock(&mdm_radio_status_lock);
    if (kind == HK_RADIO_PACKET)
        mdm_wds_peer = (struct sockaddr_qrtr){ .sq_family = AF_QIPCRTR, .sq_node = node, .sq_port = port };
    if (kind >= MDM_RADIO_UIM && kind < MDM_RADIO_SLOTS) {
        unsigned i = kind - MDM_RADIO_UIM;
        if (++mdm_aux[i].generation == 0) ++mdm_aux[i].generation;
        mdm_aux[i].present = mdm_radio_status.phase == HK_RADIO_ACTIVE && state == HK_RADIO_ENDPOINT;
    } else if (mdm_radio_status.phase == HK_RADIO_ACTIVE)
        hk_radio_invalidate(&mdm_radio_status, kind, state);
    lck_mtx_unlock(&mdm_radio_status_lock);
}
static uint32_t mdm_radio_query_begin(unsigned kind)
{
    lck_mtx_lock(&mdm_radio_status_lock);
    uint32_t token = hk_radio_begin(&mdm_radio_status, kind);
    lck_mtx_unlock(&mdm_radio_status_lock);
    return token;
}
static void mdm_radio_query_finish(unsigned kind,uint32_t token,int bytes,
    unsigned result,unsigned error,int valid,int value,int detail)
{
    lck_mtx_lock(&mdm_radio_status_lock);
    hk_radio_finish(&mdm_radio_status,kind,token,bytes,result,error,valid,value,
        detail,op_nanoseconds(NULL)/1000000);
    lck_mtx_unlock(&mdm_radio_status_lock);
}
static int mdm_radio_status_sysctl SYSCTL_HANDLER_ARGS
{
    struct hk_radio_snapshot copy;
    lck_mtx_lock(&mdm_radio_status_lock);
    copy=mdm_radio_status;
    copy.read_ms=op_nanoseconds(NULL)/1000000;
    lck_mtx_unlock(&mdm_radio_status_lock);
    return SYSCTL_OUT(req,&copy,sizeof(copy));
}
#include "gnss_sv_observe.h"
static struct mdm_sv_wire_counts mdm_sv_counts;
static volatile uint32_t mdm_loc_node, mdm_loc_port;
static bool mdm_qmi_generic;
static volatile bool mdm_resume_due;
static volatile uint32_t mdm_resume_node, mdm_resume_port;

/*
 * modem_gnss=1, the one try the owner allowed by a window (2026-11-06): what the engine reports is counted, and of a position only its uncertainty, status and the number of satellites
 * used. Coordinates, altitude, speed, heading, time of week, satellites' ids and directions are never read out of the packet, let alone kept or printed.
 */
static uint32_t mdm_gnss_enabled;
#include "gnss_fix.h"
#include <mach/mach_time.h>
static LCK_MTX_DECLARE(mdm_fix_lock, &mdm_lock_group);
static struct hk_fix_session mdm_fix;
static bool mdm_fix_enabled;
static uint64_t
mdm_fix_now(void)
{
    uint64_t ns;
    absolutetime_to_nanoseconds(mach_continuous_time(),&ns);
    return ns/1000000;
}
static void
mdm_fix_close(void)
{
    if (!mdm_fix_enabled) return;
    lck_mtx_lock(&mdm_fix_lock);hk_fix_end(&mdm_fix);lck_mtx_unlock(&mdm_fix_lock);
}
static void
mdm_fix_tick(void)
{
    if (!mdm_fix_enabled) return;
    lck_mtx_lock(&mdm_fix_lock);hk_fix_expire(&mdm_fix,mdm_fix_now());lck_mtx_unlock(&mdm_fix_lock);
}
static void
mdm_fix_packet(const struct hk_qrtr_wire_datagram *d)
{
    if (!mdm_fix_enabled || d->type!=MDM_QRTR_DATA || d->to_node!=MDM_QRTR_US ||
        d->payload_length<7 || d->payload[0]!=QMI_INDICATION || hk_fix_le(d->payload+3,2)!=0x24) return;
    lck_mtx_lock(&mdm_fix_lock);
    if(hk_fix_matches(&mdm_fix,d->from_node,d->from_port,d->to_port)){
        clock_sec_t sec;
        clock_usec_t usec;
        clock_get_calendar_microtime(&sec,&usec);
        hk_fix_observe(&mdm_fix,d->payload,d->payload_length,mdm_fix_now(),(uint64_t)sec*1000+usec/1000);
    }
    lck_mtx_unlock(&mdm_fix_lock);
}
/* Coordinate-free diagnostic output. Raw coordinates and precise UTC never leave mdm_fix. */
static int
mdm_fix_metadata SYSCTL_HANDLER_ARGS
{
    struct {uint32_t version,state,accepted,accuracy_bits;uint64_t age_ms;} out={.version=1};
    uint64_t now=mdm_fix_now();
    lck_mtx_lock(&mdm_fix_lock);
    hk_fix_expire(&mdm_fix,now);out.state=mdm_fix.state;out.accepted=mdm_fix.accepted;
    if(mdm_fix.state==HK_FIX_READY){
        out.age_ms=now-mdm_fix.value.observed_ms+mdm_fix.value.age_at_arrival_ms;out.accuracy_bits=mdm_fix.value.horizontal_uncertainty_bits;
    }
    lck_mtx_unlock(&mdm_fix_lock);
    return SYSCTL_OUT(req,&out,sizeof(out));
}

/* Read-only monitor state.  A request being sent is not an accepted GNSS
 * session, and returning to the loader is not proof of satellite reception.
 * Keep the terminal cause after teardown so the phone page can explain why
 * counters never began.  No extra register or modem transaction is needed. */
enum {
	MDM_GNSS_OFF, MDM_GNSS_BOOTING, MDM_GNSS_SAHARA, MDM_GNSS_MISSION,
	MDM_GNSS_STARTING, MDM_GNSS_ACTIVE, MDM_GNSS_LEGACY_PENDING,
	MDM_GNSS_FAILED, MDM_GNSS_STOPPED
};
enum {
	MDM_GNSS_ERROR_NONE, MDM_GNSS_ERROR_BOOT, MDM_GNSS_ERROR_SAHARA,
	MDM_GNSS_ERROR_RDDM, MDM_GNSS_ERROR_MISSION, MDM_GNSS_ERROR_ERRFATAL,
	MDM_GNSS_ERROR_LOC_MISSING
};
static volatile uint32_t mdm_gnss_stage, mdm_gnss_failure;
static struct {
	uint64_t started_ms, first_signal_ms, first_fix_ms, last_said_ms;
	uint32_t sv_reports, position_reports, fixes;
	uint32_t most_listed, most_with_signal, best_signal, best_uncertainty;
	bool shape_said;
} mdm_gnss;

/* A small, privacy-safe status feed for the on-phone GNSS monitor.  It exposes
 * counters and state only; coordinates, satellite identities and modem files
 * never leave the QMI parser. */
static int
mdm_gnss_sysctl SYSCTL_HANDLER_ARGS
{
	char text[512];
	int n = snprintf(text, sizeof(text),
		"enabled=%u started=%u sv_reports=%u listed=%u signal=%u best_dbhz10=%u position_reports=%u fixes=%u uncertainty_m10=%u qrtr_services=%u stage=%u failure=%u sv_wire=%u sv_absent=%u sv_empty=%u sv_bad=%u sv_no_cn0=%u",
		mdm_gnss_enabled, mdm_gnss.started_ms != 0, mdm_gnss.sv_reports,
		mdm_gnss.most_listed, mdm_gnss.most_with_signal, mdm_gnss.best_signal,
		mdm_gnss.position_reports, mdm_gnss.fixes, mdm_gnss.best_uncertainty,
		mdm_qrtr_servers, mdm_gnss_stage, mdm_gnss_failure, mdm_sv_counts.indications,
		mdm_sv_counts.absent, mdm_sv_counts.empty, mdm_sv_counts.malformed, mdm_sv_counts.no_cn0);
	if (n < 0 || (size_t)n >= sizeof(text))
		return EIO;
	return SYSCTL_OUT(req, text, (size_t)n + 1);
}

SYSCTL_DECL(_kern_hackios);
SYSCTL_PROC(_kern_hackios,OID_AUTO,modem_fix_quality,CTLTYPE_OPAQUE|CTLFLAG_RD|CTLFLAG_LOCKED,
    0,0,mdm_fix_metadata,"S,mdm_fix_quality","GNSS fix status and accuracy only");
SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_radio_status,
    CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
    mdm_radio_status_sysctl, "S,hk_radio_snapshot", "last observed DMS, NAS and WDS results");
SYSCTL_PROC(_kern_hackios, OID_AUTO, modem_gnss,
	CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0,
	mdm_gnss_sysctl, "A", "modem GNSS counters without location data");

static uint32_t
mdm_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
mdm_le64(const uint8_t *p)
{
	return (uint64_t)mdm_le32(p) | (uint64_t)mdm_le32(p + 4) << 32;
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

static void
mdm_qrtr_server_drop(unsigned i)
{
	struct mdm_qrtr_server *s = &mdm_qrtr_server_table[i];

	if (!s->there) {
		return;
	}
	if (mdm_loc_node == s->node && mdm_loc_port == s->port) {
		mdm_fix_close();
		mdm_loc_node = mdm_loc_port = 0;
	}
	s->there = false;
	if (mdm_qrtr_servers != 0) {
		mdm_qrtr_servers--;
	}
}

static void
mdm_qrtr_server_add(uint32_t service, uint32_t instance, uint32_t node, uint32_t port)
{
	int free_slot = -1;

	for (unsigned i = 0; i < MDM_QRTR_MAX_SERVERS; i++) {
		struct mdm_qrtr_server *s = &mdm_qrtr_server_table[i];

		if (s->there && s->node == node && s->port == port) {
			if (s->service == service && s->instance == instance) {
				if (service == MDM_LOC_SERVICE) {
					mdm_loc_node = node;
					mdm_loc_port = port;
				}
				return;                 /* a repeated NEW_SERVER is not a new live service */
			}
			mdm_qrtr_server_drop(i);
		}
		if (!s->there && free_slot < 0) {
			free_slot = (int)i;
		}
	}
	if (free_slot < 0) {
		return;
	}
	mdm_qrtr_server_table[free_slot] = (struct mdm_qrtr_server){
		.service = service, .instance = instance, .node = node, .port = port, .there = true,
	};
	mdm_qrtr_servers++;
	if (service == MDM_LOC_SERVICE) {
		mdm_loc_node = node;
		mdm_loc_port = port;
	}
}

static void
mdm_qrtr_server_del(uint32_t node, uint32_t port)
{
	for (unsigned i = 0; i < MDM_QRTR_MAX_SERVERS; i++) {
		if (mdm_qrtr_server_table[i].there && mdm_qrtr_server_table[i].node == node && mdm_qrtr_server_table[i].port == port) {
			mdm_qrtr_server_drop(i);
		}
	}
}

static void
mdm_qrtr_server_bye(uint32_t node)
{
	for (unsigned i = 0; i < MDM_QRTR_MAX_SERVERS; i++) {
		if (mdm_qrtr_server_table[i].there && mdm_qrtr_server_table[i].node == node) {
			mdm_qrtr_server_drop(i);
		}
	}
}

static bool
mdm_qrtr_radio_service(uint32_t service)
{
	return service == HK_QMI_DMS_SERVICE || service == HK_QMI_NAS_SERVICE ||
	    service == HK_QMI_WDS_SERVICE || service == HK_QMI_UIM_SERVICE || service == HK_QMI_WDA_SERVICE ||
	    service == HK_QMI_TS_SERVICE;
}

static bool mdm_radio_data(const struct hk_qrtr_wire_datagram *);

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
					const uint8_t *entry=q + at + 4 + i * MDM_SV_ENTRY;
					if (!mdm_sv_has_cn0(entry)) continue;
					uint32_t signal = mdm_tenths(mdm_le32(entry + 24));

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
mdm_qmi_position(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq,
	__unused struct qmi_txn *txn, const void *decoded)
{
	const struct hk_qmi_loc_position_report_ind *p = decoded;
	uint32_t status, uncertainty, used;
	uint64_t now;

	if (!mdm_qmi_generic || decoded == NULL) {
		return;
	}
	status = p->status_valid ? p->status : ~0u;
	uncertainty = p->uncertainty_valid ? mdm_tenths(p->uncertainty_bits) : ~0u;
	used = p->satellites_used_valid ? p->satellites_used_len : 0;
	now = mdm_gnss_ms();
	mdm_gnss.position_reports++;
	if (uncertainty != ~0u && (mdm_gnss.best_uncertainty == 0 || uncertainty < mdm_gnss.best_uncertainty)) {
		mdm_gnss.best_uncertainty = uncertainty;
	}
	if (status == 0) {
		mdm_gnss.fixes++;
		if (mdm_gnss.first_fix_ms == 0) {
			mdm_gnss.first_fix_ms = now;
			say("modem: gnss: FIRST FIX %llu ms after the start: uncertainty %u.%u m, %u satellites used\n",
			    mdm_gnss.first_fix_ms - mdm_gnss.started_ms, uncertainty / 10, uncertainty % 10, used);
		}
	}
	if (mdm_gnss.position_reports == 1 || mdm_gnss.position_reports % 30 == 0) {
		say("modem: gnss: report %u: status %u, %s%u.%u m, %u satellites used\n", mdm_gnss.position_reports, status,
		    uncertainty == ~0u ? "no uncertainty, " : "", uncertainty == ~0u ? 0 : uncertainty / 10,
		    uncertainty == ~0u ? 0 : uncertainty % 10, used);
	}
}

static void
mdm_qmi_sv_info(__unused struct qmi_handle *qmi, __unused struct sockaddr_qrtr *sq,
	__unused struct qmi_txn *txn, const void *decoded)
{
	const struct hk_qmi_loc_sv_info_ind *sv = decoded;
	uint32_t listed, with_signal = 0, best = 0;
	uint64_t now;

	if (!mdm_qmi_generic || decoded == NULL) {
		return;
	}
	listed = sv->entries_len;
	if (listed > 64) {
		listed = 64;
	}
	for (uint32_t i = 0; i < listed; i++) {
		const uint8_t *entry = sv->entries[i].bytes;
		uint32_t signal = mdm_sv_has_cn0(entry) ? mdm_tenths(mdm_le32(entry + 24)) : ~0u;

		if (signal != ~0u && signal > 0) {
			with_signal++;
			best = signal > best ? signal : best;
		}
	}
	now = mdm_gnss_ms();
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
		say("modem: gnss: +%llu s: %u satellites listed, %u with a signal, the best %u.%u dB-Hz\n",
		    (now - mdm_gnss.started_ms) / 1000, listed, with_signal, best / 10, best % 10);
	}
}

static const struct qmi_msg_handler mdm_qmi_handlers[] = {
	{ QMI_INDICATION, MDM_LOC_POSITION_REPORT, hk_qmi_loc_position_report_ind_ei,
	    sizeof(struct hk_qmi_loc_position_report_ind), mdm_qmi_position },
	{ QMI_INDICATION, MDM_LOC_SV_INFO, hk_qmi_loc_sv_info_ind_ei,
	    sizeof(struct hk_qmi_loc_sv_info_ind), mdm_qmi_sv_info },
	{ 0, 0, NULL, 0, NULL },
};

static void
mdm_loc_input(const uint8_t *q, size_t length)
{
	static const char *const kinds[] = { "request", "?", "response", "?", "indication" };
	uint16_t message, size;

	/* Once the table-backed client has completed its start sequence, the generic
	 * handlers own replies and indications.  The legacy parser still receives
	 * the same QRTR datagram for diagnostics, but must not count it a second time. */
	if (mdm_qmi_generic) {
		return;
	}

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
mdm_qrtr_input(const uint8_t *data, size_t length, bool adapter_accepted)
{
	struct hk_qrtr_wire_datagram d;
	struct hk_qrtr_wire_control control;

	if (hk_qrtr_wire_decode(data, length, 8192u, &d) != 0) {
		say("modem: qrtr: malformed packet of %zu bytes\n", length);
		return;
	}
	mdm_qrtr_in++;
	if (d.confirm && !adapter_accepted) {
		mdm_resume_node = d.from_node;
		mdm_resume_port = d.from_port;
		mdm_resume_due = true;
	}
	if (d.type == MDM_QRTR_RESUME_TX) {
		return;
	}
	if (d.to_port == MDM_QRTR_CONTROL) {
		if (hk_qrtr_wire_control_decode(&d, &control) != 0) {
			say("modem: qrtr: short control packet of %zu bytes\n", d.payload_length);
			return;
		}
		switch (control.command) {
		case MDM_QRTR_HELLO:
			say("modem: qrtr: HELLO FROM NODE %u (version %u, %zu bytes)\n", d.from_node, d.version, length);
			/* hk_qrtr_core answers HELLO inline for packets it accepted;
			 * retain the delayed legacy reply only for fallback packets. */
			if (!adapter_accepted) {
				mdm_qrtr_peer = d.from_node;
				mdm_qrtr_hello_due = true;
			}
			return;
		case MDM_QRTR_NEW_SERVER:
			if (d.payload_length < 20) {
				say("modem: qrtr: short NEW_SERVER packet of %zu bytes\n", d.payload_length);
				return;
			}
			if (mdm_radio_enabled && mdm_qrtr_radio_service(control.words[0])) {
				say("modem: qrtr: radio service %u announced\n", control.words[0]);
			} else {
				say("modem: qrtr: it offers service %u (%#x), version %u, instance %u, at node %u port %u\n", control.words[0], control.words[0], control.words[1] & 0xff,
				    control.words[1] >> 8, control.words[2], control.words[3]);
			}
			mdm_qrtr_server_add(control.words[0], control.words[1], control.words[2], control.words[3]);
			return;
		case MDM_QRTR_DEL_SERVER:
			if (d.payload_length < 20) {
				say("modem: qrtr: short DEL_SERVER packet of %zu bytes\n", d.payload_length);
				return;
			}
			if (mdm_radio_enabled && mdm_qrtr_radio_service(control.words[0])) {
				say("modem: qrtr: radio service %u has gone\n", control.words[0]);
			} else {
				say("modem: qrtr: service %u at node %u port %u has gone\n", control.words[0], control.words[2], control.words[3]);
			}
			mdm_qrtr_server_del(control.words[2], control.words[3]);
			return;
		case MDM_QRTR_BYE:
			mdm_qrtr_server_bye(d.from_node);
			return;
		case MDM_QRTR_NEW_LOOKUP:
			if (d.payload_length < 12) {
				say("modem: qrtr: short NEW_LOOKUP packet of %zu bytes\n", d.payload_length);
				return;
			}
			mdm_qrtr_lookups++;
			say("modem: qrtr: it looks for service %u (%#x), instance %u: nobody here offers it\n", control.words[0], control.words[0], control.words[1]);
			return;
		default:
			return;
		}
	}
	if (adapter_accepted) mdm_fix_packet(&d);
	if (mdm_gnss.started_ms && d.type == MDM_QRTR_DATA && d.from_node == mdm_loc_node && d.from_port == mdm_loc_port)
		mdm_sv_observe(&mdm_sv_counts,d.payload,d.payload_length);
	if (d.type == MDM_QRTR_DATA && d.from_node == mdm_loc_node && d.from_port == mdm_loc_port && d.to_port == MDM_QRTR_OUR_PORT) {
		mdm_loc_input(d.payload, d.payload_length);
		return;
	}
	if (mdm_radio_data(&d)) {
		return;
	}
	if (d.type == MDM_QRTR_DATA) {
		say("modem: qrtr: data, %zu bytes, from %u:%u to %u:%u: nobody takes it\n", d.payload_length, d.from_node, d.from_port, d.to_node, d.to_port);
	}
}

/*
 * The modem's file sync (2026-11-06, run 113829's reason: remotefs_sahara.c:850).
 * The modem offers a Sahara memory-debug exchange on the EFS pair, 10 and 11,
 * during a GNSS session.  Serve the exchange read-only and discard every byte:
 * the table and the files are never logged or written to UFS.  This is the
 * smallest useful host, but unlike the old refusal it lets the modem complete
 * SAHARA_END_OF_IMG_TRANSFER before it continues its mission.
 */
#define MDM_EFS_OUT             10
#define MDM_EFS_IN              11
#define MDM_EFS_READ_CMD        0xa
#define MDM_EFS_READ64_CMD      0x11
#define MDM_EFS_READ_CHUNK      0xfff0u
#define MDM_EFS_MAX_TABLE       4096u
#define MDM_EFS_MAX_ENTRIES     64u
#define MDM_EFS_MAX_TRANSFER    (8u * 1024u * 1024u)
enum { MDM_EFS_HELLO_DUE = 1, MDM_EFS_READ_DUE = 2, MDM_EFS_RESET_DUE = 3 };
enum { MDM_EFS_IDLE, MDM_EFS_TABLE, MDM_EFS_FILE, MDM_EFS_HELLO };
struct mdm_efs_entry { uint64_t address, length; };
static volatile uint32_t mdm_efs_due;
static uint32_t mdm_efs_mode, mdm_efs_state, mdm_efs_packets, mdm_efs_word_size;
static uint64_t mdm_efs_address, mdm_efs_remaining, mdm_efs_expected, mdm_efs_received;
static uint8_t mdm_efs_table[MDM_EFS_MAX_TABLE];
static uint32_t mdm_efs_table_length, mdm_efs_entry_count, mdm_efs_entry_index;
static struct mdm_efs_entry mdm_efs_entries[MDM_EFS_MAX_ENTRIES];
/* The combined mission observes only a fixed control header. The legacy
 * GNSS handler has a different contract and is never entered in this profile. */
static bool mdm_efs_observe_only;
static unsigned mdm_efs_seen[6];
static void
mdm_efs_header(const void *data, size_t length)
{
	const uint8_t *p=data;
	unsigned kind=5;
	if (length >= 8 && length <= 48 && mdm_le32(p+4)==length) {
		uint32_t cmd=mdm_le32(p);
		kind=4;
		if (cmd==1) {
			kind=5;
			if (length==48 && mdm_le32(p+8)>0 && mdm_le32(p+12)>0 && mdm_le32(p+12)<=mdm_le32(p+8)) {
				uint32_t mode=mdm_le32(p+20);
				kind=mode==2 ? 1 : mode<=1 ? 0 : 2;
			}
		} else if ((cmd==9 && length==16) || (cmd==16 && length==24)) {
			kind=3; /* Stop at the fixed header, before any table address. */
		}
	}
	if (__atomic_fetch_add(&mdm_efs_seen[kind],1,__ATOMIC_RELAXED)==0)
		say("modem: EFS mission header class=%u rx=11 payload_request=0 sync_completed=0\n",kind);
}

#define MDM_EFS_QUIET_AFTER     32

static void
mdm_efs_reset_due(void)
{
	mdm_efs_state = MDM_EFS_IDLE;
	mdm_efs_due = MDM_EFS_RESET_DUE;
}

static void
mdm_efs_request(uint64_t address, uint64_t length, uint32_t state)
{
	if (length == 0) {
		mdm_efs_entry_index++;
		mdm_efs_state = MDM_EFS_IDLE;
		mdm_efs_due = MDM_EFS_READ_DUE;
		return;
	}
	if (length > MDM_EFS_MAX_TRANSFER || address + length < address) {
		say("modem: efs: refusing an oversized memory transfer (%llu bytes)\n", (unsigned long long)length);
		mdm_efs_reset_due();
		return;
	}
	mdm_efs_address = address;
	mdm_efs_remaining = length;
	mdm_efs_expected = 0;
	mdm_efs_received = 0;
	mdm_efs_state = state;
	mdm_efs_due = MDM_EFS_READ_DUE;
}

static void
mdm_efs_next_entry(void)
{
	while (mdm_efs_entry_index < mdm_efs_entry_count && mdm_efs_entries[mdm_efs_entry_index].length == 0) {
		mdm_efs_entry_index++;
	}
	if (mdm_efs_entry_index == mdm_efs_entry_count) {
		mdm_efs_reset_due();
		return;
	}
	mdm_efs_request(mdm_efs_entries[mdm_efs_entry_index].address,
	    mdm_efs_entries[mdm_efs_entry_index].length, MDM_EFS_FILE);
}

static void
mdm_efs_table_done(void)
{
	uint32_t width = mdm_efs_word_size == 8 ? 64 : 52;

	if (mdm_efs_table_length == 0 || mdm_efs_table_length % width != 0 || mdm_efs_table_length / width > MDM_EFS_MAX_ENTRIES) {
		say("modem: efs: unusable table length %u\n", mdm_efs_table_length);
		mdm_efs_reset_due();
		return;
	}
	mdm_efs_entry_count = mdm_efs_table_length / width;
	mdm_efs_entry_index = 0;
	for (uint32_t i = 0; i < mdm_efs_entry_count; i++) {
		const uint8_t *p = mdm_efs_table + i * width;
		mdm_efs_entries[i].address = mdm_efs_word_size == 8 ? mdm_le64(p + 8) : mdm_le32(p + 4);
		mdm_efs_entries[i].length = mdm_efs_word_size == 8 ? mdm_le64(p + 16) : mdm_le32(p + 8);
	}
	say("modem: efs: table received (%u bytes, %u entries)\n", mdm_efs_table_length, mdm_efs_entry_count);
	mdm_efs_next_entry();
}

static void
mdm_efs_data(const uint8_t *p, size_t length)
{
	uint64_t left = mdm_efs_expected - mdm_efs_received;

	if ((uint64_t)length > left) {
		say("modem: efs: raw read is too long (%zu > %llu)\n", length, (unsigned long long)left);
		mdm_efs_reset_due();
		return;
	}
	if (mdm_efs_state == MDM_EFS_TABLE) {
		if (mdm_efs_received + length > sizeof(mdm_efs_table)) {
			say("modem: efs: table is too large\n");
			mdm_efs_reset_due();
			return;
		}
		memcpy(mdm_efs_table + mdm_efs_received, p, length);
	}
	mdm_efs_received += length;
	if (mdm_efs_received != mdm_efs_expected) {
		return;
	}
	if (mdm_efs_state == MDM_EFS_TABLE) {
		mdm_efs_table_done();
		return;
	}
	mdm_efs_address += mdm_efs_expected;
	mdm_efs_remaining -= mdm_efs_expected;
	if (mdm_efs_remaining != 0) {
		mdm_efs_request(mdm_efs_address, mdm_efs_remaining, MDM_EFS_FILE);
	} else {
		mdm_efs_entry_index++;
		mdm_efs_state = MDM_EFS_IDLE;
		mdm_efs_next_entry();
	}
}

static void
mdm_efs_input(const uint8_t *p, size_t length)
{
	/* A memory read is followed by raw bytes, not a Sahara header. */
	if ((mdm_efs_state == MDM_EFS_TABLE || mdm_efs_state == MDM_EFS_FILE) && mdm_efs_received < mdm_efs_expected) {
		mdm_efs_data(p, length);
		return;
	}
	uint32_t command = length >= 8 ? mdm_le32(p) : 0, declared = length >= 8 ? mdm_le32(p + 4) : 0;

	if (++mdm_efs_packets > MDM_EFS_QUIET_AFTER) {
		return;
	}
	say("modem: efs: channel %u: command %#x, says %u bytes, %zu came\n", MDM_EFS_IN, command, declared, length);
	if (command == 0x1 && length >= 24) {
		say("modem: efs:   hello, mode %u\n", mdm_le32(p + 20));
		/* The modem does not like a second hello answered. */
		if (mdm_efs_state == MDM_EFS_IDLE) {
			mdm_efs_mode = mdm_le32(p + 20);
			mdm_efs_word_size = 4;
			mdm_efs_state = MDM_EFS_HELLO;
			mdm_efs_due = MDM_EFS_HELLO_DUE;
		}
		return;
	}
	if (command == 0x8) {
		say("modem: efs:   the reset is answered\n");
		mdm_efs_state = MDM_EFS_IDLE;
		return;
	}
	if (command == 0x9 && length >= 16) {
		mdm_efs_word_size = 4;
		mdm_efs_table_length = mdm_le32(p + 12);
		say("modem: efs:   its table is %u bytes\n", mdm_efs_table_length);
		mdm_efs_request(mdm_le32(p + 8), mdm_efs_table_length, MDM_EFS_TABLE);
	} else if (command == 0x10 && length >= 24) {
		mdm_efs_word_size = 8;
		mdm_efs_table_length = (uint32_t)mdm_le64(p + 16);
		say("modem: efs:   its table is %u bytes\n", mdm_efs_table_length);
		mdm_efs_request(mdm_le64(p + 8), mdm_efs_table_length, MDM_EFS_TABLE);
	}
}

static struct hk_qrtr_mhi mdm_qrtr_mhi;
static bool mdm_qrtr_adapter_said;
static int mdm_qrtr_mhi_send(void *context, const void *packet, size_t length);
static void mdm_events_tx(const void *packet, size_t length, int rc);
static struct qmi_handle mdm_qmi;
/* Capture only the registered generic LOC handle's own destination port. */
static void
mdm_fix_open(uint8_t id)
{
    if(!mdm_fix_enabled || !mdm_qmi.sock)return;
    uint32_t local=0;
    for(unsigned i=0;i<HK_QRTR_CORE_MAX_PORTS;i++){
        const struct hk_qrtr_core_port *p=&mdm_qrtr_mhi.core.ports[i];
        if(p->receive && p->context==(void *)mdm_qmi.sock){local=p->number;break;}
    }
    lck_mtx_lock(&mdm_fix_lock);
    hk_fix_start(&mdm_fix,mdm_loc_node,mdm_loc_port,local,id);
    lck_mtx_unlock(&mdm_fix_lock);
}

static bool mdm_qmi_open, mdm_qmi_server;

struct mdm_radio_slot {
	struct qmi_handle handle;
	unsigned service;
	const char *name;
	bool open, connected, probed;
	uint32_t node, port;
};

_Static_assert((unsigned)MDM_RADIO_DMS==(unsigned)HK_RADIO_MODE &&
    (unsigned)MDM_RADIO_NAS==(unsigned)HK_RADIO_SIGNAL &&
    (unsigned)MDM_RADIO_WDS==(unsigned)HK_RADIO_PACKET &&
    (unsigned)MDM_RADIO_UIM==(unsigned)HK_RADIO_COUNT, "radio status slot order");
static struct mdm_radio_slot mdm_radio_slots[MDM_RADIO_SLOTS] = {
	[MDM_RADIO_DMS] = { .service = HK_QMI_DMS_SERVICE, .name = "DMS" },
	[MDM_RADIO_NAS] = { .service = HK_QMI_NAS_SERVICE, .name = "NAS" },
	[MDM_RADIO_WDS] = { .service = HK_QMI_WDS_SERVICE, .name = "WDS" },
	[MDM_RADIO_UIM] = { .service = HK_QMI_UIM_SERVICE, .name = "UIM" },
	[MDM_RADIO_WDA] = { .service = HK_QMI_WDA_SERVICE, .name = "WDA" },
	[MDM_RADIO_TS] = { .service = HK_QMI_TS_SERVICE, .name = "TS" },
};

static bool
mdm_radio_data(const struct hk_qrtr_wire_datagram *d)
{
	if (d == NULL || d->type != MDM_QRTR_DATA) {
		return false;
	}
	for (unsigned i = 0; i < MDM_RADIO_SLOTS; i++) {
		if (mdm_radio_slots[i].connected &&
		    ((d->from_node == mdm_radio_slots[i].node && d->from_port == mdm_radio_slots[i].port) ||
		     (d->to_node == mdm_radio_slots[i].node && d->to_port == mdm_radio_slots[i].port))) {
			return true;
		}
	}
	return false;
}

static struct mdm_radio_slot *
mdm_radio_slot_for(struct qmi_handle *qmi)
{
	for (unsigned i = 0; i < MDM_RADIO_SLOTS; i++) {
		if (&mdm_radio_slots[i].handle == qmi) {
			return &mdm_radio_slots[i];
		}
	}
	return NULL;
}

static int
mdm_radio_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct mdm_radio_slot *slot = mdm_radio_slot_for(qmi);
	int error;

	if (slot == NULL || service == NULL || service->service != slot->service || slot->connected ||
	    (slot->service == HK_QMI_TS_SERVICE && service->instance != 100)) {
		return ENXIO;
	}
	error = hk_qmi_connect(qmi->sock, service->node, service->port);
	if (error == 0) {
		slot->connected = true;
		slot->node = service->node;
		slot->port = service->port;
		mdm_radio_endpoint((unsigned)(slot - mdm_radio_slots), HK_RADIO_ENDPOINT, service->node, service->port);
		say("modem: qmi radio: %s endpoint ready\n", slot->name);
	} else {
		say("modem: qmi radio: %s endpoint unavailable\n", slot->name);
	}
	return error;
}

static void
mdm_radio_del_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	struct mdm_radio_slot *slot = mdm_radio_slot_for(qmi);

	if (slot == NULL || service == NULL ||
	    (slot->node != service->node || slot->port != service->port)) {
		return;
	}
	slot->connected = false;
	slot->probed = false;
	slot->node = slot->port = 0;
	mdm_radio_endpoint((unsigned)(slot - mdm_radio_slots), HK_RADIO_ABSENT, 0, 0);
	say("modem: qmi radio: %s endpoint gone\n", slot->name);
}

static const struct qmi_ops mdm_radio_ops = {
	.new_server = mdm_radio_new_server,
	.del_server = mdm_radio_del_server,
};

/* The status transaction already ran on the read-only radio handle. Publish
 * it immediately. The opt-in second read has no bearer or route operation. */
static void
mdm_wds_observe(struct mdm_radio_slot *slot, uint32_t token, int bytes,
                const struct hk_qmi_wds_packet_service_status_resp *response)
{
	if (mdm_wds_settings_enabled != 1) {
		mdm_radio_query_finish(HK_RADIO_PACKET, token, bytes, response->response.result,
		                       response->response.error, response->connection_status_valid,
		                       response->connection_status, 0);
		return;
	}
	uint64_t sample_ms = op_nanoseconds(NULL) / 1000000;
	lck_mtx_lock(&mdm_radio_status_lock);
	bool connected = mdm_wds_sample(&mdm_radio_status, token, bytes, response, sample_ms);
	struct sockaddr_qrtr peer = mdm_wds_peer;
	lck_mtx_unlock(&mdm_radio_status_lock);
	if (!connected)
		return;
	uint32_t detail;
	if (mdm_wds_read_current(&slot->handle, &peer, &detail, 1000) <= 0)
		return;
	lck_mtx_lock(&mdm_radio_status_lock);
	mdm_wds_publish(&mdm_radio_status, token, sample_ms, detail);
	lck_mtx_unlock(&mdm_radio_status_lock);
}

static void
mdm_radio_release(void)
{
	for (unsigned i = 0; i < MDM_RADIO_SLOTS; i++) {
		struct mdm_radio_slot *slot = &mdm_radio_slots[i];

		if (slot->open) {
			qmi_handle_release(&slot->handle);
		}
		slot->open = slot->connected = slot->probed = false;
		slot->node = slot->port = 0;
	}
}

/* A TS response acknowledges the notification request. The temperature comes
 * separately on this handle, sometimes before the ACK. Both must match. */
static struct {
	uint32_t advertised, generation, seq, node, port;
	bool accepted, indicated, emitted;
	int millicelsius;
} mdm_ts;
static uint32_t mdm_ts_next_seq;

static void
mdm_ts_record(uint32_t seq, uint32_t node, uint32_t port, bool ack, int millic)
{
	bool emit = false;
	int measured = 0;
	const unsigned index = MDM_RADIO_TS - MDM_RADIO_UIM;
	lck_mtx_lock(&mdm_radio_status_lock);
	if (mdm_aux[index].present && mdm_aux[index].generation == mdm_ts.generation &&
	    seq && seq == mdm_ts.seq && node == mdm_ts.node && port == mdm_ts.port && !mdm_ts.emitted) {
		if (ack) {
			mdm_ts.accepted = true;
		} else if (!mdm_ts.indicated) {
			mdm_ts.indicated = true;
			mdm_ts.millicelsius = millic;
		}
		if (mdm_ts.accepted && mdm_ts.indicated) {
			mdm_ts.emitted = emit = true;
			measured = mdm_ts.millicelsius;
		}
	}
	lck_mtx_unlock(&mdm_radio_status_lock);
	if (emit)
		say("modem: TS current: sensor=modem_tsens seq=%u millicelsius=%d\n", seq, measured);
}

static void
mdm_ts_indication(struct qmi_handle *handle, struct sockaddr_qrtr *peer,
    __unused struct qmi_txn *txn, const void *decoded)
{
	const struct hk_qmi_ts_report *r = decoded;
	int millic;
	if (handle == &mdm_radio_slots[MDM_RADIO_TS].handle && peer && r &&
	    r->name_valid && !strcmp(r->name, "modem_tsens") && r->kind_valid && !r->kind &&
	    r->temperature_valid && r->sequence_valid && !hk_qmi_ts_millicelsius(r->temperature_bits, &millic))
		mdm_ts_record(r->sequence, peer->sq_node, peer->sq_port, false, millic);
}

static const struct qmi_msg_handler mdm_ts_handlers[] = {
	{ QMI_INDICATION, HK_QMI_TS_TEMP_REPORT, hk_qmi_ts_report_ei,
	    sizeof(struct hk_qmi_ts_report), mdm_ts_indication },
	{ }
};

static int
mdm_radio_init(void)
{
	int error;

	mdm_radio_release();
	for (unsigned i = 0; i < MDM_RADIO_SLOTS; i++) {
		struct mdm_radio_slot *slot = &mdm_radio_slots[i];

		if ((slot->service == HK_QMI_UIM_SERVICE && mdm_radio_enabled < 3) ||
		    (slot->service == HK_QMI_WDA_SERVICE && mdm_radio_enabled != 4) ||
		    (slot->service == HK_QMI_TS_SERVICE && mdm_radio_enabled != 6)) {
			continue;
		}
		error = qmi_handle_init_transport(&slot->handle, 8192, &mdm_radio_ops,
		    slot->service == HK_QMI_TS_SERVICE ? mdm_ts_handlers : NULL, hk_qrtr_mhi_ops());
		if (error != 0) {
			say("modem: qmi radio: %s handle failed: %d\n", slot->name, error);
			mdm_radio_release();
			return error;
		}
		slot->open = true;
		error = qmi_add_lookup(&slot->handle, slot->service, 1, slot->service == HK_QMI_TS_SERVICE ? 100 : 0);
		if (error < 0) {
			say("modem: qmi radio: %s lookup failed: %d\n", slot->name, error);
			mdm_radio_release();
			return error;
		}
	}
	/* Announcements can have arrived during mission transition. */
	hk_qrtr_mhi_fresh(&mdm_qrtr_mhi);
	say("modem: qmi radio: read-only lookups armed\n");
	return 0;
}

/* Diagnostic level 2 only. The snapshot ABI continues to hold the original NAS
 * signal query. Discard a report if the endpoint turned over during either call. */
static bool
mdm_serving_current(uint32_t token)
{
    lck_mtx_lock(&mdm_radio_status_lock);
    bool current = mdm_radio_status.phase == HK_RADIO_ACTIVE &&
        mdm_radio_status.fields[HK_RADIO_SIGNAL].generation == token;
    lck_mtx_unlock(&mdm_radio_status_lock);
    return current;
}
static void
mdm_nas_serving_once(struct mdm_radio_slot *slot, uint32_t token)
{
    struct hk_qmi_nas_serving_status s;
    if (mdm_radio_enabled < 2 || !mdm_serving_current(token)) return;
    int rc = hk_qmi_nas_get_serving_system(&slot->handle, &s, 1000);
    if (!mdm_serving_current(token)) {
        say("modem: NAS serving: endpoint changed\n");
    } else if (rc == -ETIMEDOUT) {
        say("modem: NAS serving: no response\n");
    } else if (rc <= 0) {
        say("modem: NAS serving: unavailable errno=%d\n", rc);
    } else if (s.response.result != QMI_RESULT_SUCCESS_V01) {
        say("modem: NAS serving: modem error=%u\n", s.response.error);
    } else if (s.valid) {
        say("modem: NAS serving: registration=%u cs=%u ps=%u interfaces=%u mask=%#x unknown=%u unrecognized=%u\n",
            s.registration, s.cs_attach, s.ps_attach, s.interface_count, s.known_interfaces,
            s.unknown_interfaces, s.unrecognized_interfaces);
    }
}

static uint32_t
mdm_aux_token(struct mdm_radio_slot *slot, bool begin)
{
	unsigned index = (unsigned)(slot - mdm_radio_slots) - MDM_RADIO_UIM;
	uint32_t token = 0;

	lck_mtx_lock(&mdm_radio_status_lock);
	if (index < sizeof(mdm_aux) / sizeof(mdm_aux[0]) && mdm_aux[index].present && (!begin || !slot->probed)) {
		token = mdm_aux[index].generation;
		if (begin)
			slot->probed = true;
	}
	lck_mtx_unlock(&mdm_radio_status_lock);
	return token;
}

static void
mdm_uim_query(struct mdm_radio_slot *slot, uint32_t token, unsigned sample)
{
	struct hk_qmi_uim_status card;
	int rc = hk_qmi_uim_get_card_status(&slot->handle, &card, 1000);

	if (mdm_aux_token(slot, false) != token) {
		say("modem: UIM status: endpoint changed\n");
	} else if (rc <= 0) {
		say("modem: UIM status: transport result=%d\n", rc);
	} else if (card.response.result != QMI_RESULT_SUCCESS_V01 || !card.valid) {
		say("modem: UIM status: unavailable result=%u error=%u\n", card.response.result, card.response.error);
	} else {
		say("modem: UIM status: slots=%u absent=%u present=%u card_error=%u other=%u ready_apps=%u\n",
		    card.slots, card.card_state[0], card.card_state[1], card.card_state[2],
		    card.other_cards, card.present_ready_apps);
		say("modem: UIM readiness: sample=%u gen=%u validity_tlv=%u flags=%u raw_ready=%u present_ready=%u confirmed_ready=%u\n",
		    sample, token, card.validity_present, card.validity_count, card.application_state[7],
		    card.present_ready_apps, card.confirmed_ready_apps);
		if (mdm_radio_enabled == 5) {
			say("modem: UIM check: sample=%u generation=%u time_ms=%llu slots=%u error_slots=%u reason_mask=%x unknown_errors=%u ready_apps=%u\n",
			    sample, token, op_nanoseconds(NULL) / 1000000ull, card.slots, card.card_state[2],
			    card.error_mask, card.unrecognized_errors, card.present_ready_apps);
		}
	}
}

static void
mdm_uim_physical(struct mdm_radio_slot *slot, uint32_t token)
{
	struct hk_uim_physical_status s;
	int rc = hk_qmi_uim_get_physical_slots(&slot->handle, &s, 1000);
	if (mdm_aux_token(slot, false) != token) return;
	if (rc <= 0 || s.response.result || !s.valid) {
		say("modem: physical UIM: unavailable rc=%d result=%u error=%u\n", rc,s.response.result,s.response.error);
		return;
	}
	say("modem: physical UIM: slots=%u info_present=%u\n", s.count,s.info_valid);
	for (unsigned i=0; i<s.count && i<sizeof(s.slots)/sizeof(s.slots[0]); i++) {
		const struct hk_uim_physical_slot *v = &s.slots[i];
		say("modem: physical UIM: slot=%u card=%u activity=%u logical=%u protocol=%u atr_present=%u info_valid=%u\n",
		    i+1, v->card, v->activity, v->logical_slot, v->protocol, v->atr_present, s.info_valid);
	}
}

static void
mdm_wda_query(struct mdm_radio_slot *slot, uint32_t token)
{
	struct hk_qmi_wda_format format;
	int rc = hk_qmi_wda_get_pcie_format(&slot->handle, &format, 1000);

	if (mdm_aux_token(slot, false) != token) {
		say("modem: WDA format: endpoint changed\n");
	} else if (rc <= 0) {
		say("modem: WDA format: transport result=%d\n", rc);
	} else if (format.response.result != QMI_RESULT_SUCCESS_V01) {
		say("modem: WDA format: rejected error=%u\n", format.response.error);
	} else {
		say("modem: WDA format: endpoint=pcie/4 present=%x qos=%u link=%u uplink=%u downlink=%u\n",
		    format.present, format.value[HK_WDA_QOS], format.value[HK_WDA_LINK],
		    format.value[HK_WDA_UPLINK], format.value[HK_WDA_DOWNLINK]);
	}
}

static void
mdm_ts_read(struct mdm_radio_slot *slot, uint32_t token, bool discover)
{
	struct hk_qmi_ts_list list;
	struct qmi_response_type_v01 ack;
	const unsigned index = MDM_RADIO_TS - MDM_RADIO_UIM;
	uint32_t seq, node, port;
	if (discover) {
		int rc = hk_qmi_ts_list(&slot->handle, &list, 1000);
		if (rc <= 0 || list.response.result || !list.valid || !list.has_modem_tsens) {
			say("modem: TS list: modem_tsens unavailable rc=%d result=%u count=%u\n", rc,list.response.result,list.count);
			return;
		}
	}
	lck_mtx_lock(&mdm_radio_status_lock);
	if (!mdm_aux[index].present || mdm_aux[index].generation != token ||
	    (!discover && mdm_ts.advertised != token)) {
		lck_mtx_unlock(&mdm_radio_status_lock);
		return;
	}
	mdm_ts.advertised = mdm_ts.generation = token;
	if (!++mdm_ts_next_seq) ++mdm_ts_next_seq;
	seq = mdm_ts.seq = mdm_ts_next_seq;
	node = mdm_ts.node = slot->node;
	port = mdm_ts.port = slot->port;
	mdm_ts.accepted = mdm_ts.indicated = mdm_ts.emitted = false;
	lck_mtx_unlock(&mdm_radio_status_lock);
	if (discover) say("modem: TS list: modem_tsens advertised count=%u\n", list.count);
	int rc = hk_qmi_ts_current(&slot->handle, seq, &ack, 1000);
	if (rc > 0 && ack.result == QMI_RESULT_SUCCESS_V01)
		mdm_ts_record(seq, node, port, true, 0);
	else
		say("modem: TS current: request unavailable seq=%u rc=%d error=%u\n", seq,rc,ack.error);
}

static void
mdm_radio_query_one(struct mdm_radio_slot *slot)
{
	int result;

	if (slot == NULL || !slot->open || !slot->connected || slot->probed) {
		return;
	}
	if (slot->service == HK_QMI_UIM_SERVICE || slot->service == HK_QMI_WDA_SERVICE || slot->service == HK_QMI_TS_SERVICE) {
		uint32_t generation = mdm_aux_token(slot, true);
		if (generation != 0) {
			if (slot->service == HK_QMI_UIM_SERVICE) {
				mdm_uim_query(slot, generation, 0);
				if (mdm_radio_enabled == 6 && mdm_aux_token(slot, false) == generation)
					mdm_uim_physical(slot, generation);
			} else if (slot->service == HK_QMI_WDA_SERVICE)
				mdm_wda_query(slot, generation);
			else
				mdm_ts_read(slot, generation, true);
		}
		return;
	}
	unsigned kind = (unsigned)(slot-mdm_radio_slots);
	uint32_t token = mdm_radio_query_begin(kind);
	if (!token) return;
	slot->probed = true;
	if (slot->service == HK_QMI_DMS_SERVICE) {
		struct hk_qmi_dms_model_resp model = { 0 };
		struct hk_qmi_dms_operating_mode_resp mode = { 0 };
		uint32_t model_bytes = 0;

		result = hk_qmi_dms_get_model(&slot->handle, &model, 1000);
		if (result > 0 && model.model_valid) {
			while (model_bytes < HK_QMI_DMS_MODEL_MAX && model.model[model_bytes] != 0) {
				model_bytes++;
			}
		}
		say("modem: qmi radio: DMS model query %s (%u bytes)\n", result > 0 && model.response.result == QMI_RESULT_SUCCESS_V01 ? "ok" : "failed", model_bytes);
		result = hk_qmi_dms_get_operating_mode(&slot->handle, &mode, 1000);
		mdm_radio_query_finish(kind, token, result, mode.response.result, mode.response.error,
		    mode.mode_valid, mode.mode, hk_radio_mode_detail(mode.offline_reason_valid,
		    mode.offline_reason, mode.hardware_restricted_valid, mode.hardware_restricted));
		say("modem: qmi radio: DMS operating mode query %s%s\n", result > 0 && mode.response.result == QMI_RESULT_SUCCESS_V01 ? "ok" : "failed",
		    result > 0 && mode.mode_valid ? " (mode present)" : "");
	} else if (slot->service == HK_QMI_NAS_SERVICE) {
		struct hk_qmi_nas_signal_strength_req request = { .request_mask = 0 };
		struct hk_qmi_nas_signal_strength_resp response = { 0 };

		result = hk_qmi_nas_get_signal_strength(&slot->handle, &request, &response, 1000);
		mdm_radio_query_finish(kind, token, result, response.response.result, response.response.error, response.signal_strength_valid, response.signal_strength, response.radio_interface);
		say("modem: qmi radio: NAS signal query %s%s\n", result > 0 && response.response.result == QMI_RESULT_SUCCESS_V01 ? "ok" : "failed",
		    result > 0 && response.signal_strength_valid ? " (value present)" : "");
		mdm_nas_serving_once(slot, token);
	} else if (slot->service == HK_QMI_WDS_SERVICE) {
		struct hk_qmi_wds_packet_service_status_resp response = { 0 };

		result = hk_qmi_wds_get_packet_service_status(&slot->handle, &response, 1000);
		mdm_wds_observe(slot, token, result, &response);
		say("modem: qmi radio: WDS packet status query %s%s\n", result > 0 && response.response.result == QMI_RESULT_SUCCESS_V01 ? "ok" : "failed",
		    result > 0 && response.connection_status_valid ? " (status present)" : "");
	}
}

static void
mdm_radio_query(unsigned mission_tenths)
{
	if (!mdm_radio_enabled) {
		return;
	}
	for (unsigned i = 0; i < MDM_RADIO_SLOTS; i++) {
		mdm_radio_query_one(&mdm_radio_slots[i]);
	}
	if (mdm_radio_enabled == 6 && (mission_tenths == 300 || mission_tenths == 550)) {
		struct mdm_radio_slot *slot = &mdm_radio_slots[MDM_RADIO_TS];
		uint32_t token = mdm_aux_token(slot, false);
		if (token && slot->open && slot->connected && slot->probed) mdm_ts_read(slot, token, false);
	}
	if (mdm_radio_enabled == 5 && (mission_tenths == 300 || mission_tenths == 550)) {
		struct mdm_radio_slot *slot = &mdm_radio_slots[MDM_RADIO_UIM];
		uint32_t token = mdm_aux_token(slot, false);
		if (token && slot->open && slot->connected && slot->probed) {
			mdm_uim_query(slot, token, mission_tenths == 300 ? 1 : 2);
		}
	}
}

/* Keep the legacy LOC encoder as a fail-safe fallback.  Once the generic
 * client completes its three-message start sequence, its transaction and
 * indication handlers own the GNSS data path. */
static int
mdm_qmi_new_server(struct qmi_handle *qmi, struct qmi_service *service)
{
	int error;

	if (service->service != MDM_LOC_SERVICE) {
		return ENXIO;
	}
	error = hk_qmi_connect(qmi->sock, service->node, service->port);
	if (error == 0) {
		mdm_qmi_server = true;
		mdm_loc_node = service->node;
		mdm_loc_port = service->port;
	}
	say("modem: qmi loc: generic service at node %u port %u (version %u, instance %u)%s\n", service->node,
	    service->port, service->version, service->instance, error == 0 ? "" : ": cannot connect");
	return error;
}

static void
mdm_qmi_del_server(__unused struct qmi_handle *qmi, struct qmi_service *service)
{
	/* qmi_server() removes the service before this callback.  Clear the
	 * selected endpoint here as well as in the legacy QRTR DEL_SERVER path:
	 * either callback may observe the teardown first, and leaving the old
	 * node/port would make a later legacy fallback send to a dead service. */
	if (service != NULL && mdm_loc_node == service->node && mdm_loc_port == service->port) {
		mdm_fix_close();
		mdm_loc_node = mdm_loc_port = 0;
		mdm_qmi_server = false;
		mdm_qmi_generic = false;
	}
	say("modem: qmi loc: generic service at node %u port %u has gone\n", service->node, service->port);
}

static const struct qmi_ops mdm_qmi_ops = {
	.new_server = mdm_qmi_new_server,
	.del_server = mdm_qmi_del_server,
};

static bool mdm_abp_receive(void *,const void *,size_t);
static void
op_receive(void *c, unsigned channel, const void *data, size_t length)
{
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
		if (mdm_abp_receive(c,data,length)) return;
		int adapter_error = hk_qrtr_mhi_input(&mdm_qrtr_mhi, data, length);
		if (adapter_error == 0 && !mdm_qrtr_adapter_said) {
			mdm_qrtr_adapter_said = true;
			say("modem: qrtr adapter: first packet accepted (in %u, out %u, services %u)\n",
			    mdm_qrtr_mhi.core.packets_in, mdm_qrtr_mhi.core.packets_out,
			    hk_qrtr_core_live_services(&mdm_qrtr_mhi.core));
		}
		mdm_qrtr_input(data, length, adapter_error == 0);
	} else if (channel == MDM_EFS_IN) {
		if (mdm_efs_observe_only) mdm_efs_header(data,length);
		else mdm_efs_input(data, length);
	} else {
		say("modem: channel %u: %zu bytes (nothing takes them yet)\n", channel, length);
	}
}

static const struct mhi_host_ops mdm_ops = {
	.read = op_read, .write = op_write, .dma_alloc = op_dma_alloc, .dma_free = op_dma_free, .nanoseconds = op_nanoseconds, .lock = op_lock, .unlock = op_unlock,
	.pause = op_pause, .random = op_random, .say = op_say, .status = op_status, .receive = op_receive,
	/* The pool is cleaned once and used only through its VM_WIMG_WCOMB
	 * (ARM VM_MEM_NOT_CACHEABLE) mapping, including the client-owned ER8. */
	.dma_alloc_uncached = op_dma_alloc,
};


/* The generic QRTR adapter shares the modem's MHI sender.  It is deliberately
 * kept alongside the hand-written LOC parser until a QMI client is attached. */
static int
mdm_qrtr_mhi_send(void *context, const void *packet, size_t length)
{
	(void)context;
	int rc=mdm_mhi_send(&mdm_mhi, IPCR_OUT, packet, length, false);
	if (mdm_abp_diagnostic) mdm_events_tx(packet,length,rc);
	return rc;
}

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

/* From the thread that watches: all EFS writes are serialized here, away from the MHI receive path. */
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
	} else if (due == MDM_EFS_READ_DUE) {
		uint64_t length = mdm_efs_remaining < MDM_EFS_READ_CHUNK ? mdm_efs_remaining : MDM_EFS_READ_CHUNK;

		mdm_efs_expected = length;
		mdm_efs_received = 0;
		if (mdm_efs_word_size == 8) {
			/* The 64-bit command has address and length as its two payload words. */
			uint8_t packet[24] = { 0 };
			uint32_t command = MDM_EFS_READ64_CMD, declared = 24;
			memcpy(packet, &command, sizeof(command));
			memcpy(packet + 4, &declared, sizeof(declared));
			memcpy(packet + 8, &mdm_efs_address, sizeof(mdm_efs_address));
			memcpy(packet + 16, &length, sizeof(length));
			error = mdm_mhi_send(&mdm_mhi, MDM_EFS_OUT, packet, sizeof(packet), false);
		} else {
			uint32_t read[4] = { MDM_EFS_READ_CMD, 16, (uint32_t)mdm_efs_address, (uint32_t)length };
			error = mdm_mhi_send(&mdm_mhi, MDM_EFS_OUT, read, sizeof(read), false);
		}
		say("modem: efs: memory read %llu bytes: %d\n", (unsigned long long)length, error);
	} else if (due == MDM_EFS_RESET_DUE) {
		uint32_t reset[2] = { 0x7, 8 };

		error = mdm_mhi_send(&mdm_mhi, MDM_EFS_OUT, reset, sizeof(reset), false);
		say("modem: efs: RESET sent after read-only sync: %d\n", error);
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
static bool
mdm_qmi_common_ok(const char *name, int result, const struct hk_qmi_loc_common_resp *response)
{
	if (result < 0) {
		say("modem: qmi loc: %s failed: %d\n", name, result);
		return false;
	}
	if (response->response.result != QMI_RESULT_SUCCESS_V01 || response->response.error != QMI_ERR_NONE_V01) {
		say("modem: qmi loc: %s modem error: result %u error %u\n", name,
		    response->response.result, response->response.error);
		return false;
	}
	return true;
}

static bool
mdm_gnss_start(void)
{
	uint8_t fields[40], session = 1;
	uint64_t mask = 1u << 0 | 1u << 1 | 1u << 7;    /* position report, satellites, engine state */
	uint32_t uncertainty_ms = 5000, recurrence = 1, intermediate = 1, interval_ms = 1000, client_type = 1;
	clock_sec_t seconds;
	clock_usec_t microseconds;
	uint64_t utc_ms;
	uint16_t length;

	mdm_gnss.started_ms = mdm_gnss.last_said_ms = mdm_gnss_ms();
	mdm_gnss_stage = MDM_GNSS_STARTING;
	if (mdm_qmi_server) {
		struct hk_qmi_loc_inject_utc_req utc = { 0 };
		struct hk_qmi_loc_register_events_req events = { 0 };
		struct hk_qmi_loc_start_req start = { 1, 1, 1, 1000 };
		struct hk_qmi_loc_start_resp started = { 0 };
		struct hk_qmi_loc_common_resp response = { 0 };

		utc.utc_ms = 0; /* filled below from the same calendar source as the fallback */
		clock_get_calendar_microtime(&seconds, &microseconds);
		utc.utc_ms = (uint64_t)seconds * 1000 + microseconds / 1000;
		utc.uncertainty_ms = uncertainty_ms;
		events.event_mask = mask;
		events.client_type_valid = 1;
		events.client_type = client_type;
		if (mdm_qmi_common_ok("inject UTC", hk_qmi_loc_inject_utc(&mdm_qmi, &utc, &response, 1000), &response) &&
		    mdm_qmi_common_ok("register events", hk_qmi_loc_register_events(&mdm_qmi, &events, &response, 1000), &response) &&
		    hk_qmi_loc_start(&mdm_qmi, &start, &started, 1000) > 0 &&
		    started.response.result == QMI_RESULT_SUCCESS_V01 && started.response.error == QMI_ERR_NONE_V01) {
			mdm_qmi_generic = true;
			mdm_gnss_stage = MDM_GNSS_ACTIVE;
			mdm_fix_open(started.session_id_valid ? started.session_id : start.session_id);
			say("modem: qmi loc: generic GNSS start accepted (session %u)\n",
			    started.session_id_valid ? started.session_id : start.session_id);
			return true;
		}
		say("modem: qmi loc: generic GNSS start unavailable; using legacy LOC path\n");
	}

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
	mdm_gnss_stage = MDM_GNSS_LEGACY_PENDING;
	return false;
}

static void
mdm_gnss_stop(void)
{
	mdm_fix_close();
	uint8_t fields[8], session = 1;

	if (mdm_qmi_generic) {
		struct hk_qmi_loc_stop_req stop = { 1 };
		struct hk_qmi_loc_common_resp response = { 0 };
		int result = hk_qmi_loc_stop(&mdm_qmi, &stop, &response, 1000);

		if (mdm_qmi_common_ok("stop", result, &response)) {
			say("modem: qmi loc: generic GNSS stop accepted\n");
		} else {
			say("modem: qmi loc: generic GNSS stop failed\n");
		}
		mdm_qmi_generic = false;
		return;
	}

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

#include "mdm_abp_board.inc"
static volatile bool mdm_polling;
static bool mdm_ip_hw0_poller_active;

/* No MSI yet: the host looks at the device's rings a thousand times a second. */
static void
poll_thread(__unused void *parameter, __unused wait_result_t result)
{
	if (parameter==&mdm_abp_binding) {
		mdm_abp_run();
		thread_terminate(current_thread());
		return;
	}
	while (mdm_polling) {
		modem_mhi_host_interrupt(&mdm_mhi, -1);
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	if (mdm_ip_hw0_diagnostic)
		__atomic_store_n(&mdm_ip_hw0_poller_active, false, __ATOMIC_RELEASE);
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

static unsigned
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
		return 0;
	}
	mdm_msi_page = op_dma_alloc(NULL, 4096, 4096, &address);
	if (mdm_msi_page == NULL) {
		return 0;
	}
	wr_sync(pcie.conf + cap + 4, (uint32_t)address);
	if (control & 1u << 23) {
		wr_sync(pcie.conf + cap + 8, (uint32_t)(address >> 32));
		wr_sync(pcie.conf + cap + 0xc, MDM_MSI_DATA);
	} else {
		wr_sync(pcie.conf + cap + 8, MDM_MSI_DATA);
	}
	wr_sync(pcie.conf + cap, (control & ~(7u << 20)) | (control >> 17 & 7) << 20 | 1u << 16);
	uint32_t actual = rd(pcie.conf + cap);
	say("modem: MSI on: capability at %#x, control %#x, now %#x; its writes go to the device's %#llx\n", cap, control >> 16, actual >> 16, address);
	return actual != UINT32_MAX && (actual & (1u << 16)) ? 1u << ((actual >> 20) & 7u) : 0;
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

static bool
mdm_sfr_word(const char *s, size_t size, const char *word)
{
    size_t n=strlen(word);
    for(size_t i=0;i+n<=size;i++){
        size_t j=0;
        while(j<n){
            char c=s[i+j];
            if(c>='A' && c<='Z')c+=('a'-'A');
            if(c!=word[j])break;
            j++;
        }
        if(j==n)return true;
    }
    return false;
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
	if (mdm_rfs_requested) {
        size_t reason_length=0;
        while(reason_length<MDM_SFR_LENGTH && raw[reason_length])reason_length++;
        static const char first[]="sahara_status";
        static const char second[]="end_of_img_transfer";
        bool ordered=false;
        for(size_t i=0;i+sizeof(first)-1<=reason_length;i++){
            if(mdm_sfr_word(raw+i,sizeof(first)-1,first)){
                size_t after=i+sizeof(first)-1;
                ordered=mdm_sfr_word(raw+after,reason_length-after,second);
                if(ordered)break;
            }
        }
        say("modem: rfs sfr_public remotefs_sahara_file=%u ordered_status_end=%u\n",
            (unsigned)mdm_sfr_word(raw,reason_length,"remotefs_sahara.c"),(unsigned)ordered);
        say("modem: rfs sfr reason_present=%u watchdog=%u assert=%u timeout=%u rfs=%u efs=%u mhi=%u\n",
            (unsigned)(length>0),(unsigned)mdm_sfr_word(clean,length,"watchdog"),
            (unsigned)mdm_sfr_word(clean,length,"assert"),(unsigned)mdm_sfr_word(clean,length,"timeout"),
            (unsigned)mdm_sfr_word(clean,length,"rfs"),(unsigned)mdm_sfr_word(clean,length,"efs"),
            (unsigned)mdm_sfr_word(clean,length,"mhi"));
        bzero(clean,sizeof(clean));
    } else if (length == 0) {
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
	static const struct mhi_host_config baseline = {
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
	struct mhi_host_config config = baseline;
	uint8_t *sbl = NULL;
	size_t sbl_length = 0;
	thread_t thread;
	uint64_t until, ns;
	bool done = false, loc_asked = false;
	uint32_t ip_hw0 = 0, ip_rx = 0;
	int error;

	say("modem: modem=3: its firmware\n");
	mdm_radio_enabled = 0;
	mdm_wds_settings_enabled = 0;
	(void)PE_parse_boot_argn("modem_wds_settings", &mdm_wds_settings_enabled, sizeof(mdm_wds_settings_enabled));
	(void)PE_parse_boot_argn("modem_mission", &mdm_mission, sizeof(mdm_mission));
	(void)PE_parse_boot_argn("modem_loc", &mdm_loc_enabled, sizeof(mdm_loc_enabled));
	(void)PE_parse_boot_argn("modem_radio", &mdm_radio_enabled, sizeof(mdm_radio_enabled));
	(void)PE_parse_boot_argn("modem_gnss", &mdm_gnss_enabled, sizeof(mdm_gnss_enabled));
	(void)PE_parse_boot_argn("modem_iphw0", &ip_hw0, sizeof(ip_hw0));
	(void)PE_parse_boot_argn("modem_iphw0_rx", &ip_rx, sizeof(ip_rx));
	bool ip_trial = ip_hw0 != 0 || ip_rx != 0;
	uint32_t frontend=0,frontend_read=0,frontend_events=0;
	(void)PE_parse_boot_argn("ui_qevents",&frontend_events,sizeof(frontend_events));
	(void)PE_parse_boot_argn("ui_qread",&frontend_read,sizeof(frontend_read));
	(void)PE_parse_boot_argn("ui_qmux",&frontend,sizeof(frontend));
	uint32_t family_only=0;
	(void)PE_parse_boot_argn("ui_wds_family",&family_only,sizeof(family_only));
	uint32_t rfs = 0;
	(void)PE_parse_boot_argn("modem_rfs", &rfs, sizeof(rfs));
	if (rfs && (rfs != 1 || frontend != 3 || family_only || frontend_read || frontend_events)) {
		say("modem: rfs requires the exclusive ABP poller\n");
		return;
	}
	mdm_rfs_requested = rfs == 1;
    uint32_t combined=0;
    (void)PE_parse_boot_argn("pro_stack",&combined,sizeof(combined));
    if(combined && (combined!=1 || rfs!=1 || frontend!=3 || family_only || frontend_read || frontend_events || ip_trial)) {
        say("modem: donor incompatible profile\n");return;
    }
    pro_active=combined==1;
    bool data_profile=family_only || pro_active;
    if(pro_active)say("modem: donor profile one_poller=1 raw_transport=1 data_tx=0 rx_credits=0 no_bearer_start=1\n");
	bool abp=frontend==3 || family_only==1;
	if (family_only && (family_only!=1 || frontend || frontend_read || frontend_events)) {
		say("modem: family incompatible profile\n");return;
	}
	mdm_family.enabled=data_profile;
	if (frontend || frontend_read || frontend_events || family_only) {
		uint32_t wifi=1;
		if (!abp || frontend_events>1 || (frontend_events && frontend_read) || frontend_read>1 || ip_trial || mdm_mission!=1 || mdm_loc_enabled || mdm_gnss_enabled ||
		    mdm_radio_enabled || mdm_wds_settings_enabled || mdm_mhi.allocated ||
		    hk_qo_phase(&mdm_abp_worker.gate)!=HK_QO_OFF ||
		    !PE_parse_boot_argn("wifi",&wifi,sizeof(wifi)) || wifi) {
			say("modem: abp-board: incompatible profile\n");
			return;
		}
	}
	mdm_abp_diagnostic=abp;
	mdm_abp_read.enabled=abp && frontend_read==1;
	mdm_events.enabled=abp && frontend_events==1;
	if (mdm_mhi.allocated && mdm_mhi.config.hardware_event_count != 0) {
		say("modem: earlier IP_HW0 owner retains its DMA arena until kernel return\n");
		return;
	}
	if (ip_trial) {
		uint32_t wifi;
		if (!((ip_hw0 == 1 && !ip_rx) || (ip_rx == 1 && !ip_hw0)) || mdm_mission != 1 || mdm_loc_enabled || mdm_gnss_enabled ||
		    mdm_radio_enabled || mdm_wds_settings_enabled || mdm_mhi.allocated ||
		    !PE_parse_boot_argn("wifi", &wifi, sizeof(wifi)) || wifi != 0) {
			say("modem: IP_HW0 command trial refused before PCIe setup\n");
			return;
		}
	}
	mdm_ip_hw0_diagnostic = ip_trial;
	if (mdm_radio_enabled > 6 || mdm_mission != 1) mdm_radio_enabled = 0;
	mdm_radio_phase(mdm_radio_enabled ? HK_RADIO_STARTING : HK_RADIO_OFF);
	mdm_fix_close();
    uint32_t fix_wifi = ~0u;
    bool explicit_wifi_off = PE_parse_boot_argn("wifi", &fix_wifi, sizeof(fix_wifi)) && fix_wifi == 0;
    mdm_fix_enabled = mdm_gnss_enabled == 2 && explicit_wifi_off && mdm_loc_enabled == 1 && mdm_mission == 1;
    mdm_gnss_enabled = (mdm_gnss_enabled == 1 || mdm_fix_enabled) && mdm_loc_enabled == 1 && mdm_mission == 1;
	mdm_gnss_stage = MDM_GNSS_BOOTING;
	mdm_gnss_failure = MDM_GNSS_ERROR_BOOT;
	bzero(mdm_qrtr_server_table, sizeof(mdm_qrtr_server_table));
	mdm_qrtr_peer = mdm_qrtr_in = mdm_qrtr_servers = mdm_qrtr_lookups = 0;
	mdm_qrtr_adapter_said = false;
	mdm_loc_node = mdm_loc_port = 0;
	mdm_qrtr_hello_due = false;
	config.channel_count = mdm_gnss_enabled ? 6 : 4;
	mdm_efs_observe_only = pro_active;
	if (data_profile && mdm_mhi_tx_config(&config)) {
		say("modem: family TX profile unavailable\n");return;
	}
	if (mdm_efs_observe_only) {
		if (mdm_mhi_efs_observer_config(&config)) { say("modem: EFS mission profile unavailable\n"); return; }
		say("modem: EFS mission profile out=10 in=11 er=1 elements=64 rx_limit=48 tx_capacity=0 gnss=0\n");
	}
	if (ip_trial && (ip_rx ? mdm_mhi_rx_config(&config) : mdm_mhi_ip_hw0_config(&config)) != 0) {
		say("modem: IP_HW0 command profile unavailable\n");
		return;
	}
	if (!mdm_load_sbl(&sbl, &sbl_length) || !modem_reset() || !pcie_on()) {
		pins_say("no link");
		goto end;
	}
	if (ip_trial || data_profile)
		config.register_window_size = MIN(pcie.bar_size, 0x1000ull);
	mhi_look();
	sid_table();
	if (!mdm_pool() || !mdm_smmu_on()) {
		goto end;
	}
	config.image = sbl;
	config.image_length = config.sbl_length = sbl_length;
	if ((error = modem_mhi_host_init(&mdm_mhi, &mdm_ops, abp ? &mdm_abp_binding : NULL, &config)) != 0) {
		say("modem: mhi: %d\n", error);
		goto end;
	}
	if (ip_trial) {
		struct mhi_host_channel *ul = &mdm_mhi.channels[4], *dl = &mdm_mhi.channels[5];
		say("modem: IP_HW0 command profile UL=%u/%u DL=%u/%u TRE=%u/%u ER=%u/%u pool_used=%llu pool_limit=%u payload_buffers=%u\n",
		    ul->config->number, ul->config->event_ring, dl->config->number, dl->config->event_ring,
		    ul->ring.elements, dl->ring.elements, mdm_mhi.events[MDM_IP_HW0_UL_ER].elements,
		    mdm_mhi.events[MDM_IP_HW0_DL_ER].elements, (unsigned long long)mdm.pool_next, MDM_POOL_SIZE,
		    (unsigned)(ul->buffers != NULL) + (unsigned)(dl->buffers != NULL));
	}
	/* Install the transport seam before MHI starts any mission channels.  It
	 * remains down until mdm_mhi_to_mission() has completed. */
	hk_qrtr_mhi_init(&mdm_qrtr_mhi, MDM_QRTR_US, mdm_qrtr_mhi_send, NULL);
	hk_qrtr_mhi_set_active(&mdm_qrtr_mhi);
	mdm_check_image(sbl, sbl_length);
	set_bits(pcie.dbi + 0x04, 0, 0x4);      /* the bridge: bus master */
	set_bits(pcie.conf + 0x04, 0, 0x4);     /* the device: the SMMU has it now */
	unsigned msi_vectors = mdm_msi_on();
	if (data_profile && msi_vectors<=mdm_mhi.config.events[MDM_IP_HW0_DL_ER].vector) goto end;
	if (ip_trial) {
		say("modem: IP_HW0 MSI vectors=%u pool_high_water=%llu\n", msi_vectors,
		    (unsigned long long)mdm.pool_next);
		if (msi_vectors <= mdm_mhi.config.events[MDM_IP_HW0_DL_ER].vector)
			goto end;
	}
	if (abp && mdm_abp_arm()) goto end;
	mdm_polling = true;
	if (ip_trial)
		__atomic_store_n(&mdm_ip_hw0_poller_active, true, __ATOMIC_RELEASE);
	if (kernel_thread_start(poll_thread, abp ? &mdm_abp_binding : NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else if (abp) {
		mdm_polling=false;
		if (!hk_abp_worker_never_started(&mdm_abp_worker)) mdm_abp_hold();
		__atomic_store_n(&mdm_abp_exited,1,__ATOMIC_RELEASE);
		say("modem: abp-board: poller creation failed; untouched owner retired\n");
		goto end;
	} else if (ip_trial) {
		mdm_polling = false;
		__atomic_store_n(&mdm_ip_hw0_poller_active, false, __ATOMIC_RELEASE);
		say("modem: IP_HW0 event poller unavailable\n");
		goto end;
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
	mdm_gnss_stage = MDM_GNSS_SAHARA;
	mdm_gnss_failure = MDM_GNSS_ERROR_SAHARA;
	modem_sahara_init(&sahara, &mdm_sahara_ops, NULL, chunk);
	nanoseconds_to_absolutetime(120 * NSEC_PER_SEC, &until);
	until += mach_absolute_time();
	while (!done && mach_absolute_time() < until) {
		if (instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL)) {
			mdm_gnss_failure = MDM_GNSS_ERROR_ERRFATAL;
			hist_errfatal();
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
				if (sahara.memory_debug_offered)
					mdm_gnss_failure = MDM_GNSS_ERROR_RDDM;
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
	/* Make the adapter live before the mission transition starts its AMSS
	 * channels, so the first HELLO/NEW_SERVER datagrams populate the generic
	 * core as well as the legacy monitor. */
	if (!done) {
		goto end;
	}
	mdm_gnss_failure = MDM_GNSS_ERROR_MISSION;
	if (!abp) hk_qrtr_mhi_set_up(&mdm_qrtr_mhi, true);
	if ((error = mdm_mhi_to_mission(&mdm_mhi, 30000)) != 0) {
		if (ip_trial)
			say("modem: IP_HW0 mission gate rc=%d; no IP channel command issued\n", error);
		goto end;
	}
	mdm_gnss_failure = MDM_GNSS_ERROR_NONE;
	if (abp && hk_abp_worker_ready(&mdm_abp_worker)) goto end;
	mdm_gnss_stage = MDM_GNSS_MISSION;
	if (mdm_efs_observe_only) {
		struct mhi_host_channel *out = &mdm_mhi.channels[6], *in = &mdm_mhi.channels[7];
		say("modem: EFS mission started out=%u in=%u rx_credit=%u tx_capacity=%u pool=%llu limit=%u\n",
		    (unsigned)out->running,(unsigned)in->running,in->buffer_length,
		    (unsigned)(out->buffers != NULL),(unsigned long long)mdm.pool_next,MDM_POOL_SIZE);
	}
	if (ip_rx) {
		struct mdm_rx_receipt r;
		uint64_t epoch = op_nanoseconds(NULL);
		say("modem: IP_HW0 RX begin epoch=%llu channel=101 er=8 limit=1 window_ms=500\n",
		    (unsigned long long)epoch);
		error = mdm_mhi_rx_trial(&mdm_mhi, epoch, 500, &r);
		say("modem: IP_HW0 RX window epoch=%llu posted=%u slot=%u capacity=%u expired=%u matched=%u late=%u code=%u bytes=%u pre_reset_pending=%u\n",
		    (unsigned long long)r.epoch, r.posted, r.slot, r.capacity, r.wait_expired, r.completed,
		    r.late, r.completion_code, r.length, r.pending_before_reset);
		say("modem: IP_HW0 RX retire epoch=%llu rc=%d start_ack=%u reset_ack=%u er8_drained=%u pending=%u retired=%u quarantine=%u ul_wp=%u pool_high_water=%llu\n",
		    (unsigned long long)r.epoch, error, r.start_ack, r.reset_ack, r.er8_drained, r.pending, r.retired,
		    r.quarantined, r.ul_wp, (unsigned long long)mdm.pool_next);
		say("modem: IP_HW0 RX control epoch=%llu ipcr_running=%u ipcr_ctx=%u/%u ee=%u state=%u\n",
		    (unsigned long long)r.epoch, r.ipcr_running, r.ipcr_ul_state, r.ipcr_dl_state, r.ee, r.state);
		goto end;
	}
	if (ip_hw0) {
		struct mdm_ip_hw0_receipt receipt;
		say("modem: IP_HW0 starting empty channels 100/101, then resetting 101/100\n");
		error = mdm_mhi_ip_hw0_trial(&mdm_mhi, &receipt);
		say("modem: IP_HW0 command result rc=%d ner=%u nhwer=%u start_attempt=%u start_ack=%u reset_attempt=%u reset_ack=%u er8=%u quarantine=%u\n",
		    error, receipt.ner, receipt.nhwer, receipt.start_attempted, receipt.start_ack, receipt.reset_attempted,
		    receipt.reset_ack, receipt.er8_events, receipt.quarantined);
		if (receipt.reset_ack == 3)
			say("modem: IP_HW0 post empty_mask=%u payload_mask=%u ipcr_running=%u ipcr_ctx=%u/%u ee=%u state=%u\n",
			    receipt.empty, receipt.buffers, receipt.ipcr_running, receipt.ipcr_ul_state,
			    receipt.ipcr_dl_state, receipt.ee, receipt.state);
		goto end;
	}
	if (mdm_loc_enabled) {
		error = qmi_handle_init_transport(&mdm_qmi, 8192, &mdm_qmi_ops, mdm_qmi_handlers, hk_qrtr_mhi_ops());
		if (error == 0) {
			mdm_qmi_open = true;
			error = qmi_add_lookup(&mdm_qmi, MDM_LOC_SERVICE, 2, 0);
			if (error < 0) {
				say("modem: qmi loc: generic lookup failed: %d\n", error);
			} else {
				/* LOC may have been announced during the mission transition. */
				hk_qrtr_mhi_fresh(&mdm_qrtr_mhi);
			}
		} else {
			say("modem: qmi loc: generic handle failed: %d\n", error);
		}
	}
	if (mdm_radio_enabled) {
		mdm_radio_phase(HK_RADIO_ACTIVE);
		if (mdm_radio_init() != 0) {
			mdm_radio_phase(HK_RADIO_INIT_FAILURE);
			mdm_radio_enabled = 0;
		}
	}
	if (mdm_gnss_enabled || (abp && mdm_rfs_requested)) {
		uint64_t address;
		uint8_t *buffer = op_dma_alloc(NULL, MDM_SFR_LENGTH, 4096, &address);

		if (buffer != NULL) {
			error = mdm_mhi_sfr_config(&mdm_mhi, address, MDM_SFR_LENGTH);
			say("modem: the failure reason's buffer, %u bytes, given: %d\n", MDM_SFR_LENGTH, error);
			mdm_sfr = error == 0 ? (volatile uint8_t *)buffer : NULL;
		}
	}
	/* About a minute; three with the one GNSS try. A hard limit either way: this loop's end is what stops the firmware. */
	for (unsigned tenths = 0; tenths < (mdm_gnss_enabled ? 1800u : family_only ? 900u : pro_active ? 2400u : abp ? 1700u : 600u); tenths++) {
		if (pro_active && __atomic_load_n(&cold_modem_stop,__ATOMIC_ACQUIRE)) break;
		if (tenths % 10 == 0) mdm_fix_tick();
		if (tenths % 100 == 0) {
			if (abp && mdm_rfs_requested) say("modem: wrq-health elapsed_s=%u clock_ms=%llu status=%u errfatal=%u ee=%u state=%u\n",tenths/10,(unsigned long long)(op_nanoseconds(NULL)/NSEC_PER_MSEC),instantnoodle_gpio_level(PIN_MDM2AP_STATUS),instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL),mdm_mhi_ee(&mdm_mhi),mdm_mhi_state(&mdm_mhi));
			say("modem: %u s in its mission: MDM2AP status %u, errfatal %u, EE %u, state %#x\n", tenths / 10, instantnoodle_gpio_level(PIN_MDM2AP_STATUS),
			    instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL), mdm_mhi_ee(&mdm_mhi), mdm_mhi_state(&mdm_mhi));
		}
		if (instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL)) {
			hist_errfatal();
			say("modem: MDM2AP ERRFATAL is high: the modem has crashed\n");
			delay_for_interval(2, NSEC_PER_SEC);    /* (the text may come after the pin) */
			mdm_gnss_failure = MDM_GNSS_ERROR_ERRFATAL;
			mdm_say_reason();
			break;
		}
		if (!abp && mdm_qrtr_hello_due) {
			mdm_qrtr_hello_due = false;
			mdm_qrtr_hello();
		}
		if (mdm_loc_enabled && mdm_loc_port != 0 && !loc_asked) {
			bool generic_started = false;

			loc_asked = true;
			if (mdm_gnss_enabled) {
				generic_started = mdm_gnss_start();
			}
			if (!generic_started) {
				mdm_loc_ask(MDM_LOC_GET_ENGINE_LOCK);
				mdm_loc_ask(MDM_LOC_GET_OP_MODE);
			}
		}
		mdm_radio_query(tenths);
		if (!abp && mdm_resume_due) {
			mdm_resume_due = false;
			mdm_qrtr_resume();
		}
		if (!mdm_efs_observe_only && mdm_efs_due != 0) {
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
	if (!abp) say("modem: qrtr adapter: %u packets in, %u out, %u live services, %u dropped\n", mdm_qrtr_mhi.core.packets_in,
	    mdm_qrtr_mhi.core.packets_out, hk_qrtr_core_live_services(&mdm_qrtr_mhi.core), mdm_qrtr_mhi.dropped);
	say("modem: qrtr: %u packets in, %u services offered, %u looked for\n", mdm_qrtr_in, mdm_qrtr_servers, mdm_qrtr_lookups);
	mdm_smmu_say_faults();
end:
	if (abp) mdm_abp_drain();
	if (mdm_efs_observe_only) {
		unsigned started=0, reset=0;
		int stop=mdm_mhi_efs_observer_stop(&mdm_mhi,&started,&reset);
		say("modem: EFS mission stop rc=%d started=%u reset=%u hello_image=%u hello_memory=%u hello_other=%u mem_offer=%u other=%u malformed=%u\n",
		    stop,started,reset,mdm_efs_seen[0],mdm_efs_seen[1],mdm_efs_seen[2],mdm_efs_seen[3],mdm_efs_seen[4],mdm_efs_seen[5]);
	}

	if (mdm_radio_enabled)
		mdm_radio_phase(HK_RADIO_STOPPED);
	if (mdm_gnss_enabled && mdm_gnss_failure == MDM_GNSS_ERROR_NONE && !loc_asked)
		mdm_gnss_failure = MDM_GNSS_ERROR_LOC_MISSING;
	mdm_gnss_stage = mdm_gnss_failure != MDM_GNSS_ERROR_NONE ? MDM_GNSS_FAILED : MDM_GNSS_STOPPED;
	say("modem: gnss monitor: stage %u failure %u\n", mdm_gnss_stage, mdm_gnss_failure);
	if (mdm_qmi_open) {
		qmi_handle_release(&mdm_qmi);
		mdm_qmi_open = false;
		mdm_qmi_server = false;
		mdm_qmi_generic = false;
	}
	mdm_radio_release();
	/* Stop accepting QRTR before the MHI window and its DMA mappings go away. */
	if (!abp) hk_qrtr_mhi_set_up(&mdm_qrtr_mhi, false);
	mdm_polling = false;
	if (ip_trial) {
		uint64_t deadline = op_nanoseconds(NULL) + 1000ull * NSEC_PER_MSEC;
		while (__atomic_load_n(&mdm_ip_hw0_poller_active, __ATOMIC_ACQUIRE) &&
		    op_nanoseconds(NULL) < deadline)
			delay_for_interval(1, NSEC_PER_MSEC);
		bool drained = !__atomic_load_n(&mdm_ip_hw0_poller_active, __ATOMIC_ACQUIRE);
		if (!drained)
			__atomic_store_n(&mdm_mhi.ownership_dirty, true, __ATOMIC_RELEASE);
		say("modem: IP_HW0 event poller drained=%u\n", drained);
	} else {
		delay_for_interval(20, NSEC_PER_MSEC);  /* the poll thread out of the device's window */
	}
	hk_modem_quiesce();                     /* no link and no firmware now, however long the run goes on */
	delay_for_interval(1, NSEC_PER_SEC);
	pins_say("a second after the last reset");
	say("modem: level 3 is done\n");
}
