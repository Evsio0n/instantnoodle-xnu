# TODO — what HackiOS still needs implemented or completed

The development roadmap. It lists **work**; what is *broken for a user right now* is in [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (entries `K..`),
the session state is in [HANDOFF.md](HANDOFF.md). Entries here (`T..`) are cross-referenced, not duplicated. Evidence for every item is the
source tree and the subsystem pages; items built from source comments cite the file.

*Last audited: 2026-10-26 against the working tree (both git trees dirty, see HANDOFF).* Target device is the **OnePlus 8** unless an item
says otherwise; the owner asked not to run redwood checks.

**Status:** Not Started · Partial (some of it works) · In Progress (someone is actively changing it) · Blocked (cannot proceed without something named) ·
Built, Unverified (compiles/QEMU-tested, never seen on the phone) · Complete (listed only where an older document made it look unfinished).
**Priority:** **P0** safety, or without it the phone is not a usable device · **P1** a major missing iOS capability or a verification debt on shipped work ·
**P2** quality, efficiency, convenience · **P3** research or maintenance.
Order inside each section is by priority. Update an item when you change its state; when finished mark Complete and keep the diagnostic findings.

**The owner's order (2026-11-04): the camera first (T07: the three back cameras, zero-copy, the installed bundle, the owner's own
check), then the rest (T38, items 1-13), last TrustZone (T15: FairPlay, the Secure Enclave) and the Titan 480 IFE's hardware
ISP ("roadmap修相機 ->其他 -> 最后trustzone和Titan 480 IFE").**

## Index

| ID | Item | Status | Pri |
|---|---|---|---|
| T01 | Verify the unverified power work on the phone | Built, Unverified | P0 |
| T02 | Wi-Fi radio through system sleep | Built, Unverified | P0 |
| T03 | Install the current build / keep installed bundle current | Partial | P1 |
| T04 | Motion, light and proximity sensors (screen rotation, auto-brightness) | Not Started | P1 |
| T05 | Bluetooth | In Progress | P1 |
| T06 | Cellular modem, telephony, SMS | Not Started | P1 |
| T07 | OnePlus 8 cameras | Partial | P1 |
| T08 | Headphones / USB-C audio | Not Started | P1 |
| T09 | Fingerprint / biometric authentication | Not Started | P1 |
| T10 | Metal: from experimental to usable | In Progress | P1 |
| T11 | Heterogeneous CPU scheduling (Edge) | Partial (working, uncommitted) | P1 |
| T12 | Sleep power: SoC low-power states | Blocked | P1 |
| T13 | Battery drain measurement | Not Started | P1 |
| T14 | Location (GPS) | Not Started | P2 |
| T15 | App Store, Apple ID, FairPlay, push (Secure Enclave dependent) | Blocked | P2 |
| T16 | Brightness: prove the light changes | Partial | P2 |
| T17 | CPU time after wake (widgets, chronod) | Partial | P2 |
| T18 | Wi-Fi: 802.11r, TKIP pairwise, roaming, other SAE groups, persistence | Partial | P2 |
| T19 | Fast charging (Warp / PD) | Not Started | P2 |
| T20 | CPU idle power collapse | Not Started | P2 |
| T21 | GPU: IFPC and measured power | Blocked | P2 |
| T22 | CoreAnimation fidelity gaps (sb3, poster switcher, CC glyph opacity, switcher wallpaper) | Partial | P2 |
| T23 | Display: late swaps, cold-start jank | Partial | P2 |
| T24 | Speaker protection (TFADSP) and other microphones | Partial | P2 |
| T25 | Video and Live Photo recording; redwood-only camera parity | Not Started | P2 |
| T26 | Storage: writable APFS, fsck, HFS write speed | Partial | P2 |
| T27 | Boot menu usability (5 s countdown, graphics) | Partial | P2 |
| T28 | Security model: stand-ins protect nothing | Partial | P2 |
| T29 | Remove stand-ins and copies (`hk_wlfw.c`, duplicated DSP code) | Partial | P3 |
| T30 | Redwood re-verification and parity with OnePlus work | Not Started | P3 |
| T31 | Documentation and repository debts | Partial | P3 |
| T32 | Scripted gesture reach, test coverage | Partial | P3 |
| T33 | Thermal service never seen throttling | Built, Unverified | P3 |
| T34 | Sensors that are not sensors: NFC, UWB, Taptic edge cases | Not Started | P3 |
| T35 | Complete (do not reopen): items that older text made look open | Complete | — |
| T36 | iphont agent hardware validation and workflow migration | Partial / Built, Unverified | P1 |
| T38 | The owner's list for after the camera (Wi-Fi list, sound/vibration, wallpaper, Bold Text, clock, screenshots, smoothness, sensors, GPS) | Not Started (after T07) | P1 |

---


## P0

### T01 Verify the unverified power work on the phone — Power · Built, Unverified · P0
- **Works:** sleep/wake with timer and power key, panel off/on, governor, touch reset, audio park, GPU collapse (verified; [power.md](subsystems/power.md)).
- **Missing evidence:** shared-interrupt fix in a long session; 3-strikes push fallback; volume-key interrupts and the key thread's dark wait; slider at 300 ms dark;
  USB reconnect fix (`abort_xfer`); charge-state numbering; charger input limit (2000 mA DCP / 1500 CDP) and its save/restore; **off-mode charging** (loader);
  menu entries "Recovery"/"Bootloader"/"Power off" and the cable-out wait; **empty-battery shutdown**; first sound after an unpark.
- **Why P0:** these paths write charger registers and decide whether a flat phone shuts down safely.
- **Files:** `bsd/dev/hk_power.c`, `hk_buttons.c`, `instantnoodle_battery.c`, `xnuloader/flashboot.c`, `loader.c`.
- **Next:** run one `tools/t power` level for the sleep parts; with the owner present: plug a wall charger and read `charger` diagnostics, try off-mode charging,
  `battery_empty_mv=N` above the cell voltage to test the shutdown. Record in power.md and HANDOFF.
- **Refs:** [power.md](subsystems/power.md) "Verification record"; K08.

### T02 Wi-Fi radio through system sleep — Wi-Fi/Power · Built, Unverified · P0
- **Works:** radio up, scan, join, DHCP, Safari; `hk_power_will_sleep()` takes the radio down (network left, firmware stopped, link off) and a thread brings it back.
- **Missing:** a real sleep with the radio on and a network joined, then a wake and rejoin; firmware crash recovery by reset.
- **Files:** `bsd/dev/hk_wifi.c` (wifi-power thread), `instantnoodle_wifi.c`, `hk_power.c`.
- **Blockers:** needs RAM run with `wifi=1` + `sleep_max`; `/private/var` RAM-only so the password must be in `fw/network`.
- **Next:** `tools/t`-style run: join, `power_test sleep 5 2`, check `en0` and Settings.
- **Refs:** [wifi.md](subsystems/wifi.md); K05.

## P1

### T03 Keep the installed bundle current — Boot/Installed · Partial · P1
- **State:** the installed bundle and `boot_b` loader were updated 2026-10-25 14:20 (power rounds 1-4). Later work (GPU collapse, brightness, `ca_ambient`, vmd DENY,
  Wi-Fi) is not installed. The latest userland images were built for tests, not the installed recipe.
- **Next:** `common/flash/install-xl.sh instantnoodle update` after T01 evidence; **requires the owner's explicit OK** (writes the flash). Rebuild with `FLASH_USERLAND_VARS`.
- **Refs:** [INSTALL.md](INSTALL.md), research/boot-chain-and-recovery.md.

### T04 Motion, light and proximity sensors — Sensors · Not Started · P1
- **Why:** no accelerometer means no screen rotation, no CoreMotion, no auto-brightness (and CoreBrightness's 5 s ALS wait stays).
- **Facts:** sensors hang off the audio DSP behind its sensor framework ("SEE") via QMI service 400 over QRTR ([thermal.md](subsystems/thermal.md) has the step table:
  DSP started / GLINK / QRTR / services listed = done (`adsp=1`, `hk_adsp.c`; `sysctl kern.hackios.adsp_services`); QRTR ports of our own on the DSP channel, the service locator and notifier
  the DSP's sensor domain waits for, the sensor registry the DSP reads from Android's partitions, a SEE client, delivery as HID events for CoreMotion = **to do**).
- **Files:** `bsd/dev/hk_adsp.c`, `hk_qmi.c`, `sys/hk_qrtr.h`; new driver; iOS side: a virtual HID accelerometer service in `inject.c` (pattern: the touch service).
- **Blockers:** sensor registry (Android partitions: `persist`/`vendor`) unavailable without Android; OnePlus 8 firmware needs checking.
- **Next:** run `adsp=1` and read `kern.hackios.adsp_services` for service 400 (the previous plan's first question; result not recorded).
- **Refs:** K07.

### T05 Bluetooth — Connectivity · In Progress · P1
- **Verified on phone:** QCA6390 UART, firmware/NVM and HCI; native CoreBluetooth service-filtered discovery before/after sleep;
  initial native BLE connection and 22-byte GATT write/readback. Later supervised `221539` verifies CMF Buds Pro 2
  classic pairing, authentication/encryption and AAC A2DP channel negotiation through native Settings. Playback unverified.
- **Fixed:** optional unsupported init step, Broadcom capabilities/multi-advertising, and mixed legacy/extended LE commands.
  HCI capability masks and replies remain truthful. Malformed NVM rejection is host-tested; latest kernel also brings up the controller on the phone.
- **Files:** `instantnoodle_bt.c`, `hk_bt.c`, `HKBluetooth.cpp`; userland `btboot.c`, `inject_bt.c`, `bt_scan.m`; `tools/bt/check.py`, `tools/t`.
- **Audio gap:** original BTAudioHALPlugin.driver was excluded/hidden by HKAudio-only overlay; selection/packaging fixed and built.
  Corrected image routes the quiet tone to A2DP and enters STREAMING (`002629`); heard output unconfirmed.
  Voice Memos selects HFP, encounters format error, and audio server restarts; exact fatal stack still needed.
  No-buds `003355` records two seconds through the built-in mic and opens Voice Memos without a captured exception.
- **Owner step:** supervised Settings pairing succeeded. Quiet test ran with approval; next required action is re-pairing/tapping Record under exception tracing.
  Earlier CLI post-wake MITM failure (4815/705) is distinct from the now-verified native classic pairing flow.
- **Remaining:** BLE pairing-session validation, general foreground BLE app discovery,
  reliable reconnect, audio, bond persistence, coexistence. Audio needs a headset and an approved audible test.
- **Refs:** [subsystems/bluetooth.md](subsystems/bluetooth.md), [research/bluetooth-ios.md](research/bluetooth-ios.md), K05.
  Opt-in `bt=1` / `BLUETOOTH=1`; nothing flashed. `213836` run: 10/11 (post-wake GATT failed); host parsers/guards PASS.

### T06 Cellular modem, telephony and SMS — Connectivity · Not Started · P1
- **Facts:** the modem (MSS) is not started (`qcom,mss@4080000`); CommCenter (`-noncellular` variant) answers but dies on missing `AppleIPAppender` (SIGSEGV) and
  `AppleBasebandPCI` (SIGABRT) — stubbing one leaves the other ([research/ios-userland.md](research/ios-userland.md) §4). `vmd` denied.
- **Next:** start the modem (PAS), QMI services, then IOKit drivers for the baseband path; scope is research-heavy.
- **Refs:** K06.

### T07 OnePlus 8 cameras — Camera · Partial · P1
- **State (2026-11-04):** `instantnoodle_camera.c` has `cam=1` (look) and `cam=2` (each sensor powered up as Android does, chip ID read,
  powered down) for all four sensors; `hk_camera.c` hands `cam=N` to it on this board. `tools/camsensor.py` reads the OnePlus modules'
  "Parameter Parser V2.0.0"; `userland/build.sh` makes `camera-<sensor>.tab` from `fw-camera/com.qti.sensormodule.<maker>_<sensor>.bin`
  (taken from the phone's `odm_a`). RAM runs: `../oneplus8-audit/camera-native/run_cam_probe.py LEVEL [SENSOR [scan]]` (kernel
  BUILD_CAM, `out/camera-probe-root.apfs`). **Verified on phone:** `cam=1` (run 010516) and `cam=2` (run 081247, 08:12, kernel SHA
  4ee4b55d...): **all four give the chip IDs Android expects** -- wide IMX586 `0x0586`, ultra IMX481 `0x0481`, front IMX471 `0x0471`,
  macro GC2375 `0x2375` (at 19.2 MHz) -- every sensor powered down and restored, normal return. The wide one had been silent (runs
  044740, 080458) because its CAM_VANA2 enable, gpio156, is an **eGPIO** (SM8250 gpio146-179; cfg bit 11 = eGPIO, bit 12 = the TLMM
  owns it rather than the low-power island): it read `0x801` and the driver kept bit 12 clear, so the register took every write and the
  pad never moved. `line_set()` now sets bit 12 as Linux's `msm_pinmux_set_mux()` does (the found register gives it back at the end)
  and reads every pad back by its input. `cam_scan=1` (run 080458) had shown the bus fine (both lines high, a NACK) with the module's
  EEPROM (0x54) and lens motor (0x72/0x73) answering and the OIS (0x3e) not -- PM8009 L6 (the OIS's `cam_vaf`) was not needed.
  `cam_pll2=1` (run 082733, 08:27, kernel SHA c2726e82..., verified on phone): camcc's PLL2 at 1440 MHz as Linux starts it, the
  macro's MCLK at the 24 MHz its module file asks for, its ID read there; PLL2 and the RCG put back as found.
- **Frame path, step 3 (3a-3c) verified on phone: the front IMX471's first frame in memory (run 094343, 09:43, kernel SHA
  49e104f9...).** `cam_step=3` (run 091514, reads only): apps SMMU on, unmatched streams fault, 87 match registers / 74 contexts,
  registers 0-6 valid, none covering the IFEs' streams; GIC SPIs 500/501 (CSID-lite 0, IFE-lite 0) disabled. `cam_step=4` (runs
  092248, 092704): under `hk_smmu_lock`, free match registers 7-10 and context 6 (off), stage-1 translate with our tables mapping
  only the frame's pages at IOVA 0x7400000; **the hypervisor grants Android's IFE pairs 0x800/0x400 and 0x840/0x400 and refuses
  0x801/0x400 and 0x841/0x400** (read back 0: given back at once); everything back (but CBAR: its found 0 is not taken back, it
  keeps 0x1f300 -- context off, harmless). `cam_step=5`: one frame through CSID-lite 0 RDI 0 (payload only, DT 0x2b) and IFE-lite
  0 write master 0 (MIPI raw, stride 2912, FRAME_INCR 0, one frame in 32, stopped at the first buffer done): bus status 0x11,
  RDI SOF+EOF, 1752 packets, 0 ECC/CRC, no SMMU fault, guard pages and canary intact; handed to the loader, `xl camera` makes
  `logs/cam3c-094343-0.png` (a recognisable room, correct geometry, overexposed: no exposure control yet; it shows a person, so it
  stays in `logs/`). VFE-lite keeps 4 bits of the bus CGC override and 2 of the burst limit (runs 093614, 093955 stopped there;
  now told, not checked). **Exposure (run 094952):** the frame of 094343 was dark, not bright -- raw 63..89 of 1023 over a
  black of 64 (`camraw.py` stretches it white: it takes no black level off); the mode's own exposure is 2676 of 2685 lines at gain 0.
  `cam_gain=0x380` (8x analog, SMIA 0x0204 inside hold on/off) gives raw median 130, max 352, no clipping, a clean picture
  (`logs/cam3c-gain-black64.png`, rendered with the black taken off). Next: `cam=5` (iOS, Camera.app; redwood's `ios_session()`),
  with an exposure loop of its own (redwood's `exposure_limits()/exposure_write()`).
- **`cam=5` verified on phone (run 101751, 10:17, kernel SHA 8af6cfd4...): iOS reads the front camera.** `instantnoodle_camera.c`
  serves `/dev/hk-camera` itself (redwood's protocol, `hk_camera.c` untouched): `stream_mode()/stream_up()/stream_down()` (step 5's
  steps, split; mock K/L/P/Q/R unchanged), 3 places each at its own IOVA with an unmapped guard page after it, the write master's
  address set at each RDI 0 SOF + reg update, a place READY at each bus buffer done (1 ms polling, GIC SPIs off), reads never given a
  place the write master may take, `ios_stop()` waiting for reads before the memory goes, the DMA stopped first. `camera_test raw 60`
  (image `out/camera-ios-root.apfs`, its own rc.jobs): 60 of 60 frames, its AE at 33 ms / ~11.7x / 30 fps, a 1164x874 NV12 picture
  kept for the host (`logs/cam5-ios.png`, well exposed and balanced). mediaserverd's capture stack (libhackios-media) streamed the
  front camera in the same run (AWB ~5450 K); its Back Camera asked for sensor 1 (no frame path: ENODEV). Every session stopped
  clean: 0 written over, guard pages intact, no SMMU fault, no panic. Fixed after it: the first exposure (0xffff lines stretched the
  first frames to 0.8 s). Mock 19/19 (S: a client reads 5 frames, each the frame its header says, keeps a picture, closes).
- **Camera.app verified on phone (runs 104112, 105351, 10:41/10:53, kernel 9fce7834 unchanged; userland only).** Preview,
  tap (focus square), a 4032x3024 HEIC photograph, Photo/Video/Portrait modes, no mediaserverd death in 105351. Image
  `out/camera-app-root.apfs` = camera probe root + `out/cam5-media/libhackios-media.dylib` + `userland/rc.jobs.camera-modes`
  (rwtouch opens Camera.app and taps the mode bar); `run_cam_probe.py 5 '' exctrace launchd` with `CAM_IMAGE=...` (new flags:
  `exctrace=1`, `log_launchd=1`). First runs 102330/103046/103420: the preview stayed black because mediaserverd died 10-14 frames
  into every session -- `-[BWMultiFilterThumbnailNode prepareForCurrentConfigurationToBecomeLive]` writing through a nil Metal
  filter (fixed in `inject_media.c`: the node made ready without its filter when there is no Metal device); then 104112 died
  (SIGBUS) at each mode change -- the graph's unregister sets a non-block `PixelBufferHandler` that `camera_device.c` kept and
  called (fixed: only blocks, by isa, `Block_copy`/`Block_release`; 104931 was the intermediate build that died in `_Block_copy`).
  Details: research/ios-userland.md §3.
- **The preview upright and on the GPU (runs 110059-112756, 11:00-11:30, kernel unchanged; userland only).** Upside down until
  110059: the front sensor is mounted the other way round from the back ones (DT cam-sensor@2 roll 270, @0/@3 roll 90), so the
  OnePlus 8's Back Camera is `turned` as its front one (text on the wall read correctly after it; not mirrored). The processor's
  renderer cost 90-140 ms of CPU a 1504x1128 picture (111039: 8 pieces, 5.3 at once, the A55s' the longest) because at 1x the
  IMX471's 1164x874 cells are fewer than the picture's pixels and every pixel took the magnified path's 16 RAW10 unpacks; with
  each line of cells unpacked once (`camera_isp.c` `cell_lines`, a column table, a cache per thread; host-checked against the
  old code: at most 1 apart, 3.7-4.3x faster, the 1x nearest path unchanged) 111605 had 90-100 ms and the thermal service
  never past a short `heavy` -- where the OnePlus 8 now also halves the moving pictures to 15 a second (back at 30 when it
  cools: seen in 111605). Then, at the owner's word ("隔壁怎麼還沒有轉gpu或者isp處理？"), **the GPU ISP** (`userland/src/
  camera_gpu.c`): OpenGL ES 3 through the image's Mesa (dlopen of `/hackios/lib/libOSMesa.8.dylib` in mediaserverd, FD650), the
  RAW10 up as R8UI, cells unpacked into RGBA16F, the picture packed into RGBA8UI (Y four to a texel, CbCr under it) with
  `isp_preview()`'s crop and matrix, the same gamma table and nv12_pair()'s integers, read back with the planes' strides; the
  photograph and anything with lens shading stay on the processor; any failure or a picture over 50 ms gives the GPU up for the
  process (said why). Checked on the Mac first (the same file on macOS OpenGL 4.1 through a shim: 9 configurations at most 1
  apart from `isp_render_nv12`). **Run 112756 (`run_cam_probe.py 5 '' exctrace launchd gpuiso`: gpu_pt=1 gpu_recover=1),
  verified on phone:** the GPU's first picture of each run 0.000 apart on average, 1 at most, from the processor's (1504x1128
  and 1920x1080); 12.4-12.7 ms a picture on one thread, mostly waiting (was 90-140 ms of CPU); 1800 of 1800 frames read (30 a
  second); no throttling ("slowed 0 times", 111605 had 5), the hottest core 81-93 C, the big cores at row 11/19 on average; the
  window server unharmed (90 Hz, frames 12 ms apart, the longest 12-19, 0 pauses; ~210 GPU submissions a second, ~1.7 ms each);
  no death. Run images: `out/camera-app-root.apfs` with `userland/rc.jobs.camera` or `rc.jobs.camera-modes`.
- **Not a fault: the "bands" in preview screenshots** (110059, 112756) are `xl screenshot`'s own: it reads the framebuffer over
  0.7 s while the kernel draws on (display.md); the owner saw none on the panel, and `campattern=1` (moving grey stripes from
  the GPU instead of the picture, run 113902) cut the same way with every pool surface given out again only after 7 others
  (113459). Kept as tools: `campattern=1`, the pool's surface lines every 300 frames.
- **The GPU ISP accepted (runs 113459-121138, 11:35-12:12).** Five minutes of Camera.app's preview (120049, `run_cam_probe.py 5 ''
  launchd gpuiso long`: 420 s): 30 frames/s throughout (7200 frames in 4 min, none written over), the hottest core 58-67 C a
  minute, nominal, slowed 0 times. The kernel's "2 cores" in `kern.hackios.top` with the camera on (kernel_task 39.5 s in 20 s,
  114535; under 2.2 s on the home screen, 114902) is mostly the idle threads' account: kprof (115633) put the samples in
  `machine_idle` and at `ml_set_interrupts_enabled+0x50` (where a core woken from WFI takes its interrupt); the kernel's real
  work is ~0.7 core (pmap_page_protect, the memory compressor `WKdm_compress` -- some memory pressure --, bzero, the read's
  copyout). `gpu_irq=1` changed nothing (115246); the camera's 1 ms wait sleeps (cpu_idle_latency is 0 on this port, so
  `delay_for_interval` blocks). The home screen's widgets take ~1.2 cores of their own (Calendar, Weather, Stocks: T17). The GPU
  is given up only after 5 pictures in a row over 50 ms, a picture that made its textures anew not counted (one of 97 ms at a
  camera switch had given it up for the rest of 120049). mediaserverd died 4 times of "Output can't be nil" (SIGABRT,
  `-[FigCaptureStillImageSinkPipeline _buildStillImageSinkPipeline...]` -> `-[BWGraph connectOutput:toInput:pipelineStage:]`)
  when the owner turned the camera front and back in Video mode in 120049; a scripted Video/front/back/Photo/front/back
  (121138) did not repeat it (every still sink pipeline got its port's StillImage output), and `inject_media.c` now refuses a
  connection with no output (says the input's node, returns nil: the pipeline fails, not the process) as a guard.
- **The wide IMX586's C-PHY packets verified on phone (run 122836, 12:28, kernel SHA 78f42d20..., RAM only):** `cam=3
  cam_sensor=wide cam_step=2` (`run_cam_probe.py 3 wide step2`): CSIPHY 0 (0xac6a000; camcc csi0phytimer RCG 0x6000, branches
  0x6018/0x601c, as found and back) on three C-PHY trios with redwood's v1.2.1 settings ported (`phy_cphy_up()`: lanes enabled
  0x2a, rate row 0 for 1.93 Gbit/s a trio, settle 14, every write read back), CSID-lite 0's RX `0x1002102` (3 trios, lanes 0x210,
  PHY 0, C-PHY); the sensor's init (1709 writes) and res1 (4000x3000, 100): **35936 packets in 300 ms, ECC 0, CRC 0, no
  overflow, the last long packet DT 0x2b of 5000 bytes** (4000 RAW10 pixels), SOT/EOT on all three trios (RX status 0x400377;
  bit 22, no RDI for the VC/DT yet, as the front's step 2); everything back, normal return. Mock 20/20 (W: the wide's step 2).
  **Step 5 verified (run 124047, 12:45, kernel SHA cd7ff773...):** one 4000x3000 frame into memory (stride 5008, 3668 pages in
  one piece, only those mapped, guard pages untouched, RDI 0 SOF/EOF, no overflow, no SMMU fault) and to the host:
  `logs/cam3w-124047-0.png`, the desk with a Mac mini, correct geometry and RGGB order, pale (no black level off). Mock 22/22.
  **Camera.app's Back Camera is the IMX586 (run 124917, 12:49, kernel SHA 8a5f0df7..., verified on phone):** cam=5 serves
  both sensors -- HK_CAMERA_SENSOR 1 or 2 ends a session of the other (its DMA first) and the next is the asked one's
  (`ios_sensor_wanted`, `ios_switch`), the frames' header saying whose (sensor 1/2, RGGB/BGGR); the ring three 15 MB places
  page by page (kmem_alloc, only the SMMU makes them whole, a guard page after each). Userland: the OnePlus 8's Back Camera
  back to sensor 1, not `turned` (cam-sensor@0 roll 90, as the iPhone's back camera). The preview: the desk and a Mac mini,
  upright, natural colours (`logs/cam5-wide1-2.png`: shows a hand, logs/ only); front <-> back switches both ways; the GPU's
  first picture 0 apart from the processor's; 20.6 ms a picture (the 15 MB upload), statistics 7.3 ms; 55.8-57 C a minute, no
  throttling (a 40 s preview); no death. Mock 23/23 (Z: a client switched front -> wide). Not yet: the 2x crop (mode 6), the
  lens motor (fixed at its rest), a five-minute heat check, mediaserverd's footprint with 15 MB frames.
  **Rendering, decided (the supervisor for the owner, "Metal、OpenGL 還是 ISP 渲染，哪樣效率高 ... 這個交給你了"):** OpenGL ES stays
  (Metal here is hkmetal over the same freedreno and GPU: a layer more, T10 experimental); next for it zero-copy (the ring's
  memory as a GPU buffer, the picture drawn into the IOSurface) once the IMX586's 15 MB frames show their cost; the Titan 480
  IFE's full pipeline (demosaic, colour, gamma, scaler straight to NV12 -- no CPU or GPU on the pixels) is the most efficient
  but needs CamX/chromatix reverse engineering and tuning (weeks): long term, after the back cameras and the installed bundle.
  **13:10-14:30, verified on phone, RAM only (kernel BUILD_CAM 14:29 SHA 8a30e271..., exported; userland/tools uncommitted):**
  the zoom button 1x <-> 2x (`inject_camera.c` in Camera: the back camera's toggle sets 2x through CAMZoomControl; the app's
  own second tap is 1x; run 133140); the 2x preview live (the frames' ZoomFactor metadata the whole zoom, mode 6's 2x with it:
  `mode_zoom()`); a mode change in the session (`ios_mode_change()`, `cam_quickmode` default on: 265-315 ms, was 2.7 s; run
  133840); IMX586 autofocus (LC898217XC, `cam_focus` default 2: contrast AF sharp, parked before its power goes and on the way
  out by hand; runs 135326, 140301); the three cameras' colour tuning (`tools/camtuning.py` reads the V2.0.0 tuned files:
  cc13, lsc40, the AWB's ten reference lights; the GPU does the lens shading too, mode 6's window the sensor's middle; the
  owner: the desk's cable is "正紅色", as the tuned picture has it; run 140301); 2x by the light (mode 6 only when exposure ms x
  gain is low, else mode 1's middle magnified: mode 6 in a dim room blotched magenta/green; run 140909); the ultra-wide IMX481
  (CSIPHY 3 and its clocks csi3phytimer 0x6060/0x6078, csiphy3 0x607c; mode 0 4656x3496, its whole field -- modes 1 and 2 are
  its middle; step 2 runs 141639, step 5 run 142526 with a 24 MB piece taken at boot; cam=5 run 142958: camera_test, 3 x 20.4
  MB ring, 0 written over, AE to 16x/66 ms in the dim room, the picture right but soft -- its AK7374 lens motor is not driven).
  **15:00-15:10, the 0.5x, in progress (runs 150029, 150652; RAM only):** camera_device.c's third stream (PortTypeBackSuperWide,
  sensor 3); inject_media.c adds a Back Ultra Wide Camera entry (the Back Camera's, deviceType 7, built-in_video:5) to
  AVCaptureSession.plist as CMCapture reads it -- both ways it does: +[NSDictionary dictionaryWithContentsOfURL:] and
  +[FigCaptureFlatPlist flatPlistWithContentsOfURL:], whose FigCaptureFlatPlistDict root an NSMutableDictionary of ours stands
  in for (only that call's use of it is shown to work: the sources made, no crash); CaptureSourceInfo forgotten only where it
  lacks the ultra-wide; inject_camera.c's capabilities after init (ultra-wide yes; telephoto, dual, wide-dual, triple no) and a
  2x zoom point. Verified: the app's AVCaptureDevice has the "Back Ultra Wide Camera"; once (run 150029) the app used it and
  the kernel streamed sensor 3. Open: the zoom control is hidden altogether in Photo since the capabilities changed (under
  investigation); Portrait is still offered at the back (its use not tried). **mediaserverd SIGSEGV (run 150029):**
  -[BWImageQueueSinkNode _collectUnconsumedBuffersWithReason:] passes a NULL _imageQueue to CAImageQueueConsumeUnconsumedInRange
  (from -handleDroppedSample:forInput:, around Portrait -> Photo); guarded now (nothing collected without a queue), but why
  the queue is NULL there -- a sink torn down or not yet given its layer while samples still arrive -- is not known: a
  stopgap, the cause to be found.
  **15:10-15:33 (runs 151310-152922), verified on phone:** the 0.5x on by default (`cam_05x=0` off): ".5 1x 2" once
  -[CAMCaptureCapabilities zoomControlSupported] says yes (it is dual || wide-dual || triple || the front's ultra-wide, worked
  out at every call); the IMX481 at 0.5x upright (not `turned`: its init writes 0x0101 0); its AK7374 focusing and parked.
  Pink highlights fixed (white balance apart, each channel clipped at WHITE x min(lsc x wb) before the colour matrix, CPU and
  GPU; the owner: "粉色我自己測了，修復了已經"). CCI writes merged (`cam_cci_burst` default on): 981 ms to the Back Camera's
  first frame (2896), 489 ms to 0.5x (616), 1084 ms back to 1x (2220). **Startup time, next (the owner: "現在切換相機/開啟
  相機app要等3-4s相機才work"):** lazy power-down (the sensor kept initialised in standby for a while: no init on a reopen),
  then a faster bit-bang for the camera buses (~37 us a bit now), AE/AF from the last values. Noted: mediaserverd opens every
  stream once at its start (the ultra-wide too: one short session); the 0.5x's barrel distortion is not corrected (does the
  tuning file have its coefficients?); Camera's own blur after a change lasts a while.
  **Deferred to the end, with the DSP / Titan 480 work (the owner, 15:3x: "我覺得速度優化放在最後和dsp優化在一起吧，比較有意義"):**
  (a) lazy power-down -- written and built, not run: boot-arg `cam_keep=N` (default 0), instantnoodle_camera.c ios_stop()/
  ios_start()/ios_release_kept(): a session ended by its reader keeps its sensor in standby (power, MCLK, clocks, init table,
  lens, ring, SMMU tables; the DMA stopped and VFE-lite reset as ever), the next one of the same sensor and frame skips power-up,
  chip ID, lens start and the init table (and the res table for the same mode), released after N idle seconds, on another
  sensor or frame, at 95 C / heavy thermal pressure, on a fault, and on the way out (power_down() as ever). Before its first
  run the supervisor's conditions: the idle power-down and a session start serialised (both on the camera thread; check the
  lock under a racing open), a timeout that needs no userland (a kernel msleep with a timeout: yes), the thermal release, the
  lens parked and the AK7374 asleep on every power-down, every step logged with its time; tests: reopen within N s ("skipped"),
  wait past N s (the full power-down, then a normal open), the run's end while kept (the full power-down in order), 1x -> 0.5x
  (CameraUI reopens the wide one first: "skipped"). (b) A faster bit-bang for the camera's buses only (now ~37 us a bit:
  line()/scl_high() to be measured; instantnoodle_i2c.c has another session's changes -- a per-bus field only). (c) The Titan
  480 CCI engine (400 kHz / 1 MHz, queued). (d) AE/AWB/AF starting from each sensor's last values. Kept: `cam_cci_burst`
  (default on, verified).
  **Known, noted:** Portrait is still in the mode list at the back but there is no depth (no dual camera); the 0.5x's barrel
  distortion is not corrected (does camera-imx481.isp's source have the coefficients?); Camera's own blurred transition after a
  change lasts a second or more after the first frame (what it waits for: not found yet); mediaserverd opens each stream once at
  its start (the ultra-wide too: a one-frame session at boot).
  **Installed bundle (gen 1793779669, boot slot 1, 2026-11-04 16:2x): no Camera icon after the owner's unplugged boot -- fixed 16:4x.**
  *Observed* (the loader's log of that boot): slot 1, gen 1793779669 (SHA checked), "mount the data volume on
  /private/var (kept from earlier boots)", the camera driver serving `/dev/hk-camera` -- so the iOS side. Every camera test had a new ramfs `/private/var`
  (ufs_write=0: no `/dev/rhk_data`, run 155842 "mount a memory filesystem on /private/var"), so MobileGestalt answered
  from the device tree each boot. The installed boot keeps the HFS `/var` of the camera-less stable bundle, with its
  MobileGestalt cache (`com.apple.MobileGestalt.plist`, trusted while the build is 20F66) and its LaunchServices and
  SpringBoard state. That cache said "no camera" (*verified*: renaming it brought the icon). Ruled out: the installed loader (`loader-installed.efi`
  has the cam=5 / `camera-rear` code), and the cmdline (350 bytes, ends with cam=5 cam_after=10). Owner-side check: the
  App Library or Spotlight, and Settings' Camera entry. **Fix (built, host-tested; the owner's OK):** `init.c`
  `camera_answers()`: a stamp `/private/var/.hackios-camera` ("isp=N flash=N", none = isp=0 flash=0); when this boot's
  cam=5/torch= differ, MobileGestalt's cache (`containers/Shared/SystemGroup/*/Library/Caches/com.apple.MobileGestalt.plist`,
  the legacy `mobile/Library/Caches/` one) is renamed to `.hk-old` before iOS starts. `var_data_format=1` does not reset
  a volume that is there (newfs only without an HFS header). Image `out/camera-mgfix-root.apfs` (camera-install-root with
  only `/sbin/init` changed; the unchanged source rebuilds the installed init byte for byte); run once with
  `run_cam_probe.py 5 '' wifi gpuiso varwrite` (ufs_write=1 against the kept /var): **verified on phone, run 163941** (the
  8099-byte cache renamed, stamp isp=1; the owner: "有相机了"), but Camera then would not open on that kept /var (its
  session taken down ~0.1 s after it started, Camera exiting without a trap; cause not found) -- so /var was made anew
  (`var_data_format=2`, init.c, run 164952, the owner's OK): Camera opens and streams (110 frames).
  **The installed slot 1 alone, unplugged: Camera works** (2026-11-04 ~17:00, the owner: "已安裝版，Camera 正常"; the camera
  goal accepted). The next install carries `out/camera-mgfix-root.apfs` (cf335620) when there is a reason to install again. The installed bundle's init has no
  stamp code: booting slot 0 (stable) again would bring back a "no camera" cache, and slot 1 would not undo it until a
  bundle with this init is installed (next install: `out/camera-mgfix-root.apfs`). LaunchServices: not needed.
  **Next:** the 0.5x in Camera.app (mediaserverd's device list with a BackSuperWide stream, the least change: no MobileGestalt,
  no model identity); then the IMX481's AK7374 (as the IMX586's: read only first, the range, park, bus recovery); the installed
  bundle (the owner's approval). **Optional, noted:** the stream off at the ask (a frame less in a mode change); a faster CCI
  for the res tables (155 ms of the 265); chroma noise reduction (the CCM's ~1.9 raises the colour noise in the dark); userland
  AF_FAR 768 (redwood's) leaves the IMX586's codes 710-930 (the nearest) unused; zero-copy and AE/AWB statistics on a small
  image or the GPU (mediaserverd ~1.1 cores with 15 MB frames; the IMX481's are 20 MB).
- **Next: the back cameras (the owner, 12:2x: "接下來修後攝像頭吧。我們有三個而iPhonex只有兩個好像，有辦法修嗎"; the order agreed with
  the supervisor).** (1) The wide IMX586 (cam-sensor@0, CSIPHY 0, 3 C-PHY trios -- mainline camss has no C-PHY: the CSIPHY's
  3-phase settings and CSID's C-PHY RX to be found, then the step-2 test of MIPI packets first) as the Back Camera at 1x, its
  module's binned 4000x3000 mode at 30 frames/s (15 MB a frame: the ring and the SMMU tables grow); (2) the iPhone X's 2x from
  the same sensor's centre crop (module mode 6, 4000x3000 at 2000,1500: a real 2x, not digital) -- Portrait wants two cameras
  streaming at once, which one sensor cannot: Portrait stays as it is, noted; (3) the ultra-wide IMX481 (cam-sensor@1, CSIPHY 3,
  4 D-PHY lanes: the front's path again), and then a 0.5x in Camera.app (an iPhone X has none: CameraUI's and mediaserverd's
  device lists hooked for a BackSuperWide, not the model's identity); (4) the macro GC2375 (2 MP, no iOS device type): not
  planned. Redwood unchanged throughout.
- **Open, in order:** the back cameras (above: the wide IMX586 is the Back Camera since run 124917; next its 2x crop, then the IMX481); later optimisations: the IFE interrupt instead of 1 ms polling, the 5 MB
  upload zero-copy, the memory pressure (WKdm); the remaining heat (the kernel's 1 ms polling of the IFE -- an interrupt instead, the supervisor's condition 3 -- and two 5 MB
  copies a frame: the read and the GPU upload); a back camera of its own (the IMX481 on CSIPHY 3 D-PHY, then the IMX586's 3
  C-PHY trios on CSIPHY 0) -- the Back Camera shows the front sensor's view until then; an IMX471 tuning (`tools/camtuning.py`
  from its module's chromatix, if there is one); the photograph on the GPU too (4032x3024, 3.4 s to HEIC now); video recording and
  Live Photo (untried); `cam=5` on the installed bundle (the owner's approval, the supervisor's conditions: the same kernel and
  image verified in RAM first, slot 0 only, read back, SHA told before).
- **`cam=5` plan (2026-11-04, from reading redwood's path):** redwood serves `/dev/hk-camera` (`sys/hk_camera.h`:
  `read()` = `struct hk_camera_frame` "RWFRAME1" + packed RAW10; `write()` = EXPOSURE/KEEP/SENSOR/FOCUS commands) from a 3-place
  ring polled every 1 ms (`hk_camera.c` `serve_ios()`/`ios_session()`/`camdev_*`, the node made before `cam_after`); userland does
  all the ISP (`userland/src/camera_isp.c`) inside mediaserverd (`libhackios-media.dylib`, `inject_media.c` -> `camera_device.c`),
  and the loader's `/arm-io/isp` node (cam=5) brings the Camera icon. For the OnePlus 8, **without touching redwood's code**:
  `instantnoodle_camera.c` serves the same node and protocol itself -- the ring mapped through the SMMU context (all places, the
  write master every frame: drop period 0, FRAME_INCR 0, the address set per frame + reg update at SOF or done), SOF = CSID RDI0
  IRQ bit 12, done = VFE bus IRQ bit 4, polled (GIC SPIs stay off), exposure from userland as linear gain converted to Sony's code,
  health = the context's FSR + RDI overflow. Userland: `camera_device.c`'s `cameras[]` per board (OnePlus 8: the IMX471 is index 2,
  "front" as on redwood; the back stream would ask for index 1, the IMX481, which has no frame path yet), `load_tuning` for the
  IMX471 (none built: defaults), `ios_select`'s 4:3 test (2328x1748 is not exactly 4:3). Then the IMX481/IMX586 paths (CSIPHY 3 /
  0, C-PHY for the IMX586) for a back camera. **The supervisor's conditions for `cam=5` (2026-11-04):** (1) on the way back to
  the loader or on a panic, the DMA stopped first -- stream off, write master off, VFE reset, its idle -- then the SMMU, then the
  memory, without a lock or a sleep (done for step 5: `ife_dma_stop()` first in `power_down()`, run 100059, mock R); (2) each ring
  buffer only at its own IOVA with guards, never handed to iOS while the write master holds it (upstream 1800069's ring-reader /
  freed-ring guard to port); (3) an IRQ instead of polling only with its handler attached before GIC SPI 500/501 are enabled, the
  SPI off before the mask at quiesce; (4) Camera.app closed or in the background: streaming off and the power down (sensor, L3,
  L7, MCLK, PLL0); temperature watched when long; (5) the first iOS runs short (XL_DONE + a bounded XL_SECS), Camera.app opened and
  closed once each, before long ones.
- **Frame path, step 2 verified on phone (run 090634, 09:06, kernel SHA 4d9f8ad5...): the front IMX471's MIPI data reaches
  CSID-lite 0 intact.** Mode 1 (2328x1748, RAW10, 4 D-PHY lanes, 250 MHz link, settle 23) through CSIPHY 2: RX status `0x4003ff`
  (SOT/EOT on all four lanes, long and short packets; bit 22 VC/DT not mapped -- no RDI yet), 14016 packets in 300 ms, 0 ECC,
  0 CRC; the last long packet DT 0x2b with 2910 bytes (= 2328 x 10 / 8), the last short a frame start (VC 0, frame 1). Stream
  off, CSIPHY 2 reset, CSID-lite/VFE-lite registers back, everything off. `run_cam_probe.py 3 '' step2`.
- **Frame path, step 1 verified on phone (run 085649, 08:56, kernel SHA 9a04b470...):** PLL0 locked at 1200 MHz (found off), the
  6 RCGs and 11 branches on, refgen found **off** (switched on, back off at the end), L9 1.2 V voted (left on); CSIPHY 2
  revision `0x40010000`, CSID-lite 0 version `0x20000001` (generation 2) -- the first camera-block accesses, no hang; all back as
  found. camcc-sm8250.c's core_ahb (`0xc150`) is not a branch here (reads 0, keeps no enable: run 085220 stopped there; Linux's
  camss uses none for SM8250): dropped.
- **Frame path plan (2026-11-04, from the live DT + mainline camcc-sm8250.c / clk-alpha-pll.c):** `cam=3` with
  `cam_step=N`, the front IMX471 first (D-PHY, 4 lanes, CSIPHY 2 at `0xac6e000`), into **CSID-lite 0** (`0xacd9200`, inside
  IFE-lite 0 at `0xacd9000`; only the titan top GDSC). Step 1: camcc PLL0 (Lucid = Trion registers: L 0x04, CAL_L 0x08, USER_CTL
  0x0c, CONFIG_CTL 0x18/0x1c/0x20, OPMODE 0x38, ALPHA 0x40; `cam_cc_pll0_config` L 0x3e alpha 0x8000 = 1200 MHz, even /2 = 600,
  odd /3 = 400; used as found if the bootloader left it running, as `clk_trion_pll_configure()`), the RCGs slow_ahb `0x7058`
  (even /7.5 = 80 MHz), camnoc_axi `0xc0f8` (odd = 400), cphy_rx `0xa068` (odd = 400), csi2phytimer `0x6040` (even /2 = 300),
  ife_lite_csid `0xc01c` (odd = 400), ife_lite `0xc000` (odd = 400); branches cpas_ahb `0xc0f4`, core_ahb `0xc150`, camnoc_axi
  `0xc114`, csiphy2 `0x605c`, csi2phytimer `0x6058`, ife_lite_ahb `0xc040`, ife_lite_axi `0xc044`, ife_lite `0xc018`,
  ife_lite_cphy_rx `0xc03c`, ife_lite_csid `0xc034`; GCC camera HF/SF AXI; supplies PM8150 L9 1.2 V (`mipi-csi-vdd`, shared with
  the DSI side: voted, never switched off) and refgen (`0x88e7000`, proxy-enabled by Linux, likely on: read first); then **read
  only** CSIPHY 2's and CSID-lite's version registers -- a camera-block access without its clocks hangs the bus, so this
  step alone proves the clock tree. Step 2: CSIPHY 2 lanes (Linux `camss-csiphy-3ph-1-0.c`'s SM8250 table, settle from the
  module file), CSID-lite RX (4 lanes, PHY 2, D-PHY) + RDI 0, the sensor's init + mode + stream on, then CSID's RX interrupt
  status / counters read: packets arriving, no memory written. Step 3: IFE-lite 0's RDI write master into memory (SMMU IFE
  stream, camnoc, AXI votes), as redwood's `capture_sensor()`, in three runs (the supervisor's conditions, 2026-11-04):
  **3a** reads only -- the apps-SMMU's SCR0/IDR/SMR/S2CR (never context bank 4, the display's, nor its page tables: a CrashDump;
  no ATOS under the hypervisor), the IFE-lite's stream ID and mask from the live DT (redwood's 0x800 / 0x4e0 need not hold), the
  free SMR and context banks, and whether GICD_ISENABLER has the CSID-lite / VFE-lite SPIs (0x1d4, 0x1d5) enabled (a latched IRQ
  with no handler would storm); **3b** under `hk_smmu_lock`, a free SMR + S2CR set to **translate** (never bypass) and a free
  context bank whose page table maps only the camera's buffer, nothing streamed; **3c** one frame written: a physically contiguous
  kernel buffer, a guard page with a canary before and after, the write master's size/stride/buffer exactly inside it, one frame
  only, the own context bank's FSR read; teardown in the order stream off -> write master idle -> VFE reset -> SMR/S2CR/CB back as
  found + TLB invalidate -> buffer freed. The owner is told in this session before 3b and 3c (a CrashDump needs Power+VolUp).
- **Next:** the frame path —
  CSIPHY v1.2.1 (D-PHY for IMX471/IMX481/GC2375, C-PHY for IMX586), CSID 480 / IFE-lite 480 (Linux `camss-csid-gen2.c`,
  `camss-vfe-480.c` as data; redwood's are Titan 165's), camnoc/AXI clocks and votes, an SMMU stream; then `cam=5`
  (redwood's iOS side, `camera_device.c` formats are iPhone X names) with the front IMX471 first (D-PHY, smallest init).
- **Files:** `xnu/files/bsd/dev/instantnoodle_camera.c`, `hk_camera.c` (dispatch), `instantnoodle_i2c.c` (`instantnoodle_i2c_read_at`),
  `tools/camsensor.py`, `userland/build.sh`, `userland/src/camera_*.c` (later).
- **Refs:** K03; [input-peripherals.md](subsystems/input-peripherals.md).

### T08 Headphones / USB-C audio, earpiece — Audio · Not Started · P1
- **Facts:** USB-C analog headset: FSA4480 switch (on the speakers' I2C bus, deliberately not addressed), WCD938x RX path, MBHC detection. TFA9874 "receiver" profile exists for the earpiece.
- **Next:** MBHC/FSA4480 detection, RX path in `hk_mic.c`-style DSP bring-up; route change in `HKAudio.driver`.
- **Refs:** [audio.md](subsystems/audio.md); K04.

### T09 Fingerprint / biometric authentication — Security · Not Started · P1
- **Facts:** `biometrickitd` and `pearld` are denied; no driver; `MiniAppleKeyStore` models a device with no passcode. The phone has an under-display
  fingerprint sensor per its product specification; its interface has not been investigated in this repository. Real Touch ID semantics (Secure Enclave, `AppleBiometricSensor`)
  cannot be reproduced; a stand-in would have to satisfy `biometrickitd`/LocalAuthentication without real security.
- **Next:** identify the sensor and its bus from the live device tree; decide whether a *functional* (unlock only) emulation is wanted. Design discussion first.
- **Refs:** K02; research/kernel-standins.md (AppleKeyStore).

### T10 Metal: from experimental to usable — GPU/Graphics · In Progress · P1
- **Works:** see [gpu.md](subsystems/gpu.md) "Metal" (blits, MSAA resolve, fences/events/shared events in-process, heaps, 263/271 system fragment shaders draw, Apple Maps pipelines build).
- **Missing:** whole-system Metal is off by default (chronod widgets: 2 pipelines refused); no third-party Metal app tried; no picture compared for Maps; not implemented:
  mesh/object shaders, tessellation, ICBs, tile shading/imageblocks, function tables, indirect draw/dispatch, multiple viewports, depth-clip clamp, vertex amplification, counters,
  texture views, wide lines, per-binding LOD clamps, wrapping caller memory, heap suballocation, cross-process shared events. 135 `air.visible` modules are library functions, not
  entry points. Siri orb abort history.
- **Files:** `userland/src/hkmetal.m`, `hkdrawable.m`, `tools/mesa/air/*`, `docs/air.md`.
- **Next:** make `metal=2` the default only after: chronod pipelines; a Metal app test (e.g. a triangle app exists: `triangle_app.m`); compare Maps visually. Record numbers in air-report.
- **Refs:** K01.

### T11 Heterogeneous CPU scheduling — CPU · Partial (working, uncommitted) · P1
- **Done (2026-10-26, verified on the phone in RAM runs and in QEMU):** Edge scheduler on three clusters (prime, gold, silver), boot-time choice
  `sched=edge|clutch`, performance controller `bsd/dev/hk_perfctl.c` (group placement, deadline/interactive policy, migration graph, clock
  floors, energy model), BG-stays-on-E guard, cross-core priority inheritance, `sched_check`, `kern.hackios.amp`, `sched_test`, `tools/sched-test/`.
  Details and numbers: [scheduler.md](subsystems/scheduler.md). Exported to `xnu/`; **not committed**.
- **Open:** trace the rare 10-11 ms render frame under 7 busy DF threads; measure app launch time (Clutch vs Edge); try the render-server
  inheritance path (never raised by iOS here); redwood's `board_cpu_dpc*` values and a redwood run (only when the owner asks);
  thermal interplay; validate the energy model against a real power measurement (needs battery-only runs, T13).
- **Refs:** [scheduler.md](subsystems/scheduler.md), [cpu.md](subsystems/cpu.md).

### T12 Sleep power: SoC low-power states — Power · Blocked · P1
- **Blocker (architectural):** UEFI never exits; loader core 0 and resident UEFI drivers (storage, USB) hold votes; vdd-min needs the boot core and core 0 power-collapsed,
  RPMh sleep/wake sets. Boot core power-collapse additionally needs a PSCI resume path.
- **Next options:** (a) give storage/USB to the kernel (large); (b) accept WFI-based sleep; decide with measured drain (T13).
- **Refs:** [power.md](subsystems/power.md).

### T13 Battery drain measurement — Power · Not Started · P1
- **Facts:** no number exists for drain asleep or idle awake; `bq27541` current reads exist; runs 6's log covered only the plugged stretch; the owner said stop drain testing on 2026-10-25.
- **Next:** with the owner's consent: a timed sleep with the cable out, read gauge before/after through `power_test status`/`battery_test`. Needed to judge T12, T20, T21.

### T38 The owner's list for after the camera — Mixed · Not Started (after T07) · P1
- **(1) Wi-Fi list / 5 GHz delay / status-bar bars and (16) the clock: fixed in RAM 2026-11-04** (scan cache, wifi_dwell=1, event 39, sb_clock=28; HANDOFF). Not installed: the owner, 18:3x, "不用停和寫ufs" -- the bundle `out/instantnoodle-wifi-bars.img` waits.
- **(3) the wallpaper: after (12) Metal** (the owner, 2026-11-04: "那就把它放在metal後面再做"). Found so far: iOS 16's Settings -> Wallpaper ->
  Add New opens PosterBoard's gallery/editor, whose remote context our window server does not draw (the poster switcher's
  black, research/ios-userland.md); hypothesis (unverified): PosterBoard renders its posters with Metal (CAMetalLayer) in its
  own process, so an empty context while whole-system Metal is off. Fallback idea: an image straight into the wallpaper store
  (the pre-16 `SBSUIWallpaperSetImages`/`SBSUIWallpaperSetImageAsWallpaperForLocations`; the Apple Wiki lists the latter up
  to 16.0 -- check in 20F66). UI automation note: without cam=5 the home screen has no Camera icon and Settings sits at
  (0.613, 0.71); with it at (0.847, 0.72). In Settings (run 190357, screenshots): "Display & Brightness" at (0.4, 0.90) on its
  first page; after one swipe (0.5 0.75 -> 0.5 0.35) "Accessibility" at (0.4, 0.156) and "Wallpaper" at (0.4, 0.207).
- **(6) the clock ~2 s behind: fixed in RAM 2026-11-04** (run 191643, screenshots + timed's os_log): Settings had no "Set
  Automatically" because timed's `TMAutomaticTimeEnabled` was never set (Setup Assistant does it; we skip it). With
  `/private/var/db/timed/com.apple.timed.plist` (TMAutomaticTimeEnabled = TMAutomaticTimeOnlyEnabled = true, owner _timed 266)
  the switch is there and on, and timed fetched NTP over Wi-Fi: "Setting system time ... from TMTimeSynthesizer", settimeofday
  adjust +16.5 s, then ntp_adjtime. Done in the tree: userland/var/db/timed/com.apple.timed.plist (the skeleton; build.sh: db/timed/*
  owned by 266), init.c timed_prefs() (a kept volume gets it once if missing). Time Zone spins (no Location Services): cosmetic.
  Left: the loader's whole seconds (wait for the PMIC clock's next second, xl settime in ms) for the time before the network.
- **(5) Bold Text hangs: does not any more** -- owner-verified in RAM run 190737 (kernel ee290d6a, the installed root):
  "是，粗體不卡了". What changed is not established (no SpringBoard relaunch or GPU hang seen in that log). The bundle it hung on
  (1793719359) ran `gpu_pt=0 gpu_recover=0`; the installed camera bundle 1793779669 and the RAM runs have both on (GPU hang
  recovery) -- the likeliest difference, unverified. Web: iOS relaunches SpringBoard for Bold Text.
- **(2) no sound / no vibration: fixed in RAM 2026-11-04** (the owner: "聲音正常", run 185457; HANDOFF: args, fw-audio, hk_adsp_fw_file, IO_MAP_SIZE 64 MB). Not installed.
- **The kernel command line is near the loader's limit:** BOOT_LINE_LENGTH 608 (Apple's boot_args), the loader's prefix 187 B; the
  installed set with sound and items 1/16 is 584 B, RAM runs 604. Next: board defaults in the kernel for board=instantnoodle
  (gpu_irq=0, gpu_dcvs=0, gpu_sleep=0, gpu_hwcg=0, syssleep=0, buttons_voldown_pullup, board_gpu_zap...) or shorter names.
- **(4) no Camera app: fixed** (the owner, 2026-11-04: "4. 解決了，fix"; T07, the MobileGestalt cache and /var made anew).
- **(17) the screen's HDR, with (11)** (the owner: "和11 一起做吧"): the panel's device tree says hdr-enabled, ~430 nits peak, 8-bit,
  bl-max 2047, an HBM command (0x53=0xe0, 0x51=0x0f60); eagl.c:3017 ignores CoreAnimation's EDR (the supervisor's notes,
  outputs/相機完成後待修.txt #17). Not started.
- **(7) screenshots: work in RAM 2026-11-04** -- by rwtouch's `chord volup lock` and by the physical keys (the owner, run
  192832: "有截圖"), and saved on an HFS+ /var (run 192253, `hk_data=2`, a PNG in /private/var/mobile/Media/DCIM/100APPLE).
  Not explained for the installed boot; that /var was made anew at 16:49 (T07), which may be what fixed it. Recheck on the
  installed boot at the next install. rwtouch now has `chord NAME NAME [MS]` (userland/src/rwtouch.c).
- **(18) the sound's delay: quick fix done in RAM 2026-11-04** (the owner: "聲音延遲比較高", "先試快速改法"; then "晚点和dsp
  处理"). Measured (runs 193817-195427, hk_sound.c's new lines: each sound after silence with its time and the ring ahead of
  it, underflows as they come, the ring's longest wait): a keyboard click was heard ~0.76 s after iOS wrote it -- the parked
  ring had filled with the silence iOS writes from the moment Search opens its keyboard (32768 frames, 682 ms) and that
  backlog never drained (both sides 48 kHz) -- plus 8 x 20 ms at the DSP. Now: 8 x 10 ms at the DSP (80 ms); silence not kept
  while parked, the first sound clears what is older; silence over 43 ms at the ring's front dropped (sound never); the
  amplifiers off after 30 s of silence, not 3; after a park 60 ms of silence lead the sound (the amplifiers come up 54-69 ms
  after the write). Result: 0-10 ms in the ring, 0 underflows, no HAL complaints, a 2 s tone and the boot sound whole;
  touch to iOS's write 95-128 ms (iOS's own), then 80 ms. Left for the DSP group at the end: the ADSP's low-latency path
  (ULL ASM) and iOS's ~0.1 s from the touch to the HAL's write. Not installed.
  **DSP round, 2026-11-05 (the owner: "晚点和dsp 处理"; stopped for MDP, RAM, amplifiers down, numbers from logs only, nothing heard or measured at the speaker):** **a correction: the "95-128 ms touch to write" above includes 55 ms of the test.** rwtouch takes its mark's time, prints the line to the console, then posts, constant (touch_lat=1, run 181527). Split: kernel's post to the window server's hand-over 0.1-0.8 ms; hand-over to iOS's write 39-60 ms, median 45; so that part is iOS's own and nothing on our side stands in it. A real finger's scan and I2C read come before the post and are not measured (interrupt-driven at priority 92). Baseline run 180858, 8 x 10 ms: ring 0-9 ms, DSP 80 ms, 0 underflows. Run 181527, `sound_periods=4` (40 ms): 0 underflows, but the ring went to 19-29 ms, so 40 ms less queue gave about 20 ms; post to the queue's end about 108 ms against about 130. From a park, 60 ms of lead silence more (UNPARK_LEAD_MS, now counted over more than one filling). The default is still 8 x 10 ms: nothing shorter has been listened to. **Next:** `sound_periods=2`; silence kept in the ring, now up to 43 ms (RING_WAIT_MOST; make it an argument, try about 11 ms); iOS's IO size (the HAL logs a 512-frame request, the plug-in's Latency and SafetyOffset are 0); then ASM's low-latency and ultra-low-latency session flags and ADM's 0x2000 and 0x4000 ([Linux q6asm's enum](https://kunit.googlesource.com/linux/+/03535f4b32f2a11ba6c5bb07290995ecd77c69ac/sound/soc/qcom/qdsp6/q6asm.c), [msm's flags](https://gitlab.e.foundation/e/devices/android_kernel_fairphone_fp5/-/commit/42c2b29a1992e5d2518a33d3f4d4633f6ef07cc4)), not tried. A hardware reference: a short tone played and heard back through the microphone (mic=4), evening only, one run; waits for the supervisor. **Camera speed (item 14) not started:** nothing measured in this round; record stands at 981 ms to open, 3-4 s from the owner.
  **mediaserverd kills itself 23.2-23.45 s after it starts, sometimes:** launchd says `exited due to SIGKILL | sent by mediaserverd[N]`, in 180530 and 180858 of today's four, and in the Metal boots 151633-170709 (with camera and sound off in those, so neither is the cause); not in 180414 and 181527. Nothing of ours sends it. Its own log (`medialog`, run 180858) goes quiet 1.2 s after its start but for one line at 9.4 s, CVAPortraitVideoPipeline's "MTLCreateSystemDefaultDevice() is nil", which the second instance logs as well and lives, and Metal boots have a device and die too. Why is not known. Respawned at once and done by about 29 s of uptime, so numbers taken after 30 s are not disturbed. `mediastacks` (ustacks=mediaserverd ustacks_at=20) printed nothing: its lines go to the kernel's own log unless `ustacks_tname=*` is given too.
- **(11) 45 Hz: Safari's own cap, measured in RAM 2026-11-04** (run 200057, the owner: "還是45"): with TestUFO open in Safari
  the compositor drew 44-47 frames a second (225 every 5 s) and TestUFO's strips read 23 and 11 fps (45/2, 45/4); the
  system's own gestures were 84-88 fps (home-screen pages), 81 (Control Center), 55-65 (Settings' list); the panel's TE is
  11111 us (90 Hz). Cause (web + 20F66's dyld cache): WebKit's PreferPageRenderingUpdatesNear60FPSEnabled (on by default)
  renders pages near 60, and iOS picks a rate dividing the panel's 90: 45. **Proven in RAM run 201209:** with
  WebKitExperimentalPreferPageRenderingUpdatesNear60FPSEnabled (and the legacy WebKitPreferPageRenderingUpdatesNear60FPSEnabled)
  false through cfprefsd (`userland/src/prefs_set.c`, in mobile's global domain and com.apple.mobilesafari) TestUFO read
  68/34/17 fps and the owner saw "間歇性60-90fps" -- not steady: TestUFO's box red (stutter), the compositor's driver time
  3-6.6 ms and GPU waits 1-4.5 ms a frame on Safari's page (our GL path; with Metal later, item 8/12). Not a default yet:
  the owner chooses (steady 45, or 60-90 uneven). Settings > Safari does not open here (19), so the switch would be set at
  boot (prefs_set in rc.jobs, or init). The Settings list at 55-65 fps against the home screen's 84-88: per-app cost, to
  look at again after Metal.
- **(17) HDR: research done 2026-11-04** (read-only, 20F66's dyld cache; inferred where it says so): QuartzCore lists the
  iPhone X's A11 (0x8015) among chips without a hardware EDR factor, so CoreAnimation scales SDR down itself on the GPU
  (its Metal function `edr_scale`, our function 61, skipped in eagl.c: right while the factor is 1). EDR is only offered
  when CoreBrightness (in backboardd) sees hardware max nits above the user's: it reads `backlight-calibration-parameters`
  (lMaxPanel/lMidPanel/lMinPanel, milliAmps2NitsScaleFactor, user-accessible-max-nits, ...), `new-backlight-architecture`
  from AppleARMBacklight, which our stand-in (MiniAppleARMBacklight.cpp) does not publish; backboardd logs "EDRServerContext:
  displays 1 context not found". Overrides exist: CA_DISPLAY_EDR_HEADROOM, CA_FRAMEBUFFER_EDR_HEADROOM. Steps: (1) the
  env override in backboardd as a quick look; (2) the calibration keys on the backlight stand-in (user max ~430 nits, the
  HBM max to be measured); (3) the part above SDR to HBM (DCS 0x53=0xe0, 0x51) with a time limit and a temperature cutoff;
  (4) function 61 in eagl.c (linear multiply, dither). HDR video may still need 10-bit HEVC and HDRProcessing.
  Sources: WWDC22 "Explore EDR on iOS", WWDC21 "HDR rendering with EDR", UIScreen currentEDRHeadroom docs.
  Done since (RAM): CA_DISPLAY_EDR_HEADROOM=2 in backboardd's environment reaches CoreAnimation -- function 61's numbers
  [inf 1 0 0] became [inf 0.5 0 0] (the second is 1/headroom), run 202252; nothing dims because eagl.c skips it. HBM is
  built (`bsd/dev/hk_hbm.c`, `hk_display_hbm()`, boot-arg `hbm=1`, `power_test hbm N`; display.md), not yet run; a first
  5 s pulse waits for the supervisor's OK of its limits (out/rc.jobs.hbm, run flag `hbm`). CoreBrightness's calibration
  keys: being read out of 20F66 (types and units) before the backlight stand-in publishes any (a wrong type could take
  backboardd down).
  **HBM verified on the phone 2026-11-04** (run 204007, three 5 s pulses, kernel 7862f749, hbm=1): the owner "有很亮，不錯",
  "有，很明顯"; each OFF at 5014-5015 ms, battery 25 C, hottest 44.6-49.2 C, TE 11373-11377 us before/during/after (HBM
  does not move it; it is 87.9 Hz there, not 90: see (11)); the way to the loader sent the off-command (logdump).
  **The EDR step verified** (run 203539): with CA_DISPLAY_EDR_HEADROOM=2 the Calendar widget's white went 255 -> 188 =
  sRGB(0.5 in linear light). The GL library with it is in the shared root (nothing changes while the factor is 1).
  **CoreBrightness's new architecture (edr=1, test-only):** MiniAppleARMBacklight.cpp publishes new-backlight-architecture,
  the nine calibration numbers (x65536), user-accessible-max-nits (16.16 LE data) and a marketing table (2..600 nits);
  CoreBrightness takes it (no crash), drives the panel by brightness-nits (16.16; hk_display_set_nits: <= 430 linear to
  0x7ff, above -> hk_hbm_hold, never above brightness-nits-cap). Found (runs 204813-210942, RE of 20F66): "[1] Unexpected
  nits type" is CoreBrightness's own message on every committed step (harmless); DisplayBrightness / NitsPhysical are only
  reports (applying them too made two drivers fight: the owner's broken Control Center curve, run 210107); floating-point
  CFNumbers reach the kernel as their IEEE bits in an OSNumber of "31 bits". **Not working yet:** the slider with edr=1 --
  at each drag CoreBrightness's slider position jumps between 1 and 0 (its nits -> slider lookup in the marketing table
  fails: _DisplayLogicalToSliderBrightnessInternal returns its floor or top), so the panel sweeps oddly; and a fresh /var
  starts at 2 nits and fades up over a minute. With edr=0 (the shared and installed set) the slider is smooth (run 210543,
  the owner "平順變亮"). Next: read how CoreBrightness reads the marketing table (entry type and count, the brightness
  factor at offset 324) and fix the table; then HDR content.
  **Fixed (kernel fb3ad081, RAM run 212539, self-checked from the log):** the cause was ExtrabrightEDR -- with
  user-accessible-max-nits under lMaxPanel, libEDR takes brightness-nits over (raising the backlight up to 4x for
  CoreAnimation to dim, which it did not) and its ramps met CoreBrightness's own. Now edr=1 is the new architecture
  without it (lMax 430, a 17-entry table 2..430, no user-accessible-max-nits: the slider's top is 0x7ff, HBM out of
  reach) and edr=2 adds ExtrabrightEDR (600 over 430) for HDR later. Control Center swept in ten drags of 0.031 of the
  screen (~9.5% each): 4-5, 7-8, 14-15, 25, 43-47, 69-78, 111-123, 175-189, 258-297 nits, monotonic, no 0/1 snaps.
  The owner (2026-11-04 21:3x): "亮度目前測試是OK的". Next: edr=2 with HDR content, CoreAnimation's factor and HBM.
  **HDR content is blocked (runs 213232-215718):** SpringBoard's PhotoKit import fails (3301) because assetsd crashes
  rendering the HEIC -- its log: "No sandbox extension ... for filePath", "[api] No Metal renderer available", "[render]
  Unable to get macro state for the GLContext: 0x0", then "assetsd starting up" with a new pid; and Files does not list
  files placed in the LocalStorage provider's container (userland/src/files_drop.c puts them there, as file-N, and can
  open one by a shareddocuments:// URL, which opened Files on Recents): the location view stays empty. Every route to
  an HDR picture on screen (Photos, Quick Look, an app's EDR layer) wants CoreImage's GPU path: Metal, (12). edr=2 is
  ready for it (ExtrabrightEDR published); the coupling of CoreAnimation's factor with HBM goes through hk_hbm_hold.
  **Photos' HDR view (runs 224931-230437, 2026-11-04):** without libhackios-gl in MobileSlideShow, Photos ends at every
  start once the library has photos. With it, Photos ended within 0.1 s when it decoded an HDR gain map (on opening a
  photo, or for the grid at start).
  - exctrace, 14 times: pc 0 in CI::GLContext::init+204, reached from -[PHImageResult setHighDynamicRangeGainMap:...]
    through +[PFImageMetadata createImageRefFromAuxiliaryImagePixelBuffer:...] and +[CIContext contextWithOptions:].
    With no Metal ("[api] No Metal renderer available"), CoreImage falls back to OpenGL ES on its own: two
    EAGLContexts of API 3 per CIContext (CI::GLContext's constructor, the second sharing the first).
  - The cause was eagl.c's context table, which had no lock. Photos decodes two gain maps at once on two threads; both
    took the same place, so one context was not found again. Its -API gave 0 ("no context of API 0"), its
    getMacroContextPrivate gave NULL ("[render] Unable to get macro state"), and init called glDisable through a
    zeroed table.
  - Fixed in GL library a5074b37: the table under a mutex, a place taken before the slow part, 32 places. **Verified
    on the phone, run 231531:** no usertrap in the whole run; the photo opens full size (the owner: "現在不會閃退").
  - **The first complete EDR ramp (run 231531):** CoreAnimation's factor went 0.993 -> 0.500 in about 2 s while
    brightness-nits rose with it, 70 -> about 140 nits (ExtrabrightEDR: SDR white stays, highlights get 2x). At 20 %
    brightness it reached 0.486. The headroom is about 2 at both levels, so the photo's own headroom is the limit. No
    hbm lines (nothing over 430).
  - **Open: the full-size photo shows without colour** (the owner: "顯示黑白了"; the thumbnails are in colour). It
    reaches backboardd as a two-plane YCbCr surface (plane 0 GL_R8, plane 1 GL_RG8 at half size), drawn by
    CoreAnimation's own shader, `ycbcr_matrix * vec3(t0.r + ycbcr_bias.x, t1.rg + ycbcr_bias.yz)`: Mesa has no
    GL_APPLE_texture_ycbcr_basic_formats. The gain maps (L008) appear only in Photos' CoreImage contexts.
    - Either the chroma plane is neutral from the decoder: Photos logs "PHFigDecoder - failed async decode ... -12914",
      VideoToolbox's kVTImageRotationNotSupportedErr, so it may be on a fallback decode.
    - Or the matrix/bias do not reach the shader as meant.
    - GL library 17af903e logs the planes' bytes, glUniformMatrix3fv's values and the YCbCr shader's text to tell
      which (run 98).
    - **Answered (runs 232852, 233935): what shows is the gain map; the base never decodes at full size.**
      - The compositor's YCbCr path is right: the wallpaper's '420f' surface carries chroma 63-195, and ycbcr_matrix
        arrives as full-range BT.601. Thumbnails and previews (512x384, 500x376, 168x126, 95x127) decode in colour.
      - Each open of a photo: CoreImage textures only its gain maps (L008 240x180, 1024x768, 2016x1512); PHFigDecoder
        logs -12914 for the base; no two-plane surface appears in the full-size view. The supervisor's pixel comparison
        on a Mac (screenshot against the photo's base and gain map) agrees.
      - Not the angle alone: the landscape photo needs no turn and fails too.
      - In 20F66's VideoToolbox, 13 places give -12914, all on the rotation path (VTPixelRotationSession*,
        VTPixelTransferNodeRotationDoTransfer, vt_Rotate_y420, vt_Rotate_Celeste). VTImageRotationSessionCreate sets
        kVTPixelRotationPropertyKey_EnableGPUAcceleratedTransfer; a CPU path exists (vImageRotate90_Planar8).
        VideoToolbox reaches FigSignalErrorAt through stub islands, so there is no table to hook.
      - userland/src/vt_test.c (run 234735) asked VideoToolbox each case directly, and all returned 0: 420f at three
        sizes up to 5712x4284, 10-bit 'xf20', BGRA and L008; 0 and 90 degrees; GPU left alone, asked for or
        refused; scales to 1125 wide. ImageIO decoded both HEICs with their orientation applied (up to 4096). So
        VideoToolbox works; the failure is the path Photos' PHFigDecoder takes through CMPhoto. Its options
        (_figDecodeOptionsFromContext) are AccelerationMode, TiledDownsampling, ForceHighSpeed, UseEvenScalingMode,
        ApplyTransform, MaxPixelSize, OutputPixelFormat, HighPriority and a SourceCropRect. CMPhoto probes
        AppleM2ScalerCSCDriver and IOSurfaceAccelerator's capabilities, has "heifhw" queues and a tiled-layer Metal
        conversion, and links Metal and MPS.
      - No supported switch: allowFallbackDecoder and preferSWDecode are per-request flags in Photos' code
        (_PHDecodeImageFromURLForRequest), and CMPhoto reads no preference that would turn its acceleration off.
        Only one library photo's decode logged -12914; the other HEIC logged nothing and showed grey too, so the
        HDR composite without Metal may fall back as well.
      - **Decided with the supervisor (2026-11-05 00:xx): the HDR display side is done; the full-size HDR content
        view waits for Metal (12).** KNOWN_ISSUES K27. A possible interim (hooking Photos to its PHImageIODecoder)
        is not done (the supervisor's call): it would likely show the SDR base in colour only, since the HDR
        highlights need Tungsten's Metal renderer ("[Tungsten] Metal Device is nil ... degraded experience"). The kept-contexts teardown mitigation matters only where libhackios-gl is injected outside
        backboardd, which is in test roots only today (assetsd, MobileSlideShow); it waits with the real teardown
        fix for Metal, unless the assetsd line goes into the installed inject.conf first.
      - **Blocker found for the HDR content view (2026-11-05, read from the code, M3e planning; unverified on the
        phone):** an RGBA16Float IOSurface the GPU imports must have rows aligned to 128 bytes (freedreno's pitch rule
        pitchalign = cpp_shift + 4, at least 6: 64 B for texels up to 4 B, 128 B for 8 B, 256 B for 16 B; turnip has
        the same rule). The kernel aligns every surface's rows to 64 only, so a full-screen 1080-wide RGBA16Float
        drawable (8640-byte rows) is refused by both Metal back ends, and Gallium's refusal sentence blames the
        surface instead of the pitch rule. Fix, which changes the kernel and so needs a round of its own: the surface
        allocation (iokit/Kernel/HKGPU.cpp / MiniIOSurface) aligns 8-byte-texel rows to 128 and 16-byte ones to 256;
        and the Gallium refusal sentence (air_metal.c's import path / hkdrawable.m) is corrected to name the pitch
        rule, in the same round.
      - **Kernel item, same round: GPU writes, CPU reads, and nobody invalidates the CPU's cache (2026-11-05, read from
        the code during M3e; both Metal back ends and GL alike).** HKGPU.cpp submit() cleans and invalidates (DC CIVAC) a
        surface's lines only at submission (when its seed moved, it was mapped, or 4 ms passed); kMsmWaitFence and
        kMsmGemCpuPrep only wait, MiniIOSurface lockCommon only moves the seed (IOSurfaceLock(ReadOnly) does nothing for
        the cache), hk_adreno.c hk_gpu_done cleans only the fence word. So a CPU that read a surface before the GPU
        wrote it can read stale lines afterwards, when the next submission comes within 4 ms: screenshots, CoreImage
        read-backs, the camera's CPU post-processing. Rare and hard to reproduce. Fix: invalidate at fence signal for
        the submission's surfaces, or at IOSurfaceLock(ReadOnly). air_metal_test --surface self measures it (pre-read,
        GPU write, post-fence read: "stale k gap U us"). **Observed on the phone (run 054947), on both back ends:**
        Gallium BGRA8 100x64 6384/6400 stale with a 5062 us gap (past 4 ms: the clean happens at submission, not after
        the GPU wrote), RGBA16F 64x64 1728/4096 at 2482 us; vk BGRA8 1080/6400 at 525 us, RGBA16F 616/4096 at 290 us;
        1080x2400 0 on both (the surface is larger than the cache). A DC CIVAC from EL0 afterwards makes every read right.
    - Photos' first launch on a new /var puts "What's New in Photos" and a notifications alert in front; a scripted
      run must answer both before it can open a photo.
  - Cost: with the teardown mitigation, each CIContext keeps its 2 Mesa contexts when released, one CIContext per HDR
    gain map decoded. The teardown's real fix (below, (12)) bounds this.
  - The EDR factor in those 0.1 s: 0.993 then 0.999 with brightness-nits 70.0 -> 70.5 -> 70.0. This is libEDR's ramp
    cut off by Photos' end, not a small grant. libEDR's "EDRServerEnableSBIM: IORegistryEntrySetCFProperty() =>
    0xe00002c7" is the framebuffer refusing its enableSBIM property (MiniIOMobileFramebuffer has no setProperties);
    not yet known to matter.
- **(12) Metal, 2026-11-04 (the owner: "開始做 Metal"; "Metal 轉成 Vulkan 繼續，我們要gpu cost 最小"):**
  **First milestone done in RAM (run 223014):** CoreImage over our OpenGL ES, not Metal -- iOS 16.5's CoreImage has a
  real GL backend (CI::GLContext), and eagl.c already replaces the EAGLContext methods it needs; assetsd only lacked the
  library: a test root's inject.conf line `/hackios/lib/libhackios-gl.dylib assetsd` made the photo import work and
  Photos show a HEIC (the owner: "有顯示"); metal_test ci-eagl: "backend: OpenGL ES (CI::GLContext)", "CoreImage
  rendered: first pixel 0 229 255 255" (the linear-light inversion of sRGB orange: right). A teardown SIGSEGV
  (CoreImage releasing the EAGLContext on a thread where it was not current: my_dealloc -> hk_gl_destroy ->
  OSMesaDestroyContext, run 223900, named by metal_test's new dladdr lines and exctrace) is fixed in eagl.c (the
  context made current first, then destroyed); retest run 93. Not yet in the shared inject.conf.
  **The Metal backend (assessed read-only):** turnip from the same Mesa 25.0.7 tree over HKGPU through the existing
  libdrm shim (sync objects emulated on the single in-order ring, mmap, IOSurface import, device discovery; no kernel
  change for a first step), AIR -> NIR handed to turnip directly (no SPIR-V), behind `metal_backend=vk` (default off);
  the compositor stays on OpenGL ES. Milestones: M1 vkCmdFillBuffer over HKGPU read back; M2 a clear and a triangle;
  M3 a Metal clear/draw through turnip (the 263/271 fragment-shader corpus as the regression); M4 the same Metal
  workload on gallium and turnip: GPU time per submission (kHKGPUWorked) and CPU time per frame, on the same kernel.
  Expected: GPU cost about the same (same ir3, same hardware; turnip skips GMEM loads by Metal's load/store actions),
  CPU per frame lower (encoding at record time, any thread).
  **M1 passed on the phone, 2026-11-05 (run 002111; the owner: "Metal first"):** "Turnip Adreno (TM) 650", Vulkan
  1.3.305, one host-visible coherent memory type, one queue family. The device is made in 30 ms. A 1 MB vkCmdFillBuffer
  takes 112 us to submit and 142 us more to signal, and every word reads back right, twice. No trap, no GPU fault or hang,
  no ENOSYS request from the shim. backboardd's frames are unchanged around it.
  - Run 001544 first stopped at SIGILL: turnip read ctr_el0 for the cache line size, which XNU does not let a program
    read. On Darwin the size is 0 now: no cached non-coherent memory (HKGPU's buffers are WC), and
    tu_bo_sync_cache returns early.
  - The build is in docs/BUILD.md.
  **M2 passed on the phone, 2026-11-05 (run 002902):** in vk_test after M1, a 64 x 64 picture cleared to blue by a
  render pass and a red triangle drawn from two SPIR-V shaders (userland/src/vk_test_shaders.h, made by glslangValidator
  15.1). Result: 512 red, 3584 blue, 0 other, in all three of turnip's ways, each a new process:
  - sysmem without UBWC;
  - sysmem;
  - GMEM forced.
  The pipeline compiles (SPIR-V -> NIR -> ir3, on the phone) in 2.1-2.7 ms; the draw and copy take 5.2-5.7 ms. No trap,
  no GPU recovery, backboardd's frames unchanged. (TU_DEBUG's startup line leaves out its runtime flags, sysmem and
  gmem; they were set.) The fill fences took 6-8 ms here, against 0.14 ms in run 002111: to look at for M4.
  **M3a passed on the phone, 2026-11-05 (run 012447):** a Metal draw through turnip, AIR -> NIR, no SPIR-V.
  - tools/mesa/air/air_vk.c is a second implementation of the rwa_* contract (air_metal.h), over Vulkan, built into
    libvulkan_freedreno.dylib with the AIR frontend. tu_hackios.cc gives it turnip's nir options. It exports only
    vk_icd*, rwa_* and rwa_vk_counts (tu_hackios_exports.txt).
  - The NIR goes in by VkPipelineShaderStageNirCreateInfoMESA, with dynamic rendering and a generated full-screen
    vertex stage.
  - hkmetal.m picks the library: HK_METAL_BACKEND=vk|gallium for test programs, otherwise boot-arg metal_backend=vk;
    the default is unchanged.
  - Results, both back ends in one boot:
    - QuartzCore's path_rect_frag_lpf, attachments cleared to 0.25, reads back 0 on both, with equal digests (the shader
      ran and wrote).
    - The hand-written fragPos (tools/air/fragpos.ll, `return pos;`), through air_metal_test --gpu built against each
      library, reads (0.5 0.5 0 1) at pixel (0,0) and (3.5 3.5 0 1) at (3,3) on both: the same origin and y.
    - Nothing leaks (0 command buffers, fences and images after release). No trap or GPU fault.
  - Run 011617 first read 0 from both with a 0 clear, which proved nothing; hence --clear.
  - 19 rwa_* calls are real. The other 25 refuse by name: textures written by the processor, samplers, copies,
    compute, argument buffers, texture_from_surface.
  - Next: M3b, resources. A NIR pass from Metal's flat indices to Vulkan descriptors (push descriptors), then
    texture_write, samplers and copies, then the corpus (--draw-all) against Gallium.
  **M3b passed on the phone, 2026-11-05 (runs 020036 and 020900):** buffers, textures and samplers through turnip.
  - air_vk.c (4104 lines) lowers Metal's flat indices onto one push-descriptor set for both stages (Vulkan allows one a
    pipeline). Binding = stage (fragment 0, vertex 256) + kind (buffers 0, textures 32, samplers 160) + the Metal
    index. A buffer is a UBO when it fits (maxUniformBufferRange), otherwise a read-only SSBO. A read-only buffer bound at
    an offset Vulkan's alignment does not allow is copied into aligned memory first; a written one is refused. Barriers
    and layout changes come before each pass. 29 of 44 rwa_* calls are real; 15 refuse by name with "(M3b)": compute
    (4), texture_from_surface, depth (4), resolve, argument buffers (5).
  - Turnip's limits as printed on the phone: minUniformBufferOffsetAlignment 64, minStorageBufferOffsetAlignment 4,
    maxUniformBufferRange 65536, maxPushDescriptors 32.
  - Run 020036 (out/rc.jobs.m3b), each pair Gallium then vk in one boot: clearFragment20 (15 uniform buffers) with the
    generated vertex stage and with Apple's clearVertex, both THE SAME as softpipe (2458616957 2930221209 3401825461
    3873429713, digest fc5fe79f32951325); texturedFragment2DUInt with an RGBA32Uint 2D texture, 101 201 302 400 on both
    (digest de42bcc64cd36125); air_metal_test --gpu clearFragment20, all 16 pixels and FNV-1a the same.
  - --argbuf: the [[buffer(0)]] encoder is nil on both (Apple's shaders declare no argument buffer). The application-
    described 3-member encoder records 3 of 3 on Gallium; vk records 0 of 3 (rwa_module_argbufs and
    rwa_argbuf_encode_* refuse). Expected: argument buffers are not in air_vk yet.
  - The corpus, all 271 fragment shaders of AppleMetalGLRenderer's default.metallib on both back ends. --draw-all draws
    60 unless given FROM COUNT (metal_drv_test.m draw_count), so run 020036 drew 0-59 and run 020900 (out/rc.jobs.m3b2)
    `--draw-all 60 211`. Joined on name:
    - same, all three attachments' digests: 259.
    - differs only in channels the shader leaves undefined: 1. texturedFragment2DStencil returns one i16 (.r of a uint
      sample, by llvm-dis) into an RGBA32Uint target. R is 101 on both; G/B/A hold whatever each GPU leaves.
    - real differences: 0.
    - vk refused alone: 3, texturedFragment3DFloat/Int/UInt. rwa_texture_create makes only MTLTextureType2D.
    - both failed: 8. Six multisample textures (texture2d_ms, depth2d_ms) neither back end makes;
      texturedFragment2DDepth stores [[depth]] with no depth attachment; texturedFragment3DDepth (depth on Gallium, 3D
      on vk).
    No trap, GPU fault or hang. Every vk object is released (0 command buffers, fences, images after release).
  - Wall clock by rwtouch marks, process start included (not the M4 measurement): 60 shaders, Gallium 0.28 s and vk
    0.50 s; 211 shaders, 1.80 s and 1.32 s.
  - Next: M3c. First 3D textures in rwa_texture_create (the three vk-only refusals). Then depth attachments and
    [[depth]], multisample and resolve, vertex streams and index buffers, blending, compute, argument buffers,
    texture_from_surface (IOSurface). Then M4.
  **M3c-1 passed on the phone, 2026-11-05 (run 023037):** 3D textures, and fences given back at completion.
  - air_vk.c (4284 lines): rwa_texture_create makes MTLTextureType3D (VK_IMAGE_TYPE_3D, a 3D view, maxImageDimension3D
    checked); texture_region, rwa_texture_read/write, rwa_copy_texture and the buffer<->texture copies carry depth
    (bytesPerImage -> bufferImageHeight). Refused by name "(M3c)": a 3D colour attachment (Gallium refuses it too) and a
    pixel-coordinate sampler on a texture3d (Vulkan forbids it). Still one sample, one level, one slice.
  - hkmetal.m -hkWaitFence gives the rwa fence back once it is paid, not at -dealloc (both back ends). Run 020900's
    --draw-all had 208 command buffers and fences in use, because each finished MTLCommandBuffer, autoreleased in a loop
    with no pool of its own, held its fence and with it the back end's command buffer. A failed wait keeps its fence.
    Anything read out of a fence later (GPU times, M4) must be read there, before the release.
  - metal_drv_test.m: --texslots N=3d:FMT binds a 3D texture (the single draw used to drop the kind and bind 2D) and
    reads the 4x4x4 volume back whole; --draw-all prints vk's in-use counts every 50 shaders and the peak.
  - Results: clearFragment20 THE SAME on both. texturedFragment3DFloat (5c568146688f9365), 3DInt and 3DUInt
    (da5663e328d32a25) the same on both, 64 of 64 texels read back on both. --draw-all 5-211: Gallium 206 of 207
    identical to M3b, the other being texturedFragment2DStencil, whose undefined channels move from run to run on
    Gallium too; vk against Gallium 203 same, 1 undefined-only, 0 different, 0 vk-only refusals, 3 failed on both.
    vk in use during --draw-all: 0 command buffers and 0 fences at every 50-shader point (208 before); 10-12 images,
    the textures the autoreleased command buffers still hold (the test's behaviour); 0 after release.
  - Indices 0-4 and 212-270 were not redrawn on this library. Projected over 271: 262 same, 1 undefined-only, 0
    different, 0 vk-only, 8 failed on both (6 multisample, texturedFragment2DDepth, 3DDepth).
  - Next: M3c-2, depth: depth/stencil formats, a depth attachment and depth-stencil state, [[depth]] stores; then
    multisample and resolve, compute, argument buffers, texture_from_surface.
  **M3c-2 passed on the phone (vk), 2026-11-05 (runs 030433 + 030853):** depth and stencil.
  - air_vk.c (5173 lines): Metal 250/252/253/260 -> D16_UNORM, D32_SFLOAT, S8_UINT, D32_SFLOAT_S8_UINT (255, 261, 262
    refused by name, as on Gallium); depth textures with a sampling view (one aspect) and a pass view (all aspects);
    rwa_depth_read per aspect; rwa_clear_depth_stencil; copies with aspect masks (depth<->colour refused: Vulkan needs
    maintenance8); rwa_depth_stencil_create/_destroy; pDepthStencilState and 8 dynamic states; pDepth/StencilAttachment
    at the draw; [[depth]] stores accepted where there is a depth attachment. 48 exports.
  - metal_drv_test.m: --draw-depth ends in a depth-result line on both back ends; texturedFragment2DDepth's depth2d is
    bound a Depth32Float (both back ends bound it an RGBA32Float before); --draw-all retries a [[depth]]-storing shader
    with a Depth32Float attachment and adds "d HEX".
  - Results: --draw-depth clearFragment20 the same on both for Depth32Float, Depth32Float_Stencil8 and Depth16Unorm
    (bit for bit); Stencil8 stops by name on both. vk: texturedFragment2DDepth stores the sampled 0.0859375 and it reads
    back in all 16 texels; 3DDepth stores 0.3359375. The corpus, 271/271 in two boots: 262 same, 1 undefined-only
    (2DStencil), 0 different, 0 vk-only failures, 2 Gallium-only failures, 6 multisample on both. Gallium 269/271
    identical to runs 020036/020900/023037. vk peak in use 0 command buffers and fences.
  - New Gallium bug (KNOWN_ISSUES K28): a fragment shader storing [[depth]] times the GPU out on the Gallium path
    (2DDepth, 3DDepth; the kernel recovers the core, 4 times in run 030433). Never ran before, since both refused it.
  - Hang recovery seen again (for docs/subsystems/gpu.md, whose "3 hangs of 3 recovered" line this extends; that file
    holds another session's uncommitted edits, so it is not changed here): run 030433 had 4 hangs, all 4 recovered
    ("THE 3D CORE IS RECOVERED (n times in this run)"), and every job after them ran normally, the Vulkan back end and
    the window server included.
  - K28 is not freedreno Gallium itself: `gl_test fragdepth z32f|z16` (userland/src/gl_test.c, GLSL ES 3.00
    gl_FragDepth through libOSMesa, the window server's driver, FD650) stores 0.25 and the depth test reads it, in 1.8 ms,
    with no hang (run 034547). So the fault is in the Metal AIR -> Gallium path (air_metal.c or the NIR it hands Gallium).
  - Next: M3c-3, multisample textures and resolve (the 6 shaders both back ends fail); then compute, argument
    buffers, texture_from_surface (IOSurface); then M4.
  **M3c-3 passed on the phone, 2026-11-05 (run 034547):** multisample textures, passes and resolves.
  - Gallium never refused multisample: the six shaders failed on both back ends because metal_drv_test's
    draw_texture_descriptor asked for a MTLTextureType2DMultisample of sampleCount 1 (Metal's own rule refuses it).
  - air_vk.c (5813 lines): 2D multisample textures of the counts turnip offers for the format and usage (1, 2, 4 on
    a6xx, printed at device open); processor reads/writes and mixed-count copies refused in Gallium's words;
    rwa_resolve_texture real (an empty dynamic-rendering pass with a resolve view: AVERAGE for float colour,
    SAMPLE_ZERO for integer and depth; vkCmdResolveImage when the destination is not a render target);
    rasterizationSamples from the pipeline; Gallium's attachment_shape_fits at the draw; sampleRateShading on.
    hkmetal.m unchanged (it already passes the counts and the colour resolves; a depth attachment's resolveTexture is
    not routed: on Gallium that would be a depth-writing blit, K28).
  - metal_drv_test.m: texture2d_ms/depth2d_ms slots get a 4-sample texture filled by a Clear pass, and the drawall line
    ends in "ms match|differs" against the value computed from it; --draw-msaa N (msaa-result line); --draw-one NAME;
    a [[depth]]-storing shader refused by name on Gallium in sweeps (K28) unless --gallium-depth-stores.
  - Results: --draw-msaa 4 and 2 SAMPLE_ZERO and 4 AVERAGE equal on both back ends and to the computed values, ulps 0;
    a triangle edge resolves to exactly half with AVERAGE, to no half with SAMPLE_ZERO. All six MS shaders "ms match" on
    both (DepthMultisample on vk only). vk drew all 271 corpus shaders: 266 the same as Gallium, 2 differing only in
    undefined channels (2DStencil, 2DStencilMultisample: one ushort), 0 different, 3 Gallium-only failures (K28).
  - Left of M3c: compute (4 calls), argument buffers (5), texture_from_surface (IOSurface), mip levels and array slices,
    [[stencil]] stores, depth<->colour copies, depth resolve. Next: compute, then IOSurface (the HDR content view
    needs it, T38 (17)), then argument buffers; then M4 (GPU time, CPU time and submissions against Gallium).
  **M3d run on the phone, 2026-11-05 (runs 043116 + 043536): compute on vk, 45 of 52 the same as Gallium.**
  - air_vk.c (6653 lines, section COMPUTE in its header): rwa_compute_create, rwa_dispatch, rwa_dispatch_buffers
    real; no compute call refuses with "(M3b)" any more. A VkPipeline per threadgroup size (up to 8 kept), from the
    same NIR through VkPipelineShaderStageNirCreateInfoMESA, with ALLOW_VARYING_SUBGROUP_SIZE (Gallium's wave
    contract). Bindings in one push set at 512 + kind + index; storage images (write/read_write textures of a
    kernel) as STORAGE_IMAGE in GENERAL, image_* rewritten to image_deref_*, image_levels = 1; threadgroup memory
    as the frontend lays it out; system values left to turnip's own lowering. Refused by name "(M3d)": more than
    maxPushDescriptors bindings, more than maxComputeSharedMemorySize, a group or grid past Vulkan's limits, one
    texture index both sampled and written. Dynamic threadgroup buffers refused as on Gallium. 48 exports.
  - Tests: metal_drv_test prints `drvtest: compute-result BACKEND NAME ...` for every dispatch, and
    --compute-all [FROM COUNT] sweeps a library's kernels (the 14 PortraitFilters kernels that loop on buffer
    values are left out unless --compute-hangers); air_metal_test's kernel mode prints its own compute-result line
    and takes GROUPS THREADS; tools/air/sysvals.ll (new) writes every system value (expected b0 877fc70d892fc565).
    Jobs: out/rc.jobs.m3d1, out/rc.jobs.m3d2.
  - Results (52 kernel x program pairs, AV1SW's 11, PortraitFilters' 36 less the 14 that loop on buffer values, the
    rc.jobs.metal1/2/3 kernels with their own textures): 45 the same on both back ends, the 11 AV1SW kernels' 16-bit
    storage-texture writes included; every kernel run twice gave the same result twice on each back end, but one.
    - vk refused alone: 6, MetalFaceMask_Dilate/ErodeCube{Luma,Chroma1,Chroma2}: a texture3d sampled with its own
      sampler in pixel coordinates, which Vulkan forbids (unnormalizedCoordinates is 1D/2D only); Gallium runs them.
      Being lowered (normalized coordinates, a normalized sampler): M3d-2.
    - fbs_pcg_iter2 differs between back ends and from run to run on EACH (Gallium 5b148712/a3b0220f, vk
      48bbb488/d6a7ba41 in one boot): global atomic adds and an atomic store across threadgroups, a race in the
      kernel; it uses no simd_/quad_ intrinsic. Not a back-end difference.
    - Limits as printed: maxComputeWorkGroupInvocations 2048 (1024 reported to Metal), shared memory 32768,
      subgroups 64..128 (threadExecutionWidth 64 on vk, 32 on Gallium). No GPU timeout, no recovery, no trap.
    - sysvals did not test the multi-group values: air_metal_test_gl/_vk had been built (04:19) before GROUPS/THREADS
      went into air_metal_test.c (04:24), so both dispatched one 4x4x1 group and agreed (c328822e3e7f90a5). Rebuilt:
      air_metal_test_gl d9bda7cac120df35, air_metal_test_vk b5b8954bc4332d5c. Check a binary against its source's
      time before a run.
  **M3d-2 run on the phone, 2026-11-05 (run 050912):** a kernel's own pixel-coordinate sampler on a texture3d is lowered
    to normalized coordinates (air_vk.c pixel_to_normalized: coord x 1/txs, no half-texel offset; r must clamp; a
    normalized VkSampler); libvulkan_freedreno 5b8f2bdd4d8592ff, which prints "air_vk: built from air_vk.c of <mtime>".
  - sysvals over 2x2x1 groups of 4x2x2: b0 877fc70d892fc565 on both back ends, the host's value.
  - tools/air/sample3d.ll (new; air_metal_test's new "index" fill, R = x + 4y + 16z): texel centres, 8-texel linear
    blends, clamp_to_edge and clamp_to_zero out of range on r and x: b0 064eaea90e929282 on both, the host model's.
    That is also the first check by value of Gallium's 3D pixel-coordinate path (freedreno's hardware unnormalized mode,
    r included): right.
  - The six Dilate/ErodeCube kernels now get a pipeline on vk. Dilate t1 the same on both (weak: it saturates). Erode
    (a product of three fast-half samples, offsets x-1/x+1) differs: Gallium 891419c8, vk a8786e22, each uniform over
    its three variants; with texels of 100+ the product overflows half, so it is overflow handling or the offsets on
    the normalized path. Open: air_metal_test ErodeCubeLuma/Chroma2 with the index fill (exact products, 9240 at
    texel (1,1,1) if offsets are applied) and with the default fill, printing texels.
  - fbs_pcg_iter2 three runs per back end, three different values on each: a race in the kernel.
  - Erode settled (run 054947, air_metal_test, texels printed): sample3d_off (tools/air/sample3d_off.ll, texel offsets
    on the normalized 3D path, constant and computed) reads b0 caf0c000f2f22a7c on both back ends, the host's value;
    ErodeCube's (1,1,1) is 9240 = 20*21*22 on both. A host model reproduces all four Erode digests: every product that
    fits is the same on both; a half product that overflows is +inf (0x7C00, IEEE binary16) on vk and 65504 (0x7BFF,
    saturated) on Gallium. A Gallium deviation (KNOWN_ISSUES K29), not a vk bug.
  **M3e run on the phone, 2026-11-05 (run 054947): textures over an IOSurface on vk.**
  - air_vk.c (7302 lines): rwa_texture_from_surface imports the surface ID as a dma-buf into a LINEAR image at the
    surface's offset and pitch, with Gallium's refusals and the pitch rule by texel size; "(M3e)" refusals for YCbCr,
    depth, and a device without external_memory_fd/dma_buf/drm_format_modifier. Mesa tree: msm_bo_init_dmabuf takes
    the size from hk_drm_surface, tu_AllocateMemory does not close the "fd" (a surface ID); turnip-darwin.patch 333
    lines, regenerated from pristine, applied to a pristine copy: no diff against the tree. Every other use of an
    imported fd in turnip and the runtime checked (export fails, ENOSYS). hkdrawable.m rwd_use_library: the import
    goes to the loaded back end's library (it was looked up in libhackiosair by name, so vk had no drawables).
  - Results: air_metal_test --surface self (CPU-computed expectations): BGRA8 100x64 stride 512 and 1080x2400 stride
    4352, RGBA16Float 64x64 stride 1024: clears and copies match on both, row padding kept; fragPos into BGRA8 the same
    on both; RGBA16F 100x64 stride 832 refused on both (vk names the pitch rule). drawable_test 1080x2400: 0 of 26
    checks failed, the same digest on both. No SMMU fault.
  - Open: a draw (fragPos) into the RGBA16Float imported surface DIFFERS from the model on vk (fd7efe83 against
    8f100801; Gallium matches); clears and copies into it are right.
    - Run 060507 (pixel dump, out/rc.jobs.m3e2): the same draw into a vk-owned texture, read back through the GPU,
      matches on every variant; into the imported surface, read by the CPU after a clean+invalidate, vk default
      matched this time, TU_DEBUG=sysmem left 1024 texels, sysmem,noubwc 664, gmem 504 -- every one of them the
      pre-draw value in all four channels, the first at x = 8/16/24 (64-byte lines); every written texel is
      bit-exact. So not rendering, format, import or GMEM: some GPU writes are not yet in memory when the CPU reads
      after the fence, at random, a cache line at a time. Hypothesis (unchecked): turnip's CCU colour clean is a TS
      event the kernel's closing CACHE_FLUSH_TS fence write does not wait for. It would also hit the compositor
      reading a vk-rendered surface. Next: a post-fence delay to confirm, and a waiting end-of-buffer flush in
      air_vk behind a switch.
    - **Settled, run 061800 (out/rc.jobs.m3e3, each case 3 times on vk sysmem): not a vk bug; stale CLEAN CPU lines.**
      A 20 ms delay after the fence: 1/3 right (not late GPU write-back). A full flush + wait at the end of the command
      buffer (AIR_VK_HOSTFLUSH=wfi) and TU_DEBUG=flushall,syncdraw: no help (not turnip's flush order). A clean of the
      surface plus a seed move right before the draw: 3/3 right. Reading after evicting the CPU caches with 16 MB of
      reads: 6/6 right, while the plain read before the eviction was stale every time -- memory held the GPU's
      picture; the CPU's cache held step 8's clean lines of the pattern. Why vk and not Gallium (inferred): Gallium's
      reads and writes map the buffer through HKGPU, which marks it seen, and the kernel then cleans/invalidates it
      at every submission; turnip never maps an imported buffer, so a submission within 4 ms of the last, with no
      seed move, leaves it alone. And the test's own post-fence sys_dcache_flush did not drop the stale lines
      (eviction did): XNU's D-cache-flush platform syscall is removed here (machine_routines.c platform_syscall
      case 1), so libplatform does it at EL0, and here it evidently does not invalidate -- open (a one-line test:
      inline `dc civac; dsb sy` against sys_dcache_flush). M3e counts as passed on vk; the fix is the kernel item
      under T38 (17): every surface imported to the GPU, mapped or not, cleaned/invalidated at its submissions and
      invalidated after the GPU writes it. AIR_VK_HOSTFLUSH=wfi is useless and is to be removed from air_vk.c.
      The fix, in two layers (agreed with the supervisor, 2026-11-05):
      - now, in userland, as Gallium does: air_vk's import does once what gets a Gallium buffer marked seen by the
        kernel (its CPU map through HKGPU), so the kernel cleans/invalidates the surface at every submission on vk
        too. It does not cover a CPU reading during the GPU's write, nor GPU-writes-then-CPU-reads with no
        submission in between;
      - the kernel round (with the RGBA16F row alignment): every surface imported to the GPU, mapped or not, cleaned
        and invalidated at the submissions that use it, and invalidated after the GPU writes it (at fence retire or
        IOSurfaceLock(ReadOnly)), so "GPU writes, CPU reads" is covered;
      - the tests: sys_dcache_flush is not trusted to invalidate here; air_metal_test gets an inline `dc civac; dsb
        sy` read (--read-civac) and libplatform's implementation is to be read, so a "flushed ... match" means what
        it says. Run 054947's flushed matches were partly luck (the lines happened to be gone).
      - **sys_dcache_flush is a no-op on this system (read 2026-11-05 from 20F66's dyld cache, read-only):**
        libsystem_platform's sys_dcache_flush is two instructions, a barrier and a return: no cache maintenance at
        all -- Apple's GPUs are I/O-coherent, so iOS needs none -- and XNU's own platform syscall for it is removed. Every "flushed" read of
        the surface test before --read-civac was a plain read; and nothing iOS does with sys_dcache_flush maintains
        the cache on our non-coherent Adreno. GPU-shared memory's coherency has to come from the kernel.
      - **Userland mitigation, verified on the phone (run 062853):** air_vk's import maps the memory once (vkMapMemory ->
        hk_drm_map -> HKGPUUserClient::clientMemoryForType, which sets the buffer's `seen`, HKGPU.cpp:1414), so
        submit() cleans/invalidates it at every submission as Gallium's buffers are (AIR_VK_NOSEEN=1 turns it off).
        Cost note for M4: turnip names every buffer of the process in every submission, so a seen surface is cleaned
        at every vk submission, while Gallium pays only where the surface is named.
        Run 062853: with it, the RGBA16F draw into the imported surface matched 9/9 (vk sysmem, default, gmem x3) and
        the stale-read check read 0 stale; with AIR_VK_NOSEEN=1 the old failure came back (1/3), and air_metal_test's
        --read-civac (inline `dc civac` + `dsb sy`) read it right 3/3 -- a real invalidate, unlike sys_dcache_flush.
        **M3e passes on vk** (import, clears, copies, draws, refusals, drawable_test). libvulkan_freedreno
        a2b44a1c63233269, air_metal_test_gl/_vk 6aea8d8e/852e457b; out/rc.jobs.m3e4.
  **M4-1 verified on the phone, fixed clocks, 2026-11-05 (runs 084829, 085237, 085644; 525 MHz, cpufreq_gov=0, gpu_irq=0;
  libvulkan_freedreno cba626e5552d5aa9, metal_drv_test c28bc6e6687e45c4).** No fault, recovery or throttling. W4 per
  frame, us (wall / user+sys / GPU): owned texture vk 742 / 306 / 428, Gallium 1005 / 305 / 746 (vk was 625 / 652 / 428:
  the busy poll is gone, and sleeping at gpu_irq=0's granularity costs about 117 us of wall); imported surface vk seen
  1432 / 738 / 792, vk not seen 1215 / 334 / 800, Gallium 1306 / 327 / 998. So vk's GPU time is 0.57-0.79x Gallium's,
  its CPU equal but for the seen surface's kernel clean (about 390 us a frame, the kernel round). W2: wall 1096 against
  595 ms, draws at 1.0 submissions each on both, but read-backs in the harness are a round trip each on vk (1926 against
  275 submissions), and pipelines cost 779 against 155 ms (about 5x: a pipeline cache is the fix, not yet done). W3: wall
  945/1282 (AV1SW) and 1852/2921 ms (PortraitFilters), vk's submissions from 11.8k/36.1k to 189/375; its net GPU totals
  (9.8/74.9, 35.5/229.8 ms) are far lower because Gallium's row-by-row texture writes go through the GPU (about 65 and
  220 ms outside the dispatch phase) and vk's are CPU copies plus one batched copy. vk's "dispatch phase" reads higher
  (raw 16 against 11 ms, 34 against 12.5 ms) because the deferred uploads are submitted with the first dispatch, so a
  kernel's own GPU cost on vk against Gallium is still unmeasured: --compute-all needs a second dispatch with no uploads
  between. Per-dispatch net figures are mostly noise (a 10 ms phase against an 8 ms baseline). W1: equal. Order and
  burst tests right on both.
  To do from this: a VkPipelineCache, persisted under /var (pipeline creation is about 5x Gallium's; a real app feels it
  at launch and on each new screen).
  **M4 at default DCVS and governor (runs 090249, 090657, 091116), closed:** no fault, recovery or throttling. W4 ramps
  from 305 to 525 MHz within its 200 frames on both back ends; W1-W3 stay at 305. W4 per frame (wall / user+sys / GPU, us):
  owned vk 1032 / 707 / 428, Gallium 1806 / 846 / 746; surface vk seen 2834 / 1819 / 1106, not seen 2568 / 1986 / 817,
  Gallium 2282 / 1089 / 1017 (CPU clocks move under the governor, so the surface row's CPU is noisy: not read further
  without a larger sample). W3 wall vk 980/1902 against 1327/2981 ms; W1 2879 against 3390 ms; W2 2487 against 3162 ms
  with big swings. Energy: only a rough estimate is available (GPU-busy time at the clock seen, no power measurement):
  W4 owned, vk about 0.57x Gallium's GPU active time per frame. Conclusion: Metal over Vulkan costs less GPU than
  over Gallium in steady drawing (0.57x owned, about 0.8x imported at fixed clocks), CPU no worse apart from the seen
  surface's kernel clean.
  **The owner's decision, 2026-11-05 (relayed): Metal goes over Vulkan; no more cost comparison** ("做完这轮没必要比较了
  ... 我们无论如何都要做了"). So no gpu_irq=1 comparison and no per-kernel GPU measurement (a second dispatch with no
  uploads); an obvious performance problem met in the work is simply fixed. The VkPipelineCache stays on this list.
  Correctness is still checked: each feature keeps a per-pixel model, and Gallium as a reference where it can be.
  **M5a built, NOT run on the phone (2026-11-05): the draw path of air_vk.c.** Blending (every factor Gallium serves, plus the blend
  colour, per attachment, sRGB), [[stage_in]] (49 vertex formats, offsets, strides, Constant / PerVertex / PerInstance and the rate),
  indexed draws (16, 32, offsets, base vertex, base instance, instances, restart on the two strip types as Metal has it), cull and
  winding (dynamic, not reversed: air_vk.c, THE DRAW PATH, ORIENTATION), a viewport with an origin, scissor, Bresenham lines as Gallium
  programs them, per-attachment clears. Contract: seven fields appended to rwa_draw (air_metal.h, hkmetal.m), no new export; the Gallium
  library reads none of them and is not rebuilt. Refusals "(M5a)" are listed in the section. Not carried, because Gallium has none:
  indirect draws, depth bias, depth clamp, fill mode, dual-source blending (refused by name).
  - Tests: tools/mesa/air/draw_model.h (the CPU model, shared); tools/air/m5a_*.ll (ten modules: userland/obj/m5a_*.bc must be at
    /hackios/share/probes); air_metal_test --pixels DIR after --draw-blend, --draw-attrib, --attrib-sweep, --draw-index, --draw-vertex;
    metal_drv_test --pixels after --picture, --draw-attrib, --draw-index, --blit (Apple's functions, through the encoder).
    Lines: drvtest: draw-result BACKEND CASE ok|DIFFERS|gallium-lacks|refused digest HEX expected HEX; draw-diff, draw-map, draw-summary.
    Jobs: out/rc.jobs.m5a1 (the pictures), m5a2 + m5a3 (the corpus, split).
  - Measured on a host softpipe (Linux in the build container, a throwaway build of the same air_metal.c; not the phone): 128 of 128
    blends right including all six formats' sRGB and rgba32f exactly; 49 of 49 vertex formats; everything but the three Gallium's
    software path does not do (base instance in [[instance_id]], the shallow line's rule), so the models and the modules are checked,
    and vk itself has only been compiled.
  - Found on the way, not changed (Gallium keeps its behaviour): air_metal.c's restart is off where Metal's is on; one
    MTLDepthStencilState with two references uses the front's on Gallium when the two faces' states are equal (stencil[1].enabled);
    its rasterizer state leaves point_size_per_vertex and line_width at 0; the clear of a pass is one colour for all attachments.
  - Next: run m5a1. On a DIFFERS read draw-diff and draw-map first. Then M5b.
  **Owner's decision, 2026-11-05 ("先补充metal", relayed): complete Metal on vk before the phone goes to the MDP session.**
  The plan, M5: (a) blending, vertex attributes, index buffers; (b) mip levels, array slices, 1D/2DArray/Cube textures,
  generate_mipmaps, more pixel formats; (c) argument buffers, fragment/vertex storage writes, runtime-indexed resources;
  (d) a real app: rw_pipe_lock around -replaceRegion:/-getBytes:, triangle_app --layer --backend vk, then metal=2
  metal_backend=vk. Framebuffer fetch, [[stencil]] stores, depth resolve, depth<->colour copy only if an app needs them.
  Each with a test whose output varies per pixel, a CPU/host model, and Gallium as the reference where it can be.
  The kernel round stays after this unless M5d needs it sooner.
  Added by the supervisor: M5a also orientation (asymmetric triangle, winding and cull, viewport, scissor, primitive
  types), pass state (Load, MRT, several encoders), blend constant, sRGB, baseVertex/baseInstance, strip restart; M5b also
  sampler state (LOD clamp, mip filter, anisotropy, compare, border colour), texture views with swizzle, per-level and
  per-slice blits, ASTC/ETC2 if Gallium has them; M5c also function constants, and the MetalPerformanceShaders metallib
  through --compute-all (HDR's content view runs through CMPhoto and MPS); M5d: the Photos HDR full-size view is the last
  target, where the RGBA16F 8640-byte rows force the kernel round (say so before reaching it); metal=2 boots with
  exctrace and short secs, a failure read from its log, not retried.
  **M5b built, NOT run on the phone (2026-11-05): textures, samplers and formats of air_vk.c.** Section TEXTURES, LEVELS AND SLICES there. Contract: `border_color`
  appended to rwa_sampler_state, `color_level`, `color_slice`, `depth_level`, `depth_slice` appended to rwa_draw, one new symbol `rwa_texture_view_create` (49 exports). The Gallium
  library reads none of it and is not rebuilt. Tests: air_metal_test `--textures|--pixel-formats|--blit --pixels DIR`, metal_drv_test `--tex|--texfmt|--blit --pixels`; models
  tools/mesa/air/tex_model.h; probes tools/air/m5b_*.ll; jobs out/rc.jobs.m5b1-4. Open questions for those runs: clamp_to_zero on a format with no alpha (Apple says
  (0,0,0,1)) and turnip's blit rounding against the one-step box model. Blend model: quantizes first; sRGB colour alone keeps a measured band (linear 2^-10), the hardware's curves
  being not the book's. Measured on real Metal on an M1 Max, not A11 (the reviewer, public API): packed 16-bit has the first-named channel in the low bits, so air_vk.c has that and
  air_metal.c's table is its mirror, kept on purpose (its rows print `gallium-mirror`); and the LOD clamp applies at an explicit level, whatever Apple's header says. OPEN: who does the
  exchange the compositor's wallpaper shows; a B,G,R,A swizzle would look the same (hypothesis). hkmetal.m logs a `packed16:` line per distinct 16-bit texture for a short metal=2 boot
  on Gallium to read; until then a vk picture of formats 40-43 is a question and not a result.
  **M5a run on the phone, 2026-11-05 (runs 094931, 095351, 095811; libvulkan_freedreno 9617015acea9d0c5): the draw path.**
  No fault, recovery or usertrap. vk: 252 draw-results ok, 15 differ, 2 refused on purpose. Orientation (asymmetric
  triangle, all four cull/winding combinations with both triangle directions), viewport, scissor, pass state, MRT, indexed
  (scrambled grid, base vertex, base instance, restart on strips), vertex_id/instance_id, vertex formats: ok against
  the CPU model. The 15: 14 blends where vk's digest EQUALS Gallium's and the model is off by just over one unorm step
  (0.85098 against 0.85502, 12 of 256 pixels; sRGB reverse-subtract and subtract likewise), which is the hardware's
  rounding, so the model's tolerance is to widen; and the line list, 2 of 250 pixels at a line's first pixel (x=14,
  y=2), a line-end convention in the model. Corpus 271/271 on vk: 264 identical to the earlier digests, the other 7
  expected (the MS shaders against older lines, 2DStencil's undefined channels). Gallium control: identical but those.
  sysvals 877fc70d, surface right, depth and msaa right. Found: Gallium draws no lines or points (KNOWN_ISSUES K30).
  M5a counts as passed (the supervisor concurs). To carry into the next build: the model following the blender's path
  (quantize dst first, then blend, then round: not a wider tolerance), the line-end rule, a coverage map always printed for
  orient-asymmetric, and the -setDepthBias: notice and restart sources already in the sources.
  **M5b's results, runs 111109, 111530, 111950, 112410 (2026-11-05, phone, kernel cam-fb3ad081): no fault, no recovery, no usertrap on any.**
  vk: 864 ok, 16 DIFFERS, 5 refused by name. Of the 16: render-from-own-level was my expectation (the coordinate lands on the edge at 2x + 1 and the floor
  takes it; real Metal gives the phone's bytes); 13 are sRGB decode rows, of which 12 have the same digest on Gallium, up to 0.0028 linear from the book where
  an M1 is within 0.00012, inferred Adreno's precision and measured next by `--srgb-curve`; blend-*srgb-sub (2 rows, 5 pixels) the same curve, by the same
  reasoning and the same digest on Gallium, 0.0027 past a 2^-10 taken from two samples. orient-asymmetric's corner: top left, x 2..12, y 1..11, 61 pixels, one
  map on both back ends and on an M1. Corpus against 095351/095811: Gallium 39 of 40 identical, vk 269 of 271, the rest Stencil's undefined G/B/A (R stays 101).
  sysvals, sample3d and the surface as before. **Criterion 4 (corpus and compute) is met for DRAWS ONLY:** --compute-all ran --quiet, so AV1SW and PortraitFilters
  have ran counts and no output comparison here; M5c runs it without --quiet, kernel by kernel against Gallium where Gallium ran it. Accepted by the supervisor with
  its two gaps named: the 2 sRGB-sub rows and this.
  Stencil's "undefined channels" were this harness's own: real Metal refuses a one-ushort fragment shader on an RGBA32Uint attachment (M1), so nothing is there to
  match, and those two rows now digest the channel they write (metal_drv_test.m, target_digest). Their digests change once, and "undefined channels aside" can go.
  **M5c, built, 2026-11-05, rows passed on real Metal, phone runs next:** argument buffers, a draw's stores, arrays of descriptors, function constants, on vk.
  The design is in air_vk.c, ARGUMENT BUFFERS (at resolve_binding) and FUNCTION CONSTANTS (at rwa_compute_create), and hkmetal.m, FUNCTION CONSTANTS.
  - **tools/air/mtlref/airdump.m**, new and worth more than this milestone: this Mac's own Metal compiler turns MSL into a real metallib, through the private
    -serializeToURL:error: of _MTLLibrary, so a test shader can be written in MSL, its AIR read with llvm-dis-18, and it can run on real Metal and on the phone
    from one source. tools/air/m5c.metal and tools/air/m5c_rows.h are that: `mtlref --m5c out/msl/m5c.metallib` and `metal_drv_test --m5c LIB` run the same rows;
    real Metal gives 65 ok, 0 differ. What it says of forms is macOS 27's compiler (air64_v29), so the corpus stays the check on iOS 16's.
  - What real Metal's AIR settled, each read off that output: the second integer after air.location_index IS the array length; an array of buffer pointers has
    its own metadata form (`air.struct_type_info` first, the id an integer, the length in the inner node); -newFunctionWithName: returns ONE object, which is why
    a specialized function is a proxy here; a constant left unset still makes a function; a vertex stage may run more than once per vertex (counts 1 2 2 1).
  - Native frontend without a phone: `meson setup /tmp/hf` in the container with `-Dgallium-drivers=softpipe`, no libdrm needed, `ninja src/compiler/air/air2nir`, and
    `air2nir --report LIB` over the MetalPerformanceShaders libraries: MPSImage 1318 of 1825 kernels, MPSCore 54 kernels and 4063 air.visible functions (stitching),
    MPSRayIntersector 48 of 66. MPSImage's refusals: aggregate index out of its value 245, pointer at a computed offset passed to a call 71, a pointer stored 50,
    N-bit integer 43, phi of pointers over a loop edge 36, insertelement at a run-time index 18. To do, with air.visible measured before it is started.
  - Refused still: texturecube.read() (Vulkan has no fetch on a cube view, and the frontend says so at the coordinate); -newRenderPipelineStateWithDescriptor: with
    no fragment function, which a depth-only pass or rasterizationEnabled NO needs (M5d).
  **M5d, 2026-11-05, the owner's direction (relayed): every boot is the goal, `metal=2 metal_profile=1 metal_backend=vk camtl=1`, RAM only; what stops the system decides the next fix.** Gallium jobs, the non-camtl boot and the sweeps only if a fix needs them. Verified on phone, vk: the --m5c rows through fc-* in run 130504, 69 ok, 0 differ, 0 refused, inside a system boot (so M5c's rows have passed; the fetch rows are newer).
  Found by those boots: (1) `[MTLFunctionConstantValues alloc]` is a `MTLFunctionConstantValuesInternal`, with its own setters, so hkmetal's hook on the public class recorded nothing and every constant read as not defined (runs 125308, 125405, 125612); it is on the Internal class now and `58 fragment constants` reach QuartzCore's fixed_frag_lph_cph. (2) CoreAnimation answers a refused pipeline by trying once more and then SIGABRT, so one refusal is a dead window server in a loop (11 lives in run 125405); nothing in hkmetal stands between them, and a stand-in pipeline for backboardd is undecided. (3) air.get_null_texture as a second place in a phi was a regression of M5c's; a phi or a select of a real resource and null or a module constant now resolves to the resource (air_to_nir.c, resolve_pointer_phi, load_pointer_through_choice). (4) Function constants reach a shader through scratch, so nothing folded them: fold_module_variables, 52,451 lines of NIR to 221 for fixed_frag_lph_cph on the host with no values. (5) Framebuffer fetch is real on vk: air_vk.c FRAMEBUFFER FETCH, a subpass input by dynamic rendering local read; the multisampled case is refused. Rows fetch-*, real Metal 75 ok; phone: see HANDOFF.
  **Verified on phone, run 141516 (2026-11-05, RAM boot): the home screen is complete and right with the compositor on Metal over Vulkan** (`metal=2 metal_profile=1 metal_backend=vk camtl=1`), 30 frames a second while it moves, 0 SMMU, 0 IS, 0 DEVICE_LOST. Against the GL-compositor shot of run 122525: 0.88 % of pixels differ by more than 12, in the clock's hands and the status bar's corners and a few edges (a picture compare, not a proof). Further fixes of that day, each found by the boot: (6) a half between the stages arrived as garbage, a different colour every frame: storageInputOutput16 is off in turnip, so widen_varyings makes them 32 bits between the stages (`metal_drv_test --ca-fill`, row half-varying). (7) RenderBox's primitive_gradient_fragment has two [[function_constant]] buffers at one index, constant and device: stage_bindings takes the one the instructions touch (Weather's card). (8) MTLPixelFormatYCBCR8_420_2P: hkBiplanarTexture copies a 420f or 420v surface into RGBA8, in RGB, BT.601 (the stock wallpaper's Head.HEIC, measured against this Mac's decoder: full range); the pipeline CA draws it with has no matrix, so Apple's sampling answers colours. air_intrinsics: fmax3 family, pack.unorm.rgb565 (red in the LOW bits, measured). air_to_nir: narrow integers up to i31.
  Apps verified on phone, same config, runs 141836-143528, opened with rwtouch: Settings, Safari, Clock, Photos (first-run sheet with an alert over it), Maps (first-run sheet). Further fixes those boots asked for: attachments of different sizes (the pass is the part they share), a surface row not a multiple of 64 copied once when read-only, a whole mipmapped view sampled while a level of it is rendered, an i1 used as a vector index (ir3's "invalid src bit size: 1" was a trap that killed chronod 27 times in a row), air.pack.*/unpack.* (rows pack-*, unpack-*; real Metal 101 ok, snorm10a2 not in this Mac's compiler, not measured), bitcast of any equal total, `inttoptr` constants as null, resources whose index is a function constant's (`air.location_index` is `@__metal_implicit_attr_int_expr_N`: air_to_nir translates twice), surface formats for what a kernel lays over a surface.
  **Open from that, in order of the loop:** (a) [served since, M5e below; this was the state then] **VideoToolbox's Metal path is declined, a stopgap:** `hk_specialize` answers nil for VTMTSComputeFunction. Served, its pipeline is made in 3 s (2,037 lines of NIR), its one dispatch returns, no SpringBoard thread is waiting 45 s on (`hk_sample_threads`, a file /hackios/share/probes/sample in the root), and the wallpaper's HEIC layers are simply never decoded, 420f included. Why its success changes that is not known. Real iOS uses it. (b) The 420 copy is made once per surface: a video would not update. A real import needs VkSamplerYcbcrConversion or per-plane views. Format 500 measured on real Metal (M1): RGB, BT.601, 420f (42,133,128) and 420v (32,135,130) from Y 105, Cb 141, Cr 83, as hkBiplanarTexture computes; a surface tagged 709 is not measured. (c) [fixed: chronod's trap was ir3's refusal of an i1 index, above.] (d) compositeSphere (Siri's orb) reads a target at 4 samples per pixel: refused, lower_fbfetch has no SUBPASS_MS. (e) [done: corpus vk 271 of 271 identical to run 124507 in run 151633, after the fold and everything since.] (f) hkmetal's `spec`, `layout`, `draw`, `new texture` lines are bounded but still on; the per-draw pixel probe is off unless /hackios/share/probes/metal_probe exists. (g) Not tried yet: the map itself past Maps' first-run sheet, a video, HDR Photos, a third-party Metal app.
  **M5e, HEIC and HDR (the owner, 2026-11-05: 「heic啊，你走偏了」; stopped by 「我们保存结果接下来做dms」; RAM only, nothing committed). Done, each verified on the phone unless said:**
  (1) `hk_specialize` serves VTMTSComputeFunction again; a file /hackios/share/probes/vt_decline turns that off for a comparison, and is absent. (2) **Every texture write was typed uint:** `tex_dest_type` read the call's return type, void. The GPU converts by that type, so 0.5 and 1.0 into RGBA16Float became 0xffff and VideoToolbox's two-pass scale came out empty (read back in run 161649; fixed air_intrinsics.c, run 162255, rows kernel-write-*). (3) **The black wallpaper was compile time, not decoding:** CollectionsPoster is a Metal client, decodes a HEIC on its way up, each VTMTSComputeFunction took 1.8-3.2 s and launchd kills an extension at 5 s ("hanging on launch", XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT), 23-35 times a boot, a new process each time. Declined runs show 0. Runs with log_launchd=0 show nothing at all, which hid it (the runner's `lean` now leaves launchd's lines on). Fixed: the learning pass of `air_to_nir` skips the body, and A TRANSLATION KEPT ON DISK (air_to_nir.c; /var/tmp/hk-air; key: bitcode, entry, constants, the library's size and date; plus turnip's compute pipeline, one VkPipelineCache file per threadgroup size in air_vk.c). Host: 0.76 s to 0.40 s, 0.015 s on a hit, NIR byte-identical on three specializations. Phone: first compile 0.9-1.6 s; hits not timed on the phone; turnip's share not measured; check-in time not measured. Empty cache: killed 2 times, then stays, about 11 s after the first start. (4) Photos' fragment_main: an array of textures copied into a struct on the stack and read at a computed index, and a cursor stored 57 times and never read (POINTERS IN A LOCAL; row kernel-descriptors-in-a-local). Photos treated the refusal as an uncaught exception. (5) A sample outside a fragment function is level 0 (real Metal: row kernel-sample-level); it was not the zeros' cause, and stays because it is the rule. (6) Format 540: raw Cr, Y, Cb, alpha 1, centred chroma, for 420f and 420v, by read and by sample, measured on real Metal; served as a copy (hkBiplanarTexture raw:YES). (7) A read-only surface's copy follows it (`hkFollowSurface`); a destination whose row the GPU cannot write is refused at import rather than turned into a read-only copy.
  **Stopgaps that stand:** 420 pictures as CPU copies, format 500's once per surface, format 540's once per texture (no VkSamplerYcbcrConversion, no per-plane views). Copied surfaces cost a full read of the surface per command buffer. The disk cache has no limit and no eviction, does not hold turnip's graphics pipelines, and does nothing on a read-only /var: on the kept volume in a RAM test the kill loop could return; not tested. Pipelines are still compiled on the phone once; nothing is shipped precompiled.
  **Open, in the order they stop things:** (a) -12914 from CMPhoto with 540 served, cause unknown. (b) VideoToolbox's destination imports: format 123 (RGBA32Uint) over BGRA at a quarter of the width (four pixels a texel); R8Uint (13), R32Uint (53) per plane on a 420f; `rwd_texture_for_surface` wants plane 0 and equal widths. Plan: plane offset and row from IOSurfaceGetBaseAddressOfPlane and ...BytesPerRowOfPlane, the check by bytes. A 16-byte texel needs a 256-byte row, so format 123 over most rows waits for (e). (c) The dim screen: Photos' white 187 of 255, picture at most 191, nits 70-83, with and without hbm=1. The drawable is format 80, BGRA8; no RGBA16F surface appears. The same photo with the compositor on hk-gl has NOT been booted for comparison; do that before anything installs. (d) Full view 1512x1134 and 1496x1122 before and after 540; the photo's real size unread. (e) **The kernel round, not started:** MiniIOSurface's row alignment (8424 bytes a row in assetsd, 8640 for RGBA16Float against 128; freedreno's rule is 64 up to 4 bytes a texel, 128 for 8, 256 for 16); IOSurfaceSharedEventAddEventListener e00002c7, MiniIOSurface has no shared events, effect unknown. (f) vt_test's 420f colour on the phone 129 124 34 against this Mac's 128 133 42, the same by GPU and CPU: reads as VideoToolbox's own matrix on this boot; not settled. (g) Rows surface-540-*: the phone refuses the shader, "an inline sampler whose state is not a constant", from this Mac's compiler; use an API sampler or read what that global is. (h) threadsof (userland/src/threadsof.c) gets its task but task_threads is refused; not chased. The kernel's `ustacks=NAME ustacks_at=SECONDS` is the alternative, 40 bytes.
  **Default-on, the owner's rule (「之前成功的不进内核参数但是默认enable」, 「wifi震动声音相机都能用了」, supervisor's relay), for this board only (instantnoodle_board()), arg=0 off; touch is already on (instantnoodle_touch.c) and is not to be touched. Nothing flipped yet, all in the next new kernel copy.** Gates read so far, to be confirmed against the driver this board runs: `wifi` hk_wifi.c (with wifi_bdf_native), `adsp` hk_adsp.c, `sound` hk_sound.c, `sound_amps`, `haptic` hk_haptic.c, `cam` instantnoodle_camera.c. Then drop them from the runner and stable-plan.json, boot once with none, check each one's log line, and check a second boot in one power-on (never wifi_again or adsp_again; run 160351 logged `ath11k WARNS: firmware crashed: MHI_CB_EE_RDDM` once at the return to the loader, and the next boot's radio came up). Runner now: Wi-Fi and sound on, `nowifi`, `nosound`, `nocam`, `novoldown` (25 bytes), `datadefault` (15). Bytes of 607: ramhfs, all on 656; no ramhfs 623; ramhfs with Wi-Fi only 601; Photos' log, edr=2, photos_import with nowifi, novoldown, datadefault 599 (605 with hbm). **So goal boots have run with camera and sound off, and the Photos boots with Wi-Fi off too, for bytes alone.** The return to the loader can abort backboardd if a pipeline is being built that moment (run 160351: allocations fail after `loader_return_after expired`); read as the return, 0 in the boots since.
  **Safari, left on the owner's word (2026-11-05, 「heic啊，你走偏了」), not finished.** Verified on phone, run 154007 (root m5e1, RAM): Safari draws a page on Metal over Vulkan, fetched from `userland/src/hkhttpd.c` on the phone's loopback (page: `tools/metal/mk_testpage.py`, built only by the scratchpad's build_m5.sh, not in `userland/build.sh`); text, colour bars in order, gradients, canvas, SVG, opacity, a CSS animation at 87 frames a second, 0 SMMU, 0 IS, 0 DEVICE_LOST. **Wrong, not explained:** after scrolling, parts of the page are missing or overlap (runs 154007, 154748): bands of a picture out of step, black or white below a cut, for several seconds a white page. The page's tiles are 1125x1536 surfaces the GPU has in place (`place#N`), seed 1, and this processor reads colours in them. Not known: whether the tiles' memory or their placing is wrong. Next look: a thumbnail of a tile from this processor against the screen, and the vertices' real stride (the `at` and `t` of the frame_dump lines assume 48 bytes and are only right for some pipelines). Copied surfaces now follow changes (`hkFollowSurface`: added up once per command buffer, copied again when the sum moves); 420 copies still do not. Diagnostic left in: file `share/probes/frame_dump`. Wi-Fi: nothing joined in these boots, `fw-wifi/network` does not exist here (never read); earlier joins were by hand in Settings.
  Host census of all 103 system metallibs with `air2nir --not-defined --report` (counts of refusals, what the next blockers may be, none yet asked for by a running process): CMImaging 15 air.imageblock, QuartzCore's 4 tile_downsample imageblocks, CoreUI's 17 "no entry point metadata" (kernels of an older form, to read), MetalTools' 7 air.command_buffer, simd_shuffle* / simd_min / simd_max / quad_shuffle in MPSImage, FRC, VideoProcessing; patch_control_point inputs in LocalAuthenticationPrivateUI (7), MTLSpline, ARKitUI, RoomPlan. RenderBox 47 of 47 now (i10, pack.rgb565 with red in the LOW five bits, measured).
  **libhackiosair.dylib is not rebuilt in this work and stays 6ef66c30 (08:53).** So "0 stale" in every M5c notice holds with that exception: air_metal.c and the shared frontend (air_to_nir.c, air_intrinsics.c, nir_air.h) moved past it, and Gallium's --m5c rows test none of that. air_metal.c's new refusals of an array of descriptors are syntax-checked only. **Before that library goes into any system image it needs its own rebuild and full regression, because it is what the installed system runs on** (and OpenGL ES with the new shim, tools/mk-mesa.sh's warning stands).
  **To do after M5b's runs:** [done in the M5c build, row mtl-iosurface-swizzle] -newTextureWithDescriptor:iosurface:plane: must honour a descriptor swizzle on vk. Real Metal does
  (measured on an M1 Max: an L565 surface, B5G6R5Unorm, word 0x001F reads blue with no swizzle and red with B,G,R,A, by read
  and by sample; a view likewise; BGRA surfaces likewise). Gallium has no views and stays unswizzled, so the metal=2 Gallium
  boot's picture does not change; the log line keeps reporting what was asked. Reading that boot: an L565 wallpaper surface
  with no swizzle anywhere means the bytes themselves had red low, and the question becomes who wrote them (Apple's CPU
  code, or our GPU through a 16-bit attachment under the old table; the usage field says whether it was a render target).
  (Aside, the owner's request of 2026-11-05 to dump hyp to the desktop: tool built, not run, shelved by the owner
  ("這個再說吧"). userland/src/partdump.c (read-only, hyp_a/hyp_b only), out/hypdump-root.apfs, out/rc.jobs.hypdump: paced
  base64 lines joined from run_cam_probe's kept log copies, CRC per line, sha256 at the end. hyp is about 3.65 MB, by
  the kernel's survey identical on both slots.)
  **M4, first cost runs, 2026-11-05 (runs 071517 + 071937; GPU fixed at 525 MHz, cpufreq_gov=0, metal_drv_test
  3fcd97e10dc1195b; GPU time net of an idle baseline, since the kernel has no per-client GPU time).** Medians of 3:
  - W4, 200 frames of one full-screen draw: into an owned 1080x2400 texture, GPU per frame vk 429 us against Gallium
    753 (0.57x), wall 649 against 1014 us; into an imported IOSurface, vk 816 against 1005 us (0.81x). But CPU sys per
    frame is vk 521 us against 80-161; into the surface vk 278 ms (seen) / 219 ms (not seen) per 200 frames against 83.
  - W2 (271 draws) and W3 (AV1SW, PortraitFilters compute): net GPU about the same, but vk makes 7-40x more
    submissions (2021/278; 11.8k/349; 36.1k/895) -- air_vk submits uploads and read-backs on their own -- so its sys
    time is 4-13 s against 0.1-0.6 s. turnip's pipeline creation is about 4.3x Gallium's (1797 against 422 ms for 271).
  - **Bug found: SMMU faults on vk (run 071937).** 4 of 5 `--time` runs on vk faulted ("THE SMMU REFUSED the 3D core
    in a program's tables") right after 256 empty command buffers committed and dropped unwaited, then
    VK_ERROR_DEVICE_LOST; Gallium none; W1's vk numbers from those runs are invalid. Cause (from the code and the log; a
    pair recycled in flight was the first guess, and wrong): a second thread recording on the Vulkan device. An empty
    command buffer got no fence from rwa_flush, so hkmetal's background waiter fell back to mesa.finish, and the old
    rwa_finish began, recorded and submitted on the completion queue's thread with the main thread's pool and queue;
    -getBytes: and -replaceRegion: do not take rw_pipe_lock, so they collided. Fix (air_vk.c, libvulkan_freedreno
    9f625b63388cc528): only the thread that began a recording records or submits (device->recorder); wait_for_all
    backs rwa_finish and destroys and waits on the newest fence; rwa_flush with nothing recorded hands out that
    fence; pairs refcounted; vkDeviceWaitIdle at close. metal_drv_test --burst N is the test. **Verified on the phone
    (run 080405):** --time on vk 0 of 5 faulted (4 of 5 before), --burst 256 ok on all 6, no recovery or device loss,
    nothing in flight at any process's end, the imported-surface draws still right. W1, now valid (median of 5 vk, 1
    Gallium): wall 2471/2523 ms, cpu sys 677/584, net GPU 276/272, submissions 418/652, pipelines 385/225.
  - **Why vk's CPU sys is high (read from the code, 2026-11-05):** (1) every vk wait is a busy poll: turnip's and
    hk_drm's syncobj deadlines are on CLOCK_MONOTONIC (date minus boot date on Darwin) while HKGPU compares against
    mach_continuous_time (from SoC power-up, tens of thousands of seconds ahead), so each wait is "over before it
    began" (log 061800:7270), returns at once and is re-asked in a loop for the whole GPU time; freedreno's patch had
    already moved Gallium to CLOCK_MONOTONIC_RAW, so Gallium sleeps. (2) air_vk's texture writes and every destroy
    submit and wait on their own (the test writes textures row by row: 7-40x the submissions). (3) staging buffers
    made and closed per call. (4) turnip names every live BO in every submission; per BO the kernel's work is O(1),
    the only O(size) part being the seen surface's clean (~37 us/MB, under gGPULock).
  - **Must be fixed before metal_backend=vk goes into the system:** a narrow race left by M4-1: a recording stopped at
    an event's wait can resume on another thread in the instant the first thread writes or reads a texture it named.
    -replaceRegion: and -getBytes: need rw_pipe_lock in hkmetal.m (documented at air_vk.c record_begin). Also: a GPU
    job over 5 s now hits the kernel's "no answer" and recovery on vk too, as it always has on Gallium.
  - Next (M4-1): the SMMU-fault fix; the wait clock (hk_drm fence_wait on CLOCK_MONOTONIC_RAW with a back-off;
    turnip-darwin.patch tu_knl_drm.h to CLOCK_MONOTONIC_RAW); deferred texture writes (an upload command buffer ahead
    of the current one) and deferred destroys; a staging ring. Kernel round: patience in us, not whole ms.
    Agreed details (the supervisor, 2026-11-05): the deadline from mach_continuous_time() itself (HKGPU.cpp:505's
    source), in hk_drm and in the turnip patch, not from whichever clock id happens to match; the ETIMEDOUT back-off
    kept as a last guard; gpu_irq stays 0 for M4-1 (comparable with run 071517), gpu_irq=1 a separate run; the
    upload command buffer on the same queue, ahead of the user's, with a barrier, so a CPU write read at once by a
    blit or draw stays in order; deferred destroys share the SMMU fix's "recycle only once the fence signalled"
    mechanism; hk_drm.c built both ways (turnip's copy rebuilt, Gallium's compiled to check it still builds).
      - **Design input for the kernel round (the supervisor, 2026-11-05):** cleaning every seen surface at every
        submission does not scale -- turnip names every buffer of the process each time, so one full-screen seen
        surface costs a 10 MB clean per submission. The kernel should clean/invalidate only surfaces the CPU has
        touched since the last time (by the seed, dirty tracking, or the window a CPU mapping is open), and
        invalidate after the GPU writes only what the CPU will read (IOSurfaceLock(ReadOnly), or mapped surfaces).
      - **Also for the kernel round:** a wait's patience counted in us or rounded up (millisecondsUntil rounds down to
        whole ms, so the last <1 ms of every wait is polled); and the seen surface's clean (10 MB, ~390 us) not done
        whole under gGPULock -- it blocks every other process's submissions, backboardd's included -- but in pieces or
        outside the lock.
      - **And (2026-11-05, read during the SMMU-fault work):** every GPU address space uses ASID 0, and ring_table
        (hk_adreno.c about line 3839) switches spaces with no wait-for-idle before it; and a GPU recovery makes every
        pending fence read as done (hk_adreno.c about line 4280), so an application takes work that was given up for
        success. The SMMU fault line prints only address, FSR and FSYNR0: it cannot name the task or the buffer.
  - **M4-1, the cost part: built, NOT run on the phone (2026-11-05).** libvulkan_freedreno cba626e5552d5aa9, metal_drv_test
    c28bc6e6687e45c4; hkmetal.m unchanged, Gallium's library not rebuilt. Jobs out/rc.jobs.m4r1 (--order, --burst, regression, W4, W3),
    m4r2 (W2), m4r3 (W1); fixed clocks as run 071517, gpu_irq as it was.
    - The wait clock: hk_drm.c fence_wait and the Mesa tree's tu_knl_drm.h (get_abs_timeout, turnip-darwin.patch, 372 lines) take the deadline
      from mach_continuous_time() itself (hk_drm_gpu_now_ns, xf86drm.h); more than an hour is an hour a request (HK_DRM_LONGEST_WAIT_NS: the
      kernel multiplies seconds by 10^9 in 64 bits signed, and "forever" wrapped negative); a back-off of 50-200 us on ETIMEDOUT with time left.
      Checked on the host against a model of kMsmWaitFence (20 ms: one request; forever: two in 7 s; 3 ms never done: six). Gallium's copy
      compiled with its own command: 27 functions of 31 the same, three differ in one static's offset, drmSyncobjWait changed and nothing in
      gallium/freedreno calls it. Not fixed, not reached by air_vk (no timeline semaphores, no queries): tu_timeline_sync_wait's
      pthread_cond_timedwait with a CLOCK_MONOTONIC abstime on Darwin's realtime condition, which would spin; vk_sync_timeline's condition.
    - air_vk.c (8.6k lines): an upload command buffer, its own pool, under upload_lock with every vkQueueSubmit, one submission with the recording
      and ahead of it (UPLOADS, at upload_begin): writes and read-backs from any thread never touch `current`. A write waits only with no defer_waits
      or over an IOSurface. Destroys and an evicted pipeline: when_done, the last recording and upload that named the object (struct rwa_use), freed
      when reap sees that fence signalled: one list of pairs on the queue for pairs, objects and the ring. THE STAGING RING: 4 MB at first use; over 1 MB
      a buffer of its own; full: the upload is sent and the oldest holder waited for.
    - Open by reading: one narrow race, a recording stopped at an event's wait, resumed on another thread in the instant the first writes or reads a
      texture that recording named (record_begin's comment). rw_pipe_lock around -replaceRegion: and -getBytes: would close it. The kernel's
      "no answer" and recovery now apply to vk waits as they do to Gallium's: a wait of a second or more, asked with the right clock, can reach them.
  - Observed: CPU reads after GPU writes are stale on both back ends (the kernel item under T38 (17)): Gallium BGRA8
    6384/6400 stale with a 5.1 ms gap, RGBA16F 1728/4096 at 2.5 ms; vk 1080/6400 at 0.5 ms, 616/4096 at 0.3 ms;
    1080x2400 none (22 ms).
  **M1 built, 2026-11-05:**
  - turnip builds and links for iOS in a second build dir in the container: xnu-deps/build/mesa/build-turnip, same
    tree and cross.ini, `-Dgallium-drivers= -Dvulkan-drivers=freedreno -Dfreedreno-kmds=msm -Dopengl=false
    -Dgles2=disabled -Dosmesa=false -Dshared-glapi=disabled`, the rest as gallium's. It needs glslangValidator
    (glslang-tools, installed in the container). The result is libvulkan_freedreno.dylib (73c6e36f); its only
    unbound symbols are the entrypoint tables' weak references.
  - tools/mesa/turnip-darwin.patch, applied after freedreno-darwin.patch:
    - meson: the libdrm stand-in also for turnip, with HAVE_LIBDRM for the runtime's device list;
    - turnip's three maps and unmaps through hk_drm_map/unmap;
    - no pthread_condattr_setclock on Darwin;
    - `-undefined dynamic_lookup`, because Mach-O does not leave weak references NULL as ELF does.
  - tools/mesa/hackios-drm:
    - the connection opens on the first request;
    - drmGetDevices2/drmGetDevice2 list the one GPU, with /dev/null as its render node;
    - binary sync objects are kept in the process: one in-order ring, so a sync object is the fence of the submission
      that signals it, and a wait is the driver's fence wait;
    - GEM_SUBMIT's MSM_SUBMIT_SYNCOBJ_IN/OUT are taken off before the kernel call and applied after.
    - DRM_CAP_SYNCOBJ says 1 only in turnip's copy (HK_DRM_SYNCOBJ). gallium's copy still says 0, so its fences do
      not change. The next gallium build (mk-mesa.sh copies hackios-drm every time) carries the new shim:
      check OpenGL ES once before installing it.
  - userland/src/vk_test.c: the driver opened by its ICD entry; the instance, the Adreno and its memory types; a
    device; a 1 MB host-coherent buffer; two vkCmdFillBuffer (whole, then the middle half), each with a fence, and
    every word read back. Test root out/vk-m1-root.apfs (rc.jobs.vk). No kernel change.
- **(20) fps / smoothness, after HDR (the supervisor's note from the Metal assessment):** the GPU idles 75-83% of the
  time at its lowest level (305 MHz), while each submission costs ~490 us in the kernel and glFlush 4.6 ms median
  (docs/subsystems/gpu.md): the kernel's submit path and the GPU clock policy (`gpu_dcvs=0` in the installed args)
  are the bigger levers for smooth frames, not the API. Also: TE averages 11375 us (87.9 Hz) against 11111 at the
  panel's start (run 204007); frame pipelining / an extra buffer.
  **HDR content:** the owner's two HDR HEICs (out/hdr-photos/, untracked, test roots only). A file in DCIM is not
  imported; SpringBoard's PhotoKit import (inject_sb.c, boot-arg photos_import=1, from /hackios/import) fails with
  PHPhotosErrorDomain 3301 on a fresh RAM /var (run 210107) and on the kept UFS /var, which ufs_write=0 makes read-only
  (init: "creating a file gives EROFS"), so no library can be written there in a RAM run. Next: the import on ramhfs after
  the library is ready (or assetsd's own logs), or another HDR source.
- **(19) Settings > Safari does not open** (the owner, 2026-11-04 20:0x, run 200057: "safari設置打開失敗"). The log has only
  Preferences' scenes going foreground/inactive, no crash line: Preferences' own os_log is not in the log. Next: a run with
  OS_ACTIVITY_DT_MODE for Preferences (as wifi-diag-root does for SpringBoard) to see why MobileSafariSettings.bundle fails.
  Also: SpringBoard refused `prefs:root=SAFARI&path=ADVANCED` (battery_test open). Since: run flag `prefslog`
  (applog=1 applog_name=Preferences, kern_exec.c's aid) puts Settings' own os_log on the console (run 212043); SpringBoard
  refuses prefs:root=SAFARI too; Settings' list flings ~4400 px on a normal swipe, slow drags (1.5 s) do not:
  out/rc.jobs.item19b puts the Safari row near y 0.45. Run 212539's tap was overlapped by a hand on the phone: redo.
  **Parked** (the owner, 2026-11-04 21:3x: "safari先別管，我們先管HDR").
- **(15) the slow boot from the flash: done 2026-11-04** (the loader's log off the screen; HANDOFF, docs/INSTALL.md "Updating the installed loader").
- **Owner, 2026-11-04 08:4x / 08:5x** (relayed by the camera supervisor session): these are to be fixed **once the camera is
  done**, not before ("這些不要立即做但是後面相機搞好了要修復"; "還有之前做的sensor也要"). "The camera done" means T07 through a
  working Camera.app (`cam=5`): the list's item 4, "no Camera app", is T07 itself. Symptoms and the supervisor's first leads
  (unverified): the owner's list `相機完成後待修.txt` (outside the repository, with the supervisor session's outputs).
- **The items:** (1) the Wi-Fi list misses networks on refresh, no signal bars in the status bar, 5 GHz networks seconds late (T18,
  `hk_apple80211.c`); (2) no sound and no vibration (T08, T24; the owner allows audible and vibration tests); (3) the wallpaper cannot
  be set (K15 / T22: the poster switcher draws black); (5) choosing Bold Text in Settings hangs; (6) the clock about 2 s behind (the
  PMIC RTC's 1 s resolution, `xl settime`'s offset, no network time?); (7) screenshots do not work when booted from storage (VolUp +
  Power; the save to /var); (8) the display stutters after long use, more with background work (`gpu_dcvs=0`, `gpu_sleep=0`,
  cpufreq) -- **deferred until after (12) Metal** (the owner, 2026-11-04 19:5x: "8放在metal完成后测"); the owner's hypothesis:
  Metal work handed to the GPU contends with the compositor ("1我觉得是metal 丢给gpu"); to measure then: GPU work by client
  (HKGPU's five-second lines are IOLog, which the loader's log does not get: switch them to loader_log_line), the compositor's
  wait for the GPU while a Metal client is busy, and whether background daemons start Metal work after a while. Prepared:
  out/rc.jobs.item8 (eleven rounds of gestures, 4 utility-QoS spinners in rounds 5-8), run flags touchlat and secs=N; (9) sensors: auto-brightness (ALS), rotation, proximity (T04; the ALS agent stopped at an empty SUID list for
  ambient_light -- check it is not still working on SLPI first); (10) GPS (T14); (11) the display not at 90 Hz: "我用瀏覽器測試只有
  45hz" (the owner, 2026-11-04, on the installed bundle 1793719359: `fb_hz=90 gpu_dcvs=0 gpu_pt=0 gpu_irq=0 gpu_recover=0`) --
  probably item 8's cause too. **Leads from the camera runs (2026-11-04, unverified as causes):** during Camera.app's preview
  MiniIOMobileFramebuffer counted 35-54 frames a second and over half the swaps "a refresh late" (run 115246; display.md: one
  buffer measured 43-45, two up to 85-87); on the home screen with nothing moving, the widget extensions took ~1.2 cores between
  them -- CalendarWidgetExtension 9.9 s in 20 s, Weather 4.1, Stocks 3.1, WorldClock 2.4, GCWidgets 2.2, chronod 2.2 (run 114902,
  `power_test top`) -- and 15.7 s in 20 s with the camera on (114535). Still to measure: the home screen still and scrolling.
  (12) Metal (the owner, 12:3x: "GPU Metal層也要作為待做清單"): that is T10 -- the system's Metal off by default, no third-party
  Metal app tried, mesh/tessellation/ICB/tile shading/indirect draws missing, no event across processes; the camera's
  mediaserverd takes its Metal device out and uses OpenGL ES (camera_gpu.c). Bears on (13) and on the smoothness of (8)/(11).
  (13) Third-party apps (IPAs: Telegram, WeChat; the owner asked about 爱思助手 too): the AMFI and Sandbox stand-ins let anything
  through (K11) and the apps are arm64, so an unencrypted app should run; App Store, Apple ID, push and FairPlay are not to be had
  (K11, T15), so a store-bought or "licensed" IPA stays encrypted. 爱思助手 needs Apple's usbmux/lockdownd/pairing/
  installation_proxy over USB, which a normal boot does not present -- and its apps would stop at FairPlay anyway: much work,
  little use, not recommended. The supervisor's proposal: an installer on the phone (an .app or unencrypted IPA brought in by
  `xl` or Wi-Fi, registered through installd or LaunchServices), Telegram first (open source, built ourselves); a decrypted WeChat
  IPA is the owner's own matter (legal and terms-of-use questions): we do not help obtain one.

## P2

### T14 Location (GPS) — Connectivity · Not Started · P2
GNSS belongs to the modem (T06); location via its QMI service 16. `locationd` denied. **Blocked by T06.** Refs: K12.

### T15 App Store, Apple ID, FairPlay, push — Services · Blocked · P2
- **Facts:** App Store LOADING for ever: AMS builds a bag request but the promise never resolves; an AMS thread waits on `fairplayd.H2`, which exits 0 immediately every 10 s
  without touching IOKit (decision made with inlined syscalls). `apsd` runs but has no push courier; `mobileactivationd`: "Device is not activated". No Secure Enclave.
- **Next:** find what `fairplayd.H2` decides on (Ghidra path in `inject_fairplay.c`; kernel `systrace` extension to print sysctl MIB / mac-syscall policy). Probably unachievable without an SEP stand-in.
- **Refs:** research/ios-userland.md §4; K11.

### T16 Brightness: prove the light changes — Display · Partial · P2
- **Works:** the path from `BKSDisplayBrightnessSet` to DCS `51 hi lo` (20/90/5/100% → 0x52/0x67a/0x6/0x7ff), re-sent after wake, default restored on loader return.
- **Missing:** optical confirmation, byte order (no DSI read), auto-brightness (T04).
- **Next:** owner watches the screen during `tools/t brightness`; or implement a DSI read of the panel's brightness register. Record.
- **Refs:** [display.md](subsystems/display.md); K09.

### T17 CPU after wake: widgets and chronod — Performance · Partial · P2
After each wake Weather/Calendar/Stocks widget extensions and chronod burn ~6 s CPU in 14 s (SwiftUI timeline entries). `vmd` (40% of a core) is fixed. Next: profile (`hkprof=2`, `tools/t wakeprof`), consider
denying chronod's widget extensions at wake or throttling. Refs: K10.

### T18 Wi-Fi gaps — Wi-Fi · Partial · P2
Not done: 802.11r (iOS is told FT is absent), TKIP pairwise, roaming, WPA3 other groups and password identifiers, Bluetooth coexistence. Built, not seen on the phone: private Wi-Fi address,
AKM 6, RSNE/RSNXE downgrade check, WPA3 transition mode. Auto-join across reboots needs a persistent `/private/var` run (`var_store=1 ufs_write=1`, or installed boot) — the
Keychain itself works. Files: `wifi/lx/src/lx_wpa.c`, `lx_sae.c`, `lx_80211.c`, `hk_sta.c`. Refs: [wifi.md](subsystems/wifi.md); K05.

### T37 Driver audit follow-ups (2026-10-27) — Drivers · Partial · P2
From the audit against OnePlus's sm8250 kernel and mainline (HANDOFF "Driver audit fixes"). Open:
- `i2c_geni=2` (I2C serial engines, FIFO mode; `instantnoodle_i2c.c`) verified at boot on touch/gauge/slider; **next:** a manual run with a finger
  on the screen (`i2c_geni=2 touch_lat=1`), then make it the default; try the amplifiers' bus once with `adsp=1 sound=4 sound_amps=0`.
- Not yet seen on the phone: touch error paths (ESD, queue full, own restart), the audio DSP's RPMh claim (`adsp=1`), Wi-Fi PTK renewal (needs an
  AP with `wpa_ptk_rekey`), thermal throttling with the SM8250 map (T33), whether the haptic trim feels better.
- Merging the duplicated command-database parsers / bit-banged I2C / DSP stack: T29. Kernel log loss after a run: K25 (loader).

### T19 Fast charging (Warp / USB PD/PPS) — Power · Not Started · P2
OnePlus's fast-charge MCU (I2C `0x26`) protocol and firmware unknown; max here is the normal charger's 2000 mA (when T01 verifies the ICL). Redwood reads the charger but does not drive PD/PPS. Refs: K08.

### T20 CPU idle power collapse — Power · Not Started · P2
Awake idle is WFI (clock gated, power on). `CPU_SUSPEND` power-down (`0x40000004`) needs a resume path that would run thousands of times a second; QEMU cannot test it; risk = random hang. Do only with a measured benefit (T13).

### T21 GPU IFPC and measured GPU power — GPU/Power · Blocked · P2
IFPC needs the GMU power config and an OOB handshake around every host register access; mainline does not do it for this GPU either. hwcg is on but the power saved is unmeasured (blocked by T13).

### T22 CoreAnimation fidelity gaps — Graphics · Partial · P2
- `sb3` (function 40, single-pass blur: shadows under cards of the app switcher) is refused by the OpenGL ES renderer; `glsb3=1` hands a shader but still "refused x1".
- Poster switcher (long press on lock wallpaper) draws black; `poster_switcher_allowed()` hides it; cause unknown.
- Control Center glyph strokes at opacity < 1 come in only at the end (`cc_fade` is a check-only workaround, off).
- Switcher wallpaper wash uses a placeholder image.
- Notification banner material (`MTMaterialLayer`: `luminanceCurveMap`, `colorSaturate 2.4`, `gaussianBlur 29.5`) not compared with a real iOS.
- Mesa shader cache disabled at build.
Files: `userland/src/eagl.c`, `inject.c`, `inject_sb.c`. Refs: research/ios-userland.md §1-2; K09.

### T23 Display late swaps and cold-start jank — Display · Partial · P2
~2% of swaps a refresh late (one push in flight); first gesture set after boot ~7% late (cold); unlock-after-wake blur transition 14 ms GPU/frame; idle home screen still ~30 fps drawn. Files: `MiniIOMobileFramebuffer.cpp`, `eagl.c`. Refs: display.md.

### T24 Speaker protection (TFADSP) and other microphones — Audio · Partial · P2
TFA9874 protection runs in Android's audio DSP (AFE module `0x1000B911` on `TERT_MI2S_RX`, VI on `TERT_MI2S_TX` `0x1000B912` topology `0x1000B909`, container messages via
`AFE_PARAM_ID_TFADSP_RX_CFG`); here a software guard stands in. Only the main mic is set up. Amp health polarity only via MANALARM/MANSTATE. Refs: audio.md; K04.

### T25 Video / Live Photo recording; camera parity — Camera · Not Started · P2
README lists video and Live Photo recording as not working (redwood). No encoder path is built (software HEVC decode only is known: `VCPHEVC.videocodec`). Depends on T07 for OnePlus.

### T26 Storage — Storage · Partial · P2
Transactions for a writable APFS (the in-place write and the slot store exist; `tools/apfs/txn-probe.py` measures why transactions are hard); no `fsck_hfs`; HFS write speed limited by the loader's read-back;
`ufs_native=1` driver is read-only and used on redwood only; Block I/O 2 gives no asynchrony. Files: `apfs_write.c`, `hk_ufs.c`, `hk_ufshc.c`. Refs: [storage.md](subsystems/storage.md).

### T27 Boot menu usability — Boot · Partial · P2
5 s countdown too short to choose Developer mode by hand; text console only ("the loader's own graphics are a later step"). Files: `xnuloader/flashboot.c`. Refs: INSTALL.md.

### T28 Security model — Security · Partial · P2
AMFI and Sandbox stand-ins accept everything; `MiniAppleKeyStore` models a never-locked no-passcode device and protects nothing; no Secure Enclave; no passcode support; kernel runs whatever
it is given. Documented honestly as stand-ins ([kernel-standins.md](research/kernel-standins.md)). Not a roadmap item to "finish" unless a security goal is declared.

## P3

### T29 Remove stand-ins and copies — Maintenance · Partial · P3
`hk_wlfw.c` ("A STAND-IN, TO BE THROWN AWAY when ath11k's own qmi.c runs here" — it does now with `wifi_lx`); `hk_adsp.c` is a copy of `hk_wifi.c`'s approach ("a copy, not a shared library, for now");
`hk_haptic_pretend()` test stand-in; the RPMh command-database parser is copied in 8 files (`hk_adsp`, `hk_cpufreq`, `hk_speaker`, `hk_adreno`, `instantnoodle_touch`, `hk_camera`, `hk_goodix`, `hk_wifi`) and bit-banged I2C in 4 (`instantnoodle_i2c`, `hk_haptic`, `hk_speaker`, `hk_camera`): merge into one each (the TCS 0 race they caused is fixed by `hk_rpmh_claim()`, 2026-10-27); `hk_sparse.c`/`apfs_check` bring-up aids; `hk_hfs` fetched at build time.

### T30 Redwood re-verification and parity — Redwood · Not Started · P3
The owner said not to run POCO checks. Redwood lacks `gpu_pt`, DCVS, fence IRQ, system sleep, interrupt-driven touch; dual boot is verified only in QEMU. Needs an explicit request.

### T31 Documentation and repository debts — Docs · Partial · P3
`docs/metal.md` is cited 7x by `air.md` and exists nowhere (write it from the source comments or fix the cites); 43 `kern.hackios.*` sysctls undocumented individually; 49 boot-args only described by
file headers; no CI (removed); `uefi` rebuild-from-scratch unverified; `userland/build.sh:8` and `init.c`/`var_store.c` still say `/redwood`; kernel log lines still prefixed `redwood:` in userland;
`ios-instantnoodle/` copy-pasted READMEs (sweet/guacamole); two-repo index overlap (see architecture.md). Also: `kern.hackios.*` and `rc.jobs.*` indexes.

### T32 Scripted gesture reach and test coverage — Testing · Partial · P3
Scripted touches do not reach iOS system gestures (swipe-up for home/switcher) except from the real bottom edge path; `com.hackios.switcher` returns but changes nothing with 3 apps. `tools/t` covers boot/pods/frames/sleep/powerd/darkwake/brightness/idle;
no automated test for audio, Wi-Fi join, GPU hang recovery (manual `rc.jobs.*`).

### T33 Thermal service never seen throttling — Thermal · Built, Unverified · P3
27 sensors answered at 33-38 C on redwood; the governor has not slowed cores on either phone (needs >95 C load). Next: a controlled heat test with the owner. Refs: thermal.md.

### T34 NFC, UWB, other hardware — Hardware · Not Started · P3
`nfcd` denied; Wallet UI exists without NFC. Nothing investigated.

### T36 iphont agent hardware validation and workflow migration — Tooling · Partial / Built, Unverified · P1
The Python CLI, native loader/kernel mailbox, event logging, userspace bridge, diagnostics, input scheduling and capture interface are implemented and compile. Host/C tests and real XNU QEMU queries pass; physical state/log/framebuffer capture works. The owner explicitly deferred the required RAM loader reload, so USB RPC, daemon/framework/entitlement behavior, touch/keyboard delivery, and full deployment/recovery parity remain unverified. Normal boot profiles are unchanged. Next: use the supervised RAM sequence in [TOOLKIT.md](TOOLKIT.md), then validate one operation at a time. Still unfinished: GPU queue-level introspection, delivered keyboard guarantees, orientation/sensor support, KDP/LLDB integration, high-rate compressed mirroring, userspace function symbols, service-specific regressions and complete legacy replacement. Keep all unique recovery/test/build scripts; [TOOLKIT-MIGRATION.md](TOOLKIT-MIGRATION.md) records the static inventory and deletion gates.

## Complete — do not reopen (older documents made these look unfinished)
- Speakers on the OnePlus 8 (heard by the owner 2026-10-24); the alert slider; the main microphone (owner's claps); App Library grey pods (fixed 2026-10-25); iOS shadows "too large" (owner: resolved);
  `ca_ambient` idle cost; `vmd` spin; shared-interrupt-after-wake bug; GPU block collapse in sleep (verified 2026-10-26); per-program GPU page tables; hang recovery; cold `gpu=1`; installed boot of the OnePlus 8 (verified 2026-10-24);
  Wi-Fi join incl. WPA3 and TKIP-mixed on the OnePlus 8 (verified in RAM runs); HFS+ `/private/var` (verified); Metal fences/events/heaps (metal_sync_test 10/10).
- The README's old "alert slider: no driver" and "microphones: not yet" were stale; "Wi-Fi: a network cannot be joined from Settings yet" in `userland/build.sh`'s WIFID comment is stale too.

## Source-comment markers that are real work (inventory)
`hk_wlfw.c` stand-in; `hk_adsp.c` copy; `apfs_write.c` ("no caller in the kernel yet"); `instantnoodle_slider.c` interrupts not used (polled); `instantnoodle_speaker.c`/`hk_speaker.c` "what is not done compared with Android"
(DSP algorithm, boost current limit by battery); `mc_aes.c` no XTS/GCM; `MiniIOSurface` child surfaces/wrapping/purgeability/compression/notifications; `hkmetal.m` ~40 features; `GenericARMPE.cpp` no RTC driver (time from the loader / `xl settime`). The code has essentially no `TODO`/`FIXME` markers: unfinished work is written as prose in headers.
