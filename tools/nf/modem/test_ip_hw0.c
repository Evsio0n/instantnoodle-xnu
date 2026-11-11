/* Execute the board's actual command-only transaction against a 24-MiB
 * in-memory MHI endpoint. Build with the matching profile extracted by
 * test_ip_hw0.py. No host device, socket, firmware file or IP payload. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modem_mhi.c"
#include "board-profile.inc"

enum { BHI = 0x200, CHDB = 0x400, ERDB = 0x900, WINDOW = 0x1000 };
enum fault { NONE, NO_EE_EVENT, BAD_CFG, DROP_START_DL, DROP_RESET_UL, BAD_DL_EVENT };
static struct {
	struct mhi_host host;
	uint8_t *pool;
	size_t used;
	unsigned uncached, cmd_rp, count, chdb[128];
	unsigned numbers[16], types[16];
	uint32_t regs[WINDOW/4];
	uint64_t ns;
	bool locked;
	enum fault fault;
} dev;
static const uint8_t synthetic_sbl[547904]; /* Retained ticket #263 SBL length. */

static void *allocate(void *ctx, size_t n, size_t align, uint64_t *iova)
{
	(void)ctx;
	size_t start = (dev.used + align - 1) & ~(align - 1);
	assert(start <= MDM_POOL_SIZE && n <= MDM_POOL_SIZE - start);
	dev.used = start + n;
	*iova = MDM_ADDRESSES + start;
	return dev.pool + start;
}
static void *uncached(void *ctx, size_t n, size_t align, uint64_t *iova)
{
	dev.uncached++;
	assert(n == 2048 * MHI_ELEMENT);
	return allocate(ctx, n, align, iova);
}
static uint8_t *mapped(uint64_t addr, size_t n)
{
	assert(addr >= MDM_ADDRESSES && addr - MDM_ADDRESSES <= dev.used &&
	    n <= dev.used - (addr - MDM_ADDRESSES));
	return dev.pool + (addr - MDM_ADDRESSES);
}
static uint32_t reg_read(void *ctx, uint32_t off)
{
	(void)ctx; assert(off <= WINDOW - 4 && !(off & 3));
	return dev.regs[off/4];
}
static uint64_t reg64(unsigned off)
{
	return (uint64_t)reg_read(NULL, off + 4) << 32 | reg_read(NULL, off);
}
static void post(unsigned ring, uint64_t pointer, unsigned word0, unsigned word1)
{
	uint8_t *ctx = mapped(reg64(MHI_REG_ECABAP) + ring * MHI_CONTEXT, MHI_CONTEXT);
	uint64_t base = get64(ctx + CTX_RBASE), length = get64(ctx + CTX_RLEN), rp = get64(ctx + CTX_RP);
	assert(base && length && rp >= base && rp < base + length && rp != get64(ctx + CTX_WP));
	write_element(mapped(rp, MHI_ELEMENT), pointer, word0, word1);
	rp += MHI_ELEMENT;
	put64(ctx + CTX_RP, rp == base + length ? base : rp);
}
static void commands(void)
{
	uint8_t *ctx = mapped(reg64(MHI_REG_CRCBAP), MHI_CONTEXT);
	uint64_t base = get64(ctx + CTX_RBASE), length = get64(ctx + CTX_RLEN);
	uint64_t rp = base + MHI_ELEMENT * dev.cmd_rp, wp = reg64(MHI_REG_CRDB);
	while (rp != wp) {
		uint32_t flags;
		memcpy(&flags, mapped(rp, MHI_ELEMENT) + 12, 4);
		unsigned channel = flags >> 24, op = (flags >> 16) & 0xff;
		assert(dev.count < sizeof(dev.numbers) / sizeof(dev.numbers[0]));
		dev.numbers[dev.count] = channel;
		dev.types[dev.count++] = op;
		bool drop = (dev.fault == DROP_START_DL && channel == 101 && op == MHI_TYPE_START_CHAN) ||
		    (dev.fault == DROP_RESET_UL && channel == 100 && op == MHI_TYPE_RESET_CHAN);
		if (!drop) {
			uint8_t *chctx = mapped(reg64(MHI_REG_CCABAP) + channel * MHI_CONTEXT, MHI_CONTEXT);
			chctx[CTX_CFG] = op == MHI_TYPE_START_CHAN ? 2 : 0;
			post(0, rp, MHI_CODE_SUCCESS << 24, MHI_TYPE_CMD_DONE << 16 | channel << 24);
		}
		if (dev.fault == BAD_DL_EVENT && channel == 101 && op == MHI_TYPE_START_CHAN)
			post(8, 0, MHI_CODE_EOT << 24 | 1, MHI_TYPE_TX << 16 | 101u << 24);
		rp += MHI_ELEMENT;
		if (rp == base + length) rp = base;
		dev.cmd_rp = (unsigned)((rp - base) / MHI_ELEMENT);
		put64(ctx + CTX_RP, rp);
	}
}
static void reg_write(void *ctx, uint32_t off, uint32_t val)
{
	(void)ctx; assert(off <= WINDOW - 4 && !(off & 3));
	dev.regs[off/4] = val;
	if (off == MHI_REG_CRDB) commands();
	if (off >= CHDB && off < CHDB + 8 * 128 && (off & 7) == 0) dev.chdb[(off-CHDB)/8]++;
}
static void lock(void *c) { (void)c; assert(!dev.locked); dev.locked = true; }
static void unlock(void *c) { (void)c; assert(dev.locked); dev.locked = false; }
static uint64_t nanos(void *c) { (void)c; return dev.ns; }
static void pause_us(void *c, unsigned us) { (void)c; assert(!dev.locked); dev.ns += 1000ull * us; }
static void quiet(void *c, const char *msg) { (void)c; (void)msg; }
static void status(void *c, enum mhi_host_status s) { (void)c; (void)s; }
static void unexpected_receive(void *c, unsigned ch, const void *data, size_t n)
{
	(void)c; (void)ch; (void)data; (void)n; abort();
}
static const struct mhi_host_ops ops = { .read=reg_read, .write=reg_write, .dma_alloc=allocate,
	.nanoseconds=nanos, .lock=lock, .unlock=unlock, .pause=pause_us, .say=quiet, .status=status,
	.receive=unexpected_receive, .dma_alloc_uncached=uncached };

static void run(enum fault fault)
{
	struct mhi_host *h = &dev.host;
	struct mdm_ip_hw0_receipt receipt;
	struct mhi_host_config config = board_profile;
	memset(&dev, 0, sizeof(dev));
	dev.fault = fault;
	dev.pool = calloc(1, MDM_POOL_SIZE);
	assert(dev.pool);
	assert(mdm_mhi_ip_hw0_config(&config) == 0 && mdm_mhi_ip_hw0_config(&config) == EINVAL);
	config.image = synthetic_sbl;
	config.image_length = config.sbl_length = sizeof(synthetic_sbl);
	config.register_window_size = WINDOW;
	assert(!mhi_host_init(h, &ops, NULL, &config));
	uint64_t msi;
	size_t after_rings = dev.used;
	allocate(NULL, 4096, 4096, &msi); /* The board's separate MSI page, after host init. */
	assert(dev.uncached == 1);
	assert(!h->channels[4].buffers && !h->channels[5].buffers);
	assert(h->channels[4].ring.elements == 512 && h->channels[5].ring.elements == 512);
	h->powered = true;
	h->bhi = BHI;
	h->state = MHI_HOST_STATE_M0;
	h->channels[4].transfer_db = MHI_HOST_DB_BURST_SWITCH;
	h->channels[5].transfer_db = MHI_HOST_DB_BURST;
	dev.regs[MHI_REG_CHDBOFF/4] = CHDB;
	dev.regs[MHI_REG_ERDBOFF/4] = ERDB;
	dev.regs[MHI_REG_MHICFG/4] = 128 | (11u << 8); /* Ticket 264 hardware NCH/NHWCH. */
	dev.regs[MHI_REG_MHISTATUS/4] = MHI_HOST_STATE_M0 << 8;
	dev.regs[(BHI + MHI_BHI_EXECENV)/4] = MHI_HOST_EE_AMSS;
	assert(!setup_mmio(h));
	assert(!activate_event_rings(h, false));
	if (fault != NO_EE_EVENT) post(0, 0, MHI_HOST_EE_AMSS << 24, MHI_TYPE_EE << 16);
	int rc = mdm_mhi_to_mission(h, 1000);
	if (fault == NO_EE_EVENT) {
		assert(rc && !dev.count && !h->event_active[7] && !h->event_active[8]);
		goto done;
	}
	assert(!rc && dev.count == 2 && dev.numbers[0] == 20 && dev.numbers[1] == 21);
	assert(!h->channels[4].running && !h->channels[5].running);
	if (fault == BAD_CFG) dev.regs[MHI_REG_MHICFG/4] &= 0x00ffffffu;
	rc = mdm_mhi_ip_hw0_trial(h, &receipt);
	assert(!dev.chdb[100] && !dev.chdb[101]);
	for (unsigned i=4; i<6; i++) {
		assert(!h->channels[i].ring.rp && !h->channels[i].ring.wp);
		assert(!h->channels[i].buffers);
	}
	assert(dev.used < MDM_POOL_SIZE);
	if (fault == NONE) {
		unsigned expect_ch[] = {20,21,100,101,101,100};
		assert(!rc && !receipt.quarantined && receipt.start_ack == 3 && receipt.reset_ack == 3);
		assert(receipt.ner == 17 && receipt.nhwer == 10 && !receipt.er8_events);
		assert(receipt.empty == 3 && !receipt.buffers && receipt.ipcr_running == 3);
		assert(receipt.ipcr_ul_state == 2 && receipt.ipcr_dl_state == 2 &&
		    receipt.ee == MHI_HOST_EE_AMSS && receipt.state == MHI_HOST_STATE_M0);
		assert(dev.count == 6 && !memcmp(dev.numbers, expect_ch, sizeof(expect_ch)));
		assert(dev.types[2] == MHI_TYPE_START_CHAN && dev.types[3] == MHI_TYPE_START_CHAN);
		assert(dev.types[4] == MHI_TYPE_RESET_CHAN && dev.types[5] == MHI_TYPE_RESET_CHAN);
		printf("real board profile: pool after rings=%zu, after MSI=%zu/%u, 512+512 empty TREs, 0 IP doorbells\n",
		    after_rings, dev.used, MDM_POOL_SIZE);
	} else if (fault == BAD_CFG) {
		assert(rc && dev.count == 2 && !receipt.start_attempted && !receipt.quarantined);
	} else {
		assert(rc && receipt.quarantined && !mhi_host_ownership_clean(h));
		assert(receipt.start_ack == (fault == DROP_START_DL ? 1u : 3u));
		assert(receipt.reset_ack == (fault == DROP_RESET_UL ? 2u : 0u));
		unsigned count = dev.count;
		assert(mdm_mhi_ip_hw0_trial(h, &receipt) && receipt.quarantined && dev.count == count);
	}
done:
	assert(!dev.locked);
	/* Tear down this entire synthetic endpoint. No production pool is freed. */
	free(dev.pool);
}

int main(void)
{
	/* A board worker starts from a new copy each time. Neither the sparse
	 * table nor its data channels must replace EFS in the next profile. */
	for (unsigned order=0; order<2; order++) {
		struct mhi_host_config diag=board_profile, ordinary=board_profile;
		if (order == 0) assert(!mdm_mhi_ip_hw0_config(&diag));
		ordinary.channel_count=6; /* GNSS requests the baseline EFS pair. */
		assert(ordinary.channels[4].number == 10 && ordinary.channels[5].number == 11);
		assert(ordinary.event_count == 3 && !ordinary.hardware_event_count && !ordinary.events[8].elements);
		if (order == 1) assert(!mdm_mhi_ip_hw0_config(&diag));
		assert(diag.channels[4].number == 100 && board_profile.channel_count == 4 &&
		    board_profile.channels[4].number == 10 && board_profile.event_count == 3);
	}
	for (enum fault f=NONE; f<=BAD_DL_EVENT; f++) run(f);
	puts("PASS board IP_HW0 START/RESET, no-EE and register gates, lost command completions, unexpected DL and quarantine.");
}
