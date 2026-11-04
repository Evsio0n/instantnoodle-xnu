/* GENERATED OFFLINE ONLY. See start-adapter-review.md. No default enable. */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <os/atomic_private.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/memory_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>

#include <sys/hk_input.h>
#include <sys/instantnoodle.h>
#include <sys/hk_board.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_return(void);
extern kern_return_t thread_terminate(thread_t thread);
extern vm_offset_t pe_arm_get_soc_base_phys(void);
extern dev_t mdevlookup(int devid);
extern int mdevgetkernelrange(dev_t dev, vm_offset_t *base, uint64_t *size);


extern lck_mtx_t hk_aop_mailbox_lock;
extern lck_mtx_t hk_smem_alloc_lock;

static LCK_GRP_DECLARE(slpi_lock_group, "hk-slpi");

#include "slpi_smp2p_checks.h"
#define STEPS                   7
#define HK_SOC_BASE        0x00400000ul    

static uint32_t last_step, pause_ms = 2200, listen_seconds = 3;
static uint32_t step;                   
static uint32_t new_from;               
static char last_write[96] = "nothing written";
static bool leaving;                    


extern void loader_log_putc(char c);

static LCK_MTX_DECLARE(say_lock, &slpi_lock_group);
static bool quiet;

static void
say_line(const char *line, bool aloud)
{
	if (leaving) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
		return;
	}
	lck_mtx_lock(&say_lock);
	if (!quiet || aloud) {
		for (const char *c = line; *c != 0; c++) {
			loader_log_putc(*c);
		}
	}
	printf("%s", line);
	lck_mtx_unlock(&say_lock);
}

static int say(const char *format, ...) __printflike(1, 2);
static int tell(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, false);
	return length;
}

static int
tell(const char *format, ...)
{
	char line[400];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	say_line(line, true);
	return length;
}
#define printf say

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static uint64_t
now_us(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds / 1000;
}


static void
settle(void)
{
	__asm__ volatile ("dsb sy");
}

static bool
option(const char *name)
{
	uint32_t value = 0;

	return PE_parse_boot_argn(name, &value, sizeof(value)) && value != 0;
}



static uint32_t __attribute__((noinline))
rd(vm_offset_t address)
{
	uint32_t value;

	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

static void __attribute__((noinline))
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

struct block {
	const char *name;
	uint32_t phys, size;
	vm_offset_t base;
};

static void
need(struct block *b)
{
	if (b->base == 0) {
		b->base = ml_io_map(b->phys, b->size);
	}
}


static void
hold(void)
{
	if (step >= new_from && pause_ms && !leaving) {
		sleep_ms(pause_ms);
	}
}

static void
announce(const char *what)
{
	printf("slpi: step %u: %s\n", step, what);
	hold();
}

static uint32_t
get(struct block *b, const char *name, uint32_t offset)
{
	uint32_t value;

	need(b);
	value = rd(b->base + offset);
	printf("slpi: %u: %s %s (%#x) = %#x\n", step, b->name, name, b->phys + offset, value);
	return value;
}

static void
put(struct block *b, const char *name, uint32_t offset, uint32_t value)
{
	need(b);
	printf("slpi: %u: about to write %s %s at %#x: %#x\n", step, b->name, name, b->phys + offset, value);
	snprintf(last_write, sizeof(last_write), "step %u wrote %#x to %s %s at %#x", step, value, b->name, name, b->phys + offset);
	wr(b->base + offset, value);
}



#define SMEM_PHYS               0x80900000UL
#define SMEM_SIZE               0x00200000u
#define SMEM_US                 0
#define SMEM_SLPI               3               
#define SMEM_GLOBAL             0xfffe
#define SMEM_CANARY             0xa5a5

struct smem_table_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};
struct smem_table {
	uint8_t magic[4];
	uint32_t version, count, reserved[5];
};
struct smem_partition {
	uint8_t magic[4];
	uint16_t host0, host1;
	uint32_t size, free_uncached, free_cached, reserved[3];
};
struct smem_entry {
	uint16_t canary, item;
	uint32_t size;
	uint16_t padding_data, padding_header;
	uint32_t reserved;
};
struct smem_item {
	uint32_t item, offset, size;
};


#define ITEM_SMP2P_OURS         430
#define ITEM_SMP2P_THEIRS       481
#define ITEM_CRASH_REASON       424
#define ITEM_GLINK_DESCRIPTOR   478
#define ITEM_GLINK_OUR_QUEUE    479
#define ITEM_GLINK_THEIR_QUEUE  480

static const volatile uint8_t *smem;

static bool
smem_peek(uint64_t offset, void *to, uint32_t size)
{
	if (smem == NULL) {
		smem = (const volatile uint8_t *)ml_io_map_unmappable(SMEM_PHYS, SMEM_SIZE, VM_WIMG_WCOMB);
	}
	if (offset + size > SMEM_SIZE) {
		return false;
	}
	for (uint32_t i = 0; i < size; i++) {
		((uint8_t *)to)[i] = smem[offset + i];
	}
	return true;
}

static void
smem_poke(uint32_t offset, uint32_t value)
{
	wr((vm_offset_t)smem + offset, value);
}

static uint32_t
smem_word(uint32_t offset)
{
	return rd((vm_offset_t)smem + offset);
}

static uint32_t
smem_partition(uint16_t one, uint16_t other, uint32_t *cacheline, uint32_t *size)
{
	struct smem_table table;
	struct smem_table_entry entry;
	struct smem_partition header;

	if (!smem_peek(SMEM_SIZE - 0x1000, &table, sizeof(table)) || memcmp(table.magic, "$TOC", 4) != 0 || table.version != 1 ||
	    table.count > 64) {
		return 0;
	}
	for (uint32_t i = 0; i < table.count; i++) {
		if (!smem_peek(SMEM_SIZE - 0x1000 + sizeof(table) + i * sizeof(entry), &entry, sizeof(entry)) || entry.offset == 0 ||
		    entry.size == 0) {
			continue;
		}
		if (!((entry.host0 == one && entry.host1 == other) || (entry.host0 == other && entry.host1 == one))) {
			continue;
		}
		if ((uint64_t)entry.offset + entry.size > SMEM_SIZE - 0x1000 || !smem_peek(entry.offset, &header, sizeof(header)) ||
		    memcmp(header.magic, "$PRT", 4) != 0 || header.size != entry.size) {
			return 0;
		}
		*cacheline = entry.cacheline;
		*size = entry.size;
		return entry.offset;
	}
	return 0;
}

static unsigned
smem_items(uint32_t partition, uint32_t cacheline, struct smem_item *found, unsigned max)
{
	struct smem_partition header;
	struct smem_entry entry;
	uint32_t at, step_back;
	unsigned count = 0;

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.size ||
	    header.free_cached > header.size) {
		return 0;
	}
	for (at = sizeof(header); at + sizeof(entry) <= header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > header.size) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at + sizeof(entry) + entry.padding_header;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (cacheline == 0 || (cacheline & (cacheline - 1))) {
		return count;
	}
	step_back = ((uint32_t)sizeof(entry) + cacheline - 1) & ~(cacheline - 1);
	for (at = header.size - step_back; at > header.free_cached && at < header.size;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.size > at) {
			break;
		}
		if (count < max) {
			found[count].item = entry.item;
			found[count].offset = partition + at - entry.size;
			found[count].size = entry.size - entry.padding_data;
		}
		count++;
		if (entry.size + step_back > at) {
			break;
		}
		at -= entry.size + step_back;
	}
	return count;
}


static uint32_t
smem_item(uint32_t item, uint32_t *size)
{
	static struct smem_item found[256];
	static const uint16_t hosts[2][2] = { { SMEM_US, SMEM_SLPI }, { SMEM_GLOBAL, SMEM_GLOBAL } };

	for (unsigned h = 0; h < 2; h++) {
		uint32_t cacheline = 0, partition_size = 0;
		uint32_t partition = smem_partition(hosts[h][0], hosts[h][1], &cacheline, &partition_size);
		unsigned count = smem_items(partition, cacheline, found, 256);

		for (unsigned i = 0; i < count && i < 256; i++) {
			if (found[i].item == item) {
				*size = found[i].size;
				return found[i].offset;
			}
		}
	}
	return 0;
}

static void
show_partition(void)
{
	static struct smem_item found[256];
	struct smem_partition header;
	uint32_t cacheline = 0, size = 0, partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &size);
	unsigned count;
	char line[320];

	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("slpi: %u: there is no partition for us and the sensor DSP (0 and 2)\n", step);
		return;
	}
	count = smem_items(partition, cacheline, found, 256);
	printf("slpi: %u: the partition for us and the sensor DSP (0 and 2): at %#lx, %#x bytes, of which %#x free; %u items\n", step,
	    SMEM_PHYS + partition, size, header.free_cached - header.free_uncached, count);
	for (unsigned i = 0; i < count && i < 256;) {
		int at = 0;

		for (unsigned n = 0; n < 12 && i < count && i < 256; n++, i++) {
			at += snprintf(line + at, sizeof(line) - (size_t)at, " %u (%u)", found[i].item, found[i].size);
		}
		printf("slpi: %u:   items (bytes):%s\n", step, line);
	}
}



struct smp2p {
	uint32_t magic;
	uint32_t version;
	uint16_t local, remote, total, valid;
	uint32_t flags;
	struct {
		char name[16];
		uint32_t value;
	} entry[16];
};
#define SMP2P_MAGIC             0x504d5324
#define SMP2P_VERSION           1
#define SMP2P_RESTART_ACKS      1u
#define SMP2P_RESTART_DONE      (1u << 0)
#define SMP2P_RESTART_ACK       (1u << 1)
#define SMP2P_AT_VERSION        4
#define SMP2P_AT_PROCESSORS     8
#define SMP2P_AT_COUNTS         12
#define SMP2P_AT_FLAGS          16
#define SMP2P_AT_NAME(i)        (20 + 20 * (i))
#define SMP2P_AT_VALUE(i)       (36 + 20 * (i))


#define DSP_FATAL               (1u << 0)
#define DSP_READY               (1u << 1)
#define DSP_HANDOVER            (1u << 2)
#define DSP_STOP_ACK            (1u << 3)
#define DSP_SHUTDOWN_ACK        (1u << 7)


#define OUR_ENTRY_STOP          "master-kernel"
#define DSP_STOP                (1u << 0)

static bool
dsp_words(struct smp2p *words)
{
	static uint32_t at;
	uint32_t size = 0;

	if (at == 0) {
		at = smem_item(ITEM_SMP2P_THEIRS, &size);
		if (at == 0 || size < sizeof(*words)) {
			at = 0;
			return false;
		}
	}
	uint32_t state; return smem_peek(at, words, sizeof(*words)) && slpi_smp2p_state((const uint8_t *)words,sizeof(*words),3,0,"slave-kernel",&state);
}

static int
smp2p_entry(const struct smp2p *words, const char *name)
{
	for (unsigned i = 0; i < words->valid && i < 16; i++) {
		if (strncmp(words->entry[i].name, name, sizeof(words->entry[i].name)) == 0) {
			return (int)i;
		}
	}
	return -1;
}

static uint32_t
dsp_state(const struct smp2p *words)
{
	int entry = smp2p_entry(words, "slave-kernel");

	return entry < 0 ? 0 : words->entry[entry].value;
}

static void
show_words(const char *prefix, const struct smp2p *words)
{
	char line[320];
	int at = 0;

	line[0] = 0;
	for (unsigned i = 0; i < words->valid && i < 16 && at < (int)sizeof(line) - 40; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %.16s = %#x;", words->entry[i].name, words->entry[i].value);
	}
	printf("slpi: %u: %s: SMP2P version %u, features %#x, from processor %u to %u, flags %#x, %u of %u entries:%s\n", step, prefix,
	    words->version & 0xff, words->version >> 8, words->local, words->remote, words->flags, words->valid, words->total, line);
}

static void
show_crash_reason(void)
{
	char text[81];
	uint32_t size = 0, at = smem_item(ITEM_CRASH_REASON, &size);

	if (at == 0 || size == 0) {
		printf("slpi: %u: item %u, the DSP's last words: not there\n", step, ITEM_CRASH_REASON);
		return;
	}
	bzero(text, sizeof(text));
	smem_peek(at, text, size < sizeof(text) - 1 ? size : sizeof(text) - 1);
	if (text[0] == 0) {
		printf("slpi: %u: item %u, the DSP's last words (%u bytes): none\n", step, ITEM_CRASH_REASON, size);
	} else {
		printf("slpi: %u: THE AUDIO DSP'S LAST WORDS (item %u, %u bytes): \"%s\"\n", step, ITEM_CRASH_REASON, size, text);
	}
}



#define QMP_MAGIC               0x00
#define QMP_MAGIC_VALUE         0x4d41494c
#define QMP_VERSION             0x04
#define QMP_THEIR_LINK          0x0c
#define QMP_THEIR_LINK_ACK      0x10
#define QMP_THEIR_CHANNEL       0x14
#define QMP_THEIR_CHANNEL_ACK   0x18
#define QMP_OUR_LINK            0x24
#define QMP_OUR_LINK_ACK        0x28
#define QMP_OUR_CHANNEL         0x2c
#define QMP_OUR_CHANNEL_ACK     0x30
#define QMP_OUR_BOX_SIZE        0x34
#define QMP_OUR_BOX             0x38
#define QMP_UP                  0x0000ffffu
#define QMP_DOWN                0xffff0000u
#define QMP_MESSAGE             64

static struct block aoss = { "mailbox", 0x0c300000, 0x1000 };

#define IPCC_SEND               0x0c
#define IPCC_RECEIVED           0x10
#define IPCC_ENABLE             0x14
#define IPCC_DISABLE            0x18
#define IPCC_CLEAR              0x1c
#define IPCC_NOBODY             0xffffffffu
#define IPCC_AOP                0u
#define IPCC_SLPI              4u              
#define IPCC_GLINK              0u
#define IPCC_SMP2P              2u

static struct block ipcc = { "ipcc", 0x00408000, 0x1000 };

static struct {
	uint32_t glink, smp2p, said;
	uint64_t since;
} doorbell;


static void
doorbells(void)
{
	for (unsigned i = 0; i < 8; i++) {
		uint32_t who = rd(ipcc.base + IPCC_RECEIVED);

		if (who == IPCC_NOBODY || who >> 16 != IPCC_SLPI) {
			return;
		}
		wr(ipcc.base + IPCC_CLEAR, who);
		doorbell.glink += (who & 0xffff) == IPCC_GLINK;
		doorbell.smp2p += (who & 0xffff) == IPCC_SMP2P;
		if (doorbell.said < 24) {
			uint64_t now = now_us() - doorbell.since;

			doorbell.said++;
			printf("slpi: doorbell: +%llu.%03llu ms: signal %u of the sensor DSP; heard and cleared\n", now / 1000, now % 1000,
			    who & 0xffff);
		}
	}
}

static uint32_t our_box;                

static bool
mailbox_wait(uint32_t offset, uint32_t value, uint32_t ms)
{
	for (uint32_t i = 0; i < ms * 10; i++) {
		if (rd(aoss.base + offset) == value) {
			return true;
		}
		sleep_us(100);
	}
	return rd(aoss.base + offset) == value;
}

static void
ring_aop(void)
{
	wr(ipcc.base + IPCC_SEND, IPCC_AOP << 16 | 0);
}


static bool
mailbox_open(void)
{
	uint32_t box, size;

	need(&aoss);
	need(&ipcc);
	if (get(&aoss, "magic", QMP_MAGIC) != QMP_MAGIC_VALUE || get(&aoss, "version", QMP_VERSION) != 1) {
		printf("slpi: %u: not a mailbox this knows\n", step);
		return false;
	}
	box = get(&aoss, "our mailbox", QMP_OUR_BOX);
	size = get(&aoss, "our mailbox's size", QMP_OUR_BOX_SIZE);
	if (size < QMP_MESSAGE + 4 || box < 0x3c || box + size > aoss.size || (box & 3)) {
		printf("slpi: %u: our mailbox is not where a message fits\n", step);
		return false;
	}
	if (rd(aoss.base + QMP_OUR_LINK) == QMP_UP && rd(aoss.base + QMP_OUR_LINK_ACK) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL) == QMP_UP && rd(aoss.base + QMP_THEIR_CHANNEL) == QMP_UP &&
	    rd(aoss.base + QMP_OUR_CHANNEL_ACK) == QMP_UP) {
		printf("slpi: %u: the mailbox is open already (the Wi-Fi's start, or a kernel before this one, opened it)\n", step);
		our_box = box;
		return true;
	}
	put(&aoss, "their link, seen by us", QMP_THEIR_LINK_ACK, rd(aoss.base + QMP_THEIR_LINK));
	put(&aoss, "our link", QMP_OUR_LINK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_LINK_ACK, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR LINK within a second; taken down again\n", step);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_THEIR_CHANNEL, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT OPEN ITS CHANNEL within a second; ours is taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	put(&aoss, "their channel, seen by us", QMP_THEIR_CHANNEL_ACK, QMP_UP);
	ring_aop();
	if (!mailbox_wait(QMP_OUR_CHANNEL_ACK, QMP_UP, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT ACKNOWLEDGE OUR CHANNEL within a second; taken down again\n", step);
		put(&aoss, "our channel", QMP_OUR_CHANNEL, QMP_DOWN);
		put(&aoss, "our link", QMP_OUR_LINK, QMP_DOWN);
		ring_aop();
		return false;
	}
	printf("slpi: %u: THE MAILBOX TO THE ALWAYS-ON PROCESSOR IS OPEN\n", step);
	our_box = box;
	return true;
}


static bool
mailbox_send(const char *text)
{
	uint32_t words[QMP_MESSAGE / 4] = { 0 };
	bool sent = false;

	if (!leaving) {
		lck_mtx_lock(&hk_aop_mailbox_lock);
	}
	if (our_box == 0 && (leaving || !mailbox_open())) {
		goto out;
	}
	strlcpy((char *)words, text, sizeof(words));
	printf("slpi: %u: about to tell the always-on processor \"%s\"\n", step, text);
	snprintf(last_write, sizeof(last_write), "step %u sent the always-on processor a message", step);
	if (rd(aoss.base + our_box) != 0) {
		printf("slpi: %u: our mailbox still holds a message of %u bytes: not sent\n", step, rd(aoss.base + our_box));
		goto out;
	}
	for (unsigned i = 0; i < QMP_MESSAGE / 4; i++) {
		wr(aoss.base + our_box + 4 + 4 * i, words[i]);
	}
	wr(aoss.base + our_box, QMP_MESSAGE);
	(void)rd(aoss.base + our_box);
	ring_aop();
	if (!mailbox_wait(our_box, 0, 1000)) {
		printf("slpi: %u: THE ALWAYS-ON PROCESSOR DOES NOT TAKE THE MESSAGE within a second; it is taken back\n", step);
		wr(aoss.base + our_box, 0);
		goto out;
	}
	printf("slpi: %u: the always-on processor has taken it\n", step);
	sent = true;
out:
	if (!leaving) {
		lck_mtx_unlock(&hk_aop_mailbox_lock);
	}
	return sent;
}




#define CMD_DB_PHYS             0x80860000UL
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

static const struct cmd_db_header *cmd_db;

static struct block rsc = { "rsc", 0x18220000, 0x10000 };
#define RSC_TCS_BASE            0xd00
#define RSC_IRQ_STATUS          (RSC_TCS_BASE + 0x04)   
#define RSC_IRQ_CLEAR           (RSC_TCS_BASE + 0x08)
#define RSC_TCS(n)              (RSC_TCS_BASE + 672 * (n))
#define TCS_WAIT_FOR_DONE       0x10
#define TCS_CONTROL             0x14
#define TCS_STATUS              0x18                    
#define TCS_ENABLED             0x1c
#define TCS_COMMAND(c)          (0x30 + 20 * (c))       
#define TCS_ACTIVE_MODE         (1u << 16)
#define TCS_TRIGGER             (1u << 24)
#define MSGID_WRITE_WAITED      (8u | 1u << 8 | 1u << 16)       
#define RPMH_TCS                0                       



static bool
step_survey(void)
{
	struct smp2p words;
	uint32_t size = 0;

	announce("read our partition with the sensor DSP in the shared memory (0x80900000), and its items");
	show_partition();
	if (dsp_words(&words)) {
		show_words("the DSP's item (443) is there: it has run since the phone was started", &words);
	} else {
		printf("slpi: %u: the DSP's SMP2P item (443) is not there: it has not run since the phone was started\n", step);
	}
	printf("slpi: %u: our SMP2P item (429): %s; GLINK's descriptor (478): %s, our queue (479): %s, the DSP's (480): %s\n", step,
	    smem_item(ITEM_SMP2P_OURS, &size) ? "there" : "not there", smem_item(478, &size) ? "there" : "NOT THERE",
	    smem_item(479, &size) ? "there" : "NOT THERE", smem_item(480, &size) ? "there" : "not there (the DSP makes it)");
	show_crash_reason();
	need(&ipcc);
	printf("slpi: %u: the doorbells say %#x\n", step, rd(ipcc.base + IPCC_RECEIVED));
	return smem_partition(SMEM_US, SMEM_SLPI, &size, &size) != 0;
}




static uint32_t slpi_memory = 0x90600000u, slpi_memory_size = 0x01500000u;
#define SLPI_MEMORY             slpi_memory
#define SLPI_MEMORY_SIZE        slpi_memory_size
#define PAS_SLPI                12               



static const uint8_t *
root_file(const char *name, uint64_t *size)
{
	char path[64];
	vfs_context_t context = vfs_context_kernel();
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	uint8_t *data;
	int left = 0, error;

	if (rootvnode == NULLVP) {
		return NULL;
	}
	snprintf(path, sizeof(path), "/hackios/fw/%s", name);
	if (vnode_lookup(path, 0, &vnode, context) != 0) {
		return NULL;
	}
	VATTR_INIT(&attributes);
	VATTR_WANTED(&attributes, va_data_size);
	if (vnode_getattr(vnode, &attributes, context) != 0 || attributes.va_data_size == 0 ||
	    attributes.va_data_size > SLPI_MEMORY_SIZE) {
		vnode_put(vnode);
		return NULL;
	}
	data = kalloc_data(attributes.va_data_size, Z_WAITOK);
	if (data == NULL) {
		vnode_put(vnode);
		return NULL;
	}
	error = vn_rdwr(UIO_READ, vnode, (caddr_t)data, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
	    vfs_context_ucred(context), &left, vfs_context_proc(context));
	vnode_put(vnode);
	if (error != 0 || left != 0) {
		printf("slpi: %u: %s: read error %d, %d bytes left\n", step, path, error, left);
		kfree_data(data, attributes.va_data_size);
		return NULL;
	}
	*size = attributes.va_data_size;
	return data;
}

static bool from_root;                  


#include <libkern/crypto/sha2.h>
static int native_hash_ok(const char *name,const uint8_t *data,uint64_t size) {
static const struct {const char *name;uint64_t size;uint8_t sha[32];} expected[]={
{"slpi.b02",82680,{0xc3,0xcc,0xf5,0x63,0xc6,0x1e,0xfc,0x80,0x9c,0x65,0x55,0xbb,0x63,0x0a,0xa0,0x6f,0xf3,0xb6,0x8e,0x7c,0x27,0xe7,0xa7,0x5c,0x03,0x87,0x9a,0x96,0xea,0x95,0xfd,0xcc}},
{"slpi.b03",247232,{0x26,0xf4,0x6f,0x78,0x2a,0xea,0x50,0xec,0x60,0xea,0x8c,0x53,0xf6,0x73,0x62,0xb4,0x26,0xf3,0xfa,0x42,0xe9,0x5e,0x65,0x27,0xe7,0x5a,0x61,0x38,0x16,0xe5,0x4b,0xc1}},
{"slpi.b04",2077548,{0x7f,0x89,0xd4,0x2f,0x51,0x80,0xfa,0x98,0x73,0x42,0xc3,0x5e,0x8b,0x2c,0x9e,0xda,0xc8,0x56,0xd6,0x03,0xd2,0xea,0xef,0x72,0xc8,0xec,0x08,0xf6,0x93,0xc7,0x10,0x56}},
{"slpi.b05",1733228,{0x46,0x51,0x8b,0xb2,0x6f,0x37,0x60,0xf7,0x49,0x96,0xb7,0x10,0xcb,0xee,0x88,0x75,0x8b,0x43,0xdb,0xb4,0x17,0xb9,0xc5,0xab,0x96,0xd2,0xd9,0x57,0xe9,0x71,0xe9,0xde}},
{"slpi.b06",63642,{0x74,0x5a,0xea,0xdf,0x6a,0x13,0x4e,0xc1,0xce,0x6d,0x6f,0xfa,0x6b,0x5d,0xe7,0x22,0xe0,0x58,0xf4,0xc6,0xea,0x5e,0xbe,0x5b,0xbc,0xd6,0x59,0x11,0xda,0x27,0x5d,0x45}},
{"slpi.b07",28972,{0xe4,0xc7,0xfb,0xad,0x36,0x44,0x92,0x49,0x98,0xf9,0xea,0xad,0xe6,0x10,0xee,0x41,0xc3,0x18,0xd6,0x27,0xbd,0x9d,0x56,0x07,0x3b,0x51,0x18,0x3a,0x08,0xc8,0xdc,0xee}},
{"slpi.b08",167700,{0xea,0x80,0x4a,0x79,0xde,0x6f,0xc1,0x5c,0xfe,0xcd,0x71,0x15,0x43,0x25,0xb0,0x92,0x9c,0x54,0xd0,0x27,0x1e,0xa4,0x92,0xbe,0xde,0x51,0xb1,0x35,0x5e,0x21,0x32,0x33}},
{"slpi.b09",111164,{0xff,0x55,0xe6,0x0b,0xf4,0x87,0x22,0x06,0x54,0xc4,0x8e,0x21,0xdd,0x3a,0x5e,0x81,0x3e,0xa3,0xb1,0x51,0x6c,0x13,0x0a,0x02,0x4a,0x04,0xad,0x14,0x44,0x90,0x70,0xf2}},
{"slpi.b10",82,{0xa8,0xb6,0xb0,0xf2,0x03,0x2c,0xd8,0xba,0x8e,0xaf,0xf1,0xff,0xe6,0xf5,0xa8,0x3c,0xb3,0x6d,0x46,0x2e,0xd8,0x47,0xa4,0xe2,0xb3,0x24,0x57,0xf4,0x1b,0xb9,0x71,0xc9}},
{"slpi.b11",8192,{0x9f,0x1d,0xcb,0xc3,0x5c,0x35,0x0d,0x60,0x27,0xf9,0x8b,0xe0,0xf5,0xc8,0xb4,0x3b,0x42,0xca,0x52,0xb7,0x60,0x44,0x59,0xc0,0xc4,0x2b,0xe3,0xaa,0x88,0x91,0x3d,0x47}},
{"slpi.b12",62268,{0x82,0xbf,0x6c,0x9d,0x79,0x0d,0xd3,0xb4,0x3e,0x81,0xc1,0x83,0x3d,0x03,0x6d,0x3d,0x8a,0x10,0x5e,0x83,0x57,0x40,0x06,0xf0,0x85,0x58,0x1b,0xd0,0x12,0x73,0xdc,0x5d}},
{"slpi.b13",28084,{0x50,0x09,0x57,0x9e,0x0c,0x66,0x3f,0x6c,0xe6,0x26,0xad,0xef,0x00,0x40,0xb1,0xe7,0x69,0x24,0x3b,0xcc,0x88,0x7d,0xc5,0x06,0x45,0x62,0x43,0x0c,0x31,0xb2,0x24,0xc3}},
{"slpi.b14",229024,{0x18,0xac,0xdc,0x8e,0x54,0xc2,0x7c,0x37,0xb8,0xe6,0xc5,0x3b,0x79,0xa3,0x92,0x69,0xd7,0xee,0x83,0x63,0xff,0x35,0x42,0x5e,0x64,0x3d,0x23,0xa8,0x72,0xf9,0x99,0x6f}},
{"slpi.b15",196,{0x87,0x5a,0xbd,0x26,0xc0,0xd3,0x83,0x26,0x35,0x24,0x36,0xb2,0xc7,0xc8,0x66,0xa4,0x5f,0xf0,0x01,0x07,0xb3,0x8b,0x87,0x5e,0xd6,0x58,0x28,0xc0,0x06,0xa5,0x7b,0x55}},
{"slpi.b16",2282396,{0xf8,0xdc,0xa2,0x51,0x3f,0x7c,0x81,0x69,0x35,0x52,0x65,0xb0,0x9f,0xc8,0x02,0x3f,0xa4,0x78,0xaf,0xdf,0x67,0x86,0xe6,0x06,0x23,0x3f,0x01,0x49,0x9c,0x9b,0xcf,0xc0}},
{"slpi.b17",49160,{0xde,0xe0,0x93,0x74,0xff,0x74,0x1b,0x81,0x29,0xae,0x4c,0xff,0xe0,0x02,0x48,0x27,0x6a,0xb8,0x3c,0xcd,0x35,0x20,0xbe,0x85,0xff,0xcb,0x4a,0xb8,0x5f,0x6b,0xc4,0x57}},
{"slpi.b18",14824,{0xd1,0x0a,0x52,0x54,0x07,0x9e,0x47,0x2c,0x35,0x5b,0x97,0xcd,0x9b,0xa8,0x76,0xa2,0x4e,0x96,0x91,0xbc,0xb3,0x82,0xf8,0x8d,0x9a,0x12,0x71,0xd6,0x8c,0x8d,0xab,0x6a}},
{"slpi.b19",135440,{0x57,0xc7,0x0b,0x3a,0x4f,0x41,0xec,0xd8,0xf8,0x9f,0x26,0x64,0xe6,0x56,0x97,0xc8,0xb5,0x36,0x23,0xfa,0xdf,0xaa,0x41,0xe5,0xce,0x79,0xf7,0xa2,0x40,0xb6,0x14,0x41}},
{"slpi.b20",56436,{0x74,0xf6,0x47,0x6b,0xd9,0x25,0xaa,0xff,0x2a,0x9e,0x91,0x81,0xc9,0xc3,0x2b,0xcf,0x03,0x2a,0xae,0x2e,0x41,0x91,0x81,0x68,0x9a,0x78,0x13,0xc6,0xfb,0xc0,0xdb,0x81}},
{"slpi.b21",589108,{0xd5,0x8c,0x17,0x1e,0x5d,0xa9,0xc3,0xd3,0xca,0x18,0xa9,0x06,0xf2,0xf2,0x79,0xc3,0x87,0x37,0xd4,0xe6,0x90,0x7b,0x1c,0xec,0x13,0x3d,0x93,0x61,0xaa,0x6e,0x9a,0xcc}},
{"slpi.mdt",8460,{0x9a,0x66,0x82,0xab,0x31,0x31,0x67,0xcd,0xa8,0x69,0xa6,0xa0,0x4a,0xbf,0x56,0xc9,0xb0,0xd6,0xf8,0xae,0xfa,0x99,0x7b,0xb6,0xad,0x9b,0xbf,0x65,0x95,0x20,0x0e,0x58}},
};
for(unsigned i=0;i<sizeof(expected)/sizeof(expected[0]);i++)if(strcmp(name,expected[i].name)==0){uint8_t got[32];SHA256_CTX c;if(size!=expected[i].size)return 0;SHA256_Init(&c);SHA256_Update(&c,data,size);SHA256_Final(got,&c);return memcmp(got,expected[i].sha,32)==0;}return 0;
}

static const uint8_t *image_file(const char *name,uint64_t *size) {
 const uint8_t *data=root_file(name,size);if(data&&!native_hash_ok(name,data,*size)){void *owned_data=(void *)(uintptr_t)data;kfree_data(owned_data,*size);return NULL;}if(data)from_root=true;return data;
}

#define MDT_HASH(flags)         (((flags) >> 24 & 7) == 2)
#define MDT_RELOCATABLE(flags)  (((flags) >> 27 & 1) != 0)     
#define MAX_SEGMENTS            48

struct elf_segment {
	uint32_t type, offset, vaddr, paddr, file_size, memory_size, flags, align;
};

static struct {
	const uint8_t *header;
	uint64_t header_size;
	struct {
		uint32_t at;
		uint32_t file_size, memory_size;
		const uint8_t *file;
	} segment[MAX_SEGMENTS];
	unsigned segments;
	uint32_t size;
} image;


static void
image_forget(void)
{
	if (!from_root) {
		return;
	}
	if (image.header != NULL) {
		void *data = (void *)(uintptr_t)image.header;

		kfree_data(data, image.header_size);
	}
	for (unsigned s = 0; s < image.segments; s++) {
		if (image.segment[s].file != NULL) {
			void *data = (void *)(uintptr_t)image.segment[s].file;

			kfree_data(data, image.segment[s].file_size);
		}
	}
	bzero(&image, sizeof(image));
	from_root = false;
}

static bool
step_firmware(void)
{
	struct elf_segment segment;
	const uint8_t *header;
	uint64_t size = 0, files = 0;
	uint32_t count, hashes = 0, lowest = ~0u, highest = 0, base = SLPI_MEMORY;

	announce("find the sensor DSP's image in the small image or on the root, and check it against its own header");
	image_forget();
	image.segments = 0;
	header = image_file("slpi.mdt", &size);
	if (header == NULL) {
		return false;
	}
	image.header = header;
	image.header_size = size;
	count = size >= 52 + 2 * 32 ? header[44] | header[45] << 8 : 0;
	if (memcmp(header, "\177ELF\1", 5) != 0 || count < 3 || count > MAX_SEGMENTS || 52 + 32 * count > size || size > PAGE_SIZE) {
		printf("slpi: %u: fw/slpi.mdt is not such a header (%02x %02x %02x %02x, %u segments, %llu bytes)\n", step, header[0],
		    header[1], header[2], header[3], count, size);
		goto fail;
	}
	
	for (uint32_t i = 0; i < count; i++) {
		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (segment.type == 1 && !MDT_HASH(segment.flags) && segment.memory_size != 0 && MDT_RELOCATABLE(segment.flags) &&
		    segment.paddr < lowest) {
			lowest = segment.paddr;
		}
	}
	if (lowest != ~0u) {
		printf("slpi: %u: the image is relocatable, linked at %#x: put at %#x\n", step, lowest, SLPI_MEMORY);
		base = lowest;
		lowest = ~0u;
	}
	for (uint32_t i = 0; i < count; i++) {
		char name[16];

		memcpy(&segment, header + 52 + 32 * i, sizeof(segment));
		if (MDT_HASH(segment.flags)) {
			hashes = segment.file_size;
		}
		if (segment.type != 1 || MDT_HASH(segment.flags) || segment.memory_size == 0) {
			continue;
		}
		if (segment.paddr < base || segment.file_size > segment.memory_size ||
		    (uint64_t)segment.paddr + segment.memory_size > (uint64_t)base + SLPI_MEMORY_SIZE) {
			printf("slpi: %u: segment %u (%#x, %#x bytes) is not within the DSP's memory\n", step, i, segment.paddr,
			    segment.memory_size);
			goto fail;
		}
		image.segment[image.segments].at = segment.paddr - base;
		image.segment[image.segments].file_size = segment.file_size;
		image.segment[image.segments].memory_size = segment.memory_size;
		image.segment[image.segments].file = NULL;
		if (segment.file_size != 0) {
			uint64_t file_size = 0;

			snprintf(name, sizeof(name), "slpi.b%02u", i);
			image.segment[image.segments].file = image_file(name, &file_size);
			if (image.segment[image.segments].file == NULL) {
				goto fail;
			}
			if (file_size != segment.file_size) {
				printf("slpi: %u: fw/%s has %llu bytes, the header says %u\n", step, name, file_size, segment.file_size);
				image.segments++;               
				goto fail;
			}
			files += file_size;
		}
		lowest = segment.paddr < lowest ? segment.paddr : lowest;
		highest = segment.paddr + segment.memory_size > highest ? segment.paddr + segment.memory_size : highest;
		image.segments++;
	}
	if (image.segments == 0 || lowest != base || 52 + 32 * count + (uint64_t)hashes != size) {
		printf("slpi: %u: not what TrustZone takes: %u segments to load, the lowest at %#x; the header file has %llu bytes, "
		    "the header %u and the hashes %u\n", step, image.segments, lowest, size, 52 + 32 * count, hashes);
		goto fail;
	}
	image.header = header;
	image.header_size = size;
	image.size = ((highest + 0xfff) & ~0xfffu) - base;
	printf("slpi: %u: the image is whole (%s): a header of %llu bytes (%u of hashes), %u segments, %llu bytes of files, %#x "
	    "bytes of the DSP's %#x from %#x\n", step, from_root ? "from /fw on the root" : "from the small image", size, hashes,
	    image.segments, files, image.size, SLPI_MEMORY_SIZE, SLPI_MEMORY);
	return true;
fail:
	image_forget();
	image.segments = 0;
	return false;
}



static bool
step_mailbox(void)
{
	bool open;

	announce("open the mailbox to the always-on processor, unless it is open: words of its message memory, and its doorbell");
	lck_mtx_lock(&hk_aop_mailbox_lock);
	open = mailbox_open();
	lck_mtx_unlock(&hk_aop_mailbox_lock);
	return open;
}



#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1
#include "slpi_scm_result.h"
static bool scm_unfinished;

static LCK_MTX_DECLARE(dsp_lock, &slpi_lock_group);     
static bool rails_asked;                
static bool dsp_started;
static bool stopped_by_its_word;
static uint32_t our_item;
static bool smp2p_agreed, smp2p_restart_acks, smp2p_restart_acked;

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;
 if(scm_unfinished)return SLPI_SCM_UNCERTAIN;

	function |= SCM_64 | SCM_SIP;
	printf("slpi: %u: about to ask TrustZone %s: smc %#llx, arguments %#llx, %#llx, %#llx\n", step, what, function, first,
	    second, third);
	snprintf(last_write, sizeof(last_write), "step %u asked TrustZone %#llx(%#llx, %#llx)", step, function, first, second);
	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		enum slpi_scm_action action=slpi_scm_classify((int32_t)x0,(unsigned)i+1);
 if(action==SLPI_SCM_RETAIN){scm_unfinished=true;tell("slpi: SCM nonterminal/continuation limit; retain metadata, no more PAS calls\n");return SLPI_SCM_UNCERTAIN;}
 if(action==SLPI_SCM_TERMINAL) {
			printf("slpi: %u: TrustZone returns %d, answer %#llx\n", step, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	scm_unfinished=true;
 return SLPI_SCM_UNCERTAIN;
}

static uint32_t
our_item_find(void)
{
	uint32_t size = 0, at = smem_item(ITEM_SMP2P_OURS, &size);

	return at != 0 && size >= sizeof(struct smp2p) ? at : 0;
}


static void
dsp_listen(const struct smp2p *theirs)
{
	struct smp2p ours;

	uint32_t ours_state;
	if (our_item == 0 || !smem_peek(our_item, &ours, sizeof(ours)) || !slpi_smp2p_state((const uint8_t *)&ours,sizeof(ours),0,3,"master-kernel",&ours_state)) {
		return;
	}
	if (!smp2p_agreed) {
		uint32_t features = (ours.version >> 8) & (theirs->version >> 8);

		if ((theirs->version & 0xff) != (ours.version & 0xff)) {
			return;
		}
		if (features != ours.version >> 8) {
			smem_poke(our_item + SMP2P_AT_VERSION, features << 8 | (ours.version & 0xff));
		}
		smp2p_restart_acks = features & SMP2P_RESTART_ACKS;
		smp2p_agreed = true;
		printf("slpi: %u: the DSP and we agree on SMP2P: version %u, features %#x\n", step, ours.version & 0xff, features);
	}
	if (smp2p_restart_acks && ((theirs->flags & SMP2P_RESTART_DONE) != 0) != smp2p_restart_acked) {
		smp2p_restart_acked = !smp2p_restart_acked;
		printf("slpi: %u: the DSP's flags say it has restarted (%#x): answered in our flags (bit 1 = %u), and its SMP2P doorbell\n",
		    step, theirs->flags, smp2p_restart_acked);
		smem_poke(our_item + SMP2P_AT_FLAGS, (ours.flags & ~SMP2P_RESTART_ACK) | (smp2p_restart_acked ? SMP2P_RESTART_ACK : 0));
		settle();
		wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	}
}


static bool
dsp_ask_to_stop(uint32_t bound_ms)
{
 struct smp2p ours,theirs;uint32_t at=our_item_find(),state=0,ours_state=0;int entry;
 if(!at||bound_ms>2000||!smem_peek(at,&ours,sizeof(ours))||
    !slpi_smp2p_state((const uint8_t *)&ours,sizeof(ours),0,3,"master-kernel",&ours_state)||
    (entry=smp2p_entry(&ours,"master-kernel"))<0||!dsp_words(&theirs))return false;
 state=dsp_state(&theirs);if(!(state&DSP_READY)||(state&DSP_FATAL)||(ours_state&DSP_STOP))return false;
 struct slpi_stop_transition transition={!(state&DSP_STOP_ACK)};
 bool fresh=false;uint32_t value_at=at+SMP2P_AT_VALUE((uint32_t)entry);
 tell("slpi: owned stop request, initial ACK=%u; requires observed low-to-high ACK\n",!!(state&DSP_STOP_ACK));
 smem_poke(value_at,ours_state|DSP_STOP);settle();wr(ipcc.base+IPCC_SEND,IPCC_SLPI<<16|IPCC_SMP2P);
 uint64_t began=now_us();
 while(now_us()-began<(uint64_t)bound_ms*1000){
  if(dsp_words(&theirs)){
   dsp_listen(&theirs);state=dsp_state(&theirs);
   if(state&DSP_FATAL)break;
   if(smp2p_agreed&&slpi_stop_fresh(&transition,state)){fresh=true;break;}
  }
  sleep_us(200);
 }
 smem_poke(value_at,ours_state&~DSP_STOP);settle();wr(ipcc.base+IPCC_SEND,IPCC_SLPI<<16|IPCC_SMP2P);
 tell("slpi: owned stop fresh_ack=%u final_state=%x; PAS shutdown still applies to owned DSP\n",fresh,state);
 return fresh;
}




static bool image_written;

static uint64_t
segment_word(unsigned s, uint32_t at)
{
	uint64_t word = 0;

	for (unsigned b = 0; b < 8 && at + b < image.segment[s].file_size; b++) {
		word |= (uint64_t)image.segment[s].file[at + b] << (8 * b);
	}
	return word;
}

static bool
step_image(void)
{
	volatile uint64_t *dsp_memory;
	uint64_t written = 0, cleared = 0, wrong = 0;

	if (image.segments == 0) {
		return false;
	}
	announce("put the image's segments into the sensor DSP's memory (memory of nobody's until TrustZone starts the DSP), with "
	    "zeroes where a segment is longer than its file, and read them back");
	dsp_memory = (volatile uint64_t *)ml_io_map_unmappable(SLPI_MEMORY, SLPI_MEMORY_SIZE, VM_WIMG_WCOMB);
 if (!dsp_memory) return false;
	snprintf(last_write, sizeof(last_write), "step %u wrote the image at %#x", step, SLPI_MEMORY);
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *to = dsp_memory + image.segment[s].at / 8;
		uint32_t whole = image.segment[s].memory_size & ~7u;

		if (image.segment[s].at & 7) {
			printf("slpi: %u: a segment that does not begin at a multiple of eight bytes: not written\n", step);
			ml_io_unmap((vm_offset_t)dsp_memory, SLPI_MEMORY_SIZE);
			return false;
		}
		for (uint32_t i = 0; i < whole; i += 8) {
			to[i / 8] = segment_word(s, i);
		}
		
		for (uint32_t i = whole; i < image.segment[s].memory_size; i++) {
			((volatile uint8_t *)to)[i] = (uint8_t)(segment_word(s, i & ~7u) >> (8 * (i & 7)));
		}
		written += image.segment[s].file_size;
		cleared += image.segment[s].memory_size - image.segment[s].file_size;
	}
	settle();
	for (unsigned s = 0; s < image.segments; s++) {
		volatile uint64_t *from = dsp_memory + image.segment[s].at / 8;

		uint32_t whole = image.segment[s].memory_size & ~7u;

		for (uint32_t i = 0; i < whole; i += 8) {
			wrong += from[i / 8] != segment_word(s, i);
		}
		for (uint32_t i = whole; i < image.segment[s].memory_size; i++) {
			wrong += ((volatile uint8_t *)from)[i] != (uint8_t)(segment_word(s, i & ~7u) >> (8 * (i & 7)));
		}
	}
	printf("slpi: %u: %u segments, %llu bytes written and %llu cleared; read back, %llu words of eight bytes differ\n", step,
	    image.segments, written, cleared, wrong);
	settle();
	ml_io_unmap((vm_offset_t)dsp_memory, SLPI_MEMORY_SIZE);
	image_written = wrong == 0;
	image_forget();                         
	return image_written;
}



static struct block smem_lock = { "shared memory's lock", 0x01f43000, 0x1000 };
#define LOCK_OWNER              0x000
#define LOCK_US                 1u


static bool
item_make(uint32_t partition, uint16_t item, uint32_t bytes)
{
	struct smem_partition header;
	struct smem_entry entry;
	const uint32_t item_size = (bytes + 7) & ~7u, need_bytes = sizeof(entry) + item_size;
	uint32_t owner = ~0u, at;
	bool made = false;

	lck_mtx_lock(&hk_smem_alloc_lock);         
	need(&smem_lock);
	printf("slpi: %u: about to take the lock (%#x), write an entry for item %u with %u bytes behind it at the partition's mark, "
	    "move the mark by %u, and release the lock\n", step, smem_lock.phys + LOCK_OWNER, item, item_size, need_bytes);
	snprintf(last_write, sizeof(last_write), "step %u took the shared memory's lock", step);
	for (uint32_t ms = 0; ms <= 1000; ms++) {
		wr(smem_lock.base + LOCK_OWNER, LOCK_US);
		owner = rd(smem_lock.base + LOCK_OWNER);
		if (owner == LOCK_US) {
			break;
		}
		sleep_ms(1);
	}
	if (owner != LOCK_US) {
		printf("slpi: %u: THE LOCK IS HELD BY PROCESSOR %u AND STAYS SO FOR A SECOND: nothing is written\n", step, owner);
		lck_mtx_unlock(&hk_smem_alloc_lock);
		return false;
	}
	if (!smem_peek(partition, &header, sizeof(header)) || header.free_uncached > header.free_cached ||
	    header.free_cached > header.size) {
		printf("slpi: %u: the partition's marks make no sense: nothing is written\n", step);
		goto out;
	}
	for (at = sizeof(header); at < header.free_uncached;) {
		if (!smem_peek(partition + at, &entry, sizeof(entry)) || entry.canary != SMEM_CANARY || entry.item == item ||
		    entry.size > header.size) {
			break;
		}
		at += sizeof(entry) + entry.padding_header + entry.size;
	}
	if (at != header.free_uncached) {
		printf("slpi: %u: the partition's entries do not end at its mark (+%#x, the mark is +%#x), or the item has appeared: "
		    "nothing is written\n", step, at, header.free_uncached);
		goto out;
	}
	if (at + need_bytes > header.free_cached) {
		printf("slpi: %u: no room: %u bytes wanted, %u free: nothing is written\n", step, need_bytes, header.free_cached - at);
		goto out;
	}
	smem_poke(partition + at, SMEM_CANARY | (uint32_t)item << 16);
	smem_poke(partition + at + 4, item_size);
	smem_poke(partition + at + 8, item_size - bytes);
	smem_poke(partition + at + 12, 0);
	settle();
	smem_poke(partition + __offsetof(struct smem_partition, free_uncached), at + need_bytes);
	settle();
	made = true;
out:
	wr(smem_lock.base + LOCK_OWNER, 0);
	printf("slpi: %u: the lock is released (it reads %u)\n", step, rd(smem_lock.base + LOCK_OWNER));
	lck_mtx_unlock(&hk_smem_alloc_lock);
	return made;
}



static bool
step_our_item(void)
{
	struct smem_partition header;
	struct smp2p ours;
	uint32_t cacheline = 0, size = 0, partition, name[4] = { 0 };

	announce("look for an SMP2P item of ours (429) in our partition with the sensor DSP, make it if there is none, and fill it in");
	partition = smem_partition(SMEM_US, SMEM_SLPI, &cacheline, &size);
	if (partition == 0 || !smem_peek(partition, &header, sizeof(header))) {
		printf("slpi: %u: there is no such partition\n", step);
		return false;
	}
	our_item = our_item_find();
	if (our_item != 0) { return false; }
	if (our_item != 0) {
		printf("slpi: %u: item %u is there, at %#lx: a kernel before this one made it, and it is used again\n", step,
		    ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	} else {
		if (!item_make(partition, ITEM_SMP2P_OURS, sizeof(struct smp2p))) {
			return false;
		}
		our_item = our_item_find();
		if (our_item == 0) {
			printf("slpi: %u: THE ITEM IS NOT FOUND where it was just made\n", step);
			return false;
		}
		printf("slpi: %u: item %u made, at %#lx\n", step, ITEM_SMP2P_OURS, SMEM_PHYS + our_item);
	}
	
	snprintf(last_write, sizeof(last_write), "step %u filled in our SMP2P item", step);
	for (uint32_t at = 0; at < sizeof(ours); at += 4) {
		smem_poke(our_item + at, 0);
	}
	settle();
	smem_poke(our_item, SMP2P_MAGIC);
	smem_poke(our_item + SMP2P_AT_PROCESSORS, SMEM_US | SMEM_SLPI << 16);
	strlcpy((char *)name, OUR_ENTRY_STOP, sizeof(name));
	for (uint32_t word = 0; word < 4; word++) {
		smem_poke(our_item + SMP2P_AT_NAME(0) + 4 * word, name[word]);
	}
	smem_poke(our_item + SMP2P_AT_COUNTS, 16 | 1u << 16);
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8);
	settle();
	smem_poke(our_item + SMP2P_AT_VERSION, SMP2P_RESTART_ACKS << 8 | SMP2P_VERSION);
	settle();
	smp2p_agreed = smp2p_restart_acks = smp2p_restart_acked = false;
	need(&ipcc);
	wr(ipcc.base + IPCC_SEND, IPCC_SLPI << 16 | IPCC_SMP2P);
	if (!smem_peek(our_item, &ours, sizeof(ours)) || ours.magic != SMP2P_MAGIC || smp2p_entry(&ours, OUR_ENTRY_STOP) < 0) {
		printf("slpi: %u: OUR ITEM DOES NOT READ BACK AS WRITTEN\n", step);
		return false;
	}
	show_words("our item reads back", &ours);
	return true;
}


#include <kern/sched_prim.h>
#include "slpi_smem_ro.h"
#include "slpi_proxy_plan.h"
#include "slpi_boot_once.h"
#include "slpi_fw_wait.h"
static bool boot_cancelled;
static struct slpi_boot_state boot_once;
static vm_offset_t metadata_page;
static struct slpi_proxy_command proxy_cmd[3];
static int ro_reader(void *unused,uint32_t off,void *dst,uint32_t n){return smem_peek(off,dst,n);}
static int boot_empty(void *unused){
 struct slpi_report r;const char *err=slpi_observe(ro_reader,NULL,&r);
 if(err||r.remote_partition!=0x17f000||r.remote_bytes!=0x30000||r.found){tell("slpi: preflight REFUSED err=%s partition=%x size=%x items=%u\n",err?err:"none",r.remote_partition,r.remote_bytes,r.found);return 0;}
 return 1;
}
static int boot_prepare(void *unused,unsigned base,unsigned bytes){
 if(base!=SLPI_MEMORY||bytes!=SLPI_MEMORY_SIZE||!step_firmware()||image.size!=SLPI_MEMORY_SIZE)return 0;
 if(image.header_size>PAGE_SIZE||kmem_alloc(kernel_map,&metadata_page,PAGE_SIZE,KMA_KOBJECT|KMA_ZERO|KMA_DATA,VM_KERN_MEMORY_IOKIT)!=KERN_SUCCESS){image_forget();return 0;}
 memcpy((void *)metadata_page,image.header,image.header_size);
 for(vm_offset_t line=metadata_page;line<metadata_page+PAGE_SIZE;line+=64)__asm__ volatile("dc civac, %0"::"r"(line):"memory");
 settle();return 1;
}
static int boot_proxy_acquire(void *unused){
 const uint8_t *db=(const uint8_t *)ml_io_map_unmappable(CMD_DB_PHYS,CMD_DB_SIZE,VM_WIMG_WCOMB);
 if(!db)return 0;int valid=slpi_proxy_plan(db,CMD_DB_SIZE,proxy_cmd);ml_io_unmap((vm_offset_t)db,CMD_DB_SIZE);
 if(!valid){tell("slpi: commandDB refused: require LCX/LMX MAX=384 and distinct aligned XO/rail resources\n");return 0;}
 /* Recheck immediately before first register writes, after firmware I/O. */
 if(!boot_empty(NULL))return 0;
 need(&rsc);need(&ipcc);need(&aoss);need(&smem_lock);
 if(!rsc.base||!ipcc.base||!aoss.base||!smem_lock.base)return 0;
 if(!hk_rpmh_claim(true))return 0;
 if(rd(rsc.base+RSC_TCS(RPMH_TCS)+TCS_STATUS)==0){hk_rpmh_release();return 0;}
 for(unsigned i=0;i<3;i++)tell("slpi: DIAGNOSTIC RETAINED vote address=%x value=%u (cold reboot releases)\n",proxy_cmd[i].address,proxy_cmd[i].value);
 wr(rsc.base+RSC_IRQ_CLEAR,1u<<RPMH_TCS);
 for(unsigned i=0;i<3;i++){
  wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_COMMAND(i),MSGID_WRITE_WAITED);
  wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_COMMAND(i)+4,proxy_cmd[i].address);
  wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_COMMAND(i)+8,proxy_cmd[i].value);
 }
 wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_WAIT_FOR_DONE,7);
 wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_ENABLED,7);settle();
 uint32_t control=rd(rsc.base+RSC_TCS(RPMH_TCS)+TCS_CONTROL)&~TCS_TRIGGER;
 wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_CONTROL,control);
 wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_CONTROL,control|TCS_ACTIVE_MODE);
 wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_CONTROL,control|TCS_ACTIVE_MODE|TCS_TRIGGER);
 uint64_t began=now_us();int done=0;
 while(now_us()-began<100000){if(rd(rsc.base+RSC_IRQ_STATUS)&(1u<<RPMH_TCS)){done=1;break;}sleep_us(50);}
 if(done){wr(rsc.base+RSC_IRQ_CLEAR,1u<<RPMH_TCS);wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_CONTROL,control&~TCS_ACTIVE_MODE);wr(rsc.base+RSC_TCS(RPMH_TCS)+TCS_ENABLED,0);}
 else tell("slpi: RPMh uncertain; leave hardware request intact, no retry, cold reboot REQUIRED\n");
 hk_rpmh_release();return done?1:-1;
}
static int boot_aop(void *unused,int on){return mailbox_send(on?"{class: image, res: load_state, name: slpi, val: on}":"{class: image, res: load_state, name: slpi, val: off}");}
static int boot_scm_result(int32_t result,uint64_t answer){
 if(result==SLPI_SCM_UNCERTAIN){boot_once.scm_uncertain=1;boot_once.preserve_metadata=1;boot_once.poisoned=1;return -1;}
 return result==0&&answer==0;
}
static int boot_pas_header(void *unused,unsigned pas){uint64_t answer=~0ull;if(pas!=12||!metadata_page)return 0;int32_t result=scm("SLPI header",SCM_PAS_INIT_IMAGE,SCM_VALUE_AND_BUFFER,pas,kvtophys(metadata_page),0,&answer);return boot_scm_result(result,answer);}
static int boot_pas_memory(void *unused,unsigned pas,unsigned base,unsigned bytes){uint64_t answer=~0ull;if(pas!=12||base!=SLPI_MEMORY||bytes!=SLPI_MEMORY_SIZE)return 0;int32_t result=scm("SLPI memory",SCM_PAS_MEM_SETUP,SCM_THREE_VALUES,pas,base,bytes,&answer);return boot_scm_result(result,answer);}
static int boot_copy(void *unused){return step_image();}
static int boot_smp2p(void *unused){return step_our_item();}
static int boot_auth(void *unused,unsigned pas){
 uint64_t answer=~0ull;if(pas!=12)return 0;
 wr(ipcc.base+IPCC_ENABLE,IPCC_SLPI<<16|IPCC_SMP2P);
 int32_t result=scm("SLPI auth/reset ONCE",SCM_PAS_AUTH_AND_RESET,SCM_ONE_VALUE,pas,0,0,&answer);return boot_scm_result(result,answer);
}
static int boot_ready(void *unused,unsigned bound){
 if(bound!=10000)return 0;uint64_t began=now_us();struct smp2p words;
 while(now_us()-began<10000000){
  doorbells();
  if(dsp_words(&words)){dsp_listen(&words);uint32_t state=dsp_state(&words);if(state&DSP_FATAL){tell("slpi: FATAL state=%x\n",state);return 0;}if(smp2p_agreed&&(state&DSP_READY)){tell("slpi: READY state=%x after%llu us; proxy floors retained\n",state,now_us()-began);return 1;}}
  sleep_us(200);
 }
 tell("slpi: READY timeout; owned DSP shutdown follows, no retry\n");return 0;
}
static int boot_request_stop(void *unused,unsigned bound){return bound==2000&&dsp_ask_to_stop(bound);}
static int boot_shutdown(void *unused,unsigned pas){uint64_t answer=~0ull;if(pas!=12)return 0;int32_t result=scm("owned SLPI shutdown ONCE",SCM_PAS_SHUTDOWN,SCM_ONE_VALUE,pas,0,0,&answer);int ok=boot_scm_result(result,answer);if(ok==1)wr(ipcc.base+IPCC_DISABLE,IPCC_SLPI<<16|IPCC_SMP2P);return ok;}
static int boot_proxy_hold(void *unused){tell("slpi: diagnostic LCX/LMX/XO floors RETAINED; no rail0/XOoff; cold reboot REQUIRED\n");return 0;}
static void boot_forget(void *unused){if(leaving||scm_unfinished){tell("slpi: panic/SCM metadata retained; no allocator calls\n");return;}image_forget();if(metadata_page){kmem_free(kernel_map,metadata_page,PAGE_SIZE);metadata_page=0;}}
static const struct slpi_boot_ops boot_ops={NULL,boot_empty,boot_prepare,boot_proxy_acquire,boot_aop,boot_pas_header,boot_pas_memory,boot_copy,boot_smp2p,boot_auth,boot_ready,boot_request_stop,boot_shutdown,boot_proxy_hold,boot_forget};
static void slpi_link_quiesce(void);
void hk_slpi_boot_stop(void){os_atomic_store(&boot_cancelled,true,release);thread_wakeup((event_t)&boot_cancelled);lck_mtx_lock(&dsp_lock);(void)slpi_boot_stop_once(&boot_once,&boot_ops);lck_mtx_unlock(&dsp_lock);}
void hk_slpi_boot_quiesce(void){os_atomic_store(&boot_cancelled,true,release);leaving=true;boot_once.preserve_metadata=1;slpi_link_quiesce();(void)slpi_boot_stop_once(&boot_once,&boot_ops);}
static int boot_metadata_ready(void *unused){
 if(os_atomic_load(&boot_cancelled,acquire))return -1;
 vnode_t vp=NULLVP;vfs_context_t context=vfs_context_kernel();struct vnode_attr attr;
 if(vnode_lookup("/hackios/fw/slpi.mdt",0,&vp,context)!=0)return 0;
 VATTR_INIT(&attr);VATTR_WANTED(&attr,va_data_size);
 int err=vnode_getattr(vp,&attr,context);vnode_put(vp);
 if(err)return 0;
 if(attr.va_data_size!=8460){tell("slpi: fixed metadata path has wrong size; no mutation\n");return -1;}
 return 1; /* Full SHA/read validation remains mandatory before proxy acquire. */
}
static uint64_t boot_clock_ms(void *unused){return now_us()/1000;}
static void boot_wait_ms(void *unused,unsigned ms){
 assert_wait_timeout((event_t)&boot_cancelled,THREAD_UNINT,ms,NSEC_PER_MSEC);
 thread_block(THREAD_CONTINUE_NULL);
}
#include "slpi_glink_probe.inc"
static void slpi_boot_thread(void *parameter,wait_result_t result){
 if(!slpi_wait_firmware(NULL,boot_metadata_ready,boot_clock_ms,boot_wait_ms)){tell("slpi: /hackios/fw/slpi.mdt unavailable/cancelled within60s; NO hardware mutation\n");thread_terminate(current_thread());return;}
 struct slpi_boot_evidence e={1,1,1,1,1};
 lck_mtx_lock(&dsp_lock);
 if(os_atomic_load(&boot_cancelled,acquire)){lck_mtx_unlock(&dsp_lock);thread_terminate(current_thread());return;}
 int ready=slpi_boot_start_once(&boot_once,&boot_ops,&e);
 tell("slpi: boot-only result READY=%d phase=%s owned=%u poisoned=%u\n",ready,boot_once.last?boot_once.last:"none",boot_once.owned_running,boot_once.poisoned);
 if(ready&&link_probe_enabled)(void)slpi_glink_probe(link_probe_enabled-1);
 /* Boot-only proof never leaves a running DSP for userspace in this candidate. */
 (void)slpi_boot_stop_once(&boot_once,&boot_ops);
 tell("slpi: ended; active=%u floors-held=%u poisoned=%u; COLD REBOOT before next DSP attempt\n",boot_once.owned_running,boot_once.proxy_held,boot_once.poisoned);
 lck_mtx_unlock(&dsp_lock);thread_terminate(current_thread());
}
void hk_slpi_boot_init(void){
 static bool dispatched;uint32_t enable=0,cold=0,hold_votes=0,legacy=0;thread_t thread;
 PE_parse_boot_argn("slpi_boot_once",&enable,sizeof(enable));if(enable!=1||dispatched)return;dispatched=true;
 PE_parse_boot_argn("slpi_glink_probe",&link_probe_enabled,sizeof(link_probe_enabled));if(link_probe_enabled>3)return;
 PE_parse_boot_argn("slpi_cold_provenance",&cold,sizeof(cold));PE_parse_boot_argn("slpi_hold_proxy",&hold_votes,sizeof(hold_votes));
 if(!instantnoodle_board()||pe_arm_get_soc_base_phys()!=HK_SOC_BASE||cold!=1||hold_votes!=1){tell("slpi: refused missing board/cold-provenance/held-proxy diagnostic contract\n");return;}
 PE_parse_boot_argn("adsp",&legacy,sizeof(legacy));if(legacy)return;PE_parse_boot_argn("adsp_step",&legacy,sizeof(legacy));if(legacy)return;
 new_from=~0u;pause_ms=0;step=7;
 if(kernel_thread_start(slpi_boot_thread,NULL,&thread)!=KERN_SUCCESS){tell("slpi: no thread; no mutation\n");return;}thread_deallocate(thread);
}
