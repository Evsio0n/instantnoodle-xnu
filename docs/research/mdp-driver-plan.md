# Research: our own MDP (DPU) driver for the OnePlus 8 -- findings and staged plan

**Status (2026-11-05): research and design only. Nothing here was run on a phone and no code exists.** Every later phone run
needs the owner's go-ahead and the phone lock (`./xl wait`). Read [hardware-qualcomm.md](hardware-qualcomm.md) first; the
present pipeline is in [../subsystems/display.md](../subsystems/display.md).

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
| The MDP's streams `0x820`/mask `0x402` match SMR4, whose S2CR translates through context bank 4 (at `0x15084000`: 128 global pages, then 4K per bank) | observed |
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
scaler `qseedv3lite`, and mainline's version `0x30000` takes the same "lite" path. Mainline leaves the LUTs unprogrammed and
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
  (stage value is stage + 1; VIG0 bits 0-2, VIG1 3-5, VIG2 6-8, VIG3 26-28, DMA0 18-20, DMA1 21-23, DMA2/3 in EXT2).
- `CTL_DSC_ACTIVE`, `CTL_MERGE_3D_ACTIVE`, `LM_OP_MODE`, `LM_OUT_SIZE`, the blend stages.
- Each pipe's QoS LUTs (`+0x60..0x6c`), `CDP` (`+0x134`), format (`+0x30..0x38`), `OUT_XY`.
- **The question it must answer:** is the firmware in **source split** (both pipes staged on both mixers, `OUT_XY` in the pair's
  coordinates) or one pipe per mixer? Every later coordinate depends on this. The tree declares `qcom,sde-has-src-split`.
- Newly read, announced first: `dispcc +0x20c8/+0x200c`, one VIG2 scaler register, `sid_phys`, VBIF. Each behind the
  `regs_alive()` pattern and its own sub-arg, in case one hangs.

Pass: the derived pipeline equals the expected one; otherwise everything after it refuses to run.

### (b) second layer

**`mdp=2`, solid fill, no fetch at all (1 boot).** Set the solid-fill bit in `SRC_FORMAT` and a colour in `SRC_CONSTANT_COLOR`: **the
pipe reads no memory, so no SMMU risk**. Target: DMA0 in the left half, 256 x 256, stage 1, opaque, for a few seconds over the live UI;
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
