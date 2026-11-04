# Research: Qualcomm hardware under a hypervisor — rules, interfaces, hard-won facts

This is the page to read **before touching a register**. The kernel runs at EL1 under Qualcomm's hypervisor, with
TrustZone and the always-on processor (AOP) owning parts of the SoC. An access the hypervisor does not like ends the
run at once or a quarter of a minute later, usually silently, and some leave the phone in **Qualcomm CrashDump mode**
(recovery: hold Power + Volume Up; on the OnePlus 8 the phone has only the stock bootloader and TWRP, so `xl reboot` lands
in the bootloader and `xl enter` goes from stock fastboot). Facts are tagged **[observed]** (phone), **[source]** (from
Linux/vendor kernel source read as data) or **[inferred]**. Device names: redwood = POCO X5 Pro (SM7325, "Kodiak"),
instantnoodle = OnePlus 8 (SM8250, "kona").

Related: [../architecture.md](../architecture.md), [boot-chain-and-recovery.md](boot-chain-and-recovery.md),
[../subsystems/](../subsystems/).

## 1. Things that crash the phone (do not repeat)

| Trigger | Result | Where found |
|---|---|---|
| Reading apps-SMMU **context bank 4** registers or its page tables (the display's MDP stream) | CrashDump. Only global SMMU registers (SCR0, IDR, SMR, S2CR) are safe; **SMMU ATOS is not supported** (the hypervisor hides it), so a mapping can only be found by trying | display work, 2026-10-23 [observed] |
| TrustZone `PAS_SHUTDOWN(13)` of the GPU zap shader before any `PAS_INIT_IMAGE` in that boot | CrashDump (cold `gpu=1`). Hand TZ the image header first (`step_zap_load`, commit `3e84e9c`) | [observed] |
| `touchscreen=1` on the OnePlus 8 (redwood's Goodix driver touches GPIO 68) | NoC error at `0x0f144000` CrashDump (2026-10-23) | [observed] |
| Reading a PCIe 0 register before its GDSC and clocks are on (QCA6390) | NoC error, CrashDump | [observed] |
| Reading an LPASS clock register from the application processor | external abort ends the kernel (the audio DSP owns that block) | [observed] |
| Voting the VA core clock `0x30b` or writing the VA GFMux `0x33a0000` (it reads 0 = TX MCLK) | NoC CrashDump at the TX macro `0x3220500` | microphone work [observed] |
| Starting the Wi-Fi/audio DSP when the previous one was shut down without having acknowledged a stop (`wifi_again=1`, `adsp_again=1`) | reset the phone **twice out of two** | [observed] |
| A register of a collapsed GPU block (CX/GX off) touched while asleep | hang | GPU collapse design [inferred/observed] |
| Sending DCS sleep-in (0x10) to the ANA6705 panel | DSC setup lost; coloured static until a phone reset | [observed] |
| `ml_io_map` of a large block (10 MB) | panic: the arm **IO submap (8 MB, permanent)** is exhausted. Map one page of TLMM, not 1 MB; map the second framebuffer with `IOMemoryDescriptor::withPhysicalAddress` | [observed] |
| A GPU run that fails | can leave CX stuck on; `fastboot reboot bootloader` (no flashing) recovers | [observed] |
| An external abort / hang with the loader's watchdog | loader resets the kernel after 50 s silence; the phone returns to the loader unless it is in CrashDump | [observed] |

## 2. SMEM, SMP2P, GLINK, QRTR, IPCC (the DSP conversation)
Used identically for the Wi-Fi DSP (WPSS) on redwood and the audio DSP (ADSP) on both phones.
- **SMEM:** about 2 MB every processor can reach, laid out by the boot loader; numbered items. On the OnePlus 8 no Android has
  run, so the DSP's items 478/479 are created by the driver.
- **SMP2P:** a pair of items between two processors with named 32-bit entries ("I am up", "I have crashed", "I have stopped",
  "master-kernel" bit 0 = please stop). ADSP is processor 2 (redwood items 443/429, last words 423); WPSS processor 13.
- **IPCC:** doorbells. Client 3 = LPASS (signal 0 GLINK, signal 2 SMP2P). Base `0x408000` (SM8250 and SC7280).
- **GLINK** channels: `IPCRTR` (QRTR transport), `apr_audio_svc`. **QRTR** on `IPCRTR`: hello both ways, then services the DSP
  announces. **APR** carries the audio services (ASM/ADM/AFE).
- **AOP mailbox:** the always-on processor (which runs RPMh) must be told "adsp: on" / "wpss: on" (five words of its message
  memory plus its doorbell) before it honours the DSP's own requests for power; "off" on stop. The Wi-Fi and audio drivers
  share this mailbox and the SMEM allocation lock, each behind a lock both take.
- **RPMh:** the DSP's LCX and LMX rails at level 384 while booting, given back when it asks for its own. Command DB at
  `0x80860000` (redwood default `board_cmd_db`), apps RSC at `0x18220000`.
- **TrustZone `PAS`:** images authenticate with `init_image` (header) then start; image ids: ADSP 1, WPSS 6, GPU zap shader 13.
  Reserved memory: ADSP 64 MB at `0x86700000` (redwood).

## 3. SMMU and DMA
- The apps SMMU gives each device a stream. Known streams: audio DSP `0x1801` (mask 0xf: stream's low 4 bits in address bits
  32..35), QCA6390 `0x1c01` mask 0 (not `0x1c00` mask 1), GPU CB0 (per-program TTBR0 via `CP_SMMU_TABLE_UPDATE`), MDP SMR4 /
  context bank 4 (hands off). Match registers and contexts are found free and taken by Wi-Fi and audio start under a shared lock
  (`hk_smmu_lock`).
- The hypervisor accepts the MP aperture `0x1b` (SCM) for the GPU.
- PCI coherent DMA memory aligned to a power of two at least its size, or the QCA6390 firmware crashes.

## 4. Display (SM8250)
DSI command-mode panel (ANA6705, 1080x2400, bl max `0x7ff`), CTL0 → VIG0/VIG1 (540 px halves, SRC0_ADDR `0x9c000000`,
stride 4320) → LM0/LM1 → dual DSC → INTF1; tear check on, autorefresh `0x80000001` left by firmware; PP0 done = MDP INTR bit 8,
MDSS SPI 83 (GIC 115); MDP streams `0x820/0x402` → apps SMMU SMR4 / CB4. Second framebuffer `0x9ca00000` works. Firmware pitch 4320,
GPU render targets need 64-byte-multiple rows so VIGs are re-pointed at pitch 4352. DCS packets through DSI0's command DMA
from a page between the framebuffers. 90 Hz = nine DCS long writes. Detail: [../subsystems/display.md](../subsystems/display.md).
Redwood: DSI **video** panel, 119.93 Hz, INTF 1 at `0x0ae36000`, dispcc `0x0af00000`.

## 5. GPU (A650 "kona" vs A660)
SM8250's GPU block is at the same addresses as SC7280's. Differences handled in `hk_adreno.c`: no A660-only `CP_CHICKEN_DBG` /
`QOS` writes (the cause of step 21 failing), clock-controller debug selectors, the SMMU stream the hypervisor accepts,
`a650_hwcg[]`, a650 `CP_PROTECT`. GPU interrupt GIC 332; sensors tsens0 15 / tsens1 8. Detail: [../subsystems/gpu.md](../subsystems/gpu.md).

## 6. Power domains and the PMIC arbiter
- **PMIC arbiter (SPMI), version 5.** A system's write channel for a peripheral is `0x10000` bytes of "chnls" per channel;
  reads go through "observer" channels. Transfers are serialised (`hk_spmi_lock`). OnePlus 8 slaves: PM8150 (0) and PM8150B (2: charger, USBIN
  `0x1310/0x1006`), PM8150L (flash LED), PM8150A (LDOs). Redwood: PMK8350 (0), PM7325 (1), PM8350C (2).
- **Power key wake path (OnePlus 8):** PON KPDPWR both edges → arbiter channel 13 `ACC_ENABLE` → PDC pin 1 level high → GIC SPI 481.
  Arbiter channel 33 (PM8150B USBIN `0x213`) arrives with ACC enabled by firmware and must be quieted.
- **Charger registers (PM8150B, Linux `smb5-reg.h`):** ICL `0x1370` (50 mA a step), `USBIN_MODE_CHG 0x1366`, command override `0x1342` bit 0,
  `ICL_OVERRIDE_AFTER_APSD 0x1365`; charge states 0 inhibit … 7 disable. PS_HOLD reset type: PM8150 PON slave 0 peripheral `0x08`,
  `PS_HOLD_RST_CTL 0x5a`, `..CTL2 0x5b` bit 7.
- **PSCI:** `CPU_ON`/`CPU_OFF` via SMC; `CPU_SUSPEND` power-down state `0x40000004` exists but is not used.
- **CX/GX GDSCs, PCIe GDSC `0x6b004`:** see GPU and Wi-Fi docs. RPMh needs the supply names from the command DB; the OnePlus
  RPMh names for the touch supplies are `ldoc1`/`ldoa13`, slider `ldoc10`.

## 7. Pins and buses, OnePlus 8 (TLMM south tile `0x0f500000`, pin registers `0x1000 * gpio`)
Touch I2C (SE13) gpio36/37, reset 38, report 39, supplies LDO PM8150A L1 / PM8150 L13; slider gpio4/5 (i2c@984000: QUP 0 SE 1,
function qup1; the code calls it "se5"); battery gauge (SE16) gpio48/49 `0x55`; speakers (SE15) gpio44/45, resets 91/99; PCIe: WLAN_EN 20,
PERST 79, CLKREQ 80 (func 1), WAKE 81; microphone codec reset gpio32 (driven as GPIO). I2C is bit-banged with clock stretching
(`instantnoodle_i2c.c`; TLMM CFG: [1:0] pull, [5:2] function, [8:6] drive strength in 2 mA steps, [9] output enable) because "the engine's
FIFO path is disabled (GPI DMA only)" -- **source:** that was measured on the OnePlus 7 Pro's (SM8150) touch engine in `ios-instantnoodle`,
**not on this phone**; **observed** here: QUP 0 SE 6 (Bluetooth UART) has `GENI_IF_DISABLE_RO` 0 (`logs/061256-run.log.live`). **Observed** 2026-10-27 (`024633`, `i2c_geni=1`): touch QUP 1 SE 5, gauge QUP 2 SE 2 and slider QUP 0 SE 1 all have protocol 3 (I2C
loaded), `if_disable 0` (FIFO there), 16-word TX FIFO, clocks in DFS with level 0 = 19.2 MHz; `i2c_geni=2` drives them (`024827`). Engines, GCC votes and SE interrupts (gcc-sm8250.c, the live tree): touch
QUP 1 SE 5 `0xa94000` (vote 0x52008 bits 18-21, 27; SPI 358), gauge QUP 2 SE 2 `0x888000` (0x52010 bits 0-3, 6; SPI 584, 100 kHz),
speakers QUP 2 SE 1 `0x884000` (0x52010 bits 0-3, 5; SPI 583), slider QUP 0 SE 1 `0x984000` (0x52008 bits 6-9, 11; SPI 602, 400 kHz).
Every engine register needs its clocks voted on first (an unclocked read is a NoC CrashDump).
Cameras (OnePlus 8, **[source]** live device tree, pinctrl-sm8250.c, camcc-sm8250.c, gcc-sm8250.c): CCI0 gpio101-104, CCI1 gpio105-108
(north tile, function 1 `cci_i2c`), MCLK0-3 gpio94-97 (north, function 1 `cam_mclk`), resets gpio145/78/90/144, supply enables gpio26/24
(south), 139/156/159 (west) and the PM8009's GPIO 1 (slave 10, `0xc000`). camcc `0x0ad00000`: MCLKn RCG `0x5000 + 0x1c*n`, branch
`+0x18`; titan top GDSC `0xc144` (state in `0xc148`: [16] up, [15] down, gdsc.c POLL_CFG_GDSCR), `cam_cc_gdsc_clk` `0xc16c` (critical);
GCC camera AHB `0xb008` (Linux turns it on at boot; **no camcc register may be read with it off** [inferred: unclocked access =
NoC error, as for PCIe/QUP]). Camera rails are RPMh resources: PM8009 `ldof1`-`ldof7` (pmic5-ldo), PM8150 `smpa5`, PM8150L `bobc1`;
**[observed, `cam=1` run 010516]** no channel of the PMIC arbiter is any of their peripherals (PM8009 LDOn `0xa40 + n - 1`,
S5 `0x020`, BOB `0x4a0` were looked for among all 512): RPMh drives them its own way and the AP cannot read them back -- RPMh's
completion is the only read-back. Also observed there: the PM8009 (slave 10) answers, REVID `00 00 00 03`; its GPIO 1 (`0xac0`) is
an MV GPIO (type `0x10`, subtype `0x11`, enabled, input) that system 0 may write; GCC's camera AHB, HF AXI, SF AXI and XO clocks
are all **off** under this firmware (Linux turns AHB on at boot); every camera pin is at its reset state (GPIO, input, pull-down;
gpio24 pull-up and high; gpio78 no pull) and gpio156/gpio159 read `0x801` -- bit 11 set, bit 12 clear: **eGPIOs left to the low-power island** (see below). The sensors' module files of this phone are in `odm_a:/lib64/camera/` (not `vendor_a`), "Parameter Parser
V2.0.0" (every field 64 bits wide; format notes in `tools/camsensor.py`). **[observed, `cam=2` run 044740]** RPMh takes every camera
vote (PM8009 L1-L5, L7; S5 2040 mV; BOB 3312 mV); GCC camera AHB switches on (`0x1`) and back off; camcc's GDSC clock is found **on**;
titan top GDSC comes up (GDSCR `0xf822f000`, CFG `0xf0000`) and collapses again (`0x22f001`, CFG `0x88000`); MCLKn at 19.2 MHz from
the crystal (RCG cfg `0x1`, branch `0x1`) and back off; the PM8009's GPIO 1 drives the IMX481's VANA switch. With these, at
bit-banged I2C on the CCI pins (GPIO, internal pull-up), IMX481 (CCI1 M1 0x1a), IMX471 (CCI1 M0 0x10) and GC2375 (CCI0 M1 0x17,
8-bit registers, at 19.2 MHz instead of its module's 24) read their chip IDs at the first try. The IMX586 (CCI0 M0 0x1a, same
supplies as Android's sequence, every pin register read back) did not answer. **[observed, run 080458, `cam_scan=1`]** a NACK,
SDA/SCL high before and after; on the bus 0x54 (the module file's EEPROM, `0xa8`) and 0x72/0x73 (its lens motor, `0xe4`) answer, the
OIS (LC898124, `0x7c`; its `cam_vaf` PM8009 L6 off) and the sensor do not. **[source, Linux pinctrl-sm8250.c since 2025-09,
b39e9199fbe1 + pinctrl-msm.c]** SM8250 gpio146-179 are **eGPIOs**: cfg bit 11 `egpio_present`, bit 12 `egpio_enable`; while bit 12
is clear the pin belongs to the low-power island and the TLMM's level and output enable do not reach the pad (its registers still
take and read back every write); `msm_pinmux_set_mux()` sets bit 12 when it muxes such a pin to function 0. gpio156 is the wide
sensor's CAM_VANA2 (the external 2.8 V regulator's enable from BOB), so its analog supply never came. **[observed, run 081247]**
with bit 12 set (and the found `0x801` put back at the end) every driven pad follows by its input bit, and the IMX586 answers
`0x0586` at the first read, the other three as before. Any eGPIO (146-179) the AP drives needs bit 12 first. **[observed, run 082733, `cam_pll2=1`]** camcc's PLL2 (Zonda,
`0xad02000`) under this firmware: not in FSM mode, off (mode 0, L 0, OPMODE 0), USER_CTL `0x8`, CONFIG_CTL `0x08200920` (as
Linux's `cam_cc_pll2_config`), CONFIG_CTL_U `0x05008001` (Linux writes `0x05002015`); with Linux's configure and enable steps it
locks within the first poll (mode `0xe0000007`: lock, active, frequency lock), TEST_CTL 0 (no CFA), and an MCLK RCG switches to it
(source 5, /10, M/N 1/6: 24 MHz) and back to the crystal while it runs. The GC2375 answers at 24 MHz as at 19.2. **[observed, runs 085220 / 085649, `cam=3` step 1]** camcc PLL0 (Lucid, Trion
registers) is off under this firmware (mode 0, user 0x1) and locks at once with `cam_cc_pll0_config` (mode `0xd8800005`); the RCGs
slow_ahb, camnoc_axi, cphy_rx, csi2phytimer, ife_lite_csid and ife_lite switch to it; camcc-sm8250.c's `cam_cc_core_ahb_clk`
(0xc150) is **not** a branch here (reads 0, keeps no enable); refgen (`0x88e7000`, PWRDWN_CTRL5 bit 0) is **off** (so the display's
DSI PHY does not need it lit). **[source, live DT]** PM8150 L9 (`ldoa9`, 1.2 V only) is not the camera's alone: the UFS PHY's
vdda-pll, PCIe 0/1/2 (the Wi-Fi's too), both USB SS PHYs' core, DP and both DSI controllers' vdda-1p2 and all six CSIPHYs take
it -- never switch it off, never vote it below its high-power mode (the storage and the screen would go with it). With cpas_ahb, slow_ahb and the blocks' own branches on: CSIPHY 2 revision (COMMON_STATUS 12-15 after
CTRL6 bit 1) `0x40010000`, CSID-lite 0 HW_VERSION `0x20000001` (generation 2). **[observed, run 090634, `cam=3 cam_step=2`]** Linux camss's SM8250 path as
data (VFE-lite global reset acked by IRQ_STATUS0 bit 17; CSIPHY reset, CTRL5 `0xd5`, CTRL6 1, CTRL7 2, `lane_regs_sm8250`, settle
23 for a 250 MHz link with the 300 MHz timer; CSID reset done by TOP bit 0 after RST_STROBES `0x1e`; CSI2_RX_CFG0 `0x232103`, CFG1
`0x41`) receives the IMX471's mode 1 cleanly: RX status `0x4003ff`, TOTAL_PKTS_RCVD (0x160, the vendor's register) 14016 in 300
ms, STATS_ECC 0, TOTAL_CRC_ERR 0, captured long packet DT 0x2b WC 2910, short packet frame start. The vendor's RX bit meanings
hold (bit 22 = VC/DT not mapped while no RDI path takes it). **[observed, runs 091514-094343, `cam=3` steps 3-5]** apps SMMU: SCR0
`0x2a0406`, IDR0 `0x4c017e57` (87 SMRs), IDR1 `0x6000004a` (74 contexts, 128 pages before them); with Wi-Fi off the boot chain uses
SMRs 0-6, none for the IFEs. **The hypervisor grants the IFE pairs 0x800/0x400 and 0x840/0x400 and refuses 0x801/0x400 and
0x841/0x400** (an SMR written with them reads 0; run 095528: refused again on another free SMR, 11 -- **the stream IDs themselves are not
granted**, not the slots; INFERRED: kept for the secure camera). A free context's CBAR written stage-1 (`0x1f300`) is kept when 0 is written back.
A stage-1 context with 4K tables of ours at IOVA 0x7400000 serves IFE-lite 0's write master (stream 0x800 or 0x840) with no fault;
FSR reads `0x400` (its format field) when clean. VFE-lite 0 keeps 4 bits of the bus CGC override (0x1a08) and 2 of WM 0's burst
limit (0x1c64). The front IMX471's mode-1 frame lands whole: 1748 lines of 2910 bytes at stride 2912.
Redwood: touch SPI SE13; haptics SE9 gpio36/37, reset 35; speakers SE3 gpio12/13; cameras CCI0 gpio69-72, CCI1 gpio73-76, MCLK gpio64-67.

## 8. Audio/LPASS
See [../subsystems/audio.md](../subsystems/audio.md): LPASS owned by the DSP, macros need hw votes and clocks before any
access; SoundWire master (swrm) frame 50 rows x 2 columns; WCD938x TX device ID `0x000d01170223`; codec reset gpio83 (redwood) /
gpio32 (OnePlus), LPASS pins 0,1,2,14.

## 9. Method for new blocks (what has worked)
1. Take addresses from the phone's **live device tree** (`ios-instantnoodle/research/instantnoodle-live.dts` for the OnePlus,
   `uefi/files/Resources/DTBs/redwood.dtb` for redwood) and register layouts from Linux / the vendor kernel, **read as data**.
2. Write a driver in **steps with a boot-arg per step** (`adreno=N`, `wifi_step=N`, `adsp_step=N`, `buttons_steps=N`, `cam=1..5`,
   `mic=1..3`): each step announces what it will touch before touching it, reads before writes, and a step that does not
   find what it needs ends the sequence. The loader, watching from another core (`bootcpu=N`), saves the tail of the log
   every ~2 s (`*_pause=MS` waits so it can), which is all there is after a silent reset.
3. Writes go through one checked function with read-back, and any surprise ends all writing.
4. Never power or write anything on a PMIC outside a short whitelist; the PMICs make the phone's supplies.
5. Put every phone-specific number behind `board=`/`board_<block>=0x...` (`bsd/sys/hk_board.h`; `tools/board-from-dt.py`).
