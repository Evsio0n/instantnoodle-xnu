/*
 * The OnePlus 8's modem: not in the SoC but an external SDX55 on PCIe 2 (the live device tree's mdm0: qcom,ext-sdx55m, link
 * "0306_02.01.00"), flashless: the AP gives it its firmware. The plan, its sources and its risks: docs/research/next-features.md, section 3.
 *
 * Nothing here is done unless boot-arg modem=N asks for it, and for now that is one step:
 *
 *   modem=1    a look, reading only (Boot C of that page): is the SDX55 powered at boot, and can the AP reset it at all?
 *
 *              TLMM, the SOUTH tile for all of them (Linux pinctrl-sm8250.c, whose table agrees with all 19 pins this kernel already drives on this
 *              phone: a pin read in the wrong tile is a NoC error, hardware-qualcomm.md 1), the pins Android's esoc driver and PCIe 2 use, read as
 *              the gauge's gpio48 in that tile is read: MDM2AP errfatal gpio1 and status gpio3, AP2MDM status gpio56 and errfatal
 *              gpio57 (the tree's qcom,mdm2ap-*-gpio, qcom,ap2mdm-*-gpio), PERST gpio85, CLKREQ gpio86, WAKE gpio87 (qcom,pcie@1c10000).
 *              GCC: pcie_2_gdsc's register, 0x6004 (Linux gcc-sm8250.c). NOT ONE REGISTER OF PCIe 2 ITSELF: an unclocked block is a NoC
 *              error and a CrashDump (hardware-qualcomm.md 1, PCIe 0).
 *              The PMIC arbiter: which channels are the modem's PMIC (qcom,pmxprairie@8 and @9), whose they are, and through ours the
 *              REVID of slaves 8 and 9 and the PON of slave 8 (qcom,power-on@800, qcom,modem-reset: esoc's warm reset goes through it).
 *              Reads as hk_torch.c's, one at a time with the buttons' driver (hk_spmi_lock()). A read of a chip that does not
 *              answer is the arbiter's failure status, said and not an abort [source: spmi-pmic-arb.c].
 *
 *   modem=2    ... and the first writes (the owner's word, 2026-11-06: 「modem 继续」): the modem reset as Android's esoc does it, and PCIe 2 up to its first
 *              milestone, device 17cb:0306 answering in its primary boot loader:
 *
 *              reset    qpnp_pon_modem_pwr_off(PON_POWER_OFF_WARM_RESET) of OnePlus's qpnp-power-on.c, on the PON of slave 8 through channel 105 (Boot C: the one
 *                       system 0 may write): 0x63 <- 0, 500 us, 0x62 <- 1, 0x63 <- 0x80, 500 us, 0x64 <- 0xa5. Three registers by a list, the first two read
 *                       back. Then 150 ms, and AP2MDM status high (esoc-mdm-pon.c, mdm4x_do_first_power_on), AP2MDM errfatal low, at 16 mA with no pull
 *                       (the tree's ap2mdm_active); MDM2AP's two inputs with no pull.
 *              PCIe 2   as instantnoodle_wifi.c's PCIe 0, the same block at another address, from the tree's qcom,pcie@1c10000 and gcc-sm8250.c: PHY 1.2 V and
 *                       0.88 V (L9, L5, the UFS's too, never switched off here), GDSC 0x6004, branches by the vote register 0x52010 bits 10-15, the
 *                       reference through 0x6f034 and the modem's buffer 0x8c00c, PERST gpio85, CLKREQ gpio86 at its function, WAKE gpio87. The PHY is two lanes:
 *                       status 0xa14, power-down 0xa40. Target 5 GT/s, as the Wi-Fi's: Android's asks for nothing, and Gen 3 wants more of CX (qcom,bw-scale).
 *                       A register of PCIe 2 is touched only after the domain and the clocks are on.
 *
 *   modem=3    ... and its firmware, to its mission: modem_boot.inc says how.
 *
 * The modem's firmware is not touched by modem=1 and modem=2. At the run's end, and on a panic's way out, PERST goes low (hk_modem_quiesce()).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/instantnoodle.h>
#include <sys/hk_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well (iOS's start pauses the kernel's printf) */
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

/* ---- the PMIC arbiter (version 5, as hk_torch.c) -------------------------------------------------------- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_SYSTEM              0
#define ARB_VERSION             0x000
#define ARB_FEATURES            0x004
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512
#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define OBSERVER_DATA0          0x18
#define OBSERVER_DATA1          0x1c
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000

static vm_offset_t arb_core, arb_cnfg, arb_observer;
static uint32_t arb_channels;

/* the channel of slave << 8 | peripheral, or -1 */
static int
channel_of(uint32_t id)
{
	for (uint32_t n = 0; n < arb_channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n));

		if (map != 0 && ((map >> 8) & 0xfff) == id) {
			return (int)n;
		}
	}
	return -1;
}

/* `count' (1 to 8) registers from `reg' on; NULL, or what went wrong */
static const char *
spmi_read(int channel, uint8_t reg, uint8_t *bytes, uint32_t count)
{
	vm_offset_t ch = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * (uint32_t)channel;
	uint32_t data[2] = { 0, 0 }, status = 0;
	uint64_t start, interval;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hk_spmi_lock();
	wr(ch + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
	start = mach_absolute_time();
	do {
		status = rd(ch + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	if (status & STATUS_DONE) {
		data[0] = rd(ch + OBSERVER_DATA0);
		data[1] = count > 4 ? rd(ch + OBSERVER_DATA1) : 0;
	}
	hk_spmi_unlock();
	if (!(status & STATUS_DONE)) {
		return "no answer from the arbiter";
	}
	if (status & (STATUS_DENIED | STATUS_FAILURE | STATUS_DROPPED)) {
		return status & STATUS_DENIED ? "the arbiter refuses" : status & STATUS_FAILURE ? "the chip did not answer" : "dropped";
	}
	for (uint32_t i = 0; i < count; i++) {
		bytes[i] = (uint8_t)(data[i / 4] >> (8 * (i % 4)));
	}
	return NULL;
}

static void
show_peripheral(const char *name, uint32_t id, unsigned lines)
{
	int channel = channel_of(id);

	if (channel < 0) {
		say("modem: %s (%#x): no channel of the arbiter is it\n", name, id);
		return;
	}
	say("modem: %s (%#x): channel %d, written by system %u%s\n", name, id, channel, rd(arb_cnfg + ARB_OWNER(channel)) & 7,
	    (rd(arb_cnfg + ARB_OWNER(channel)) & 7) == ARB_SYSTEM ? ", which is us" : "");
	for (unsigned base = 0; base < lines * 0x20; base += 0x20) {
		char line[200];
		int n = snprintf(line, sizeof(line), "modem: %#05x:", id << 8 | base);

		for (unsigned i = 0; i < 0x20; i += 8) {
			uint8_t r[8];
			const char *problem = spmi_read(channel, (uint8_t)(base + i), r, 8);

			if (problem != NULL) {
				say("modem: reading %#x of it: %s\n", base + i, problem);
				return;
			}
			for (unsigned j = 0; j < 8; j++) {
				n += snprintf(line + n, sizeof(line) - (size_t)n, " %02x", r[j]);
			}
		}
		say("%s\n", line);
	}
}

/* ---- modem=1: a look --------------------------------------------------------------------------------------- */

#define GCC_PCIE_2_GDSCR_PHYS   (0x00100000UL + 0x6000UL)       /* a page of GCC; the register at +4: pcie_2_gdsc, 0x6004 */

static void
look(void)
{
	static const struct {
		unsigned pin;
		const char *name;
	} pins[] = {
		{ 1, "MDM2AP errfatal" }, { 3, "MDM2AP status" }, { 56, "AP2MDM status" }, { 57, "AP2MDM errfatal" },
		{ 85, "PCIe 2 PERST" }, { 86, "PCIe 2 CLKREQ" }, { 87, "PCIe 2 WAKE" },
	};
	uint32_t version, gdsc, slave8 = 0, slave9 = 0, slave8_ours = 0;

	say("modem: modem=1: a look, reading only: pins, pcie_2_gdsc, the arbiter's channels of the modem's PMIC\n");
	for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		struct instantnoodle_pin p = { INSTANTNOODLE_TLMM_SOUTH, pins[i].pin };

		say("modem: gpio%u (%s): cfg %#x, in/out %#x\n", pins[i].pin, pins[i].name, instantnoodle_gpio_cfg(p), instantnoodle_gpio_io(p));
	}
	gdsc = rd(ml_io_map(GCC_PCIE_2_GDSCR_PHYS, 0x1000) + 4);
	say("modem: pcie_2_gdsc %#x: %s, %s\n", gdsc, (gdsc & (1u << 31)) ? "on" : "off", (gdsc & 1) ? "asked to collapse" : "not asked to collapse");
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	version = rd(arb_core + ARB_VERSION);
	arb_channels = rd(arb_core + ARB_FEATURES) & 0x7ff;
	if (version < 0x50000000u || version >= 0x70000000u) {
		say("modem: PMIC arbiter version %#x, not 5: no more\n", version);
		return;
	}
	if (arb_channels == 0 || arb_channels > ARB_MAX_CHANNELS) {
		arb_channels = ARB_MAX_CHANNELS;
	}
	for (uint32_t n = 0; n < arb_channels; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), id = (map >> 8) & 0xfff, owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;

		if (map == 0 || (id >> 8) < 8 || (id >> 8) > 9) {
			continue;
		}
		slave8 += (id >> 8) == 8;
		slave9 += (id >> 8) == 9;
		slave8_ours += (id >> 8) == 8 && owner == ARB_SYSTEM;
		say("modem: channel %u is %#x, written by system %u\n", n, id, owner);
	}
	say("modem: %u channels of slave 8 (%u written by us), %u of slave 9, of %u\n", slave8, slave8_ours, slave9, arb_channels);
	show_peripheral("slave 8's REVID", 0x801, 1);
	show_peripheral("slave 9's REVID", 0x901, 1);
	show_peripheral("slave 8's PON", 0x808, 8);
	say("modem: the look is done: nothing written\n");
}

/* ---- modem=2: the reset ------------------------------------------------------------------------------------ */

#define ARB_CHNLS_PHYS          0x0c600000UL    /* writes: 0x10000 per channel */
#define CHANNEL_WDATA0          0x10
#define COMMAND_WRITE           (0u << 27)
#define PON_ID                  0x808           /* slave 8, peripheral 8 */
#define PON_SW_RST_S2_CTL       0x62
#define PON_SW_RST_S2_CTL2      0x63
#define PON_SW_RST_GO           0x64
#define PON_RESET_TYPE_WARM     0x01            /* PON_POWER_OFF_WARM_RESET */
#define PON_RESET_EN            0x80
#define PON_SW_RST_GO_VAL       0xa5

static int pon_ours = -1;                       /* the channel system 0 may write */
static vm_offset_t pon_write_channel;

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

/* One of the three registers. False, and nothing more is written, if it is not taken. */
static bool
pon_put(uint8_t reg, uint8_t value, bool read_back)
{
	uint32_t status = 0;
	uint64_t start, interval;
	uint8_t back = 0xff;
	const char *problem;

	if ((reg != PON_SW_RST_S2_CTL && reg != PON_SW_RST_S2_CTL2 && reg != PON_SW_RST_GO) || pon_ours < 0 ||
	    ((rd(arb_core + ARB_CHANNEL(pon_ours)) >> 8) & 0xfff) != PON_ID || (rd(arb_cnfg + ARB_OWNER(pon_ours)) & 7) != ARB_SYSTEM) {
		say("modem: pon: %#x <- %#x may not be written\n", reg, value);
		return false;
	}
	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	hk_spmi_lock();
	wr(pon_write_channel + CHANNEL_WDATA0, value);
	wr(pon_write_channel + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
	start = mach_absolute_time();
	do {
		status = rd(pon_write_channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	hk_spmi_unlock();
	if (!(status & STATUS_DONE) || (status & (STATUS_DENIED | STATUS_FAILURE | STATUS_DROPPED))) {
		say("modem: pon: %#x <- %#x: status %#x\n", reg, value, status);
		return false;
	}
	if (read_back) {
		problem = spmi_read(pon_ours, reg, &back, 1);
		if (problem != NULL || back != value) {
			say("modem: pon: %#x <- %#x, but it reads %#x (%s)\n", reg, value, back, problem != NULL ? problem : "no error");
			return false;
		}
	}
	return true;
}

#define MODEM_PIN(n)            ((struct instantnoodle_pin){ INSTANTNOODLE_TLMM_SOUTH, (n) })
#define PIN_MDM2AP_ERRFATAL     MODEM_PIN(1)
#define PIN_MDM2AP_STATUS       MODEM_PIN(3)
#define PIN_AP2MDM_STATUS       MODEM_PIN(56)
#define PIN_AP2MDM_ERRFATAL     MODEM_PIN(57)
#define PIN_PERST               MODEM_PIN(85)
#define PIN_CLKREQ              MODEM_PIN(86)
#define PIN_WAKE                MODEM_PIN(87)
#define FUNCTION_PCI_E2         1

static void
pins_say(const char *when)
{
	say("modem: %s: MDM2AP status %u, errfatal %u; AP2MDM status %u; PERST %u, CLKREQ %u, WAKE %u\n", when, instantnoodle_gpio_level(PIN_MDM2AP_STATUS),
	    instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL), instantnoodle_gpio_level(PIN_AP2MDM_STATUS), instantnoodle_gpio_level(PIN_PERST),
	    instantnoodle_gpio_level(PIN_CLKREQ), instantnoodle_gpio_level(PIN_WAKE));
}

static bool
modem_reset(void)
{
	for (uint32_t n = 0; n < arb_channels; n++) {
		if (((rd(arb_core + ARB_CHANNEL(n)) >> 8) & 0xfff) == PON_ID && (rd(arb_cnfg + ARB_OWNER(n)) & 7) == ARB_SYSTEM) {
			pon_ours = (int)n;
		}
	}
	if (pon_ours < 0) {
		say("modem: no channel of slave 8's PON is ours: no reset\n");
		return false;
	}
	pon_write_channel = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * (uint32_t)pon_ours, 0x1000);
	instantnoodle_gpio_input(PIN_MDM2AP_STATUS, INSTANTNOODLE_PULL_NONE);
	instantnoodle_gpio_input(PIN_MDM2AP_ERRFATAL, INSTANTNOODLE_PULL_NONE);
	instantnoodle_gpio_output(PIN_AP2MDM_ERRFATAL, false, INSTANTNOODLE_PULL_NONE, 16);
	instantnoodle_gpio_output(PIN_AP2MDM_STATUS, false, INSTANTNOODLE_PULL_NONE, 16);
	pins_say("before the reset");
	say("modem: the warm reset, through channel %d\n", pon_ours);
	if (!pon_put(PON_SW_RST_S2_CTL2, 0, true)) {
		return false;
	}
	spin_us(500);
	if (!pon_put(PON_SW_RST_S2_CTL, PON_RESET_TYPE_WARM, true) || !pon_put(PON_SW_RST_S2_CTL2, PON_RESET_EN, true)) {
		return false;
	}
	spin_us(500);
	if (!pon_put(PON_SW_RST_GO, PON_SW_RST_GO_VAL, false)) {
		return false;
	}
	delay_for_interval(150, NSEC_PER_MSEC);         /* (esoc: for the PON sequence to complete) */
	instantnoodle_gpio_output(PIN_AP2MDM_STATUS, true, INSTANTNOODLE_PULL_NONE, 16);
	pins_say("AP2MDM status high");
	return true;
}

/* ---- modem=2: PCIe 2, as instantnoodle_wifi.c's PCIe 0 ----------------------------------------------------- */

#define GCC_PHYS                0x00100000ul
#define GCC_VOTE                0x52010         /* bit 10 slv_q2a, 11 slv_axi, 12 mstr_axi, 13 cfg_ahb, 14 aux, 15 pipe */
#define GCC_PCIE_2_BCR          0x06000
#define GCC_PCIE_2_GDSCR        0x06004
#define GCC_PCIE_2_SLV_Q2A_CBCR 0x06010
#define GCC_PCIE_2_SLV_AXI_CBCR 0x06014
#define GCC_PCIE_2_MSTR_AXI_CBCR 0x0601c
#define GCC_PCIE_2_CFG_AHB_CBCR 0x06024
#define GCC_PCIE_2_AUX_CBCR     0x06028
#define GCC_PCIE_2_AUX_RCG      0x06038
#define GCC_PCIE_2_PHY_BCR      0x1f01c
#define GCC_PCIE_REFGEN_RCG     0x6f014
#define GCC_PCIE2_REFGEN_CBCR   0x6f034
#define GCC_PCIE_MDM_CLKREF     0x8c00c
#define GCC_DDRSS_PCIE_SF_TBU   0x8d058
#define GCC_AGGRE_NOC_PCIE_TBU  0x9000c
#define CBCR_OFF                (1u << 31)
#define GDSC_COLLAPSE           (1u << 0)
#define GDSC_ON                 (1u << 31)
#define RCG_UPDATE              (1u << 0)

#define PARF_PHYS               0x01c10000ul
#define PHY_PHYS                0x01c16000ul
#define DBI_PHYS                0x64000000ul
#define IATU_PHYS               0x64001000ul
#define CONF_PHYS               0x64100000ul
#define BAR_PHYS                0x64300000ul
#define BAR_WINDOW              0x03d00000ul
#define PARF_SYS_CTRL           0x000
#define PARF_PM_CTRL            0x020
#define PARF_PHY_CTRL           0x040
#define PARF_MHI_CLOCK_RESET_CTRL 0x174
#define PARF_AXI_MSTR_WR_ADDR_HALT 0x1a8
#define PARF_LTSSM              0x1b0
#define PARF_INT_ALL_MASK       0x22c
#define PARF_DBI_BASE_ADDR      0x350
#define PARF_SLV_ADDR_SPACE_SIZE 0x358
#define PARF_DEVICE_TYPE        0x1000
#define ELBI_SYS_STTS           0xf28
#define XMLH_LINK_UP            0x400
#define DBI_LINK_STATUS         0x080
#define DBI_DEVICE_CONTROL      0x078
#define DBI_LINK_CONTROL2       0x0a0
#define DBI_SPCIE_CAP           0x154
#define DBI_ACK_F_ASPM_CTRL     0x70c
#define DBI_GEN2_CTRL           0x80c
#define DBI_GEN3_RELATED        0x890
#define DBI_MISC_CONTROL        0x8bc
#define DBI_AUX_CLK_FREQ        0xb40
#define IATU_CTRL1              0x00
#define IATU_CTRL2              0x04
#define IATU_LBAR               0x08
#define IATU_UBAR               0x0c
#define IATU_LAR                0x10
#define IATU_LTAR               0x14
#define IATU_UTAR               0x18
#define PHY_STATUS              0xa14           /* bit 6 set: not ready (dt qcom,phy-status-offset, -bit) */
#define PHY_POWER_DOWN          0xa40
#define EP_BDF                  0x0100

/* The PHY's sequence (dt: qcom,phy-sequence of qcom,pcie@1c10000): offset, value; none of its delays is set. */
static const uint16_t phy_sequence[][2] = {
	{ 0xa40, 0x03 }, { 0x010, 0x01 }, { 0x01c, 0x31 }, { 0x020, 0x01 }, { 0x024, 0xde }, { 0x028, 0x07 }, { 0x030, 0x4c }, { 0x034, 0x06 },
	{ 0x048, 0x90 }, { 0x058, 0x0f }, { 0x074, 0x06 }, { 0x078, 0x06 }, { 0x07c, 0x16 }, { 0x080, 0x16 }, { 0x084, 0x36 }, { 0x088, 0x36 },
	{ 0x094, 0x08 }, { 0x0a4, 0x42 }, { 0x0ac, 0x0a }, { 0x0b0, 0x1a }, { 0x0b4, 0x14 }, { 0x0b8, 0x34 }, { 0x0bc, 0x82 }, { 0x0c4, 0x68 },
	{ 0x0cc, 0x55 }, { 0x0d0, 0x55 }, { 0x0d4, 0x03 }, { 0x0d8, 0xab }, { 0x0dc, 0xaa }, { 0x0e0, 0x02 }, { 0x10c, 0x02 }, { 0x110, 0x24 },
	{ 0x118, 0xb4 }, { 0x11c, 0x03 }, { 0x154, 0x34 }, { 0x158, 0x01 }, { 0x16c, 0x08 }, { 0x1ac, 0xca }, { 0x1b0, 0x1e }, { 0x1b4, 0xa2 },
	{ 0x1b8, 0x18 }, { 0x1bc, 0x11 }, { 0x23c, 0x11 }, { 0x284, 0x75 }, { 0x29c, 0x12 }, { 0x304, 0x02 }, { 0x408, 0x0c }, { 0x414, 0x03 },
	{ 0x434, 0x7f }, { 0x444, 0x70 }, { 0x460, 0x30 }, { 0x4d4, 0x04 }, { 0x4d8, 0x07 }, { 0x4dc, 0x1b }, { 0x4e8, 0x04 }, { 0x4ec, 0x0e },
	{ 0x4f0, 0x4a }, { 0x4f4, 0x0f }, { 0x4f8, 0xc0 }, { 0x4fc, 0x00 }, { 0x510, 0x17 }, { 0x518, 0x1c }, { 0x51c, 0x03 }, { 0x524, 0x1e },
	{ 0x570, 0xbf }, { 0x574, 0x3f }, { 0x578, 0xff }, { 0x57c, 0x7f }, { 0x580, 0x15 }, { 0x584, 0x24 }, { 0x588, 0xe4 }, { 0x58c, 0xec },
	{ 0x590, 0x3b }, { 0x594, 0x36 }, { 0x598, 0xd4 }, { 0x59c, 0x54 }, { 0x5a0, 0xdb }, { 0x5a4, 0x3b }, { 0x5a8, 0x31 }, { 0x5bc, 0x0c },
	{ 0x5b8, 0x38 }, { 0x63c, 0x11 }, { 0x684, 0x75 }, { 0x69c, 0x12 }, { 0x704, 0x20 }, { 0x808, 0x0c }, { 0x814, 0x03 }, { 0x834, 0x7f },
	{ 0x844, 0x70 }, { 0x860, 0x30 }, { 0x8d4, 0x04 }, { 0x8d8, 0x07 }, { 0x8dc, 0x1b }, { 0x8e8, 0x04 }, { 0x8ec, 0x0e }, { 0x8f0, 0x4a },
	{ 0x8f4, 0x0f }, { 0x8f8, 0xc0 }, { 0x8fc, 0x00 }, { 0x910, 0x17 }, { 0x918, 0x1c }, { 0x91c, 0x03 }, { 0x924, 0x1e }, { 0x970, 0xbf },
	{ 0x974, 0x3f }, { 0x978, 0xff }, { 0x97c, 0x7f }, { 0x980, 0x15 }, { 0x984, 0x24 }, { 0x988, 0xe4 }, { 0x98c, 0xec }, { 0x990, 0x3b },
	{ 0x994, 0x36 }, { 0x998, 0xd4 }, { 0x99c, 0x54 }, { 0x9a0, 0xdb }, { 0x9a4, 0x3b }, { 0x9a8, 0x31 }, { 0x9bc, 0x0c }, { 0x9b8, 0x38 },
	{ 0xadc, 0x05 }, { 0xb88, 0x77 }, { 0xb98, 0x0b }, { 0xba4, 0x01 }, { 0xbe0, 0x0f }, { 0xe0c, 0x0d }, { 0xe14, 0x07 }, { 0xe1c, 0xc1 },
	{ 0xe40, 0x01 }, { 0xe48, 0x01 }, { 0xe90, 0x00 }, { 0xeb4, 0x33 }, { 0xebc, 0x00 }, { 0xee0, 0x58 }, { 0xa00, 0x00 }, { 0xa44, 0x03 },
};

static struct {
	vm_offset_t gcc06, gcc1f, gcc52, gcc6f, gcc8c, gcc8d, gcc90;
	vm_offset_t parf, phy, dbi, iatu, conf, bar;
	bool clocked;
	volatile bool up, quiesced;
	uint64_t bar_size;
} pcie;

static inline void
wr_sync(vm_offset_t address, uint32_t value)
{
	wr(address, value);
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
set_bits(vm_offset_t address, uint32_t clear, uint32_t set)
{
	wr_sync(address, (rd(address) & ~clear) | set);
	(void)rd(address);
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static vm_offset_t
gcc(uint32_t offset)
{
	vm_offset_t page = (offset & ~0xfffu) == 0x06000 ? pcie.gcc06 : (offset & ~0xfffu) == 0x1f000 ? pcie.gcc1f : (offset & ~0xfffu) == 0x52000 ? pcie.gcc52 :
	    (offset & ~0xfffu) == 0x6f000 ? pcie.gcc6f : (offset & ~0xfffu) == 0x8c000 ? pcie.gcc8c : (offset & ~0xfffu) == 0x8d000 ? pcie.gcc8d : pcie.gcc90;

	return page + (offset & 0xfff);
}

static bool
branch_on(const char *name, vm_offset_t enable, uint32_t bit, uint32_t cbcr)
{
	set_bits(enable, 0, bit);
	for (unsigned us = 0; cbcr != 0 && (rd(gcc(cbcr)) & CBCR_OFF); us += 10) {
		if (us > 2000) {
			say("modem: pcie: the clock %s does not start (cbcr %#x)\n", name, rd(gcc(cbcr)));
			return false;
		}
		sleep_us(10);
	}
	return true;
}

static bool
rcg_set(uint32_t rcg, uint32_t source, uint32_t half_divider)
{
	uint32_t cfg = rd(gcc(rcg + 4));

	if ((cfg & 0x71f) == (source << 8 | half_divider)) {
		return true;
	}
	wr_sync(gcc(rcg + 4), (cfg & ~0x371fu) | source << 8 | half_divider);
	set_bits(gcc(rcg), 0, RCG_UPDATE);
	for (unsigned us = 0; rd(gcc(rcg)) & RCG_UPDATE; us += 10) {
		if (us > 2000) {
			say("modem: pcie: the clock source at %#x does not take its configuration\n", rcg);
			return false;
		}
		sleep_us(10);
	}
	return true;
}

static void
reset_pulse(uint32_t bcr)
{
	wr_sync(gcc(bcr), 1);
	sleep_us(1000);
	wr_sync(gcc(bcr), 0);
	sleep_us(1000);
}

static bool
clocks_on(void)
{
	pcie.gcc06 = ml_io_map(GCC_PHYS + 0x06000, 0x1000);
	pcie.gcc1f = ml_io_map(GCC_PHYS + 0x1f000, 0x1000);
	pcie.gcc52 = ml_io_map(GCC_PHYS + 0x52000, 0x1000);
	pcie.gcc6f = ml_io_map(GCC_PHYS + 0x6f000, 0x1000);
	pcie.gcc8c = ml_io_map(GCC_PHYS + 0x8c000, 0x1000);
	pcie.gcc8d = ml_io_map(GCC_PHYS + 0x8d000, 0x1000);
	pcie.gcc90 = ml_io_map(GCC_PHYS + 0x90000, 0x1000);
	say("modem: pcie: PCIe 2's domain %#x, votes %#x, aux %#x, cfg %#x: power and clocks on\n", rd(gcc(GCC_PCIE_2_GDSCR)), rd(gcc(GCC_VOTE)),
	    rd(gcc(GCC_PCIE_2_AUX_CBCR)), rd(gcc(GCC_PCIE_2_CFG_AHB_CBCR)));
	if (!instantnoodle_supply("ldoa9", 1200, true) || !instantnoodle_supply("ldoa5", 880, true) || !instantnoodle_bcm_vote("SN12", 1, 1)) {
		say("modem: pcie: a supply or the bus vote refused\n");
		return false;
	}
	set_bits(gcc(GCC_PCIE_2_GDSCR), GDSC_COLLAPSE, 0);
	for (unsigned us = 0; !(rd(gcc(GCC_PCIE_2_GDSCR)) & GDSC_ON); us += 10) {
		if (us > 5000) {
			say("modem: pcie: PCIe 2's power domain does not come on (%#x)\n", rd(gcc(GCC_PCIE_2_GDSCR)));
			return false;
		}
		sleep_us(10);
	}
	/* aux 19.2 MHz from the crystal (dt qcom,use-19p2mhz-aux-clk); the PHY's reference 100 MHz from GPLL0, shared with PCIe 0 (600 / 6) */
	if (!rcg_set(GCC_PCIE_2_AUX_RCG, 0, 1) || !rcg_set(GCC_PCIE_REFGEN_RCG, 1, 11)) {
		return false;
	}
	if (!branch_on("aggre noc pcie tbu", gcc(GCC_AGGRE_NOC_PCIE_TBU), 1, GCC_AGGRE_NOC_PCIE_TBU) ||
	    !branch_on("ddrss pcie sf tbu", gcc(GCC_DDRSS_PCIE_SF_TBU), 1, GCC_DDRSS_PCIE_SF_TBU) ||
	    !branch_on("pcie mdm clkref", gcc(GCC_PCIE_MDM_CLKREF), 1, GCC_PCIE_MDM_CLKREF) ||
	    !branch_on("pcie2 phy refgen", gcc(GCC_PCIE2_REFGEN_CBCR), 1, GCC_PCIE2_REFGEN_CBCR) ||
	    !branch_on("pcie_2_aux", gcc(GCC_VOTE), 1u << 14, GCC_PCIE_2_AUX_CBCR) ||
	    !branch_on("pcie_2_cfg_ahb", gcc(GCC_VOTE), 1u << 13, GCC_PCIE_2_CFG_AHB_CBCR) ||
	    !branch_on("pcie_2_mstr_axi", gcc(GCC_VOTE), 1u << 12, GCC_PCIE_2_MSTR_AXI_CBCR) ||
	    !branch_on("pcie_2_slv_axi", gcc(GCC_VOTE), 1u << 11, GCC_PCIE_2_SLV_AXI_CBCR) ||
	    !branch_on("pcie_2_slv_q2a_axi", gcc(GCC_VOTE), 1u << 10, GCC_PCIE_2_SLV_Q2A_CBCR)) {
		return false;
	}
	reset_pulse(GCC_PCIE_2_BCR);
	reset_pulse(GCC_PCIE_2_PHY_BCR);
	pcie.clocked = true;            /* only now may PCIe 2's own registers be touched */
	return true;
}

static bool
link_train(void)
{
	uint32_t status = 0, link = 0;

	set_bits(pcie.dbi + DBI_GEN2_CTRL, 0x1f00, 1u << 8);
	set_bits(pcie.dbi + DBI_GEN3_RELATED, 1, 0);
	set_bits(pcie.dbi + DBI_MISC_CONTROL, 0, 1);
	wr_sync(pcie.dbi + DBI_SPCIE_CAP, 0x55555555);
	set_bits(pcie.dbi + DBI_MISC_CONTROL, 1, 0);
	set_bits(pcie.dbi + DBI_LINK_CONTROL2, 0xf, 2);
	set_bits(pcie.dbi + DBI_DEVICE_CONTROL, 0x7000, 5u << 12);
	set_bits(pcie.parf + PARF_LTSSM, 0, 1u << 8);
	/* (Android's 100 ms is for a device already up: this one has been out of reset for 150 ms and its PBL brings the link up) */
	for (unsigned tries = 0; tries <= 200; tries++) {
		sleep_us(5000);
		status = rd(pcie.dbi + ELBI_SYS_STTS);
		link = rd(pcie.dbi + DBI_LINK_STATUS);
		if ((status & XMLH_LINK_UP) && (link & 1u << 29) && rd(pcie.dbi) != 0xffffffffu) {
			say("modem: pcie: THE LINK IS UP after %u ms: %s, x%u (LTSSM %#x)\n", 5 * (tries + 1), (link >> 16 & 0xf) == 1 ?
			    "2.5 GT/s" : (link >> 16 & 0xf) == 2 ? "5 GT/s" : "8 GT/s", link >> 20 & 0x3f, status >> 12 & 0x3f);
			return true;
		}
		if (tries % 40 == 0) {
			say("modem: pcie: %u ms: LTSSM %#x, link status %#x\n", 5 * tries, status >> 12 & 0x3f, link);
		}
	}
	say("modem: pcie: the link does not come up (ELBI status %#x, LTSSM %#x, link status %#x)\n", status, status >> 12 & 0x3f, link);
	return false;
}

static bool
pcie_on(void)
{
	uint32_t ready = 0, ids, bar, size_mask;
	bool wide;

	instantnoodle_gpio_output(PIN_PERST, false, INSTANTNOODLE_PULL_DOWN, 2);
	instantnoodle_gpio_function(PIN_CLKREQ, FUNCTION_PCI_E2, INSTANTNOODLE_PULL_UP, 2);
	instantnoodle_gpio_input(PIN_WAKE, INSTANTNOODLE_PULL_UP);
	sleep_us(1000);
	if (!clocks_on()) {
		return false;
	}
	pcie.parf = ml_io_map(PARF_PHYS, 0x3000);
	pcie.phy = ml_io_map(PHY_PHYS, 0x2000);
	pcie.dbi = ml_io_map(DBI_PHYS, 0x1000);
	pcie.iatu = ml_io_map(IATU_PHYS, 0x1000);
	pcie.conf = ml_io_map(CONF_PHYS, 0x1000);
	say("modem: pcie: PARF %#x, PHY status %#x: the controller set up as a root complex\n", rd(pcie.parf + PARF_SYS_CTRL), rd(pcie.phy + PHY_STATUS));
	wr_sync(pcie.parf + PARF_DEVICE_TYPE, 0x4);
	set_bits(pcie.parf + PARF_PM_CTRL, 1u << 5, 0);
	set_bits(pcie.parf + PARF_PHY_CTRL, 1, 0);
	wr_sync(pcie.parf + PARF_DBI_BASE_ADDR, 0);
	wr_sync(pcie.parf + PARF_SYS_CTRL, 0x365e);
	set_bits(pcie.parf + PARF_MHI_CLOCK_RESET_CTRL, 0, 1u << 4);
	wr_sync(pcie.parf + PARF_INT_ALL_MASK, 0);
	wr_sync(pcie.parf + PARF_SLV_ADDR_SPACE_SIZE, 0x4000000);
	set_bits(pcie.parf + PARF_AXI_MSTR_WR_ADDR_HALT, 0, 1u << 31);
	for (unsigned i = 0; i < sizeof(phy_sequence) / sizeof(phy_sequence[0]); i++) {
		wr_sync(pcie.phy + phy_sequence[i][0], phy_sequence[i][1]);
		(void)rd(pcie.phy + phy_sequence[i][0]);
	}
	sleep_us(1000);
	branch_on("pcie_2_pipe", gcc(GCC_VOTE), 1u << 15, 0);
	for (unsigned tries = 0; tries <= 10 && ((ready = rd(pcie.phy + PHY_STATUS)) & 1u << 6); tries++) {
		sleep_us(1000);
	}
	if (ready & 1u << 6) {
		say("modem: pcie: the PHY is not ready (status %#x)\n", ready);
		return false;
	}
	say("modem: pcie: the PHY is ready\n");
	sleep_us(10000);                /* dt qcom,ep-latency */
	instantnoodle_gpio_output(PIN_PERST, true, INSTANTNOODLE_PULL_DOWN, 2);
	sleep_us(1000);
	if (!link_train()) {
		return false;
	}
	/* the device's configuration space, through iATU region 0: bus 1, device 0 */
	wr_sync(pcie.iatu + IATU_CTRL2, 0);
	wr_sync(pcie.iatu + IATU_CTRL1, 0x4);
	wr_sync(pcie.iatu + IATU_LBAR, (uint32_t)CONF_PHYS);
	wr_sync(pcie.iatu + IATU_UBAR, 0);
	wr_sync(pcie.iatu + IATU_LAR, (uint32_t)CONF_PHYS + 0xfff);
	wr_sync(pcie.iatu + IATU_LTAR, (uint32_t)EP_BDF << 16);
	wr_sync(pcie.iatu + IATU_UTAR, 0);
	wr_sync(pcie.iatu + IATU_CTRL2, 1u << 31);
	set_bits(pcie.dbi + DBI_ACK_F_ASPM_CTRL, 0, 1u << 15);
	wr_sync(pcie.dbi + DBI_AUX_CLK_FREQ, 0x14);
	for (unsigned ms = 0; (ids = rd(pcie.conf)) == 0xffffffffu; ms++) {
		if (ms > 1000) {
			say("modem: pcie: the device's configuration space does not answer\n");
			return false;
		}
		sleep_us(1000);
	}
	wr_sync(pcie.dbi + 0x18, 0x00010100);
	wr_sync(pcie.dbi + 0x1c, 0x000000f0);
	wr_sync(pcie.dbi + 0x20, (uint32_t)((BAR_PHYS + BAR_WINDOW - 1) >> 16 & 0xfff0) << 16 | (uint32_t)(BAR_PHYS >> 16 & 0xfff0));
	wr_sync(pcie.dbi + 0x24, 0x0000fff0);
	set_bits(pcie.dbi + 0x04, 0, 0x2);      /* memory; no bus master yet: this device reaches for nothing */
	bar = rd(pcie.conf + 0x10);
	wide = (bar & 0x6) == 0x4;
	wr_sync(pcie.conf + 0x10, 0xffffffffu);
	size_mask = rd(pcie.conf + 0x10) & ~0xfu;
	pcie.bar_size = size_mask == 0 ? 0 : (uint64_t)(~size_mask) + 1;
	if (pcie.bar_size == 0 || pcie.bar_size > BAR_WINDOW) {
		say("modem: pcie: the device's BAR 0 is not one this places (%#x, size %#llx)\n", bar, pcie.bar_size);
		return false;
	}
	wr_sync(pcie.conf + 0x10, (uint32_t)BAR_PHYS | (bar & 0xf));
	if (wide) {
		wr_sync(pcie.conf + 0x14, 0);
	}
	set_bits(pcie.conf + 0x04, 0, 0x2);
	say("modem: pcie: DEVICE %04x:%04x behind the bridge; its BAR 0 (%s) at %#lx, %llu KB\n", ids & 0xffff, ids >> 16, wide ? "64-bit" : "32-bit",
	    BAR_PHYS, pcie.bar_size >> 10);
	pcie.up = true;
	return true;
}

/* PERST low, the PHY down, if PCIe 2 was touched: at the run's end, and when a step fails. */
static void
pcie_off(bool can_sleep)
{
	pcie.up = false;
	instantnoodle_gpio_output(PIN_PERST, false, INSTANTNOODLE_PULL_DOWN, 2);
	if (pcie.clocked) {
		wr_sync(pcie.phy + PHY_POWER_DOWN, 0);
		set_bits(pcie.parf + PARF_PHY_CTRL, 0, 1);
	}
	(void)can_sleep;
}

/* What the device's own window says: MHI's registers (Linux drivers/bus/mhi/common.h), its boot loader's state by BHI. */
static void
mhi_look(void)
{
	uint32_t bhi, version;

	pcie.bar = ml_io_map(BAR_PHYS, (vm_size_t)(pcie.bar_size > 0x1000 ? 0x1000 : pcie.bar_size));
	version = rd(pcie.bar + 0x08);
	bhi = rd(pcie.bar + 0x28);
	say("modem: mhi: MHIREGLEN %#x, version %#x, MHICFG %#x, BHI at %#x, MHISTATUS %#x\n", rd(pcie.bar + 0x00), version, rd(pcie.bar + 0x10), bhi, rd(pcie.bar + 0x48));
	if (bhi != 0 && bhi < 0xff0) {
		say("modem: mhi: BHI version %#x, execution environment %u (0 PBL, 1 SBL, 2 AMSS, 3 RDDM, 6 EDL), status %#x, error %#x\n", rd(pcie.bar + bhi),
		    rd(pcie.bar + bhi + 0x28), rd(pcie.bar + bhi + 0x2c), rd(pcie.bar + bhi + 0x30));
	}
}

static void
level2(void)
{
	say("modem: modem=2: the reset and PCIe 2\n");
	if (!modem_reset()) {
		say("modem: level 2 is done: no reset\n");
		return;
	}
	if (!pcie_on()) {
		pins_say("no link");
		pcie_off(true);
		say("modem: level 2 is done: no device\n");
		return;
	}
	mhi_look();
	pins_say("the device answers");
	for (unsigned s = 0; s < 3; s++) {
		delay_for_interval(1, NSEC_PER_SEC);
		say("modem: %u s on: MDM2AP status %u, errfatal %u; link status %#x\n", s + 1, instantnoodle_gpio_level(PIN_MDM2AP_STATUS), instantnoodle_gpio_level(PIN_MDM2AP_ERRFATAL),
		    rd(pcie.dbi + DBI_LINK_STATUS));
	}
	mhi_look();
	say("modem: level 2 is done\n");
}

static uint32_t modem_level;

/*
 * What stops its firmware. PERST low stops the link and not the chip: MDM2AP status is still 1 after it (run 073746). Android's sdx55m_power_down() is this same warm reset ("cold reset
 * is not supported", esoc-mdm-pon.c); status falls within 150 ms, and the chip waits in its primary boot loader for a host. Spins only: this is on a panic's way out as well.
 */
static bool
modem_stop(void)
{
	if (pon_write_channel == 0 || !pon_put(PON_SW_RST_S2_CTL2, 0, true)) {
		return false;
	}
	spin_us(500);
	if (!pon_put(PON_SW_RST_S2_CTL, PON_RESET_TYPE_WARM, true) || !pon_put(PON_SW_RST_S2_CTL2, PON_RESET_EN, true)) {
		return false;
	}
	spin_us(500);
	if (!pon_put(PON_SW_RST_GO, PON_SW_RST_GO_VAL, false)) {
		return false;
	}
	instantnoodle_gpio_output(PIN_AP2MDM_STATUS, false, INSTANTNOODLE_PULL_NONE, 16);
	return true;
}

#include "modem_boot.inc"

static void
modem_thread(__unused void *parameter, __unused wait_result_t result)
{
	look();
	if (modem_level >= 3) {
		level3();
	} else if (modem_level >= 2) {
		level2();
	}
	thread_terminate(current_thread());
}

/* At the run's end and on a panic's way out: the link down before the memory it may reach is reused, and the firmware stopped. A kernel that hangs never comes here: the firmware then goes on. */
void
hk_modem_quiesce(void)
{
#ifdef HK_MDM_HAS_ABP_POLLER
	mdm_abp_quiesce_guard();
#endif
	if (pcie.clocked && !pcie.quiesced) {
		pcie.quiesced = true;
		mdm_polling = false;
		uint32_t before;
		bool stopped;

		pcie_off(false);
		mdm_smmu_off();
		before = instantnoodle_gpio_level(PIN_MDM2AP_STATUS);
		stopped = modem_stop();
		spin_us(150000);
		say("modem: at the run's end: PERST low, the PHY down, the stream given back; THE WARM RESET %s; MDM2AP status %u before it, %u 150 ms after\n", stopped ? "IS DONE" : "FAILED", before,
		    instantnoodle_gpio_level(PIN_MDM2AP_STATUS));
	}
}

void
hk_modem_init(void)
{
	thread_t thread;

	if (!instantnoodle_board() || !PE_parse_boot_argn("modem", &modem_level, sizeof(modem_level)) || modem_level == 0) {
		return;
	}
	if (kernel_thread_start(modem_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
