# Research: our own MDP (DPU) driver for the OnePlus 8 -- findings and staged plan

**Status (2026-11-05): built and running in RAM on the OnePlus 8; `mdp=8` is the board's default in `xnu-src` (nothing committed or installed).** The window server's frames go straight to the panel through the MDP's scaler, through a context bank of ours, and CoreAnimation's detached layers go through their own pipes. **Section 8 has the results and overrides what sections 0-7 guessed.** Code: `bsd/dev/instantnoodle_mdp.c`; boots: `tools/mdp/boot.sh`. Read [hardware-qualcomm.md](hardware-qualcomm.md) first; the pipeline is in [../subsystems/display.md](../subsystems/display.md).

Tags: **[observed]** seen on this phone (or in its iOS binaries on the host, where said); **[source]** read in public code or
device trees, listed at the end; **[inferred]** follows from those but was not seen; **[hypothesis]** a guess to be tested.

## 0. The answers in brief

1. **SMMU.** Stock Android programs the display's context bank from the kernel, through the hypervisor. It does not need the
   cont-splash region for anything but the handover [source]. Nothing public reads that bank first, and we need not either.
   The recommended route is a **fresh context bank of ours, then changing only the context index in the MDP's S2CR** (section
   1.4, option A). That keeps context bank 4 and its unknown tables intact as the way back. **Not yet seen on this phone**
   [inferred]. Until it is, layers are limited to what scanout has proven mapped: `0x9c000000-0x9d3f6000` [observed], not the
   whole 35 MB.
2. **Hardware.** 4 VIG (scaler, YUV), 4 DMA (RGB), 6 mixers, 6 ping-pongs, 4 DSC, 4 INTF. Today only VIG0/1, LM0/1, the first
   two ping-pongs and DSCs, and INTF1 carry the picture. All pipes share the MDP clock that is already running (section 2).
3. **Plan.** Keep the firmware's DSI, DSC, ping-pong and INTF as they are; only add pipes and blending. Steps `mdp=1..8`,
   first read-only, then a solid fill that fetches no memory, then fetches from the proven range (section 4).
4. **What it buys.** The largest win is not video: **the MDP's scaler can replace the GPU's scale pass on every frame**, saving
   about 1.9 GB/s of DDR traffic at 90 fps by arithmetic (section 6). iOS already has a hardware-layer path
   (`IOMFBDisplay::detach_layer`), but the SSPPs cannot rotate, so landscape video still needs one GPU rotation pass.

## 1. SMMU: the crux

### 1.1 What is known

| Fact | Tag |
|---|---|
| The MDP's streams `0x820`/mask `0x402` match SMR4, whose S2CR translates through context bank 4 (at `0x15084000`: 128 global pages, then 4K per bank). **Since 2026-11-05: match register 6, context 5 (section 8): find it by stream** | observed |
| Reading CB4's registers or its page tables is a CrashDump; no ATOS. The line is in hardware-qualcomm.md §1; the original log is not in `logs/`, so which of the two reads crashed, and how the tables were found, is not recorded | observed, thin |
| Reading free banks' registers, writing free SMRs/S2CRs/banks, and running our own stage-1 tables work (Wi-Fi, sound, camera) | observed |
| A stage-1 bank of ours with 4K tables serves stream `0x800`/`0x840` (IFE) with no fault. Those streams are on the same TBU range as the MDP's first port (`mnoc_hf_0_tbu`, `0x800-0xbff`) | observed + source |
| The hypervisor answers for match registers: it grants some stream IDs and refuses others by reading back 0 (`0x801`, `0x841`), harmlessly. It keeps a stage-1 CBAR when 0 is written back | observed |
| Scanout at `0x9c000000` and `0x9ca00000` and DSI DMA at `0x9c9ff000` work, so `0x9c000000-0x9d3f6000` is mapped at IOVA = PA. The rest of the 35 MB (`0x9d400000-0x9e300000`) has never been fetched by the MDP | observed |
| CB4's contents (identity table or bank with translation off, and how far it reaches) have never been read | unknown |
| What a translation fault on CB4 does: its FSR cannot be read, so a fault could not be seen or cleared | unknown |

### 1.2 Who owns context bank 4

- **The boot chain sets it up; the HLOS kernel is the intended next owner; the hypervisor mediates every write.** Evidence:
  - The phone's own device tree and Lineage's `kona-sde.dtsi` both mark the unsecure display stream with
    `qcom,iommu-earlymap`, and `apps-smmu` with `qcom,skip-init` [source; observed in `out/device-live.dtb`].
  - Android's `arm_smmu_handoff_cbs()` reads only SMR and S2CR, records the translating entries, marks their bank used and
    reuses that bank for the client that attaches. It reads no context-bank register first [source].
- **TrustZone** appears only for the secure stream `0x821`/`0x400`, `qcom,iommu-vmid` 0xa [source]. That is not our stream.
- **The hypervisor** on these SoCs emulates part of the SMMU. U-Boot's driver for these boards says so, and warns of "angry
  hypervisor firmware" turning BYPASS writes into FAULT [source]. Linux's EL2 remoteproc series says that under QHEE, IOMMU
  translation for remote processors belongs to the hypervisor and the EL1 host only gets one stage [source]. SM8250 being
  QHEE rather than Gunyah is [inferred].
- **Why reading CB4 crashes is not known.** [hypothesis] The hypervisor keeps that bank's page out of stage 2 until the first
  write, and Android and mainline both write first. **Do not test this**: no step below needs it.

### 1.3 How Android and mainline program it

**Android (Lineage `android_kernel_oneplus_sm8250`) [source]:**
1. `qcom,skip-init`: no reset; the boot loader's SMRs, S2CRs and banks stay in hardware.
2. `arm_smmu_handoff_cbs()` keeps the bank index for the client.
3. On attach, `qcom,iommu-earlymap` withholds `SCTLR.M`: the bank has new registers but translation is off, so the MDP fetches
   physical addresses while it scans the splash.
4. `_sde_kms_mmu_init()` maps each splash region with `one_to_one_map()` (same base as IOVA and PA), then clears
   `DOMAIN_ATTR_EARLY_MAP`, which turns translation on. Normal buffers then use `0x20000 + 0xfffe0000` of IOVA.
5. After the first commit the splash is unmapped and freed (`_sde_kms_splash_mem_put()`).

**Mainline [source]:**
1. `qcom_smmu_cfg_probe()` reads back valid SMRs and marks them bypass, so the splash survives. It probes the last S2CR for the
   firmware quirk that turns BYPASS into FAULT.
2. With the quirk, a BYPASS stream becomes TRANS through a reserved last bank with translation off.
3. `arm_smmu_device_reset()` writes **every** S2CR and every bank's `SCTLR`/`FSR`, and reads none of those banks.
4. `qcom,sm8250-mdss` is in the identity-domain client list. The msm DRM driver later allocates its own domain
   (`msm_kms_init_vm()`, `msm_iommu_disp_new()`, VA `0x1000` to just under 4 GB).
5. Mainline also resets the MDSS (`msm_mdss_reset()`) and re-initialises the panel. **We cannot do that**: see section 3.
   `sm8250-xiaomi-elish-common.dtsi` describes a simple-framebuffer in the same `0x9c000000 + 0x2300000` region on a retail
   SM8250 tablet, which is why [inferred] this path works with S2CR and bank writes under the retail hypervisor. Nobody has
   been seen doing it on a OnePlus 8 under this firmware, or on a port without the Linux probe.

**Common to both [inferred from the two]: a write to the MDP's stream mapping and bank is accepted, and neither reads the bank
before writing it.**

### 1.4 Options for arbitrary buffers (no read of CB4 in any of them)

| Option | How | For | Against |
|---|---|---|---|
| **A: new bank, flip S2CR4 (recommended)** | Own stage-1 bank: identity map of the splash plus an IOSurface window under 4 GB. Between pushes change only S2CR4's `[7:0]` | CB4 and its tables stay untouched, so writing back the found value is a full rollback. Our bank's FSR/FAR/TLB are readable: **faults become visible**. Same setup as the camera's | One S2CR write never tried here (section 1.5). Makes CB4 look free to our own scanners (section 1.6) |
| **E: alternate stream for layer pipes (cheap check first)** | Android's SID block (`sid_phys`, `0x0ae8f000`) writes a value into each pipe's SID register: later vendor code writes VM ID `<<2` for VIG/DMA, and the rotator's stream `0x215c` fits base + `0x1c`. Pipe gets e.g. `0x824`; a new SMR `0x824/0x402` to our bank | Base pipes stay on CB4 and S2CR4 is never touched | [hypothesis] on this SoC: per-pipe writes are seen only in later vendor code, and the hypervisor may refuse the SMR. The refusal is harmless and is a one-register test |
| B: Android's way | Program CB4 itself, write-only, with an identity map | Exact stock behaviour | No way back: firmware tables are gone. CB4's page is the known hazard. Only if A is refused |
| D: bank with translation off | S2CR TRANS to a bank with `SCTLR.M=0`, as U-Boot and the mainline quirk do | No tables | SSPP addresses are 32 bits, so nothing above 4 GB; IOSurfaces are not contiguous; a stray fetch cannot fault, so no visibility. Only as a fallback for contiguous buffers |
| C: second SMR for the same stream | | | **Never**: two matches are a stream-match conflict |

**Must every layer live in the cont-splash region? Only if A and E are both refused.** Then the budget is the proven 20 MB
(two framebuffers) until a fetch from the upper 15 MB is shown safe, which cannot be done without risking an invisible CB4 fault.

An own table is needed anyway, beyond reach: `MiniIOSurface` memory is wired but not contiguous, and the SSPP takes one linear
address per plane. The table is what makes such a surface whole, as for the camera's frames.

### 1.5 Accesses and what they can do

| Access | Evidence | Risk |
|---|---|---|
| Read SCR0, IDR0/1, any SMR, any S2CR | observed | none seen |
| Write a free SMR/S2CR, a free bank's registers | observed | none seen; refused IDs read back 0 |
| Read a free bank's registers (lowest free numbers only) | observed | none seen; high numbers untested, and mainline hides TZ-held last banks on some SoCs |
| **Read any register of CB4, or memory through its TTBR** | observed | **CrashDump. Never** |
| Read `CBAR(4)`/`CBA2R(4)` (global page) | never done | unknown. Not needed: do not |
| Write CB4 | Android and mainline do; never here | unknown. Option B only |
| **Write S2CR4 with a new context index** | mainline's quirk does (source); never here | **unknown; the riskiest single write in this plan** (announce it; Power+VolUp recovers) |
| Write S2CR type BYPASS for the MDP | firmware may make it FAULT | **Never**: the MDP would fault at once |
| An SMR overlapping `0x820/0x402` | conflict | **Never** |
| A new SMR for a non-overlapping ID | refused cleanly for other IDs | low |
| **MDP fetch that faults on CB4** (bad address, unstaged garbage) | never tried; invisible | **unknown: may hang the fetch. All addresses go through one range check until our own bank carries them** |
| Global TLB operations | not needed | avoid; only `TLBIASID` in our bank, as the camera does |
| Read the MDP SID block `0x0ae8f000`, VBIF `0x0aeb0000` | never done here | unclocked access is a hang on these SoCs; check the clocks first and announce |

### 1.6 A trap in our own code

`radio_space_find()` (`hk_wifi.c`), the same loop in `hk_sound.c`, and `smmu_claim()` (`instantnoodle_camera.c`) all take **the
lowest context no valid S2CR refers to**, and the first two then **read its registers**. CB4 is safe today only because SMR4
points at it. After option A's flip, CB4 looks free: if lower banks are taken, **starting the Wi-Fi or the sound would read CB4,
which is a CrashDump**. The camera already refuses context 4 but would then fail rather than skip it.

**Before any flip, all three must treat context 4 as used** (one shared constant; `used[4] = true` before the loop). Those
files carry other sessions' uncommitted work, so coordinate. Option E avoids this trap altogether.

## 2. The hardware

Offsets are MDSS-relative (base `0x0ae00000`, the repo's `gDPU`) = mainline catalog + `0x1000`. They match Lineage's
`kona-sde.dtsi`. DPU core 6.0. The `Today` column is from `probeDisplay()` (`fb_probe=1`) as summarised in display.md; **its raw
log is no longer in `logs/`** (no match in the uncompressed files), so step (a) repeats it.

### Pipes (SSPP)

| Pipe | Offset | Flush bit | xin | Abilities | Today |
|---|---|---|---|---|---|
| VIG0 | `0x5000` | 0 | 0 | scaler, CSC, YUV, multirect | **active**, left 540 px. `SRC0_ADDR`, `YSTRIDE0` written: known-safe |
| VIG1 | `0x7000` | 1 | 4 | same | **active**, right 540 px: known-safe |
| VIG2 | `0x9000` | 2 | 8 | same | read only by the probe; writes untested |
| VIG3 | `0xb000` | 18 | 12 | same | read only; writes untested |
| DMA0 | `0x25000` | 11 | 1 | RGB, multirect, no scaler or CSC | read only; writes untested |
| DMA1 | `0x27000` | 12 | 5 | same | read only; writes untested |
| DMA2 | `0x29000` | 24 | 9 | same, cursor-capable | read only; writes untested |
| DMA3 | `0x2b000` | 25 | 13 | same, cursor-capable | read only; writes untested |

Sub-blocks on VIG: scaler at `+0xa00` (`0xa0`), CSC at `+0x1a00` (`0x100`): **never touched here**. The vendor tree calls the
scaler `qseedv3lite`, and mainline's version 3.0 takes the same "lite" path. Mainline leaves the LUTs unprogrammed and
uses bilinear [source]. Whether the reset coefficients are right for that is [inferred].

**No SSPP on this SoC rotates by 90 degrees**: `rotation_cfg` is NULL in the catalog, and flips are `SRC_OP_MODE` bits 13/14
[source]. An offline rotator node exists (`qcom,mdss_rotator@aea8800`, stream `0x215c`) but is `status = "disabled"` in both the
public tree and this phone's [source, observed]: not planned.

### Mixers and output

| Block | Offsets | Today |
|---|---|---|
| CTL0-5 | `0x2000`, `0x2200` ... `0x2a00` | **CTL0 active**; `CTL_FLUSH` `+0x18`, `CTL_START` `+0x1c` written: known-safe. Layers `+0`/`+4` for LM0/LM1, EXT `+0x40`, EXT2 `+0x70`, EXT3 `+0xa0`. Others read only |
| LM0-5 | `0x45000` ... `0x4a000` (pairs 0-1, 2-3, 4-5) | **LM0/1 active**; LM2/3 read only; LM4/5 never touched |
| PP0-5 | `0x71000`, `0x71800` ... `0x73800` | **PP0/1 active** (PP0 done = INTR bit 8); PP2/3 read only; PP4/5 never touched |
| DSC0-3 | `0x81000`, `0x81400`, `0x81800`, `0x81c00` | **DSC0/1 active; never write** (section 3); DSC2/3 never touched |
| INTF0-3 | `0x6b000` (DP), `0x6b800` (DSI0), `0x6c000` (DSI1), `0x6c800` (DP) | **INTF1 active** (tear check written: known-safe); INTF2 read only; DP never touched |
| DSPP0-3 | `0x55000` ... `0x5b000` | never touched |
| MERGE_3D0-2 | `0x84000`, `0x84100`, `0x84200` | never touched |
| WB2 | `0x66000` | never touched |
| VBIF | physical `0x0aeb0000`, outside the `gDPU` map | never touched |

Per-mixer stage registers at `blendstage_base` `0x20, 0x38, 0x50 ... 0x110` (11 stages): `+0` `BLEND_OP`, `+4` `CONST_ALPHA`.
`LM_OP_MODE` bit 31 marks the right mixer. MISR at `+0x310/+0x314` gives a machine-readable checksum of a mixer's output.

### Limits and clocks

- **Limits [source]:** pipe line width 4096, mixer width 2560, upscale 20x, downscale 4x, YUV source rects even, pixel RAM 50 KB.
  Multirect needs no scaling and no YUV, so two RGB rects can share a DMA pipe. Max bandwidth low/high 13.7/16.6 GB/s.
- **Power and clocks [observed/inferred]:** the core GDSC is on (`dispcc +0x3000`), `mapDisplay()` forces the AHB clock (`+0x2080`),
  and the MDP core clock runs because scanout runs. The catalog shows no per-pipe power domain, only clock-force bits (`0x2ac`,
  `0x2b4`, `0x2bc`, `0x2c4`), so **an unused pipe's registers need nothing switched on** [inferred; the probe's reads agree].
- **The MDP clock's rate is unknown** (RCG `dispcc +0x20c8`, branch `+0x200c`; table 200/300/345/460 MHz): read it in step (a).
  It only matters if a scaled source raises the line rate. **Never write it.**
- **Bandwidth:** Linux votes `mdp0-mem`/`mdp1-mem` for each plane. Check what our `ddr` vote covers before a second full-screen
  layer: 1 GB/s average, about 1.2 GB/s burst during the 8.7 ms transfer.

## 3. Design principles

**"Our own driver" means owning the layers, not the display path.**

- **Do not re-initialise:** no `msm_mdss_reset()`-style reset, no writes to DSC, ping-pong, INTF timing, DSI or its PHY. Mainline can
  because it reprograms the panel; here DSC setup lost is coloured static until a phone reset [observed], and Android's on-command
  did not recover it. Mainline's own DSC over DSI on SM8150/8250 has a tracker issue for garbage output [source, title and search
  snippet only: the page blocked fetching].
- **One file:** `xnu/files/bsd/dev/instantnoodle_mdp.c` (board-gated, `kona`), called by `MiniIOMobileFramebuffer.cpp`. The
  framebuffer keeps swaps, fences, TE and the panel lock; the new file only knows pipes and stages.
- **One write function** with read-back and an allowed-offset list; any surprise ends all writing for the boot
  (hardware-qualcomm.md §9). Step (a) must learn which registers read back: `CTL_FLUSH` clears itself; SSPP registers are latched
  by the flush, so whether a read returns the written value is to be seen on an unused pipe first.
- **Writes only between pushes**, with `gPanelLock` held and `pushWait()` done: autorefresh is off, the MDP is idle, and everything
  latches together at the next `CTL_FLUSH`.
- **Fully programmed before staged, unstaged before torn down.** Never stage a pipe whose address has not been read back.
- **One way back:** `mdpRestore()`, registers only, no lock, bounded, safe to run twice. It restores the found `CTL_LAYER`s, blend stages,
  VIG0/1 size, format, scaler and S2CR. Call it at the top of `pushStop()`, the one place the 3-strikes failure, `hk_display_quiesce`
  and a panic all reach.

## 4. The staged plan

Order: (a) -> (b) -> (c) -> (d1-3) -> (e) -> (d4) -> (f). (f0) can ride on any boot. `mdp=0` (default) is today's behaviour exactly.
Each step is a boot-arg, says what it will touch before touching it, and ends with the found state restored. A failed check is read
from its log, not retried. Add the arg to `docs/BOOT_ARGS.md` when it is built.

### (a) `mdp=1`: re-derive the pipeline, read-only (1 boot)
Decode what `fb_probe=1` only prints:
- `CTL0` layers for LM0/LM1 plus EXT/EXT2/EXT3, as Android's `sde_rm_cont_splash_res_init()`/`get_staged_sspp` do; the stage per pipe
  (VIG0 bits 0-2, VIG1 3-5, VIG2 6-8, VIG3 26-28, DMA0 18-20, DMA1 21-23, DMA2/3 in EXT2; what the value means: section 8).
- `CTL_DSC_ACTIVE`, `CTL_MERGE_3D_ACTIVE`, `LM_OP_MODE`, `LM_OUT_SIZE`, the blend stages.
- Each pipe's QoS LUTs (`+0x60..0x6c`), `CDP` (`+0x134`), format (`+0x30..0x38`), `OUT_XY`.
- **The question it must answer:** is the firmware in **source split** (both pipes staged on both mixers, `OUT_XY` in the pair's
  coordinates) or one pipe per mixer? Every later coordinate depends on this. The tree declares `qcom,sde-has-src-split`.
- Newly read, announced first: `dispcc +0x20c8/+0x200c`, one VIG2 scaler register, `sid_phys`, VBIF. Each behind the
  `regs_alive()` pattern and its own sub-arg, in case one hangs.

Pass: the derived pipeline equals the expected one; otherwise everything after it refuses to run.

### (b) second layer

**`mdp=2`, solid fill, no fetch at all (1 boot).** Set the solid-fill bit in `SRC_FORMAT` and a colour in `SRC_CONSTANT_COLOR`: **the
pipe reads no memory, so no SMMU risk**. Target: DMA0 in the left half, 256 x 256, at CTL value 6 (one above the frame, section 8), opaque, for a few seconds over the live UI;
flush `CTL_FLUSH_FRAME | bit 11 | bit 6`. Then unstage and flush. Checks: the owner sees the square; the LM0 MISR changes and returns;
`MDP_INTR` bit 26 (INTF1 underrun) stays clear; PP done keeps arriving.

**`mdp=3`, fetch from the proven range (1-2 boots).** Source: a rect of the framebuffer **not being written**, inside
`0x9c000000-0x9d3f6000`, so no new mapping is assumed. The range check is in the write function. Then cross the seam: DMA0 + DMA1, one
per mixer, splitting a rect over x = 540. Copy VIG0's QoS LUTs. New xins may lack the firmware's QoS: if underruns appear under GPU
load, read VBIF and program the priority as mainline's `qos_rt_tbl`.

### (c) `mdp=4`: blending and z-order (1-2 boots)
Program modes from `_dpu_crtc_setup_blend_cfg()` [source]: opaque (`FG_CONST | BG_CONST`), premultiplied (`FG_CONST | BG_FG_PIXEL` with
`BG_INV_ALPHA`), and constant alpha. Test both orders, layer over base and layer under base (base needs alpha, so a test frame with a
transparent rectangle). CoreAnimation's surfaces are premultiplied, and its detach model appears to put video **below** a composite with
a hole (section 4f): that order is the one that matters.

### (d) scaler and YUV

- **`mdp=5` (d1-3, 2-4 boots).** On VIG2 + VIG3, off to the side of the live pipes:
  - d1: scaler enabled at 1:1 (`QSEED3_OP_MODE` bit 0, bilinear filter fields, `blend_cfg` 1, no LUTs).
  - d2: 1125 -> 1080 on one half (phase step `(1 << 21) * src / dst`, `SRC_SIZE`/`DST_SIZE`, `PRELOAD`; mainline's defaults).
  - d3: **the seam.** Two pipes must step identically: left from 0, right from source x = 562.5, that is 562 plus an initial phase of
    0.5 in the 21-bit `PHASE_INIT` field, and each must fetch a pixel past its half. Mainline's `dpu_plane_split` cuts at the integer
    midpoint and leaves the initial phase 0 [source], so this is **[hypothesis]**. Test with one-pixel vertical lines at the seam; judged by the owner's eye, because
    `xl screenshot` reads memory and cannot see the MDP's output.
- NV12 + CSC comes with (f): matrix by the surface's own attachment (601/709, video/full range; mainline carries only 601L).
- **`mdp=7` (d4, 2-3 boots, after (e)): the base frame through the scaler.** CoreAnimation's own surfaces (1125 x 2436, about 11.1 MB
  each at a 64-byte stride) go straight to VIG0/VIG1 with QSEED, and `eagl.c`'s `glBlitFramebuffer` pass is skipped. **This is the main
  benefit** (section 6). Fence wait stays; `SwapWait` already keeps the surface being sent from being drawn. Needs: `fb_gpu` off in a
  new way, `panelFor()` no longer the path, `loader_screen` told width and pitch or still fed a copy. Fallback: any frame this cannot
  take goes the existing GPU or CPU way.
  EDR is unaffected: its step is in CoreAnimation's own composite (`eagl.c` function 61), not in the scale pass [observed in source].

### (e) `mdp=6`: buffers outside the cont-splash region (3-5 boots; **tell the owner before each, a CrashDump is possible**)
Prerequisite: the three context scans fixed (section 1.6), unless E succeeds.

- **e0, reads and one refused-or-not write.** Find the MDP's SMR by matching the ID, not by number 4. Check S2CR is TRANS and remember it.
  Try option E's SMR on a spare register; if it reads back, keep E as an option and give it back.
- **e1, our bank, unreferenced.** `hk_smmu_lock`, first free context by the fixed scan, stage-1 as the camera does (`CBAR`, `TCR2`, `TCR`,
  `TTBR0`, `MAIR`, `SCTLR` last), its own ASID. Differences from the camera, untested:
  - **Tables at the top of the splash region**, not in kernel memory: a hung kernel's S2CR then still points at valid tables after the
    next kernel loads (the loader's 50 s watchdog path cannot restore it).
  - **Non-cacheable walks** (`IRGN`/`ORGN` 0, as Linux's non-coherent setup), since the mapping is write-combined.
  - Identity map: `0x9c000000-0x9e300000` only (2 MB blocks and 4K at the end, never into the DFPS data past it). Everything goes through
    it, **including the DSI DMA page, or every DCS command fails**.
- **e2, the flip, one frame.** In the gap after a PP done, with `gPanelLock` held:
  1. Write S2CR4 with only `[7:0]` changed; read back. A refusal ends here, harmlessly.
  2. Push one frame. Wait for PP done at most 100 ms.
  3. Read our FSR. Send one DCS (brightness) through the DSI DMA page; read FSR again.
  4. **Always flip back** and push once more.
  Any fault or timeout: back, and never again this boot. Log every value.
- **e3, stay on it for a run.** A `tools/t display`-size run: 90 Hz, screen off/on, one sleep (`tools/t power`), return to the loader, panic
  path (`hk_display_quiesce` flips back). Then the first fetch from the upper 15 MB, now with a visible FSR.
- **e4, IOSurfaces.** A window under 4 GB outside the identity range. Map surface pages when first scanned; unmap only after a PP done with
  the surface no longer staged, then `TLBIASID` + `TLBSYNC`. A guard page after each, as the camera's. **CPU-written surfaces must be
  cleaned to PoC by the kernel before the push**: `sys_dcache_flush` is a no-op here (HANDOFF, TODO T38 (17)).
- If underruns appear from TLB misses: `ACTLR` as `qcom_smmu_actlr_client_of_match` has it for `qcom,sm8250-mdss` [source]; untested under
  the hypervisor.

### (f) `mdp=8`: iOS multi-layer swaps

**What iOS has [observed in the host's QuartzCore and IOMobileFramebuffer of 20F66; symbol and string names only]:**
- `CA::WindowServer::IOMFBDisplay::detach_layer`, `swap_set_layer`, `update_surface_has_alpha`, `max_layer_bandwidth`,
  `AppleDisplay::detachable_layer_impl`, and "detached-below"/"detached-content" passes. `CAImageQueue` carries the detach state, which is the
  path of video and camera preview [inferred].
- The swap has 4 layers: `SWAP_SURFACE(n)`, `SWAP_SOURCE(n)`, `SWAP_DEST(n)` in `MiniIOMobileFramebuffer.cpp` [observed], the same shape as
  Asahi's `dcp_swap` per-surface arrays [source].
- **Limits CA reads, which our kernel does not publish:** `IOMFBScalingLimits` (`RGBLayer_MaxScale`, `RGBLayer_MinScaleFraction`, `YUVLayer_MaxScale`,
  `YUVLayer_MinScaleFraction`), `IOMFBMaxSrcPixels` (`MaxSrcBufferWidth/Height`, `MaxSrcRectWidth/Total`), `IOMFBMaxCompressedSizeInBytes`.
- Reason codes, three families: `kCA...` (`NonOpaque`, `Rotation`, `RectClipped`, `UnderBackdrop`, `NotClearAboveBlackBelow`, `NoDisplayCompositing`,
  `MismatchEDR` ...), `kDisplay...` (`YUVTooSmall`, `DestRectTooLarge`, `CompressionNotSupported` ...), `kServer...` (`ExceedScalarSizeLimits`,
  `SurfaceCopyFailed`, `VirtualizedDevice`, `ScreenTelemetry` ...). Environment names: `CA_PRINT_DETACHING`, `CA_DISABLE_DETACHED`, `CA_COLOR_DETACHED`.
  There is also the string "avoid detaching rgb".

**(f0) The next question costs no driver.** A boot with `CA_PRINT_DETACHING` in backboardd's environment, playing a video, would print why each
surface is rejected on today's kernel [hypothesis: that a release build honours it]. That says what to publish. Whether CA has ever sent a layer above
0 here is **not known**: the `IOLog` line for it does not reach the logs I searched, so also watch `SWAP_LAYERS_SET`.

**Mapping in the kernel:**
- `IOMobileFramebuffer::show()`: each surface with a non-empty `SWAP_DEST(n)` becomes a logical layer. Scale dest rects from 1125 x 2436 to the panel
  (0.96 and 0.985, as the GPU pass does), and split at x = 540 into one or two pipes. YUV takes VIG2/3; RGB takes DMA0-3 (two rects can share one with
  multirect, no scaling).
- Stage order from the layer index and `surface_has_alpha` [hypothesis: layer 0, the composite with its hole, on top; prove by test (c)].
- **Publish true limits only.** CA cannot be told after the fact, so a swap the MDP cannot take must still show: the existing path composites it (`drawLayer`).
  `panelFor()`'s "whole" test, which today refuses any swap with another layer, needs changing or every such frame falls to the CPU copy.
- Hold surfaces until the PP done of the push that used them; in command mode that is enough (no scanning after it).
- `panelPower(true)` re-pushes the latest frame: it must recommit the whole layer set, not only the base address.

**Realistic reach:**
- **Landscape video does not reach it unrotated.** `kCA...Rotation` exists and the SSPP cannot rotate; a real iPhone has the M2 scaler for that
  (`AppleM2ScalerCSCDriver`, `copy_surface`), and this port has none, so `kServer...SurfaceCopyFailed` is likely [hypothesis].
  What is left is one GPU rotation pass into a surface, which can still save the composite.
- Portrait video and a preview already upright are the direct cases. Whether the camera's `AVCaptureVideoPreviewLayer` arrives with a rotation
  transform is to be seen in (f0).

## 5. What must keep working

| Item | Why it could break | Rule |
|---|---|---|
| `fb_push`/TE, PP done, `gPushDone`, 3 strikes | `CTL_FLUSH_FRAME` must grow by the staged pipes' and mixers' bits | Mask computed per commit; `mdp=0` is exactly today's value |
| `xl screenshot` / `rec_secs` | Read a framebuffer in memory, so they do not see MDP layers or scaling | Say so in the log; `loader_screen` gets the real pitch and width in (d4) |
| 90 Hz switching (`panelRate`, DCS on DSI DMA) | DSI DMA reads through the MDP's stream; bandwidth at 90 is higher | Identity map must include `0x9c9ff000`; test both rates in e3 |
| HBM (`hk_display_hbm`, DCS `0x53`/`0x51`) and brightness | Same DSI path and `gPanelLock`; off on panic | Unchanged; e2 sends one DCS on our bank before anything else relies on it |
| `return_dark=1` | 0x28 after the last push, then back to autorefresh | `mdpRestore()` before `pushStop()`'s autorefresh, after the DCS; DSI DMA still mapped either way |
| `hk_display_quiesce` from a panic | No lock, may be in any state | Registers only, bounded; S2CR back before autorefresh; our bank off after. `loader_return.c` calls it last, after the camera's and Wi-Fi's teardown |
| A `hk_wifi_quiesce` halt before display quiesce | The Wi-Fi's halt path can stop first (HANDOFF) | S2CR stays on ours with valid tables: reason for tables in the splash region |
| A later kernel with `mdp=0` after a hang | Finds S2CR not at CB4 | Tables stay valid. New kernels log and restore when S2CR is not as the boot found it. Reset clears all |
| Screen off/on, sleep | `pushFrameLocked` "remembers" only one address | Remember the whole commit; rerun `tools/t power` |
| Wi-Fi, sound, camera SMMU scans | Section 1.6 | Fixed before any flip |
| **Redwood** | Shared `MiniIOMobileFramebuffer.cpp` | All of it behind `instantnoodle()` and `mdp=` |

**Never in this work:** DCS 0x10; reading CB4; `touchscreen=1`; writing an MDP clock; any flash write. No register dumps in docs: this page holds public offsets only.

## 6. What it buys, risks, effort

**Cost model at 90 fps [arithmetic, not measured]:**

| Traffic per second | Today | After (d4) |
|---|---|---|
| GPU reads CA's frame for scaling | 986 MB (1125 x 2436 x 4) | 0 |
| GPU writes the scaled frame | 933 MB (1080 x 2400 x 4) | 0 |
| MDP reads the scanout buffer | 933 MB | 986 MB |
| **Total** | **2.85 GB** | **0.99 GB** |

The GPU's time for that pass and the fence wait behind it also go; neither is measured here. M4's numbers plus `tools/pipeline-latency.py` are
the before and after. Later, not planned: UBWC between A650 and MDP (`msm_mdss_4x_setup_ubwc`, `UBWC_STATIC`), DSPP gamut/PCC, WB2 as an off-line compositor.

| Step | Boots | Work | Main risk | If it goes wrong |
|---|---|---|---|---|
| (a) read-only | 1 | 0.5 day | New reads of an unclocked block | Hang; loader's watchdog. Sub-args separate them |
| (b) solid fill, fetch | 2-3 | 1 day | Underrun on a new xin; a pipe left staged | Log; `mdpRestore` |
| (c) blending | 1-2 | 0.5-1 day | Low | Wrong picture only |
| (d1-3) scaler, seam | 2-4 | 2-3 days | A visible seam or LUT defaults wrong; **the seam may not be fully solvable** | Falls back to the GPU pass, no loss |
| (e) SMMU | 3-5 | 2-4 days | **The S2CR write: refusal is harmless, a hypervisor fault is a CrashDump**; a fault under real-time fetch; other drivers' scans | Power+VolUp; RAM only, nothing lasting. If refused: splash-only, see 1.4 |
| (d4) base through scaler | 2-3 | 2-3 days | CA surface lifetime vs the push; `xl screenshot` changes | `mdp=0`, GPU pass |
| (f) iOS layers | 3-6 | 1-2 weeks, mostly userland, most uncertain | CA may reject for reasons we cannot satisfy | Stop at (d4); nothing is lost |

**Recommendation:** (a), (b), (c) and (f0) first: small, low risk, and (a) decides the coordinates while (f0) decides whether (f) is worth doing. Then (d1-3) and
(e), with the owner told beforehand. (d4) is the goal that pays off regardless of (f).

## 7. Open questions

1. Does the hypervisor accept a new index in S2CR4 here? (e2; the deciding test.)
2. Source split or per-mixer staging? (a)
3. Can the seam be made invisible with a fractional initial phase? (d3)
4. Does QSEED work without LUTs from this firmware's state? (d1)
5. Which VBIF/QoS state do unused xins have? (a)
6. Why does CA reject surfaces on this port, and which order does it want? (f0, c)
7. Two or three CA surfaces in flight? `fb_queue` suggests up to three, more than the splash could hold (only matters if (e) is refused).

## 8. Results on the phone

**(a) `mdp=1`, kernel mdp-5fef2e52, run 182621, verified on phone (reads only).** The phone came back to the loader, no panic, no underrun latched after 60 pushes.

- **Answer to section 7's question 2: both.** Each pipe is staged only on its own mixer (VIG0 on LM0, VIG1 on LM1), but the coordinates are the pair's: VIG1's `OUT_XY` x is 540, and LM1 has the right-mixer bit. So a layer's x is in 0..1079, and a layer crossing x = 540 needs one pipe on each mixer. Both LMs are 540 x 2400; border-out is on for both.
- **Stages.** The firmware stages the base at CTL value 5, not at the bottom. In Linux's numbering the value is `dpu_stage + 1` with `DPU_STAGE_BASE` = 0 and `DPU_STAGE_0` = 1 [source], so value 5 is `DPU_STAGE_3`, with its blend registers at `blendstage_base[3]`, LM + `0x68`. Value 1 would be the base layer, with no blend registers. **Layers above the frame take values 6 and 7, or the extension bit; four values lie below it.** The first three blend stages read opaque (foreground constant, alpha 255 over 0).
- **Format of the frame:** ARGB8888 as Linux's formats have it (four bytes, BGRA in memory, alpha enabled in the pipe); the stage's constant blend makes it opaque. No scaling, no multirect, no compression.
- **Every pipe already has the firmware's QoS** (danger, safe and creq LUTs, `QOS_CTRL` 1), used or not, one set for the VIGs and another for the DMAs. So section 4(b)'s "copy VIG0's QoS LUTs" is not needed for a first try, and VBIF was not read.
- **The MDP clock is 300 MHz** (PLL1 at 600 MHz, divided by 2; `dispcc` read safely). Scaling 1125 x 2436 to 1080 x 2400 needs roughly 540 x 2436 x 90 pixels a second per pipe, under 125 MHz [arithmetic, ignoring blanking and prefill], so **no clock write is expected**.
- **VIG2's scaler registers read safely**, and it is off (what they say: `mdp=3` below).
- After the pushes: `SRC0_ADDR` and `YSTRIDE0` read back what `pushFrameLocked()` and the framebuffer wrote, and a push leaves `CTL_FLUSH` at 0.
- Not seen: the screen (nothing was to change). `xl screenshot` cannot see MDP layers anyway (section 5).

**(b) `mdp=2`, a solid fill with no fetch, kernel mdp-05dbce3e, run 184016, verified on phone by the mixers' checksums (nobody looked at the panel).** DMA0 at CTL value 6 with the frame at 5, 256 x 256 at x 142, y 1000, on LM0.

- **Proof, six pushes of one frame under one hold of the panel's lock (so the window server's next push waits):** the checksums repeat on the same frame; programming the unstaged pipe changes neither mixer; staging changes LM0 and leaves LM1 identical; staged repeats; unstaging gives LM0's original checksum back exactly. So the second layer is in the mixer's output and leaves nothing behind.
- **MISR works as Linux's `dpu_hw_setup_misr()` does it** (free-running, one frame), and it is the check for what `xl screenshot` cannot see. It proves the picture changed and came back, not the square's colour or place.
- **A pipe's registers read back what was written at once**, before any flush, and after it (14 registers on DMA0, no difference). So the one write function can trust an immediate read-back there.
- **The window server's own pushes go on over a staged layer:** 182 pushes in 3 s, all finished, no underrun latched. Their flush needs no extra bits once the layer has latched.
- **Run 183700 could not decide:** a new frame arrived during the same push as the staging, so both mixers changed. Compare only under the lock.
- `hk_mdp_restore()` was run at the end of each test; it has not yet been seen on a failure or panic path.

The owner wrote 「我看到了 OK」 right after the supervisor's report on this step. The supervisor reads that as the green square seen on the panel, though it could mean the message: **reported, with that doubt**. Not otherwise confirmed by eye.

**(b) `mdp=3`, the first fetches by new pipes, kernels mdp-450ab32e (run 184629) and mdp-bb263517 (run 185001), verified on phone by checksums, 0 underruns, no SMMU change.** All under one hold of the lock, so every push sends the same frame.

- **A fetch is what memory holds, not stale and not empty.** Source: the rows' unused ends (x 1080..1087 of the front framebuffer, never shown, in the proven range), written by the kernel between pushes. Fetched colour X and Y through DMA0 gave exactly the checksums of solid fills of X and Y; a pattern gave another; one pixel changed gave another; back to X gave X's again. This also proves the channels: `0xAARRGGBB` in memory under unpack `0x03020001` equals `0xAABBGGRR` as a solid fill under `0x03010002`.
- **VIG2 and VIG3 are exact against the firmware's path.** VIG2 over the frame's own pixels gave the frame's checksums; a pixel along changed LM0 only. VIG2 + VIG3 across x = 540 gave the frame; the right pipe a pixel along changed LM1 only. Each half whole gave the frame, with twice the fetching for that frame.
- **DMA pipes are not exact against VIG pipes.** On the same bytes, DMA and VIG give different checksums, for the frame's pixels and for a plain colour. The sampled frame's alpha is all 255, and a fetched colour's alpha does not change an opaque DMA layer, so alpha is not the cause. The cause, and whether the difference can be seen, are **not known** [hypothesis: the two paths carry different bit depths into the mixer]. **So a frame or a part of it goes through VIG pipes**, and DMA results must be compared with DMA.
- **Fetching through new pipes reaches the same memory**, which also says their streams land in the same context bank [inferred: no fault, right pixels, and Android's tree gives one stream mask for all of them].
- **Load:** both halves whole on VIG2 and VIG3 over the live frames for 3 s, with the window server's 129 pushes and the GPU drawing: 0 underruns, while `MDP_INTR_STATUS` bit 26 is read after every push and cleared if set. VBIF was not read or needed.
- **Scaler block:** +0 reads `0x3000` on all four VIGs, which fits scaler 3.0 of Linux's catalog (`dpu_vig_sblk_qseed3_3_0`), not the `0x30000` this page first said [inferred]. It is off on all four. Linux reads that offset both as `QSEED3_HW_VERSION` and as `SSPP_VIG_OP_MODE`; only the first reading makes sense here. CSC op reads `0x2` on all four: the source-format bit, enable off, so VIG0's frame is not converted.
- `mdp=2` alone has not been run since the code was reorganised; `mdp=3` runs the same checks first, and they passed.

**(e) `mdp=6`, the display's own context bank: taken, verified on phone in every boot since run 193149.**

- **The hypervisor accepts a new context in the display's S2CR, and a clean return puts it back** (the next boot finds the old value; the return line was also seen in the log of run 194653). No CrashDump in any of about ten boots.
- **The display's stream is not at SMR4 / CB4 any more: runs from 2026-11-05 find match register 6, context 5.** The driver finds it by stream (`0x820`, mask `0x402`) and the other drivers' scans now leave out both 4 and whatever the display had (`hk_display_context`). Which context is the unreadable one has moved, so **never read by number**. Why it moved is not known [hypothesis: more streams set up by the boot chain than in October].
- Our bank: identity map of the display's region with 2 MB blocks, tables at the top of it, walked non-cacheable, plus a 512 MB window of 4K pages at `0x40000000` for surfaces. `ACTLR` as Linux's table for `qcom,sm8250-mdss` reads back `0x103`. A push and a DCS through it came through with `FSR` 0 before anything relied on it; **0 faults in tens of thousands of frames**.

**(d), (d4) `mdp=7`, frames straight from their surfaces, scaled by VIG0 and VIG1: verified on phone by logs, run 193149 on; panel not confirmed by eye yet.**

- 1125 x 2436 BGRA to 1080 x 2400, bilinear, no filter tables, version `0x3000` so QSEED4's preloads. The right pipe starts at source column 562 with initial phase 0.4999; both pipes have 563 columns for 540 shown, and the hardware takes that.
- `QSEED3_OP_MODE` bit 31 (`blend_cfg`) reads back 0 on this scaler.
- **0 underruns**, read after every push. **The owner, using the phone on the pipes (runs just before 195236), relayed by the supervisor: 「試過了，很不錯」, and asked about wrong colours, a line at the middle or stutter: 「完全沒有」.** That is the eye's check of colours and the seam, in those general terms; no closer look was made, and there is no checksum for the seam (each mixer's covers one side).
- The GPU's panel framebuffers are not asked for (selector 200 says not ready), so `eagl.c` stops scaling by itself. A frame the pipes cannot take goes the old way through a framebuffer, by the CPU: a swap with no layer does that on every screen-off.

| Same scene, nobody touching | GPU scale (runs 192908, 194917) | MDP pipes (run 195236) |
|---|---|---|
| Home screen moving | 77-81 fps, GPU busy 27-30 %, 3.45-3.73 ms a frame | 76-80 fps, GPU busy 21-25 %, 2.69-3.13 ms a frame |
| Still, the Clock icon's 30 fps | GPU busy 4 %, 1.3 ms a frame | GPU busy 2 %, 0.65 ms a frame |

The scale pass cost about 0.65-0.75 ms of GPU a frame. Boots where the owner was using the phone read 40-70 % busy: that is what they were doing (116 touches in run 194653), not the pipes. **Count `touch: finger` lines before comparing two boots.**

**Frame times, home screen moving, nobody touching** (`from one frame to the next`, printed to the log with `mdp=`):

| | GPU scale, run 203118 | Pipes, run 203542 (kernel mdp-1de6a002) |
|---|---|---|
| Frames a second | 79-81 | 78-82 |
| Half under, 95 % under | 12 ms, 13-14 ms | 12 ms, 14-15 ms |
| The longest in 5 s | 38-59 ms | 47-70 ms |
| Swaps a refresh late, in 5 s | 8-16 | 21-39 |
| GPU busy | 27-30 % | 22-25 % |

Late swaps are gaps of more than 13.9 ms between the window server's swaps, not pushes. Why there are more is not known; it did not change when `show()` stopped waiting for the push's end (run 202241: 25-37, that wait was removed for run 203542, which also brought the frame-time histogram to the GPU scale's). The longest figures include the shots below.

**The owner's 「現在卡卡的」 (run 201040) was on the Metal compositor**, which they had not used before, in a boot of Maps and Weather. Compared by numbers: the same with `edr=0` (run 200821), 0 underruns, nothing falling back. Two causes [inferred from these numbers]: **the Metal compositor is slow by itself:** moving 49-62 fps with the pipes, 56 without (runs 202548, 202819), Maps and Weather 2-24 fps both ways, GPU wait up to 20 ms. **The pipes do not speed Metal up:** an earlier "62 against 30" on this page compared with an older kernel. And **my copy for `xl screenshot`, then every 300 ms, on the presenter:** now every 2 s, in runs from the computer only.

**(f) `mdp=8`, iOS's layers in their own pipes: verified on phone by logs, runs 194212 and 195543; the picture itself not confirmed by eye.**

- From QuartzCore's code (20F66, host) [observed]: with nothing detached, the composite is layer 0. With a surface detached, the composite is layer 1, on top, with a hole of colour 0 and alpha; the surface is layer 0 (RGB) or 2 (YUV). So the order here is 0, 2, 3, then 1 on top, and the composite blends premultiplied.
- **A swap names only the layers that change** [observed, run 204521]: with a surface detached, `SWAP_LAYERS_SET` is `0x7` with `SWAP_LAYERS_CLEARED` `0x4` when the detached surface is new, and `0x2` with nothing cleared when only the composite is (about 150 of 240 swaps in Maps). A layer neither set nor cleared stays as it is, and stays in use. **Taking it for gone was the owner's flashing in Maps (runs 201040, 202548): the composite with its hole alone on the panel, black for a frame, every few frames.** Fixed in kernel mdp-070cd902: 44 such frames in run 204134, 0 in run 204521; the owner's eye on the fixed kernel has not been reported. `mdp_trace=N` prints the first N of these swaps with pixels sampled from memory.
- Published: `IOMFBScalingLimits` (4 and 20 for both kinds), `DisplayPipeStrideRequirements` and `DisplayPipePlaneBaseAlignment` (16). Nothing in the registry was blocking detach before; without the limits CoreAnimation would ask for what the pipes cannot do.
- **CoreAnimation does detach on the Metal compositor:** Maps' full-screen `CAMetalLayer`, BGRA 1125 x 2436, under the composite: two pipes a mixer, 0 underruns. It needs an app with a surface layer: on the OpenGL compositor nothing on the home screen or in Settings qualified.
- **"luma boost active" (Display reason 0x22) refused about three of four tries, and the cause was ours: CoreAnimation was told the screen has 0 nits.** QuartzCore's `begin_update` boosts dim content when its SDR nits are under 20 on this product type [host]. **With the backlight as it was (`edr=0`), `[CAWindowServerDisplay nits]` reads 0 for the whole run** (`setNits: 0`, run 200821), so the boost was on at every brightness. CoreBrightness's new architecture (`edr=1`) calls `setNits:` with the real value: 47 at the first brightness, no refusal, 242 of 242 detaches (run 201040). Along the slider the nits follow, 2 to 350 (run 201647). **So `edr=1` is the board's default now.** It was an environment switch for an hour (`caenv=CA_DISABLE_LUMA_BOOST=1`, 99 of 99, run 195543): not needed any more, and not a default. Below 20 nits detaches are refused again, as on a real phone of this type [inferred]: the frame still goes through the pipes, composited.
- **The dim Photos picture (HEIC's open item) is not this:** its runs had `edr=2`, with the display at 70-100 nits (runs 165643, 170709), so CoreAnimation had its nits. Its white of 187 is the EDR factor of 0.5 with no rise of the backlight behind it; that stays open (KNOWN_ISSUES K33).
- **Not run on the phone: the NV12 path** (`FORMAT_NV12`, CSC, chroma scaling). No video or preview surface reached the pipes: no video has been played on this system yet, and Camera.app refuses to open on a RAM `/var`. Rec.709 NV12 also needs `CA_DETACH_ANY_PRIMARIES=1`, or an update with an empty dirty shape; it is not among the defaults. Landscape camera buffers would be refused for rotation [inferred].
- Flips and turns (`SWAP_TRANSFORM`, `+0x114`) are not mapped: such a swap goes the old way, where a YUV layer is not drawn at all.

**What must keep working (section 5), as seen with frames on the pipes:** 90 Hz yes; a DCS through our bank yes; screen off and on yes (runs 194653, 195236); return to the loader dark and the next boot yes; camera, Wi-Fi and sound scans with the fix yes; **HBM** on and off, 3 s, TE unchanged, 0 faults (run 201353); **CoreAnimation's EDR step** with a headroom of 2 forced: the Calendar widget's white 187 where it is 255 without, in the composite as before (run 201353); the brightness slider, 2 to 350 nits (run 201647). **Not exercised:** three failed pushes, a panic (`hk_mdp_restore()` is the same path as the return), system sleep (`syssleep=0` in these boots), HDR content on the pipes.

## 9. Metal on the pipes: what the panel is sent, and who may draw it (2026-11-05/06, Metal session, RAM boots, uncommitted)

**The owner's black triangles were frames being drawn while they were sent.** Observed: with the pipes on and Metal as compositor, a black triangle along a quad's diagonal in animations (the owner's photos, run 213407); absent with `mdp=0` (222000) and with `fb_hold_push=1` (223431). Observed: submissions write the surface a push is fetching: 730 named during a push by the end of 224034; hk-gl has 440 on the same script (freedreno marks everything read and write, so that number is not all writes; it shows no triangle, unexplained). Inferred, not seen: what the panel gets is the frame's opening fill, as far as it has reached memory. By the owner's eye the triangle is gone on every kind of wait below, one look each.

**Why CoreAnimation cycled two surfaces [source: QuartzCore of 20F66, disassembled; the option bits inferred from its callers].** A display has three pages. Before a frame `IOMFBDisplay::can_update_` asks `IOMobileFramebufferSwapWait(the page's swap, 0x80000000)`, not blocking, about the page it will use and the next. First done: it gives that page's buffer back and may draw. Second not done: it marks the frame triple buffered, and `allocate_buffer` makes a third surface. First not done: it does not draw. Busy is `kIOReturnBusy`, which CA compares against. Ours ignored the swap and the options, blocked, and said done, so CA always had two free pages. IOSurfaceIsInUse is not on this path. Answering honestly (`fb_wait` bit 4): three surfaces at once (run 231434, confirmed 232152).

**What is a swap's "done" here:** taken means its push has started (or it has been copied); a later swap taken means its push has ended; for the last one taken, `panelInFlight`. A first version went by `fMdpInUse`, which `mdpFrameGone` clears before it waits for the push: 485 writes caught by the guard in 231434, 0 once fixed (232152). So the guard works as a backstop.

**Pacing matters more than the number of surfaces.** The blocking SwapWait also had a use: it started each frame as the previous push started, a whole push period to draw it in. Non-blocking, CA runs on a clock of its own. Heavy script (`out/rc.jobs.heavy`), share of frame gaps over 18 ms, App Library / Today / page swipes; windows do not line up exactly and App Library takes in the first slide-in:
| `fb_wait` | run | App Library | Today | swipes | overlap |
|---|---|---|---|---|---|
| 0 | 004930 | 14.9% | 2.5% | 2.1% | yes |
| 13 | 004626 | 20.4% | 3.5% | 2.3% | 0 writes during a push |
| 13, later libraries | 005452 | 14.9% | 1.9% | 3.2% | 0 |
| 5 | 003927 (8 real touches) | 26.8% | 4.8% | 9.0% | 0 |
| 3 | 004231 | 28.2% | 8.4% | 5.1% | 0 |
Busy answers do not cost frames: 23 of 4301, and 0 of 5011, were for the oldest page, the only ones that stop CA. **Not measured: touch latency with the third surface.** The owner has not judged this default yet (row `metal-all2`). `fb_wait=3` was 普通 by the owner's eye, as were 1 and 5-like rows.

**hk-gl against Metal, heavy script (hk-gl run 010021 with `fb_wait=0`, Metal 005452):** GPU per frame 5.5 against 6.4 ms in App Library, 3.0 against 3.9 on swipes, measured as busy over frames. So not the factor of three an earlier comparison of different scenes suggested. Gaps over 18 ms: 10.8 / 1.7 / 1.9% against 14.9 / 1.9 / 3.2%. The clear difference: frames ready later on Metal (the presenter waits 4.4 against 2.0 ms in App Library, 1.9 against 0.6 on swipes). (An earlier version gave the Maps pinch as a second one, 182 frames on hk-gl against 76: **invalid**, first-run sheets covered the map in those runs. No valid hk-gl comparison of the pinch exists.) Cold shader translation and VideoToolbox's hold of the ring (183-261 ms) are one-off hitches these shares hide; hk-gl has no equal numbers for them. This does not yet explain the owner's 「opengl很快，沒metal這麼卡」.

**Passes:** App Library, 416 commits in 5 s: 142 draws, 91 passes, 40.5 fetching draws a commit. Nearly every blended draw reads its target in the shader, not only `_Idst`, and stands apart: about two passes each. A by-region barrier could remove most of them; the GPU gain is bounded by the row above, about a millisecond a frame, and joining fetches without a barrier broke the blur once (row metal-now).

**Maps' pinch (runs 013944, 014540, 015419, 020307; scripted, no fingers; kernel mdp-11824ed0).** The answer to 「地圖伸縮卡頓，你看看是不是是metal問題」 as far as it goes: yes, in texture making. Everything below is with no network, so the map is a blank grid: 3 draws in one pass a frame. **Nothing here was measured with real tiles.**
- *Observed:* VectorKit asks for 2 new textures a frame (020307: 425 reused and 7 made over 216 passes): 1125x2436 4x `BGRA8Unorm_sRGB`, 44 MB, and 4x `Depth32Float_Stencil8`, 55 MB, both storage mode 3 (memoryless) and render target only (020936). On Apple's GPU those have no memory. Here they do, and a real one takes 35-76 ms, the kernel's GEM allocation under `msm_bo_init` on Maps' main thread.
- *The fix tried:* IMAGES KEPT FOR THE NEXT TEXTURE in `air_vk.c`, images reused by shape. 014540, 96 MB, less than one set: 50 made, 2.3 s in a 5 s window. 256 MB: 6-9 made, 0.35-0.48 s a window.
- *Compositor, frames over 18 ms in the pinch:* 4.8% (020307) and 5.4% (015419), against 47.4% (013944, no pool) and 60.1% (014540). **A weak comparison:** Maps died in 015038 by SIGSEGV, my sampler reading frame pointers of threads still running (none after it stops them first), and the same is inferred for 013944 and 014540 (no `mark done`, no CPU lines); their windows differ. There is no clean baseline. Why a client's allocation would delay the compositor is a hypothesis: serialised kernel GEM work, not looked at.
- *CPU over 12 s (020307):* Maps' main thread 3224 ms (27%); then 447, 420, 376 (event fetch), 315, 288 ms. No separate render thread: display link, VectorKit, commit and texture making all run on main.
- *What main waits on (few samples):* of 14 waiting, 12 in the run loop with nothing to do, 2 in `-[CAMetalLayer nextDrawable]` (symbolised), 0 on a GPU fence; fence waits are on worker threads, the completion path. Of 22 running, 3 in `msm_bo_init`.
- *Maps' rate in motion (run 061838, 2026-11-06; kernel mdp-93fe105e, seeded, six scripted pinches, no network, blank grid):* map frames 142 in 5.07 s and 143 in 5.76 s, 28 and 25 a second, three commits each; compositor 66 and 57; half the gaps under 12 ms, longest 56 and 80; GPU busy 54% and 49%; done 7-8 ms after commit; textures 6 a window, about 51 ms each. Submissions waited for a push 40 times, 275 ms in about 11 s, longest 17 ms. Why near 28 is still not known; the earlier rates below had the script's rests in them.
- *Not settled (earlier):* Maps' rate in motion. 23-39 passes a second, with the script's rests averaged in, while the compositor does 52-59. There is no per-frame timing, so a cap is neither shown nor ruled out.
- *Also seen:* its layer has a pipe of its own (447 of 900 frames of two layers, three surfaces of Maps' in the swaps). That is why shots show it black. 99 writes waited for a push in 015419, 562 ms in all; the 40 logged are all Maps', median about 5 ms. Why CA gives such a surface back while it is on its way to the panel is not known.
- *Lead, not built:* one shared image per shape for memoryless targets, whose contents do not outlive a pass, on a GPU that runs submissions in order. It would remove the remaining allocations and most of the memory. Unproven.

**Limits of a shot.** `mdpShot` copies the held surface on the CPU with no wait for the GPU: a seam across a shot can be that copy racing a later draw (goal-alibs/home-4, a seam the owner did not see on the panel). Of two layers a shot has the composite alone. Touches of the owner's during a scripted boot show as `touch: finger` lines: check for them before reading its shots.

## Sources

Read for this page (GPL or public code as data; offsets and names only):

- Mainline DPU: [dpu_6_0_sm8250.h](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/catalog/dpu_6_0_sm8250.h),
  [dpu_hw_catalog.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_catalog.c),
  [dpu_hw_ctl.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_ctl.c),
  [dpu_hw_sspp.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_sspp.c),
  [dpu_hw_lm.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_lm.c),
  [dpu_hw_util.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_hw_util.c),
  [dpu_plane.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_plane.c),
  [dpu_crtc.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/disp/dpu1/dpu_crtc.c),
  [msm_mdss.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_mdss.c),
  [msm_kms.c](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/msm/msm_kms.c),
  [dispcc-sm8250.c](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/dispcc-sm8250.c).
- Mainline SMMU: [arm-smmu-qcom.c](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c),
  [arm-smmu.c](https://github.com/torvalds/linux/blob/master/drivers/iommu/arm/arm-smmu/arm-smmu.c),
  [Read back stream mappings](https://lkml.iu.edu/hypermail/linux/kernel/2010.2/02870.html),
  [Emulate bypass by using context banks](https://lkml.iu.edu/hypermail/linux/kernel/2009.0/05751.html),
  [Allow inheriting stream mapping from bootloader](https://lkml.iu.edu/hypermail/linux/kernel/1912.3/00963.html).
- Mainline device trees: [sm8250.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi),
  [sm8250-xiaomi-elish-common.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-common.dtsi).
- Lineage `android_kernel_oneplus_sm8250` (branch lineage-20):
  [kona-sde.dtsi](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-20/arch/arm64/boot/dts/vendor/qcom/kona-sde.dtsi),
  [msm-arm-smmu-kona.dtsi](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-20/arch/arm64/boot/dts/vendor/qcom/msm-arm-smmu-kona.dtsi),
  [arm-smmu.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-20/drivers/iommu/arm-smmu.c),
  [sde_kms.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-20/techpack/display/msm/sde/sde_kms.c),
  [sde_rm.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-20/techpack/display/msm/sde/sde_rm.c),
  [sde_hw_ctl.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-20/techpack/display/msm/sde/sde_hw_ctl.c).
- Hypervisor: [U-Boot qcom-hyp-smmu.c](https://github.com/u-boot/u-boot/blob/master/drivers/iommu/qcom-hyp-smmu.c),
  [Peripheral Image Loader for Linux host at EL2, v8](https://patchew.org/linux/20251121-kvm._5Frproc._5Fv8-v8-0-8e8e9fb0eca0@oss.qualcomm.com).

Seen only as search results (the page itself was blocked or not fetched), so weigh accordingly:

- [DSC outputs garbage over DSI on SM8[12]50 boards](https://gitlab.freedesktop.org/drm/msm/-/issues/24),
  [OnePlus 8T initial support, v2](https://lists.freedesktop.org/archives/dri-devel/2024-June/460451.html) (panel `samsung,amb655x`; a different board from this one),
  [postmarketOS SM8250 kernel](https://pkgs.postmarketos.org/package/main/postmarketos/aarch64/linux-postmarketos-qcom-sm8250),
  [vendor per-pipe SID setup](https://git.evolution-x.org/anierin/display-drivers/commit/bfec52ae7b8f5ce257558f71cbd51b45f0baab68),
  [Asahi DCP `dcp_swap`](https://github.com/AsahiLinux/linux/pull/288/files),
  [early-boot identity mapping RFC](https://lore.kernel.org/all/CAGETcx8YAXOdr1__gTCT2xCPq47Cg9vGj+5HJ_ZLzy4mHxU2xA@mail.gmail.com/).

No public document on QHEE's SM8250 SMMU trapping was found: the hypervisor's rules above come from those drivers' behaviour and this phone's own answers.
