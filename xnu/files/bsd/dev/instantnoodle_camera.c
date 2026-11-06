/*
 * The cameras of the OnePlus 8 (instantnoodle, SM8250 "kona"; its camera subsystem is the Titan 480), brought up as
 * the POCO X5 Pro's were (bsd/dev/hk_camera.c, which hands boot-arg cam=N to this file on board=instantnoodle): step by
 * step, behind cam=N. Every number below is this phone's: its live device tree (cam-sensor@0..3 under cci@ac4f000 and
 * cci@ac50000 and what they point to, the pins' cam_sensor_*_active states, the rpmh-regulator nodes), the sensors'
 * module files of its camera stack (/odm/lib64/camera/com.qti.sensormodule.<maker>_<sensor>.bin: the slave address,
 * the chip ID and where it is read, the power sequences, the register tables; made into camera-<sensor>.tab by
 * tools/camsensor.py, which reads their older "Parameter Parser V2.0.0" too), and Linux's camcc-sm8250.c,
 * gcc-sm8250.c, pinctrl-sm8250.c, gdsc.c and pinctrl-spmi-gpio.c and Qualcomm's cam_sensor_util.c (the order in which a
 * sensor's supplies and pins are switched), read as data. The four:
 *
 *   wide    Sony IMX586 (48 MP, semco_imx586; cam-sensor@0): CCI 0 master 0 (GPIO 101 data, 102 clock), slave 0x1a,
 *           chip ID 0x0586 at 0x0016; MCLK0 (camcc RCG 0x5000, branch 0x5018) on GPIO 94; reset GPIO 145; three
 *           C-PHY trios into CSIPHY 0. Supplies by the kinds of its power sequence: VANA the PM8150's S5 at 2.04 V
 *           and GPIO 26 (an external regulator's enable), CUSTOM_REG1 the PM8150L's BOB, CUSTOM_GPIO1 GPIO 156
 *           ("CAM_VANA2"), VAF the PM8009's L5 at 2.8 V, VDIG its L1 at 1.104 V, CUSTOM_REG2 its L2 at 1.1 V, VIO
 *           its L7 (1.8 V)
 *   ultra   Sony IMX481 (16 MP ultra-wide, ofilm_imx481; cam-sensor@1): CCI 1 master 1 (GPIO 107/108), slave 0x1a,
 *           ID 0x0481 at 0x0016; MCLK3 (0x5054/0x506c) on GPIO 97; reset GPIO 78; four D-PHY lanes into CSIPHY 3;
 *           VANA BOB and the PM8009's GPIO 1, VDIG L4 at 1.056 V, VIO L7, CUSTOM_GPIO1 GPIO 159 ("CAM_VAF")
 *   front   Sony IMX471 (16 MP, truly_imx471; cam-sensor@2): CCI 1 master 0 (GPIO 105/106), slave 0x10, ID 0x0471 at
 *           0x0016; MCLK2 (0x5038/0x5050) on GPIO 96; reset GPIO 90; four D-PHY lanes into CSIPHY 2; VANA BOB and
 *           GPIO 24, VDIG L3 at 1.056 V, VIO L7
 *   macro   GalaxyCore GC2375 (2 MP, shine_gc2375; cam-sensor@3): CCI 0 master 1 (GPIO 103/104), slave 0x17, ID
 *           0x2375 at 0xf0 (8-bit addresses, the ID in two bytes); MCLK1 (0x501c/0x5034) on GPIO 95; its power-down
 *           pin on the cell's reset GPIO 144 (high = powered down: its sequence raises it first and lowers it last);
 *           one D-PHY lane into CSIPHY 1; VANA BOB and GPIO 139, VIO L7
 *
 * Which module is in which cell: the device tree does not name them. The slave addresses and supplies fit (the
 * macro's module has neither VDIG nor a lens motor, as cam-sensor@3 has neither; the ultra-wide's module is the one
 * of the two others with a motor, as cam-sensor@1 has the actuator); cam=2's chip IDs are what confirms it.
 *
 * The supplies: the PM8009's LDOs, S5 and BOB through RPMh (instantnoodle_supply(): the application processor's
 * votes). Their PMIC registers are not reachable from here -- cam=1 (run 010516) found no channel of the PMIC arbiter
 * for any of them, RPMh drives them its own way -- so RPMh's completion of the vote is the read-back, and 2 ms settle
 * after it. S5 and BOB are rails others hang on (the Wi-Fi's 1.9 V on S5, the PM8150L's LDOs on BOB) and cannot be
 * found out: they are voted at fixed voltages of the device tree's range that lower nothing (S5 2.04 V, given back at
 * the Wi-Fi's 1.9 V; BOB at its qcom,init-voltage 3.312 V, what Linux sends at boot, inside the camera's 3.008-3.96 V,
 * and left there) and never switched off (struct rail). The PM8009 (slave 10) itself does answer: its revision and
 * its GPIO 1 (peripheral 0xc0, an MV GPIO this system may write), which is set as Android's
 * cam_sensor_rear_2_ana_active has it (the normal function, an output, VIN0), its level what the sequence asks,
 * every write read back.
 *
 * MCLK: camcc's MCLKn at 19.2 MHz from the crystal (RCG source 0, divider 1, no M/N), as redwood's macro has it;
 * before it the titan top power domain (GDSCR 0xc144; its state in 0xc148, gdsc.c's POLL_CFG_GDSCR) with camcc's
 * cam_cc_gdsc_clk (0xc16c), and before any camcc register GCC's camera AHB clock (0xb008): Linux keeps both of
 * these always on, and so does this from the first camcc access; if they were found off they are switched off
 * again with the rest. The macro's module file asks for 24 MHz from camcc's PLL2; without cam_pll2=1 that PLL is
 * not started and its ID is read at 19.2 MHz (inside the GC2375's 6-27 MHz). With it, as camcc-sm8250.c and
 * clk-alpha-pll.c have it: PLL2 (a Zonda PLL at 0x2000; not in FSM mode, not found running) configured as
 * cam_cc_pll2_config (L 0x4b: 1440 MHz from the crystal; CONFIG_CTL 0x08200920, CONFIG_CTL_U 0x05002015, USER_CTL
 * 0x100), put in standby, then clk_zonda_pll_enable()'s steps (out of bypass, out of reset, RUN, its lock -- or the
 * frequency lock if TEST_CTL asks for CFA -- then its outputs); the MCLK's RCG from its early output (source 5)
 * divided by 10 with M/N 1/6 (ftbl_cam_cc_mclk0_clk_src's 24 MHz; M 1, N ~5, D ~6 in their 8 bits, dual edge).
 * Every register read back; after the MCLK is off its RCG (M, N, D, CFG) and then PLL2 (clk_zonda_pll_disable()'s
 * steps, then every register it was found with) are put back.
 *
 * Nothing here is done unless a boot-arg asks for it:
 *
 *   cam=1   a look, reading only: GCC's camera clocks; if camcc's AHB clock is on, the titan top power domain, its
 *           clock and the four MCLKs; every pin of the four sensors; the PM8009 (its revision and GPIO 1) and every
 *           rail as the PMICs have it
 *   cam=2   ... then each sensor (boot-arg cam_sensor=wide|ultra|front|macro; without it all four in turn) powered up
 *           as Android powers it (its pins first as cam_sensor_util's power-up has them: the supplies' and reset's
 *           outputs low; then the module file's sequence), its chip ID read, powered down by the module file's own
 *           sequence, and everything put back as it was found
 *   cam=3   the frames' path, a step at a time (boot-arg cam_step=N, 1 without it; frames()): 1 the camera subsystem's
 *           clocks and supplies up (titan_up()) and the first reads of CSIPHY 2 and CSID-lite 0 -- their versions --,
 *           then all down (run 085649); 2 the front sensor streaming its mode 1 through CSIPHY 2 into CSID-lite 0's RX,
 *           whose packet state and counters are read -- nothing written to memory (frames_packets(); run 090634); 3
 *           what the SMMU and the GIC have for the camera, read only (frames_smmu_look(); run 091514); 4 the SMMU set up
 *           for the IFEs' streams and a frame's memory, nothing streamed, all given back (frames_smmu_set(); runs 092248,
 *           092704); 5 one frame of the front sensor's into that memory through CSID-lite 0's RDI 0 and IFE-lite 0's write
 *           master 0, handed to the loader for the host (frames_stream(true); xl camera)
 *   cam=5   the front camera's frames for iOS through /dev/hk-camera (sys/hk_camera.h), as redwood's (serve_ios())
 *   cam_scan=1  (with cam=2) a sensor that does not answer: its bus listed while it is still powered up, every address
 *           from 0x08 to 0x77 read once, one byte with no register before it (i2cdetect's read-byte probe: nothing is
 *           written to any of them). Run 044740 found the wide sensor silent on CCI 0 master 0, where its EEPROM,
 *           actuator and OIS chip are too (the device tree's eeprom@0, actuator@0, ois@0: an LC898124, whose cam_vaf,
 *           PM8009 L6, nothing here switches on), the other three answering. (Whatever asks, a sensor that does not
 *           answer is told as a NACK or SCL held low, with both lines' levels before the first start and after.)
 *           Run 080458 with it: both lines high, a NACK, and 0x54 (the module file's EEPROM, 0xa8) and 0x72/0x73
 *           (its lens motor, 0xe4) answering, the OIS (0x7c) and the sensor not -- the sensor's CAM_VANA2 pin
 *           was still the low-power island's (an eGPIO: line_set())
 *   cam_pll2=1  (with cam=2) a module file's 24 MHz MCLK from camcc's PLL2 (above) instead of 19.2 MHz from the crystal
 *
 * Limits, whatever asks: Android's voltages only (the device tree's); only the registers named here are written,
 * each read back (pins by their registers and their pads' input, the PM8009's GPIO by reading it, rails by RPMh's
 * completion); a write that does not read
 * back, a vote RPMh does not take, a PMIC GPIO not of the kind expected, and everything is switched off again in the
 * power-down's order and
 * nothing more is written in the run. A sensor that does not answer is powered down and the next one tried (nothing
 * on the phone's side has gone wrong then). At the run's end (and on a panic's way out) whatever was switched on is
 * switched off, except the RPMh votes, which are left to the next reset there (no lock may be taken then).
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <vm/vm_kern.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <sys/hk_camera.h>
#include <sys/hk_input.h>
#include <sys/instantnoodle.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static uint32_t level, scan, pll2;
static bool broken, leaving;

static int say(const char *format, ...) __printflike(1, 2);

/* every line to the loader's log as well (and to the console when that may be written) */
static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != '\0'; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
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
	__asm__ volatile ("dsb osh" ::: "memory");
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

static void
give_up(const char *why)
{
	if (!broken) {
		broken = true;
		say("camera: %s: nothing more is switched on in this run\n", why);
	}
}

/* ---- the four sensors ------------------------------------------------------------------------------ */

/* Android's cam_sensor power kinds (cam_sensor_cmn_header.h), as the module files' sequences name them */
enum {
	K_MCLK, K_VANA, K_VDIG, K_VIO, K_VAF, K_VAF_PWDM, K_CUSTOM_REG1, K_CUSTOM_REG2, K_RESET, K_STANDBY, K_CUSTOM_GPIO1,
	K_CUSTOM_GPIO2, K_KINDS
};
static const char *const kind_names[K_KINDS] = {
	"MCLK", "VANA", "VDIG", "VIO", "VAF", "VAF_PWDM", "CUSTOM_REG1", "CUSTOM_REG2", "RESET", "STANDBY", "CUSTOM_GPIO1",
	"CUSTOM_GPIO2"
};

/*
 * A regulator of the device tree's, voted through RPMh. Its PMIC registers cannot be read from here: no channel of
 * the PMIC arbiter is any of these peripherals (cam=1, run 010516: RPMh drives them its own way), so RPMh's having
 * taken the vote (instantnoodle_supply(), which waits for its completion) is the read-back, and a settle follows.
 * The shared ones cannot be found out either, so they are voted at fixed voltages of the device tree's range that
 * lower nothing: S5 at the camera's 2.04 V and back at the Wi-Fi's own 1.9 V (instantnoodle_wifi.c; qcom,init-voltage
 * is 1.824 V); BOB at its qcom,init-voltage 3.312 V (what Linux sends at boot; inside the camera's 3.008-3.96 V) and
 * left there. Neither is ever switched off.
 */
struct rail {
	const char *    name;           /* its resource in RPMh's command database */
	const char *    what;
	uint16_t        mv;             /* the vote: the device tree's rgltr-min-voltage (VIO: the LDO's only voltage) */
	uint16_t        spmi;           /* slave << 8 | peripheral (for the look: unreadable so far) */
	uint16_t        back_mv;        /* a shared rail: the vote it is left at, on (0: not shared, switched off) */
};
enum { R_NONE, R_L1, R_L2, R_L3, R_L4, R_L5, R_L7, R_S5, R_BOB, R_L9A, R_RAILS };
static const struct rail rails[R_RAILS] = {
	[R_L1] = { "ldof1", "PM8009 L1", 1104, 0xa40, 0 },
	[R_L2] = { "ldof2", "PM8009 L2", 1100, 0xa41, 0 },
	[R_L3] = { "ldof3", "PM8009 L3", 1056, 0xa42, 0 },
	[R_L4] = { "ldof4", "PM8009 L4", 1056, 0xa43, 0 },
	[R_L5] = { "ldof5", "PM8009 L5", 2800, 0xa44, 0 },
	[R_L7] = { "ldof7", "PM8009 L7", 1800, 0xa46, 0 },
	[R_S5] = { "smpa5", "PM8150 S5", 2040, 0x020, 1900 },
	[R_BOB] = { "bobc1", "PM8150L BOB", 3312, 0x4a0, 3312 },
	[R_L9A] = { "ldoa9", "PM8150 L9", 1200, 0x048, 1200 },     /* the CSIPHYs' mipi-csi-vdd (cam=3): titan_up() */
};
#define RAIL_SETTLE_MS          2       /* after RPMh's completion, before the next step: margin over the LDOs' own ramps */

/* A pin a power sequence drives: a TLMM one (its active state's drive and pull), or one of the PM8009's GPIOs. */
enum { LINE_NONE, LINE_TLMM, LINE_PMIC };
struct line {
	uint8_t                 kind;
	struct instantnoodle_pin pin;
	uint8_t                 ma, pull;
};

struct sensor_hw {
	const char *            name;
	const char *            tab;            /* in fw/ of the small image (tools/camsensor.py) */
	unsigned                cell, csiphy;
	struct instantnoodle_i2c *bus;
	struct instantnoodle_pin mclk_pin;
	uint32_t                mclk_cmd, mclk_cbcr;
	uint8_t                 rail[K_KINDS];  /* by kind: rails[] (R_NONE: none) */
	struct line             line[K_KINDS];  /* by kind: VANA's switch, RESET, CUSTOM_GPIO1 */
};

#define WEST                    INSTANTNOODLE_TLMM_WEST
#define SOUTH                   INSTANTNOODLE_TLMM_SOUTH
#define NORTH                   INSTANTNOODLE_TLMM_NORTH
#define NO_PULL                 INSTANTNOODLE_PULL_NONE
#define TLMM_LINE(tile, n, ma, pull)    { LINE_TLMM, { tile, n }, ma, pull }
#define FUNCTION_CAM_MCLK       1       /* pinctrl-sm8250.c: GPIO 94-97's function 1 */
#define FUNCTION_CCI_I2C        1       /* GPIO 101-108's */
#define TLMM_EGPIO_PRESENT      (1u << 11)      /* an eGPIO (gpio146-179; line_set()) */
#define TLMM_EGPIO_ENABLE       (1u << 12)      /* ... the TLMM's, not the low-power island's */

/* the four CCI masters' pins, by hand (the device tree's cciN_active: cci_i2c, pull-up, 2 mA; here GPIOs with the pull-up) */
static struct instantnoodle_i2c cci_buses[4] = {
	{ .name = "CCI 0 master 0", .sda = { NORTH, 101 }, .scl = { NORTH, 102 }, .pull = INSTANTNOODLE_PULL_UP },
	{ .name = "CCI 0 master 1", .sda = { NORTH, 103 }, .scl = { NORTH, 104 }, .pull = INSTANTNOODLE_PULL_UP },
	{ .name = "CCI 1 master 0", .sda = { NORTH, 105 }, .scl = { NORTH, 106 }, .pull = INSTANTNOODLE_PULL_UP },
	{ .name = "CCI 1 master 1", .sda = { NORTH, 107 }, .scl = { NORTH, 108 }, .pull = INSTANTNOODLE_PULL_UP },
};

static const struct sensor_hw sensors[] = {
	{ "wide (IMX586)", "camera-imx586.tab", 0, 0, &cci_buses[0], { NORTH, 94 }, 0x5000, 0x5018,
	  .rail = { [K_VANA] = R_S5, [K_VDIG] = R_L1, [K_VIO] = R_L7, [K_VAF] = R_L5, [K_CUSTOM_REG1] = R_BOB, [K_CUSTOM_REG2] = R_L2 },
	  .line = { [K_VANA] = TLMM_LINE(SOUTH, 26, 2, NO_PULL), [K_RESET] = TLMM_LINE(WEST, 145, 2, NO_PULL),
	            [K_CUSTOM_GPIO1] = TLMM_LINE(WEST, 156, 16, INSTANTNOODLE_PULL_UP) } },
	{ "ultra (IMX481)", "camera-imx481.tab", 1, 3, &cci_buses[3], { NORTH, 97 }, 0x5054, 0x506c,
	  .rail = { [K_VANA] = R_BOB, [K_VDIG] = R_L4, [K_VIO] = R_L7 },
	  .line = { [K_VANA] = { LINE_PMIC, { 0, 1 }, 0, 0 }, [K_RESET] = TLMM_LINE(NORTH, 78, 2, NO_PULL),
	            [K_CUSTOM_GPIO1] = TLMM_LINE(WEST, 159, 2, NO_PULL) } },
	{ "front (IMX471)", "camera-imx471.tab", 2, 2, &cci_buses[2], { NORTH, 96 }, 0x5038, 0x5050,
	  .rail = { [K_VANA] = R_BOB, [K_VDIG] = R_L3, [K_VIO] = R_L7 },
	  .line = { [K_VANA] = TLMM_LINE(SOUTH, 24, 2, NO_PULL), [K_RESET] = TLMM_LINE(SOUTH, 90, 2, NO_PULL) } },
	{ "macro (GC2375)", "camera-gc2375.tab", 3, 1, &cci_buses[1], { NORTH, 95 }, 0x501c, 0x5034,
	  .rail = { [K_VANA] = R_BOB, [K_VIO] = R_L7 },
	  .line = { [K_VANA] = TLMM_LINE(WEST, 139, 2, NO_PULL), [K_RESET] = TLMM_LINE(WEST, 144, 2, NO_PULL) } },
};
#define SENSORS                 (sizeof(sensors) / sizeof(sensors[0]))

static const struct sensor_hw *hw = &sensors[0];

/* ---- pins: each one's registers kept before it is first written, and put back at the end ------------------ */

static struct {
	struct instantnoodle_pin pin;
	uint32_t cfg, io;
} found_pins[24];
static unsigned found_pins_n;

static bool
take_pin(struct instantnoodle_pin p)
{
	for (unsigned i = 0; i < found_pins_n; i++) {
		if (found_pins[i].pin.tile == p.tile && found_pins[i].pin.pin == p.pin) {
			return true;
		}
	}
	if (found_pins_n == sizeof(found_pins) / sizeof(found_pins[0])) {
		give_up("more pins than this keeps");
		return false;
	}
	found_pins[found_pins_n].pin = p;
	found_pins[found_pins_n].cfg = instantnoodle_gpio_cfg(p);
	found_pins[found_pins_n].io = instantnoodle_gpio_io(p);
	found_pins_n++;
	return true;
}

static void
give_pins_back(void)
{
	for (unsigned i = found_pins_n; i-- > 0;) {
		instantnoodle_gpio_restore(found_pins[i].pin, found_pins[i].cfg, found_pins[i].io);
	}
	if (found_pins_n != 0) {
		say("camera: %u pins back as they were found\n", found_pins_n);
	}
	found_pins_n = 0;
}

static void
show_pin(struct instantnoodle_pin p, const char *what)
{
	uint32_t cfg = instantnoodle_gpio_cfg(p), io = instantnoodle_gpio_io(p);

	say("camera:   gpio%u (%s): cfg %#x (function %u, pull %u, %u mA%s), in %u, out %u\n", p.pin, what, cfg, cfg >> 2 & 0xf,
	    cfg & 3, ((cfg >> 6 & 7) + 1) * 2, cfg & (1u << 9) ? ", output" : "", io & 1, io >> 1 & 1);
}

/* ---- the PMIC arbiter (version 5), as hk_camera.c and hk_torch.c have it: reads of any peripheral, writes of GPIO 1 --- */

#define ARB_CORE_PHYS           0x0c440000UL
#define ARB_CORE_SIZE           0x1100UL
#define ARB_CNFG_PHYS           0x0c40a000UL
#define ARB_CNFG_SIZE           0x1000UL
#define ARB_OBSERVER_PHYS       0x0e600000UL
#define ARB_OBSERVER_SIZE       0x10000UL
#define ARB_CHNLS_PHYS          0x0c600000UL
#define ARB_SYSTEM              0

#define ARB_VERSION             0x000
#define ARB_VERSION_5           0x50000000u
#define ARB_VERSION_7           0x70000000u
#define ARB_CHANNEL(n)          (0x900 + 4 * (n))
#define ARB_OWNER(n)            (0x700 + 4 * (n))
#define ARB_MAX_CHANNELS        512

#define CHANNEL_COMMAND         0x00
#define CHANNEL_STATUS          0x08
#define CHANNEL_WDATA0          0x10
#define OBSERVER_DATA0          0x18
#define STATUS_DONE             0x1
#define STATUS_FAILURE          0x2
#define STATUS_DENIED           0x4
#define STATUS_DROPPED          0x8
#define COMMAND_WRITE           (0u << 27)
#define COMMAND_READ            (1u << 27)
#define TRANSFER_TIMEOUT_US     2000
#define DROPPED                 "the transfer was dropped"

#define PERIPH_TYPE             0x04
#define PERIPH_SUBTYPE          0x05
#define REG_STATUS1             0x08    /* a regulator's: [7] ready */
#define REG_VSET                0x40    /* a regulator's set point in mV, 16 bits (lower byte first) */
#define REG_EN_CTL              0x46    /* [7] enabled */

#define PM8009_REVID            0xa01
#define PM8009_GPIO1            0xac0   /* (slave 10, 0xc000) */
#define GPIO_TYPE               0x10
#define GPIO_SUBTYPE_LV         0x10
#define GPIO_SUBTYPE_MV         0x11
#define GPIO_RT_STS             0x10    /* [0] what the pin is at */
#define GPIO_MODE_CTL           0x40    /* LV/MV: [1:0] direction (1 output) */
#define GPIO_DIG_VIN_CTL        0x41    /* [2:0] which supply: 0 = VIN0 ("power-source 0") */
#define GPIO_DIG_PULL_CTL       0x42
#define GPIO_OUT_SOURCE_CTL     0x44    /* LV/MV: [7] the level (the "normal" function inverted), [3:0] the function: 0 normal */
#define GPIO_EN_CTL             0x46    /* [7] enabled */

static vm_offset_t arb_core, arb_cnfg, arb_observer;

static struct periph {
	uint16_t        id, channel;
	bool            found, writable;
	vm_offset_t     write;
} periphs[16];
static unsigned periphs_n;

/* A peripheral's channel: looked up once (the one our system may write through, if any, else the first) */
static struct periph *
periph(uint16_t id)
{
	struct periph *p;

	for (unsigned i = 0; i < periphs_n; i++) {
		if (periphs[i].id == id) {
			return &periphs[i];
		}
	}
	if (periphs_n == sizeof(periphs) / sizeof(periphs[0]) || arb_core == 0 || arb_cnfg == 0) {
		return NULL;
	}
	p = &periphs[periphs_n++];
	p->id = id;
	for (uint32_t n = 0; n < ARB_MAX_CHANNELS; n++) {
		uint32_t map = rd(arb_core + ARB_CHANNEL(n)), owner;

		if (map == 0 || ((map >> 8) & 0xfff) != id) {
			continue;
		}
		owner = rd(arb_cnfg + ARB_OWNER(n)) & 7;
		if (!p->found || owner == ARB_SYSTEM) {
			p->found = true;
			p->channel = (uint16_t)n;
			p->writable = owner == ARB_SYSTEM;
		}
	}
	return p;
}

static const char *
wait_done(vm_offset_t channel)
{
	uint64_t start = mach_absolute_time(), interval;
	uint32_t status;

	nanoseconds_to_absolutetime(TRANSFER_TIMEOUT_US * (uint64_t)NSEC_PER_USEC, &interval);
	do {
		status = rd(channel + CHANNEL_STATUS);
	} while (!(status & STATUS_DONE) && mach_absolute_time() - start < interval);
	return !(status & STATUS_DONE) ? "no answer from the arbiter" : (status & STATUS_DENIED) ? "the arbiter refuses" :
	       (status & STATUS_FAILURE) ? "the transfer failed" : (status & STATUS_DROPPED) ? DROPPED : NULL;
}

static void
lock(void)
{
	if (!leaving) {
		hk_spmi_lock();
	}
}

static void
unlock(void)
{
	if (!leaving) {
		hk_spmi_unlock();
	}
}

/* 1 to 4 registers from `reg' on; NULL, or what went wrong */
static const char *
spmi_read(uint16_t id, uint8_t reg, uint8_t *values, unsigned count)
{
	struct periph *p = periph(id);
	vm_offset_t channel;
	const char *problem = "no channel of the arbiter is that peripheral";
	uint32_t data = 0;

	if (p == NULL || !p->found || count == 0 || count > 4) {
		return problem;
	}
	channel = arb_observer + 0x10000UL * ARB_SYSTEM + 0x80UL * p->channel;
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(channel + CHANNEL_COMMAND, COMMAND_READ | (uint32_t)reg << 4 | (count - 1));
		problem = wait_done(channel);
		data = rd(channel + OBSERVER_DATA0);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	for (unsigned i = 0; i < count; i++) {
		values[i] = (uint8_t)(data >> (8 * i));
	}
	return problem;
}

/* One of the PM8009 GPIO 1's registers that the active state sets, written and read back */
static bool
pgpio_put(uint8_t reg, uint8_t value)
{
	struct periph *p = periph(PM8009_GPIO1);
	const char *problem = NULL;
	uint8_t back = 0xff;

	if (p == NULL || !p->found || !p->writable || (reg != GPIO_MODE_CTL && reg != GPIO_DIG_VIN_CTL &&
	    reg != GPIO_OUT_SOURCE_CTL && reg != GPIO_EN_CTL && reg != GPIO_DIG_PULL_CTL)) {
		give_up("a write to the PM8009 that may not be made");
		return false;
	}
	if (p->write == 0) {
		p->write = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * p->channel, 0x1000);
	}
	if (((rd(arb_core + ARB_CHANNEL(p->channel)) >> 8) & 0xfff) != PM8009_GPIO1) {
		give_up("the arbiter's channel is no longer the PM8009's GPIO 1");
		return false;
	}
	for (int tries = 0; tries < 3; tries++) {
		lock();
		wr(p->write + CHANNEL_WDATA0, value);
		wr(p->write + CHANNEL_COMMAND, COMMAND_WRITE | (uint32_t)reg << 4);
		problem = wait_done(p->write);
		unlock();
		if (problem == NULL || strcmp(problem, DROPPED) != 0) {
			break;
		}
	}
	if (problem != NULL || (problem = spmi_read(PM8009_GPIO1, reg, &back, 1)) != NULL || back != value) {
		say("camera: PM8009 GPIO 1 %#04x <- %#04x: %s (reads %#04x)\n", reg, value, problem ? problem : "does not read back", back);
		give_up("a write to the PM8009 that failed");
		return false;
	}
	return true;
}

/* GPIO 1's DIG_VIN_CTL, DIG_PULL_CTL, MODE_CTL, OUT_SOURCE_CTL, EN_CTL: as found, and as they are */
static const uint8_t pgpio_which[5] = { GPIO_DIG_VIN_CTL, GPIO_DIG_PULL_CTL, GPIO_MODE_CTL, GPIO_OUT_SOURCE_CTL, GPIO_EN_CTL };
static uint8_t pgpio_found[5];
static bool pgpio_changed;

static bool
pgpio_read_all(uint8_t *regs)
{
	for (int i = 0; i < 5; i++) {
		if (spmi_read(PM8009_GPIO1, pgpio_which[i], &regs[i], 1) != NULL) {
			return false;
		}
	}
	return true;
}

/* as cam_sensor_rear_2_ana_active, with the level low (the power-up's output-low): the enable last */
static bool
pgpio_prepare(void)
{
	uint8_t type = 0, subtype = 0, regs[5], want[5];

	if (spmi_read(PM8009_GPIO1, PERIPH_TYPE, &type, 1) != NULL || spmi_read(PM8009_GPIO1, PERIPH_SUBTYPE, &subtype, 1) != NULL ||
	    !pgpio_read_all(regs)) {
		give_up("the PM8009's GPIO 1 cannot be read");
		return false;
	}
	if (type != GPIO_TYPE || (subtype != GPIO_SUBTYPE_LV && subtype != GPIO_SUBTYPE_MV)) {
		say("camera: the PM8009's GPIO 1 is of type %#04x subtype %#04x, not an LV/MV GPIO\n", type, subtype);
		give_up("a PMIC GPIO of a kind not known here");
		return false;
	}
	if (!pgpio_changed) {
		memcpy(pgpio_found, regs, sizeof(regs));
	}
	want[0] = (uint8_t)(regs[0] & ~7);
	want[1] = regs[1];
	want[2] = (uint8_t)((regs[2] & ~3) | 1);
	want[3] = 0x00;
	want[4] = (uint8_t)(regs[4] | 0x80);
	pgpio_changed = true;
	for (int i = 0; i < 5; i++) {
		if (want[i] != regs[i] && !pgpio_put(pgpio_which[i], want[i])) {
			return false;
		}
	}
	return true;
}

static bool
pgpio_level(bool high)
{
	return pgpio_put(GPIO_OUT_SOURCE_CTL, high ? 0x80 : 0x00);
}

static void
pgpio_give_back(void)
{
	uint8_t now[5];
	bool all;

	if (!pgpio_changed) {
		return;
	}
	all = pgpio_read_all(now);
	for (int i = 4; all && i >= 0; i--) {
		if (now[i] != pgpio_found[i]) {
			all &= pgpio_put(pgpio_which[i], pgpio_found[i]);
		}
	}
	pgpio_changed = false;
	say("camera: the PM8009's GPIO 1 %s\n", all ? "back as it was found" : "COULD NOT BE PUT BACK");
}

/* ---- the rails ---------------------------------------------------------------------------------------------- */

/* (a status of all ones is no answer, not a ready rail: upstream hakios.camera.c learnt it on redwood's PM8008i) */
#define READY(status)           ((status) != 0xff && ((status) & 0x80))

static bool rail_on[R_RAILS];

static bool
rail_read(unsigned r, uint8_t *status, uint16_t *mv, uint8_t *enable)
{
	uint8_t v[2] = { 0, 0 };

	if (spmi_read(rails[r].spmi, REG_STATUS1, status, 1) != NULL || spmi_read(rails[r].spmi, REG_VSET, v, 2) != NULL ||
	    spmi_read(rails[r].spmi, REG_EN_CTL, enable, 1) != NULL) {
		return false;
	}
	*mv = (uint16_t)(v[0] | v[1] << 8);
	return true;
}

static void
show_rail(unsigned r)
{
	uint8_t type = 0, subtype = 0, status = 0, enable = 0;
	uint16_t mv = 0;
	const char *problem = spmi_read(rails[r].spmi, PERIPH_TYPE, &type, 1);

	if (problem != NULL || spmi_read(rails[r].spmi, PERIPH_SUBTYPE, &subtype, 1) != NULL || !rail_read(r, &status, &mv, &enable)) {
		say("camera:   %s (%s, peripheral %#05x): cannot be read: %s\n", rails[r].what, rails[r].name, rails[r].spmi,
		    problem ? problem : "a read failed");
		return;
	}
	say("camera:   %s (%s): type %#04x subtype %#04x, status %#04x (%s), set point %u mV, enable %#04x\n", rails[r].what,
	    rails[r].name, type, subtype, status, READY(status) ? "ready" : status == 0xff ? "no answer" : "not ready", mv, enable);
}

/* On at its vote: RPMh's completion is the read-back (see struct rail), then the settle */
static bool
rail_up(unsigned r)
{
	const struct rail *x = &rails[r];

	if (broken) {
		return false;
	}
	if (rail_on[r]) {
		return true;
	}
	rail_on[r] = true;              /* (recorded before the request: one RPMh finishes late must still be given back) */
	if (!instantnoodle_supply(x->name, x->mv, true)) {
		say("camera: %s (%s): RPMh did not take the vote of %u mV\n", x->what, x->name, x->mv);
		give_up("a vote RPMh did not take");
		return false;
	}
	wait_ms(RAIL_SETTLE_MS);
	say("camera: %s (%s) on at %u mV (RPMh took it)%s\n", x->what, x->name, x->mv, x->back_mv ? "; a shared rail" : "");
	return true;
}

static void
rail_down(unsigned r)
{
	const struct rail *x = &rails[r];
	bool done;

	if (r == R_NONE || !rail_on[r]) {
		return;
	}
	if (leaving) {
		say("camera: %s left as it is to the next reset (no RPMh vote on the way out)\n", x->what);
		return;
	}
	done = x->back_mv ? instantnoodle_supply(x->name, x->back_mv, true) : instantnoodle_supply(x->name, x->mv, false);
	rail_on[r] = false;
	if (x->back_mv) {
		say("camera: %s left on at %u mV (shared: never switched off)%s\n", x->what, x->back_mv, done ? "" : ": RPMH DID NOT TAKE IT");
	} else {
		say("camera: %s off%s\n", x->what, done ? "" : ": RPMH DID NOT TAKE IT");
	}
}

/* ---- camcc and GCC ------------------------------------------------------------------------------------------ */

#define GCC_PHYS                0x00100000UL
#define GCC_CAMERA_PAGE         0xb000
#define GCC_CAMERA_AHB_CBCR     0xb008          /* gcc-sm8250.c keeps it on (qcom_branch_set_clk_en) */
#define GCC_CAMERA_HF_AXI_CBCR  0xb02c
#define GCC_CAMERA_SF_AXI_CBCR  0xb030
#define GCC_CAMERA_XO_CBCR      0xb040
#define CAMCC_PHYS              0x0ad00000UL
#define CAMCC_SIZE              0x10000UL       /* (the device tree's: all of camcc, one mapping) */
#define PLL2_MODE               0x2000          /* [0] outputs, [1] out of bypass, [2] out of reset, [20] FSM vote, [31] lock */
#define PLL2_L                  0x2004
#define PLL2_ALPHA              0x2008
#define PLL2_USER_CTL           0x200c          /* [3:0] the outputs, [9:8] the main output's post-divider */
#define PLL2_CONFIG_CTL         0x2010
#define PLL2_CONFIG_CTL_U       0x2014
#define PLL2_TEST_CTL           0x201c          /* [16] CFA: wait for the frequency lock instead */
#define PLL2_OPMODE             0x2028          /* 0 standby (off), 1 run */
#define PLL_OUTCTRL             (1u << 0)
#define PLL_BYPASSNL            (1u << 1)
#define PLL_RESET_N             (1u << 2)
#define PLL_VOTE_FSM_ENA        (1u << 20)
#define PLL_FREQ_LOCK_DET       (1u << 29)
#define PLL_LOCK_DET            (1u << 31)
#define PLL_CFA                 (1u << 16)
#define PLL2_REGS               6               /* L, ALPHA, USER_CTL, CONFIG_CTL, CONFIG_CTL_U, OPMODE: kept, put back */
#define TITAN_TOP_GDSCR         0xc144          /* [0] collapse, [31] on */
#define TITAN_TOP_GDSC_CFG      0xc148          /* [16] powered up, [15] powered down (gdsc.c, POLL_CFG_GDSCR) */
#define CAM_CC_GDSC_CBCR        0xc16c          /* CLK_IS_CRITICAL in camcc-sm8250.c */
#define MCLK_CMD_RCGR           (hw->mclk_cmd)  /* [0] update, [31] the root is off */
#define MCLK_CFG_RCGR           (hw->mclk_cmd + 4)      /* [4:0] divider (2n - 1), [10:8] source (0: the crystal), [13:12] M/N mode */
#define MCLK_M_RCGR             (hw->mclk_cmd + 8)      /* [7:0] M */
#define MCLK_N_RCGR             (hw->mclk_cmd + 0xc)    /* [7:0] ~(N - M) */
#define MCLK_D_RCGR             (hw->mclk_cmd + 0x10)   /* [7:0] ~2D */
#define MCLK_CBCR               (hw->mclk_cbcr) /* [0] enable, [31] the clock is off */
#define CBCR_OFF                (1u << 31)
#define CMD_UPDATE              1u
#define GDSC_COLLAPSE           1u
#define GDSC_ON                 (1u << 31)
#define GDSC_UP_COMPLETE        (1u << 16)
#define GDSC_DOWN_COMPLETE      (1u << 15)

static vm_offset_t gcc_camera, camcc;
static uint32_t found_mclk_cfg_rcgr, found_mclk_cbcr, found_mclk_mnd[3], found_pll2[PLL2_REGS], found_pll2_mode;
static bool ahb_switched, gdsc_clk_switched, gdsc_switched, mclk_switched, mclk_mnd_switched, pll2_switched, powered;
static const uint32_t pll2_regs[PLL2_REGS] = { PLL2_L, PLL2_ALPHA, PLL2_USER_CTL, PLL2_CONFIG_CTL, PLL2_CONFIG_CTL_U, PLL2_OPMODE };

static vm_offset_t
gcc_reg(uint32_t offset)
{
	return gcc_camera + (offset - GCC_CAMERA_PAGE);
}

static vm_offset_t
camcc_reg(uint32_t offset)
{
	return camcc + offset;
}

static bool
ahb_on(void)
{
	return !(rd(gcc_reg(GCC_CAMERA_AHB_CBCR)) & CBCR_OFF);
}

static bool titan_writable(uint32_t offset);

/* camcc's registers: only these (and the frames' clocks, titan_writable()), each read back */
static bool
camcc_put(uint32_t offset, uint32_t value, uint32_t check_mask)
{
	bool pll2_reg = offset == PLL2_MODE || titan_writable(offset);

	for (unsigned i = 0; i < PLL2_REGS; i++) {
		pll2_reg |= offset == pll2_regs[i];
	}
	if (offset != TITAN_TOP_GDSCR && offset != CAM_CC_GDSC_CBCR && offset != MCLK_CFG_RCGR && offset != MCLK_CMD_RCGR &&
	    offset != MCLK_CBCR && offset != MCLK_M_RCGR && offset != MCLK_N_RCGR && offset != MCLK_D_RCGR && !pll2_reg) {
		give_up("a camcc register that is not to be written");
		return false;
	}
	wr(camcc_reg(offset), value);
	if ((rd(camcc_reg(offset)) & check_mask) != (value & check_mask)) {
		say("camera: camcc %#x <- %#x, but it reads %#x\n", offset, value, rd(camcc_reg(offset)));
		give_up("a camcc register that did not read back");
		return false;
	}
	return true;
}

static bool
wait_bits(vm_offset_t address, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t waited = 0; waited <= us; waited += 10) {
		if ((rd(address) & mask) == want) {
			return true;
		}
		spin_us(10);
	}
	return false;
}

/* GCC's camera AHB clock, without which no camcc register may be read (Linux has it on from boot on) */
static bool
camera_ahb_up(void)
{
	uint32_t cbcr = rd(gcc_reg(GCC_CAMERA_AHB_CBCR));

	if (!(cbcr & CBCR_OFF)) {
		return true;
	}
	if (broken) {
		return false;
	}
	wr(gcc_reg(GCC_CAMERA_AHB_CBCR), cbcr | 1);
	ahb_switched = true;
	if (!(rd(gcc_reg(GCC_CAMERA_AHB_CBCR)) & 1) || !wait_bits(gcc_reg(GCC_CAMERA_AHB_CBCR), CBCR_OFF, 0, 500)) {
		say("camera: GCC's camera AHB clock does not come on (%#x)\n", rd(gcc_reg(GCC_CAMERA_AHB_CBCR)));
		give_up("a clock that does not come on");
		return false;
	}
	say("camera: GCC's camera AHB clock on (%#x), as Linux has it\n", rd(gcc_reg(GCC_CAMERA_AHB_CBCR)));
	return true;
}

static void
camera_ahb_down(void)
{
	if (!ahb_switched) {
		return;
	}
	wr(gcc_reg(GCC_CAMERA_AHB_CBCR), rd(gcc_reg(GCC_CAMERA_AHB_CBCR)) & ~1u);
	ahb_switched = false;
	say("camera: GCC's camera AHB clock off again, as it was found (%#x)\n", rd(gcc_reg(GCC_CAMERA_AHB_CBCR)));
}

static bool
gdsc_up(void)
{
	uint32_t found;

	if (broken) {
		return false;
	}
	found = rd(camcc_reg(CAM_CC_GDSC_CBCR));
	if (found & CBCR_OFF) {
		if (!camcc_put(CAM_CC_GDSC_CBCR, found | 1, 1)) {
			return false;
		}
		gdsc_clk_switched = true;
		if (!wait_bits(camcc_reg(CAM_CC_GDSC_CBCR), CBCR_OFF, 0, 500)) {
			say("camera: camcc's GDSC clock does not come on (%#x)\n", rd(camcc_reg(CAM_CC_GDSC_CBCR)));
			give_up("a clock that does not come on");
			return false;
		}
		say("camera: camcc's GDSC clock on (%#x)\n", rd(camcc_reg(CAM_CC_GDSC_CBCR)));
	}
	found = rd(camcc_reg(TITAN_TOP_GDSCR));
	if (!(found & GDSC_COLLAPSE) && (found & GDSC_ON)) {
		say("camera: the titan top power domain was on already (%#x)\n", found);
		return true;
	}
	if (!camcc_put(TITAN_TOP_GDSCR, found & ~GDSC_COLLAPSE, GDSC_COLLAPSE)) {
		return false;
	}
	gdsc_switched = true;
	spin_us(1);
	if (!wait_bits(camcc_reg(TITAN_TOP_GDSC_CFG), GDSC_UP_COMPLETE, GDSC_UP_COMPLETE, 500)) {
		say("camera: the titan top power domain does not come on (%#x, cfg %#x)\n", rd(camcc_reg(TITAN_TOP_GDSCR)),
		    rd(camcc_reg(TITAN_TOP_GDSC_CFG)));
		give_up("a power domain that does not come on");
		return false;
	}
	say("camera: the titan top power domain on (%#x, cfg %#x)\n", rd(camcc_reg(TITAN_TOP_GDSCR)), rd(camcc_reg(TITAN_TOP_GDSC_CFG)));
	return true;
}

static void
gdsc_down(void)
{
	if (gdsc_switched) {
		wr(camcc_reg(TITAN_TOP_GDSCR), rd(camcc_reg(TITAN_TOP_GDSCR)) | GDSC_COLLAPSE);
		gdsc_switched = false;
		say("camera: the titan top power domain %s (%#x, cfg %#x)\n",
		    wait_bits(camcc_reg(TITAN_TOP_GDSC_CFG), GDSC_DOWN_COMPLETE, GDSC_DOWN_COMPLETE, 500) ? "off" : "NOT OFF",
		    rd(camcc_reg(TITAN_TOP_GDSCR)), rd(camcc_reg(TITAN_TOP_GDSC_CFG)));
	}
	if (gdsc_clk_switched) {
		wr(camcc_reg(CAM_CC_GDSC_CBCR), rd(camcc_reg(CAM_CC_GDSC_CBCR)) & ~1u);
		gdsc_clk_switched = false;
		say("camera: camcc's GDSC clock off again, as it was found (%#x)\n", rd(camcc_reg(CAM_CC_GDSC_CBCR)));
	}
}

/*
 * camcc's PLL2 at 1440 MHz, for a 24 MHz MCLK (cam_pll2=1; the header comment): clk_zonda_pll_configure() with
 * cam_cc_pll2_config, then clk_zonda_pll_enable(). Not if it is in FSM mode or found running: then it is somebody else's.
 */
static bool
pll2_up(void)
{
	uint32_t mode, lock;

	if (broken) {
		return false;
	}
	if (pll2_switched) {
		return true;
	}
	mode = rd(camcc_reg(PLL2_MODE));
	for (unsigned i = 0; i < PLL2_REGS; i++) {
		found_pll2[i] = rd(camcc_reg(pll2_regs[i]));
	}
	say("camera: camcc's PLL2 as found: mode %#x, L %#x, alpha %#x, user %#x, config %#x %#x, opmode %#x, test %#x\n", mode,
	    found_pll2[0], found_pll2[1], found_pll2[2], found_pll2[3], found_pll2[4], found_pll2[5], rd(camcc_reg(PLL2_TEST_CTL)));
	if ((mode & (PLL_VOTE_FSM_ENA | PLL_OUTCTRL | PLL_LOCK_DET)) != 0 || (found_pll2[5] & 1) != 0) {
		give_up("camcc's PLL2 is in use (somebody else's)");
		return false;
	}
	found_pll2_mode = mode;
	pll2_switched = true;
	/* the configuration (Linux writes its values that are not 0; alpha's 0 too here), in bypass, then standby */
	if (!camcc_put(PLL2_L, 0x4b, 0xffff) || !camcc_put(PLL2_ALPHA, 0, ~0u) || !camcc_put(PLL2_CONFIG_CTL, 0x08200920, ~0u) ||
	    !camcc_put(PLL2_CONFIG_CTL_U, 0x05002015, ~0u) || !camcc_put(PLL2_USER_CTL, (found_pll2[2] & ~0xf0fu) | 0x100, 0xf0f) ||
	    !camcc_put(PLL2_MODE, mode & ~(PLL_BYPASSNL | PLL_OUTCTRL), 7) || !camcc_put(PLL2_OPMODE, 0, 7) ||
	    !camcc_put(PLL2_MODE, (mode & ~(PLL_BYPASSNL | PLL_OUTCTRL)) | PLL_RESET_N, 7)) {
		return false;
	}
	/* out of bypass, 1 us, out of reset, RUN; its lock (the frequency lock with CFA); its outputs, then the global one */
	if (!camcc_put(PLL2_MODE, rd(camcc_reg(PLL2_MODE)) | PLL_BYPASSNL, 7)) {
		return false;
	}
	spin_us(1);
	if (!camcc_put(PLL2_MODE, rd(camcc_reg(PLL2_MODE)) | PLL_RESET_N, 7) || !camcc_put(PLL2_OPMODE, 1, 7)) {
		return false;
	}
	lock = rd(camcc_reg(PLL2_TEST_CTL)) & PLL_CFA ? PLL_FREQ_LOCK_DET : PLL_LOCK_DET;
	if (!wait_bits(camcc_reg(PLL2_MODE), lock, lock, 1500)) {
		say("camera: camcc's PLL2 does not lock (mode %#x)\n", rd(camcc_reg(PLL2_MODE)));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!camcc_put(PLL2_USER_CTL, rd(camcc_reg(PLL2_USER_CTL)) | 0xf, 0xf) ||
	    !camcc_put(PLL2_MODE, rd(camcc_reg(PLL2_MODE)) | PLL_OUTCTRL, 7)) {
		return false;
	}
	say("camera: camcc's PLL2 on at 1440 MHz (mode %#x, L %#x, user %#x, opmode %#x)\n", rd(camcc_reg(PLL2_MODE)),
	    rd(camcc_reg(PLL2_L)), rd(camcc_reg(PLL2_USER_CTL)), rd(camcc_reg(PLL2_OPMODE)));
	return true;
}

/* clk_zonda_pll_disable()'s steps (the outputs off, bypass and reset, standby), then every register as it was found */
static void
pll2_down(void)
{
	if (!pll2_switched) {
		return;
	}
	wr(camcc_reg(PLL2_MODE), rd(camcc_reg(PLL2_MODE)) & ~PLL_OUTCTRL);
	wr(camcc_reg(PLL2_USER_CTL), rd(camcc_reg(PLL2_USER_CTL)) & ~0xfu);
	wr(camcc_reg(PLL2_MODE), rd(camcc_reg(PLL2_MODE)) & ~(PLL_RESET_N | PLL_BYPASSNL));
	wr(camcc_reg(PLL2_OPMODE), 0);
	for (unsigned i = 0; i < PLL2_REGS; i++) {
		wr(camcc_reg(pll2_regs[i]), found_pll2[i]);
	}
	wr(camcc_reg(PLL2_MODE), (rd(camcc_reg(PLL2_MODE)) & ~7u) | (found_pll2_mode & 7));
	pll2_switched = false;
	say("camera: camcc's PLL2 off, as it was found (mode %#x, L %#x, opmode %#x)\n", rd(camcc_reg(PLL2_MODE)),
	    rd(camcc_reg(PLL2_L)), rd(camcc_reg(PLL2_OPMODE)));
}

/*
 * MCLK: at 19.2 MHz the RCG from the crystal, divider 1, no M/N; at 24 MHz (cam_pll2=1) from PLL2's early output,
 * divider 10, M/N 1/6 (the header comment); then the branch; then the pin (cam_mclk, no pull, 2 mA)
 */
static bool
mclk_up(uint32_t khz)
{
	bool from_pll2 = khz == 24000;

	if (broken) {
		return false;
	}
	found_mclk_cfg_rcgr = rd(camcc_reg(MCLK_CFG_RCGR));
	found_mclk_cbcr = rd(camcc_reg(MCLK_CBCR));
	found_mclk_mnd[0] = rd(camcc_reg(MCLK_M_RCGR));
	found_mclk_mnd[1] = rd(camcc_reg(MCLK_N_RCGR));
	found_mclk_mnd[2] = rd(camcc_reg(MCLK_D_RCGR));
	if (found_mclk_cbcr & 1) {
		give_up("MCLK is on already (somebody else's)");
		return false;
	}
	if (from_pll2 && !pll2_up()) {
		return false;
	}
	mclk_switched = true;
	mclk_mnd_switched = from_pll2;
	if (from_pll2 && (!camcc_put(MCLK_M_RCGR, (found_mclk_mnd[0] & ~0xffu) | 1u, 0xff) ||
	    !camcc_put(MCLK_N_RCGR, (found_mclk_mnd[1] & ~0xffu) | (~(6u - 1u) & 0xffu), 0xff) ||
	    !camcc_put(MCLK_D_RCGR, (found_mclk_mnd[2] & ~0xffu) | (~6u & 0xffu), 0xff))) {
		return false;
	}
	if (!camcc_put(MCLK_CFG_RCGR, (found_mclk_cfg_rcgr & ~0x371fu) | (from_pll2 ? 5u << 8 | 2u << 12 | 19u : 0u << 8 | 1u), 0x371f) ||
	    !camcc_put(MCLK_CMD_RCGR, rd(camcc_reg(MCLK_CMD_RCGR)) | CMD_UPDATE, 0)) {
		return false;
	}
	if (!wait_bits(camcc_reg(MCLK_CMD_RCGR), CMD_UPDATE, 0, 500)) {
		say("camera: MCLK's RCG does not take its configuration (cmd %#x)\n", rd(camcc_reg(MCLK_CMD_RCGR)));
		give_up("an RCG that does not update");
		return false;
	}
	if (!camcc_put(MCLK_CBCR, found_mclk_cbcr | 1, 1)) {
		return false;
	}
	if (!wait_bits(camcc_reg(MCLK_CBCR), CBCR_OFF, 0, 500)) {
		say("camera: MCLK's branch does not come on (%#x)\n", rd(camcc_reg(MCLK_CBCR)));
		give_up("a clock that does not come on");
		return false;
	}
	if (!take_pin(hw->mclk_pin)) {
		return false;
	}
	instantnoodle_gpio_function(hw->mclk_pin, FUNCTION_CAM_MCLK, INSTANTNOODLE_PULL_NONE, 2);
	if (from_pll2) {
		say("camera: MCLK on at 24 MHz from PLL2 (RCG cfg %#x, M %#x, N %#x, D %#x, branch %#x, gpio%u cfg %#x)\n",
		    rd(camcc_reg(MCLK_CFG_RCGR)), rd(camcc_reg(MCLK_M_RCGR)), rd(camcc_reg(MCLK_N_RCGR)), rd(camcc_reg(MCLK_D_RCGR)),
		    rd(camcc_reg(MCLK_CBCR)), hw->mclk_pin.pin, instantnoodle_gpio_cfg(hw->mclk_pin));
	} else {
		say("camera: MCLK on at 19.2 MHz (RCG cfg %#x, branch %#x, gpio%u cfg %#x)\n", rd(camcc_reg(MCLK_CFG_RCGR)),
		    rd(camcc_reg(MCLK_CBCR)), hw->mclk_pin.pin, instantnoodle_gpio_cfg(hw->mclk_pin));
	}
	return true;
}

static void
mclk_down(void)
{
	if (!mclk_switched) {
		return;
	}
	instantnoodle_gpio_input(hw->mclk_pin, INSTANTNOODLE_PULL_DOWN);       /* (cam_sensor_*_clock_suspend; then as found) */
	wr(camcc_reg(MCLK_CBCR), rd(camcc_reg(MCLK_CBCR)) & ~1u);
	(void)wait_bits(camcc_reg(MCLK_CBCR), CBCR_OFF, CBCR_OFF, 500);
	/* (the RCG back to the crystal while PLL2 still runs: it switches only between two running sources) */
	wr(camcc_reg(MCLK_CFG_RCGR), found_mclk_cfg_rcgr);
	if (mclk_mnd_switched) {
		wr(camcc_reg(MCLK_M_RCGR), found_mclk_mnd[0]);
		wr(camcc_reg(MCLK_N_RCGR), found_mclk_mnd[1]);
		wr(camcc_reg(MCLK_D_RCGR), found_mclk_mnd[2]);
		mclk_mnd_switched = false;
	}
	wr(camcc_reg(MCLK_CMD_RCGR), rd(camcc_reg(MCLK_CMD_RCGR)) | CMD_UPDATE);
	(void)wait_bits(camcc_reg(MCLK_CMD_RCGR), CMD_UPDATE, 0, 500);
	mclk_switched = false;
	say("camera: MCLK off (branch %#x, RCG cfg %#x as found)\n", rd(camcc_reg(MCLK_CBCR)), rd(camcc_reg(MCLK_CFG_RCGR)));
}

/*
 * ---- the camera subsystem for the frames (cam=3) ---------------------------------------------------------------------
 *
 * As Linux's camcc-sm8250.c and clk-alpha-pll.c have it: camcc's PLL0 (a Lucid PLL: the Trion registers) at 1200 MHz
 * (cam_cc_pll0_config; its even output /2 = 600, odd /3 = 400) -- or as found if the firmware left it running
 * (clk_trion_pll_configure() does not touch it then) --, the RCGs of the front sensor's path from it, the branches
 * the device tree gives the CPAS, CSIPHY 2 and CSID-lite / IFE-lite 0, GCC's camera AXI clocks; the CSIPHYs' 1.2 V
 * (PM8150 L9, mipi-csi-vdd -- in the device tree also the UFS PHY's vdda-pll, the three PCIe controllers' (the Wi-Fi's
 * among them), both USB SuperSpeed PHYs', DP's and both DSI controllers': so voted in its high-power mode, as
 * instantnoodle_supply() votes every LDO, and never switched off -- the storage and the screen hang on it) and refgen (0x88e7000,
 * qcom-refgen-regulator.c's SM8250 enable: PWRDWN_CTRL5 bit 0). Whatever is found on is left so; what this switches
 * on goes off again in the reverse order, every RCG back on its found source while PLL0 still runs.
 */
#define PLL0_MODE               0x0000          /* (PLL2's bits: PLL_OUTCTRL, ...; [23] update bypass) */
#define PLL0_L                  0x0004
#define PLL0_CAL_L              0x0008
#define PLL0_USER_CTL           0x000c          /* [2:0] the outputs */
#define PLL0_USER_CTL_U         0x0010
#define PLL0_CONFIG_CTL         0x0018
#define PLL0_CONFIG_CTL_U       0x001c
#define PLL0_CONFIG_CTL_U1      0x0020
#define PLL0_OPMODE             0x0038
#define PLL0_ALPHA              0x0040
#define PLL_UPDATE_BYPASS       (1u << 23)
#define REFGEN_PHYS             0x088e7000UL
#define REFGEN_PWRDWN_CTRL5     0x80            /* [0] on */
#define CSIPHY2_PHYS            0x0ac6e000UL    /* (cam_csiphy2) */
#define CSIPHY0_PHYS            0x0ac6a000UL    /* (cam_csiphy0: the wide IMX586's, three C-PHY trios) */
#define CSIPHY3_PHYS            0x0ac70000UL    /* (cam_csiphy3: the ultra-wide IMX481's, four D-PHY lanes) */
#define CSIPHY_SIZE             0x2000UL
#define CSIPHY_COMMON_CTRL6     0x818           /* camss-csiphy-3ph-1-0.c (SM8250: common 0x800): [0] out of power-down, [1] show the revision */
#define CSIPHY_COMMON_STATUS(n) (0x8b0 + 4 * (n))       /* 12..15: the revision, a byte each */
#define IFE_LITE0_PHYS          0x0acd9000UL    /* (cam_vfe_lite0; cam_csid_lite0 at 0x200 in it) */
#define IFE_LITE_SIZE           0x3000UL
#define CSID_LITE0              0x200
#define CSID_HW_VERSION         0x0             /* camss-csid.c: [31:28] generation, [27:16] revision, [15:0] stepping */

static const struct {
	uint32_t offset, value;
} pll0_config[] = {                     /* (Linux writes the values that are not 0, CAL_L its TRION_PLL_CAL_VAL) */
	{ PLL0_L, 0x3e }, { PLL0_CAL_L, 0x44 }, { PLL0_ALPHA, 0x8000 }, { PLL0_CONFIG_CTL, 0x20485699 },
	{ PLL0_CONFIG_CTL_U, 0x00002261 }, { PLL0_CONFIG_CTL_U1, 0x329a699c }, { PLL0_USER_CTL, 0x00003100 },
	{ PLL0_USER_CTL_U, 0x00000805 },
};
#define PLL0_REGS               (sizeof(pll0_config) / sizeof(pll0_config[0]))

/* an RCG from PLL0 (source 2 its even output, 3 its odd one; the divider field 2n - 1, no M/N), then the branches */
struct titan_rcg {
	const char *    name;
	uint32_t        cmd, source, divider, found;
	bool            set;
};
static struct titan_rcg titan_rcgs[] = {
	{ "slow_ahb 80 MHz", 0x7058, 2, 14 },           /* 600 / 7.5 */
	{ "camnoc_axi 400 MHz", 0xc0f8, 3, 1 },
	{ "cphy_rx 400 MHz", 0xa068, 3, 1 },
	{ "csi2phytimer 300 MHz", 0x6040, 2, 3 },       /* 600 / 2 */
	{ "csi0phytimer 300 MHz", 0x6000, 2, 3 },       /* (the wide one's PHY; camcc-sm8250.c, as csi2phytimer) */
	{ "csi3phytimer 300 MHz", 0x6060, 2, 3 },       /* (the ultra-wide one's; camcc-sm8250.c's, 0x20 a PHY on) */
	{ "ife_lite_csid 400 MHz", 0xc01c, 3, 1 },
	{ "ife_lite 400 MHz", 0xc000, 3, 1 },
};
struct titan_branch {
	const char *    name;
	bool            gcc, halt;      /* halt: its off bit tells (all of these; BRANCH_HALT_DELAY ones would not) */
	uint32_t        cbcr;
	bool            set;
};
static struct titan_branch titan_branches[] = {
	{ "gcc camera_hf_axi", true, true, GCC_CAMERA_HF_AXI_CBCR }, { "gcc camera_sf_axi", true, true, GCC_CAMERA_SF_AXI_CBCR },
	/*
	 * (not camcc-sm8250.c's core_ahb at 0xc150: on this phone that register reads 0 and keeps no enable -- run 085220 --,
	 * and Linux's camss takes none for SM8250: cpas_ahb and slow_ahb for the blocks' registers, their own clocks)
	 */
	{ "cpas_ahb", false, true, 0xc0f4 }, { "camnoc_axi", false, true, 0xc114 },
	{ "csiphy2", false, true, 0x605c }, { "csi2phytimer", false, true, 0x6058 },
	{ "csiphy0", false, true, 0x601c }, { "csi0phytimer", false, true, 0x6018 },
	{ "csiphy3", false, true, 0x607c }, { "csi3phytimer", false, true, 0x6078 },
	{ "ife_lite_ahb", false, true, 0xc040 }, { "ife_lite_axi", false, true, 0xc044 }, { "ife_lite", false, true, 0xc018 },
	{ "ife_lite_cphy_rx", false, true, 0xc03c }, { "ife_lite_csid", false, true, 0xc034 },
};
#define TITAN_RCGS              (sizeof(titan_rcgs) / sizeof(titan_rcgs[0]))
#define TITAN_BRANCHES          (sizeof(titan_branches) / sizeof(titan_branches[0]))

static vm_offset_t refgen, csiphy0, csiphy2, csiphy3, ife_lite0;
static vm_offset_t csiphy;              /* the stream's PHY: csiphy2 (the front sensor's), csiphy0 (the wide one's), csiphy3 */
/* cam=3: whose frames (sensors[]: boot-arg cam_sensor=wide the IMX586, 0; =ultra the IMX481, 1; else the front, 2) */
static unsigned frames_sensor = 2;

/*
 * The frames' sensor (frames_sensor, an index of sensors[]) as the frames say it: its colour filter's order (the module
 * files, tools/camsensor.py: the IMX586's and the IMX481's RGGB, 0; the IMX471's BGGR, 3), its number in sys/hk_camera.h
 * (1 the wide, 2 the front, 3 the ultra-wide camera), its chip; and the other way, the index of hk_camera.h's number.
 */
static uint32_t
frames_bayer(void)
{
	return frames_sensor == 2 ? 3 : 0;
}

static uint32_t
frames_number(void)
{
	return frames_sensor == 0 ? 1 : frames_sensor == 1 ? 3 : 2;
}

static const char *
frames_chip(void)
{
	return frames_sensor == 0 ? "IMX586" : frames_sensor == 1 ? "IMX481" : "IMX471";
}
/*
 * ... in which of its modes (the module file's resN): 1 as a rule; the wide one's 6 is the middle of its full resolution,
 * 4000x3000 at 2000,1500 -- the iPhone's 2x, the sensor's own pixels (boot-arg cam_mode=N with cam=3; iOS's HK_CAMERA_SENSOR b)
 */
static unsigned frames_mode = 1;
static uint32_t found_pll0[PLL0_REGS], found_pll0_mode, found_pll0_opmode;
static bool pll0_switched, refgen_switched, titan_switched;

static bool
titan_writable(uint32_t offset)
{
	if (offset == PLL0_MODE || offset == PLL0_OPMODE) {
		return true;
	}
	for (unsigned i = 0; i < PLL0_REGS; i++) {
		if (offset == pll0_config[i].offset) {
			return true;
		}
	}
	for (unsigned i = 0; i < TITAN_RCGS; i++) {
		if (offset == titan_rcgs[i].cmd || offset == titan_rcgs[i].cmd + 4) {
			return true;
		}
	}
	for (unsigned i = 0; i < TITAN_BRANCHES; i++) {
		if (!titan_branches[i].gcc && offset == titan_branches[i].cbcr) {
			return true;
		}
	}
	return false;
}

static bool
pll0_up(void)
{
	uint32_t mode = rd(camcc_reg(PLL0_MODE)), opmode = rd(camcc_reg(PLL0_OPMODE));

	if (broken) {
		return false;
	}
	say("camera: camcc's PLL0 as found: mode %#x, L %#x, alpha %#x, opmode %#x, user %#x\n", mode, rd(camcc_reg(PLL0_L)),
	    rd(camcc_reg(PLL0_ALPHA)), opmode, rd(camcc_reg(PLL0_USER_CTL)));
	if (mode & PLL_VOTE_FSM_ENA) {
		give_up("camcc's PLL0 is in FSM mode (somebody else's)");
		return false;
	}
	if ((mode & (PLL_LOCK_DET | PLL_OUTCTRL)) == (PLL_LOCK_DET | PLL_OUTCTRL) && (opmode & 1)) {
		if ((rd(camcc_reg(PLL0_L)) & 0xffff) != 0x3e || (rd(camcc_reg(PLL0_ALPHA)) & 0xffff) != 0x8000) {
			give_up("camcc's PLL0 runs at another rate");
			return false;
		}
		say("camera: camcc's PLL0 runs already at 1200 MHz: used as found, left running\n");
		return true;
	}
	if ((mode & (PLL_LOCK_DET | PLL_OUTCTRL)) != 0 || (opmode & 1)) {
		give_up("camcc's PLL0 is half on");
		return false;
	}
	for (unsigned i = 0; i < PLL0_REGS; i++) {
		found_pll0[i] = rd(camcc_reg(pll0_config[i].offset));
	}
	found_pll0_mode = mode;
	found_pll0_opmode = opmode;
	pll0_switched = true;
	/* clk_trion_pll_configure(): the configuration; update bypass, the output off, standby, out of reset */
	for (unsigned i = 0; i < PLL0_REGS; i++) {
		if (!camcc_put(pll0_config[i].offset, pll0_config[i].value, pll0_config[i].offset == PLL0_L ? 0xffff : ~0u)) {
			return false;
		}
	}
	if (!camcc_put(PLL0_MODE, rd(camcc_reg(PLL0_MODE)) | PLL_UPDATE_BYPASS, PLL_UPDATE_BYPASS) ||
	    !camcc_put(PLL0_MODE, rd(camcc_reg(PLL0_MODE)) & ~PLL_OUTCTRL, PLL_OUTCTRL) || !camcc_put(PLL0_OPMODE, 0, 7) ||
	    !camcc_put(PLL0_MODE, rd(camcc_reg(PLL0_MODE)) | PLL_RESET_N, PLL_RESET_N)) {
		return false;
	}
	/* clk_trion_pll_enable(): RUN, its lock, its outputs, the global output */
	if (!camcc_put(PLL0_OPMODE, 1, 7)) {
		return false;
	}
	if (!wait_bits(camcc_reg(PLL0_MODE), PLL_LOCK_DET, PLL_LOCK_DET, 1500)) {
		say("camera: camcc's PLL0 does not lock (mode %#x)\n", rd(camcc_reg(PLL0_MODE)));
		give_up("a PLL that does not lock");
		return false;
	}
	if (!camcc_put(PLL0_USER_CTL, rd(camcc_reg(PLL0_USER_CTL)) | 7, 7) ||
	    !camcc_put(PLL0_MODE, rd(camcc_reg(PLL0_MODE)) | PLL_OUTCTRL, PLL_OUTCTRL)) {
		return false;
	}
	say("camera: camcc's PLL0 on at 1200 MHz (mode %#x, user %#x, opmode %#x)\n", rd(camcc_reg(PLL0_MODE)),
	    rd(camcc_reg(PLL0_USER_CTL)), rd(camcc_reg(PLL0_OPMODE)));
	return true;
}

/* clk_trion_pll_disable()'s steps (the outputs off, standby), then every register it was found with */
static void
pll0_down(void)
{
	if (!pll0_switched) {
		return;
	}
	wr(camcc_reg(PLL0_MODE), rd(camcc_reg(PLL0_MODE)) & ~PLL_OUTCTRL);
	wr(camcc_reg(PLL0_USER_CTL), rd(camcc_reg(PLL0_USER_CTL)) & ~7u);
	wr(camcc_reg(PLL0_OPMODE), 0);
	for (unsigned i = 0; i < PLL0_REGS; i++) {
		wr(camcc_reg(pll0_config[i].offset), found_pll0[i]);
	}
	wr(camcc_reg(PLL0_OPMODE), found_pll0_opmode);
	wr(camcc_reg(PLL0_MODE), (rd(camcc_reg(PLL0_MODE)) & ~(PLL_UPDATE_BYPASS | 7u)) | (found_pll0_mode & (PLL_UPDATE_BYPASS | 7u)));
	pll0_switched = false;
	say("camera: camcc's PLL0 off, as it was found (mode %#x, L %#x, opmode %#x)\n", rd(camcc_reg(PLL0_MODE)),
	    rd(camcc_reg(PLL0_L)), rd(camcc_reg(PLL0_OPMODE)));
}

static bool
titan_up(void)
{
	unsigned found_on = 0;

	if (broken || !camera_ahb_up() || !gdsc_up() || !pll0_up()) {
		return false;
	}
	titan_switched = true;
	for (unsigned i = 0; i < TITAN_RCGS; i++) {
		struct titan_rcg *r = &titan_rcgs[i];

		r->found = rd(camcc_reg(r->cmd + 4));
		r->set = true;
		if (!camcc_put(r->cmd + 4, (r->found & ~0x371fu) | r->source << 8 | r->divider, 0x371f) ||
		    !camcc_put(r->cmd, rd(camcc_reg(r->cmd)) | CMD_UPDATE, 0)) {
			return false;
		}
		if (!wait_bits(camcc_reg(r->cmd), CMD_UPDATE, 0, 500)) {
			say("camera: the RCG %s does not take its configuration (cmd %#x)\n", r->name, rd(camcc_reg(r->cmd)));
			give_up("an RCG that does not update");
			return false;
		}
	}
	for (unsigned i = 0; i < TITAN_BRANCHES; i++) {
		struct titan_branch *b = &titan_branches[i];
		vm_offset_t at = b->gcc ? gcc_reg(b->cbcr) : camcc_reg(b->cbcr);

		if (rd(at) & 1) {
			found_on++;
			continue;               /* (on already: left on at the end) */
		}
		b->set = true;
		if (b->gcc) {
			wr(at, rd(at) | 1);
			if (!(rd(at) & 1)) {
				say("camera: GCC's %s does not take its enable (%#x)\n", b->name, rd(at));
				give_up("a clock that does not read back");
				return false;
			}
		} else if (!camcc_put(b->cbcr, rd(at) | 1, 1)) {
			return false;
		}
		if (b->halt && !wait_bits(at, CBCR_OFF, 0, 500)) {
			say("camera: the branch %s does not come on (%#x)\n", b->name, rd(at));
			give_up("a clock that does not come on");
			return false;
		}
		spin_us(10);
	}
	if (!(rd(refgen + REFGEN_PWRDWN_CTRL5) & 1)) {
		refgen_switched = true;
		wr(refgen + REFGEN_PWRDWN_CTRL5, rd(refgen + REFGEN_PWRDWN_CTRL5) | 1);
		if (!(rd(refgen + REFGEN_PWRDWN_CTRL5) & 1)) {
			say("camera: refgen does not take its enable (%#x)\n", rd(refgen + REFGEN_PWRDWN_CTRL5));
			give_up("a regulator that does not read back");
			return false;
		}
		spin_us(5);
		say("camera: refgen on\n");
	} else {
		say("camera: refgen on already (PWRDWN_CTRL5 %#x): left so\n", rd(refgen + REFGEN_PWRDWN_CTRL5));
	}
	if (!rail_up(R_L9A)) {
		return false;
	}
	say("camera: the camera subsystem's clocks on: %u RCGs from PLL0, %u branches (%u of them on already)\n",
	    (unsigned)TITAN_RCGS, (unsigned)TITAN_BRANCHES, found_on);
	return true;
}

/* (the branches off, the RCGs back on their found source while PLL0 runs, PLL0, refgen; L9 is left to rail_down()) */
static void
titan_down(void)
{
	if (!titan_switched) {
		return;
	}
	for (unsigned i = TITAN_BRANCHES; i-- > 0;) {
		struct titan_branch *b = &titan_branches[i];
		vm_offset_t at = b->gcc ? gcc_reg(b->cbcr) : camcc_reg(b->cbcr);

		if (b->set) {
			wr(at, rd(at) & ~1u);
			if (b->halt) {
				(void)wait_bits(at, CBCR_OFF, CBCR_OFF, 500);
			}
			b->set = false;
		}
	}
	for (unsigned i = TITAN_RCGS; i-- > 0;) {
		struct titan_rcg *r = &titan_rcgs[i];

		if (r->set) {
			wr(camcc_reg(r->cmd + 4), r->found);
			wr(camcc_reg(r->cmd), rd(camcc_reg(r->cmd)) | CMD_UPDATE);
			(void)wait_bits(camcc_reg(r->cmd), CMD_UPDATE, 0, 500);
			r->set = false;
		}
	}
	pll0_down();
	if (refgen_switched) {
		wr(refgen + REFGEN_PWRDWN_CTRL5, rd(refgen + REFGEN_PWRDWN_CTRL5) & ~1u);
		refgen_switched = false;
		say("camera: refgen off again, as it was found\n");
	}
	titan_switched = false;
	say("camera: the camera subsystem's clocks off again, as they were found\n");
}

/* ---- the sensor's tables (hk_camera.c's "RWCTAB02", from fw/ of the small image) ------------------------------- */

struct ctab_head {
	char            magic[8];       /* "RWCTAB02" */
	uint32_t        slave, address_bytes, data_bytes, id_address, id, id_mask, lane_assign, modes;
};
struct ctab_mode {
	uint32_t        width, height, x, y, lanes, cphy, settle, pixel_clock, line_length, frame_length, fps_milli, data_type;
};
struct ctab_entry {
	uint16_t        address, data;  /* (a power sequence's: kind, value -- MCLK in kHz) */
	uint8_t         address_bytes, data_bytes, operation, zero;
	uint32_t        delay_us;
};

static const struct ctab_head *ctab;
static const uint8_t *ctab_power, *ctab_tables;
static uint64_t ctab_size;

static bool
ctab_load(void)
{
	const struct ctab_mode *modes;
	const uint8_t *at, *end;

	ctab = (const struct ctab_head *)hk_wifi_file(hw->tab, &ctab_size);
	if (ctab == NULL || ctab_size < sizeof(*ctab) || memcmp(ctab->magic, "RWCTAB02", 8) != 0 || ctab->modes == 0 ||
	    ctab->modes > 16) {
		say("camera: no %s (RWCTAB02) in fw/ of the small image (tools/camsensor.py makes it of the sensor's module file)\n",
		    hw->tab);
		ctab = NULL;
		return false;
	}
	end = (const uint8_t *)ctab + ctab_size;
	modes = (const struct ctab_mode *)(ctab + 1);
	ctab_power = (const uint8_t *)(modes + ctab->modes);
	at = ctab_power;
	for (int p = 0; p < 2; p++) {
		if (at + 12 > end) {
			ctab = NULL;
			return false;
		}
		at += 12 + *(const uint32_t *)(at + 8) * sizeof(struct ctab_entry);
	}
	ctab_tables = at;
	if (ctab_tables + 4 > end) {
		ctab = NULL;
		return false;
	}
	say("camera: the %s sensor's tables: slave %#x, chip ID %#x at %#x, lanes %#x, %u modes, %u tables\n", hw->name, ctab->slave,
	    ctab->id, ctab->id_address, ctab->lane_assign, ctab->modes, *(const uint32_t *)ctab_tables);
	return true;
}

static const struct ctab_entry *
ctab_block(const uint8_t *at, uint32_t blocks, const char *name, uint32_t *count)
{
	const uint8_t *end = (const uint8_t *)ctab + ctab_size;

	for (uint32_t t = 0; t < blocks && at + 12 <= end; t++) {
		uint32_t n = *(const uint32_t *)(at + 8);

		if (strncmp((const char *)at, name, 8) == 0 && at + 12 + n * sizeof(struct ctab_entry) <= end) {
			*count = n;
			return (const struct ctab_entry *)(at + 12);
		}
		at += 12 + n * sizeof(struct ctab_entry);
	}
	*count = 0;
	return NULL;
}

/* ---- the sensor's power ------------------------------------------------------------------------------------------ */

static bool lines_driven;

/* A line of the sensor's at a level (a TLMM pin: an output with its active state's drive and pull, read back) */
static bool
line_set(const struct line *l, bool high)
{
	uint32_t cfg, io, want;

	if (l->kind == LINE_NONE) {
		return true;
	}
	if (!lines_driven || (l->kind == LINE_PMIC && !pgpio_changed)) {
		/* (a power-down before the power-up set the lines up: nothing of this one's was driven, nothing to lower) */
		if (high) {
			give_up("a line raised before the power-up set it up");
		}
		return !high;
	}
	if (l->kind == LINE_PMIC) {
		return pgpio_level(high);
	}
	if (!take_pin(l->pin)) {
		return false;
	}
	/*
	 * Pull, function 0, drive and output enable as the active state has them, the level first (as
	 * instantnoodle_gpio_output()); the register's other bits as they are, as Linux's read-modify-write leaves
	 * them -- but for an eGPIO's ownership. gpio146-179 are eGPIOs (pinctrl-sm8250.c's egpio, Linux since
	 * 2025-09): bit 11 set says a pin is one, and while its bit 12 is clear the pin is the low-power island's and
	 * the TLMM's level and output enable do not reach the pad. Linux's msm_pinmux_set_mux() sets bit 12 when it
	 * muxes one to function 0, as cam_default's gpio156 is; so does this, and the found register (bit 12 clear)
	 * gives the pin back at the end. gpio156 (the wide's CAM_VANA2) and gpio159 (the ultra-wide's CAM_VAF) read
	 * 0x801 at cam=1, and before this the wide sensor stayed silent while its EEPROM and lens motor answered
	 * (run 080458): its 2.8 V never came. Then the pad itself, read back by its input: it follows the level.
	 */
	want = (instantnoodle_gpio_cfg(l->pin) & ~0x3ffu) | l->pull | (uint32_t)(l->ma / 2 - 1) << 6 | 1u << 9;
	if (want & TLMM_EGPIO_PRESENT) {
		want |= TLMM_EGPIO_ENABLE;
	}
	instantnoodle_gpio_restore(l->pin, want, high ? 2 : 0);
	cfg = instantnoodle_gpio_cfg(l->pin);
	io = instantnoodle_gpio_io(l->pin);
	if (cfg != want || ((io >> 1) & 1) != (high ? 1u : 0u)) {
		say("camera: gpio%u does not read back (cfg %#x, io %#x)\n", l->pin.pin, cfg, io);
		give_up("a pin that does not read back");
		return false;
	}
	spin_us(10);
	io = instantnoodle_gpio_io(l->pin);
	if ((io & 1) != (high ? 1u : 0u)) {
		say("camera: gpio%u is driven %s, but its pad reads %u (cfg %#x)\n", l->pin.pin, high ? "high" : "low", io & 1, cfg);
		give_up("a pad that does not follow its pin");
		return false;
	}
	return true;
}

static void
sensor_delay(uint32_t us)
{
	if (us >= 1000) {
		wait_ms(us / 1000);
		us %= 1000;
	}
	spin_us(us);
}

/*
 * One step of the module file's power sequence, as cam_sensor_util.c's power-up and power-down take it: a supply's
 * regulator and then its pin (if the device tree gives the kind one) on, or off and then the pin low; a pin's kind
 * to the step's level; MCLK on at its rate, or off. (`up': part of the power-up, where a fault stops it.)
 */
static bool
power_step(const struct ctab_entry *e, bool up)
{
	unsigned kind = e->address;
	const struct line *l = kind < K_KINDS ? &hw->line[kind] : NULL;
	unsigned r = kind < K_KINDS ? hw->rail[kind] : R_NONE;

	switch (kind) {
	case K_MCLK:
		if (!up) {
			mclk_down();
			break;
		}
		if (e->data != 19200 && e->data != 24000) {
			give_up("an MCLK rate not known here");
			return false;
		}
		if (e->data == 24000 && !pll2) {
			say("camera: the module file's MCLK is 24 MHz, from camcc's PLL2, which is started only with cam_pll2=1: "
			    "19.2 MHz for now\n");
		}
		if (!gdsc_up() || !mclk_up(e->data == 24000 && pll2 ? 24000 : 19200)) {
			return false;
		}
		break;
	case K_VANA: case K_VDIG: case K_VIO: case K_VAF: case K_VAF_PWDM: case K_CUSTOM_REG1: case K_CUSTOM_REG2:
		if (r == R_NONE && l->kind == LINE_NONE) {
			say("camera: a power step of kind %s, which the device tree does not give the %s sensor\n", kind_names[kind], hw->name);
			give_up("a supply the device tree does not give this sensor");
			return false;
		}
		if (up) {
			if ((r != R_NONE && !rail_up(r)) || (!broken && !line_set(l, true))) {
				return false;
			}
		} else {
			rail_down(r);
			(void)line_set(l, false);
		}
		break;
	case K_RESET: case K_STANDBY: case K_CUSTOM_GPIO1: case K_CUSTOM_GPIO2:
		if (l->kind == LINE_NONE) {
			say("camera: a power step of kind %s, which the device tree does not give the %s sensor\n", kind_names[kind], hw->name);
			give_up("a pin the device tree does not give this sensor");
			return false;
		}
		if (up && broken) {
			return false;
		}
		if (!line_set(l, e->data != 0) && up) {
			return false;
		}
		break;
	default:
		say("camera: a power step of kind %u, which this does not know\n", kind);
		give_up("a power sequence not understood");
		return false;
	}
	/* (capped at 100 ms, as upstream hakios.camera.c caps redwood's: a corrupt table must not keep the rails up for minutes) */
	sensor_delay(e->delay_us > 100000 ? 100000 : e->delay_us);
	return true;
}

/* What cam_sensor_util's power-up does before the sequence: its pins as the active states have them, the outputs low */
static bool
lines_prepare(void)
{
	for (unsigned k = 0; k < K_KINDS; k++) {
		const struct line *l = &hw->line[k];

		if (l->kind == LINE_PMIC) {
			lines_driven = true;
			if (!pgpio_prepare()) {
				return false;
			}
		} else if (l->kind == LINE_TLMM) {
			lines_driven = true;
			if (!line_set(l, false)) {
				return false;
			}
		}
	}
	return true;
}

static bool
power_up(void)
{
	uint32_t count = 0;
	const struct ctab_entry *e = ctab != NULL ? ctab_block(ctab_power, 2, "pwrup", &count) : NULL;

	if (e == NULL) {
		give_up("no power sequence");
		return false;
	}
	powered = true;
	if (!camera_ahb_up() || !lines_prepare()) {
		return false;
	}
	for (uint32_t i = 0; i < count; i++) {
		if (!power_step(&e[i], true)) {
			return false;
		}
	}
	say("camera: the %s sensor powered up (%u steps)\n", hw->name, count);
	return true;
}

static void smmu_ife_down(void);
static void ife_dma_stop(void);
static void act_park(void);
static unsigned smmu_matches_taken;
static bool smmu_ctx_written;

/* Everything that was switched on, off again: the module file's power-down, then the rest as it was found. */
static void
power_down(void)
{
	uint32_t count = 0;
	const struct ctab_entry *e = ctab != NULL ? ctab_block(ctab_power, 2, "pwrdn", &count) : NULL;
	bool was = powered || mclk_switched || gdsc_switched || gdsc_clk_switched || ahb_switched || lines_driven || pgpio_changed ||
	    found_pins_n != 0 || titan_switched || pll0_switched || smmu_matches_taken != 0 || smmu_ctx_written;

	for (unsigned r = R_L1; r < R_RAILS; r++) {
		was |= rail_on[r];
	}
	if (!was) {
		return;
	}
	say("camera: powering down\n");
	ife_dma_stop();                 /* (no DMA left before the SMMU or the memory are given back) */
	act_park();                     /* (the lens taken back before its power goes) */
	for (uint32_t i = 0; e != NULL && i < count; i++) {
		(void)power_step(&e[i], false);
	}
	/* (whatever the sequence did not reach; PLL2 after the MCLK's RCG is back on the crystal) */
	mclk_down();
	pll2_down();
	smmu_ife_down();
	titan_down();
	for (unsigned k = 0; lines_driven && k < K_KINDS; k++) {
		if (hw->line[k].kind == LINE_TLMM || (hw->line[k].kind == LINE_PMIC && pgpio_changed)) {
			(void)line_set(&hw->line[k], false);
		}
	}
	for (unsigned r = R_RAILS; r-- > R_L1;) {
		rail_down(r);
	}
	gdsc_down();
	pgpio_give_back();
	give_pins_back();
	camera_ahb_down();
	lines_driven = false;
	powered = false;
	say("camera: powered down\n");
}

static const char *
bus_error(int err)
{
	return err == ENXIO ? "no acknowledgement" : err == ETIMEDOUT ? "SCL held low" : "a bus error";
}

/* cam_scan=1: the bus of a sensor that does not answer, while it is powered up (the header comment) */
static void
scan_bus(void)
{
	unsigned found = 0;
	uint8_t byte;

	say("camera: %s, every address read once (one byte, no register):\n", hw->bus->name);
	for (unsigned a = 0x08; a <= 0x77; a++) {
		int err = instantnoodle_i2c_read_at(hw->bus, (uint8_t)a, NULL, 0, &byte, 1);

		if (err == 0) {
			say("camera:   %#04x answers (reads %#04x)\n", a, byte);
			found++;
		} else if (err != ENXIO) {
			say("camera:   the scan stopped at %#04x: %s (SDA %u, SCL %u)\n", a, bus_error(err),
			    instantnoodle_gpio_level(hw->bus->sda), instantnoodle_gpio_level(hw->bus->scl));
			break;
		}
	}
	say("camera: %s: %u address(es) answer\n", hw->bus->name, found);
}

static bool
read_chip_id(void)
{
	uint8_t where[2] = { (uint8_t)(ctab->id_address >> 8), (uint8_t)ctab->id_address }, id[4] = { 0, 0, 0, 0 };
	unsigned out = ctab->address_bytes == 2 ? 2 : 1, in = ctab->data_bytes >= 1 && ctab->data_bytes <= 4 ? ctab->data_bytes : 2;
	uint8_t address = (uint8_t)(ctab->slave >> 1);  /* (the module file's is the 8-bit one) */
	uint32_t chip = 0;
	unsigned go;
	int err = 0;
	bool sda0, scl0;

	if (!take_pin(hw->bus->sda) || !take_pin(hw->bus->scl)) {
		return false;
	}
	/* the lines before the first start, let go as the bus lets them go (inputs, pulled up): 1 unless something holds them */
	instantnoodle_gpio_input(hw->bus->sda, hw->bus->pull);
	instantnoodle_gpio_input(hw->bus->scl, hw->bus->pull);
	spin_us(100);
	sda0 = instantnoodle_gpio_level(hw->bus->sda);
	scl0 = instantnoodle_gpio_level(hw->bus->scl);
	for (go = 0; go < 20; go++) {
		err = instantnoodle_i2c_read_at(hw->bus, address, out == 2 ? where : where + 1, out, id, in);
		if (err == 0) {
			break;
		}
		wait_ms(2);
	}
	if (go == 20) {
		/* (the lines before the first start and as the bus leaves them: let go, so high unless something holds them) */
		say("camera: the %s sensor does not answer at %#04x on %s: %s (SDA %u, SCL %u before it; %u, %u after)\n",
		    hw->name, address, hw->bus->name, bus_error(err), sda0, scl0, instantnoodle_gpio_level(hw->bus->sda),
		    instantnoodle_gpio_level(hw->bus->scl));
		if (scan) {
			scan_bus();
		}
		return false;
	}
	if (go != 0) {
		say("camera: the %s sensor answered at %#04x after %u more goes (%u ms)\n", hw->name, address, go, go * 2);
	}
	for (unsigned i = 0; i < in; i++) {
		chip = chip << 8 | id[i];
	}
	say("camera: the %s sensor's chip ID on %s at %#04x: %#06x (%s)\n", hw->name, hw->bus->name, address, chip,
	    (chip & ctab->id_mask) == (ctab->id & ctab->id_mask) ? "as Android expects" : "NOT what Android expects");
	return (chip & ctab->id_mask) == (ctab->id & ctab->id_mask);
}

/*
 * ---- the frames' path, step 2 (cam_step=2): the sensor's packets counted by CSID-lite 0 ---------------------------------
 *
 * Linux's camss for SM8250, read as data: camss-vfe-480.c's vfe_global_reset() (VFE-lite 0, whose register space holds
 * CSID-lite 0), camss-csiphy-3ph-1-0.c's csiphy_reset() and csiphy_lanes_enable() with lane_regs_sm8250 and
 * csiphy_settle_cnt_calc() (its 300 MHz timer), camss-csid-gen2.c's csid_reset() and __csid_configure_rx(); the CSI2 RX
 * interrupt bits, the packet capture and the counters are the vendor's (cam_ife_csid_core.h, cam_ife_csid_lite480.h:
 * Linux has none). The front IMX471 (CSIPHY 2, four D-PHY lanes) in its module file's mode 1, 2328x1748 at 30 fps,
 * the slowest link (500 Mb/s a lane): its init table, the mode's, stream on; the RX's state read; stream off. No RDI
 * path is set up -- nothing is written to memory. Every register written is read back where it keeps what it is
 * given; CSIPHY 2 is reset again and the CSID's and VFE-lite's registers are put back as found at the end.
 */
#define VFE_LITE_RESET_CMD      0x0c            /* [1] the global reset (VFE-lite) */
#define VFE_LITE_IRQ_CMD        0x24            /* [0] clear */
#define VFE_LITE_IRQ_MASK0      0x28
#define VFE_LITE_IRQ_CLEAR0     0x34
#define VFE_LITE_IRQ_STATUS0    0x40
#define VFE_LITE_RESET_ACK      (1u << 17)
#define CSID_RST_STROBES        0x10
#define CSID_RX_IRQ_STATUS      0x20            /* [3:0] EOT and [7:4] SOT on data lanes 0-3, [8] long packet, [9] short packet, */
#define CSID_RX_IRQ_MASK        0x24            /* [14] ECC warning, [18:15] lane FIFO overflow, [19] CRC, [20] ECC errors, */
#define CSID_RX_IRQ_CLEAR       0x28            /* [22:21] VC/DT not mapped, [23] stream underflow, [24] unbounded frame */
#define CSID_TOP_IRQ_STATUS     0x70            /* [0] reset done */
#define CSID_TOP_IRQ_MASK       0x74
#define CSID_TOP_IRQ_CLEAR      0x78
#define CSID_IRQ_CMD            0x80            /* [0] clear */
#define CSID_RX_CFG0            0x100           /* [1:0] lanes - 1, [19:4] the lanes' assignment, [22:20] the PHY */
#define CSID_RX_CFG1            0x104           /* [0] ECC correction, [6] MISR */
#define CSID_RX_CAPTURE_CTRL    0x108           /* [0] long, [1] short packets captured; [9:4] long's DT, [14:10] its VC, [19:15] short's VC */
#define CSID_RX_SHORT_PKT0      0x128           /* [31:22] VC, [21:16] DT, [15:0] the frame or line number */
#define CSID_RX_LONG_PKT0       0x130           /* [15:0] word count, [21:16] DT, [31:22] VC */
#define CSID_RX_TOTAL_PKTS      0x160
#define CSID_RX_STATS_ECC       0x164
#define CSID_RX_TOTAL_CRC_ERR   0x168
#define CSIPHY_COMMON_CTRL(n)   (0x800 + 4 * (n))
#define CSIPHY_TIMER_HZ         300000000ull

enum { PHY_VALUE, PHY_SETTLE, PHY_SKIP };       /* lane_regs_sm8250's CSIPHY_DEFAULT_PARAMS, SETTLE_CNT_LOWER_BYTE, DNP_PARAMS */
static const struct {
	uint16_t        offset;
	uint8_t         value, kind;
} lane_regs_sm8250[] = {
	{ 0x0030, 0x00, PHY_VALUE }, { 0x0900, 0x05, PHY_VALUE }, { 0x0908, 0x10, PHY_VALUE }, { 0x0904, 0x00, PHY_VALUE },
	{ 0x0904, 0x07, PHY_VALUE }, { 0x0004, 0x0c, PHY_VALUE }, { 0x002c, 0x01, PHY_VALUE }, { 0x0034, 0x07, PHY_VALUE },
	{ 0x0010, 0x02, PHY_VALUE }, { 0x001c, 0x08, PHY_VALUE }, { 0x003c, 0xb8, PHY_VALUE }, { 0x0008, 0x10, PHY_SETTLE },
	{ 0x0000, 0x8d, PHY_VALUE }, { 0x000c, 0x00, PHY_SKIP }, { 0x0038, 0xfe, PHY_VALUE }, { 0x0014, 0x60, PHY_VALUE },
	{ 0x0028, 0x00, PHY_SKIP }, { 0x0024, 0x00, PHY_SKIP }, { 0x0800, 0x02, PHY_VALUE }, { 0x0884, 0x01, PHY_VALUE },
	{ 0x0730, 0x00, PHY_VALUE }, { 0x0c80, 0x05, PHY_VALUE }, { 0x0c88, 0x10, PHY_VALUE }, { 0x0c84, 0x00, PHY_VALUE },
	{ 0x0c84, 0x07, PHY_VALUE }, { 0x0704, 0x0c, PHY_VALUE }, { 0x072c, 0x01, PHY_VALUE }, { 0x0734, 0x07, PHY_VALUE },
	{ 0x0710, 0x02, PHY_VALUE }, { 0x071c, 0x08, PHY_VALUE }, { 0x073c, 0xb8, PHY_VALUE }, { 0x0708, 0x10, PHY_SETTLE },
	{ 0x0700, 0x80, PHY_VALUE }, { 0x070c, 0xa5, PHY_VALUE }, { 0x0738, 0x1f, PHY_VALUE }, { 0x0714, 0x60, PHY_VALUE },
	{ 0x0728, 0x04, PHY_VALUE }, { 0x0724, 0x00, PHY_SKIP }, { 0x0800, 0x02, PHY_VALUE }, { 0x0884, 0x01, PHY_VALUE },
	{ 0x0230, 0x00, PHY_VALUE }, { 0x0a00, 0x05, PHY_VALUE }, { 0x0a08, 0x10, PHY_VALUE }, { 0x0a04, 0x00, PHY_VALUE },
	{ 0x0a04, 0x07, PHY_VALUE }, { 0x0204, 0x0c, PHY_VALUE }, { 0x022c, 0x01, PHY_VALUE }, { 0x0234, 0x07, PHY_VALUE },
	{ 0x0210, 0x02, PHY_VALUE }, { 0x021c, 0x08, PHY_VALUE }, { 0x023c, 0xb8, PHY_VALUE }, { 0x0208, 0x10, PHY_SETTLE },
	{ 0x0200, 0x8d, PHY_VALUE }, { 0x020c, 0x00, PHY_SKIP }, { 0x0238, 0xfe, PHY_VALUE }, { 0x0214, 0x60, PHY_VALUE },
	{ 0x0228, 0x00, PHY_SKIP }, { 0x0224, 0x00, PHY_SKIP }, { 0x0800, 0x02, PHY_VALUE }, { 0x0884, 0x01, PHY_VALUE },
	{ 0x0430, 0x00, PHY_VALUE }, { 0x0b00, 0x05, PHY_VALUE }, { 0x0b08, 0x10, PHY_VALUE }, { 0x0b04, 0x00, PHY_VALUE },
	{ 0x0b04, 0x07, PHY_VALUE }, { 0x0404, 0x0c, PHY_VALUE }, { 0x042c, 0x01, PHY_VALUE }, { 0x0434, 0x07, PHY_VALUE },
	{ 0x0410, 0x02, PHY_VALUE }, { 0x041c, 0x08, PHY_VALUE }, { 0x043c, 0xb8, PHY_VALUE }, { 0x0408, 0x10, PHY_SETTLE },
	{ 0x0400, 0x8d, PHY_VALUE }, { 0x040c, 0x00, PHY_SKIP }, { 0x0438, 0xfe, PHY_VALUE }, { 0x0414, 0x60, PHY_VALUE },
	{ 0x0428, 0x00, PHY_SKIP }, { 0x0424, 0x00, PHY_SKIP }, { 0x0800, 0x02, PHY_VALUE }, { 0x0884, 0x01, PHY_VALUE },
	{ 0x0630, 0x00, PHY_VALUE }, { 0x0c00, 0x05, PHY_VALUE }, { 0x0c08, 0x10, PHY_VALUE }, { 0x0c04, 0x00, PHY_VALUE },
	{ 0x0c04, 0x07, PHY_VALUE }, { 0x0604, 0x0c, PHY_VALUE }, { 0x062c, 0x01, PHY_VALUE }, { 0x0634, 0x07, PHY_VALUE },
	{ 0x0610, 0x02, PHY_VALUE }, { 0x061c, 0x08, PHY_VALUE }, { 0x063c, 0xb8, PHY_VALUE }, { 0x0608, 0x10, PHY_SETTLE },
	{ 0x0600, 0x8d, PHY_VALUE }, { 0x060c, 0x00, PHY_SKIP }, { 0x0638, 0xfe, PHY_VALUE }, { 0x0614, 0x60, PHY_VALUE },
	{ 0x0628, 0x00, PHY_SKIP }, { 0x0624, 0x00, PHY_SKIP }, { 0x0800, 0x02, PHY_VALUE }, { 0x0884, 0x01, PHY_VALUE },
};

/*
 * cam_step=5 (docs/TODO.md T07's 3c): CSID-lite 0's RDI 0 and VFE-lite 0's write master 0 (camss-csid-gen2.c's
 * __csid_configure_rdi_stream(), camss-vfe-480.c's vfe_wm_start() and vfe_reg_update(), RDI_WM(0) and RDI_COMP_GROUP(0) on a
 * VFE-lite) -- but for one frame: no address step from frame to frame (FRAME_INCR 0), one frame in 32 written, and the write
 * master off and RDI 0 halted at the first buffer done.
 */
#define VFE_LITE_REG_UPDATE     0x20            /* [0] RDI 0's registers taken at the next frame */
#define VFE_LITE_BUS_TOP        (1u << 4)
#define VFE_BUS_CGC_OVERRIDE    0x1a08
#define VFE_BUS_IRQ_MASK0       0x1a18          /* [0] RDI 0's register update done, [4] its group's buffer done */
#define VFE_BUS_IRQ_CLEAR0      0x1a20
#define VFE_BUS_IRQ_STATUS0     0x1a28
#define VFE_BUS_IRQ_CLEAR_ALL   0x1a30
#define VFE_BUS_TEST_CTRL       0x1adc
#define VFE_WM0_CFG             0x1c00          /* [0] on, [17:16] 1 MIPI raw */
#define VFE_WM0_IMAGE_ADDR      0x1c04
#define VFE_WM0_FRAME_INCR      0x1c08
#define VFE_WM0_IMAGE_CFG_0     0x1c0c
#define VFE_WM0_IMAGE_CFG_2     0x1c14          /* the stride */
#define VFE_WM0_PACKER_CFG      0x1c18
#define VFE_WM0_IRQ_SUB_PERIOD  0x1c30
#define VFE_WM0_IRQ_SUB_PATTERN 0x1c34
#define VFE_WM0_DROP_PERIOD     0x1c38
#define VFE_WM0_DROP_PATTERN    0x1c3c
#define VFE_WM0_BURST_LIMIT     0x1c64
#define CSID_RDI0_IRQ_STATUS    0x30            /* [12] start of frame, [9] end, [2] FIFO overflow, [15] CCIF violation */
#define CSID_RDI0_IRQ_MASK      0x34
#define CSID_RDI0_IRQ_CLEAR     0x38
#define CSID_RDI0_CFG0          0x200           /* [0] byte counter, [1] format measure, [2] timestamp, [15:12] decode format (0xf
	                                         * payload only), [21:16] DT, [26:22] VC, [31] on */
#define CSID_RDI0_CFG1          0x204
#define CSID_RDI0_CTRL          0x208           /* [1:0] 0 halt, 1 resume, at a frame's boundary */

static const uint32_t rdi_regs[] = { CSID_RDI0_IRQ_MASK, CSID_RDI0_CFG0, CSID_RDI0_CFG1, CSID_RDI0_CTRL, 0x20c, 0x210, 0x214, 0x218, 0x224,
	                             0x228, 0x22c, 0x230 };
static const uint32_t wm_regs[] = { VFE_BUS_CGC_OVERRIDE, VFE_BUS_TEST_CTRL, VFE_BUS_IRQ_MASK0, VFE_WM0_CFG, VFE_WM0_IMAGE_ADDR,
	                            VFE_WM0_FRAME_INCR, VFE_WM0_IMAGE_CFG_0, VFE_WM0_IMAGE_CFG_2, VFE_WM0_PACKER_CFG, VFE_WM0_IRQ_SUB_PERIOD,
	                            VFE_WM0_IRQ_SUB_PATTERN, VFE_WM0_DROP_PERIOD, VFE_WM0_DROP_PATTERN, VFE_WM0_BURST_LIMIT };
#define RDI_REGS                (sizeof(rdi_regs) / sizeof(rdi_regs[0]))
#define WM_REGS                 (sizeof(wm_regs) / sizeof(wm_regs[0]))

/*
 * The write master stopped and the bus idle: its enable off and taken, RDI 0 halted, VFE-lite 0 reset and its
 * acknowledgement polled. From any context (no lock, no sleep: a panic's way back to the loader takes it too), before
 * the SMMU or the frame's memory are given back -- the loader or the next kernel would otherwise find the IFE writing.
 */
static bool ife_dma_live;

static void
ife_dma_stop(void)
{
	bool idle;

	if (!ife_dma_live) {
		return;
	}
	wr(ife_lite0 + VFE_WM0_CFG, 0);
	wr(ife_lite0 + VFE_LITE_REG_UPDATE, 1);
	wr(ife_lite0 + CSID_LITE0 + CSID_RDI0_CTRL, 0);
	wr(ife_lite0 + VFE_LITE_IRQ_MASK0, VFE_LITE_RESET_ACK);
	wr(ife_lite0 + VFE_LITE_RESET_CMD, 1u << 1);
	idle = wait_bits(ife_lite0 + VFE_LITE_IRQ_STATUS0, VFE_LITE_RESET_ACK, VFE_LITE_RESET_ACK, 50000);
	ife_dma_live = false;
	say("camera: IFE-lite 0's write master off, RDI 0 halted, VFE-lite 0 reset%s\n", idle ? "" : ": ITS RESET NOT ACKNOWLEDGED");
}

/* (the frame's memory and the SMMU: further down, with step 4's) */
static bool ife_memory_alloc(uint32_t frame_bytes, unsigned places);
/*
 * cam=3 cam_step=5: memory for one frame in one piece, taken at boot (instantnoodle_camera_init()) while the system's memory
 * still is in large pieces -- at the step's time 19.9 MB in one piece for the ultra-wide IMX481's 4656x3496 frame was not
 * there (run 142004), the wide one's 14.7 MB was. ife_memory_alloc() uses it for a frame it holds.
 */
#define IFE_EARLY_BYTES         (24ull << 20)
static vm_offset_t ife_early;
static uint64_t ife_early_bytes;
static bool ife_memory_early;           /* ife_memory is that piece */
static bool smmu_ife_up(void);
static uint64_t ife_canaries_hit(void);
static void ife_memory_free(void);
static void ife_hand_over(const struct ctab_mode *mode, uint32_t stride);
static uint32_t ife_place_address(unsigned place);

/* a camera block's register: written, read back (check: the bits it keeps; 0 for a command or a status's clear) */
static bool
blk_put(vm_offset_t base, const char *what, uint32_t offset, uint32_t value, uint32_t check)
{
	if (broken) {
		return false;
	}
	wr(base + offset, value);
	if (check != 0 && (rd(base + offset) & check) != (value & check)) {
		say("camera: %s %#x <- %#x, but it reads %#x\n", what, offset, value, rd(base + offset));
		give_up("a camera block's register that did not read back");
		return false;
	}
	return true;
}

/* one of the module file's tables to the sensor, write by write (the sensor's bus already taken, read_chip_id()) */
/*
 * Boot-arg cam_cci_burst=1: consecutive registers of a table (16-bit addresses, a byte each, no delay between them) in one
 * write of up to CCI_BURST bytes, the sensor's own address counting on (SMIA's sequential write, which the Sony sensors
 * here have): the wide IMX586's init table is 1709 writes, 98 such runs, and at about 1.1 ms a write (this bit-banged
 * bus's; run 152342: 2220 ms from the ask to the IMX586's first frame, 616 ms to the IMX481's with its 353) its tables were
 * most of a change of cameras. The tables' order and values are kept. Each table's time is said either way. Run 152922
 * (cam_cci_burst=1): the IMX586's init 1709 writes in 74, 643-684 ms; the IMX481's 206 in 86, 119 ms; the IMX471's 155 in
 * 36, 109 ms; every sensor's frames as before (the GPU's picture 0.014 from the processor's); the first frame of the
 * Back Camera 981 ms after the ask (2896 before), the ultra-wide's 489 (616), back to the wide 1084 (2220). Since then the
 * default; cam_cci_burst=0 writes a register at a time.
 */
#define CCI_BURST               64
static uint32_t cci_burst = 1;          /* boot-arg cam_cci_burst (0: a write a register, as before) */

static bool
sensor_table(const char *name)
{
	uint32_t count = 0, sent = 0;
	const struct ctab_entry *e = ctab_block(ctab_tables + 4, *(const uint32_t *)ctab_tables, name, &count);
	uint64_t began = mach_absolute_time(), ns;

	if (e == NULL || count == 0) {
		say("camera: the %s sensor has no table %s\n", hw->name, name);
		give_up("a sensor table that is not there");
		return false;
	}
	for (uint32_t i = 0; i < count && !broken; i++) {
		uint8_t bytes[2 + CCI_BURST];
		unsigned n = 0, run = 1;
		int err;

		if (e[i].operation != 0 || e[i].address_bytes < 1 || e[i].address_bytes > 2 || e[i].data_bytes < 1 ||
		    e[i].data_bytes > 2) {
			say("camera: %s[%u]: operation %u, %u/%u bytes: not a write known here\n", name, i, e[i].operation,
			    e[i].address_bytes, e[i].data_bytes);
			give_up("a sensor table not understood");
			return false;
		}
		if (e[i].address_bytes == 2) {
			bytes[n++] = (uint8_t)(e[i].address >> 8);
		}
		bytes[n++] = (uint8_t)e[i].address;
		if (e[i].data_bytes == 2) {
			bytes[n++] = (uint8_t)(e[i].data >> 8);
		}
		bytes[n++] = (uint8_t)e[i].data;
		/* (the run: the next entries at the next addresses, plain one-byte writes, no delay before them) */
		while (cci_burst != 0 && e[i].address_bytes == 2 && e[i].data_bytes == 1 && i + run < count && run < CCI_BURST &&
		    e[i + run - 1].delay_us == 0 && e[i + run].operation == 0 && e[i + run].address_bytes == 2 &&
		    e[i + run].data_bytes == 1 && e[i + run].address == e[i].address + run) {
			bytes[n++] = (uint8_t)e[i + run].data;
			run++;
		}
		err = instantnoodle_i2c_write(hw->bus, (uint8_t)(ctab->slave >> 1), bytes, n);
		sent++;
		if (err != 0) {
			say("camera: %s[%u] (%#x <- %#x%s) not taken by the sensor: %s\n", name, i, e[i].address, e[i].data,
			    run > 1 ? " and the next ones" : "", bus_error(err));
			give_up("a sensor write not acknowledged");
			return false;
		}
		i += run - 1;
		if (e[i].delay_us != 0) {
			sensor_delay(e[i].delay_us > 100000 ? 100000 : e[i].delay_us);
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
	say("camera: the %s sensor's %s table written (%u writes in %u, %llu ms)\n", hw->name, name, count, sent, ns / 1000000);
	return true;
}

/* a 16-bit register of the sensor's (SMIA: its address and value high byte first) */
static bool
sensor_reg16(uint16_t reg, uint16_t value)
{
	uint8_t bytes[4] = { (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(value >> 8), (uint8_t)value };
	int err = instantnoodle_i2c_write(hw->bus, (uint8_t)(ctab->slave >> 1), bytes, 4);

	if (err != 0) {
		say("camera: the %s sensor's %#06x <- %#x not taken: %s\n", hw->name, reg, value, bus_error(err));
		give_up("a sensor write not acknowledged");
		return false;
	}
	return true;
}

/*
 * ---- the wide IMX586's lens: its motor (boot-arg cam_focus=N) ------------------------------------------------------------
 *
 * An LC898217XC (ON Semiconductor's closed-loop voice-coil driver), as the module file names it (com.qti.sensormodule.
 * semco_imx586.bin: actuatorName "imx586_lc898217xc", its actuatorDriver): slave 0xe4 (8-bit; 0x72 here) on the sensor's own
 * bus (the DT's actuator@0: cci-master 0; its cam_vaf the sensor's, PM8009 L5 at 2.8 V, which the sensor's power sequence
 * switches on), 10-bit codes, the position a 16-bit word at 0x84 (high byte first); its initialisation 0xe0 <- 1 and 1 ms,
 * then 0xb3 polled until it reads 0 (1 ms apart); the region 400 steps of 2.2 codes from 50, so codes 50 (infinity) to 930
 * (macro) -- no other code is ever written. cam_focus=1 only reads (0xf0, 0xb3, 0xe0 and the position: does it answer,
 * and with what); 2 drives it: HK_CAMERA_FOCUS's 0..1023 (sys/hk_camera.h) made a code of that range, a move of at most an
 * eighth of it each frame (a frame apart, 33 ms and more: never thrown from one end to the other), every position read
 * back. Before the sensor's power goes, the lens is taken back to 50 the same way (parked), on the run's end and a panic's
 * way out as well -- there by hand, spinning, no lock taken (act_leaving_write()) -- so that it is never dropped onto its
 * stop. Observed with cam_focus=1 (run 134921), before its start: 0xf0 0x72 (taken for its ID: driven only if so), 0xb3 0x20
 * (busy), 0xe0 0, the position 0x200 -- the middle of its travel, where a start would send the lens: 50 is written there
 * first. Run 135326 (cam_focus=2): started (0xb3 0 after 11-37 ms), every position read back as written, the contrast
 * search's 432-492 sharp (the screenshot's braid and edges), parked from 431 and from 339 to 50 when the camera stopped;
 * since then the default (cam_focus=0 leaves the lens alone). [the way-out park by hand: to be observed]
 */
#define ACT_ADDR                0x72
#define ACT_POSITION            0x84
#define ACT_CODE_MIN            50
#define ACT_CODE_MAX            930
#define ACT_MOVE_MAX            ((ACT_CODE_MAX - ACT_CODE_MIN) / 8)

static uint32_t focus_level = 2;        /* boot-arg cam_focus (0: the lens not touched; 1: read only) */
static bool act_ready;                  /* initialised and driven: parked before the power goes */
static uint32_t act_code;               /* the code last written */
static uint32_t act_target = ACT_CODE_MIN;      /* where it is going, a move a frame */
/*
 * The chip being driven (act_up()): the wide one's LC898217XC (ACT_ADDR, its position at ACT_POSITION as it is, parked at
 * 50), or the ultra-wide one's AK7374 (ACT_ULTRA_ADDR, below: its position at 0x00 shifted 6, parked at 0 -- where it was
 * found, its rest -- and then put to sleep).
 */
static uint8_t act_addr = ACT_ADDR, act_reg = ACT_POSITION;
static unsigned act_shift;
static uint32_t act_park_code = ACT_CODE_MIN;
static bool act_sleeps;
static unsigned act_mismatches;

static int
act_read(uint8_t reg, uint8_t *buf, unsigned n)
{
	return instantnoodle_i2c_read_at(hw->bus, act_addr, &reg, 1, buf, n);
}

/* one position, read back (what reads back otherwise is said, the first few times: a closed loop may read its own) */
static bool
act_write_code(uint32_t code)
{
	uint32_t word = code << act_shift;
	uint8_t bytes[3] = { act_reg, (uint8_t)(word >> 8), (uint8_t)word }, back[2] = { 0, 0 };
	int err;

	if (code < MIN(ACT_CODE_MIN, act_park_code) || code > ACT_CODE_MAX) {
		/*
		 * (never: every caller keeps to the range, or on the way to where the chip was found -- the AK7374's rest at 0,
		 * below the range: a park's last move from 146 to 36 was refused here once, run 152342, the lens left at 146)
		 */
		say("camera: the lens's position %u refused (out of %u to %u)\n", code, MIN(ACT_CODE_MIN, act_park_code), ACT_CODE_MAX);
		return false;
	}
	err = instantnoodle_i2c_write(hw->bus, act_addr, bytes, 3);
	if (err != 0) {
		say("camera: the lens's position %u not taken: %s\n", code, bus_error(err));
		act_ready = false;
		return false;
	}
	act_code = code;
	err = act_read(act_reg, back, 2);
	if ((err != 0 || ((uint32_t)back[0] << 8 | back[1]) != word) && act_mismatches++ < 5) {
		say("camera: the lens's position %u reads back %#x (%s)\n", code, (uint32_t)back[0] << 8 | back[1], err ? bus_error(err) : "read");
	}
	return true;
}

/* a move toward act_target of at most ACT_MOVE_MAX (ios_session(): one a frame, at its end) */
static void
act_step(void)
{
	uint32_t to = act_target;

	if (!act_ready || act_code == to) {
		return;
	}
	to = act_code < to ? MIN(to, act_code + ACT_MOVE_MAX) : MAX(to, act_code - ACT_MOVE_MAX);
	(void)act_write_code(to);
}

/* HK_CAMERA_FOCUS's 0 (the furthest) .. 1023 to a code, and back */
static uint32_t
act_code_of(uint32_t position)
{
	return ACT_CODE_MIN + MIN(position, 1023u) * (ACT_CODE_MAX - ACT_CODE_MIN) / 1023;
}

static uint32_t
act_position(void)
{
	return act_ready ? (MAX(act_code, ACT_CODE_MIN) - ACT_CODE_MIN) * 1023 / (ACT_CODE_MAX - ACT_CODE_MIN) : HK_CAMERA_NO_FOCUS;
}

/*
 * The ultra-wide IMX481's lens motor: an AK7374 (AKM's), as its module file names it (actuatorName "imx481_ak7374"): slave
 * 0x18 (8-bit; 0x0c here) on the sensor's bus, CCI 1 master 1; 10-bit codes, the position a 16-bit word at 0x00 with the code
 * in its top ten bits (shifted 6); its initialisation 0x02 <- 0 (active); the region as the IMX586's, codes 50 to 930. Its
 * supply is the sensor's CUSTOM_GPIO1, GPIO 159 ("CAM_VAF"), which the sensor's power sequence raises. Boot-arg
 * cam_focus_ultra=1 reads it only (0x02, 0x00-0x01, 0x03: does it answer, and with what); run 143558: 0x02 0x40 (asleep), the
 * position 0 (its rest), 0x03 0xe. cam_focus_ultra=2 drives it: woken where it rests (0x02 <- 0, the position still 0: no
 * move), then as the wide one's, a move of at most an eighth of the range a frame, every position read back; parked back to 0
 * in such moves and put to sleep again (0x02 <- 0x40) before its power goes, on the way out by hand.
 * Run 144927 (cam_focus_ultra=2, camera_test): woken, 0 -> 50 read back as written, parked 50 -> 0 and asleep at the close;
 * since then the default (cam_focus_ultra=0 leaves it alone). [its contrast search in Camera: to be observed]
 */
#define ACT_ULTRA_ADDR          0x0c
static uint32_t focus_ultra = 2;        /* boot-arg cam_focus_ultra (0: not touched; 1: read only) */

static void
act_ultra_up(void)
{
	uint8_t control = 0, at[2] = { 0, 0 }, three = 0, reg;
	int e_control, e_at, e_three;
	uint32_t found;

	reg = 0x02;
	e_control = instantnoodle_i2c_read_at(hw->bus, ACT_ULTRA_ADDR, &reg, 1, &control, 1);
	reg = 0x00;
	e_at = instantnoodle_i2c_read_at(hw->bus, ACT_ULTRA_ADDR, &reg, 1, at, 2);
	reg = 0x03;
	e_three = instantnoodle_i2c_read_at(hw->bus, ACT_ULTRA_ADDR, &reg, 1, &three, 1);
	say("camera: the ultra-wide's lens driver (AK7374) at %#04x on %s: 0x02 %#x (%s), position %#x (code %u; %s), 0x03 %#x (%s)\n",
	    ACT_ULTRA_ADDR, hw->bus->name, control, e_control ? bus_error(e_control) : "read", (uint32_t)at[0] << 8 | at[1],
	    ((uint32_t)at[0] << 8 | at[1]) >> 6, e_at ? bus_error(e_at) : "read", three, e_three ? bus_error(e_three) : "read");
	found = ((uint32_t)at[0] << 8 | at[1]) >> 6;
	if (focus_ultra < 2 || e_control != 0 || e_at != 0 || e_three != 0) {
		return;
	}
	if (found > ACT_CODE_MIN) {
		say("camera: the ultra-wide's lens found at %u, not at its rest: not driven\n", found);
		return;
	}
	uint8_t wake[2] = { 0x02, 0x00 };
	int err = instantnoodle_i2c_write(hw->bus, ACT_ULTRA_ADDR, wake, 2);

	reg = 0x02;
	if (err != 0 || instantnoodle_i2c_read_at(hw->bus, ACT_ULTRA_ADDR, &reg, 1, &control, 1) != 0 || control != 0) {
		say("camera: the ultra-wide's lens driver not woken (%s, 0x02 %#x): not driven\n", err ? bus_error(err) : "written", control);
		return;
	}
	act_addr = ACT_ULTRA_ADDR;
	act_reg = 0x00;
	act_shift = 6;
	act_park_code = found;
	act_sleeps = true;
	act_code = found;
	act_target = ACT_CODE_MIN;
	act_ready = true;
	say("camera: the ultra-wide's lens driver awake (0x02 0), the lens at %u, to go to %u (codes %u to %u, moves of at most %u a "
	    "frame)\n", found, ACT_CODE_MIN, ACT_CODE_MIN, ACT_CODE_MAX, ACT_MOVE_MAX);
}

/* the wide sensor powered and answering: cam_focus=1 the chip's registers read, 2 it initialised and at 50 */
static void
act_up(void)
{
	uint8_t id = 0, status = 0, run = 0, at[2] = { 0, 0 };
	int e_id, e_status, e_run, e_at;
	unsigned waited;

	act_ready = false;
	act_mismatches = 0;
	act_addr = ACT_ADDR;
	act_reg = ACT_POSITION;
	act_shift = 0;
	act_park_code = ACT_CODE_MIN;
	act_sleeps = false;
	if (frames_sensor == 1 && focus_ultra != 0) {
		act_ultra_up();
		return;
	}
	if (focus_level == 0 || frames_sensor != 0) {
		return;
	}
	e_id = act_read(0xf0, &id, 1);
	e_status = act_read(0xb3, &status, 1);
	e_run = act_read(0xe0, &run, 1);
	e_at = act_read(ACT_POSITION, at, 2);
	say("camera: the lens's driver (LC898217XC) at %#04x on %s: 0xf0 %#x (%s), 0xb3 %#x (%s), 0xe0 %#x (%s), position %#x (%s)\n",
	    ACT_ADDR, hw->bus->name, id, e_id ? bus_error(e_id) : "read", status, e_status ? bus_error(e_status) : "read", run,
	    e_run ? bus_error(e_run) : "read", (uint32_t)at[0] << 8 | at[1], e_at ? bus_error(e_at) : "read");
	if (focus_level < 2 || e_id != 0 || e_status != 0 || e_run != 0 || e_at != 0) {
		return;
	}
	if (id != 0x72) {
		say("camera: the lens's driver's 0xf0 is not 0x72: not an LC898217XC as found before, not driven\n");
		return;
	}
	/* the position the servo starts at: 50, not the 0x200 it holds (the middle) */
	uint8_t first[3] = { ACT_POSITION, ACT_CODE_MIN >> 8, ACT_CODE_MIN & 0xff };
	int err = instantnoodle_i2c_write(hw->bus, ACT_ADDR, first, 3);

	if (err != 0 || act_read(ACT_POSITION, at, 2) != 0) {
		say("camera: the lens's driver did not take its first position: not driven\n");
		return;
	}
	say("camera: the lens's position before its start: %u written, %#x read back\n", ACT_CODE_MIN, (uint32_t)at[0] << 8 | at[1]);
	/* the module file's initialisation: 0xe0 <- 1, 1 ms, 0xb3 until 0 */
	uint8_t start[2] = { 0xe0, 0x01 };

	err = instantnoodle_i2c_write(hw->bus, ACT_ADDR, start, 2);

	if (err != 0) {
		say("camera: the lens's driver did not take its start (0xe0 <- 1): %s\n", bus_error(err));
		return;
	}
	for (waited = 1; waited <= 50; waited++) {
		wait_ms(1);
		if (act_read(0xb3, &status, 1) == 0 && status == 0) {
			break;
		}
	}
	if (waited > 50) {
		say("camera: the lens's driver still busy after 50 ms (0xb3 %#x): not driven\n", status);
		return;
	}
	act_ready = true;
	act_target = ACT_CODE_MIN;
	if (!act_write_code(ACT_CODE_MIN)) {
		return;
	}
	say("camera: the lens's driver started (0xb3 0 after %u ms), the lens at %u (codes %u to %u, moves of at most %u a frame)\n",
	    waited, act_code, ACT_CODE_MIN, ACT_CODE_MAX, ACT_MOVE_MAX);
}

/*
 * The run's end or a panic's way out (instantnoodle_camera_quiesce(): the other processors stopped, no lock to be taken):
 * the bus by hand -- open drain, a line let go to its pull-up or held low -- at about 100 kHz, spinning.
 */
static void
act_leaving_line(struct instantnoodle_pin p, bool high)
{
	if (high) {
		instantnoodle_gpio_input(p, hw->bus->pull);
	} else {
		instantnoodle_gpio_output(p, false, hw->bus->pull, 2);
	}
	spin_us(5);
}

static bool
act_leaving_write(const uint8_t *bytes, unsigned n)
{
	struct instantnoodle_pin sda = hw->bus->sda, scl = hw->bus->scl;
	bool acked = true;

	act_leaving_line(sda, true);
	act_leaving_line(scl, true);
	act_leaving_line(sda, false);           /* (start) */
	act_leaving_line(scl, false);
	for (unsigned k = 0; k <= n; k++) {
		uint8_t v = k == 0 ? (uint8_t)(act_addr << 1) : bytes[k - 1];

		for (int b = 7; b >= 0; b--) {
			act_leaving_line(sda, (v >> b) & 1);
			act_leaving_line(scl, true);
			act_leaving_line(scl, false);
		}
		act_leaving_line(sda, true);
		act_leaving_line(scl, true);
		acked = acked && !instantnoodle_gpio_level(sda);
		act_leaving_line(scl, false);
	}
	act_leaving_line(sda, false);
	act_leaving_line(scl, true);
	act_leaving_line(sda, true);            /* (stop) */
	return acked;
}

/*
 * Before the leaving writes: a transfer the way out cut short may have left the target holding SDA low -- up to nine
 * clocks until it lets go, then a stop (the supervisor's, 2026-11-04). False: the bus not free, and nothing written.
 */
static bool
act_leaving_recover(void)
{
	struct instantnoodle_pin sda = hw->bus->sda, scl = hw->bus->scl;

	act_leaving_line(sda, true);
	act_leaving_line(scl, true);
	for (unsigned i = 0; i < 9 && !instantnoodle_gpio_level(sda); i++) {
		act_leaving_line(scl, false);
		act_leaving_line(scl, true);
	}
	if (!instantnoodle_gpio_level(sda) || !instantnoodle_gpio_level(scl)) {
		return false;
	}
	act_leaving_line(sda, false);
	act_leaving_line(sda, true);            /* (a stop: SDA rising while SCL is high) */
	return true;
}

/* the lens back to 50 in moves of at most ACT_MOVE_MAX, before its power goes (power_down()) */
static void
act_park(void)
{
	uint32_t from = act_code;
	bool ok = true;

	if (!act_ready) {
		return;
	}
	act_target = act_park_code;
	if (leaving && !act_leaving_recover()) {
		act_ready = false;
		say("camera: the lens NOT parked (at %u): the bus not free on the way out (SDA %u, SCL %u)\n", act_code,
		    instantnoodle_gpio_level(hw->bus->sda), instantnoodle_gpio_level(hw->bus->scl));
		return;
	}
	while (act_code > act_park_code && ok) {
		uint32_t to = act_code - MIN(act_code - act_park_code, ACT_MOVE_MAX);

		if (leaving) {
			uint32_t word = to << act_shift;
			uint8_t bytes[3] = { act_reg, (uint8_t)(word >> 8), (uint8_t)word };

			ok = act_leaving_write(bytes, 3);
			act_code = to;
			spin_us(10000);
		} else {
			ok = act_write_code(to);
			wait_ms(10);
		}
	}
	bool asleep = false;

	if (act_sleeps && ok) {
		/* (the AK7374 asleep again, as it was found: 0x02 <- 0x40) */
		uint8_t sleep[2] = { 0x02, 0x40 };

		ok = asleep = leaving ? act_leaving_write(sleep, 2) : instantnoodle_i2c_write(hw->bus, act_addr, sleep, 2) == 0;
	}
	act_ready = false;
	say("camera: the lens parked: from %u to %u%s%s%s\n", from, act_code, asleep ? ", asleep" : "",
	    leaving ? " (by hand, leaving)" : "", ok ? "" : " -- A WRITE FAILED");
}

/*
 * Boot-args cam_shutter=LINES and cam_gain=CODE for the front sensor, after its mode's table, held so the sensor takes them
 * together (redwood's exposure()): SMIA's coarse integration 0x0202 (within the frame's lines less 10) and analog gain 0x0204
 * (Sony's code, 1024 / (1024 - code) times: 0x380 is 8x, 0x3c0 16x, the most taken here). The mode's own are 2676 lines of
 * 2685 and gain 0: run 094343's frame was 63..89 of 1023 with the black at 64, about 20 counts of light.
 */
static bool
frames_exposure(const struct ctab_mode *mode)
{
	uint32_t shutter = 0, gain = 0;

	(void)PE_parse_boot_argn("cam_shutter", &shutter, sizeof(shutter));
	(void)PE_parse_boot_argn("cam_gain", &gain, sizeof(gain));
	if (shutter == 0 && gain == 0) {
		return true;
	}
	shutter = shutter > mode->frame_length - 10 ? mode->frame_length - 10 : shutter;
	gain = gain > 0x3c0 ? 0x3c0 : gain;
	if (!sensor_table("holdon") || (shutter != 0 && !sensor_reg16(0x0202, (uint16_t)shutter)) ||
	    (gain != 0 && !sensor_reg16(0x0204, (uint16_t)gain)) || !sensor_table("holdoff")) {
		return false;
	}
	say("camera: the exposure: %u lines%s, analog gain code %#x (%u.%02ux)\n", shutter ? shutter : 2676, shutter ? "" : " (the mode's)",
	    gain, 1024 / (1024 - gain), 1024 * 100 / (1024 - gain) % 100);
	return true;
}

static unsigned
bits_of(uint32_t data_type)
{
	return data_type == 0x2a ? 8 : data_type == 0x2b ? 10 : data_type == 0x2c ? 12 : data_type == 0x2d ? 14 : 0;
}

/*
 * The front sensor's stream through CSIPHY 2 into CSID-lite 0 (and with `into_memory' through RDI 0 and the write master to
 * `address', one frame in `drop_period' + 1 written), split so that one frame (cam_step=5) and iOS's frames (cam=5) take the
 * same steps; what is found is kept in `st' and put back by stream_down(), whatever stopped stream_up().
 */
static struct {
	const struct ctab_mode *mode;
	uint32_t        stride, settle, found_vfe_mask, found[5], found_rdi[RDI_REGS], found_wm[WM_REGS];
	bool            csid_set, phy_lanes, streaming, rdi_set, wm_set;
} st;
static const uint32_t kept_regs[5] = { CSID_TOP_IRQ_MASK, CSID_RX_IRQ_MASK, CSID_RX_CFG0, CSID_RX_CFG1, CSID_RX_CAPTURE_CTRL };
static bool ios_exposure_first(void);

/*
 * The sensor's tables and its mode (the front one's 1, 2328x1748; the ultra-wide one's 0, 4656x3496, its whole field: its
 * modes 1 and 2 are the sensor's middle; the wide one's 1 or 6): the stride and the PHY's settle count
 */
static bool
stream_mode(bool into_memory)
{
	uint64_t link_hz, ui_ps;
	unsigned bits;

	bzero(&st, sizeof(st));
	hw = &sensors[frames_sensor];
	csiphy = hw->csiphy == 0 ? csiphy0 : hw->csiphy == 3 ? csiphy3 : csiphy2;
	say("camera: the %s camera, its %s CSID-lite 0 through CSIPHY %u\n", hw->name, into_memory ? "frames into memory through" :
	    "packets to", hw->csiphy);
	if ((hw->csiphy != 2 && hw->csiphy != 0 && hw->csiphy != 3) || !ctab_load() || ctab->modes < 2) {
		give_up("not the front, the wide or the ultra-wide sensor's tables");
		return false;
	}
	if (frames_mode >= ctab->modes || (hw->csiphy == 2 && frames_mode != 1) || (hw->csiphy == 3 && frames_mode != 0)) {
		give_up("a mode the sensor's tables do not have");
		return false;
	}
	st.mode = &((const struct ctab_mode *)(ctab + 1))[frames_mode];
	bits = bits_of(st.mode->data_type);
	if (hw->csiphy != 0 ? st.mode->cphy || st.mode->lanes != 4 : !st.mode->cphy || st.mode->lanes != 3) {
		give_up(hw->csiphy != 0 ? "a mode not of four D-PHY lanes" : "a mode not of three C-PHY trios");
		return false;
	}
	if (bits == 0 || st.mode->pixel_clock == 0) {
		give_up("a mode not of raw pixels");
		return false;
	}
	st.stride = (st.mode->width * bits / 8 + 15) & ~15u;
	if (st.mode->cphy) {
		/*
		 * C-PHY (the wide IMX586's mode 1, 4000x3000 on three trios): the settle count is the module file's, in periods of the
		 * 300 MHz timer, as redwood's (hk_camera.c, whose wide S5KHM2SD is C-PHY on the same CSIPHY v1.2.1)
		 */
		st.settle = st.mode->settle;
		say("camera: mode %u: %ux%u at %u,%u, data type %#x (%u bits), %u C-PHY trios, %llu Mbit/s a trio, settle count %u\n",
		    frames_mode, st.mode->width, st.mode->height, st.mode->x, st.mode->y, st.mode->data_type, bits, st.mode->lanes,
		    (uint64_t)st.mode->pixel_clock * bits / st.mode->lanes / 1000000, st.settle);
		return true;
	}
	/* csiphy_settle_cnt_calc(): the link's clock (half the lane's bit rate), T_HS_PREPARE max 85 ns + 6 UI, in 300 MHz ticks, - 6 */
	link_hz = (uint64_t)st.mode->pixel_clock * bits / st.mode->lanes / 2;
	ui_ps = 1000000000000ull / link_hz / 2;
	st.settle = (uint32_t)((85000 + 6 * ui_ps) / (1000000000000ull / CSIPHY_TIMER_HZ)) - 6;
	say("camera: mode %u: %ux%u, data type %#x (%u bits), %u lanes, %llu MHz link, settle count %u\n", frames_mode, st.mode->width,
	    st.mode->height, st.mode->data_type, bits, st.mode->lanes, link_hz / 1000000, st.settle);
	return true;
}

/*
 * The wide IMX586's PHY, CSIPHY 0, for three C-PHY trios: redwood's (hk_camera.c csiphy_up(), csiphy_common(), csiphy_cphy():
 * Android's cam_csiphy_1_2_1_hwreg.h and cam_csiphy_core.c for "qcom,csiphy-v1.2.1", this phone's CSIPHYs' too), every write
 * read back. The lanes to enable from the module file's assignment (trio n at bit 2n + 1), the common registers of a C-PHY,
 * csiphy_3ph_v1_2_1_reg for each of the three trio positions with the settle count, the deltas of data_rate_delta_table_1_2_1's
 * first row that covers the rate (2.5, 3.5 and 4.5 Gsym/s a trio at 2.28 bits a symbol), the interrupts' masks.
 */
static bool
phy_cphy_up(uint32_t lane_assign, unsigned settle, uint64_t bits_per_trio)
{
	static const uint32_t trios[3][2] = { { 0x100, 0x980 }, { 0x300, 0xa80 }, { 0x500, 0xb80 } };
	static const struct {
		uint64_t        bandwidth;
		uint8_t         r144, r164, r16c, rb4;  /* (rb4: 0 for none) */
	} rates[3] = {
		{ 5700000000ull, 0x32, 0x0b, 0x25, 0 }, { 7980000000ull, 0xb2, 0x33, 0x1d, 0x03 }, { 10260000000ull, 0xb2, 0x33, 0x25, 0x02 },
	};
	static const uint8_t irq_masks[11] = { 0xff, 0xff, 0xfb, 0xff, 0x7f, 0xff, 0xff, 0xef, 0xff, 0xff, 0xff };
	uint32_t lane_enable = 0;
	unsigned r = 0;

	for (unsigned l = 0; l < 3; l++, lane_assign >>= 4) {
		unsigned n = lane_assign & 0xf;

		if (n > 2) {
			give_up("a C-PHY lane assignment not known here");
			return false;
		}
		lane_enable |= 1u << (2 * n + 1);
	}
	st.phy_lanes = true;
	if (!blk_put(csiphy, "CSIPHY 0", 0x814, lane_enable, ~0u) || !blk_put(csiphy, "CSIPHY 0", 0x818, 0x01, ~0u) ||
	    !blk_put(csiphy, "CSIPHY 0", 0x81c, 0x52, ~0u)) {
		return false;
	}
	wr(csiphy + 0x800, 0x03);
	spin_us(1);
	wr(csiphy + 0x800, 0x0e);
	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!blk_put(csiphy, "CSIPHY 0", m + 0x10, 0x00, ~0u) || !blk_put(csiphy, "CSIPHY 0", m + 0x14, 0x00, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", m + 0x18, 0x00, ~0u) || !blk_put(csiphy, "CSIPHY 0", m + 0x10, 0x08, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", m + 0x14, 0x08, ~0u) || !blk_put(csiphy, "CSIPHY 0", m + 0x18, 0x1a, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", m + 0x0c, 0xaf, ~0u)) {
			return false;
		}
		spin_us(1);
		if (!blk_put(csiphy, "CSIPHY 0", b + 0x5c, 0x46, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x68, 0xa0, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x04, 0x06, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x0c, settle & 0xff, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x08, (settle >> 8) & 0xff, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x14, 0x20, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x50, 0x00, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x88, 0x00, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x8c, 0x7f, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x90, 0x7f, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x18, 0x3e, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x1c, 0x41, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x20, 0x41, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x24, 0x7f, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x28, 0x00, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x2c, 0x00, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x60, 0x02, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0xcc, 0x41, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0xdc, 0x50, ~0u) || !blk_put(csiphy, "CSIPHY 0", m + 0x04, 0x20, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", m + 0x08, 0x05, ~0u) || !blk_put(csiphy, "CSIPHY 0", m + 0x00, 0x60, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", m + 0x2c, 0x35, ~0u) || !blk_put(csiphy, "CSIPHY 0", m + 0x30, 0x01, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", 0x800, 0x0e, ~0u) || !blk_put(csiphy, "CSIPHY 0", 0x884, 0x01, ~0u)) {
			return false;
		}
	}
	while (r < 2 && bits_per_trio > rates[r].bandwidth) {
		r++;
	}
	for (unsigned t = 0; t < 3; t++) {
		uint32_t b = trios[t][0], m = trios[t][1];

		if (!blk_put(csiphy, "CSIPHY 0", b + 0x44, rates[r].r144, ~0u) || !blk_put(csiphy, "CSIPHY 0", b + 0x64, rates[r].r164, ~0u) ||
		    !blk_put(csiphy, "CSIPHY 0", b + 0x6c, rates[r].r16c, ~0u) ||
		    (rates[r].rb4 != 0 && !blk_put(csiphy, "CSIPHY 0", m + 0x34, rates[r].rb4, ~0u))) {
			return false;
		}
		if (rates[r].rb4 != 0) {
			spin_us(1);
		}
	}
	for (unsigned i = 0; i < 11; i++) {
		wr(csiphy + 0x82c + 4 * i, irq_masks[i]);
		wr(csiphy + 0x858 + 4 * i, 0xff);
	}
	wr(csiphy + 0x828, 0x1);
	wr(csiphy + 0x828, 0x0);
	say("camera: CSIPHY 0's trios on (lanes enabled %#x, rate row %u for %llu bit/s a trio, settle count %u)\n", lane_enable, r,
	    bits_per_trio, settle);
	return true;
}

/* the subsystem, the sensor powered and checked, the blocks reset and set, the sensor's PHY's lanes, the sensor streaming */
static bool
stream_up(bool into_memory, uint32_t address, uint32_t drop_period, bool ios, bool kept, bool res_too)
{
	vm_offset_t csid = ife_lite0 + CSID_LITE0;
	uint32_t lane_cfg;

	/* (kept: the sensor up and initialised from the session before, ios_stop(): none of it again) */
	if (!kept && (!titan_up() || !power_up() || !read_chip_id())) {
		return false;
	}
	if (!kept) {
		act_up();
	}
	/* VFE-lite 0's global reset (vfe_global_reset(); its acknowledgement polled) */
	if (!kept) {
		st.found_vfe_mask = rd(ife_lite0 + VFE_LITE_IRQ_MASK0);         /* (as found: the first session's, kept or not) */
	}
	if (!blk_put(ife_lite0, "VFE-lite 0", VFE_LITE_IRQ_MASK0, VFE_LITE_RESET_ACK, ~0u) ||
	    !blk_put(ife_lite0, "VFE-lite 0", VFE_LITE_RESET_CMD, 1u << 1, 0)) {
		return false;
	}
	if (!wait_bits(ife_lite0 + VFE_LITE_IRQ_STATUS0, VFE_LITE_RESET_ACK, VFE_LITE_RESET_ACK, 50000)) {
		say("camera: VFE-lite 0 does not acknowledge its reset (status %#x)\n", rd(ife_lite0 + VFE_LITE_IRQ_STATUS0));
		give_up("a block that does not reset");
		return false;
	}
	wr(ife_lite0 + VFE_LITE_IRQ_CLEAR0, rd(ife_lite0 + VFE_LITE_IRQ_STATUS0));
	wr(ife_lite0 + VFE_LITE_IRQ_CMD, 1);
	/* the PHY's reset (csiphy_reset(); for C-PHY cam_csiphy_reset() as redwood's csiphy_up() has it) */
	if (st.mode->cphy) {
		wr(csiphy + 0x814, 0x00);
		spin_us(5);
		wr(csiphy + 0x818, 0x00);
		wr(csiphy + 0x81c, 0x00);
		wr(csiphy + 0x800, 0x01);
		spin_us(1);
		wr(csiphy + 0x800, 0x00);
	} else {
		wr(csiphy + CSIPHY_COMMON_CTRL(0), 1);
		wait_ms(6);
		wr(csiphy + CSIPHY_COMMON_CTRL(0), 0);
	}
	/* CSID-lite 0's reset (csid_reset(), its done polled) */
	for (unsigned i = 0; i < 5; i++) {
		st.found[i] = kept ? st.found[i] : rd(csid + kept_regs[i]);
	}
	st.csid_set = true;
	wr(csid + CSID_TOP_IRQ_CLEAR, 1);
	wr(csid + CSID_IRQ_CMD, 1);
	if (!blk_put(csid, "CSID-lite 0", CSID_TOP_IRQ_MASK, 1, ~0u)) {
		return false;
	}
	wr(csid + CSID_IRQ_CMD, 1);
	wr(csid + CSID_RST_STROBES, 0x1e);
	if (!wait_bits(csid + CSID_TOP_IRQ_STATUS, 1, 1, 500000)) {
		say("camera: CSID-lite 0 does not finish its reset (top %#x)\n", rd(csid + CSID_TOP_IRQ_STATUS));
		give_up("a block that does not reset");
		return false;
	}
	wr(csid + CSID_TOP_IRQ_CLEAR, rd(csid + CSID_TOP_IRQ_STATUS));
	wr(csid + CSID_IRQ_CMD, 1);
	if (into_memory) {
		/* the write master: vfe_wm_start() but one frame (above), MIPI raw, to the SMMU's addresses; its state latched; taken */
		for (unsigned i = 0; i < WM_REGS; i++) {
			st.found_wm[i] = kept ? st.found_wm[i] : rd(ife_lite0 + wm_regs[i]);
		}
		st.wm_set = true;
		/*
		 * The bus's tuning: what VFE-lite keeps of Linux's values (fewer bits than the full VFE's: the clock-gating override
		 * four, run 093614; the burst limit two, run 093955) is told, not checked. Checked: where and how much is written --
		 * the frame's increment 0, the st.stride, the address, the drop pattern's first frame, the write master's st.mode
		 */
		static const struct {
			uint32_t offset, value;
		} tuning[] = {
			{ VFE_BUS_CGC_OVERRIDE, 0x3ffffff }, { VFE_BUS_TEST_CTRL, 0 }, { VFE_WM0_BURST_LIMIT, 0xf }, { VFE_WM0_IMAGE_CFG_0, 0xffff },
			{ VFE_WM0_PACKER_CFG, 0 }, { VFE_WM0_IRQ_SUB_PERIOD, 0 }, { VFE_WM0_IRQ_SUB_PATTERN, 1 },
		};
		for (unsigned i = 0; i < sizeof(tuning) / sizeof(tuning[0]); i++) {
			wr(ife_lite0 + tuning[i].offset, tuning[i].value);
			if (rd(ife_lite0 + tuning[i].offset) != tuning[i].value) {
				say("camera: VFE-lite 0 %#x <- %#x keeps %#x\n", tuning[i].offset, tuning[i].value, rd(ife_lite0 + tuning[i].offset));
			}
		}
		wr(ife_lite0 + VFE_WM0_DROP_PERIOD, drop_period);       /* (a tuning one too: told if it keeps fewer bits) */
		if (rd(ife_lite0 + VFE_WM0_DROP_PERIOD) != drop_period) {
			say("camera: VFE-lite 0 %#x <- %#x keeps %#x\n", VFE_WM0_DROP_PERIOD, drop_period, rd(ife_lite0 + VFE_WM0_DROP_PERIOD));
		}
		if (!blk_put(ife_lite0, "VFE-lite 0", VFE_WM0_FRAME_INCR, 0, ~0u) ||
		    !blk_put(ife_lite0, "VFE-lite 0", VFE_WM0_IMAGE_CFG_2, st.stride, ~0u) ||
		    !blk_put(ife_lite0, "VFE-lite 0", VFE_WM0_DROP_PATTERN, 1, 1) ||
		    !blk_put(ife_lite0, "VFE-lite 0", VFE_WM0_IMAGE_ADDR, address, ~0u) ||
		    !(ife_dma_live = true) || !blk_put(ife_lite0, "VFE-lite 0", VFE_WM0_CFG, 1u | 1u << 16, 0x30001) ||
		    !blk_put(ife_lite0, "VFE-lite 0", VFE_BUS_IRQ_MASK0, 1u | 1u << 4, 1u | 1u << 4) ||
		    !blk_put(ife_lite0, "VFE-lite 0", VFE_LITE_IRQ_MASK0, VFE_LITE_RESET_ACK | VFE_LITE_BUS_TOP,
		    VFE_LITE_RESET_ACK | VFE_LITE_BUS_TOP)) {
			return false;
		}
		wr(ife_lite0 + VFE_BUS_IRQ_CLEAR0, 0xffffffff);
		wr(ife_lite0 + VFE_BUS_IRQ_CLEAR_ALL, 1);
		wr(ife_lite0 + VFE_LITE_REG_UPDATE, 1);
		/* RDI 0 (__csid_configure_rdi_stream()): payload only, the st.mode's DT, VC 0, nothing dropped; its state latched; on */
		for (unsigned i = 0; i < RDI_REGS; i++) {
			st.found_rdi[i] = kept ? st.found_rdi[i] : rd(csid + rdi_regs[i]);
		}
		st.rdi_set = true;
		if (!blk_put(csid, "CSID-lite 0", CSID_RDI0_CFG0, 7u | 0xfu << 12 | st.mode->data_type << 16, ~0u) ||
		    !blk_put(csid, "CSID-lite 0", CSID_RDI0_CFG1, 2, ~0u) || !blk_put(csid, "CSID-lite 0", 0x210, 1, ~0u) ||
		    !blk_put(csid, "CSID-lite 0", 0x20c, 0, ~0u) || !blk_put(csid, "CSID-lite 0", 0x218, 1, ~0u) ||
		    !blk_put(csid, "CSID-lite 0", 0x214, 0, ~0u) || !blk_put(csid, "CSID-lite 0", 0x228, 1, ~0u) ||
		    !blk_put(csid, "CSID-lite 0", 0x224, 0, ~0u) || !blk_put(csid, "CSID-lite 0", 0x230, 1, ~0u) ||
		    !blk_put(csid, "CSID-lite 0", 0x22c, 0, ~0u) || !blk_put(csid, "CSID-lite 0", CSID_RDI0_CTRL, 0, ~0u) ||
		    !blk_put(csid, "CSID-lite 0", CSID_RDI0_IRQ_MASK, 1u << 2 | 1u << 9 | 1u << 12 | 1u << 15, ~0u) ||
		    !blk_put(csid, "CSID-lite 0", CSID_RDI0_CFG0, 1u << 31 | 7u | 0xfu << 12 | st.mode->data_type << 16, ~0u)) {
			return false;
		}
		wr(csid + CSID_RDI0_IRQ_CLEAR, 0xffffffff);
	}
	/*
	 * the RX: the mode's lanes (the front's four, the wide one's three trios), the module file's assignment, the sensor's PHY,
	 * C-PHY or D-PHY ([24], as redwood's csid_config()); ECC correction and MISR; every RX bit latched; both captures
	 */
	lane_cfg = (st.mode->lanes - 1) | (ctab->lane_assign & 0xffff) << 4 | hw->csiphy << 20 | (st.mode->cphy ? 1u : 0u) << 24;
	if (!blk_put(csid, "CSID-lite 0", CSID_RX_CFG0, lane_cfg, ~0u) || !blk_put(csid, "CSID-lite 0", CSID_RX_CFG1, 0x41, ~0u) ||
	    !blk_put(csid, "CSID-lite 0", CSID_RX_IRQ_MASK, 0x1ffffff, 0x1ffffff) ||
	    !blk_put(csid, "CSID-lite 0", CSID_RX_CAPTURE_CTRL, 3u | st.mode->data_type << 4, ~0u)) {
		return false;
	}
	wr(csid + CSID_RX_IRQ_CLEAR, 0xffffffff);
	wr(csid + CSID_IRQ_CMD, 1);
	if (into_memory && !blk_put(csid, "CSID-lite 0", CSID_RDI0_CTRL, 1, ~0u)) {
		return false;
	}
	if (st.mode->cphy) {
		if (!phy_cphy_up(ctab->lane_assign, st.settle, (uint64_t)st.mode->pixel_clock * bits_of(st.mode->data_type) / st.mode->lanes)) {
			return false;
		}
		say("camera: CSID-lite 0's RX set (cfg %#x %#x)\n", rd(csid + CSID_RX_CFG0), rd(csid + CSID_RX_CFG1));
	} else {
		/* CSIPHY 2's or 3's lanes (csiphy_lanes_enable(): the clock lane and data lanes 0-3, out of power-down, the table, IRQs masked) */
		st.phy_lanes = true;
		wr(csiphy + CSIPHY_COMMON_CTRL(5), 0x80 | 0x55);
		wr(csiphy + CSIPHY_COMMON_CTRL(6), 1);
		wr(csiphy + CSIPHY_COMMON_CTRL(7), 2);
		wr(csiphy + CSIPHY_COMMON_CTRL(0), 0);
		for (unsigned i = 0; i < sizeof(lane_regs_sm8250) / sizeof(lane_regs_sm8250[0]); i++) {
			if (lane_regs_sm8250[i].kind != PHY_SKIP) {
				wr(csiphy + lane_regs_sm8250[i].offset, lane_regs_sm8250[i].kind == PHY_SETTLE ? st.settle & 0xff :
				    lane_regs_sm8250[i].value);
			}
		}
		for (unsigned n = 11; n < 22; n++) {
			wr(csiphy + CSIPHY_COMMON_CTRL(n), 0);
		}
		say("camera: CSIPHY %u's lanes on (CTRL5 %#x, CTRL6 %#x); CSID-lite 0's RX set (cfg %#x %#x)\n", hw->csiphy,
		    rd(csiphy + CSIPHY_COMMON_CTRL(5)), rd(csiphy + CSIPHY_COMMON_CTRL(6)), rd(csid + CSID_RX_CFG0), rd(csid + CSID_RX_CFG1));
	}
	/* the sensor: its initialisation, the mode, its exposure (the boot-args', or iOS's first), streaming */
	char res[8];

	snprintf(res, sizeof(res), "res%u", frames_mode);
	if ((!kept && !sensor_table("init")) || (res_too && !sensor_table(res)) ||
	    !(ios ? ios_exposure_first() : frames_exposure(st.mode))) {
		return false;
	}
	st.streaming = true;
	return sensor_table("on");
}

/* what CSID-lite 0's RX and CSIPHY 2 say of the stream */
static void
stream_report(void)
{
	vm_offset_t csid = ife_lite0 + CSID_LITE0;
	uint32_t status, top;

	status = rd(csid + CSID_RX_IRQ_STATUS);
	top = rd(csid + CSID_TOP_IRQ_STATUS);
	say("camera: CSID-lite 0 after 300 ms: RX status %#x (SOT %#x, EOT %#x, long %u, short %u, ECC warning %u, overflow %#x, CRC %u, "
	    "ECC %u, unmapped %u, underflow %u, unbounded %u), top %#x\n", status, status >> 4 & 0xf, status & 0xf, status >> 8 & 1,
	    status >> 9 & 1, status >> 14 & 1, status >> 15 & 0xf, status >> 19 & 1, status >> 20 & 1, status >> 21 & 3,
	    status >> 23 & 1, status >> 24 & 1, top);
	say("camera: CSID-lite 0: %u packets received, ECC %#x, %u CRC errors; the last short packet %#x (VC %u DT %#x, %u), the last long "
	    "%#x (DT %#x, %u bytes)\n", rd(csid + CSID_RX_TOTAL_PKTS), rd(csid + CSID_RX_STATS_ECC), rd(csid + CSID_RX_TOTAL_CRC_ERR),
	    rd(csid + CSID_RX_SHORT_PKT0), rd(csid + CSID_RX_SHORT_PKT0) >> 22, rd(csid + CSID_RX_SHORT_PKT0) >> 16 & 0x3f,
	    rd(csid + CSID_RX_SHORT_PKT0) & 0xffff, rd(csid + CSID_RX_LONG_PKT0), rd(csid + CSID_RX_LONG_PKT0) >> 16 & 0x3f,
	    rd(csid + CSID_RX_LONG_PKT0) & 0xffff);
	say("camera: CSIPHY %u's status: %#x %#x %#x %#x\n", hw->csiphy, rd(csiphy + CSIPHY_COMMON_STATUS(0)),
	    rd(csiphy + CSIPHY_COMMON_STATUS(1)), rd(csiphy + CSIPHY_COMMON_STATUS(2)), rd(csiphy + CSIPHY_COMMON_STATUS(3)));
}

/* stream off, CSIPHY 2 reset, VFE-lite 0 and CSID-lite 0 as found, everything powered down (the DMA stopped first there) */
static void
stream_down(void)
{
	vm_offset_t csid = ife_lite0 + CSID_LITE0;

	ife_dma_stop();                 /* (the DMA first: before the registers are put back, and before the sensor stops) */
	if (st.streaming) {
		bool was = broken;

		broken = false;         /* (stream off is a write the run's end takes in any case) */
		(void)sensor_table("off");
		broken = was;
		wait_ms(40);
	}
	if (st.phy_lanes && st.mode->cphy) {
		/* the trios off and the PHY reset, as redwood's csiphy_down() */
		wr(csiphy + 0x814, 0x00);
		spin_us(5);
		wr(csiphy + 0x818, 0x00);
		wr(csiphy + 0x81c, 0x00);
		wr(csiphy + 0x800, 0x01);
		spin_us(1);
		wr(csiphy + 0x800, 0x00);
	} else if (st.phy_lanes) {
		/* csiphy_lanes_disable(), then a reset: CSIPHY 2 as it came out of it */
		wr(csiphy + CSIPHY_COMMON_CTRL(5), 0);
		wr(csiphy + CSIPHY_COMMON_CTRL(6), 0);
		wr(csiphy + CSIPHY_COMMON_CTRL(0), 1);
		wait_ms(6);
		wr(csiphy + CSIPHY_COMMON_CTRL(0), 0);
	}
	if (st.wm_set) {
		/* VFE-lite 0 reset again (the bus idle after it), then the write master's registers as found */
		wr(ife_lite0 + VFE_LITE_IRQ_MASK0, VFE_LITE_RESET_ACK);
		wr(ife_lite0 + VFE_LITE_RESET_CMD, 1u << 1);
		if (!wait_bits(ife_lite0 + VFE_LITE_IRQ_STATUS0, VFE_LITE_RESET_ACK, VFE_LITE_RESET_ACK, 50000)) {
			say("camera: VFE-lite 0 DOES NOT ACKNOWLEDGE ITS RESET at the end (status %#x)\n", rd(ife_lite0 + VFE_LITE_IRQ_STATUS0));
		}
		wr(ife_lite0 + VFE_LITE_IRQ_CLEAR0, rd(ife_lite0 + VFE_LITE_IRQ_STATUS0));
		wr(ife_lite0 + VFE_LITE_IRQ_CMD, 1);
		wr(ife_lite0 + VFE_BUS_IRQ_CLEAR0, 0xffffffff);
		wr(ife_lite0 + VFE_BUS_IRQ_CLEAR_ALL, 1);
		for (unsigned i = WM_REGS; i-- > 0;) {
			wr(ife_lite0 + wm_regs[i], st.found_wm[i]);
		}
	}
	if (st.rdi_set) {
		wr(csid + CSID_RDI0_IRQ_CLEAR, 0xffffffff);
		for (unsigned i = RDI_REGS; i-- > 0;) {
			wr(csid + rdi_regs[i], st.found_rdi[i]);
		}
	}
	if (st.csid_set) {
		wr(csid + CSID_RX_IRQ_CLEAR, 0xffffffff);
		wr(csid + CSID_TOP_IRQ_CLEAR, 0xffffffff);
		wr(csid + CSID_IRQ_CMD, 1);
		for (unsigned i = 5; i-- > 0;) {
			wr(csid + kept_regs[i], st.found[i]);
		}
		wr(ife_lite0 + VFE_LITE_IRQ_MASK0, st.found_vfe_mask);
		say("camera: CSID-lite 0's and VFE-lite 0's registers back as found; CSIPHY %u reset\n", hw->csiphy);
	}
	power_down();           /* (the SMMU's part after the write master is off and VFE-lite reset; before the memory) */
}

static void
frames_stream(bool into_memory)
{
	vm_offset_t csid = ife_lite0 + CSID_LITE0;
	uint64_t hit;
	bool up, written = false;

	if (!stream_mode(into_memory)) {
		return;
	}
	if (into_memory) {
		/* the frame's memory and the SMMU first: without them nothing else is switched on */
		if (!ife_memory_alloc(st.stride * st.mode->height, 1) || !smmu_ife_up()) {
			power_down();
			ife_memory_free();
			return;
		}
	}
	up = stream_up(into_memory, ife_place_address(0), 31, false, false, true);
	if (up && into_memory) {
		uint32_t bus, rdi;

		/* the first buffer done, then at once the write master off and RDI 0 halted: one frame in memory */
		written = wait_bits(ife_lite0 + VFE_BUS_IRQ_STATUS0, 1u << 4, 1u << 4, 500000);
		bus = rd(ife_lite0 + VFE_BUS_IRQ_STATUS0);
		rdi = rd(csid + CSID_RDI0_IRQ_STATUS);
		ife_dma_stop();
		say("camera: IFE-lite 0's write master: %s (bus status %#x, VFE status %#x); RDI 0: status %#x (start of frame %u, end %u, "
		    "overflow %u, CCIF violation %u)\n", written ? "a frame written" : "NO FRAME DONE IN 500 ms", bus,
		    rd(ife_lite0 + VFE_LITE_IRQ_STATUS0), rdi, rdi >> 12 & 1, rdi >> 9 & 1, rdi >> 2 & 1, rdi >> 15 & 1);
	} else if (up) {
		wait_ms(300);   /* (some nine frames at 30 a second) */
	}
	if (up) {
		stream_report();
	}
	stream_down();
	if (into_memory) {
		hit = ife_canaries_hit();
		say("camera: the guard pages and the frame's last page's rest: %llu word(s) changed%s\n", hit,
		    hit ? " -- SOMETHING WROTE THERE" : "");
		if (written) {
			ife_hand_over(st.mode, st.stride);
		} else {
			ife_memory_free();
		}
	}
}


/*
 * ---- the frames' path, step 3 (cam_step=3): what the SMMU and the GIC have for the camera, read only -------------------
 *
 * Before any write master is given an address (docs/TODO.md T07: steps 3a, 3b, 3c): the application processors' SMMU
 * (apps-smmu@15000000, an MMU-500) as hk_wifi.c's smmu_look() and radio_space_find() read it -- only its global SCR0,
 * IDR0, IDR1 and the stream match registers with their S2CRs (no context bank's register: context 4, the display's, is a
 * CrashDump to read, docs/research/hardware-qualcomm.md §1), under hk_smmu_lock, which the Wi-Fi and the sound take when
 * they look for free ones --: which valid match register covers one of the IFEs' streams (dt msm_cam_smmu_ife: 0x800,
 * 0x801, 0x840, 0x841, 0xc00, 0xc01, 0xc40, 0xc41, mask 0x400 each), the first free match register and the lowest context
 * no valid one sends a stream to. And the GIC distributor's enables of CSID-lite 0's and IFE-lite 0's SPIs (dt 0x1d4,
 * 0x1d5): an interrupt latched there with no handler would come again and again. Nothing is written.
 */
#define APPS_SMMU_PHYS          0x15000000UL
#define SMMU_SCR0               0x000           /* [0] off, [3] 16-bit streams, [10] a stream nobody matches faults */
#define SMMU_IDR0               0x020           /* [7:0] match registers */
#define SMMU_IDR1               0x024           /* [7:0] contexts */
#define SMMU_SMR(n)             (0x800 + 4 * (n))       /* [31] valid, [30:16] mask, [14:0] stream */
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))       /* [17:16] 0 translate, 1 bypass, 2 fault; [7:0] context */
#define GICD_PHYS               0x17a00000UL
#define GICD_ISENABLER(n)       (0x100 + 4 * (n))
#define CSID_LITE0_SPI          0x1d4
#define IFE_LITE0_SPI           0x1d5

extern lck_mtx_t hk_smmu_lock;          /* (bsd/dev/hk_wifi.c) */

static const uint16_t ife_streams[] = { 0x800, 0x801, 0x840, 0x841, 0xc00, 0xc01, 0xc40, 0xc41 };
#define IFE_STREAM_MASK         0x400

static void
frames_smmu_look(void)
{
	static const char *const kinds[] = { "translate", "BYPASS", "fault", "reserved" };
	vm_offset_t smmu = ml_io_map(APPS_SMMU_PHYS, 0x1000), gicd = ml_io_map(GICD_PHYS, 0x1000);
	bool used[256] = { false };
	uint32_t scr0, idr0, idr1, matches, contexts, valid = 0, covering = 0, first_free = ~0u, free_n = 0, context, unused = 0;

	lck_mtx_lock(&hk_smmu_lock);
	scr0 = rd(smmu + SMMU_SCR0);
	idr0 = rd(smmu + SMMU_IDR0);
	idr1 = rd(smmu + SMMU_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	say("camera: the apps SMMU: scr0 %#x (%s; streams of %u bits; a stream nobody matches %s), idr0 %#x, idr1 %#x: %u match "
	    "registers, %u contexts\n", scr0, scr0 & 1 ? "OFF" : "on", scr0 & 1u << 3 ? 16 : 15, scr0 & 1u << 10 ? "faults" :
	    "passes untranslated", idr0, idr1, matches, contexts);
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));
		uint32_t stream = smr & 0x7fff, mask = smr >> 16 & 0x7fff;

		if (!(smr >> 31)) {
			free_n++;
			if (first_free == ~0u) {
				first_free = n;
			}
			continue;
		}
		valid++;
		if ((s2cr >> 16 & 3) == 0) {
			used[s2cr & 0xff] = true;
		}
		for (unsigned i = 0; i < sizeof(ife_streams) / sizeof(ife_streams[0]); i++) {
			if (((stream ^ ife_streams[i]) & ~(mask | IFE_STREAM_MASK) & 0x7fff) == 0) {
				say("camera:   match register %u covers the IFEs' stream %#x: stream %#x mask %#x, s2cr %#x (%s, context %u)\n",
				    n, ife_streams[i], stream, mask, s2cr, kinds[s2cr >> 16 & 3], s2cr & 0xff);
				covering++;
				break;
			}
		}
	}
	for (context = 0; context < contexts && used[context]; context++) {
	}
	for (uint32_t c = 0; c < contexts; c++) {
		unused += !used[c];
	}
	lck_mtx_unlock(&hk_smmu_lock);
	say("camera: the apps SMMU: %u match registers valid, %u free (the first %d); %u of them cover the IFEs' streams; the lowest "
	    "context nothing is sent to %u (%u such)\n", valid, free_n, first_free == ~0u ? -1 : (int)first_free, covering,
	    context, unused);
	for (unsigned spi = CSID_LITE0_SPI; spi <= IFE_LITE0_SPI; spi++) {
		uint32_t intid = 32 + spi;

		say("camera: the GIC: %s's SPI %#x (interrupt %u) is %s\n", spi == CSID_LITE0_SPI ? "CSID-lite 0" : "IFE-lite 0", spi,
		    intid, rd(gicd + GICD_ISENABLER(intid / 32)) & 1u << (intid % 32) ? "ENABLED" : "not enabled");
	}
}

/*
 * ---- the frames' path, step 4 (cam_step=4; docs/TODO.md T07's 3b): the IFEs' streams through a context of ours ----------
 *
 * As redwood's smmu_up()/smmu_down() (bsd/dev/hk_camera.c) and this phone's radio's radio_space_on()/_off() (hk_wifi.c,
 * which the hypervisor granted on 13 October) make it, under hk_smmu_lock: the first free match registers -- four, for
 * Android's own pairs of the IFEs (dt msm_cam_smmu_ife: 0x800, 0x801, 0x840, 0x841, each with mask 0x400, so 0xc00..
 * as well) -- sent to the lowest context no valid match register sends anything to and that is off, translating
 * (never bypass) with page tables of ours that hold one frame's memory and nothing else, from the dt's address pool
 * (qcom,iommu-dma-addr-pool 0x7400000): a write master given a wrong address is refused by the SMMU, it writes no
 * memory of anybody's. The frame's memory is one piece (kmem_alloc_contig, as redwood's for the loader's handing it to
 * the host) between two guard pages the SMMU does not map, each filled with a canary, and so is the frame's last
 * page past the frame; they are looked at before the memory is given back. Every register of the context is kept
 * before it is written and put back after (the context's own off first, the TLB forgotten after); the match registers
 * and their S2CRs too. Here (step 4) nothing streams: the SMMU's taking it is read back, the context's fault status
 * read, and all of it given back. Context 4 (the display's) is never read: it is not free.
 */
#define SMMU_SGFSR              0x048
#define SMMU_CBAR(n)            (4 * (n))               /* in the second page; [17:16] the kind of context */
#define SMMU_CBAR_STAGE_1       (1u << 16 | 3u << 8 | 0xfu << 12)
#define SMMU_CBA2R(n)           (0x800 + 4 * (n))
#define SMMU_CB_SCTLR           0x00                    /* a context's own page: [0] translation on */
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
#define SMMU_SCTLR_ON           (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)   /* faults recorded, not held */
#define SMMU_TCR_39_BITS        (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)
#define SMMU_TCR2               (1u | 1u << 4 | 7u << 15)
#define SMMU_MAIR0              0xf404ff44
#define SMMU_FSR_ALL            0xc00001fe
#define IFE_PTE                 (3ull | 1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)  /* as redwood's CAM_PTE */
#define IFE_ADDRESSES           0x07400000ull   /* (2 MB aligned: a third-level table's first entry is its first page) */
_Static_assert((IFE_ADDRESSES & 0x1fffff) == 0, "IFE_ADDRESSES is not 2 MB aligned");
#define IFE_PAIRS               4
#define SMMU_SPARES             2               /* free match registers more: a refused pair tried again on one of them */
#define IFE_CANARY              0x5a17c0deu

extern uint64_t kvtophys(vm_offset_t va);

static const uint16_t ife_pairs[IFE_PAIRS] = { 0x800, 0x801, 0x840, 0x841 };
static const uint32_t ctx_regs[] = { SMMU_CB_TCR2, SMMU_CB_TTBR0, SMMU_CB_TTBR0 + 4, SMMU_CB_TTBR1, SMMU_CB_TTBR1 + 4, SMMU_CB_TCR,
	                             SMMU_CB_MAIR0, SMMU_CB_MAIR1 };
#define CTX_REGS                (sizeof(ctx_regs) / sizeof(ctx_regs[0]))

static vm_offset_t smmu_global, smmu_second, smmu_ctx, ife_memory, ife_tables;
static uint64_t ife_memory_bytes;       /* a guard page, the header for the host, the frame's pages, a guard page */
static uint32_t ife_frame_bytes, ife_frame_pages, ife_table_count, ife_places;
static uint32_t smmu_ctx_n, smmu_match[IFE_PAIRS + SMMU_SPARES], smmu_old_smr[IFE_PAIRS + SMMU_SPARES],
    smmu_old_s2cr[IFE_PAIRS + SMMU_SPARES];
static uint32_t smmu_old_sctlr, smmu_old_cbar, smmu_old_cba2r, smmu_old_ctx[CTX_REGS];
static unsigned smmu_matches_taken;
static bool smmu_ctx_written;
static uint32_t smmu_pairs_taken;   /* which of ife_pairs[] the hypervisor granted */

/*
 * The frame's memory: one piece -- a guard page, the header the loader hands the host with the frame (redwood's
 * "RWCAM001", tools/camraw.py), the frame's pages, a guard page. Only the frame's pages are in the SMMU's tables; the
 * guard pages and the frame's last page's rest are filled with the canary.
 */
struct cam_header {
	char            magic[8];       /* "RWCAM001" */
	uint32_t        width, height, stride;
	uint32_t        format;         /* 1: MIPI RAW10, packed */
	uint32_t        bayer;          /* 0 RGGB, 1 GRBG, 2 GBRG, 3 BGGR */
	uint32_t        frames, frame_bytes, source;
	char            sensor[16];
	uint32_t        last;
};
extern uint64_t loader_recording[3];
#define IFE_FRAME_AT            (2 * 4096ull)   /* the frame's first byte in ife_memory */

/* (a place's frame pages and the guard page after it: the same in memory, from IFE_FRAME_AT, and at the IFE's addresses) */
#define IFE_SLOT_BYTES          ((ife_frame_pages + 1) * 4096ull)

/* is `at' (an offset in the memory) a canary's: the first guard page, a frame's last page's rest, the guard page after it */
static bool
ife_canary_at(uint64_t at)
{
	return at < 4096 || (at >= IFE_FRAME_AT && (at - IFE_FRAME_AT) % IFE_SLOT_BYTES >= ife_frame_bytes);
}

static bool
ife_memory_alloc(uint32_t frame_bytes, unsigned places)
{
	kern_return_t kr;

	ife_frame_bytes = frame_bytes;
	ife_frame_pages = (frame_bytes + 4095) / 4096;
	ife_places = places;
	ife_memory_bytes = IFE_FRAME_AT + places * IFE_SLOT_BYTES;
	/*
	 * (one frame in one piece: the loader hands it over -- the piece taken at boot where there is one and it holds the
	 * frame, instantnoodle_camera_init(); iOS's places page by page, the SMMU making them whole)
	 */
	ife_memory_early = places == 1 && ife_early != 0 && ife_memory_bytes <= ife_early_bytes;
	if (ife_memory_early) {
		ife_memory = ife_early;
		ife_early = 0;
		bzero((void *)ife_memory, ife_memory_bytes);
		kr = KERN_SUCCESS;
	} else if (places == 1) {
		kr = kmem_alloc_contig(kernel_map, &ife_memory, ife_memory_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT);
	} else {
		kr = kmem_alloc(kernel_map, &ife_memory, ife_memory_bytes, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT);
	}
	if (kr != KERN_SUCCESS) {
		ife_memory = 0;
		say("camera: no memory in one piece for a frame (%llu KB)\n", ife_memory_bytes >> 10);
		return false;
	}
	for (uint64_t at = 0; at < ife_memory_bytes; at += 4) {
		if (ife_canary_at(at)) {
			*(volatile uint32_t *)(ife_memory + at) = IFE_CANARY;
		}
	}
	for (vm_offset_t line = ife_memory; line < ife_memory + ife_memory_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	say("camera: %llu KB at %#llx for %u frame(s) of %u bytes, each followed by a guard page\n", ife_memory_bytes >> 10,
	    kvtophys(ife_memory), places, frame_bytes);
	return true;
}

/* how many of the canary's words are no longer there (the caches forgotten first: what is in memory) */
static uint64_t
ife_canaries_hit(void)
{
	uint64_t hit = 0;

	for (vm_offset_t line = ife_memory; line < ife_memory + ife_memory_bytes; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint64_t at = 0; at < ife_memory_bytes; at += 4) {
		if (ife_canary_at(at) && *(volatile uint32_t *)(ife_memory + at) != IFE_CANARY) {
			hit++;
		}
	}
	return hit;
}

/* where the IFE writes a place: the IFE's addresses mirror the memory's layout from IFE_FRAME_AT, guard pages unmapped */
static uint32_t
ife_place_address(unsigned place)
{
	return (uint32_t)(IFE_ADDRESSES + place * IFE_SLOT_BYTES);
}

/* the header and the frame to the loader, which gives them to the host after the run ("xl camera"); the memory is kept */
static void
ife_hand_over(const struct ctab_mode *mode, uint32_t stride)
{
	struct cam_header *h = (struct cam_header *)(ife_memory + 4096);

	if (ife_tables != 0 && !leaving) {
		kmem_free(kernel_map, ife_tables, 4096ull * ife_table_count);   /* (the SMMU is given back already) */
	}
	ife_tables = 0;
	bzero(h, sizeof(*h));
	memcpy(h->magic, "RWCAM001", 8);
	h->width = mode->width;
	h->height = mode->height;
	h->stride = stride;
	h->format = 1;
	h->bayer = frames_bayer();
	h->frames = 1;
	h->frame_bytes = ife_frame_bytes;
	h->source = 1;
	strlcpy(h->sensor, frames_chip(), sizeof(h->sensor));
	for (vm_offset_t line = ife_memory + 4096; line < ife_memory + 8192; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	loader_recording[0] = kvtophys(ife_memory + 4096);
	loader_recording[1] = 4096 + (uint64_t)ife_frame_pages * 4096;
	loader_recording[2] = (uint64_t)(ife_memory + 4096);
	say("camera: the frame (%ux%u, stride %u, %u bytes) handed to the loader for the host at %#llx (xl camera)\n", mode->width,
	    mode->height, stride, ife_frame_bytes, loader_recording[0]);
}

static void
ife_memory_free(void)
{
	if (ife_memory != 0 && ife_memory_early) {
		ife_early = ife_memory;         /* (the piece taken at boot: kept for the next frame) */
		ife_memory_early = false;
	} else if (ife_memory != 0 && !leaving) {
		kmem_free(kernel_map, ife_memory, ife_memory_bytes);
	}
	if (ife_tables != 0 && !leaving) {
		kmem_free(kernel_map, ife_tables, 4096ull * ife_table_count);
	}
	ife_memory = ife_tables = 0;
}

static bool
smmu_put(vm_offset_t base, uint32_t offset, uint32_t value, uint32_t check)
{
	wr(base + offset, value);
	if ((rd(base + offset) & check) != (value & check)) {
		say("camera: SMMU %#x <- %#x, but it reads %#x\n", offset, value, rd(base + offset));
		give_up("an SMMU register that did not read back");
		return false;
	}
	return true;
}

/* which match registers and which context are free (hk_smmu_lock held); the context's registers kept */
static bool
smmu_claim(void)
{
	bool used[256] = { false };
	uint32_t scr0 = rd(smmu_global + SMMU_SCR0), idr0 = rd(smmu_global + SMMU_IDR0), idr1 = rd(smmu_global + SMMU_IDR1);
	uint32_t matches = idr0 & 0xff, contexts = idr1 & 0xff, page = idr1 >> 31 ? 0x10000 : 0x1000, pages = 2u << (idr1 >> 28 & 7);

	if ((scr0 & 1u << 3) || 2 * pages * page != 0x100000 || page != 0x1000) {
		say("camera: the SMMU is not laid out as known here (scr0 %#x, idr1 %#x)\n", scr0, idr1);
		return false;
	}
	smmu_matches_taken = 0;
	used[4] = true;         /* the display's, as it once was: never read */
	{
		extern uint32_t hk_display_context;

		used[hk_display_context] = true;        /* the display's now, whether or not its stream is sent there: reading it is a CrashDump (instantnoodle_mdp.c) */
	}
	for (uint32_t n = 0; n < matches && SMMU_SMR(n) < 0xc00; n++) {
		uint32_t smr = rd(smmu_global + SMMU_SMR(n)), s2cr = rd(smmu_global + SMMU_S2CR(n));

		if (smr >> 31) {
			for (unsigned i = 0; i < sizeof(ife_streams) / sizeof(ife_streams[0]); i++) {
				if (((ife_streams[i] ^ smr) & ~(smr >> 16) & 0x7fff) == 0) {
					say("camera: match register %u (%#x) takes the IFEs' stream %#x already: left alone, nothing set up\n", n, smr,
					    ife_streams[i]);
					return false;
				}
			}
			used[s2cr & 0xff] = true;
		} else if (smmu_matches_taken < IFE_PAIRS + SMMU_SPARES) {
			smmu_match[smmu_matches_taken] = n;
			smmu_old_smr[smmu_matches_taken] = smr;
			smmu_old_s2cr[smmu_matches_taken] = s2cr;
			smmu_matches_taken++;
		}
	}
	for (smmu_ctx_n = 0; smmu_ctx_n < contexts && used[smmu_ctx_n]; smmu_ctx_n++) {
	}
	if (smmu_matches_taken < IFE_PAIRS + SMMU_SPARES || smmu_ctx_n >= contexts || smmu_ctx_n == 4) {
		say("camera: the SMMU has not %u match registers and a context free\n", IFE_PAIRS);
		smmu_matches_taken = 0;
		return false;
	}
	smmu_second = smmu_global + page;
	smmu_ctx = ml_io_map(APPS_SMMU_PHYS + (pages + smmu_ctx_n) * page, page);
	smmu_old_sctlr = rd(smmu_ctx + SMMU_CB_SCTLR);
	if (smmu_old_sctlr & 1) {
		say("camera: context %u is on although nothing is sent to it: somebody's, left alone\n", smmu_ctx_n);
		smmu_matches_taken = 0;
		return false;
	}
	smmu_old_cbar = rd(smmu_second + SMMU_CBAR(smmu_ctx_n));
	smmu_old_cba2r = rd(smmu_second + SMMU_CBA2R(smmu_ctx_n));
	for (unsigned i = 0; i < CTX_REGS; i++) {
		smmu_old_ctx[i] = rd(smmu_ctx + ctx_regs[i]);
	}
	say("camera: free: match registers %u, %u, %u, %u (spares %u, %u) and context %u (sctlr %#x, cbar %#x, fsr %#x)\n", smmu_match[0],
	    smmu_match[1], smmu_match[2], smmu_match[3], smmu_match[4], smmu_match[5], smmu_ctx_n, smmu_old_sctlr, smmu_old_cbar,
	    rd(smmu_ctx + SMMU_CB_FSR));
	return true;
}

/* the frame's memory behind IFE_ADDRESSES for the IFEs' streams, through the context found, with tables of ours */
static bool
smmu_ife_up(void)
{
	uint32_t slots = (uint32_t)(ife_places * IFE_SLOT_BYTES / 4096), thirds = (slots + 511) / 512;
	uint64_t *root, *second, table_root;

	if (broken || ife_memory == 0) {
		return false;
	}
	smmu_global = ml_io_map(APPS_SMMU_PHYS, 0x2000);
	lck_mtx_lock(&hk_smmu_lock);
	if (!smmu_claim()) {
		lck_mtx_unlock(&hk_smmu_lock);
		give_up("no place in the SMMU");
		return false;
	}
	ife_table_count = 2 + thirds;
	if (kmem_alloc(kernel_map, &ife_tables, 4096ull * ife_table_count, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		ife_tables = 0;
		smmu_matches_taken = 0;
		lck_mtx_unlock(&hk_smmu_lock);
		give_up("no memory for page tables");
		return false;
	}
	root = (uint64_t *)ife_tables;
	second = (uint64_t *)(ife_tables + 4096);
	root[IFE_ADDRESSES >> 30 & 0x1ff] = kvtophys((vm_offset_t)second) | 3;
	for (uint32_t t = 0; t < thirds; t++) {
		uint64_t *third = (uint64_t *)(ife_tables + (2 + t) * 4096ull);

		second[(IFE_ADDRESSES >> 21 & 0x1ff) + t] = kvtophys((vm_offset_t)third) | 3;
		for (uint32_t p = 0; p < 512 && t * 512 + p < slots; p++) {
			uint32_t v = t * 512 + p;

			if (v % (ife_frame_pages + 1) != ife_frame_pages) {     /* (a place's guard page: not mapped) */
				third[p] = kvtophys(ife_memory + IFE_FRAME_AT + 4096ull * v) | IFE_PTE;
			}
		}
	}
	for (vm_offset_t line = ife_tables; line < ife_tables + 4096ull * ife_table_count; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	table_root = kvtophys(ife_tables) | (uint64_t)smmu_ctx_n << 48;
	say("camera: SMMU: context %u gets tables of ours (root %#llx) for %u x %u pages at %#llx; match registers %u-%u send the IFEs' "
	    "pairs %#x/%#x/%#x/%#x (mask 0x400) through it, translated\n", smmu_ctx_n, kvtophys(ife_tables), ife_places, ife_frame_pages,
	    IFE_ADDRESSES,
	    smmu_match[0], smmu_match[3], ife_pairs[0], ife_pairs[1], ife_pairs[2], ife_pairs[3]);
	smmu_ctx_written = true;
	if (!smmu_put(smmu_ctx, SMMU_CB_SCTLR, 0, ~0u) || !smmu_put(smmu_second, SMMU_CBA2R(smmu_ctx_n), 1, 1) ||
	    !smmu_put(smmu_second, SMMU_CBAR(smmu_ctx_n), SMMU_CBAR_STAGE_1, 0x30000) ||
	    !smmu_put(smmu_ctx, SMMU_CB_TCR2, SMMU_TCR2, 0) || !smmu_put(smmu_ctx, SMMU_CB_TCR, SMMU_TCR_39_BITS, ~0u) ||
	    !smmu_put(smmu_ctx, SMMU_CB_TTBR0, (uint32_t)table_root, ~0u) ||
	    !smmu_put(smmu_ctx, SMMU_CB_TTBR0 + 4, (uint32_t)(table_root >> 32), ~0u) ||
	    !smmu_put(smmu_ctx, SMMU_CB_TTBR1, 0, ~0u) || !smmu_put(smmu_ctx, SMMU_CB_TTBR1 + 4, 0, ~0u) ||
	    !smmu_put(smmu_ctx, SMMU_CB_MAIR0, SMMU_MAIR0, ~0u) || !smmu_put(smmu_ctx, SMMU_CB_MAIR1, 0, ~0u)) {
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	wr(smmu_ctx + SMMU_CB_FSR, SMMU_FSR_ALL);
	wr(smmu_ctx + SMMU_CB_TLBIASID, smmu_ctx_n);
	wr(smmu_ctx + SMMU_CB_TLBSYNC, 1);
	if (!wait_bits(smmu_ctx + SMMU_CB_TLBSTATUS, 1, 0, 10000)) {
		lck_mtx_unlock(&hk_smmu_lock);
		give_up("the SMMU does not finish forgetting");
		return false;
	}
	if (!smmu_put(smmu_ctx, SMMU_CB_SCTLR, SMMU_SCTLR_ON, 1)) {
		lck_mtx_unlock(&hk_smmu_lock);
		return false;
	}
	/*
	 * Each of Android's pairs on its own: the hypervisor answers for the match registers and does not grant every stream
	 * (run 092248: 0x800/0x400 taken, 0x801/0x400 read back 0) -- a pair it does not take is given back at once
	 */
	smmu_pairs_taken = 0;
	for (unsigned i = 0; i < IFE_PAIRS; i++) {
		uint32_t want = 1u << 31 | 0x400u << 16 | ife_pairs[i];

		wr(smmu_global + SMMU_S2CR(smmu_match[i]), smmu_ctx_n);
		wr(smmu_global + SMMU_SMR(smmu_match[i]), want);
		if (rd(smmu_global + SMMU_SMR(smmu_match[i])) == want && (rd(smmu_global + SMMU_S2CR(smmu_match[i])) & 0x300ff) == smmu_ctx_n) {
			smmu_pairs_taken |= 1u << i;
			continue;
		}
		say("camera: the hypervisor did not grant the IFEs' pair %#x/0x400 (match register %u reads %#x, s2cr %#x): given back\n",
		    ife_pairs[i], smmu_match[i], rd(smmu_global + SMMU_SMR(smmu_match[i])), rd(smmu_global + SMMU_S2CR(smmu_match[i])));
		wr(smmu_global + SMMU_SMR(smmu_match[i]), smmu_old_smr[i]);
		wr(smmu_global + SMMU_S2CR(smmu_match[i]), smmu_old_s2cr[i]);
		/* once more on a spare: taken there, the register was the hypervisor's; refused again, the stream is */
		for (unsigned k = IFE_PAIRS; k < smmu_matches_taken; k++) {
			uint32_t m = smmu_match[k];

			if (rd(smmu_global + SMMU_SMR(m)) >> 31) {
				continue;               /* (this spare took an earlier pair) */
			}
			wr(smmu_global + SMMU_S2CR(m), smmu_ctx_n);
			wr(smmu_global + SMMU_SMR(m), want);
			if (rd(smmu_global + SMMU_SMR(m)) == want && (rd(smmu_global + SMMU_S2CR(m)) & 0x300ff) == smmu_ctx_n) {
				smmu_pairs_taken |= 1u << i;
				say("camera: ... but match register %u took it: register %u is the hypervisor's, not the stream\n", m,
				    smmu_match[i]);
			} else {
				say("camera: ... and match register %u refused it too: the stream %#x is not granted\n", m, ife_pairs[i]);
				wr(smmu_global + SMMU_SMR(m), smmu_old_smr[k]);
				wr(smmu_global + SMMU_S2CR(m), smmu_old_s2cr[k]);
			}
			break;
		}
	}
	lck_mtx_unlock(&hk_smmu_lock);
	if (smmu_pairs_taken == 0) {
		give_up("the hypervisor grants none of the IFEs' streams");
		return false;
	}
	say("camera: the SMMU took it: the IFEs' pairs%s%s%s%s go through context %u (sctlr %#x, ttbr0 %#x%08x), fsr %#x\n",
	    smmu_pairs_taken & 1 ? " 0x800" : "", smmu_pairs_taken & 2 ? " 0x801" : "", smmu_pairs_taken & 4 ? " 0x840" : "",
	    smmu_pairs_taken & 8 ? " 0x841" : "", smmu_ctx_n, rd(smmu_ctx + SMMU_CB_SCTLR), rd(smmu_ctx + SMMU_CB_TTBR0 + 4),
	    rd(smmu_ctx + SMMU_CB_TTBR0), rd(smmu_ctx + SMMU_CB_FSR));
	return true;
}

/* what the context recorded (its own fault status only: no global register past the ones read above) */
static uint32_t
smmu_ife_faults(void)
{
	uint32_t status;

	if (!smmu_ctx_written) {
		return 0;
	}
	status = rd(smmu_ctx + SMMU_CB_FSR);
	if (status & SMMU_FSR_ALL) {
		say("camera: THE SMMU REFUSED THE IFE: context %u fsr %#x, address %#llx, syndrome %#x\n", smmu_ctx_n, status,
		    (uint64_t)rd(smmu_ctx + SMMU_CB_FAR + 4) << 32 | rd(smmu_ctx + SMMU_CB_FAR), rd(smmu_ctx + SMMU_CB_FSYNR0));
	} else {
		say("camera: the SMMU refused the IFE nothing (context %u, fsr %#x)\n", smmu_ctx_n, status);
	}
	return status & SMMU_FSR_ALL;
}

/* the match registers sending nothing, the context as found, its translations forgotten (no lock on a panic's way out) */
static void
smmu_ife_down(void)
{
	if (smmu_matches_taken == 0 && !smmu_ctx_written) {
		return;
	}
	(void)smmu_ife_faults();
	if (!leaving) {
		lck_mtx_lock(&hk_smmu_lock);
	}
	for (unsigned i = smmu_matches_taken; i-- > 0;) {
		wr(smmu_global + SMMU_SMR(smmu_match[i]), smmu_old_smr[i]);
		wr(smmu_global + SMMU_S2CR(smmu_match[i]), smmu_old_s2cr[i]);
	}
	if (smmu_ctx_written) {
		wr(smmu_ctx + SMMU_CB_SCTLR, 0);
		wr(smmu_ctx + SMMU_CB_FSR, SMMU_FSR_ALL);
		for (unsigned i = 0; i < CTX_REGS; i++) {
			wr(smmu_ctx + ctx_regs[i], smmu_old_ctx[i]);
		}
		wr(smmu_second + SMMU_CBAR(smmu_ctx_n), smmu_old_cbar);
		wr(smmu_second + SMMU_CBA2R(smmu_ctx_n), smmu_old_cba2r);
		wr(smmu_ctx + SMMU_CB_TLBIASID, smmu_ctx_n);
		wr(smmu_ctx + SMMU_CB_TLBSYNC, 1);
		(void)wait_bits(smmu_ctx + SMMU_CB_TLBSTATUS, 1, 0, 10000);
		wr(smmu_ctx + SMMU_CB_SCTLR, smmu_old_sctlr);
	}
	if (!leaving) {
		lck_mtx_unlock(&hk_smmu_lock);
	}
	{
		unsigned kept = 0;

		for (unsigned i = 0; i < smmu_matches_taken; i++) {
			if (rd(smmu_global + SMMU_SMR(smmu_match[i])) != smmu_old_smr[i] ||
			    rd(smmu_global + SMMU_S2CR(smmu_match[i])) != smmu_old_s2cr[i]) {
				say("camera: the SMMU kept match register %u: smr %#x s2cr %#x (found %#x %#x)\n", smmu_match[i],
				    rd(smmu_global + SMMU_SMR(smmu_match[i])), rd(smmu_global + SMMU_S2CR(smmu_match[i])), smmu_old_smr[i],
				    smmu_old_s2cr[i]);
				kept++;
			}
		}
		if (smmu_ctx_written) {
			for (unsigned i = 0; i < CTX_REGS; i++) {
				if (rd(smmu_ctx + ctx_regs[i]) != smmu_old_ctx[i]) {
					say("camera: the SMMU kept context %u's %#x: %#x (found %#x)\n", smmu_ctx_n, ctx_regs[i], rd(smmu_ctx + ctx_regs[i]),
					    smmu_old_ctx[i]);
					kept++;
				}
			}
			if (rd(smmu_second + SMMU_CBAR(smmu_ctx_n)) != smmu_old_cbar || rd(smmu_second + SMMU_CBA2R(smmu_ctx_n)) != smmu_old_cba2r) {
				/* (the hypervisor keeps a stage-1 CBAR when 0, a stage-2 kind, is written: runs 092248, 092704) */
				say("camera: the SMMU kept context %u's cbar %#x, cba2r %#x (found %#x, %#x)\n", smmu_ctx_n,
				    rd(smmu_second + SMMU_CBAR(smmu_ctx_n)), rd(smmu_second + SMMU_CBA2R(smmu_ctx_n)), smmu_old_cbar, smmu_old_cba2r);
				kept++;
			}
			if (rd(smmu_ctx + SMMU_CB_SCTLR) != smmu_old_sctlr) {
				say("camera: the SMMU kept context %u's sctlr %#x (found %#x)\n", smmu_ctx_n, rd(smmu_ctx + SMMU_CB_SCTLR), smmu_old_sctlr);
				kept++;
			}
		}
		say("camera: the SMMU's match registers and context %u given back: %u register(s) not as found (told above)\n", smmu_ctx_n,
		    kept);
	}
	say("camera: the SMMU's match registers %u-%u and context %u back as found (smr %#x, sctlr %#x)\n", smmu_match[0],
	    smmu_match[smmu_matches_taken ? smmu_matches_taken - 1 : 0], smmu_ctx_n, rd(smmu_global + SMMU_SMR(smmu_match[0])),
	    rd(smmu_ctx + SMMU_CB_SCTLR));
	smmu_matches_taken = 0;
	smmu_ctx_written = false;
}

/* cam_step=4: the SMMU set up for the IFEs and the frame's memory, nothing streamed; all given back */
static void
frames_smmu_set(void)
{
	uint64_t hit;

	if (!ife_memory_alloc(2912 * 1748, 1)) {
		return;
	}
	(void)smmu_ife_up();
	smmu_ife_down();                /* (the context's fault status read there) */
	hit = ife_canaries_hit();
	say("camera: the guard pages and the frame's last page's rest: %llu word(s) changed%s\n", hit, hit ? " -- SOMETHING WROTE THERE" :
	    "");
	ife_memory_free();
}

/*
 * cam=3 (the header comment): step 1 -- the camera subsystem's clocks and supplies up, then the first accesses of
 * CSIPHY 2 and CSID-lite 0, their versions (as camss-csiphy-3ph-1-0.c's csiphy_hw_version_read() and camss-csid.c's
 * csid_hw_version(): CTRL6's "show the revision", then four status bytes; CTRL6 back as found), then all down again.
 */
static void
frames(void)
{
	uint32_t step = 1, ctrl6, phy = 0, csid;

	(void)PE_parse_boot_argn("cam_step", &step, sizeof(step));
	say("camera: cam=3 step %u%s (the %s sensor's)\n", step, step > 5 ? ": only steps 1 to 5 are there yet" : step == 5 ?
	    ": one frame into memory through CSID-lite 0, IFE-lite 0 and the SMMU" : step == 4 ? ": the SMMU set up for "
	    "the IFEs and a frame's memory, nothing streamed" : step == 3 ? ": what the SMMU and "
	    "the GIC have for the camera, read only" : step == 2 ? ": the sensor's packets counted by CSID-lite 0" :
	    ": the camera subsystem, CSIPHY 2's and CSID-lite 0's versions", frames_sensor == 0 ? "wide" : frames_sensor == 1 ?
	    "ultra-wide" : "front");
	if (step == 3) {
		frames_smmu_look();
		return;
	}
	if (step == 4) {
		frames_smmu_set();
		return;
	}
	if (step == 2 || step == 5) {
		frames_stream(step == 5);
		return;
	}
	if (titan_up()) {
		ctrl6 = rd(csiphy2 + CSIPHY_COMMON_CTRL6);
		wr(csiphy2 + CSIPHY_COMMON_CTRL6, 2);
		for (unsigned n = 0; n < 4; n++) {
			phy |= (rd(csiphy2 + CSIPHY_COMMON_STATUS(12 + n)) & 0xff) << (8 * n);
		}
		wr(csiphy2 + CSIPHY_COMMON_CTRL6, ctrl6);
		csid = rd(ife_lite0 + CSID_LITE0 + CSID_HW_VERSION);
		say("camera: CSIPHY 2: revision %#010x (CTRL6 %#x as found, back so); CSID-lite 0: version %#010x (generation %u, "
		    "revision %#x, stepping %#x)\n", phy, ctrl6, csid, csid >> 28, (csid >> 16) & 0xfff, csid & 0xffff);
	}
	power_down();
}

/*
 * ---- cam=5: the front camera for iOS (/dev/hk-camera, sys/hk_camera.h) ------------------------------------------------------
 *
 * As redwood's (bsd/dev/hk_camera.c: serve_ios(), ios_session(), camdev_*()) and for the same programs (libhackios-media.dylib
 * in mediaserverd, camera_test): the front IMX471's mode 1 (2328x1748 at 30 fps) for as long as the node is held open --
 * powered up and streaming at the first read after the open, everything down again at the last close, after IOS_IDLE_MS
 * without a read, or on a fault (and then for the rest of the run). Its frames go into RING places of kernel memory, each at
 * its own addresses of the IFE's through our SMMU context with an unmapped guard page after it (step 4's; a write past a
 * place is refused, not written): one the write master writes, one a read may be copying, one ready. At each start of
 * frame (RDI 0's, polled every millisecond; the GIC's SPIs stay off) the address of the place for the frame after it goes to
 * the write master, taken at the next one; a buffer done makes the place it wrote ready. Exposure as the program asks, in
 * linear gain (IOS_GAIN_UNITY is 1x) made Sony's code (1024 - 1024 * 1024 / gain), written between two frames in the
 * sensor's hold; it counts from the second frame after. On any way down the DMA is stopped first (power_down()).
 */
#define RING                    3
#define IOS_IDLE_MS             1000
#define IOS_FRAME_TIMEOUT_MS    1500
#define IOS_GAIN_UNITY          1024
#define IOS_GAIN_MAX            (16 * 1024)
#define IOS_MARGIN              10
#define IOS_FRAME_LINES_MAX     0xfff0
#define CAM_KEEP_MB             24              /* memory in one piece for a picture for the host (HK_CAMERA_KEEP) */

enum { PLACE_FREE, PLACE_WRITING, PLACE_READY, PLACE_READING };

static LCK_GRP_DECLARE(ios_lck_grp, "hk-camera");
static LCK_MTX_DECLARE(ios_lock, &ios_lck_grp);
static struct {
	uint8_t         state;
	uint64_t        sequence, time;
	uint32_t        exposure_lines, gain, frame_lines;
	uint32_t        mode;           /* the sensor's mode the frame was taken in (frames_mode then) */
	uint32_t        focus;          /* the lens's position then (act_position()) */
} place[RING];
static uint32_t ios_line_ns;
static volatile bool ios_open, ios_streaming, ios_wanted;  /* open; frames coming; a read wants them */
/*
 * Which camera's frames (sys/hk_camera.h's numbers: 1 the wide IMX586, 2 the front IMX471), as the program asks with
 * HK_CAMERA_SENSOR: a session of the other camera ends (its DMA stopped first) and the next is this one's -- the program reads
 * on, its frames saying whose they are, as redwood's protocol has it (camera_device.c's worker asks and waits).
 */
static volatile uint32_t ios_sensor_wanted = 2, ios_mode_wanted = 1;
static volatile bool ios_switch;
/*
 * The wide one's other mode of the same frame (1 and 6, both 4000x3000 RAW10 on the same three trios)
 * in the session, not in a new one (ios_mode_change()): the stream off at a frame's end, the frame the sensor may have begun
 * waited out, the mode's table and the exposure written in its standby, the stream on; power, CSIPHY 0, CSID-lite 0, the
 * write master, the SMMU and the ring left as they are. A new session -- all of it down and up again, the init table's
 * 1709 writes -- left the preview without a frame for 2.6-2.7 s (run 133140: 2731 ms to mode 6, 2598 back to mode 1); in the
 * session the first frame of the other mode is done 265-280 ms after the ask (run 133840, four changes, each step's time said).
 * Boot-arg cam_quickmode=0: a new session for each mode, as before.
 */
static uint32_t ios_quick_modes = 1;
static volatile bool ios_mode_quick;    /* a mode asked for, to be changed in the session */
static uint64_t ios_mode_asked_ms;      /* when it was asked for (ios_ms()) */
static bool ios_mode_first;             /* the first frame of the changed mode not done yet */
/*
 * Boot-arg cam_keep=N (seconds; 0, the default: off): a session ended by its reader (closed, or nobody reading) leaves its
 * sensor up -- in standby, stream off, the DMA stopped and VFE-lite reset as ever, its power, MCLK, the camera subsystem's
 * clocks, its init table, the lens where it was, the ring and the SMMU's tables as they were -- and the next session of
 * the same sensor and frame starts from there: no power-up, no chip ID, no lens start, no init table (1709 writes for the
 * IMX586, 643 ms at the least), no res table for the same mode; only the blocks set again, the exposure and stream on.
 * Powered down as ever (stream_down(), power_down(), the memory given back) when N seconds have gone without a session,
 * another sensor or frame is asked for, the thermal service reports 95 C or heavy pressure, a fault, or the way out
 * (instantnoodle_camera_quiesce(): power_down() of all that was switched on, the lens parked first). All of it on the
 * camera thread (serve_ios()): a session and the idle power-down never at once. Built, not yet run on the phone: the owner
 * put the speed-ups last, with the DSP and the Titan 480's own CCI engine ("我覺得速度優化放在最後和dsp優化在一起吧") --
 * docs/TODO.md T07 has the supervisor's conditions for its first run.
 */
static uint32_t ios_keep_secs;
static bool ios_kept;
static unsigned ios_kept_sensor, ios_kept_mode;
static uint64_t ios_kept_since;
static uint64_t ios_last_read, ios_given, ios_frames_done, ios_writes_late;
static unsigned ios_readers;            /* reads copying out of a place: the memory is not given back under them */
static struct { uint32_t lines, gain, frame_lines; bool pending; } ios_ask;
static struct { uint32_t lines, gain, frame_lines; uint64_t from; } ios_now, ios_next;
static vm_offset_t ios_keep;
static uint64_t ios_keep_bytes;

static uint64_t
ios_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000;
}

/* what the sensor can take: lines within the frame less IOS_MARGIN, the frame at least the mode's, gain 1x to 16x */
static void
ios_limits(uint32_t *lines, uint32_t *gain, uint32_t *frame_lines)
{
	uint32_t shortest = st.mode->frame_length;

	*lines = *lines == 0 ? 1 : *lines;
	*frame_lines = *frame_lines == 0 ? MAX(shortest, *lines + IOS_MARGIN) : *frame_lines;
	*frame_lines = MIN(MAX(*frame_lines, shortest), IOS_FRAME_LINES_MAX);
	*lines = MIN(*lines, *frame_lines - IOS_MARGIN);
	*gain = MIN(MAX(*gain, IOS_GAIN_UNITY), IOS_GAIN_MAX);
}

/* SMIA in the sensor's hold: the frame's length 0x0340, the exposure 0x0202, the analog gain 0x0204 (Sony's code) */
static bool
ios_exposure_write(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	uint32_t code = 1024 - (1024u * 1024u) / gain;

	return sensor_table("holdon") && sensor_reg16(0x0340, (uint16_t)frame_lines) && sensor_reg16(0x0202, (uint16_t)lines) &&
	       sensor_reg16(0x0204, (uint16_t)code) && sensor_table("holdoff");
}

/* (stream_up()'s, before "on": what the program asked for, else the boot-args', else a room's -- 8x, the longest exposure) */
static bool
ios_exposure_first(void)
{
	uint32_t lines, gain, frame_lines;

	lck_mtx_lock(&ios_lock);
	lines = ios_ask.lines;
	gain = ios_ask.gain;
	frame_lines = ios_ask.frame_lines;
	ios_ask.pending = false;
	lck_mtx_unlock(&ios_lock);
	ios_limits(&lines, &gain, &frame_lines);
	if (!ios_exposure_write(lines, gain, frame_lines)) {
		return false;
	}
	ios_now.lines = ios_next.lines = lines;
	ios_now.gain = ios_next.gain = gain;
	ios_now.frame_lines = ios_next.frame_lines = frame_lines;
	ios_next.from = 0;
	say("camera: the frames for iOS: exposure %u lines, gain %u/1024 (code %#x), frame %u lines\n", lines, gain,
	    1024 - (1024u * 1024u) / gain, frame_lines);
	return true;
}

/* a kept sensor (above) powered down as any session's end does it */
static void
ios_release_kept(const char *why)
{
	uint64_t was = ios_ms() - ios_kept_since;

	if (!ios_kept) {
		return;
	}
	ios_kept = false;
	stream_down();          /* (the DMA stopped already; the PHY, VFE-lite and CSID-lite as found; power_down(): the lens parked first) */
	ife_memory_free();
	say("camera: the kept %s sensor powered down after %llu ms in standby (%s)\n", hw->name, was, why);
}

/* (bsd/kern/kern_newsysctl.c's, declared there only) */
int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);

/* the thermal service's word: 95 C or more anywhere, or heavy pressure (kern.hackios.thermal_*) */
static bool
ios_too_hot(void)
{
	int hottest = 0, pressure = 0;
	size_t a = sizeof(hottest), b = sizeof(pressure);

	if (kernel_sysctlbyname("kern.hackios.thermal_hottest", &hottest, &a, NULL, 0) != 0) {
		hottest = 0;
	}
	if (kernel_sysctlbyname("kern.hackios.thermal_pressure", &pressure, &b, NULL, 0) != 0) {
		pressure = 0;
	}
	return hottest >= 950 || pressure >= 2;
}

static void
ios_stop(const char *why)
{
	uint64_t hit;
	bool keep = ios_keep_secs != 0 && !broken && !ios_switch && st.streaming && !ios_too_hot();

	if (keep) {
		/* (the frames stop, the sensor stays up: ios_keep_secs) */
		lck_mtx_lock(&ios_lock);
		ios_streaming = false;
		for (int i = 0; i < RING; i++) {
			place[i].state = PLACE_FREE;
		}
		lck_mtx_unlock(&ios_lock);
		wakeup(&ios_frames_done);
		ife_dma_stop();
		(void)sensor_table("off");
		st.streaming = false;
		hit = ife_canaries_hit();
		ios_kept = true;
		ios_kept_sensor = frames_sensor;
		ios_kept_mode = frames_mode;
		ios_kept_since = ios_ms();
		say("camera: the frames for iOS stopped (%s): %llu done, %llu given, %llu written over; guard pages: %llu word(s) changed%s; "
		    "the %s sensor kept up in standby for %u s\n", why, ios_frames_done, ios_given, ios_writes_late, hit,
		    hit ? " -- SOMETHING WROTE THERE" : "", hw->name, ios_keep_secs);
		return;
	}
	lck_mtx_lock(&ios_lock);
	ios_streaming = false;
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
	}
	lck_mtx_unlock(&ios_lock);
	wakeup(&ios_frames_done);
	stream_down();                  /* (the write master first, then the sensor, the blocks, the SMMU) */
	hit = ife_memory != 0 ? ife_canaries_hit() : 0;
	/* the memory given back only when no read is copying out of it (at most a second: a read copies 5 MB in milliseconds) */
	lck_mtx_lock(&ios_lock);
	for (unsigned waited = 0; ios_readers != 0 && waited < 10; waited++) {
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };

		(void)msleep(&ios_readers, &ios_lock, PZERO, "hk-camera-readers", &ts);
	}
	lck_mtx_unlock(&ios_lock);
	if (ios_readers != 0) {
		say("camera: %u read(s) still copying after a second: the frames' memory is kept\n", ios_readers);
	} else {
		ife_memory_free();
	}
	say("camera: the frames for iOS stopped (%s): %llu done, %llu given, %llu written over; guard pages: %llu word(s) changed%s\n", why,
	    ios_frames_done, ios_given, ios_writes_late, hit, hit ? " -- SOMETHING WROTE THERE" : "");
}

static bool
ios_start(void)
{
	bool kept = false, res_too = true;

	ios_frames_done = ios_given = ios_writes_late = 0;
	if (ios_kept && ios_kept_sensor == frames_sensor) {
		const struct ctab_mode *to = &((const struct ctab_mode *)(ctab + 1))[frames_mode];

		if (frames_mode == ios_kept_mode) {
			kept = true;
			res_too = false;
		} else if (frames_mode < ctab->modes && to->width == st.mode->width && to->height == st.mode->height &&
		    to->data_type == st.mode->data_type && to->cphy == st.mode->cphy && to->lanes == st.mode->lanes &&
		    to->pixel_clock == st.mode->pixel_clock) {
			kept = true;
			st.mode = to;
		}
	}
	if (ios_kept && !kept) {
		ios_release_kept(ios_kept_sensor != frames_sensor ? "another sensor asked for" : "another frame asked for");
	}
	if (kept) {
		say("camera: the %s sensor as it was kept (%llu ms in standby): power, chip ID, lens, init table%s skipped\n", hw->name,
		    ios_ms() - ios_kept_since, res_too ? "" : " and res table");
		ios_kept = false;
	} else if (!stream_mode(true) || !ife_memory_alloc(st.stride * st.mode->height, RING) || !smmu_ife_up()) {
		return false;
	}
	ios_line_ns = (uint32_t)(1000000000000ull / ((uint64_t)st.mode->fps_milli * st.mode->frame_length));
	lck_mtx_lock(&ios_lock);
	for (int i = 0; i < RING; i++) {
		place[i].state = PLACE_FREE;
		place[i].sequence = 0;
	}
	place[0].state = PLACE_WRITING;
	lck_mtx_unlock(&ios_lock);
	/* every frame written (drop period 0), to place 0 first */
	return stream_up(true, ife_place_address(0), 0, true, kept, res_too);
}

/*
 * (ios_quick_modes, above) The wide sensor to the mode asked for, at a frame's end (ios_session()), without a new session --
 * where the mode's frame is not the one streaming (another size, rate or PHY), a new session after all. The exposure kept
 * in time, not in lines: the modes' lines may be of other lengths. Each step's time after the ask is said.
 */
static bool
ios_mode_change(void)
{
	vm_offset_t csid = ife_lite0 + CSID_LITE0;
	const struct ctab_mode *to;
	uint32_t want, from = frames_mode, lines, gain, frame_lines, line_ns;
	uint64_t asked, off_ms, waited, res_ms;
	char res[8];

	lck_mtx_lock(&ios_lock);
	want = ios_mode_wanted;
	asked = ios_mode_asked_ms;
	ios_mode_quick = false;
	lck_mtx_unlock(&ios_lock);
	if (want == frames_mode) {
		return true;            /* (asked for and asked back before it was made) */
	}
	to = &((const struct ctab_mode *)(ctab + 1))[want];
	if (want >= ctab->modes || to->width != st.mode->width || to->height != st.mode->height || to->data_type != st.mode->data_type ||
	    to->cphy != st.mode->cphy || to->lanes != st.mode->lanes || to->pixel_clock != st.mode->pixel_clock || to->fps_milli == 0 ||
	    to->frame_length == 0) {
		say("camera: mode %u is not mode %u's frame: a new session for it\n", want, from);
		lck_mtx_lock(&ios_lock);
		ios_switch = true;
		lck_mtx_unlock(&ios_lock);
		return true;
	}
	if (!sensor_table("off")) {
		return false;
	}
	off_ms = ios_ms();
	/* the frame the sensor may have begun before it stops: its time waited out, and its start and end let go unseen */
	waited = (uint64_t)ios_now.frame_lines * ios_line_ns / 1000000 + 5;
	wait_ms((unsigned)waited);
	wr(csid + CSID_RDI0_IRQ_CLEAR, 1u << 12);
	wr(csid + CSID_IRQ_CMD, 1);
	wr(ife_lite0 + VFE_BUS_IRQ_CLEAR0, 1u << 4 | 1u);
	wr(ife_lite0 + VFE_BUS_IRQ_CLEAR_ALL, 1);
	snprintf(res, sizeof(res), "res%u", want);
	if (!sensor_table(res)) {
		return false;
	}
	res_ms = ios_ms();
	line_ns = (uint32_t)(1000000000000ull / ((uint64_t)to->fps_milli * to->frame_length));
	lines = (uint32_t)((uint64_t)ios_now.lines * ios_line_ns / line_ns);
	frame_lines = (uint32_t)((uint64_t)ios_now.frame_lines * ios_line_ns / line_ns);
	gain = ios_now.gain;
	st.mode = to;
	ios_limits(&lines, &gain, &frame_lines);
	if (!ios_exposure_write(lines, gain, frame_lines)) {
		return false;
	}
	lck_mtx_lock(&ios_lock);
	frames_mode = want;
	ios_line_ns = line_ns;
	ios_now.lines = ios_next.lines = lines;
	ios_now.gain = ios_next.gain = gain;
	ios_now.frame_lines = ios_next.frame_lines = frame_lines;
	ios_next.from = 0;
	ios_mode_first = true;
	lck_mtx_unlock(&ios_lock);
	if (!sensor_table("on")) {
		return false;
	}
	say("camera: mode %u to %u in the session: stream off %llu ms after it was asked for, %llu ms waited for the frame begun, "
	    "res%u written at %llu ms, streaming at %llu ms (exposure %u lines, gain %u/1024, frame %u lines)\n", from, want,
	    off_ms - asked, waited, want, res_ms - asked, ios_ms() - asked, lines, gain, frame_lines);
	return true;
}

static void
ios_session(void)
{
	vm_offset_t csid = ife_lite0 + CSID_LITE0;
	int writing = 0, next = 0;      /* (place 0's address is the write master's: taken at the first start of frame) */
	uint64_t last_frame;

	if (!ios_start()) {
		give_up("the camera did not start");
		ios_stop("it did not start");
		return;
	}
	last_frame = ios_ms();
	wr(csid + CSID_RDI0_IRQ_CLEAR, 0xffffffff);
	wr(csid + CSID_IRQ_CMD, 1);
	wr(ife_lite0 + VFE_BUS_IRQ_CLEAR0, 0xffffffff);
	wr(ife_lite0 + VFE_BUS_IRQ_CLEAR_ALL, 1);
	lck_mtx_lock(&ios_lock);
	ios_streaming = true;
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	while (!broken && ios_open && !ios_switch) {
		uint32_t rdi, bus, lines, gain, frame_lines;
		uint64_t now;
		bool sof, done, ask;

		wait_ms(1);
		now = ios_ms();
		rdi = rd(csid + CSID_RDI0_IRQ_STATUS);
		bus = rd(ife_lite0 + VFE_BUS_IRQ_STATUS0);
		sof = rdi & 1u << 12;
		done = bus & 1u << 4;
		if (!sof && !done) {
			if (now - last_frame > IOS_FRAME_TIMEOUT_MS) {
				stream_report();
				give_up("no frame for 1.5 s");
				break;
			}
			if (now - ios_last_read > IOS_IDLE_MS) {
				break;
			}
			continue;
		}
		if (sof) {
			wr(csid + CSID_RDI0_IRQ_CLEAR, 1u << 12);
			wr(csid + CSID_IRQ_CMD, 1);
		}
		if (done) {
			wr(ife_lite0 + VFE_BUS_IRQ_CLEAR0, 1u << 4 | 1u);
			wr(ife_lite0 + VFE_BUS_IRQ_CLEAR_ALL, 1);
		}
		lck_mtx_lock(&ios_lock);
		if (done) {
			last_frame = now;
			ios_frames_done++;
			if (ios_frames_done >= ios_next.from && ios_next.from != 0) {
				ios_now = ios_next;
				ios_next.from = 0;
			}
			place[writing].state = PLACE_READY;
			place[writing].sequence = ios_frames_done;
			place[writing].time = mach_absolute_time();
			place[writing].exposure_lines = ios_now.lines;
			place[writing].gain = ios_now.gain;
			place[writing].frame_lines = ios_now.frame_lines;
			place[writing].mode = frames_mode;
			place[writing].focus = act_position();
		}
		if (sof) {
			if (next >= 0) {
				writing = next;         /* (the address given at the last start of frame, taken at this one) */
				next = -1;
			} else {
				ios_writes_late++;      /* no address waiting: this frame goes where the last one went */
			}
			place[writing].state = PLACE_WRITING;
			/* the address for the frame after this one: never a place a read is copying */
			for (int i = 0; i < RING; i++) {
				int c = (writing + 1 + i) % RING;

				if (c != writing && place[c].state != PLACE_READING && (next < 0 || place[c].sequence < place[next].sequence)) {
					next = c;
				}
			}
			if (next >= 0) {
				wr(ife_lite0 + VFE_WM0_IMAGE_ADDR, ife_place_address((unsigned)next));
				wr(ife_lite0 + VFE_LITE_REG_UPDATE, 1);
			}
		}
		ask = done && ios_ask.pending;
		lines = ios_ask.lines;
		gain = ios_ask.gain;
		frame_lines = ios_ask.frame_lines;
		ios_ask.pending = ios_ask.pending && !ask;
		lck_mtx_unlock(&ios_lock);
		if (done) {
			wakeup(&ios_frames_done);
		}
		if (done && ios_mode_first) {
			ios_mode_first = false;
			say("camera: the first frame in mode %u done %llu ms after it was asked for\n", frames_mode, now - ios_mode_asked_ms);
		}
		if (ask) {
			ios_limits(&lines, &gain, &frame_lines);
			if (lines != ios_now.lines || gain != ios_now.gain || frame_lines != ios_now.frame_lines) {
				if (!ios_exposure_write(lines, gain, frame_lines)) {
					break;
				}
				ios_next.lines = lines;
				ios_next.gain = gain;
				ios_next.frame_lines = frame_lines;
				ios_next.from = ios_frames_done + 2;
			}
		}
		if (done) {
			act_step();
		}
		if (done && ios_mode_quick && !ios_mode_change()) {
			break;
		}
		if (done && ios_frames_done % 30 == 0) {
			uint32_t r = rd(csid + CSID_RDI0_IRQ_STATUS);

			if (smmu_ife_faults() != 0) {
				give_up("the SMMU refused the IFE");
				break;
			}
			if (r & (1u << 2 | 1u << 15)) {
				say("camera: RDI 0 status %#x (overflow %u, CCIF violation %u) at frame %llu\n", r, r >> 2 & 1, r >> 15 & 1, ios_frames_done);
				wr(csid + CSID_RDI0_IRQ_CLEAR, 1u << 2 | 1u << 15);
				wr(csid + CSID_IRQ_CMD, 1);
			}
			if (ios_frames_done == 30 || ios_frames_done % 900 == 0) {
				say("camera: %llu frames for iOS, %llu given, %llu written over\n", ios_frames_done, ios_given, ios_writes_late);
			}
		}
	}
	ios_stop(broken ? "a fault" : !ios_open ? "closed" : ios_switch ? "the other camera asked for" : "nobody reads");
}

static int
camdev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	if (broken) {
		return EIO;
	}
	ios_open = true;
	return 0;
}

/* (the last close) */
static int
camdev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	ios_open = false;
	ios_wanted = false;
	wakeup(&ios_open);
	return 0;
}

static int
camdev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hk_camera_frame f;
	uint64_t since = ios_ms();
	int b = -1, error = 0;

	lck_mtx_lock(&ios_lock);
	ios_last_read = ios_ms();
	if (!ios_streaming && !ios_wanted) {
		ios_wanted = true;
		wakeup(&ios_open);
	}
	for (;;) {
		if (broken) {
			lck_mtx_unlock(&ios_lock);
			return EIO;
		}
		b = -1;
		for (int i = 0; i < RING; i++) {
			if (place[i].state == PLACE_READY && place[i].sequence > ios_given && (b < 0 || place[i].sequence > place[b].sequence)) {
				b = i;
			}
		}
		if (b >= 0) {
			break;
		}
		if (ios_ms() - since > (ios_streaming ? 3000u : 15000u)) {      /* (the sensor's tables take seconds) */
			lck_mtx_unlock(&ios_lock);
			return EAGAIN;
		}
		if (!ios_streaming && !ios_wanted) {
			ios_wanted = true;
			wakeup(&ios_open);
		}
		struct timespec ts = { 0, 100 * NSEC_PER_MSEC };
		error = msleep(&ios_frames_done, &ios_lock, PZERO | PCATCH, "hk-camera", &ts);
		if (error != 0 && error != EWOULDBLOCK) {
			lck_mtx_unlock(&ios_lock);
			return error;
		}
		ios_last_read = ios_ms();
	}
	place[b].state = PLACE_READING;         /* (the write master is never given it meanwhile: ios_session()) */
	ios_readers++;
	ios_given = place[b].sequence;
	bzero(&f, sizeof(f));
	memcpy(f.magic, HK_CAMERA_MAGIC, 8);
	f.header_bytes = sizeof(f);
	f.width = st.mode->width;
	f.height = st.mode->height;
	f.stride = st.stride;
	f.format = 1;
	f.bayer = frames_bayer();
	f.black = 64;
	f.sequence = place[b].sequence;
	f.time = place[b].time;
	f.exposure_lines = place[b].exposure_lines;
	f.line_ns = ios_line_ns;
	f.gain = place[b].gain;
	f.gain_unity = IOS_GAIN_UNITY;
	f.gain_max = IOS_GAIN_MAX;
	f.frame_lines = place[b].frame_lines;
	f.frame_lines_min = st.mode->frame_length;
	f.frame_lines_max = IOS_FRAME_LINES_MAX;
	f.margin = IOS_MARGIN;
	f.sensor = frames_number();
	f.mode = place[b].mode;                 /* (the wide one's 6: the middle of its full resolution, a 2x) */
	f.focus = place[b].focus;               /* (the IMX586's with cam_focus=2; the IMX471 has no lens motor) */
	lck_mtx_unlock(&ios_lock);

	error = uiomove((caddr_t)&f, (int)MIN(sizeof(f), (size_t)uio_resid(uio)), uio);
	if (error == 0 && uio_resid(uio) > 0) {
		vm_offset_t at = ife_memory + IFE_FRAME_AT + (uint64_t)b * IFE_SLOT_BYTES;
		uint32_t n = (uint32_t)MIN((uint64_t)ife_frame_bytes, (uint64_t)uio_resid(uio));

		/* (what the IFE wrote went past the processor's caches) */
		for (vm_offset_t line = at & ~63ul; line < at + n; line += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (line));
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		error = uiomove((caddr_t)at, (int)n, uio);
		if (f.sequence <= 3 || f.sequence % 300 == 0) {
			say("camera: frame %llu read: %u bytes\n", f.sequence, n);
		}
	}
	lck_mtx_lock(&ios_lock);
	if (place[b].state == PLACE_READING) {
		place[b].state = PLACE_READY;
	}
	ios_readers--;
	ios_last_read = ios_ms();
	lck_mtx_unlock(&ios_lock);
	wakeup(&ios_readers);
	return error;
}

/* HK_CAMERA_KEEP: a picture for the host, in the memory in one piece taken when the node was made (xl camera) */
static int
keep_picture(const struct hk_camera_command *c, struct uio *uio)
{
	struct cam_header *h;
	uint64_t want = round_page(4096 + (uint64_t)c->bytes);
	int error;

	if (c->bytes == 0 || c->d < 1 || c->d > 3 || (uint64_t)c->c * c->b > (uint64_t)c->bytes * 2) {
		return EINVAL;
	}
	if (ios_keep == 0 || ios_keep_bytes < want) {
		return ENOSPC;
	}
	h = (struct cam_header *)ios_keep;
	bzero(h, 4096);
	memcpy(h->magic, "RWCAM001", 8);
	h->width = c->a;
	h->height = c->b;
	h->stride = c->c;
	h->format = c->d;
	h->bayer = c->d == 1 ? frames_bayer() : 0;     /* (a raw picture: its camera's order) */
	h->frame_bytes = c->bytes;
	h->source = 2;
	strlcpy(h->sensor, "iOS", sizeof(h->sensor));
	error = uiomove((caddr_t)(ios_keep + 4096), (int)MIN((uint64_t)c->bytes, (uint64_t)uio_resid(uio)), uio);
	h->frames = error == 0 ? 1 : 0;
	for (vm_offset_t line = ios_keep; line < ios_keep + want; line += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	loader_recording[0] = kvtophys(ios_keep);
	loader_recording[1] = want;
	loader_recording[2] = (uint64_t)ios_keep;
	say("camera: a picture from iOS for the host: %ux%u, format %u, %u bytes\n", c->a, c->b, c->d, c->bytes);
	return error;
}

static int
camdev_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	struct hk_camera_command c;

	if (uio_resid(uio) < (user_ssize_t)sizeof(c) || uiomove((caddr_t)&c, sizeof(c), uio) != 0) {
		return EINVAL;
	}
	switch (c.command) {
	case HK_CAMERA_EXPOSURE:
		lck_mtx_lock(&ios_lock);
		ios_ask.lines = c.a;
		ios_ask.gain = c.b;
		ios_ask.frame_lines = c.c;
		ios_ask.pending = true;
		lck_mtx_unlock(&ios_lock);
		return 0;
	case HK_CAMERA_KEEP:
		return keep_picture(&c, uio);
	case HK_CAMERA_FOCUS:
		if (act_ready) {
			act_target = act_code_of(c.a);  /* (the front camera has no lens motor: nothing) */
		}
		return 0;
	case HK_CAMERA_SENSOR:
		if (c.a != 1 && c.a != 2 && c.a != 3) {
			return ENODEV;  /* (the wide, the front and the ultra-wide camera have a frame path on this phone, the macro not) */
		}
		if (c.b != ~0u && (c.a != 1 || (c.b != 1 && c.b != 6))) {
			return ENODEV;  /* (the wide one's modes 1 and 6, the front one's and the ultra-wide one's usual one) */
		}
		lck_mtx_lock(&ios_lock);
		ios_sensor_wanted = c.a;
		ios_mode_wanted = c.b != ~0u ? c.b : c.a == 3 ? 0 : 1;
		ios_mode_asked_ms = ios_ms();
		if (ios_quick_modes != 0 && ios_streaming && c.a == 1 && frames_sensor == 0) {
			ios_mode_quick = ios_mode_quick || ios_mode_wanted != frames_mode;     /* (the same camera: in the session) */
		} else {
			ios_switch = ios_streaming && (c.a != frames_number() || ios_mode_wanted != frames_mode);
		}
		lck_mtx_unlock(&ios_lock);
		return 0;
	default:
		return EINVAL;
	}
}

static const struct cdevsw camdev_cdevsw = {
	.d_open = camdev_open, .d_close = camdev_close, .d_read = camdev_read, .d_write = camdev_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * The node, made as the thread starts (before mediaserverd does: its capture stack looks for cameras once, when it starts),
 * and the memory in one piece for HK_CAMERA_KEEP while there is still memory in one piece; reads wait for serve_ios().
 */
static bool camdev_made;

static void
camdev_make(void)
{
	int major = cdevsw_add(-1, &camdev_cdevsw);

	camdev_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hk-camera") != NULL;
	ios_keep_bytes = (uint64_t)CAM_KEEP_MB << 20;
	if (kmem_alloc_contig(kernel_map, &ios_keep, ios_keep_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		ios_keep = 0;
		ios_keep_bytes = 0;
	}
}

/* cam=5: a session whenever a read wants frames, until a fault */
static void
serve_ios(void)
{
	if (!camdev_made) {
		say("camera: /dev/hk-camera cannot be made\n");
		return;
	}
	/*
	 * the first exposure, until the program asks for its own: boot-args cam_shutter / cam_gain (linear, 1024 1x), else the
	 * mode's longest at 8x (not more lines than the mode's frame: run 101751's 0xffff stretched the first frames to 0.8 s)
	 */
	ios_ask.lines = 2685 - IOS_MARGIN;      /* (the IMX471's mode 1: 2685 lines a frame; ios_limits() keeps it in any case) */
	ios_ask.gain = 8 * IOS_GAIN_UNITY;
	ios_ask.frame_lines = 0;
	(void)PE_parse_boot_argn("cam_shutter", &ios_ask.lines, sizeof(ios_ask.lines));
	(void)PE_parse_boot_argn("cam_gain", &ios_ask.gain, sizeof(ios_ask.gain));
	(void)PE_parse_boot_argn("cam_quickmode", &ios_quick_modes, sizeof(ios_quick_modes));
	(void)PE_parse_boot_argn("cam_focus", &focus_level, sizeof(focus_level));
	(void)PE_parse_boot_argn("cam_focus_ultra", &focus_ultra, sizeof(focus_ultra));
	(void)PE_parse_boot_argn("cam_keep", &ios_keep_secs, sizeof(ios_keep_secs));
	say("camera: /dev/hk-camera: the front (IMX471) camera's frames (mode 1: 2328x1748), the wide (IMX586) one's (mode 1: 4000x3000) "
	    "or the ultra-wide (IMX481) one's (mode 0: 4656x3496), "
	    "30 frames/s at the most, whenever it is read; %llu MB in one piece for pictures to the host\n", ios_keep_bytes >> 20);
	while (!broken) {
		lck_mtx_lock(&ios_lock);
		while (!broken && !(ios_open && ios_wanted)) {
			if (!ios_kept) {
				(void)msleep(&ios_open, &ios_lock, PZERO, "hk-camera-idle", NULL);
				continue;
			}
			/* (a kept sensor: looked at every half second; powered down here, on this thread, never under a session) */
			struct timespec ts = { 0, 500 * NSEC_PER_MSEC };

			(void)msleep(&ios_open, &ios_lock, PZERO, "hk-camera-kept", &ts);
			if (ios_kept && !(ios_open && ios_wanted) && (ios_ms() - ios_kept_since >= ios_keep_secs * 1000ull || ios_too_hot())) {
				lck_mtx_unlock(&ios_lock);
				ios_release_kept(ios_too_hot() ? "too hot" : "idle");
				lck_mtx_lock(&ios_lock);
			}
		}
		ios_wanted = false;
		ios_switch = false;
		ios_mode_quick = false;
		/* (the camera the program asked for last, in its mode: the ultra-wide one's 0, its whole field) */
		frames_sensor = ios_sensor_wanted == 1 ? 0 : ios_sensor_wanted == 3 ? 1 : 2;
		frames_mode = ios_sensor_wanted == 2 ? 1 : ios_mode_wanted;
		lck_mtx_unlock(&ios_lock);
		if (broken) {
			break;
		}
		ios_session();
	}
	ios_release_kept("a fault");
	say("camera: /dev/hk-camera: no more frames in this run\n");
}

/* ---- the look, and the thread -------------------------------------------------------------------------------------- */

static void
look(void)
{
	uint8_t revid[4] = { 0 }, type = 0, subtype = 0, sts = 0, regs[5];

	say("camera: GCC: camera AHB %#x, HF AXI %#x, SF AXI %#x, XO %#x\n", rd(gcc_reg(GCC_CAMERA_AHB_CBCR)),
	    rd(gcc_reg(GCC_CAMERA_HF_AXI_CBCR)), rd(gcc_reg(GCC_CAMERA_SF_AXI_CBCR)), rd(gcc_reg(GCC_CAMERA_XO_CBCR)));
	if (ahb_on()) {
		say("camera: camcc: titan top GDSC %#x (cfg %#x), its clock %#x\n", rd(camcc_reg(TITAN_TOP_GDSCR)),
		    rd(camcc_reg(TITAN_TOP_GDSC_CFG)), rd(camcc_reg(CAM_CC_GDSC_CBCR)));
	} else {
		say("camera: camcc not read: GCC's camera AHB clock is off (cam=2 switches it on first, as Linux does at boot)\n");
	}
	for (unsigned i = 0; i < SENSORS; i++) {
		const struct sensor_hw *s = &sensors[i];

		say("camera: the %s camera (cam-sensor@%u, %s, CSIPHY %u):\n", s->name, s->cell, s->bus->name, s->csiphy);
		if (ahb_on()) {
			say("camera:   MCLK RCG cmd %#x cfg %#x, branch %#x\n", rd(camcc_reg(s->mclk_cmd)), rd(camcc_reg(s->mclk_cmd + 4)),
			    rd(camcc_reg(s->mclk_cbcr)));
		}
		show_pin(s->bus->sda, "CCI data");
		show_pin(s->bus->scl, "CCI clock");
		show_pin(s->mclk_pin, "MCLK");
		for (unsigned k = 0; k < K_KINDS; k++) {
			if (s->line[k].kind == LINE_TLMM) {
				show_pin(s->line[k].pin, kind_names[k]);
			}
		}
	}
	if (spmi_read(PM8009_REVID, 0x00, revid, 4) == NULL) {
		say("camera: the PM8009: revision %02x %02x %02x %02x\n", revid[0], revid[1], revid[2], revid[3]);
	} else {
		say("camera: the PM8009's revision cannot be read\n");
	}
	if (spmi_read(PM8009_GPIO1, PERIPH_TYPE, &type, 1) == NULL && spmi_read(PM8009_GPIO1, PERIPH_SUBTYPE, &subtype, 1) == NULL &&
	    spmi_read(PM8009_GPIO1, GPIO_RT_STS, &sts, 1) == NULL && pgpio_read_all(regs)) {
		struct periph *p = periph(PM8009_GPIO1);

		say("camera: the PM8009's GPIO 1 (the ultra-wide's VANA switch): type %#04x subtype %#04x, at %u; vin %#04x pull %#04x "
		    "mode %#04x source %#04x enable %#04x; %s by us\n", type, subtype, sts & 1, regs[0], regs[1], regs[2], regs[3], regs[4],
		    p != NULL && p->writable ? "writable" : "NOT writable");
	} else {
		say("camera: the PM8009's GPIO 1 cannot be read\n");
	}
	say("camera: the rails as their PMICs have them:\n");
	for (unsigned r = R_L1; r < R_RAILS; r++) {
		show_rail(r);
	}
}

static void
camera_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t secs = 20;
	char which[16] = "";
	unsigned tried = 0, answered = 0;

	(void)PE_parse_boot_argn("cam_after", &secs, sizeof(secs));
	(void)PE_parse_boot_arg_str("cam_sensor", which, sizeof(which));
	if (level == 3 && strcmp(which, "wide") == 0) {
		frames_sensor = 0;      /* (cam=3 cam_sensor=wide: the wide IMX586's frames' path) */
		(void)PE_parse_boot_argn("cam_mode", &frames_mode, sizeof(frames_mode));
	} else if (level == 3 && strcmp(which, "ultra") == 0) {
		frames_sensor = 1;      /* (cam=3 cam_sensor=ultra: the ultra-wide IMX481's, its mode 0) */
		frames_mode = 0;
		(void)PE_parse_boot_argn("cam_focus_ultra", &focus_ultra, sizeof(focus_ultra));
	}
	(void)PE_parse_boot_argn("cam_scan", &scan, sizeof(scan));
	(void)PE_parse_boot_argn("cam_cci_burst", &cci_burst, sizeof(cci_burst));
	(void)PE_parse_boot_argn("cam_pll2", &pll2, sizeof(pll2));
	if (level == 5) {
		camdev_make();          /* (before mediaserverd looks for cameras) */
	}
	wait_ms(secs * 1000);           /* (the system up first) */
	gcc_camera = ml_io_map(GCC_PHYS + GCC_CAMERA_PAGE, 0x1000);
	camcc = ml_io_map(CAMCC_PHYS, CAMCC_SIZE);
	if (level >= 3) {
		refgen = ml_io_map(REFGEN_PHYS, 0x1000);
		csiphy2 = ml_io_map(CSIPHY2_PHYS, CSIPHY_SIZE);
		csiphy0 = ml_io_map(CSIPHY0_PHYS, CSIPHY_SIZE);
		csiphy3 = ml_io_map(CSIPHY3_PHYS, CSIPHY_SIZE);
		ife_lite0 = ml_io_map(IFE_LITE0_PHYS, IFE_LITE_SIZE);
	}
	arb_core = ml_io_map(ARB_CORE_PHYS, ARB_CORE_SIZE);
	arb_cnfg = ml_io_map(ARB_CNFG_PHYS, ARB_CNFG_SIZE);
	arb_observer = ml_io_map(ARB_OBSERVER_PHYS, ARB_OBSERVER_SIZE);
	if (rd(arb_core + ARB_VERSION) < ARB_VERSION_5 || rd(arb_core + ARB_VERSION) >= ARB_VERSION_7) {
		say("camera: the PMIC arbiter is of version %#x, not 5: nothing is done\n", rd(arb_core + ARB_VERSION));
		thread_terminate(current_thread());
	}
	say("camera: cam=%u on the OnePlus 8: %s\n", level, level == 5 ? "a look, then the front camera's frames for iOS (/dev/hk-camera)" :
	    level >= 3 ? "a look, then the frames' path, a step at a time" :
	    level >= 2 ? "a look, then each sensor powered up, its chip ID read, powered down" : "a look, reading only");
	look();
	if (level == 5) {
		serve_ios();
		say("camera: done\n");
		thread_terminate(current_thread());
	}
	if (level >= 3) {
		frames();
		say("camera: done\n");
		thread_terminate(current_thread());
	}
	for (unsigned i = 0; level >= 2 && i < SENSORS && !broken; i++) {
		size_t n = strlen(which);

		if (n != 0 && !(strncmp(which, sensors[i].name, n) == 0 && sensors[i].name[n] == ' ')) {
			continue;
		}
		hw = &sensors[i];
		say("camera: the %s camera\n", hw->name);
		if (!ctab_load()) {
			continue;
		}
		tried++;
		if (power_up()) {
			answered += read_chip_id();
		}
		power_down();
	}
	if (level >= 2) {
		say("camera: %u of %u sensors gave the chip ID Android expects%s\n", answered, tried,
		    broken ? "; stopped by what is above" : "");
	}
	say("camera: done\n");
	thread_terminate(current_thread());
}

void
instantnoodle_camera_init(void)
{
	thread_t thread;
	uint32_t step = 1;

	if (!PE_parse_boot_argn("cam", &level, sizeof(level)) || level == 0) {
		return;
	}
	(void)PE_parse_boot_argn("cam_step", &step, sizeof(step));
	if (level == 3 && step == 5) {
		ife_early_bytes = IFE_EARLY_BYTES;
		if (kmem_alloc_contig(kernel_map, &ife_early, ife_early_bytes, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			ife_early = 0;
			ife_early_bytes = 0;
		}
	}
	if (kernel_thread_start(camera_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* At the run's end and on a panic's way out: whatever was switched on, off (but the RPMh votes: rail_down()) */
void
instantnoodle_camera_quiesce(void)
{
	if (level < 2 || gcc_camera == 0) {
		return;
	}
	leaving = true;
	power_down();
}
