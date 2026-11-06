/*
 * Our own MDP (DPU 6.0, SM8250) layers on the OnePlus 8: the pipes, the mixers and later the scaler, so that the panel's
 * frame need not be scaled by the GPU, and iOS's layers can go through the hardware. The plan, the offsets and what is known
 * of each of them: docs/research/mdp-driver-plan.md. The firmware's DSI, DSC, ping-pong and INTF stay as they are: a DSC setup
 * lost is coloured static until the phone is reset. iokit/Kernel/MiniIOMobileFramebuffer.cpp keeps swaps, fences, TE and the
 * panel lock; this file knows pipes and stages.
 *
 * In steps, boot-arg mdp=N, default 0: nothing of this runs at all.
 *   mdp=1  read only: the pipeline the firmware left, decoded, and whether it is the one the later steps assume.
 *   mdp=2  a second layer that fetches no memory: DMA0 as a solid fill over the frame, then everything back. Judged by the
 *          mixers' own checksums (MISR), since xl screenshot reads memory and cannot see a layer.
 *   mdp=3  the same, then layers that fetch, from memory the MDP is already proven to reach (PROVEN_FROM..PROVEN_TO): the rows'
 *          unused ends, whose content this file sets, must give the checksums of solid fills of those colours; VIG2 and VIG3
 *          over the frame's own pixels must give the frame's checksums, across x = 540 and over the whole panel.
 *
 *   mdp=6  the display's own context bank: the MDP's stream sent through tables of ours instead of the firmware's context, which
 *          stays as it was and is never read. Faults can be seen. Frames as today.
 *   mdp=7  and the window server's frames straight from their surfaces: VIG0 and VIG1 scale them to the panel (QSEED), and nothing
 *          copies them or scales them on the GPU. mdp=8: and every layer of a swap in its own pipes, blended by the mixers.
 *
 * Writes: one function with read-back and a list of what may be written; any surprise ends all writing for the boot and puts
 * back what was found. They happen between pushes, under the panel's lock (hk_display_locked()). hk_mdp_restore(), at the top
 * of pushStop(), is the one way back on every other path: registers only, no lock, bounded, safe to run twice.
 *
 * mdp_reads: reads no boot has made before, one bit each, each announced in the log first (the loader keeps the tail, which is
 * all there is after a reset). Default 3.
 *   1  dispcc's MDP clock and its PLLs (the block is already read for its GDSC)
 *   2  the VIG pipes' scaler and CSC registers (+0xa00, +0x1a00)
 *   4  the MDP SID block (0x0ae8f000, inside the MDSS map): option E of the plan
 *   8  VBIF (0x0aeb0000: its own map): read only when underruns ask for it
 *
 * Offsets are MDSS-relative: Linux's dpu_6_0_sm8250.h + 0x1000. Registers only; no line in docs of what they held.
 */
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

#include "instantnoodle_mdp.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern void IOSleep(unsigned milliseconds);
extern void delay(int microseconds);
extern bool hk_display_locked(void (*work)(void *), void *context);     /* (MiniIOMobileFramebuffer.cpp) */
extern bool hk_display_push_locked(void);
extern uint64_t loader_screen[2];       /* (osfmk/arm64/loader_return.c) the frame the panel shows and its row bytes */

#define MDSS_SID_BLOCK          0x8f000
#define MDP_TOP                 0x1000
#define MDP_TOP_INTR_EN         0x1010
#define MDP_TOP_INTR_STATUS     0x1014
#define MDP_TOP_INTR_CLEAR      0x1018
#define MDP_INTR_INTF1_UNDERRUN (1u << 26)

#define CTL_BASE(n)             (0x2000u + (n) * 0x200u)
#define CTL_LAYER(lm)           ((lm) == 5 ? 0x24u : (lm) * 4u)
#define CTL_LAYER_EXT(lm)       (0x40u + (lm) * 4u)
#define CTL_LAYER_EXT2(lm)      (0x70u + (lm) * 4u)
#define CTL_LAYER_EXT3(lm)      (0xa0u + (lm) * 4u)
#define CTL_TOP                 0x14
#define CTL_FLUSH               0x18
#define CTL_MERGE_3D_ACTIVE     0xe4
#define CTL_DSC_ACTIVE          0xe8
#define CTL_INTF_ACTIVE         0xf4
#define CTL_FETCH_PIPE_ACTIVE   0xfc
#define CTL_MIXER_BORDER_OUT    (1u << 24)

#define LM_BASE(n)              (0x45000u + (n) * 0x1000u)
#define LM_OP_MODE              0x00
#define LM_OUT_SIZE             0x04
#define LM_BORDER_COLOR_0       0x08
#define LM_BORDER_COLOR_1       0x10
#define LM_BLEND(stage)         (0x20u + (stage) * 0x18u)
#define LM_MISR_CTRL            0x310
#define LM_MISR_SIGNATURE       0x314
#define MISR_FRAME_COUNT        0x1             /* (Linux dpu_hw_util.h) */
#define MISR_ENABLE             (1u << 8)
#define MISR_STATUS             (1u << 9)
#define MISR_STATUS_CLEAR       (1u << 10)
#define MISR_FREE_RUN           (1u << 31)

#define SSPP_SRC_SIZE           0x00
#define SSPP_SRC_IMG_SIZE       0x04
#define SSPP_SRC_XY             0x08
#define SSPP_OUT_SIZE           0x0c
#define SSPP_OUT_XY             0x10
#define SSPP_SRC0_ADDR          0x14
#define SSPP_SRC1_ADDR          0x18
#define SSPP_SRC_YSTRIDE0       0x24
#define SSPP_SRC_FORMAT         0x30
#define SSPP_SRC_UNPACK_PATTERN 0x34
#define SSPP_SRC_OP_MODE        0x38
#define SSPP_SRC_CONSTANT_COLOR 0x3c
#define SSPP_OP_PE_OVERRIDE     (1u << 31)
#define SSPP_FETCH_CONFIG       0x48
#define SSPP_DANGER_LUT         0x60
#define SSPP_SAFE_LUT           0x64
#define SSPP_CREQ_LUT           0x68
#define SSPP_QOS_CTRL           0x6c
#define SSPP_CREQ_LUT_0         0x74
#define SSPP_CREQ_LUT_1         0x78
#define SSPP_PIX_EXT_C0_LR      0x100
#define SSPP_PIX_EXT_C0_TB      0x104
#define SSPP_PIX_EXT_C0_REQ     0x108
#define SSPP_PIX_EXT_C1C2_REQ   0x118
#define SSPP_PIX_EXT_C3_REQ     0x128
#define SSPP_CDP_CNTL           0x134
#define SSPP_MULTIRECT_OPMODE   0x170

/* VIG only: scaler (qseedv3lite, version 0x30000 in Linux's catalog) */
#define VIG_SCALER              0xa00
#define VIG_CSC                 0x1a00
#define QSEED_OP_MODE           0x04            /* (Linux dpu_hw_util.c) */
#define QSEED_STEP_Y_H          0x10
#define QSEED_STEP_Y_V          0x14
#define QSEED_STEP_UV_H         0x18
#define QSEED_STEP_UV_V         0x1c
#define QSEED_PRELOAD           0x20
#define QSEED_SRC_Y             0x40
#define QSEED_SRC_UV            0x44
#define QSEED_DST               0x48
#define QSEED_INIT_Y_H          0x90
#define QSEED_INIT_Y_V          0x94
#define QSEED_INIT_UV_H         0x98
#define QSEED_INIT_UV_V         0x9c
#define PHASE_ONE               (1u << 21)
#define CSC_MATRIX              0x04            /* CSC_10BIT_OFFSET: all of it, to +0x44 */
#define CSC_OP_ON               3u              /* VIG_CSC_10_EN, VIG_CSC_10_SRC_DATAFMT */

/* dispcc, Linux's dispcc-sm8250.c */
#define DISPCC_PHYS             0x0af00000
#define DISPCC_SIZE             0x4000
#define DISPCC_PLL0             0x0000
#define DISPCC_PLL1             0x1000
#define PLL_MODE                0x00
#define PLL_L_VAL               0x04
#define PLL_ALPHA_VAL           0x40
#define DISPCC_MDP_CBCR         0x200c
#define DISPCC_MDP_RCG_CMD      0x20c8
#define DISPCC_MDP_RCG_CFG      0x20cc

#define VBIF_PHYS               0x0aeb0000
#define VBIF_SIZE               0x1000

enum { PIPE_VIG0, PIPE_VIG1, PIPE_VIG2, PIPE_VIG3, PIPE_DMA0, PIPE_DMA1, PIPE_DMA2, PIPE_DMA3, PIPE_COUNT };

static const struct {
	const char *name;
	uint32_t base;
	uint32_t flush;         /* its bit in CTL_FLUSH */
	uint8_t reg, shift;     /* its stage: 0 CTL_LAYER (3 bits, the fourth in EXT at ext), 2 CTL_LAYER_EXT2 (4 bits) */
	int8_t ext;
} mdp_pipes[PIPE_COUNT] = {
	{ "VIG0", 0x5000, 0, 0, 0, 0 },
	{ "VIG1", 0x7000, 1, 0, 3, 2 },
	{ "VIG2", 0x9000, 2, 0, 6, 4 },
	{ "VIG3", 0xb000, 18, 0, 26, 6 },
	{ "DMA0", 0x25000, 11, 0, 18, 16 },
	{ "DMA1", 0x27000, 12, 0, 21, 18 },
	{ "DMA2", 0x29000, 24, 2, 0, -1 },
	{ "DMA3", 0x2b000, 25, 2, 4, -1 },
};

static TUNABLE(uint32_t, mdp_step, "mdp", 0);
static TUNABLE(uint32_t, mdp_reads, "mdp_reads", 3);

static vm_offset_t mdp_dpu;
static vm_offset_t mdp_dispcc;

static void
mdp_say(const char *format, ...)
{
	char line[512];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	loader_log_line(line);
}

static uint32_t
mdp_read(unsigned offset)
{
	return *(volatile uint32_t *)(mdp_dpu + offset);
}

/*
 * A pipe's value in the CTL's layer registers on mixer `lm`, 0 when it is not staged there. Linux's numbering: the value is
 * dpu_stage + 1, DPU_STAGE_BASE 0, so 1 is the base layer with no blend registers, and N > 1 blends at LM_BLEND(N - 2).
 */
static unsigned
mdp_stage_of(unsigned pipe, unsigned lm, unsigned ctl)
{
	uint32_t c = CTL_BASE(ctl), value;

	if (mdp_pipes[pipe].reg == 2) {
		value = (mdp_read(c + CTL_LAYER_EXT2(lm)) >> mdp_pipes[pipe].shift) & 0xf;
	} else {
		value = (mdp_read(c + CTL_LAYER(lm)) >> mdp_pipes[pipe].shift) & 7;
		if (mdp_pipes[pipe].ext >= 0 && ((mdp_read(c + CTL_LAYER_EXT(lm)) >> mdp_pipes[pipe].ext) & 1)) {
			value += 8;
		}
	}
	return value;
}

static void
mdp_read_clocks(void)
{
	if (mdp_dispcc == 0) {
		mdp_dispcc = ml_io_map(DISPCC_PHYS, DISPCC_SIZE);
	}
	mdp_say("mdp: reading dispcc's MDP clock next (RCG +%#x, branch +%#x, PLLs)\n", DISPCC_MDP_RCG_CMD, DISPCC_MDP_CBCR);

	uint32_t cmd = *(volatile uint32_t *)(mdp_dispcc + DISPCC_MDP_RCG_CMD);
	uint32_t cfg = *(volatile uint32_t *)(mdp_dispcc + DISPCC_MDP_RCG_CFG);
	uint32_t cbcr = *(volatile uint32_t *)(mdp_dispcc + DISPCC_MDP_CBCR);
	uint32_t source = (cfg >> 8) & 7, half_div = cfg & 0x1f;
	uint32_t pll = source == 1 ? DISPCC_PLL0 : source == 4 ? DISPCC_PLL1 : 0;

	mdp_say("mdp: MDP RCG cmd %#x cfg %#x (source %u, half-divider %u) branch %#x\n", cmd, cfg, source, half_div, cbcr);
	if (pll == 0 && source != 0) {
		mdp_say("mdp: MDP clock from a source not in Linux's table\n");
		return;
	}
	if (source == 0) {
		mdp_say("mdp: MDP clock from the 19.2 MHz crystal\n");
		return;
	}
	uint32_t mode = *(volatile uint32_t *)(mdp_dispcc + pll + PLL_MODE);
	uint32_t l = *(volatile uint32_t *)(mdp_dispcc + pll + PLL_L_VAL) & 0xffff;
	uint32_t alpha = *(volatile uint32_t *)(mdp_dispcc + pll + PLL_ALPHA_VAL) & 0xffff;
	/* a Lucid PLL: 19.2 MHz x (L + alpha / 2^16); the RCG divides by (half_div + 1) / 2 */
	uint64_t pll_khz = (19200ull * ((uint64_t)l * 65536 + alpha)) >> 16;
	uint64_t mdp_khz = half_div != 0 ? pll_khz * 2 / (half_div + 1) : pll_khz;

	mdp_say("mdp: PLL%u mode %#x L %#x alpha %#x = %llu kHz; the MDP clock %llu kHz\n", pll == DISPCC_PLL1, mode, l, alpha, pll_khz, mdp_khz);
}

static void
mdp_read_scaler(void)
{
	mdp_say("mdp: reading the VIG pipes' op, scaler and CSC registers next (+%#x, +%#x)\n", VIG_SCALER, VIG_CSC);
	for (unsigned pipe = PIPE_VIG0; pipe <= PIPE_VIG3; pipe++) {
		uint32_t p = mdp_pipes[pipe].base, s = p + VIG_SCALER;

		/* (+0 is both Linux's SSPP_VIG_OP_MODE and QSEED3_HW_VERSION) */
		mdp_say("mdp: %s +0 %#x scaler op %#x src %#x dst %#x, CSC op %#x\n", mdp_pipes[pipe].name, mdp_read(s), mdp_read(s + 4), mdp_read(s + 0x40),
		    mdp_read(s + 0x48), mdp_read(p + VIG_CSC));
	}
}

static void
mdp_read_sid(void)
{
	mdp_say("mdp: reading the SID block next (MDSS +%#x)\n", MDSS_SID_BLOCK);
	for (unsigned i = 0; i < 0x40; i += 16) {
		mdp_say("mdp: SID +%#x: %#x %#x %#x %#x\n", i, mdp_read(MDSS_SID_BLOCK + i), mdp_read(MDSS_SID_BLOCK + i + 4),
		    mdp_read(MDSS_SID_BLOCK + i + 8), mdp_read(MDSS_SID_BLOCK + i + 12));
	}
}

static void
mdp_read_vbif(void)
{
	mdp_say("mdp: reading VBIF next (%#x)\n", VBIF_PHYS);

	vm_offset_t vbif = ml_io_map(VBIF_PHYS, VBIF_SIZE);

#define VBIF_READ(offset) (*(volatile uint32_t *)(vbif + (offset)))
	mdp_say("mdp: VBIF clk-force %#x write-gather %#x rd-limits %#x %#x %#x halt %#x %#x\n", VBIF_READ(0x4), VBIF_READ(0xac), VBIF_READ(0xc0),
	    VBIF_READ(0xc4), VBIF_READ(0xc8), VBIF_READ(0x200), VBIF_READ(0x204));
	mdp_say("mdp: VBIF qos-remap rp %#x %#x lvl %#x %#x\n", VBIF_READ(0x550), VBIF_READ(0x554), VBIF_READ(0x590), VBIF_READ(0x594));
#undef VBIF_READ
}

/*
 * What the firmware left, and what it is now: every line says it, and the last line says whether the later steps may run.
 * Called from the framebuffer's start, while autorefresh is still fetching, and again after some pushes. `when`: said in
 * each line's first.
 */
static bool mdp_pipeline_as_expected;

static void
mdp_read_state(const char *when)
{
	mdp_say("mdp: state %s: HW %#x, INTR en %#x status %#x (INTF1 underrun latched: %s), clock force %#x %#x %#x %#x, split %#x\n", when, mdp_read(0),
	    mdp_read(MDP_TOP_INTR_EN), mdp_read(MDP_TOP_INTR_STATUS), (mdp_read(MDP_TOP_INTR_STATUS) & MDP_INTR_INTF1_UNDERRUN) ? "yes" : "no",
	    mdp_read(MDP_TOP + 0x2ac), mdp_read(MDP_TOP + 0x2b4), mdp_read(MDP_TOP + 0x2bc), mdp_read(MDP_TOP + 0x2c4), mdp_read(MDP_TOP + 0x2f4));

	for (unsigned ctl = 0; ctl < 6; ctl++) {
		uint32_t c = CTL_BASE(ctl), fetch = mdp_read(c + CTL_FETCH_PIPE_ACTIVE), intf = mdp_read(c + CTL_INTF_ACTIVE);

		if (ctl != 0 && fetch == 0 && intf == 0 && mdp_read(c + CTL_LAYER(0)) == 0 && mdp_read(c + CTL_LAYER(1)) == 0) {
			continue;
		}
		mdp_say("mdp: CTL%u top %#x flush %#x intf %#x dsc %#x merge3d %#x fetch %#x\n", ctl, mdp_read(c + CTL_TOP), mdp_read(c + CTL_FLUSH), intf,
		    mdp_read(c + CTL_DSC_ACTIVE), mdp_read(c + CTL_MERGE_3D_ACTIVE), fetch);
		for (unsigned lm = 0; lm < 6; lm++) {
			uint32_t layer = mdp_read(c + CTL_LAYER(lm)), ext = mdp_read(c + CTL_LAYER_EXT(lm));
			uint32_t ext2 = mdp_read(c + CTL_LAYER_EXT2(lm)), ext3 = mdp_read(c + CTL_LAYER_EXT3(lm));

			if (layer == 0 && ext == 0 && ext2 == 0 && ext3 == 0) {
				continue;
			}
			mdp_say("mdp: CTL%u LM%u layer %#x ext %#x %#x %#x%s\n", ctl, lm, layer, ext, ext2, ext3, (layer & CTL_MIXER_BORDER_OUT) ? " border-out" : "");
			for (unsigned pipe = 0; pipe < PIPE_COUNT; pipe++) {
				unsigned stage = mdp_stage_of(pipe, lm, ctl);

				if (stage != 0) {
					mdp_say("mdp: CTL%u LM%u: %s at value %u\n", ctl, lm, mdp_pipes[pipe].name, stage);
				}
			}
		}
	}
	for (unsigned pipe = 0; pipe < PIPE_COUNT; pipe++) {
		uint32_t p = mdp_pipes[pipe].base;

		mdp_say("mdp: %s src %#x img %#x xy %#x out %#x oxy %#x addr %#x stride %#x fmt %#x unpack %#x op %#x fetch %#x multirect %#x\n",
		    mdp_pipes[pipe].name, mdp_read(p + SSPP_SRC_SIZE), mdp_read(p + SSPP_SRC_IMG_SIZE), mdp_read(p + SSPP_SRC_XY), mdp_read(p + SSPP_OUT_SIZE),
		    mdp_read(p + SSPP_OUT_XY), mdp_read(p + SSPP_SRC0_ADDR), mdp_read(p + SSPP_SRC_YSTRIDE0), mdp_read(p + SSPP_SRC_FORMAT),
		    mdp_read(p + SSPP_SRC_UNPACK_PATTERN), mdp_read(p + SSPP_SRC_OP_MODE), mdp_read(p + SSPP_FETCH_CONFIG), mdp_read(p + SSPP_MULTIRECT_OPMODE));
		mdp_say("mdp: %s qos danger %#x safe %#x creq %#x ctrl %#x creq0 %#x creq1 %#x cdp %#x\n", mdp_pipes[pipe].name, mdp_read(p + SSPP_DANGER_LUT),
		    mdp_read(p + SSPP_SAFE_LUT), mdp_read(p + SSPP_CREQ_LUT), mdp_read(p + SSPP_QOS_CTRL), mdp_read(p + SSPP_CREQ_LUT_0), mdp_read(p + SSPP_CREQ_LUT_1),
		    mdp_read(p + SSPP_CDP_CNTL));
		mdp_say("mdp: %s color %#x pixel-ext c0 lr %#x tb %#x req %#x c1c2 req %#x c3 req %#x\n", mdp_pipes[pipe].name, mdp_read(p + SSPP_SRC_CONSTANT_COLOR),
		    mdp_read(p + SSPP_PIX_EXT_C0_LR), mdp_read(p + SSPP_PIX_EXT_C0_TB), mdp_read(p + SSPP_PIX_EXT_C0_REQ), mdp_read(p + SSPP_PIX_EXT_C1C2_REQ),
		    mdp_read(p + SSPP_PIX_EXT_C3_REQ));
	}
	for (unsigned lm = 0; lm < 4; lm++) {
		uint32_t l = LM_BASE(lm);

		mdp_say("mdp: LM%u op %#x out %#x border %#x %#x misr %#x %#x blend0 %#x %#x blend1 %#x %#x blend2 %#x %#x\n", lm, mdp_read(l + LM_OP_MODE),
		    mdp_read(l + LM_OUT_SIZE), mdp_read(l + LM_BORDER_COLOR_0), mdp_read(l + LM_BORDER_COLOR_1), mdp_read(l + LM_MISR_CTRL),
		    mdp_read(l + LM_MISR_SIGNATURE), mdp_read(l + LM_BLEND(0)), mdp_read(l + LM_BLEND(0) + 4), mdp_read(l + LM_BLEND(1)),
		    mdp_read(l + LM_BLEND(1) + 4), mdp_read(l + LM_BLEND(2)), mdp_read(l + LM_BLEND(2) + 4));
		mdp_say("mdp: LM%u blend3 %#x %#x blend4 %#x %#x blend5 %#x %#x blend6 %#x %#x\n", lm, mdp_read(l + LM_BLEND(3)), mdp_read(l + LM_BLEND(3) + 4),
		    mdp_read(l + LM_BLEND(4)), mdp_read(l + LM_BLEND(4) + 4), mdp_read(l + LM_BLEND(5)), mdp_read(l + LM_BLEND(5) + 4),
		    mdp_read(l + LM_BLEND(6)), mdp_read(l + LM_BLEND(6) + 4));
	}
}

/*
 * The question step (a) answered, on which every later coordinate depends (run 182621): one pipe per mixer, and OUT_XY in the
 * pair's coordinates: VIG1's x is 540 and LM1 has the right-mixer bit. Anything else: the later steps refuse.
 */
static void
mdp_judge(void)
{
	uint32_t c = CTL_BASE(0);
	unsigned vig0_lm0 = mdp_stage_of(PIPE_VIG0, 0, 0), vig0_lm1 = mdp_stage_of(PIPE_VIG0, 1, 0);
	unsigned vig1_lm0 = mdp_stage_of(PIPE_VIG1, 0, 0), vig1_lm1 = mdp_stage_of(PIPE_VIG1, 1, 0);
	uint32_t oxy0 = mdp_read(mdp_pipes[PIPE_VIG0].base + SSPP_OUT_XY), oxy1 = mdp_read(mdp_pipes[PIPE_VIG1].base + SSPP_OUT_XY);
	uint32_t right = mdp_read(LM_BASE(1) + LM_OP_MODE) >> 31;
	bool others = false;

	for (unsigned pipe = PIPE_VIG2; pipe < PIPE_COUNT; pipe++) {
		others = others || mdp_stage_of(pipe, 0, 0) != 0 || mdp_stage_of(pipe, 1, 0) != 0;
	}
	mdp_pipeline_as_expected = vig0_lm0 == 5 && vig1_lm1 == 5 && vig0_lm1 == 0 && vig1_lm0 == 0 && oxy0 == 0 && oxy1 == 540 && right == 1 && !others &&
	    mdp_read(c + CTL_INTF_ACTIVE) == 0x2 && mdp_read(c + CTL_DSC_ACTIVE) == 0x3;
	mdp_say("mdp: VIG0 on LM0 %u LM1 %u, VIG1 on LM0 %u LM1 %u, out-xy %#x %#x, right mixer %u, other pipes staged: %s (CTL0 intf %#x dsc %#x): the later steps %s\n",
	    vig0_lm0, vig0_lm1, vig1_lm0, vig1_lm1, oxy0, oxy1, right, others ? "yes" : "no", mdp_read(c + CTL_INTF_ACTIVE), mdp_read(c + CTL_DSC_ACTIVE),
	    mdp_pipeline_as_expected ? "may run" : "will refuse");
}

/*
 * The one way registers are written. Only what is in the list; each is read back, and a pipe's registers do read back at once
 * (run 184016), so a difference ends all writing.
 */
static bool mdp_stopped;                /* a surprise: nothing more is written this boot */
static bool mdp_touched;                /* something has been written that hk_mdp_restore() must put back */
static uint32_t mdp_pending;            /* flush bits for the next push (hk_mdp_flush_bits()) */
static uint32_t mdp_underruns;          /* INTF1's underrun bit seen set, and cleared */
static bool mdp_on_pipes_now;           /* the frame is in the pipes as hk_mdp_commit() left them */

/* The test's pipes: index & 1 is the mixer. Their CTL value: one above the frame's 5. */
#define TEST_PIPES              6
static const uint8_t mdp_test_pipes[TEST_PIPES] = { PIPE_DMA0, PIPE_DMA1, PIPE_VIG2, PIPE_VIG3, PIPE_VIG0, PIPE_VIG1 };
#define TEST_PIPE(t)            mdp_test_pipes[t]
#define TEST_DMA0               0
#define TEST_DMA1               1
#define TEST_VIG2               2
#define TEST_VIG3               3
#define TEST_VIG0               4
#define TEST_VIG1               5
#define TEST_VALUE              6u
#define TEST_BLEND_OP           0x400u  /* foreground constant, background 1 - that: as the firmware's other stages */
#define TEST_BLEND_ALPHA        0xff0000u
#define FLUSH_LM(t)             (1u << (6 + (t)))
#define FLUSH_CTL               (1u << 17)

enum { R_SRC_SIZE, R_SRC_XY, R_OUT_SIZE, R_OUT_XY, R_ADDR, R_ADDR1, R_STRIDE, R_FORMAT, R_UNPACK, R_OP, R_COLOR, R_QOS, R_REQ_C0, R_REQ_C1C2, R_REQ_C3, R_COUNT };
static const uint16_t mdp_regs[R_COUNT] = {
	[R_SRC_SIZE] = SSPP_SRC_SIZE, [R_SRC_XY] = SSPP_SRC_XY, [R_OUT_SIZE] = SSPP_OUT_SIZE, [R_OUT_XY] = SSPP_OUT_XY, [R_ADDR] = SSPP_SRC0_ADDR, [R_ADDR1] = SSPP_SRC1_ADDR,
	[R_STRIDE] = SSPP_SRC_YSTRIDE0, [R_FORMAT] = SSPP_SRC_FORMAT, [R_UNPACK] = SSPP_SRC_UNPACK_PATTERN, [R_OP] = SSPP_SRC_OP_MODE,
	[R_COLOR] = SSPP_SRC_CONSTANT_COLOR, [R_QOS] = SSPP_QOS_CTRL, [R_REQ_C0] = SSPP_PIX_EXT_C0_REQ, [R_REQ_C1C2] = SSPP_PIX_EXT_C1C2_REQ,
	[R_REQ_C3] = SSPP_PIX_EXT_C3_REQ,
};

static uint32_t mdp_pipe_found[TEST_PIPES][R_COUNT], mdp_layer_found[2], mdp_layer_now[2];

/* Blend stages a frame's layers may take: CTL value 4 and up, and the firmware's own opaque setting found in each */
#define BLEND_FIRST             2u
#define BLEND_STAGES            4u
static uint32_t mdp_blend_found[2][BLEND_STAGES][2];

/* The VIG pipes' scaler and CSC */
static const uint16_t mdp_vig_regs[] = {
	VIG_SCALER + QSEED_OP_MODE, VIG_SCALER + QSEED_STEP_Y_H, VIG_SCALER + QSEED_STEP_Y_V, VIG_SCALER + QSEED_STEP_UV_H, VIG_SCALER + QSEED_STEP_UV_V,
	VIG_SCALER + QSEED_PRELOAD, VIG_SCALER + QSEED_SRC_Y, VIG_SCALER + QSEED_SRC_UV, VIG_SCALER + QSEED_DST, VIG_SCALER + QSEED_INIT_Y_H,
	VIG_SCALER + QSEED_INIT_Y_V, VIG_SCALER + QSEED_INIT_UV_H, VIG_SCALER + QSEED_INIT_UV_V, VIG_CSC,
};
#define VIG_REGS                (sizeof(mdp_vig_regs) / sizeof(mdp_vig_regs[0]))
#define CSC_REGS                17u             /* the matrix, clamps and biases: CSC_MATRIX + 4 * n */
static uint32_t mdp_vig_found[PIPE_VIG3 + 1][VIG_REGS + CSC_REGS];

/* What hk_mdp_restore() puts back: taken before anything is written, or it would put back zeros and unstage the frame. */
static void
mdp_save(void)
{
	for (unsigned lm = 0; lm < 2; lm++) {
		mdp_layer_found[lm] = mdp_layer_now[lm] = mdp_read(CTL_BASE(0) + CTL_LAYER(lm));
		for (unsigned b = 0; b < BLEND_STAGES; b++) {
			mdp_blend_found[lm][b][0] = mdp_read(LM_BASE(lm) + LM_BLEND(BLEND_FIRST + b));
			mdp_blend_found[lm][b][1] = mdp_read(LM_BASE(lm) + LM_BLEND(BLEND_FIRST + b) + 4);
		}
	}
	for (unsigned t = 0; t < TEST_PIPES; t++) {
		for (unsigned r = 0; r < R_COUNT; r++) {
			mdp_pipe_found[t][r] = mdp_read(mdp_pipes[TEST_PIPE(t)].base + mdp_regs[r]);
		}
	}
	for (unsigned pipe = PIPE_VIG0; pipe <= PIPE_VIG3; pipe++) {
		for (unsigned r = 0; r < VIG_REGS; r++) {
			mdp_vig_found[pipe][r] = mdp_read(mdp_pipes[pipe].base + mdp_vig_regs[r]);
		}
		for (unsigned r = 0; r < CSC_REGS; r++) {
			mdp_vig_found[pipe][VIG_REGS + r] = mdp_read(mdp_pipes[pipe].base + VIG_CSC + CSC_MATRIX + 4 * r);
		}
	}
}

static bool
mdp_may_write(unsigned offset)
{
	for (unsigned lm = 0; lm < 2; lm++) {
		if (offset == CTL_BASE(0) + CTL_LAYER(lm)) {
			return true;
		}
		for (unsigned b = 0; b < BLEND_STAGES; b++) {
			if (offset == LM_BASE(lm) + LM_BLEND(BLEND_FIRST + b) || offset == LM_BASE(lm) + LM_BLEND(BLEND_FIRST + b) + 4) {
				return true;
			}
		}
	}
	for (unsigned t = 0; t < TEST_PIPES; t++) {
		uint32_t pipe = mdp_pipes[TEST_PIPE(t)].base;

		if (offset >= pipe && offset <= pipe + SSPP_PIX_EXT_C3_REQ) {
			return true;
		}
		if (TEST_PIPE(t) <= PIPE_VIG3 && ((offset >= pipe + VIG_SCALER && offset <= pipe + VIG_SCALER + QSEED_INIT_UV_V) ||
		    (offset >= pipe + VIG_CSC && offset < pipe + VIG_CSC + CSC_MATRIX + 4 * CSC_REGS))) {
			return true;
		}
	}
	return false;
}

static void
mdp_put(unsigned offset, uint32_t value)
{
	*(volatile uint32_t *)(mdp_dpu + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
mdp_write(unsigned offset, uint32_t value)
{
	if (mdp_stopped) {
		return;
	}
	if (!mdp_may_write(offset)) {
		mdp_say("mdp: +%#x is not on the list; no more writes\n", offset);
		mdp_stopped = true;
		return;
	}
	mdp_touched = true;
	mdp_put(offset, value);

	uint32_t back = mdp_read(offset);

	if (back != value) {
		/* (the scaler and the CSC have not been written before: what reads differently is said, not yet a surprise) */
		static unsigned said;
		bool soft = offset < LM_BASE(0) && (offset & 0x1fff) >= VIG_SCALER;

		if (said < 16 || !soft) {
			said++;
			mdp_say("mdp: +%#x written %#x, reads %#x%s\n", offset, value, back, soft ? "" : "; no more writes");
		}
		mdp_stopped = mdp_stopped || !soft;
	}
}

static void
mdp_pipe_write(unsigned t, const uint32_t values[R_COUNT])
{
	for (unsigned r = 0; r < R_COUNT; r++) {
		mdp_write(mdp_pipes[TEST_PIPE(t)].base + mdp_regs[r], values[r]);
	}
	mdp_pending |= 1u << mdp_pipes[TEST_PIPE(t)].flush;
}

/* A solid fill, as Linux's _dpu_plane_color_fill() does it. abgr: alpha, blue, green, red, high to low. */
static void
mdp_pipe_fill(unsigned t, uint32_t abgr, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
	uint32_t size = h << 16 | w;
	const uint32_t values[R_COUNT] = {
		[R_SRC_SIZE] = size, [R_SRC_XY] = 0, [R_OUT_SIZE] = size, [R_OUT_XY] = y << 16 | x, [R_ADDR] = 0, [R_STRIDE] = 0,
		[R_FORMAT] = 0x4237ff,          /* the frame's format with bit 22, solid fill */
		[R_UNPACK] = 0x03010002, [R_OP] = SSPP_OP_PE_OVERRIDE, [R_COLOR] = abgr,
		[R_QOS] = 0,                    /* (Linux: danger and safe off for a color fill) */
		[R_REQ_C0] = size, [R_REQ_C1C2] = size, [R_REQ_C3] = size,
	};

	mdp_pipe_write(t, values);
}

/*
 * A fetch: a rectangle of a framebuffer in the frame's own format. What has not been shown to be mapped in the display's context
 * bank is never fetched: a fault there cannot be seen or cleared (docs/research/mdp-driver-plan.md, 1.5). Everything goes through
 * this one check.
 */
#define PROVEN_FROM             0x9c000000ull
#define PROVEN_TO               0x9d3f6000ull

static void
mdp_pipe_fetch(unsigned t, uint64_t phys, uint32_t stride, uint32_t sx, uint32_t sy, uint32_t w, uint32_t h, uint32_t x, uint32_t y)
{
	uint32_t size = h << 16 | w;
	uint64_t first = phys + (uint64_t)sy * stride + (uint64_t)sx * 4;
	uint64_t last = phys + (uint64_t)(sy + h - 1) * stride + (uint64_t)(sx + w) * 4;
	const uint32_t values[R_COUNT] = {
		[R_SRC_SIZE] = size, [R_SRC_XY] = sy << 16 | sx, [R_OUT_SIZE] = size, [R_OUT_XY] = y << 16 | x, [R_ADDR] = (uint32_t)phys, [R_STRIDE] = stride,
		[R_FORMAT] = 0x237ff, [R_UNPACK] = 0x03020001, [R_OP] = SSPP_OP_PE_OVERRIDE, [R_COLOR] = 0, [R_QOS] = mdp_pipe_found[t][R_QOS],
		[R_REQ_C0] = size, [R_REQ_C1C2] = size, [R_REQ_C3] = size,
	};

	if (first < PROVEN_FROM || last > PROVEN_TO || (uint32_t)phys != phys) {
		mdp_say("mdp: a fetch of %#llx..%#llx is outside what is proven; no more writes\n", first, last);
		mdp_stopped = true;
		return;
	}
	mdp_pipe_write(t, values);
}

static void
mdp_stage(unsigned t, bool on)
{
	unsigned lm = t & 1, shift = mdp_pipes[TEST_PIPE(t)].shift;

	if (on) {
		mdp_write(LM_BASE(lm) + LM_BLEND(4), TEST_BLEND_OP);
		mdp_write(LM_BASE(lm) + LM_BLEND(4) + 4, TEST_BLEND_ALPHA);
	}
	mdp_layer_now[lm] = (mdp_layer_now[lm] & ~(7u << shift)) | (on ? TEST_VALUE << shift : 0);
	mdp_write(CTL_BASE(0) + CTL_LAYER(lm), mdp_layer_now[lm]);
	mdp_pending |= 1u << mdp_pipes[TEST_PIPE(t)].flush | FLUSH_LM(lm);
}

/* (MiniIOMobileFramebuffer.cpp, pushFrameLocked(): what this file has changed goes with the frame's own flush) */
uint32_t
hk_mdp_flush_bits(void)
{
	return __atomic_exchange_n(&mdp_pending, 0, __ATOMIC_RELAXED);
}

/*
 * The found state again, from any context (pushStop(): the three failed pushes, hk_display_quiesce() on the way to the loader or
 * a panic). Registers only. Latched by the flush here or by the first autorefresh frame.
 */
void
hk_mdp_restore(void)
{
	uint32_t flush = FLUSH_CTL;

	mdp_on_pipes_now = false;
	if (mdp_touched) {
		mdp_stopped = true;
		mdp_touched = false;
		for (unsigned lm = 0; lm < 2; lm++) {
			mdp_put(CTL_BASE(0) + CTL_LAYER(lm), mdp_layer_found[lm]);
			for (unsigned b = 0; b < BLEND_STAGES; b++) {
				mdp_put(LM_BASE(lm) + LM_BLEND(BLEND_FIRST + b), mdp_blend_found[lm][b][0]);
				mdp_put(LM_BASE(lm) + LM_BLEND(BLEND_FIRST + b) + 4, mdp_blend_found[lm][b][1]);
			}
			mdp_put(LM_BASE(lm) + LM_MISR_CTRL, 0);
			flush |= FLUSH_LM(lm);
		}
		for (unsigned t = 0; t < TEST_PIPES; t++) {
			for (unsigned r = 0; r < R_COUNT; r++) {
				mdp_put(mdp_pipes[TEST_PIPE(t)].base + mdp_regs[r], mdp_pipe_found[t][r]);
			}
			flush |= 1u << mdp_pipes[TEST_PIPE(t)].flush;
		}
		for (unsigned pipe = PIPE_VIG0; pipe <= PIPE_VIG3; pipe++) {
			for (unsigned r = 0; r < VIG_REGS; r++) {
				mdp_put(mdp_pipes[pipe].base + mdp_vig_regs[r], mdp_vig_found[pipe][r]);
			}
			for (unsigned r = 0; r < CSC_REGS; r++) {
				mdp_put(mdp_pipes[pipe].base + VIG_CSC + CSC_MATRIX + 4 * r, mdp_vig_found[pipe][VIG_REGS + r]);
			}
		}
		mdp_put(CTL_BASE(0) + CTL_FLUSH, flush);
		mdp_say("mdp: the found state put back\n");
	}
	hk_mdp_smmu_down();
}

/* The latched underrun bit, counted and cleared. (The presenter's interrupt clears only what is enabled: the done bit.) */
static uint32_t
mdp_count_underruns(void)
{
	if (mdp_read(MDP_TOP_INTR_STATUS) & MDP_INTR_INTF1_UNDERRUN) {
		mdp_put(MDP_TOP_INTR_CLEAR, MDP_INTR_INTF1_UNDERRUN);
		__atomic_add_fetch(&mdp_underruns, 1, __ATOMIC_RELAXED);
	}
	return __atomic_load_n(&mdp_underruns, __ATOMIC_RELAXED);
}

/*
 * The mixers' checksums: armed before a push, read after it. xl screenshot reads memory, so these are what says a layer is in the
 * picture. Linux: dpu_hw_setup_misr(), dpu_hw_collect_misr().
 */
static uint32_t mdp_pushes;             /* the window server's, so far */
static uint32_t mdp_failed;             /* checks that did not hold */

struct mdp_sums {
	uint32_t lm[2];
	bool done;
};

/* One push of the frame the panel shows, with what has been written so far. In hk_display_locked()'s work. */
static bool
mdp_push_locked(const char *what, struct mdp_sums *sums)
{
	uint32_t ctrl[2];
	bool done;

	/* (not mdp_write(): the control register says its own status, so it does not read back) */
	for (unsigned lm = 0; lm < 2 && !mdp_stopped; lm++) {
		mdp_touched = true;
		mdp_put(LM_BASE(lm) + LM_MISR_CTRL, MISR_STATUS_CLEAR);
		mdp_put(LM_BASE(lm) + LM_MISR_CTRL, MISR_FRAME_COUNT | MISR_ENABLE | MISR_FREE_RUN);
	}
	done = hk_display_push_locked();
	for (unsigned lm = 0; lm < 2; lm++) {
		ctrl[lm] = mdp_read(LM_BASE(lm) + LM_MISR_CTRL);
		sums->lm[lm] = mdp_read(LM_BASE(lm) + LM_MISR_SIGNATURE);
	}
	sums->done = done && (ctrl[0] & MISR_STATUS) && (ctrl[1] & MISR_STATUS);
	mdp_say("mdp: %s: push %s, LM0 %#x (%#x) LM1 %#x (%#x), underruns %u, window server's pushes %u\n", what, done ? "done" : "NOT done", sums->lm[0], ctrl[0],
	    sums->lm[1], ctrl[1], mdp_count_underruns(), __atomic_load_n(&mdp_pushes, __ATOMIC_RELAXED));
	return done && !mdp_stopped;
}

static bool
mdp_same(const struct mdp_sums *a, const struct mdp_sums *b, unsigned lm)
{
	return a->done && b->done && a->lm[lm] == b->lm[lm];
}

static bool
mdp_both(const struct mdp_sums *a, const struct mdp_sums *b)
{
	return mdp_same(a, b, 0) && mdp_same(a, b, 1);
}

/* `a` and `b` differ on mixer lm and are the same on the other */
static bool
mdp_only(const struct mdp_sums *a, const struct mdp_sums *b, unsigned lm)
{
	return a->done && b->done && a->lm[lm] != b->lm[lm] && a->lm[!lm] == b->lm[!lm];
}

static void
mdp_check(const char *what, bool good)
{
	mdp_say("mdp: check %s: %s\n", what, good ? "yes" : "NO");
	mdp_failed += !good;
}

/* A question, not a requirement */
static void
mdp_note(const char *what, bool answer)
{
	mdp_say("mdp: note %s: %s\n", what, answer ? "yes" : "no");
}

/*
 * Memory to fetch whose content can be changed with nothing on the screen moving: the frame is 1080 pixels of rows of 4352 bytes, so
 * every row of both framebuffers ends in 8 pixels nobody sees, inside what the MDP already reads. Written through a device
 * mapping, so what is written is in memory after the barrier. Only the front one's, under the lock, and the GPU is not drawing there.
 */
#define FRAME_STRIDE            4352u
#define STRIP_X                 1080u
#define STRIP_Y                 1000u
#define STRIP_W                 8u
#define STRIP_H                 128u
#define STRIP_OUT_X             142u
#define STRIP_OUT_Y             1000u
#define FRAME_A                 0x9c000000ull
#define FRAME_B                 0x9ca00000ull
#define PAGE_16K                0x4000ull

static vm_offset_t mdp_strips[2];

static void
mdp_map_strips(void)
{
	for (unsigned i = 0; i < 2; i++) {
		uint64_t row = (i == 0 ? FRAME_A : FRAME_B) + (uint64_t)STRIP_Y * FRAME_STRIDE;
		uint64_t from = row & ~(PAGE_16K - 1);

		mdp_strips[i] = ml_io_map((vm_offset_t)from, (vm_size_t)(row - from + (uint64_t)STRIP_H * FRAME_STRIDE)) + (vm_offset_t)(row - from);
	}
}

static inline volatile uint32_t *
mdp_strip_pixel(vm_offset_t strip, uint32_t x, uint32_t y)
{
	return (volatile uint32_t *)(strip + (size_t)y * FRAME_STRIDE + (size_t)(STRIP_X + x) * 4);
}

static void
mdp_strip_color(vm_offset_t strip, uint32_t argb)
{
	for (uint32_t y = 0; y < STRIP_H; y++) {
		for (uint32_t x = 0; x < STRIP_W; x++) {
			*mdp_strip_pixel(strip, x, y) = argb;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
mdp_strip_pattern(vm_offset_t strip)
{
	for (uint32_t y = 0; y < STRIP_H; y++) {
		for (uint32_t x = 0; x < STRIP_W; x++) {
			*mdp_strip_pixel(strip, x, y) = 0xff000000u | (y * 2) << 16 | (x * 30) << 8 | (x ^ y);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* Two colours, in memory's order and in the solid fill's: the same pixels if the channels mean what Linux says they do. */
#define COLOR_X_ARGB            0xff20c060u
#define COLOR_X_ABGR            0xff60c020u
#define COLOR_Y_ARGB            0xffc03090u
#define COLOR_Y_ABGR            0xff9030c0u

struct mdp_proof {
	bool finished;
};

/*
 * The proof, all under one hold of the lock, so that every push sends the same frame: LM0 alone may change, and only while
 * something is staged on it. With mdp=3, what a fetched layer shows is compared with what it must show, with the memory it fetches
 * changed between pushes, so a stale or empty fetch cannot pass.
 */
static void
mdp_proof_work(void *context)
{
	struct mdp_proof *proof = context;
	struct mdp_sums base, sums, x, y, fetched, pattern;
	uint64_t front = loader_screen[0];
	uint32_t stride = (uint32_t)loader_screen[1];
	vm_offset_t strip;

	if (!mdp_push_locked("the frame", &base) || !mdp_push_locked("the frame again", &sums)) {
		return;
	}
	mdp_check("the frame's checksums repeat", mdp_both(&base, &sums));
	mdp_pipe_fill(0, COLOR_X_ABGR, STRIP_W, STRIP_H, STRIP_OUT_X, STRIP_OUT_Y);
	if (!mdp_push_locked("programmed, not staged", &sums)) {
		return;
	}
	mdp_check("a pipe programmed and not staged changes nothing", mdp_both(&base, &sums));
	mdp_stage(0, true);
	if (!mdp_push_locked("solid X", &x)) {
		return;
	}
	mdp_check("staged, LM0 changes and LM1 does not", mdp_only(&base, &x, 0));
	mdp_pipe_fill(0, COLOR_Y_ABGR, STRIP_W, STRIP_H, STRIP_OUT_X, STRIP_OUT_Y);
	if (!mdp_push_locked("solid Y", &y)) {
		return;
	}
	mdp_check("another colour, another checksum", mdp_only(&x, &y, 0));
	if (mdp_step < 3) {
		mdp_stage(0, false);
		if (mdp_push_locked("unstaged", &sums)) {
			mdp_check("unstaged, the frame's checksums again", mdp_both(&base, &sums));
			proof->finished = true;
		}
		return;
	}
	if ((front != FRAME_A && front != FRAME_B) || stride != FRAME_STRIDE) {
		mdp_say("mdp: the frame is at %#llx with rows of %u bytes: not one this test knows\n", front, stride);
		return;
	}
	strip = mdp_strips[front == FRAME_B];
	mdp_say("mdp: first fetch: DMA0 from %#llx, the rows' unused ends\n", front);
	mdp_strip_color(strip, COLOR_X_ARGB);
	mdp_pipe_fetch(0, front, stride, STRIP_X, STRIP_Y, STRIP_W, STRIP_H, STRIP_OUT_X, STRIP_OUT_Y);
	if (!mdp_push_locked("fetched X", &fetched)) {
		return;
	}
	mdp_check("fetched X is exactly solid X", mdp_both(&x, &fetched));
	mdp_strip_color(strip, COLOR_Y_ARGB);
	if (!mdp_push_locked("the memory changed to Y", &sums)) {
		return;
	}
	mdp_check("fetched Y is exactly solid Y", mdp_both(&y, &sums));
	mdp_strip_pattern(strip);
	if (!mdp_push_locked("the memory changed to a pattern", &pattern)) {
		return;
	}
	mdp_check("a pattern is neither colour", mdp_only(&x, &pattern, 0) && mdp_only(&y, &pattern, 0));
	*mdp_strip_pixel(strip, 3, 64) ^= 0x00010101;
	__asm__ volatile ("dsb sy" ::: "memory");
	if (!mdp_push_locked("one pixel of it changed", &sums)) {
		return;
	}
	mdp_check("one pixel changes the checksum", mdp_only(&pattern, &sums, 0));
	mdp_strip_color(strip, COLOR_X_ARGB);
	if (!mdp_push_locked("the memory back to X", &sums)) {
		return;
	}
	mdp_check("X again is exactly solid X", mdp_both(&x, &sums));
	mdp_stage(0, false);
	mdp_strip_color(strip, 0);
	if (!mdp_push_locked("unstaged", &sums)) {
		return;
	}
	mdp_check("unstaged, the frame's checksums again", mdp_both(&base, &sums));

	/*
	 * DMA0 over the frame's own pixels did not give the frame's checksum (run 184629), while it was exact against its own solid fill.
	 * So: what alpha the frame holds, whether a fetched colour's alpha or the kind of pipe matters.
	 */
	{
		uint32_t opaque = 0, first = 0, last = 0;

		for (uint32_t row = 0; row < STRIP_H; row++) {
			for (uint32_t col = 0; col < 256; col++) {
				last = *(volatile uint32_t *)(strip + (size_t)row * FRAME_STRIDE + (size_t)(142 + col) * 4);
				first = row == 0 && col == 0 ? last : first;
				opaque += (last >> 24) == 0xff;
			}
		}
		mdp_say("mdp: the frame at 142,1000: %u of %u pixels have alpha 255; first %#x last %#x\n", opaque, STRIP_H * 256, first, last);
	}
	mdp_strip_color(strip, COLOR_X_ARGB & 0x00ffffffu);
	mdp_pipe_fetch(TEST_DMA0, front, stride, STRIP_X, STRIP_Y, STRIP_W, STRIP_H, STRIP_OUT_X, STRIP_OUT_Y);
	mdp_stage(TEST_DMA0, true);
	if (!mdp_push_locked("DMA0, X with alpha 0 in memory", &sums)) {
		return;
	}
	mdp_note("a fetched pixel's alpha does not matter", mdp_both(&x, &sums));
	mdp_stage(TEST_DMA0, false);
	mdp_strip_color(strip, COLOR_X_ARGB);
	mdp_pipe_fetch(TEST_VIG2, front, stride, STRIP_X, STRIP_Y, STRIP_W, STRIP_H, STRIP_OUT_X, STRIP_OUT_Y);
	mdp_stage(TEST_VIG2, true);
	if (!mdp_push_locked("VIG2, X", &sums)) {
		return;
	}
	mdp_note("VIG2's X is DMA0's X", mdp_both(&x, &sums));
	mdp_strip_pattern(strip);
	if (!mdp_push_locked("VIG2, the pattern", &sums)) {
		return;
	}
	mdp_note("VIG2's pattern is DMA0's pattern", mdp_both(&pattern, &sums));
	mdp_stage(TEST_VIG2, false);
	mdp_strip_color(strip, 0);

	/* The frame's own pixels, over themselves: the same picture, unless the fetch is wrong anywhere */
	mdp_pipe_fetch(TEST_DMA0, front, stride, 142, 1000, 256, 256, 142, 1000);
	mdp_stage(TEST_DMA0, true);
	if (!mdp_push_locked("DMA0 over the same pixels", &sums)) {
		return;
	}
	mdp_note("the frame's pixels through DMA0 are the frame", mdp_both(&base, &sums));
	mdp_stage(TEST_DMA0, false);
	mdp_pipe_fetch(TEST_VIG2, front, stride, 142, 1000, 256, 256, 142, 1000);
	mdp_stage(TEST_VIG2, true);
	if (!mdp_push_locked("VIG2 over the same pixels", &sums)) {
		return;
	}
	mdp_check("the frame's pixels through VIG2 are the frame", mdp_both(&base, &sums));
	mdp_pipe_fetch(TEST_VIG2, front, stride, 143, 1000, 256, 256, 142, 1000);
	if (!mdp_push_locked("one pixel along", &sums)) {
		return;
	}
	mdp_check("one pixel along is not", mdp_only(&base, &sums, 0));

	/* Across x = 540: one pipe on each mixer, x in the pair's coordinates */
	mdp_pipe_fetch(TEST_VIG2, front, stride, 412, 1000, 128, 256, 412, 1000);
	mdp_pipe_fetch(TEST_VIG3, front, stride, 540, 1000, 128, 256, 540, 1000);
	mdp_stage(TEST_VIG3, true);
	if (!mdp_push_locked("VIG2 and VIG3 across the seam", &sums)) {
		return;
	}
	mdp_check("across the seam, the frame", mdp_both(&base, &sums));
	mdp_pipe_fetch(TEST_VIG3, front, stride, 541, 1000, 128, 256, 540, 1000);
	if (!mdp_push_locked("the right one a pixel along", &sums)) {
		return;
	}
	mdp_check("the right pipe moved, only LM1 changes", mdp_only(&base, &sums, 1));

	/* Each half, whole: twice the fetching of a frame, for one frame */
	mdp_pipe_fetch(TEST_VIG2, front, stride, 0, 0, 540, 2400, 0, 0);
	mdp_pipe_fetch(TEST_VIG3, front, stride, 540, 0, 540, 2400, 540, 0);
	if (!mdp_push_locked("both halves whole, over the frame", &sums)) {
		return;
	}
	mdp_check("both halves whole, the frame", mdp_both(&base, &sums));
	mdp_stage(TEST_VIG2, false);
	mdp_stage(TEST_VIG3, false);
	if (!mdp_push_locked("unstaged", &sums)) {
		return;
	}
	mdp_check("unstaged, the frame's checksums again", mdp_both(&base, &sums));
	proof->finished = true;
}

/* With the window server's frames going: both halves whole on top, so twice the fetching while the GPU draws. The picture stands still. */
static void
mdp_live_stage(void *context __unused)
{
	struct mdp_sums sums;

	mdp_pipe_fetch(TEST_VIG2, loader_screen[0], (uint32_t)loader_screen[1], 0, 0, 540, 2400, 0, 0);
	mdp_pipe_fetch(TEST_VIG3, loader_screen[0], (uint32_t)loader_screen[1], 540, 0, 540, 2400, 540, 0);
	mdp_stage(TEST_VIG2, true);
	mdp_stage(TEST_VIG3, true);
	mdp_push_locked("both halves staged over the live frames", &sums);
}

static void
mdp_live_fill(void *context __unused)
{
	struct mdp_sums sums;

	mdp_pipe_fill(TEST_DMA0, COLOR_X_ABGR, 256, 256, 142, 1000);
	mdp_stage(TEST_DMA0, true);
	mdp_push_locked("solid fill staged over the live frames", &sums);
}

static void
mdp_live_unstage(void *context __unused)
{
	struct mdp_sums sums;

	for (unsigned t = 0; t <= TEST_VIG3; t++) {
		mdp_stage(t, false);
	}
	mdp_push_locked("unstaged", &sums);
}

static void
mdp_last_push(void *context __unused)
{
	if (!hk_display_push_locked()) {
		mdp_say("mdp: the last push did not finish\n");
	}
}

static void
mdp_test(void)
{
	struct mdp_proof proof = { };
	uint32_t before;

	if (mdp_step >= 3) {
		mdp_map_strips();
	}
	if (!hk_display_locked(mdp_proof_work, &proof) || !proof.finished) {
		mdp_say("mdp: the proof did not finish\n");
		goto out;
	}
	mdp_say("mdp: proof %s: %u checks failed, %u underruns\n", mdp_failed == 0 ? "PASS" : "FAIL", mdp_failed, mdp_underruns);
	if (mdp_failed != 0) {
		goto out;
	}
	if (mdp_underruns != 0) {
		mdp_say("mdp: underruns already: no test over the live frames\n");
		goto out;
	}
	if (hk_display_locked(mdp_step >= 3 ? mdp_live_stage : mdp_live_fill, NULL)) {
		before = __atomic_load_n(&mdp_pushes, __ATOMIC_RELAXED);
		mdp_say("mdp: hold: %s for 3 s\n", mdp_step >= 3 ? "the picture stands still, fetched twice" : "a green square should be on the panel");
		IOSleep(3000);
		mdp_say("mdp: over the live frames: the window server's pushes %u, underruns %u\n", __atomic_load_n(&mdp_pushes, __ATOMIC_RELAXED) - before, mdp_underruns);
		hk_display_locked(mdp_live_unstage, NULL);
	}
out:
	hk_mdp_restore();
	hk_display_locked(mdp_last_push, NULL);
	mdp_say("mdp: after the test: underruns %u, CTL0 layers %#x %#x\n", mdp_count_underruns(), mdp_read(CTL_BASE(0) + CTL_LAYER(0)), mdp_read(CTL_BASE(0) + CTL_LAYER(1)));
	mdp_read_state("after the test");
}

static void
mdp_thread(void *parameter __unused, wait_result_t result __unused)
{
	/* the window server up and its boot's frames going: the test runs over the real thing */
	for (unsigned waited = 0; waited < 900 && __atomic_load_n(&mdp_pushes, __ATOMIC_RELAXED) < 40; waited++) {
		IOSleep(100);
	}
	IOSleep(2000);
	mdp_test();
	mdp_say("mdp: done\n");
}

/*
 * (e) The display's own context bank. The MDP's stream reaches a context through the firmware's match register (found by the stream,
 * not by number); its registers and tables are never read (a read is a CrashDump, hardware-qualcomm.md 1). Android and Linux write such a bank without reading it, and
 * we do not even do that: the stream's S2CR is pointed at a free bank of ours with an identity map of the display's memory, and
 * pointing it back is the whole way back. In our bank faults can be seen, and any memory can be mapped.
 *
 * The tables are in the top of the display's region, not in the kernel's memory: if a kernel hangs and the loader starts the next
 * one without our having said goodbye, the S2CR still leads to valid tables. Mapped as device memory here and walked non-cacheable,
 * so a table entry written is an entry seen. The region's own proven part is below them: 0x9c000000..0x9d3f6000.
 */
#define APPS_SMMU_PHYS          0x15000000u
#define SMMU_PAGE               0x1000u
#define SMMU_R_IDR0             0x20
#define SMMU_R_IDR1             0x24
#define SMMU_R_SMR(n)           (0x800 + 4 * (n))
#define SMMU_R_S2CR(n)          (0xc00 + 4 * (n))
#define SMMU_R_CBAR(n)          (SMMU_PAGE + 4 * (n))
#define SMMU_R_CBA2R(n)         (SMMU_PAGE + 0x800 + 4 * (n))
#define CB_SCTLR                0x00
#define CB_ACTLR                0x04
#define CB_TCR2                 0x10
#define CB_TTBR0                0x20
#define CB_TTBR1                0x28
#define CB_TCR                  0x30
#define CB_MAIR0                0x38
#define CB_MAIR1                0x3c
#define CB_FSR                  0x58
#define CB_FAR                  0x60
#define CB_FSYNR0               0x68
#define CB_TLBIASID             0x610
#define CB_TLBSYNC              0x7f0
#define CB_TLBSTATUS            0x7f4
#define CB_FSR_ALL              0xc00001feu
#define CB_CBAR_STAGE_1         (1u << 16 | 3u << 8 | 0xfu << 12)       /* (as instantnoodle_camera.c's) */
#define CB_SCTLR_ON             (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 8 | 1u << 12)    /* faults recorded, not held */
#define CB_TCR_39_NC            (25u | 2u << 12 | 1u << 23)             /* walks non-cacheable, outer shareable: Linux's non-coherent setup */
#define CB_TCR2_VALUE           (1u | 1u << 4 | 7u << 15)
#define CB_MAIR0_VALUE          0xf404ff44u                             /* attribute 0: normal, non-cacheable */
#define CB_ACTLR_DISPLAY        (1u << 8 | 1u << 1 | 1u << 0)           /* prefetch: Linux's qcom_smmu_actlr_client_of_match for qcom,sm8250-mdss */
#define DISPLAY_STREAM          0x820u
#define DISPLAY_STREAM_MASK     0x402u
#define DISPLAY_CONTEXT_OLD     4u              /* hardware-qualcomm.md's, of 2026-10; run 192908 found match register 6, context 5. Never read either */
#define DISPLAY_FROM            0x9c000000ull
#define DISPLAY_TO              0x9e300000ull
#define TABLES_PHYS             0x9e1fc000ull
#define TABLE_BYTES             4096u
#define WINDOW_TABLES           256u                                    /* third-level tables of the window: 2 MB each */
#define TABLES_COUNT            (4u + WINDOW_TABLES)
_Static_assert(TABLES_PHYS + TABLES_COUNT * TABLE_BYTES == DISPLAY_TO, "the tables end the display's region");
_Static_assert((TABLES_PHYS & 0x3fff) == 0, "the tables start on a page");
#define WINDOW_BASE             0x40000000u                             /* outside the identity map; SSPP addresses are 32 bits */
#define WINDOW_PAGES            (WINDOW_TABLES * 512u)
#define PTE_ATTR                (1ull << 6 | 2ull << 8 | 1ull << 10 | 1ull << 11)
#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_BLOCK               1ull

extern lck_mtx_t hk_smmu_lock;
/* The context the firmware gave the display: the others' scans for a free one (hk_wifi.c, hk_sound.c, instantnoodle_camera.c) must not take or read it once no S2CR points at it */
uint32_t hk_display_context = DISPLAY_CONTEXT_OLD;
static vm_offset_t smmu_regs, smmu_bank, smmu_tables;
static uint32_t smmu_match, smmu_s2cr_found, smmu_bank_n;
static bool smmu_flipped;
static uint32_t smmu_faults;

static inline uint32_t
smmu_rd(vm_offset_t base, unsigned offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static inline void
smmu_wr(vm_offset_t base, unsigned offset, uint32_t value)
{
	*(volatile uint32_t *)(base + offset) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
smmu_put(vm_offset_t base, unsigned offset, uint32_t value, uint32_t mask, const char *name)
{
	uint32_t back;

	smmu_wr(base, offset, value);
	back = smmu_rd(base, offset);
	if ((back & mask) != (value & mask)) {
		mdp_say("mdp: SMMU: %s written %#x, reads %#x\n", name, value, back);
		return false;
	}
	return true;
}

static inline volatile uint64_t *
smmu_table(unsigned table)
{
	return (volatile uint64_t *)(smmu_tables + (size_t)table * TABLE_BYTES);
}

/* table 0 the root, 1 the identity's second level, 2 the third level of its last 2 MB, 3 the window's second level, 4.. its third */
static void
smmu_tables_make(void)
{
	uint64_t last_block = DISPLAY_TO & ~0x1fffffull;

	smmu_tables = ml_io_map((vm_offset_t)TABLES_PHYS, TABLES_COUNT * TABLE_BYTES);
	for (unsigned table = 0; table < TABLES_COUNT; table++) {
		for (unsigned i = 0; i < 512; i++) {
			smmu_table(table)[i] = 0;
		}
	}
	smmu_table(0)[DISPLAY_FROM >> 30] = (TABLES_PHYS + 1ull * TABLE_BYTES) | PTE_TABLE;
	for (uint64_t at = DISPLAY_FROM; at < last_block; at += 0x200000ull) {
		smmu_table(1)[at >> 21 & 0x1ff] = at | PTE_ATTR | PTE_BLOCK;
	}
	smmu_table(1)[last_block >> 21 & 0x1ff] = (TABLES_PHYS + 2ull * TABLE_BYTES) | PTE_TABLE;
	for (uint64_t at = last_block; at < DISPLAY_TO; at += TABLE_BYTES) {
		smmu_table(2)[at >> 12 & 0x1ff] = at | PTE_ATTR | PTE_PAGE;
	}
	smmu_table(0)[(uint64_t)WINDOW_BASE >> 30] = (TABLES_PHYS + 3ull * TABLE_BYTES) | PTE_TABLE;
	for (unsigned t = 0; t < WINDOW_TABLES; t++) {
		smmu_table(3)[((uint64_t)WINDOW_BASE >> 21 & 0x1ff) + t] = (TABLES_PHYS + (4ull + t) * TABLE_BYTES) | PTE_TABLE;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
smmu_forget(void)
{
	smmu_wr(smmu_bank, CB_TLBIASID, smmu_bank_n);
	smmu_wr(smmu_bank, CB_TLBSYNC, 1);
	for (unsigned i = 0; i < 10000; i++) {
		if (!(smmu_rd(smmu_bank, CB_TLBSTATUS) & 1)) {
			return true;
		}
		delay(1);
	}
	mdp_say("mdp: SMMU: the context does not finish forgetting\n");
	return false;
}

bool
hk_mdp_smmu_is_up(void)
{
	return smmu_flipped;
}

bool
hk_mdp_smmu_up(void)
{
	bool used[256] = { false };
	uint32_t idr0, idr1, matches, contexts, pages, n;
	uint64_t root;
	bool found = false, good = false;

	if (smmu_flipped) {
		return true;
	}
	smmu_regs = ml_io_map(APPS_SMMU_PHYS, 2 * SMMU_PAGE);
	lck_mtx_lock(&hk_smmu_lock);
	idr0 = smmu_rd(smmu_regs, SMMU_R_IDR0);
	idr1 = smmu_rd(smmu_regs, SMMU_R_IDR1);
	matches = idr0 & 0xff;
	contexts = idr1 & 0xff;
	pages = 2u << (idr1 >> 28 & 7);
	used[DISPLAY_CONTEXT_OLD] = true;       /* (and never read) */
	for (n = 0; n < matches && SMMU_R_SMR(n) < 0xc00; n++) {
		uint32_t smr = smmu_rd(smmu_regs, SMMU_R_SMR(n)), s2cr = smmu_rd(smmu_regs, SMMU_R_S2CR(n));

		if (smr >> 31) {
			used[s2cr & 0xff] = true;
			if ((smr & 0x7fff) == DISPLAY_STREAM && (smr >> 16 & 0x7fff) == DISPLAY_STREAM_MASK && !found) {
				found = true;
				smmu_match = n;
				smmu_s2cr_found = s2cr;
			}
		}
	}
	if (!found || (smmu_s2cr_found >> 16 & 3) != 0 || (idr1 >> 31) != 0 || 2 * pages * SMMU_PAGE != 0x100000) {
		mdp_say("mdp: SMMU: the display's stream is not as known (found %d, match %u, s2cr %#x, idr1 %#x): not touched\n", found, smmu_match,
		    smmu_s2cr_found, idr1);
		goto out;
	}
	for (smmu_bank_n = 0; smmu_bank_n < contexts && used[smmu_bank_n]; smmu_bank_n++) {
	}
	if (smmu_bank_n >= contexts) {
		mdp_say("mdp: SMMU: no context free\n");
		goto out;
	}
	smmu_bank = ml_io_map(APPS_SMMU_PHYS + (pages + smmu_bank_n) * SMMU_PAGE, SMMU_PAGE);
	if (smmu_rd(smmu_bank, CB_SCTLR) & 1) {
		mdp_say("mdp: SMMU: context %u is on although nothing is sent to it: left alone\n", smmu_bank_n);
		goto out;
	}
	smmu_tables_make();
	root = TABLES_PHYS | (uint64_t)smmu_bank_n << 48;
	mdp_say("mdp: SMMU: the display's stream is match register %u, s2cr %#x; context %u gets our tables at %#llx\n", smmu_match, smmu_s2cr_found, smmu_bank_n,
	    TABLES_PHYS);
	if (!smmu_put(smmu_bank, CB_SCTLR, 0, ~0u, "sctlr") || !smmu_put(smmu_regs, SMMU_R_CBA2R(smmu_bank_n), 1, 1, "cba2r") ||
	    !smmu_put(smmu_regs, SMMU_R_CBAR(smmu_bank_n), CB_CBAR_STAGE_1, 0x30000, "cbar") || !smmu_put(smmu_bank, CB_TCR2, CB_TCR2_VALUE, 0, "tcr2") ||
	    !smmu_put(smmu_bank, CB_TCR, CB_TCR_39_NC, ~0u, "tcr") || !smmu_put(smmu_bank, CB_TTBR0, (uint32_t)root, ~0u, "ttbr0") ||
	    !smmu_put(smmu_bank, CB_TTBR0 + 4, (uint32_t)(root >> 32), ~0u, "ttbr0 high") || !smmu_put(smmu_bank, CB_TTBR1, 0, ~0u, "ttbr1") ||
	    !smmu_put(smmu_bank, CB_TTBR1 + 4, 0, ~0u, "ttbr1 high") || !smmu_put(smmu_bank, CB_MAIR0, CB_MAIR0_VALUE, ~0u, "mair0") ||
	    !smmu_put(smmu_bank, CB_MAIR1, 0, ~0u, "mair1")) {
		goto out;
	}
	smmu_wr(smmu_bank, CB_FSR, CB_FSR_ALL);
	smmu_wr(smmu_bank, CB_ACTLR, CB_ACTLR_DISPLAY);
	mdp_say("mdp: SMMU: actlr reads %#x\n", smmu_rd(smmu_bank, CB_ACTLR));
	if (!smmu_forget() || !smmu_put(smmu_bank, CB_SCTLR, CB_SCTLR_ON, 1, "sctlr on")) {
		goto out;
	}
	hk_display_context = smmu_s2cr_found & 0xff;
	mdp_say("mdp: SMMU: pointing the display's stream at context %u next (it is at %u)\n", smmu_bank_n, hk_display_context);
	smmu_wr(smmu_regs, SMMU_R_S2CR(smmu_match), (smmu_s2cr_found & ~0xffu) | smmu_bank_n);
	if ((smmu_rd(smmu_regs, SMMU_R_S2CR(smmu_match)) & 0x300ff) != smmu_bank_n) {
		mdp_say("mdp: SMMU: the S2CR reads %#x: refused\n", smmu_rd(smmu_regs, SMMU_R_S2CR(smmu_match)));
		smmu_wr(smmu_regs, SMMU_R_S2CR(smmu_match), smmu_s2cr_found);
		smmu_wr(smmu_bank, CB_SCTLR, 0);
		goto out;
	}
	smmu_flipped = true;
	good = true;
	mdp_say("mdp: SMMU: taken: s2cr %#x\n", smmu_rd(smmu_regs, SMMU_R_S2CR(smmu_match)));
out:
	lck_mtx_unlock(&hk_smmu_lock);
	return good;
}

/* Back to the firmware's context, from any context (no lock: a panic may hold it). Safe to run twice. */
void
hk_mdp_smmu_down(void)
{
	if (!smmu_flipped) {
		return;
	}
	smmu_flipped = false;
	smmu_wr(smmu_regs, SMMU_R_S2CR(smmu_match), smmu_s2cr_found);
	smmu_wr(smmu_bank, CB_SCTLR, 0);
	smmu_wr(smmu_bank, CB_FSR, CB_FSR_ALL);
	mdp_say("mdp: SMMU: the display's stream back on context %u (%u faults seen on ours)\n", hk_display_context, smmu_faults);
}

uint32_t
hk_mdp_smmu_fsr(void)
{
	uint32_t fsr;

	if (!smmu_flipped) {
		return 0;
	}
	fsr = smmu_rd(smmu_bank, CB_FSR);
	if (fsr & CB_FSR_ALL) {
		if (smmu_faults++ < 16) {
			mdp_say("mdp: SMMU: fault: fsr %#x far %#x%08x fsynr0 %#x\n", fsr, smmu_rd(smmu_bank, CB_FAR + 4), smmu_rd(smmu_bank, CB_FAR),
			    smmu_rd(smmu_bank, CB_FSYNR0));
		}
		smmu_wr(smmu_bank, CB_FSR, fsr);
	}
	return fsr & CB_FSR_ALL;
}

/*
 * Addresses in the window, for surfaces: first fit in a list of what is taken, each with an unmapped page after it. Pages are the
 * SMMU's 4K. hk_mdp_range_free() is called when a surface goes, and it is held by the presenter while it is on its way to the panel.
 */
#define RANGES                  128
static struct { uint32_t page, pages; } mdp_ranges[RANGES];
static unsigned mdp_range_count;
static LCK_GRP_DECLARE(mdp_lock_group, "hk-mdp");
static LCK_MTX_DECLARE(mdp_range_lock, &mdp_lock_group);

uint32_t
hk_mdp_range(uint32_t pages)
{
	uint32_t page = 0, address = 0;
	unsigned i;

	if (!smmu_flipped) {
		return 0;
	}
	lck_mtx_lock(&mdp_range_lock);
	for (i = 0; i < mdp_range_count; i++) {
		if (page + pages + 1 <= mdp_ranges[i].page) {
			break;
		}
		page = mdp_ranges[i].page + mdp_ranges[i].pages + 1;
	}
	if (mdp_range_count < RANGES && page + pages + 1 <= WINDOW_PAGES) {
		memmove(&mdp_ranges[i + 1], &mdp_ranges[i], (mdp_range_count - i) * sizeof(mdp_ranges[0]));
		mdp_ranges[i].page = page;
		mdp_ranges[i].pages = pages;
		mdp_range_count++;
		address = WINDOW_BASE + page * TABLE_BYTES;
	}
	lck_mtx_unlock(&mdp_range_lock);
	return address;
}

void
hk_mdp_range_page(uint32_t address, uint64_t phys)
{
	uint32_t page = (address - WINDOW_BASE) / TABLE_BYTES;

	smmu_table(4 + page / 512)[page % 512] = (phys & ~0xfffull) | PTE_ATTR | PTE_PAGE;
}

void
hk_mdp_range_ready(void)
{
	__asm__ volatile ("dsb sy" ::: "memory");
}

void
hk_mdp_range_free(uint32_t address, uint32_t pages)
{
	uint32_t page = (address - WINDOW_BASE) / TABLE_BYTES;

	if (smmu_tables == 0) {
		return;
	}
	lck_mtx_lock(&mdp_range_lock);
	for (uint32_t p = page; p < page + pages; p++) {
		smmu_table(4 + p / 512)[p % 512] = 0;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	if (smmu_flipped) {
		smmu_forget();
	}
	for (unsigned i = 0; i < mdp_range_count; i++) {
		if (mdp_ranges[i].page == page) {
			memmove(&mdp_ranges[i], &mdp_ranges[i + 1], (mdp_range_count - i - 1) * sizeof(mdp_ranges[0]));
			mdp_range_count--;
			break;
		}
	}
	lck_mtx_unlock(&mdp_range_lock);
}

/*
 * (d), (d4), (f): a frame's layers into the pipes. The panel is two mixers of 540 x 2400 with x in the pair's coordinates (run 182621),
 * so a layer is one pipe on each mixer it falls on. Each part keeps the whole layer's phase: dest x maps to source x as
 * (x - dx) * step on both sides of 540, the right pipe starting at the source pixel below that with the rest as its initial phase, so
 * that the seam falls on no pixel of its own. Scaling as Linux's _dpu_plane_setup_scaler3(): bilinear, no filter tables.
 *
 * Lowest layer at CTL value 4 (one below the firmware's, whose blend stages all say opaque), the next above it. All that is written
 * here is latched by the frame's flush.
 */
#define COLOR_FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))
#define FOURCC_BGRA             COLOR_FOURCC('B', 'G', 'R', 'A')
#define FOURCC_RGBA             COLOR_FOURCC('R', 'G', 'B', 'A')
#define FOURCC_420V             COLOR_FOURCC('4', '2', '0', 'v')
#define FOURCC_420F             COLOR_FOURCC('4', '2', '0', 'f')
#define FORMAT_RGB32            0x237ffu        /* the firmware's own */
#define FORMAT_NV12             (3u << 23 | 2u << 19 | 3u << 4 | 3u << 2 | 3u | 1u << 12 | 1u << 17 | 1u << 9 | 1u << 15)      /* mdp_format.c, YUV bit */
#define UNPACK_BGRA             0x03020001u
#define UNPACK_RGBA             0x03010002u
#define UNPACK_NV12             0x00000201u
#define QSEED_ON                (1u << 0 | 3u << 16 | 1u << 31 | 1u << 14)      /* bilinear; blend_cfg 1 */
#define QSEED_ON_ALPHA          (1u << 10 | 1u << 29)                           /* bilinear alpha */
#define QSEED_ON_YUV            (1u << 12 | 3u << 24)
#define PRELOAD_QSEED4          (4u | 2u << 8 | 4u << 16 | 2u << 24)
#define BLEND_OPAQUE            0x400u          /* (and constant 255 over 0) */
#define BLEND_PREMULTIPLIED     0x600u          /* foreground constant, background 1 - the pixel's alpha: _dpu_crtc_setup_blend_cfg() */
#define BLEND_CONSTANT          0xff0000u
#define LEFT_WIDTH              540u
#define PANEL_WIDTH             1080u
#define FRAME_CTL_BITS          (1u << 24)      /* border-out, as the firmware has it */

/* The pipes each mixer offers, in the order they are taken; DMA has no scaler or CSC */
static const uint8_t mdp_mixer_pipes[2][3] = { { TEST_VIG0, TEST_VIG2, TEST_DMA0 }, { TEST_VIG1, TEST_VIG3, TEST_DMA1 } };

static inline bool
mdp_is_yuv(uint32_t format)
{
	return format == FOURCC_420V || format == FOURCC_420F;
}

static inline bool
mdp_is_rgb(uint32_t format)
{
	return format == FOURCC_BGRA || format == FOURCC_RGBA;
}

/* one part of a layer on a mixer */
struct mdp_part {
	uint32_t sx, sw, phase;                 /* source column and width, initial phase */
	uint32_t dx, dw;
};

static bool
mdp_part_of(const struct hk_mdp_layer *l, unsigned lm, struct mdp_part *part)
{
	uint32_t from = lm == 0 ? l->dx : (l->dx > LEFT_WIDTH ? l->dx : LEFT_WIDTH);
	uint32_t to = lm == 0 ? (l->dx + l->dw < LEFT_WIDTH ? l->dx + l->dw : LEFT_WIDTH) : l->dx + l->dw;
	uint64_t step = ((uint64_t)l->sw << 21) / l->dw, first, last;
	uint32_t column, end;

	if (from >= to) {
		return false;
	}
	first = (from - l->dx) * step;
	last = (to - 1 - l->dx) * step;
	column = l->sx + (uint32_t)(first >> 21);
	end = l->sx + (uint32_t)(last >> 21) + 2;       /* the pixel after the last one that falls: bilinear reads it */
	end = end > l->sx + l->sw ? l->sx + l->sw : end;
	if (step == PHASE_ONE) {
		end = column + (to - from);             /* (no horizontal scaling: as many as it shows) */
	}
	part->phase = (uint32_t)first & (PHASE_ONE - 1);
	if (mdp_is_yuv(l->format)) {
		if (column & 1) {
			/* source columns are even here; the phase cannot say more than 1, so this much of a pixel is lost at the seam */
			column--;
			part->phase = PHASE_ONE - 1;
		}
		end = (end + 1) & ~1u;
		end = end > l->sx + l->sw ? l->sx + l->sw : end;
	}
	part->sx = column;
	part->sw = end - column;
	part->dx = from;
	part->dw = to - from;
	return true;
}

bool
hk_mdp_can_take(const struct hk_mdp_layer *layers, unsigned count)
{
	unsigned taken[2] = { 0, 0 };

	if (count == 0 || count > HK_MDP_LAYERS) {
		return false;
	}
	for (unsigned z = 0; z < count; z++) {
		const struct hk_mdp_layer *l = &layers[z];
		bool scaled = l->sw != l->dw || l->sh != l->dh;

		if ((!mdp_is_rgb(l->format) && !mdp_is_yuv(l->format)) || l->sw == 0 || l->sh == 0 || l->dw == 0 || l->dh == 0 || l->address[0] == 0 ||
		    l->dx + l->dw > PANEL_WIDTH || l->dy + l->dh > 2400) {
			return false;
		}
		/* limits Linux's catalog gives: 4 down, 20 up. Not asked of the hardware beyond them. */
		if (l->sw > 4 * l->dw || l->sh > 4 * l->dh || l->dw > 20 * l->sw || l->dh > 20 * l->sh) {
			return false;
		}
		if (mdp_is_yuv(l->format) && ((l->sx | l->sy | l->sw | l->sh) & 1)) {
			return false;
		}
		for (unsigned lm = 0; lm < 2; lm++) {
			struct mdp_part part;

			if (!mdp_part_of(l, lm, &part)) {
				continue;
			}
			/* a DMA pipe neither scales nor converts */
			if (taken[lm] >= 3 || (taken[lm] == 2 && (scaled || part.phase != 0 || mdp_is_yuv(l->format)))) {
				return false;
			}
			taken[lm]++;
		}
	}
	return true;
}

static uint32_t mdp_set_changed;

static void
mdp_set(unsigned offset, uint32_t value)
{
	if (mdp_read(offset) != value) {
		mdp_write(offset, value);
		mdp_set_changed++;
	}
}

/* YCbCr to RGB, S4.9, as Linux's dpu_csc10_* lay them out: BT.601 and BT.709, video and full range */
static const int16_t mdp_csc_matrix[4][9] = {
	{ 596, 0, 817, 596, -201, -416, 596, 1033, 0 },
	{ 596, 0, 918, 596, -109, -273, 596, 1082, 0 },
	{ 512, 0, 718, 512, -176, -366, 512, 907, 0 },
	{ 512, 0, 806, 512, -96, -240, 512, 950, 0 },
};

static void
mdp_csc_set(unsigned base, bool bt709, bool full)
{
	const int16_t *m = mdp_csc_matrix[(full ? 2 : 0) + (bt709 ? 1 : 0)];
	uint32_t c = base + VIG_CSC + CSC_MATRIX;

#define S13(v) ((uint32_t)(v) & 0x1fff)
	mdp_set(c + 0x00, S13(m[0]) | S13(m[1]) << 16);
	mdp_set(c + 0x04, S13(m[2]) | S13(m[3]) << 16);
	mdp_set(c + 0x08, S13(m[4]) | S13(m[5]) << 16);
	mdp_set(c + 0x0c, S13(m[6]) | S13(m[7]) << 16);
	mdp_set(c + 0x10, S13(m[8]));
#undef S13
	mdp_set(c + 0x14, full ? 0x3ffu : 0x40u << 16 | 0x3acu);
	mdp_set(c + 0x18, full ? 0x3ffu : 0x40u << 16 | 0x3c0u);
	mdp_set(c + 0x1c, full ? 0x3ffu : 0x40u << 16 | 0x3c0u);
	mdp_set(c + 0x20, 0x3ffu);
	mdp_set(c + 0x24, 0x3ffu);
	mdp_set(c + 0x28, 0x3ffu);
	mdp_set(c + 0x2c, full ? 0 : 0xffc0u);
	mdp_set(c + 0x30, 0xfe00u);
	mdp_set(c + 0x34, 0xfe00u);
	mdp_set(c + 0x38, 0);
	mdp_set(c + 0x3c, 0);
	mdp_set(c + 0x40, 0);
}

/* A pipe for one part of a layer. Only what differs is written; a pipe whose registers changed goes in the flush. */
static void
mdp_pipe_program(unsigned t, const struct hk_mdp_layer *l, const struct mdp_part *part)
{
	unsigned pipe = TEST_PIPE(t);
	uint32_t base = mdp_pipes[pipe].base;
	bool yuv = mdp_is_yuv(l->format), full = l->format == FOURCC_420F;
	uint32_t step_x = (uint32_t)(((uint64_t)l->sw << 21) / l->dw), step_y = (uint32_t)(((uint64_t)l->sh << 21) / l->dh);
	bool scaled = yuv || step_x != PHASE_ONE || step_y != PHASE_ONE || part->phase != 0;
	uint32_t src = l->sh << 16 | part->sw, src_uv = yuv ? (l->sh / 2) << 16 | part->sw / 2 : src;

	mdp_set_changed = 0;
	mdp_set(base + SSPP_SRC_SIZE, src);
	mdp_set(base + SSPP_SRC_XY, l->sy << 16 | part->sx);
	mdp_set(base + SSPP_OUT_SIZE, l->dh << 16 | part->dw);
	mdp_set(base + SSPP_OUT_XY, l->dy << 16 | part->dx);
	mdp_set(base + SSPP_SRC0_ADDR, l->address[0]);
	mdp_set(base + SSPP_SRC1_ADDR, yuv ? l->address[1] : 0);
	mdp_set(base + SSPP_SRC_YSTRIDE0, yuv ? l->stride[1] << 16 | l->stride[0] : l->stride[0]);
	mdp_set(base + SSPP_SRC_FORMAT, yuv ? FORMAT_NV12 : FORMAT_RGB32);
	mdp_set(base + SSPP_SRC_UNPACK_PATTERN, yuv ? UNPACK_NV12 : l->format == FOURCC_RGBA ? UNPACK_RGBA : UNPACK_BGRA);
	mdp_set(base + SSPP_SRC_OP_MODE, SSPP_OP_PE_OVERRIDE);
	mdp_set(base + SSPP_QOS_CTRL, mdp_pipe_found[t][R_QOS]);
	mdp_set(base + SSPP_PIX_EXT_C0_REQ, src);
	mdp_set(base + SSPP_PIX_EXT_C1C2_REQ, src_uv);
	mdp_set(base + SSPP_PIX_EXT_C3_REQ, src);
	if (pipe <= PIPE_VIG3) {
		uint32_t s = base + VIG_SCALER;

		if (scaled) {
			uint32_t step_uv_x = yuv ? step_x / 2 : step_x, step_uv_y = yuv ? step_y / 2 : step_y;

			/* (the op mode last in Linux, too; all of it latched together) */
			mdp_set(s + QSEED_INIT_Y_H, part->phase);
			mdp_set(s + QSEED_INIT_Y_V, 0);
			mdp_set(s + QSEED_INIT_UV_H, yuv ? part->phase / 2 : part->phase);
			mdp_set(s + QSEED_INIT_UV_V, 0);
			mdp_set(s + QSEED_STEP_Y_H, step_x);
			mdp_set(s + QSEED_STEP_Y_V, step_y);
			mdp_set(s + QSEED_STEP_UV_H, step_uv_x);
			mdp_set(s + QSEED_STEP_UV_V, step_uv_y);
			mdp_set(s + QSEED_PRELOAD, PRELOAD_QSEED4);
			mdp_set(s + QSEED_SRC_Y, src);
			mdp_set(s + QSEED_SRC_UV, src_uv);
			mdp_set(s + QSEED_DST, l->dh << 16 | part->dw);
			/* (bit 31, blend_cfg, reads back 0 on this scaler: run 193149. Compared without it, or every frame would rewrite it) */
			if ((mdp_read(s + QSEED_OP_MODE) | 1u << 31) != (QSEED_ON | (yuv ? QSEED_ON_YUV : QSEED_ON_ALPHA))) {
				mdp_set(s + QSEED_OP_MODE, QSEED_ON | (yuv ? QSEED_ON_YUV : QSEED_ON_ALPHA));
			}
		} else {
			mdp_set(s + QSEED_OP_MODE, mdp_vig_found[pipe][0]);
		}
		if (yuv) {
			mdp_csc_set(base, l->sh >= 720, full);
			mdp_set(base + VIG_CSC, CSC_OP_ON);
		} else {
			mdp_set(base + VIG_CSC, mdp_vig_found[pipe][VIG_REGS - 1]);
		}
	}
	if (mdp_set_changed != 0) {
		mdp_pending |= 1u << mdp_pipes[pipe].flush;
	}
}

bool
hk_mdp_commit(const struct hk_mdp_layer *layers, unsigned count)
{
	uint32_t want[2] = { mdp_layer_found[0] & FRAME_CTL_BITS, mdp_layer_found[1] & FRAME_CTL_BITS };
	unsigned taken[2] = { 0, 0 };

	if (mdp_stopped || !hk_mdp_can_take(layers, count)) {
		return false;
	}
	for (unsigned z = 0; z < count; z++) {
		const struct hk_mdp_layer *l = &layers[z];
		uint32_t value = 4 + z;

		for (unsigned lm = 0; lm < 2; lm++) {
			struct mdp_part part;
			unsigned t;

			if (!mdp_part_of(l, lm, &part)) {
				continue;
			}
			t = mdp_mixer_pipes[lm][taken[lm]++];
			mdp_pipe_program(t, l, &part);
			want[lm] |= value << mdp_pipes[TEST_PIPE(t)].shift;
			mdp_set_changed = 0;
			mdp_set(LM_BASE(lm) + LM_BLEND(value - 2), z == 0 || mdp_is_yuv(l->format) ? BLEND_OPAQUE : BLEND_PREMULTIPLIED);
			mdp_set(LM_BASE(lm) + LM_BLEND(value - 2) + 4, BLEND_CONSTANT);
			if (mdp_set_changed != 0) {
				mdp_pending |= FLUSH_LM(lm);
			}
		}
	}
	for (unsigned lm = 0; lm < 2; lm++) {
		if (want[lm] != mdp_layer_now[lm]) {
			mdp_write(CTL_BASE(0) + CTL_LAYER(lm), want[lm]);
			mdp_layer_now[lm] = want[lm];
			mdp_pending |= FLUSH_LM(lm);
		}
	}
	mdp_on_pipes_now = !mdp_stopped;
	return mdp_on_pipes_now;
}

/* The firmware's two pipes on a framebuffer again, a frame at the panel's size, for one that cannot go straight (the address: pushFrameLocked()) */
void
hk_mdp_leave_pipes(uint32_t stride)
{
	if (!mdp_on_pipes_now) {
		return;
	}
	mdp_on_pipes_now = false;
	for (unsigned t = TEST_VIG0; t <= TEST_VIG1; t++) {
		unsigned pipe = TEST_PIPE(t);

		for (unsigned r = 0; r < R_COUNT; r++) {
			if (r != R_ADDR) {
				mdp_write(mdp_pipes[pipe].base + mdp_regs[r], r == R_STRIDE ? (mdp_pipe_found[t][r] & ~0xffffu) | stride : mdp_pipe_found[t][r]);
			}
		}
		mdp_write(mdp_pipes[pipe].base + VIG_SCALER + QSEED_OP_MODE, mdp_vig_found[pipe][0]);
		mdp_write(mdp_pipes[pipe].base + VIG_CSC, mdp_vig_found[pipe][VIG_REGS - 1]);
	}
	for (unsigned lm = 0; lm < 2; lm++) {
		mdp_write(CTL_BASE(0) + CTL_LAYER(lm), mdp_layer_found[lm]);
		mdp_layer_now[lm] = mdp_layer_found[lm];
		mdp_pending |= FLUSH_LM(lm);
	}
	mdp_say("mdp: a frame the pipes cannot take: the framebuffers again\n");
}

bool
hk_mdp_on_pipes(void)
{
	return mdp_on_pipes_now;
}

void
hk_mdp_say_stats(uint32_t frames, uint32_t layered)
{
	mdp_say("mdp: %u frames through the pipes, %u of them of more than one layer; %u underruns, %u SMMU faults; %u surfaces in the window; layers %#x %#x\n", frames, layered,
	    mdp_count_underruns(), smmu_faults, mdp_range_count, mdp_layer_now[0], mdp_layer_now[1]);
}

uint32_t
hk_mdp_level(void)
{
	return mdp_step;
}

/* (MiniIOMobileFramebuffer.cpp, its start: autorefresh still fetching, with the MDSS registers mapped) */
void
hk_mdp_start(vm_offset_t dpu)
{
	if (mdp_step == 0 || dpu == 0) {
		return;
	}
	mdp_dpu = dpu;
	mdp_say("mdp: step %u; reads %#x%s\n", mdp_step, mdp_reads, mdp_step == 1 ? "; nothing is written in this step" : "");
	mdp_read_state("firmware's");
	mdp_judge();
	if (mdp_reads & 1) {
		mdp_read_clocks();
	}
	if (mdp_reads & 2) {
		mdp_read_scaler();
	}
	if (mdp_reads & 4) {
		mdp_read_sid();
	}
	if (mdp_reads & 8) {
		mdp_read_vbif();
	}
	if (mdp_step >= 2) {
		thread_t thread;

		if (!mdp_pipeline_as_expected) {
			mdp_say("mdp: not the pipeline this driver knows: step %u refused\n", mdp_step);
			mdp_say("mdp: done\n");
			mdp_stopped = true;
		} else {
			mdp_save();
			if (mdp_step <= 5 && kernel_thread_start(mdp_thread, NULL, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
			}
		}
	}
}

/* (MiniIOMobileFramebuffer.cpp, after each of the window server's pushes. gPanelLock is not held; only reads.) */
void
hk_mdp_after_push(void)
{
	uint32_t pushes;

	if (mdp_step == 0 || mdp_dpu == 0) {
		return;
	}
	pushes = __atomic_add_fetch(&mdp_pushes, 1, __ATOMIC_RELAXED);
	if (mdp_step != 1) {
		mdp_count_underruns();
		return;
	}
	if (pushes == 8) {
		mdp_read_state("after 8 pushes");
		mdp_judge();
	} else if (pushes == 60) {
		mdp_say("mdp: after 60 pushes: INTR status %#x (INTF1 underrun latched: %s)\n", mdp_read(MDP_TOP_INTR_STATUS),
		    (mdp_read(MDP_TOP_INTR_STATUS) & MDP_INTR_INTF1_UNDERRUN) ? "yes" : "no");
		mdp_say("mdp: done\n");
	}
}
