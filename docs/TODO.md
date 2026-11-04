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
- **Owner, 2026-11-04 08:4x / 08:5x** (relayed by the camera supervisor session): these are to be fixed **once the camera is
  done**, not before ("這些不要立即做但是後面相機搞好了要修復"; "還有之前做的sensor也要"). "The camera done" means T07 through a
  working Camera.app (`cam=5`): the list's item 4, "no Camera app", is T07 itself. Symptoms and the supervisor's first leads
  (unverified): the owner's list `相機完成後待修.txt` (outside the repository, with the supervisor session's outputs).
- **The items:** (1) the Wi-Fi list misses networks on refresh, no signal bars in the status bar, 5 GHz networks seconds late (T18,
  `hk_apple80211.c`); (2) no sound and no vibration (T08, T24; the owner allows audible and vibration tests); (3) the wallpaper cannot
  be set (K15 / T22: the poster switcher draws black); (5) choosing Bold Text in Settings hangs; (6) the clock about 2 s behind (the
  PMIC RTC's 1 s resolution, `xl settime`'s offset, no network time?); (7) screenshots do not work when booted from storage (VolUp +
  Power; the save to /var); (8) the display stutters after long use, more with background work (`gpu_dcvs=0`, `gpu_sleep=0`,
  cpufreq); (9) sensors: auto-brightness (ALS), rotation, proximity (T04; the ALS agent stopped at an empty SUID list for
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
