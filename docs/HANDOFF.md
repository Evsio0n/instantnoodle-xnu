# HANDOFF — start here when you begin a new session

**The owner's roadmap (2026-11-04, relayed by the camera supervisor): "roadmap修相機 ->其他 -> 最后trustzone和Titan 480 IFE".**
(1) the camera: the three back cameras, zero-copy, the installed bundle, the owner's own check (T07); (2) the rest: T38 (the
owner's list, items 1-13); (3) last: TrustZone (FairPlay, the Secure Enclave: T15) and the Titan 480 IFE's full hardware ISP.

## OnePlus 8 cameras (T07) — 2026-11-04 ~00:45, Claude session (parallel to the ALS one)

**16:4x-16:5x installed camera bundle: the Camera icon (MobileGestalt cache) and Camera opening (/var made anew) -- verified on phone in RAM runs against the phone's /var (the owner: "有相机了", "相機工作正常"), then on the
installed slot 1 alone, unplugged (2026-11-04 ~17:00, the owner: "已安裝版，Camera 正常") -- the camera goal accepted**
Installed (the owner's OK): boot slot 1 = gen 1793779669 "IN2010 Camera 0.5x 1x 2x" (kernel 000b0447..., root
`out/camera-install-root.apfs` 9c9635ff..., cmdline `out/camera-install-cmdline.txt`: the stable args + gpu_pt=1 gpu_recover=1
cam=5 cam_after=10); slot 0 still the stable 1793719359. Its first boot (the loader's log) came up from slot 1 with the camera
driver serving, but no Camera icon: every camera test had a new ramfs /var (ufs_write=0), the installed boot keeps the HFS
/var, whose MobileGestalt cache (`/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/Library/Caches/com.apple.MobileGestalt.plist`)
was made by the camera-less stable bundle and is trusted while the build is 20F66. Fix: `userland/src/init.c` `camera_answers()`
(stamp `/private/var/.hackios-camera` "isp=N flash=N" from cam=5/torch=; when it differs, the cache is renamed to `.hk-old`
before iOS starts; host-tested both ways). Run 163941 (`run_cam_probe.py 5 '' wifi gpuiso varwrite exctrace`,
CAM_IMAGE=`out/camera-mgfix-root.apfs` a126d667 = camera-install-root with only /sbin/init 1fd43f29 changed; ufs_write=1
against the kept /var, the owner's OK) renamed the 8099-byte cache and wrote the stamp isp=1: Camera.app showed.
But Camera did not open on that kept /var (the owner: "但是相机打不开"): each tap started Camera and its capture session,
the session was taken down ~0.1 s later ("stream: stop (0 frames)", "TimeMachineResumeNow", handlers taken away), Camera
exited with no trap (exctrace=1) and was started again by the next tap; the cause in the kept /var was not found.
**Fix (the owner: no data to keep, "可以reset"):** `init.c` `var_data_format=2` (makes the volume anew even when one is
there; =1 only where there is none). Run 164952 (`run_cam_probe.py 5 '' wifi gpuiso varwrite varformat exctrace long`,
image `out/camera-mgfix-root.apfs` cf335620, init 09d2fc07): newfs ok, the template in, stamp isp=1, 49 checks 0 failed;
Camera opened and streamed (first frame 1269 ms after the ask, 110 frames, 110 pictures given). **Never leave
var_data_format=2 on a command line that boots again** (it wipes /var each boot).
(The `.hk-old` cache went with the format.) **Not installed yet:** the bundles in both slots have the old init (no stamp):
booting slot 0 again would make a "no camera" cache that slot 1 cannot undo; the next install should carry this init.
The owner: "相機工作正常" (16:5x). **Next install** (when there is a reason to install again, not now; the owner's OK each time): root `out/camera-mgfix-root.apfs` cf335620
(= camera-install-root + this init), the same kernel 000b0447 and `out/camera-install-cmdline.txt` (var_data_format=0).
Uncommitted from this step: `userland/src/init.c` (userland repo: camera_answers, var_data_format=2), docs/HANDOFF.md,
docs/TODO.md (T07), docs/subsystems/storage.md, `../oneplus8-audit/camera-native/run_cam_probe.py` (`varwrite`, `varformat`);
the camera work before it is listed in the 15:33 entry below. Logs 163941, 164952 (`logs/`).

**15:33 runs 141639-152922: the IMX481, the 0.5x, white highlights, faster camera switches -- verified on phone, RAM only**
(kernel BUILD_CAM 15:33 SHA b13f7c71..., exported; defaults: cam_quickmode 1, cam_focus 2, cam_focus_ultra 2,
cam_cci_burst 1; userland/tools uncommitted; images out/camera-app-root(.apfs), out/camera-ios-root(.apfs) are test images).
(1) IMX481 ultra-wide: CSIPHY 3 (0xac70000; camcc csi3phytimer RCG 0x6060, branches 0x6078/0x607c), 4 D-PHY lanes, mode 0
4656x3496 (its whole field; modes 1/2 are crops), step 5's 20 MB frame from a 24 MB piece taken at boot (`ife_early`), cam=5
sensor 3 (HK_CAMERA_SENSOR a=3). AK7374 lens at 0x0c (position 0x00 << 6, woken 0x02<-0, parked to its rest 0 and put to
sleep 0x40; park range fix run 152342). (2) The 0.5x (on by default; `cam_05x=0` the iPhone X's 1x<->2x button):
mediaserverd: camera_device.c's third stream PortTypeBackSuperWide; inject_media.c adds a "Back Ultra Wide Camera" entry
(deviceType 7, built-in_video:5) to AVCaptureSession.plist as read (+[NSDictionary dictionaryWithContentsOfURL:] and
+[FigCaptureFlatPlist flatPlistWithContentsOfURL:]), forgets CaptureSourceInfo only without it, guards a NULL
_imageQueue in -[BWImageQueueSinkNode _collectUnconsumedBuffersWithReason:] (a SIGSEGV at Portrait->Photo); Camera:
inject_camera.c sets CAMCaptureCapabilities' ivars after init (ultra-wide yes, telephoto/dual/wide-dual/triple no,
switch-over 2.0), -zoomControlSupported YES, a 2x zoom point: ".5 1x 2" (buttons at x 0.40/0.50/0.60, y 0.663), 0.5x the
IMX481, 2x the IMX586's mode 6 in good light. The IMX481 not `turned` (its init writes 0x0101 0; run 151700 upside down).
Portrait still offered. (3) Highlights: white balance applied apart and each channel clipped at WHITE x min(lsc x wb) before
the colour matrix (CPU pixel() and the GPU passes): no pink -- the owner tested it themselves: "粉色我自己測了，修復了已經". (4) CCI:
consecutive registers in one write (`cam_cci_burst`): first frame after the ask 981 ms opening (2896), 489 ms 1x->0.5x (616),
1084 ms 0.5x->1x (2220); the bit-bang itself is ~37 us a bit (27 kHz). Next (supervisor's order): lazy power-down (keep
the sensor initialised in standby for a while), then a faster bit-bang for the camera buses only (instantnoodle_i2c.c has
another session's changes: add a per-bus field only), AE/AF starting from the last values; then the installed bundle (the
owner's own approval). Logs: each run's `.live.N` packed into `<stamp>-run.log.live.tar.zst`.

**14:10 runs 134921-140909: IMX586 autofocus, the three cameras' colour tuning, 2x by the light -- verified on phone, RAM
only** (kernel BUILD_CAM 14:02 SHA 3ff366e8..., exported: `cam_quickmode` and `cam_focus=2` the defaults; userland and
tools uncommitted). (1) Focus: the module file's embedded actuator (LC898217XC, 0x72 on CCI 0 master 0, DAC word at 0x84,
codes 50-930, init 0xe0<-1 then 0xb3 polled; 0xf0 reads 0x72, the position 0x200 before its start, so 50 is written
first): `act_up()`/`act_step()` (a move of at most 1/8 a frame, read back)/`act_park()` (before power_down(); on the way out
by hand, open drain, after a bus recovery, no lock). Run 135326: contrast AF sharp (braid and edges), parked 431->50 and
339->50; run 140301's logdump: "parked: from 452 to 50 (by hand, leaving)". `instantnoodle_i2c.c` not touched (another
session's uncommitted changes). (2) Tuning: `tools/camtuning.py` reads the OnePlus 8's V2.0.0 files (64-bit fields,
arrays padded to 8 bytes, lsc40; the first non-identity tree: the IMX471's 3rd) -> `out/camera-app-root/fw/camera-
{imx586,imx471,imx481}.isp` (vendor-derived: never in git). The GPU's cells pass does lens shading now (`isp_preview()` no
longer refuses it; 17x13 RGBA32F grid, texelFetch bilinear; `isp->lsc_window`: mode 6 the sensor's middle half), GPU vs
processor Y 0.015 apart; 19-21 ms a picture. Owner: the desk's cable "正紅色" -- the tuned red is right. (3) Mode 6 in
a dim room blotched magenta/green (16x gain, the CCM's ~1.9): 2x is mode 6 only in good light (exposure ms x gain: from
mode 1 below 25, back above 200 in mode 6), else mode 1's middle magnified (run 140909: no mode 6 in the dim room up to
4.75x). Logs: each run's `.live.N` packed into `<stamp>-run.log.live.tar.zst` after the run (supervisor; owner agreed).

**13:40 runs 131307-133840: the 1x/2x zoom button and a quick mode change -- verified on phone, RAM only** (kernel
BUILD_CAM 13:37 SHA 8e85eb8a... with the quick change behind `cam_quickmode=1`; since then the default, `cam_quickmode=0` off,
exported, **not yet rebuilt**; userland uncommitted). (1) The zoom button did nothing on a tap (owner): with only the wide
camera, CameraUI's single-camera-plus-toggle tap asks `-[CAMViewfinderViewController toggleBetweenZoomFactorsForZoomControl:]`
to change the *device* to the absent telephoto one. New `userland/src/inject_camera.c` (`libhackios-camera.dylib`, INJECT
"Camera", nothing off instantnoodle): the back camera's toggle sets 2x through `-[CAMZoomControl
_setZoomFactor:interactionType:shouldNotifyDelegate:]` (interaction 1: the viewfinder ramps); the app's own second tap is 1x.
(2) At 2x the preview stood still (owner: "一直不動"; the window server refilled no texture from an IOSurface): the frames'
`ZoomFactor` metadata said the picture's zoom (1.0) without mode 6's 2x; `camera_device.c` `mode_zoom()` now (run 133140: live,
three different screenshots). Mode 6 asked at 1.99 (a ramp may end below 2). (3) A mode change was a new session, 2.6-2.7 s
without frames; `ios_mode_change()` changes it in the session (off at a frame's end, the begun frame waited out, res table and
exposure in standby, on): 265-290 ms (run 133840, four changes, each step's time logged; frames keep their numbers; per-place
`mode`). `rwtouch pinch X Y D0 D1 MS` added (two fingers). Next (supervisor's order): IMX586 focus (actuator, HK_CAMERA_FOCUS),
colour tuning of the three sensors (camtuning.py), the IMX481, 0.5x, the installed bundle. Optional, in T07: mode 6 only in
good light (else mode 1 and a digital 2x: mode 6 needs ~4.6x the light, 19 fps in a dim room), a faster CCI for the res table.

**12:52 run 124917: Camera.app's Back Camera is the IMX586 -- verified on phone** (kernel BUILD_CAM 12:48 SHA 8a5f0df7...,
exported; userland media dylib rebuilt; mock 23/23): cam=5 switches sensors on HK_CAMERA_SENSOR, a preview of the desk upright
in natural colours, both directions of the switch, GPU = processor's, 20.6 ms a picture, no throttling, no death. Next: the 2x
crop (mode 6), a five-minute heat check with the 15 MB frames, then the IMX481.

**12:45 run 124047: the IMX586's first frame in memory and on the host -- verified on phone** (`cam=3 cam_sensor=wide
cam_step=5`): one 4000x3000 RAW10 frame, stride 5008 (3668 pages in one piece, only those mapped by the SMMU, guard pages
untouched), RDI 0 SOF/EOF, no overflow, no SMMU fault; `xl camera logs/cam3w-124047` -> `logs/cam3w-124047-0.png`: the desk,
cables, a Mac mini, correct geometry and Bayer order (RGGB), pale (no black level taken off, the mode's own exposure).
BUILD_CAM 12:40 SHA cd7ff773..., exported; mock 22/22 (X, Y). Next: iOS's Back Camera from the wide sensor (cam=5: a ring of
three 15 MB places, the kernel switching sensors on HK_CAMERA_SENSOR; userland's Back Camera back to sensor 1, not `turned`).

**12:40 run 122836: the IMX586's C-PHY packets reach CSID-lite 0 -- verified on phone** (35936 packets in 300 ms, ECC/CRC 0,
long packets of 5000 bytes; `cam=3 cam_sensor=wide cam_step=2`; BUILD_CAM 12:28 SHA 78f42d20..., exported; mock 20/20 with the
scratchpad's new W scenario -- patch_wide.py plus the 7-RCG arrays and two branch initial values). Next: the wide's step 5.

**12:30 the IMX586 (wide, Back Camera 1x) -- the plan.** The owner: the back camera and the installed bundle
before T38 ("做完後鏡頭和安裝"). Found: **redwood's `hk_camera.c` already has the C-PHY code** for the same CSIPHY v1.2.1
(`csiphy_common()`, `csiphy_cphy()` with csiphy_3ph_v1_2_1_reg's three trio blocks at 0x100/0x980, 0x300/0xa80, 0x500/0xb80 and
data_rate_delta_table_1_2_1's three rows, `csiphy_up()`'s lane enable: trio n at bit 2n+1; CSID RX_CFG0 bit 24 = C-PHY) -- to be
ported, not downloaded (a fetch of the Android header was refused by auto mode; WebFetch quotes no tables). IMX586 module
(`fw-camera/com.qti.sensormodule.semco_imx586.bin`, `camsensor.py ... -`): slave 0x34, lane assignment 0x210; mode 1 4000x3000
RAW10 3 trios settle 14, pixel clock 577.65 MHz, 30.1 fps, RGGB (~1.93 Gbit/s a trio: rate row 0); mode 6 the same size at
2000,1500 (the 2x crop); mode 4 2000x1128@240. CSIPHY0 at 0xac6a000 (DT; IRQ 0x1dd), its clocks camcc csi0phytimer RCG
0x6000 (300 MHz) + branch 0x6018, csiphy0 branch 0x601c, cphy_rx 0xa068 shared. **Step 1 (`cam=3 cam_sensor=wide
cam_step=2`):** `stream_mode()`/`stream_up()` by sensor (front unchanged: CSIPHY 2, 4 D-PHY lanes); for the wide one CSIPHY 0
C-PHY, CSID-lite 0 RX = lanes-1 | lane_assign << 4 | 0 << 20 | 1 << 24, res1; the RX packet counts, ECC/CRC 0, long packets of
5000 bytes. The mock (scratchpad) to model CSIPHY 0 first. Then the ring at 15 MB a frame (stride 5008), then iOS's Back Camera.

**12:15 runs 113459-121138: the GPU ISP accepted (supervisor) -- verified on phone, RAM only.** Five minutes of preview (120049):
30 fps, 58-67 C a minute, 0 throttling. kernel_task's "2 cores" with the camera on is mostly idle accounting (kprof 115633);
`gpu_irq=1` no effect (115246). Screenshot "bands" are `xl screenshot`'s own (display.md; `campattern=1` diagnostic). New guards in
userland: `inject_media.c` refuses a nil-output graph connection (the "Output can't be nil" SIGABRT of the owner's front/back
switch in Video mode, 120049; not repeated by script in 121138) and logs each still sink pipeline's source outputs;
`camera_gpu.c` gives up only after 5 slow pictures in a row. Run-script flags added (`../oneplus8-audit/camera-native/
run_cam_probe.py`): `gpuiso`, `gpuirq`, `campattern`, `long` (420 s). **Next: the IMX586 (3 C-PHY trios, CSIPHY 0) as the Back
Camera**, then the installed bundle (owner's approval; supervisor's conditions in T07). Uncommitted: as below, plus these.

**11:35 runs 110059-112756: the preview upright and made by the GPU -- verified on phone (RAM only, kernel 9fce7834 unchanged).**
Userland (userland repo, uncommitted): `camera_device.c` (the OnePlus 8's Back Camera `turned`; per-stage and render timing lines;
15 pictures a second at heavy thermal pressure; the GPU path in `deliver()` with a one-off comparison against the processor's
picture), `camera_isp.c` (magnified path: lines of cells unpacked once, column table, a cache per thread; `isp_preview()`),
**new `camera_gpu.c`/`camera_gpu.h`** (OpenGL ES 3 through `/hackios/lib/libOSMesa.8.dylib` in mediaserverd), `userland/build.sh`
(the media dylib's line only: `src/camera_gpu.c` added; the file had others' uncommitted changes, left alone). Run script flag
`gpuiso` (gpu_pt=1 gpu_recover=1). Host checks in the scratchpad (not in the repo): the old/new CPU render and the GPU file on
macOS OpenGL through a shim, at most 1 apart. 112756: GPU picture = processor's (≤1), 12-13 ms a picture, 30 fps, no throttling,
window server unharmed. The preview "bands" in screenshots are `xl screenshot`'s own (the owner saw none; display.md). **Next (supervisor, 11:4x):**
the IFE interrupt instead of 1 ms polling, the 5 MB upload zero-copy; target ~85 C, no throttling, 30 fps; then the IMX586.

**10:58 runs 104112 / 105351: Camera.app works on the OnePlus 8 -- verified on phone (RAM only).** Live preview, tap (focus
square), a 4032x3024 HEIC photograph, Photo -> Video -> Portrait -> Photo with no mediaserverd death (`logs/cam5-app2-3.png`,
`logs/cam5-app4-3.png` Video mode; some other frames of these runs show a person: logs/ only). Kernel unchanged (BUILD_CAM 9fce7834);
**userland only** (userland repo, uncommitted): `inject_media.c` replaces `-[BWMultiFilterThumbnailNode
prepareForCurrentConfigurationToBecomeLive]` (nil Metal filter -> SIGSEGV 10-14 frames into every session: runs 102330, 103046,
103420); `camera_device.c` takes only blocks as `PixelBufferHandler` and keeps them with `Block_copy` (the graph's unregister on a
mode change gave a non-block: SIGBUS in 104112, SIGSEGV in 104931), plus the earlier Back-Camera-is-sensor-2 / OnePlus tuning names;
new `userland/rc.jobs.camera-modes`. Built in the container to `out/cam5-media/` (not `userland/obj`). Diagnosis method:
`run_cam_probe.py 5 '' exctrace launchd` (flags added to `../oneplus8-audit/camera-native/run_cam_probe.py`), launchd's "exited due
to ..." in the live log, `usertrap:` lines only in the logdump, `../ipsw-tools/ipsw dyld a2s ios165/20F66__iPhone10,3_6/
dyld_shared_cache_arm64 ADDR` (research/ios-userland.md §3). **Watch:** the CPU ISP holds the big cores at 95-96 C (`thermal: heavy`,
throttled) after a minute of preview. Not done: commit (not asked), a back sensor's path, IMX471 tuning, recording, installed bundle.
Uncommitted now: instantnoodle_camera.c + MiniIOMobileFramebuffer.cpp (exported), camera_device.c, inject_media.c,
rc.jobs.camera-modes, the docs (TODO T07, KNOWN_ISSUES K03, input-peripherals, devices/instantnoodle, research/ios-userland).

**10:17 run 101751 `cam=5`: iOS reads the OnePlus 8's front camera -- verified on phone.** `instantnoodle_camera.c` now serves
`/dev/hk-camera` (redwood's protocol; `hk_camera.c` untouched): `serve_ios()/ios_session()/camdev_*()/keep_picture()` on top of
`stream_mode()/stream_up()/stream_down()` (step 5 split, `st` keeps what was found), `ife_memory_alloc(bytes, places)` (3 places,
each with an unmapped guard page in memory and IOVA), reads counted (`ios_readers`) so the memory is never freed under a copy,
`stream_down()` stops the DMA first. Image `out/camera-ios-root.apfs` (= camera-probe-root + rc.jobs `camera_test raw 60`):
60/60 frames, AE converged, picture kept (`logs/cam5-ios.png`); mediaserverd's stack also streamed. Mock 19/19. Then (10:23,
kernel 9fce7834): the first exposure fixed; **userland** `userland/src/camera_device.c`: on board=instantnoodle the Back Camera
takes sensor 2 (only the front has a frame path) and `load_tuning` uses the OnePlus names (imx471 etc.) -- the media library
built in the container to `out/cam5-media/` (not `userland/obj`), image `out/camera-app-root.apfs` (+ `userland/rc.jobs.camera`:
Camera.app opened by rwtouch, 40 s preview). `run_cam_probe.py 5` with `CAM_IMAGE=...`. Uncommitted: instantnoodle_camera.c,
camera_device.c, the docs.

**10:00 runs 095528 / 100059: SMMU diagnosis and DMA-first teardown.** `smmu_claim()` takes two spare match registers; a
refused IFE pair is tried again on one: 0x801 and 0x841 are refused on SMR 11 too, so **the stream IDs themselves are not
granted** (095528). The teardown now reads back every SMMU register it restores and tells any kept (0 in both runs; CBAR found
0x1f300 from an earlier run). `ife_dma_stop()` (write master off + reg update, RDI 0 halt, VFE-lite reset polled; no lock, no
sleep) runs first in `power_down()`, so a panic mid-stream stops the DMA before the SMMU and the memory are given back (mock R);
100059 is the regression run of step 5 with it (frame written, 0 kept, canary intact). Mock 18/18. BUILD_CAM 10:00, SHA
49b861db..., exported. The supervisor's five conditions for `cam=5` are in T07. Next: `cam=5`.

**09:49 run 094952 `cam=3 cam_step=5 cam_gain=0x380`: a clean front-camera picture.** 094343's frame was dark, not bright (raw
63..89 over a black of 64; `tools/camraw.py` takes no black level off and stretched it white). New boot-args `cam_gain`/`cam_shutter`
(`frames_exposure()`: SMIA 0x0204/0x0202 in hold on/off, after the mode's table): at 8x the raw median is 130, max 352, nothing
clipped (`logs/cam3c-gain-black64.png`, rendered with the black taken off by a scratch script). BUILD_CAM 09:49, SHA 9a7e7b27...,
exported; `run_cam_probe.py 3 '' step5 cam_gain=0x380` (600 of 607 command bytes). Not changed: `tools/camraw.py` (a black-level
option would help every sensor's raw frames). Next: `cam=5`, iOS and Camera.app.

**09:43 run 094343 `cam=3 cam_step=5`: the OnePlus 8's first camera frame in memory and on the host -- verified on phone.**
The owner (relayed 09:1x, "我不在就一直推進，別問") wants the work pushed on without asking; RAM only, the supervisor's step-3
conditions kept. New in `instantnoodle_camera.c`: `frames_smmu_look()` (step 3: SMMU globals + GIC enables, reads only),
`smmu_claim()/smmu_ife_up()/smmu_ife_down()` + `ife_memory_alloc()` (step 4: free SMRs + context under `hk_smmu_lock`, translate,
tables mapping only the frame, guard pages + canary; each IFE pair tried alone -- the hypervisor grants 0x800 and 0x840, not
0x801/0x841), `frames_stream(true)` (step 5: CSID-lite RDI 0 + VFE-lite WM 0 for one frame, stopped at the first buffer done;
teardown stream off -> CSIPHY reset -> VFE reset -> registers back -> SMMU back + TLB -> canary -> hand-over). Runs: 091514 (3a),
092248/092704 (3b), 093614/093955 (3c stopped safely at VFE-lite registers that keep fewer bits: now told, not checked), 094343
(the frame: bus 0x11, 1752 packets, no SMMU fault, canary intact). `xl camera logs/cam3c-094343` (profile loaded) gave
`logs/cam3c-094343-0.png`: a room, correct geometry, overexposed (no exposure set; it shows a person: logs/ only). Mock 17/17
(P: the frame through the page tables; Q: an overrun refused past the last page, its canary told). BUILD_CAM 09:43, SHA 49e104f9...,
exported. Next: exposure, then `cam=5` (iOS / Camera.app; redwood's `ios_session()` as the model).

**09:06 run 090634 `cam=3 cam_step=2`: the front IMX471's MIPI packets reach CSID-lite 0 -- verified on phone.** New:
`frames_packets()` (VFE-lite 0 reset, CSIPHY 2 reset + Linux's `lane_regs_sm8250` with settle 23, CSID-lite 0 reset + RX cfg
`0x232103`/`0x41`, RX IRQ bits latched, packet capture; the sensor's init + res1 + stream on via `sensor_table()`; read; stream off,
CSIPHY reset, CSID/VFE registers back). Result: RX status `0x4003ff`, 14016 packets in 300 ms, 0 ECC/CRC, long packet DT 0x2b
2910 bytes (one line), short packet frame start. Mock 12/12 (K: step 2; L: a sensor that never streams). BUILD_CAM 09:06, SHA
4d9f8ad5..., exported. `run_cam_probe.py 3 '' step2`. Next: step 3 -- IFE-lite 0's RDI0 write master into memory (needs an SMMU
stream for the IFE: read the SMMU first, as redwood's `show_smmu()`/`smmu_up()`).

**08:56 run 085649 `cam=3` (step 1, the frames' path begun): verified on phone.** New in `instantnoodle_camera.c`: camcc mapped
whole (0x10000); `titan_up()/titan_down()` -- PLL0 (Lucid, `cam_cc_pll0_config`, 1200 MHz; used as found if running), 6 RCGs, 11
branches incl. GCC camera HF/SF AXI, refgen, PM8150 L9 1.2 V (shared, never off) -- then CSIPHY 2's revision (`0x40010000`) and
CSID-lite 0's version (`0x20000001`) read; all back. Run 085220 before it stopped safely at camcc-sm8250's core_ahb (0xc150: not a
branch on this phone), now dropped. Mock 10/10 (I: step 1, J: PLL0 found running; it fails any camera-block access without its
clocks). BUILD_CAM 08:56, SHA 9a04b470..., exported. `run_cam_probe.py 3`. The full returned log of each run is read back with
`oem readpart logdump` (scripts in this entry's runs: `oneplus8-audit/camera-native/cam3s1-*-full.log`). Register data for
step 2 (CSIPHY lanes, CSID RX/RDI, counters): `scratchpad`-extracted from mainline camss + LineageOS sm8250 CSID 480 headers (not
in the repo). Next: step 2.

**08:35 `return_dark=1` (owner, relayed 08:2x: no still loader screen burnt into the OLED): verified on phone.**
`MiniIOMobileFramebuffer.cpp`: with it `hk_display_quiesce()` sends DCS display off (0x28, HS, as `panelPower()`) after the last
push; `pushStart()` now sends display on (0x29) before every run's first push (all kernels built from here on). Run 083542
(`cam=1`): "display on (0x29) sent" at the start, "back to the loader dark (display off sent)" at the end -- seen only in the full
returned log (`fastboot oem readpart logdump` + `get_staged`, decoded with `oneplus8-audit/firmware-reader/returned_log.py`; the
loader's own 32 KB log cuts the end). The owner confirmed it works. **A kernel built before this does not send 0x29: after a
dark return the panel stays dark under it until the power key twice (iOS's lock and wake) or a reset.** `run_cam_probe.py`
always passes it. BUILD_CAM 08:35, SHA cc25f811..., exported (`MiniIOMobileFramebuffer.cpp`, which had others' uncommitted
changes before: only these two hunks and the moved `gPanelOff` declaration are this session's).

**08:27 run 082733 `cam=2 cam_pll2=1`, all four: 4 of 4 again, the macro GC2375 at 24 MHz from camcc's PLL2, verified on
phone.** New boot-arg `cam_pll2=1`: PLL2 (Zonda, camcc 0x2000) configured and enabled as Linux's camcc-sm8250 /
clk-alpha-pll (L 0x4b = 1440 MHz; refuses if FSM-voted or found running), MCLK RCG source 5, /10, M/N 1/6; every register read
back; RCG (CFG, M, N, D) back on the crystal while PLL2 still runs, then PLL2 disabled and every found register written back.
Found: mode 0, USER_CTL 0x8, CONFIG_CTL 0x08200920, CONFIG_CTL_U 0x05008001; locked at once (0xe0000007). Normal return 852029
bytes, no panic, phone in the loader. Mock 8/8 (G: 24 MHz; H: PLL2 in use; also fixed the mock's own buffer index). BUILD_CAM
08:26, SHA c2726e82..., exported. `run_cam_probe.py LEVEL [SENSOR [scan] [pll2]]` (SENSOR '' = all four). Between tests the phone stays in the loader: the owner
was offered an idle stable system (it would hold the phone lock up to an hour; saving data needs ufs_write=1) and chose to keep
it as it is ("還是3吧", relayed by the supervisor session, ~08:3x). Next: frame path.

**08:12 run 081247 `cam=2 cam_scan=1`, all four: 4 of 4 chip IDs, verified on phone** -- wide IMX586 `0x0586`, ultra `0x0481`,
front `0x0471`, macro `0x2375`, each at the first read; everything put back, normal return 884317 bytes, no panic, phone left in the
loader (RAM only: ufs_write=0, var_store=0, wifi=0; the owner said through the supervisor session at 08:0x that RAM-only camera
tests need no per-run OK; flash writes still do). Cause of the wide sensor's silence: **gpio156 (its CAM_VANA2) is an SM8250 eGPIO**
(gpio146-179; cfg bit 11 present, bit 12 = the TLMM owns it); it read `0x801` and the driver kept bit 12 clear, so the pad never
moved although the register read back. Found by run **080458** (08:05, `cam=2 cam_sensor=wide cam_scan=1`: NACK with SDA/SCL high;
EEPROM 0x54 and lens motor 0x72/0x73 answer, OIS 0x3e and sensor not) plus mainline pinctrl-sm8250.c's eGPIO table. Fix in
`line_set()`: set bit 12 when bit 11 is set (as `msm_pinmux_set_mux()`), and read every driven pad back by its input (a pad that
does not follow stops the run). Mock 6/6 (new F: stuck pad; the pre-fix driver reproduces the silence). BUILD_CAM 08:12, SHA
4ee4b55d..., exported (`instantnoodle_camera.c`). PM8009 L6 was not needed and is still never switched on. Nothing committed.
Next: camcc PLL2 (GC2375's 24 MHz), then CSIPHY/CSID/IFE-lite frames (T07).

**04:47 run 044740 `cam=2` (owner-approved, the first camera writes; RAM only, ufs_write=0, wifi=0, installed XNULOADER v3, resident
flashboot uploaded, 150 s, normal return 881500 bytes, no panic): 3 of 4 chip IDs, verified on phone** -- ultra IMX481 `0x0481`,
front IMX471 `0x0471`, macro GC2375 `0x2375` (at 19.2 MHz), each at the first read; everything powered down and put back (pins, PM8009
GPIO 1, MCLKs, GDSC, GCC AHB; S5 left 1900 mV, BOB 3312 mV). **Wide IMX586: "does not answer at 0x1a on CCI 0 master 0"** after its
full sequence. Phone left in the loader. Since then (05:01, not run): `cam_scan=1` names a silent sensor's failure (NACK / SCL held
low, SDA/SCL levels before the first START and after) and lists every address answering on its bus while powered (read-byte probes, nothing written);
`instantnoodle_i2c_read_at()` with m 0 = a read with no register. Mock scenario E PASS (5/5); BUILD_CAM rebuilt (05:01, SHA 616f9135...),
exported (`instantnoodle_camera.c`, `instantnoodle_i2c.c`). Next: `run_cam_probe.py 2 wide scan` (587 of 607 command bytes) -- needs
the owner's OK. Docs: TODO T07, K03, input-peripherals.md, hardware-qualcomm.md §7, BOOT_ARGS.

**01:05 run 010516 `cam=1` PASS** (owner-approved, installed XNULOADER v3 Developer mode, resident `userland/flashboot.apfs` uploaded
first, ufs_write=0, 150 s, full return 720705 bytes): `camera: done`. Found: GCC camera AHB/AXI/XO clocks off, pins at reset, PM8009
answers (rev 00 00 00 03), its GPIO 1 MV and writable, **no PMIC-arbiter channel for any camera rail** (research/hardware-qualcomm.md
§7). Run **010201** before it panicked `exec of /sbin/launchd failed, errno 2`: a fresh loader session has no resident image, so the
small image became md0 -- the script now runs `xl ramdisk userland/flashboot.apfs` first, as run_persistent_latency.py does (that,
not SLPI or UFS, was also 003948's likely cause). Driver revised for what cam=1 showed: rails read back by RPMh's completion + 2 ms;
S5 2040 mV then left at 1900, BOB 3312 mV and left there, never off; TLMM writes keep bits 10+ (gpio156/159 bit 11). Mock PASS,
BUILD_CAM rebuilt 01:11 (SHA 554a1533...), exported. **`cam=2` (the first writes: RPMh votes, pins, camcc) not run: needs the owner's OK.**

**Run 003948 (00:39, owner-approved `cam=1`, RAM only, BUILD_CAM + camera-probe-root, wifi=0): the kernel RETURNED EARLY** -- loader
result `returned 104795 0` within seconds (ALS run000154 had returned 585450 bytes). No camera line: the camera thread waits 45 s and
never ran. The cause is in the part of the log that is lost: this USB loader has no partitions, so the returned kernel log is echoed
into its 32 KB `log_buf` and cut there (`logs/003948-run.log` ends in adreno step 20); xl's live follow had not fetched yet. Visible:
GPU steps to 20, battery/slider/touch I2C fine. **The phone had NOT been cold-reset after ALS run000154** ("held SLPI floors require
cold reset BEFORE next kernel" -- that note appeared after this session read HANDOFF); most likely cause, unproven. BUILD_CAM also has
the ALS session's untested 00:06 SLPI edits. Phone left in the loader; nothing else sent. `run_cam_probe.py` now follows every 1 s and
reads the `.live.N` copies. **Correction 00:56:** the supervisor session (OnePlus 8 無人值守監督) paused phone runs at 00:45 (it holds
`/tmp/hackios-phone.run`): that loader had no UFS ("the block service is off") while the args mount the root (`root_apfs=ufs_darwin`)
and `/var` (`var_data=hfs`) from UFS -- the likelier cause of the early return -- and the script had `ufs_write=1`. Fixed: the script
forces `ufs_write=0` and refuses to start while the run lock is held or the loader log says it has no block service. Phone now in the
installed XNULOADER v3 Developer mode (boot slots and logdump readable). Waiting for the supervisor to release the lock; then `cam=1`.

New `xnu/files/bsd/dev/instantnoodle_camera.c`: `cam=1` look (read only), `cam=2` each of the four sensors (IMX586 wide, IMX481 ultra,
IMX471 front, GC2375 macro) powered up per its module file, chip ID read, powered down, all restored. `hk_camera.c` now hands `cam=N`
to it on `board=instantnoodle` (before, `cam=N` here ran redwood's driver at redwood addresses). Also: `instantnoodle_i2c_read_at()`
and `instantnoodle_gpio_io/restore()` (`instantnoodle_i2c.c`, header), `tools/camsensor.py` reads "Parameter Parser V2.0.0",
`userland/build.sh` makes OnePlus tables. Exported to `xnu/files` + `xnu/hackios.patch` (conf/files line); trees in sync. Uncommitted.
Built in a **separate `BUILD_CAM`** (copy of BUILD_GPU + camera; BUILD_GPU untouched), kernel SHA af0e5076... (00:37); host mock (scratchpad) PASS;
`tools/t host` 4/5 (nix-shell absent). Vendor module files in `fw-camera/` (ignored) and `../oneplus8-audit/camera-native/fw-camera/`
(from `odm_a.erofs`, not `vendor_a`). Image `out/camera-probe-root.apfs` = stable root + `fw/camera-*.tab`, apfsck clean, 532 entries.
**No phone run yet** (phone not on USB). Next: owner present → `python3 ../oneplus8-audit/camera-native/run_cam_probe.py 1`, then `2`
(wifi=0, var_store=0, existing HFS, RAM only; takes `./xl wait`). Details: input-peripherals.md (OnePlus 8 cameras), TODO T07.

## Latest ALS runtime — 2026-11-04 00:xx

Run000154 `slpi-see` finished/captured gpu-plan/slpi-ambient-lookup-full.log. Native request ACK QMI result0/error0; indication event768 has only datatype ambient_light, EMPTY SUID list. No lux. lookup-result.json and raw replies saved under als-native/glink. Kernel returned, held SLPI floors require cold reset BEFORE next kernel. Root has NOT sent reset yet. Agents ufs_diagnosis (minimal FastRPC registry bootstrap/native binaries, own fastrpc-next/) and slpi_review (independent query validity review, own lookup-review/) working offline per REA §6; root sole USB owner. Need continue initialization investigation, don't declare auto brightness fixed.

Animation latency report corrected to EXCLUDE GPU teardown submit-failure and later input backlog: persistent-latency-before-stop.json:2788 touch samples post→read median0.014ms p95.080 max7.141; HID dispatch median.063 p95.247;5920 frames submit→panelRAM median21.641ms p95 23.835. Owner says this exact-kernel diagnostic run smooth; initial intermittent stutter not reproduced. No performance change installed.

## Active ALS query and performance results — 2026-11-04

Performance run234859: exact installed kernel/root +touch_lat1, owner reports smooth this round. Timing tails after ~196211000us occurred during GPU teardown (Mesa submit refused then `adreno: run is about to end`); exclude those from animation analysis. No performance fix installed; intermittent original animation lag still unproven. User unsure whether only just after boot.

ALS trace235213 proved sixth frame was remote INTENT size128/id0 incorrectly rejected. Removed only remote !id check; native Linux remote IDR accepts0 and local presence flag separates missing. ASan/UBSan regression covers id0 send/reuse/release. Run235547 hardwarePASS: QRTR HELLO node9,8 services, service400 v1 inst0 node9 port12. Exact log gpu-plan/slpi-service-discovery-full.log. Normal owned shutdown succeeded, then full reset confirmed by owner.

Next active trial: new default-off `slpi_glink_probe=2` sends ONE native-QMI ambient_light SUID lookup to discovered service400 v1/inst0 using AP QRTR port0x4001. Not sensor config, no registry write, no lux claim. Core records up to8 replies<=1100 bytes from exact learned endpoint; bounded hex emitted afterprobe. Mode1 remains inventory-only. Host tests wire offsets/native fixture/endpoint filter/capacity/gating PASS; kernel linked (known export errors). `run_combined.py slpi-see` prepared567B. User confirmed second FASTBOOT reset, fwreader RAM boot just done; uploading residentflashboot.apfs, query run not yet started. Must cold reset after any SLPI trial before next kernel/DSP use. Current installed stable bundle unchanged.

## Active follow-up: animation lag and ALS — 2026-11-03

Owner reports animations do not follow gestures smoothly; not described as missed touch. Normal boot has no fastboot/ADB even after replug. Asked owner normal power-cycle, HakiOS Developer mode and reconnect; awaiting response. Do not flash. First run prepared `../oneplus8-audit/gpu-plan/run_persistent_latency.py`:180s, exact installed kernel extracted to out/wifi-volume-stable-kernel (SHA0ba82636...), stable clean root, existing HFS, touch_lat1, var_store0. Compare pipeline timestamps; mixed old persistent-log windows show late submissions but are not a controlled benchmark. Source/readback UFS path is another hypothesis, not proven cause.

ALS run225107 actually FAILED inventory: READY1, GLINK negotiated/IPCRTR open, then frame6 rejection, hello0/services0/QRTR0/broken1. No lux yet. Added bounded RX-header/rejection logging in tracked+applied slpi_glink_probe.inc, rebuilt BUILD_GPU (linked; known export-symbolset errors), actual-binding ASan/UBSan PASS. No protocol validation relaxed. BUILD_GPU kernel now contains this instrumentation and is DIFFERENT from installed kernel; do not accidentally substitute for strict performance baseline. Trace hashes/report in als-native/glink/trace-validation.json. No new hardware run or installation.

## Latest correction — 2026-11-03 23:24 HKT

The Apple-logo failure of generation1793717896 was SpringBoard SIGSEGV in Core Image GLContext init, not a WiFi/kernel failure. Fixed with guarded UIImage style-preset fallback in userland/src/inject_sb.c; CPU CIContext option was tried and still faulted, removed. Run232103 owner confirmed desktop, volume-minus and WiFi/Safari. Final clean bundle generation1793719359 `out/instantnoodle-wifi-volume-stable.img` now installed SLOT0/readbackPASS. Slot1 is original baseline's BYTE-IDENTICAL payload, promoted header generation1793719358 `IN2010 recovery baseline` so the alternating installer could replace broken slot0 while preserving old recovery content. Both installs readbackPASS. No boot_a change or HFS format. Stored boot verified: existing HFS mounted, original-image style hook active, FD650 compositor/WiFi UP, overlay removed, bootok1793719359 at start4. Orderly `oem stop` issued for final independent reboot; OWNER CONFIRMED final USB-unplugged independent reboot to desktop, volume-minus and WiFi web browsing all normal. Persistent WiFi/volume task COMPLETE. Auto brightness remains pending; do not infer it works. Exact hashes/args/status in ../oneplus8-audit/wifi-native/stable-plan.json; install logs stable-install.log and recovery-promote-install.log. Do not mistake bootok for desktop success. Earlier paragraphs are history.

## Latest persistent WiFi/volume update — 2026-11-03 23:xx HKT

Supersedes older pending-install notes below. Owner asked to persist WiFi and volume now. Installed `out/instantnoodle-wifi-volume-final.img`, generation 1793717896, label IN2010 WiFi Volume GPU90, SHA256 cedcd00671a88a15770b752093826b55961cfb00097dfe5f4846a6dc50fd17b3. Slot0 body/header readback passed; slot1 generation1793694486 retained; boot_a unchanged. No format. Stored boot passed SHA, existing HFS mounted /private/var, radio UP, GPIO7 pull-up readback passed, var_store bootok generation1793717896 observed. OWNER REPORT: still Apple logo. Live log confirms SpringBoard repeatedly exits; no fault trace with current args. bootok is background-only and NOT desktop success. Root issued orderly oem stop; owner asked unplug and enter Developer mode for diagnostic RAM trial using existing HFS plus applog/exctrace. Independent boot NOT verified. Logs and exact331-byte args in ../oneplus8-audit/wifi-native/persistent-{final.json,install.log,live.log}. Do not reinstall or replace fallback needlessly.

RAM WiFi run222502 owner confirmed 5GHz Safari working (5240MHz/channel48), volume-minus restored. GPU90 doublebuffer/TE synchronization owner confirmed smooth/no tearing/manual brightness good; peak85–87fps variable, not locked90. Native BDF and temporary locally administered MAC in private runtime image. No passwords in artifacts. First persistent boot may need network credentials reentered on phone.

ALS remains disabled/unimplemented end-to-end. Native SLPI READY/owned shutdown and GLINK trial225107 completed; held proxy floors cleared by subsequent hard reset and owner FASTBOOT confirmation before this install. No SLPI/ALS flags in final bundle; no sound/Bluetooth enabled. Further ALS work paused for user priority. For final reboot use orderly host `fastboot -s xnuloader oem stop` (sync and WiFi/GPU cleanup); untimed stored boot returns through hardware reboot. Inform owner and unplug USB for stock bootloader reset. Do not claim iOS poweroff/sleep validated.

## Local IN2010 session — 2026-11-03 (this checkout; read before historical notes)

### Current state — persistent installation verified; GPU tests underway

LATEST21:31+: WiFi nativeBDF stage4 hardwarePASS run213151:10requests/10ACKs/57932bytes, intentionalM3stop, noRFmode, checkedteardown+watch. Source fallbackproved fromactualCAPmissingboard→CNSSff/nonGF/ELF/convergedprefix, selectedmodemimage/qca6390/bdwlan.elf SHA7fdcdadb...; NOT ODMvariant. Stage3run200513CAPfields0xd chip0/familyb/SoCffffffff/fw10121492,boardTLVABSENT, MHI129ms. Fullath11k7.0.14+FreeBSD15.1 built (initialmissinglinuxkpi_ieee80211_get_tid fixed+ASan/UBSan tested);7exports,cmd0ABI51434133/cmd2ABI51434134verified. Stage4codeapplied. Normalruntimecleanuphardening currentlygpu_readiness agentOFFLINE beforefullRFscan. Rootwifi-runtime-root.apfs hasverifiedfirmware+BDF+temporaryLAA fw/mac (notfactory,privatemanifest); noSSID/password.

Useraddedvolume-minusbug; rawPM8150GPIO7 hadinput/VIN1/enabled butpull4, nativeFDTpullup. Opt-inbuttons_voldown_pullup1 fixesonlyregc642low3bitswithownedchannel/readback. Reviewed+hosttested; actualreadbackPASS213151, physicalpressnotyetconfirmed. Useraskedcombine tests, noextraisolatedkeyround. Earlierbuttonsrun205352argwasTRUNCATED, nofixappliedthere: loader608-byteABI/max607. Usegpu-plan/run_combined.py compactargs withcriticalflagsfirst+32byteprefixreserve:WiFi4expected529B,normalWiFi506B,SLPI539B; defaultsverified. Don'treuseolderlongcommands.

Readbacktoolsfixed: sharedUSBtransactionlock guardsFWread/getstaged andlogcapture; anoverlappedinventoryreadwasabortedbylengthguard, serialrerunconfirmedmodemrootNOamss20/wlanfiles. Caplogserialrecaptureidentical. Logdecoder nowusesreturnedsize/footer, notfirstNUL(iOSlogscontainembeddedNUL); retrocheckedallarchives,noNEWpanicafterknownWiFisummaryfix. capture_run.py usesphonegetvarresultandsharedlock; savefullrawandtext.

SLPIbootcandidate80aae627...+headersisNOWintegrated/defaultoff, cleanuphooks+targetedbsd/conf/files exportinhackios.patch; fullkernelLINKED. ReviewedSMCuncertaintyretention/panicnofree/strictSMP2P/cancelledFWwait. Packageout/slpi-boot-root.apfs540entries checked; exactnativefilesexposed/hackios/fw. NOTactivatedyet. CmdDB193200actualLCX30030max384index6,LMX30040max384index4,XO300a0on3; readonlySMEM184253PID0/3offset17f000size30000selecteditems0. Requirescoldprovenance/no priorSoCSLPI/audio start, bootonlyREADY→ownedstop, heldfloors, mandatoryhardresetBEFOREanotherkernel/DSPtrialaftermutation. Currentbootstockfastboot→RAMUEFIhasnoSoCSLPI/audio start; QCAexternalMHIhascleanlystopped. Needfinalrunning-memory-exclusioncheckbeforeactivation.

USER STEERING: proceed with Wi-Fi and auto brightness now; do not stop for GPUdeploymentconfirmation. GPU90 persistentcandidate generation1793703016, SHAfcab63f930cceb6689d5eed7af2fc169826273ae5453151bfd65d2a8347168e6 is built/verified but NOT installed. BestGPUmanualownerconfirmed smooth/no tearing/brightnessnormal; GPU+UFSreadonlypreview184947passed/cleanreturn. Phone nowidleRAMxnuloader, residentflashboot.apfs4MB; nextOSRAMruns must use ufs2 root_apfs=ufs_darwin ufs_write0 hk_data0 var_store0 (or reupload2.3GB system).

WiFi readonlystage1 applied/tracked+appliedtrees synced, BUILD_GPU SHA866eb560da6d503e7e3390e944cf19f03f1c8133cd3a5dfed4e0e2a6d37b93c8. Run190622PASS: PCIeGDSCR22f001(off),GCCvoteslow6bits0,all8resourcesfound; nohardwaremutation/noRF. Stage2NOTyetapplied: rootfoundoldclocks_on setsclockedbeforeclocksready; errorcleanupmaytouchPHY/PARF unclocked. gpu_readiness agent fixing thisoffline withactualsequencingtests + preparinglatercap-onlystage. DoNOTrun normalwifi1yet; nativeBDF ambiguous, neverusehistorical12wlan.b0c. NativeQCAvariants underfirmware-reader/native-qca6390; publicath11k pair pinned/license underwifi-native/public-firmware.

ALSreadonlyprobe appliedandstage1passed; startcapablecandidateNOTapplied. ufs_diagnosis fixing independentreviewfindings (kfreecastcompile, allocatorpaniccleanup, invalidSMP2PREADY, staleSTOPACK) andaddingreadonlyLCX/LMX/XOinventory. slpi_review reportunderals-review. iOSCF/HIDbackendunderals-ios compiled/mocktestedonly, inactivewithoutrealcalibratedfeed. SelectedODMconfigs readfromodm_a, no persistedcalibrationread, noactualchip/luxyet. UserexplicitlyconfirmsmanualbrightnessOK.

LATEST: GPU/display performance through DDR voting tested. Run183937: fb_push2,fb_gpu1,fb_push_irq1,fb_panel=amb655uv01,fb_hz90,adreno_speed3(525MHz),ddr2 => busy windows85–87fps peak, variable (not locked90); submit-to-panelRAM median21.702ms vs51.447single. Native90/TE tearing fix already owner-confirmed; bestmanual UI verification pending. Current900s bestmanual RAMrun exec51930 (no automated swipe/touch_lat); preserve session. NativeSLPI read-only stage1 run184253 PASS: PID0/3 partitionoffset17f000,size30000,selecteditems0; noDSP mutation or ownership acquired, no panic. Agent native now preparing justified one-shot start withODMconfig; iOSbackend offline linked/tested underals-ios, not integrated. Shared hk_input.c now includes defaultoffSLPIprobe inc/header, compiled and applied/tracked synchronized. Latest BUILD_GPU SHA25c09d808db21c4af508a30a86685d34c9012ecbdd1e673976b17508b95d66e5. No persistentGPUwrite.

CURRENT DISPLAY WORK supersedes GPU-install readiness: owner reported mild tearing with fb_push=0 after cold GPU boot, then confirmed tearing disappeared with fb_push=1 fb_push_irq=0 fb_panel_rate=0 fb_hz=60. Native panel is AMB655UV01, supports90Hz; added opt-in fb_panel=amb655uv01 native3packet LP sequence (packet types/delays from active FDT) plus fb_panel_rate=0 isolation switch. Run181523 measured TE60→90, but owner felt drops. Controlled run182007 single-buffer90 yielded43–45fps. Run182324 double-buffer at0x9ca00000 succeeds,60–80fps windows but still CPU scaling. Current run_perf.sh scaled is testing fb_push=2 fb_gpu=1 at90, GPU fixed305MHz; automatic swipes at42–56s after launchd,100s total. Latest BUILD_GPU also routes5s frame stats through displayLog; kernel build only usual export-list errors. Do not edit executing shell scripts in place (earlier harmless EOF after completed single run from that mistake).

User additionally requests real auto brightness and confirms manual brightness works. Two existing agents (REA skill§6) doing OFFLINE independent native SLPI and iOS ALS interface work, no hardware. FDT SLPI is PAS12/PID3/IPCC4, reserved0x90600000+0x1500000; historical hk_adsp audio override overlaps it, never start audio driver to probe ALS. Native sensor config hints TCS3408 but actual chip not yet proven. iOS ALS constructor4args, details forthcoming. GPU persistent candidate out/instantnoodle-gpu-bundle.img generation1793700236 is HELD/STALE: fb_push0, no displayfix; NOT installed. boot_a/userdata baseline still original verified software-rendered install.

LATEST GPU milestone: stages1–21 passed; stage21 CPU readback0x600dc0de. After fixing diagnostic exit panic, stages14–21 clean. Uploaded known-good system.apfs RAM image in81s. Isolated GPU service/Mesa run175722 with gpu=1 gl=0 and optional PT/IRQ/DCVS/hwcg/sleep/recovery off: HKGPU retained, freedreno/FD650, GLES3.2 Mesa25.0.7, triangle256²13ms + readback1ms, GLerror0,17086/65536pixels inside (26% expected). Host stopped run, full log has no panic. Now second RAM run with gl omitted for automatic HKGPU compositor selection,600s budget; process session28308. Need logs/screenshot/owner touch confirmation. Persistent boot_a/userdata baseline untouched.

Correction/latest: full logdump capture exposed a post-probe console panic in stages5/9/10/12/14 (earlier “normal return” meant transport returned, not clean kernel exit). Symbolicated to hk_apple80211_summary called from hk_wifi_quiesce after psci_smp_stop_others disables interrupts. Fixed summary-only logging to use loader_log_putc, synchronized applied/tracked hk_apple80211.c. BUILD_GPU rebuilt SHA2f181d13d9be08bb7a789cf3f8639d1408afda3c6d1039de05b14585cccc2ff7; only expected export-list postlink errors. Repeat stage14 run174822: steps1–14,20s watch, cleanup and return PASS with no panic. Probe validator now rejects any panic. Stage15 underway. Isolated GL/Mesa candidate out/gpu-gl-root.apfs built and checked (519 entries), not yet hardware tested.

Latest: RAM-booted out/instantnoodle-fwreader.img successfully. fwinfo OKAY payload is hidden by fastboot CLI 37; task-local native_fetch.py reads that response via libusb, serial-checked. Read-only LP slot0 primary/backup metadata match; vendor_a EROFS acquired with mapping checks before/after each bounded chunk, fsck.erofs extraction passed. Found this phone’s a650 GMU/SQE/ZAP files under vendor_a:/firmware; private hashes/provenance in out/native-gpu/validation.json. modem_a contains no a650 files. GPU stages9 and10 passed (full captured logs and 20s watch); stage10 tested BUILD_GPU successfully. Native firmware included in out/gpu-probe-root.apfs; APFS checker and all516 entry readback passed. Stage12 signature test next. Installed boot/root unchanged.

This update supersedes the earlier local chronological notes below. Owner confirmed USB-disconnected cold boot into iOS 16.5, normal touch and light appearance. Authorized userdata replacement and boot_a write are complete, with written boot-image bytes read back verified. Android userdata is gone; no slot change was performed. Active slot a. Installed boot image SHA256: 0d77ff34367885caadf66d198bb711fc673bef70bc3c2fc1fbd4c17dfe764438.

Native XBL 00358 UFSDxe failed on SCSI NOT READY 04/02 (initialization required). A narrowly gated bounded START STOP UNIT recovery patch passed host emulation and real UFS reads; this patched driver is in the installed image. Private evidence: ../oneplus8-audit/ufs-ida/patch/. Touch/UI and persistent trial passed; final bundle has no runtime limit. Keep installed baseline unchanged during GPU experiments.

Owner entered Developer mode (USB, xl); USB serial xnuloader. GPU stages 2 and 3 passed in RAM tests (logs 172006 and 172439); stage 5 also passed (173422, full log ../oneplus8-audit/gpu-plan/stage5-full.log): CX on, GMU 200000688 Hz, AHB 47446704 Hz, alive 20s and normal return. GPU acceleration remains unverified and disabled in installed configuration. A separate BUILD_GPU kernel adds board_gpu_zap; current FDT requires explicit board_gpu_zap=0x8e21a000 before zap loading, unlike the historical default. Native GPU firmware acquisition is pending; bounded read-only loader integration proposal lives outside source at ../oneplus8-audit/firmware-reader/. The read-only API is now integrated, ASan/UBSan and six Python tests passed, and loader-fwreader.efi built. out/instantnoodle-fwreader.img preserves the installed FD and header geometry; USB-mode RAM candidate not yet tested. It has NOT been flashed. Stage5 may leave CX powered: reboot-bootloader sent; waiting for owner to unplug USB then reconnect at stock FASTBOOT before RAM-booting candidate. tools/t host 4/5 passed; look/check requires absent nix-shell/numpy, not a code failure. The installed loader writes diagnostic logdump even when XNU ufs_write=0. Main sequence GPU, then Wi-Fi, then audio/Bluetooth. EFI character-at-a-time console output is an incidental optimization, not the main task.

### Earlier local chronology (superseded where it describes current state)

- CURRENT MILESTONE: RAM-only UEFI (`out/instantnoodle-v2-ramonly.img`, SHA f53badf74a64b1d5ce0353de75b7dd0043d94b9d40003a6e154d4c5a458ba751) successfully reaches `product: xnuloader` after ~10s; owner confirms text/menu. USB serial is now `xnuloader`, NOT the Android serial. UFSDxe absent from extracted FV. `./xl state` reports loader. Upload `./xl ramdisk userland/system.apfs` started in exec session 1306; do NOT issue concurrent fastboot commands during upload. Next XNU trial should use RAM root and no ufs/ufs_write, software rendering (fb_push=0 fb_hz=60 gpu=0), no radio/audio, syssleep=0; loaded XNU not yet verified.
- CURRENT 2026-11-03: exact hardware target audit passed (instantnoodle FD, payload and loader verified). Owner photo IMG_0927.HEIC proves direct `ios-instantnoodle` probe runs (`BOARD instantnoodle`, live FDT, 30s countdown); ADB returned at 36s. That probe carries no XNU payload. Header-normalized UEFI still crashed; diagnosis below supersedes speculative header/USB explanations.
- Read-only Sahara diagnostics now work on USB 05c6:900e. HELLO mode=2; respond with memory-debug mode and read table. Drain zero-length IN packets after reads divisible by 512, otherwise next OUT times out. `../oneplus8-audit/capture-uefi-crash.py` implements a bounded capture; `read-sahara-region.py` reads another RAM region in an already-negotiated session. NO loader upload or flash commands. Sahara RESET_REQ (7,8), RESET_RSP (8,8) has twice restored Android automatically.
- ACTUAL STOP: original UFSDxe at base b2fc0000 spins at RVA f74c (CpuDeadLoop) after `ASSERT UFS.c +1004: !EFI_ERROR (... | (7))`; stack also contains `ufs_error_log: 28b`. Saved IRQ context ESR/FAR=0 is not a synchronous-fault record. HOB, EFI system table, debug image table, stack and exact build maps establish the driver identity. Evidence `crash-normalized/ufs-assert-finding.json`, `loaded-uefi-images.json`, raw stack. UEFI reaches DXE driver dispatch; XNU has NOT run.
- Current XBL is **00358**, identical a/b. Historical 00248 in abl_log is stale. Extracted current XBL with uefi_firmware into `../oneplus8-audit/firmware/`. Replacing only UFSDxe and its real DEPEX with this unit's native PE (SHA 78f04782a353b6c51a3fae6f458e7e89abdade1d8d5c6bc259eabd3c52918f97) was verified by extracting rebuilt FD. Incremental build first retained old FFS due copy2 timestamps; touch input EFI/DEPEX/INF and verify embedded PE bytes before any test.
- Native UFSDxe test also fails: `ASSERT UFS.c +1029`, same `ufs_error_log: 28b`, CpuDeadLoop RVA 5400. Capture `crash-ownufs/`; replacing driver version alone did NOT fix storage. Underlying controller/DMA/platform cause remains unknown.
- IN PROGRESS: build RAM-only UEFI, omit UFSDxe from local Mu-Silicium `Platforms/OnePlus/instantnoodlePkg/Include/{APRIORI,DXE}.inc`. No assert bypass or storage write: no UFS driver at all; USB supplies system.apfs. Persistent install remains blocked on UFS. Phone reset by Sahara and back in Android. Build log `../oneplus8-audit/uefi-ramonly-build.log`. Next extract FD to verify absent GUID 0d35cd8e, repack normalized v2/current Android OS header, announce and test.
- All diagnostic phone partitions/firmware/RAM dumps are outside git. No phone `flash`, GPT change, set_active or userdata erase was executed. Binary edits/driver overlays are local build-only. iOS not booted yet; do not claim completion.
- Latest supervised test: owner confirmed phone is at hand. Tested `out/instantnoodle-v2-stock-dtb.img` (sha256 e997985991bd3b9e939d418aa211dc612be595abee301439a9d6bc675376b32b), fastboot boot accepted it, but no xnuloader product appeared in the 20-second monitor; USB became 05c6:900e and owner reported black/CrashDump. Header-v2 packaging alone did NOT fix boot. Requested physical restart again. Do not retry this image unchanged. No partition flash or slot-change command was issued.
- Firmware compatibility lead, not a diagnosis: recovered ABL/XBL log identifies BOOT.XF.3.2-00248-SM8250-1, while `uefi/files/Binaries/instantnoodle/README.md` attributes its five USB DXEs to 00295. Need current phone firmware inventory and earlier UEFI-stage evidence. REA doctor ran via task-local npm cache; MCP registration and native providers are absent. No client config was changed or provider installed. Ordinary source/structure observations are not REA native-analysis Evidence.
- Latest recovery update: user reports system booted. Verified Android 16, sys.boot_completed=1, slot a, ADB connected. `adb root` succeeds even though `su` is absent; root diagnostics are available. The previous 900E blocker is cleared.
- Read only the first 4 MiB of abl_log/logdump and boot_a's header/base DTB for diagnosis. logdump segment is empty; ABL log does not prove the failed UEFI stage. Current boot_a uses header v2, 4096-byte pages and a separate DTB; earlier candidate images use header v1/appended DTB. Built and structurally verified `out/instantnoodle-v2-stock-dtb.img` with current base DTB and matching header addresses, preserving UEFI and loader payload. This is an UNTESTED hypothesis, not a confirmed fix. Exact metadata: `../oneplus8-audit/v2-candidate.json`.
- User previously said they were away; asked whether they can reach the phone for physical recovery before another hardware attempt. Keep Android running until that availability is known. No new boot or flash was attempted in this recovery update.
- This is a different owner's IN2010, with Android still installed. The owner explicitly declined backups and authorized proceeding. Do not treat the historical owner's approvals, missing Android, firmware or calibration as this phone's state.
- Live hardware: SM8250, about 8 GB RAM, S6SY771 touch at 0x48 (reset 38, IRQ 39), 1080x2400 amb655uv01 display. Framebuffer reservation matches 0x9c000000 + 0x2300000. Live DTB and private diagnostics are outside git in `../oneplus8-audit/`.
- Built loader, XNU, and instantnoodle UEFI from source. Shipped prebuilts lack instantnoodle and were NOT used. Linux build container: `oneplus8-build`; THEOS=/build/local-theos. GNU awk fixes makesyscalls escaping; python -> python3 fixes export-check invocation. The remaining kernel export-list failures are documented upstream; the kernel linked.
- `userland/build.sh` has one local portability fix: TZ overrides timedatectl, and absent timedatectl falls back to UTC. GNU utilities/flock were installed on the Mac for xl. Userland uses TZ=Asia/Hong_Kong and empty GL/ADSP for the first software-rendering build (GL=0 is NOT disabled in this script).
- First RAM boot: `out/instantnoodle-boot.img` accepted by fastboot, but product stayed kona. Reboot to Android succeeded. Initial Android reported slot b with root; the FIRST fastboot query, before any boot image was sent, already reported slot a. Android returned on a without su. Both slots report successful=yes/unbootable=no. NO set_active or partition flash command was executed.
- Second RAM boot: `out/instantnoodle-live-dtb.img`, using the live Android FDT, accepted by fastboot but entered USB 05c6:900e QUSB_BULK. No ADB/fastboot, no iOS or loader success. The user is away. STOP device work until a physical restart restores a normal transport. Suggested recovery: unplug USB, hold Power+Volume Up about 20 seconds, reconnect after restart. Verify state; do not assume recovery occurred.
- The first APFS system build exhausted disk; its incomplete image was removed. The newly downloaded Mu-Silicium dependency checkout was removed after preserving FD/shim/DTB/mkbootimg in `out/uefi-built/`. Original port sources remain in `uefi/`. OrbStack was restarted and Docker service restored. Do not use bare mkxnuimg now: its Mu tree is absent and fallback is redwood. Existing boot images are diagnostic failures, NOT ready-to-flash deliverables.
- Rebuilt system.apfs on macOS with `out/build-system-host.sh` (GNU utilities, pipefail), 608453 x 4096 bytes. root.apfs and system.apfs both passed apfsck -cw; the kernel's APFS reader read init, stage0, original launchd and SpringBoard byte-identical to their source files. Four tools/t host checks passed; fifth requires Nix in its wrapper but passed independently with Python/numpy.
- Exact artifact hashes, commands/results and next actions: `../oneplus8-audit/artifacts.json`, `progress.json`, `content-validation.json`, and build logs. No full partition backups exist. Boot success remains UNVERIFIED. Next: physically recover, inspect actual boot-chain/firmware diagnostics before another RAM test; never repeat the failing live-DTB image blindly.

---

Short, actionable, and dated. It does **not** replace [TODO.md](TODO.md) (roadmap), [KNOWN_ISSUES.md](KNOWN_ISSUES.md) (what is broken) or the subsystem pages;
it says where the work stands *today*. **Update this file after every substantial session** (rules in [../AGENTS.md](../AGENTS.md)); keep it under ~150 lines and move
anything that has aged into the right permanent page.

**Last updated: 2026-10-27 02:40 MSK** for the driver audit fixes below (before that 01:15 MSK, the Bluetooth audio transport). Earlier state: ~04:55 MSK by the scheduler session (sections 2, 3, 4, 7 for the scheduler; the rest as the documentation session left it at ~03:30). **Checked state** = what I ran/read on that date; **historical** = taken from earlier session notes and flagged as such.

## Driver audit fixes — 2026-10-27 ~02:40-02:55 MSK (built; partly verified in RAM runs on the phone)

Audit of the OnePlus drivers against OnePlus's kernel (OnePlusOSS sm8250 R11) and mainline Linux; fixes ported from them as data
(GPL code is not copied verbatim; wpa_supplicant's logic is BSD). Kernel builds clean (`out/kernel-build.log`, the usual symbolset
errors only), `tools/mk-ath11k.sh` builds, `tools/wpa-test/run.sh` 267 + 218 checks pass (new: PTK renewal case). Edited in
`xnu-src` and copied to `xnu/files` file by file (no `xnu/export.sh`). Uncommitted. Nothing flashed, no phone run yet.
- RPMh TCS 0 shared claim `hk_rpmh_claim/release` (`hk_board.c`), used by `instantnoodle_touch.c`, `hk_adsp.c`, `hk_cpufreq.c`, `hk_goodix.c` ([power.md](subsystems/power.md)).
- Wi-Fi PTK renewal + KRACK guards (`wifi/lx/src/lx_wpa.[ch]`, `lx_80211.c`, `tools/wpa-test`) ([wifi.md](subsystems/wifi.md#the-station-shared)).
- SM8250 thermal sensor map (`hk_thermal.c`) ([thermal.md](subsystems/thermal.md)).
- Touch: vendor status-event handling, chip reset, charger mode (`instantnoodle_touch.c`, hook in `instantnoodle_battery.c`).
- AW8697 factory f0 trim `haptic_trim=11` (`hk_haptic.c`, profile `common/devices/instantnoodle.conf`).
- I2C serial engines (GENI FIFO, `instantnoodle_i2c.c`), opt-in `i2c_geni=1` (read-only probe) / `2` (use). Default unchanged.
- Not done: panel sleep-in (Android's on-command already carries the PPS and was tried: [display.md](subsystems/display.md)); merging the
  8 command-database parsers / DSP stack copies (T29).
- **Phone runs (owner-approved, RAM only, 02:38-02:50 MSK):** `tools/t quick` `023834` boot PASS; `024422` PASS 7/7 (`haptic=5
  haptic_trim=11 i2c_geni=1`); `024633`/`024827` (`--keep`, the latter `i2c_geni=2`) boot PASS, `pods` FAIL only because iOS's "Low
  Battery 20%" alert covered the App Library (screenshot `t-024630-library.png`; battery 19-20 %, charging). Seen: TRIM_LRA `0x0b` read back;
  touch charger mode 2; touch/gauge/slider engines have FIFO and work in FIFO mode with 0 errors (touch read 2082 -> 368 us); no RPMh claim
  failures logged. Not exercised: audio DSP rails (`adsp=1`), Wi-Fi renewal, touch under a finger, touch error paths, thermal throttling.
- **Found (pre-existing):** a run's log no longer brings the kernel's message buffer back: the loader's `oem log` recomposes without it
  after the return (`compose_logs(NULL, 0)`), so plain kernel `printf` lines are lost (also why "IOLog lines no longer reach phone logs").
  Work-around used: touch, I2C and battery print through `say()` to the loader's log as `hk_buttons.c` does. Fix belongs in `xnuloader`
  (needs a loader reload, which the owner deferred).
- **Next ([TODO T37](TODO.md#t37-driver-audit-follow-ups-2026-10-27--drivers--partial--p2)):** the owner will run `i2c_geni=2` with the touch screen later (then make it the default); `adsp=1 sound=4` once to try the
  amplifiers' bus on its engine (quiet hours: `sound_amps=0`).

## Post-wake UI fix — 2026-10-26 23:50 MSK (Claude session that owns sleep/power; parallel to Bluetooth)

- Owner report: after sleep, Control Center empty, status bar items/bar gone, apps blank. **Fixed and verified on the phone
  (RAM runs only, nothing flashed):** `MiniIOMobileFramebuffer.cpp` `vsync()` now sends `mach_continuous_time()`; QuartzCore's
  frame clock lagged by all the time slept. Diagnosis and A/B in [power.md](subsystems/power.md#ui-missing-after-a-wake-fixed-2026-10-26).
  New diagnostic boot-arg `sleep_freeze=0` (`hk_power.c`). Both files exported to `xnu/files` by copy (not `xnu/export.sh`); uncommitted.
- Not in the installed build (T03 needs the owner's OK). Pre-existing: Control Center close after a wake is slow (KNOWN_ISSUES K10).
- Found, not fixed: `IOLog` lines (HKGPU, MiniIOSurface...) no longer reach phone logs (they did on 2026-10-25); `printf` does.
- Repro used `tools/t` through a temporary wrapper outside the repo; `tools/t` itself untouched. Phone left in the loader.

## Bluetooth audio transport — 2026-10-27 01:15 MSK (latest)

Owner-supervised RAM runs `005834`, `010727` (nothing flashed; phone back in loader). **A2DP heard** (lock sound, record chime).
**HFP/SCO UART stall fixed** in `hk_bt.c` (uncommitted, exported by copy to `xnu/files`): legacy SCO set-up sent as Enhanced with
HCI data path (`bt_sco_hci`), SCO paced to the chip's own packets (`bt_sco_pace`); `010727`: 3 voice links, 0 drops, no restart.
**Voice Memos through buds: owner-verified working in `012839`** (record + playback). Its crash in `011859` was CoreAudio's
uncaught `thread_policy_set realtime failed` (KERN_INVALID_ARGUMENT); `xnu-src/.../osfmk/kern/thread_policy.c` now cuts an
over-50-ms computation (`rt_clamp`, logged) -- not exported to `xnu/hackios.patch` yet, and not proven necessary (no cut logged in 012839).
`010727`'s other failure (buds answering after 3 s, 1809) did not recur. Details: [bluetooth.md](subsystems/bluetooth.md#bluetooth-audio-fixed-at-the-transport--2026-10-27-0115).
Also changed: `userland/src/sound_test.c` (`bluetooth-level`, max 0.1), new `userland/rc.jobs.btaudio`, `tools/bt/check.py` (SCO fixtures; 8/8 PASS).

## Recording diagnosis — 2026-10-27 00:36 MSK

Owner reports Voice Memos crash, silent lock sound, brief black screen then recovery; buds disconnected.
`002629` shows Voice Memos selects CMF HFP input/output at 00:27:33; VirtualStream reports client-format
error 'what' for 16-bit/8 kHz/mono. Standard HCI Setup Synchronous Connection completes status 0. Audio
server PID changes 40 → 239 around 00:27:56. No usable crash stack in that run; do not assert root cause.
Lock sound remains deliberately muted by `sound_amps=0`; black-screen cause is unproven. Sleep agent owns it.
Silent RAM isolation `003355-run.log.live.*` (exec 22955) enabled `exctrace=1 applog=1`, no buds. Existing
`record_test 2` selects MicrophoneBuiltIn, records 96000 non-silent samples/two seconds successfully; Voice
Memos launches without a captured exception. Test ended normally after 73 s; phone returned to loader.
No source fix justified yet. Next physical gate: re-pair buds and repeat Voice Memos Record under exception
tracing, preserving HFP/SCO and audio-server logs. Quiet-tone approval persists, but no new tone played.
Work stopped for that gate. No sleep/power edits, flash, pairing-privilege changes or persistent recording.

## Bluetooth RAM audio test — 2026-10-27 00:28 MSK (ended)

Owner authorized RAM restart and quiet playback. Shared phone lock was free; used current kernel including
Claude's sleep/display fixes without changing their files. Run `002629-run.log.live.*`, exec session 57319:
`bt=1 bt_trace=1 adsp=1 sound=4 mic=4 sound_amps=0`, native BT HAL bundle present, DSP/mic/controller up.
Settings shows CMF Buds Pro 2 Connected. Diagnostic selected BluetoothA2DPOutput/CMF Buds Pro 2, volume 0.30;
played 2 s at 1% sample peak, AudioQueueStart 0, 96000/96000 frames accepted. Native AVDTP START accepted,
OPEN → STREAMING at 00:27:20; no Bluetooth restart observed. **Heard output awaits owner confirmation.**
No microphone/bond/sleep-reconnect proof. No new driver edit, flash, loader replacement or automatic pairing confirmation.
Manual window ends after the queued 120-second post-tone hold (max run 420 seconds); preserve logs on return.

## Bluetooth audio diagnosis — 2026-10-26 22:30 MSK (historical)

- Owner paired CMF Buds Pro 2 in native Settings. `221539-run.log.live.*` + screenshot: Connected; HCI Simple
  Pairing/Authentication Complete status 0; Encryption Change 0/enabled 1; AAC selected; AVDTP CONFIG/OPEN
  accepted, media channel OI_OK/OPEN. Owner reports no heard sounds and Voice Memos recording fails.
- **Found actual audio gap:** build overlay exposes only HKAudio; original BTAudioHALPlugin.driver was excluded
  by select-system.py. bluetoothd publishes audio devices but CoreAudio has no Bluetooth device plug-in.
  Fixed selection + opt-in packaging; original unmodified bundle extracted locally from rootfs.dmg, never added to git.
- Previous test boot also omitted `adsp=1 sound=4 mic=4` and forced `sound_amps=0`; built-in silence/recording failure
  cannot diagnose Bluetooth hardware from that boot. Do not infer Voice Memos selected the buds' microphone.
- Owner authorized **quiet** audio. Added `sound_test bluetooth-tone 180`: activates Playback, waits for exactly
  CMF Buds Pro 2/BluetoothA2DPOutput, then plays 2 seconds at 1% sample peak; other routes/timeouts play nothing.
  Host gate fixtures PASS (not framework proof); HAL selection/staging/original-byte identity and H4/NVM/UUID checks PASS.
- Native userland build with HAL plug-in, diagnostic tone, timeout bounds and opt-out cleanup PASS; no new
  hardware boot yet. Final host checks PASS. Playback still unverified.
  Further transport blocker may appear: native plug-in contains AudioSkywalkPipe/AudioRingBuffer; inspect runtime logs.
- Current loader is old (`./iphont agent`: no mailbox), cannot exec into running iOS. Existing manual run ended after
  638 seconds and returned to loader; no agent tone was played. Next test needs a RAM boot/re-pair and mediaserverd logs.
- **Coordination:** owner says another Claude agent owns deep/system sleep. Leave sleep/power code to that agent;
  use the shared phone lock for every run; record Bluetooth retention/reconnect dependencies without changing its work.
  No loader replacement, flashing, private pairing privileges, key export, or parallel transport authorized.
- Next owner action is re-pairing the buds in the corrected RAM test. Stop at that physical gate; quiet-tone approval
  persists. Diagnostic recipe: `adsp=1 sound=4 mic=4 sound_amps=0 bt=1 bt_trace=1`; native HAL route is the test gate.
  Preserve ignored userland sources and uncommitted drivers; no commit/push requested. Earlier notes are historical.

Earlier implementation details: [Bluetooth subsystem](subsystems/bluetooth.md#archived-implementation-handoff-historical-2026-10-26-2142).

## 1. State of the project (checked 2026-10-26)

- Active device: **OnePlus 8 (instantnoodle)**, no Android on it, iOS 16.5 user space running from its own flash. Installed boot verified 2026-10-24 (historical, owner-confirmed); iOS boots to SpringBoard, touch, 90 Hz display,
  GPU (OpenGL ES), speakers, main mic, haptics, flashlight, battery, Wi-Fi (RAM runs), sleep/wake, HFS+ `/private/var` work per [devices/instantnoodle.md](devices/instantnoodle.md).
- Redwood (POCO X5 Pro): not touched recently, owner asked not to run POCO checks.
- Everything is **uncommitted**: git HEAD of the kernel repo is `de24a37` (2026-10-25), of the userland repo `d2c1e87`. Work since 2026-10-25 ~15:00 (GPU collapse, brightness,
  `ca_ambient`, vmd DENY, Wi-Fi station work, rendering fixes, `tools/t`, wakeperf/look) lives in the working tree only.
- Docs: this overhaul created `docs/{README,architecture,BUILD,BOOT_ARGS,TODO,KNOWN_ISSUES,HANDOFF}.md`, `docs/{devices,subsystems,research}/`, `AGENTS.md`, `CLAUDE.md` and moved images to
  `docs/images/` — **all uncommitted**; run `git add -A docs AGENTS.md CLAUDE.md README.md` (+ `xnu/README.md`, `uefi/README.md`, `prebuilt/NOTICES.md`) in the **kernel** repo. `docs/air*.md` belong to the **userland** repo.

## 2. Uncommitted changes a new session must preserve

Check with `git status --short` **and** `common/ugit status --short` (many paths appear in both; see [architecture.md](architecture.md#two-repositories-one-tree)). 2026-10-26 03:25: 68 modified/deleted entries in the kernel
repo, 39 modified in the userland repo. Do not `git checkout`/`reset`/`stash` them. Groups:

| Group | Paths (representative) | Owner/meaning |
|---|---|---|
| Wi-Fi station and QCA6390 (verified in RAM runs, 2026-10-25) | `wifi/lx/src/*`, `wifi/lx/include/lx_more.h`, `xnu/files/bsd/dev/hk_wifi.c`, `hk_lx.c`, `hk_sta.c`, `hk_apple80211.c`, `hk_net.c`, `sys/hk_{lx,net,sta}.h`, `tools/mk-ath11k.sh`, `tools/wpa-test/run.sh` | WPA3 H2E, TKIP, private MAC etc. |
| Power / GPU / display (GPU collapse and brightness verified at command level 2026-10-26) | `hk_power.c`, `hk_adreno.c`, `HKGPU.cpp`, `hk_cpufreq.c`, `MiniIOMobileFramebuffer.cpp`, `loader_return.c`, `GenericARMPE.cpp`, `MiniDrivers.h`, `instantnoodle.h`, `hk_input.c/.h`, `instantnoodle_{i2c,touch}.c` | `xnu/export.sh` was run for round 3 addenda; **re-check that `xnu/` matches `xnu-src/`** before exporting again |
| Tools | `xl`, `xnu-build.sh`, `tools/frames.py`, `tools/t`, `tools/look/`, `tools/wakeperf/`, `tools/sleep-test/` | test harness (short tests) |
| Profile | `common/devices/instantnoodle.conf` | sound/sb_clock args |
| Userland repo | `userland/build.sh`, `mkjobs.py`, `src/eagl.c`, `inject.c`, `inject_sb.c`, `rwtouch.c`, `power_test.c`, `hkprof.h`, ... | rendering fixes, vmd DENY, ca_ambient, profilers; new files need `common/ugit add -f` |
| Docs | all of `docs/`, `AGENTS.md`, `CLAUDE.md`, `README.md` | this overhaul |

**Scheduler work (2026-10-26 02:20-04:55, done for now, uncommitted):** Edge scheduler + performance controller ([subsystems/scheduler.md](subsystems/scheduler.md)).
`xnu/export.sh` was run at 04:48 after checking `xnu/` was not ahead of `xnu-src/` (only `hk_cpufreq.c` and `GenericARMPE.cpp` differed, both newer in `xnu-src`); the export
also picked up other sessions' `xnu-src` edits then present (`bsd_init.c`, `kern_exit.c`, `IOService.cpp`, `exception.c`, `osfmk/conf/files.arm64`, `iokit/conf/files`, `bsd/sys/Makefile`).
New files to add when committing: kernel repo `xnu/files/bsd/dev/hk_perfctl.c`, `hk_sched_check.c`, `tools/sched-test/{qemu.sh,amp.py}`, `docs/subsystems/scheduler.md`;
userland repo `userland/src/sched_test.c` (`common/ugit add -f`). Baseline kernel binary from before the work: `out/sched-baseline/mach.development.bcm2837` (`XL_KERNEL=`).
A second session is building a **developer toolkit** (`tools/hkdev/`, `docs/TOOLKIT.md`, edits to TESTING/README/INSTALL command references). Ask before editing a file that is modified and not yours.

## 3. Active priorities (see TODO.md for all)

1. **T01/T02** verify power and Wi-Fi-through-sleep on the phone (safety paths unverified): [TODO.md](TODO.md#t01-verify-the-unverified-power-work-on-the-phone--power--built-unverified--p0).
2. **T11** scheduler — working (Edge default, `sched=clutch` baseline); open points in [TODO.md](TODO.md#t11-heterogeneous-cpu-scheduling--cpu--partial-working-uncommitted--p1).
3. **T03** install the current build (needs the owner's explicit approval; writes flash).
4. **T04/T05/T07/T08** missing hardware: sensors, Bluetooth, OnePlus cameras, headphones.
5. **T10** Metal usability; research notes in `docs/air.md` (userland repo).

## 4. Recent changes worth knowing (2026-10-24 .. 26, historical summary, details in the pages)

- 10-24: speakers, alert slider, main mic, HFS+ `/private/var`, installed boot on the OnePlus 8 ([audio.md](subsystems/audio.md), [storage.md](subsystems/storage.md)).
- 10-25: sleep/wake rounds 1-4 and the GIC shared-interrupt fix ([power.md](subsystems/power.md)); Wi-Fi QCA6390 ([wifi.md](subsystems/wifi.md)); App Library vibrancy fix; custom boot logo; `tools/t` ([TESTING.md](TESTING.md)).
- 10-26: **Edge scheduler on three clusters with our CLPC replacement** (`hk_perfctl.c`; boot-arg `sched=`; `config_pervasive_energy` on; `kern.hackios.amp`);
  membench threads bound and self-starting (a caller-starvation hang under `sched=clutch`).
- 10-26: GPU block collapse in sleep (133 ms wake), hwcg, touch boost, brightness service, `ca_ambient`, vmd denied (post-wake CPU), kernel probes (`top`, `cpuspeed`, `memlat`, `membench`, `kprof`, `hkprof`). The installed bundle predates these (T03).

## 5. Blockers
- Owner approval for anything that writes flash (`userdata`, `boot_b`), swaps slots/ABL/XBL/TZ (forbidden) or runs audibly.
- SoC low-power states are architecturally blocked ([power.md](subsystems/power.md)). Sensors need the DSP's SEE client; App Store/FairPlay need Secure Enclave behaviour.
- No Android on the OnePlus 8: its vendor/persist/modem firmware only exists in backups outside the repo (`../ios-instantnoodle/`).

## 6. Verified findings to rely on (details in pages)
CrashDump triggers ([research/hardware-qualcomm.md](research/hardware-qualcomm.md)); never DCS 0x10; never `touchscreen=1` on the OnePlus; Android's QCA6390 firmware asserts under ath11k (use linux-firmware);
GIC per-core shutdown must not quiesce shared SPIs; `vmd` was the post-wake CPU hog; sleep timers use wall time; tests: `tools/t host` 2 s, `quick` ~70 s, `power` ~2 min.

## 7. Tests performed and results (checked unless marked)
- 2026-10-26 02:40-04:50 (scheduler session): `tools/sched-test/qemu.sh` 4/4 modes PASS; phone `tools/t boot sched` Edge 8/8 kernel checks, A/B workloads (scheduler.md);
  `tools/t full` Edge 16/17 and Clutch 16/17 (both fail only "frames after sleep" phase 2 after 5 rapid sleeps, ~66 fps: pre-existing); one false `sleep` FAIL (K24).
- 2026-10-26 (historical, from session notes): `tools/t power` 10 sleeps with GPU CX/GX off, wake 133 ms; `tools/t brightness`: 20/90/5/100% reach the panel as 0x52/0x67a/0x6/0x7ff; `tools/t full` passed except a noisy frames-after-wake phase (verdict loosened).
- 2026-10-25 (historical): `common/flash/qemu-test.sh` 14/14; `tools/sleep-test/run.sh` 4/4; phone: 3/3 sleeps, power-key wake, WPA2/TKIP-mixed/WPA3 joins.
- This documentation session ran **no hardware tests** and made no functional changes. It checked links and referenced paths mechanically with a throw-away script (not kept in the repository).

## 8. Where things are
Index [README.md](README.md) · architecture [architecture.md](architecture.md) · build [BUILD.md](BUILD.md) · boot-args [BOOT_ARGS.md](BOOT_ARGS.md) · tests [TESTING.md](TESTING.md) ·
research [research/README.md](research/README.md) · agent rules [../AGENTS.md](../AGENTS.md) · phone logs `logs/` (`t-<time>.txt`, `<time>-run.log`, `.live` copies) · build log `out/kernel-build.log`.

## 9. Recommended immediate next actions
1. Read `git status` of both trees; commit the docs (kernel repo) without touching the others' files.
2. Ask the owner for a short supervised phone session: `tools/t power`, then T01 charger/off-mode checks.
3. When the scheduler session finishes, export (`xnu/export.sh` after syncing `xnu-src` to the patch), update [subsystems/cpu.md](subsystems/cpu.md), TODO T11.
4. Pick one missing-hardware item (T04 sensors is the most valuable for iOS behaviour) and start with its first read-only probe.

## iphont continuation — 2026-10-26

- New agent CLI is **`./iphont`**, Python standard library, not `tools/hkdev`/Rust. Start with `./iphont describe` and [TOOLKIT.md](TOOLKIT.md). Host modules, `xnuloader/iphont.c`, `xnu/files/{bsd/dev/iphont.c,bsd/sys/iphont.h,iokit/Kernel/IPhont.cpp,osfmk/arm64/iphont_threads.c}`, kernel hooks, and the userland daemon/job are uncommitted. New userland files need explicit `common/ugit add -f` when a commit is requested.
- Preserved scheduler/Wi-Fi work. Added only a narrow `hk_touch_inject` API to the scheduler-owned input files: same queue/boost, no physical recovery-chord counting. Apple-file patch sections were regenerated selectively; **no whole-tree export from a stale private tree**. `xl` log/capture staging and fetches now hold one USB lock. No legacy script was deleted; [TOOLKIT-MIGRATION.md](TOOLKIT-MIGRATION.md) inventories 170 owned scripts/modules and the parity gates.
- Verified: 32 host checks including the real loader C protocol fixture; existing `tools/t host` 4/4; native loader/daemon builds; clean private XNU compile/link (known export checks still fail); APFS verified by both readers; seven real XNU QEMU probes and a timed return. Physical phone: state/log snapshots/screenshots and five PNG captures (~1.6 fps), without changing the run. This is **not** mailbox, daemon, touch, or keyboard hardware validation.
- **Owner deferral:** “Continue host/build checks; leave phone running.” No reboot, slot change, partition write, or new physical boot was performed. The next physical test needs that restriction lifted and coordination with the running phone session, including the known cable replug requirement.
- Ignored reviewable artifacts are in `out/iphont/`: `loader-ram.img`, new `loader.efi`, `iphontd`, diagnostic `root.apfs`, private `xnu-clean/.../mach.development.bcm2837`, host/QEMU/build/image reports, and capture files. `prepare` packages a RAM-only instantnoodle image and never touches the phone. The new root is md1 diagnostic content; preserve the ordinary resident md0 flashboot workflow.
- **Build trap found:** copied `BUILD/` embeds absolute `SOURCE_DIR` paths into the original checkout. A copy can otherwise compile concurrent scheduler edits against stale private headers. The clean private build discarded only its own generated `BUILD/`; shared builds/source were not reset. Use `out/iphont/xnu-clean`, not the older mixed-path `xnu-integrated` artifact, for the next RAM test.
- Remaining scope is T36/K22: supervised USB/agent/control tests, framework/entitlement behavior, deeper GPU/service tests, recovery/deployment parity, function symbolication, and legacy removal after evidence. No claim of comprehensive completion.

Validation note: source whitespace checks pass. Full `git diff --check` flags unified-diff context whitespace inside the stored `xnu/hackios.patch`; raw source checks are separate. The original concurrent scheduler/Wi-Fi/docs changes remain uncommitted and were not reset.
