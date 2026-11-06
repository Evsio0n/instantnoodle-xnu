# HANDOFF — start here when you begin a new session

**The owner's roadmap (2026-11-04, relayed by the camera supervisor): "roadmap修相機 ->其他 -> 最后trustzone和Titan 480 IFE".**
(1) the camera: the three back cameras, zero-copy, the installed bundle, the owner's own check (T07); (2) the rest: T38 (the
owner's list, items 1-13); (3) last: TrustZone (FairPlay, the Secure Enclave: T15) and the Titan 480 IFE's full hardware ISP.

**Git, 2026-11-05 about 18:50:** the work below is committed locally (kernel d63f37e, userland 596393b on branch instantnoodle) and published to github.com/Evsio0n/instantnoodle-xnu without `tools/metal/loop/`, whose scripts name local paths. "Nothing committed" in the sections below was true when they were written. Kernel default-on is done (cam-6329a3e9); the HEIC section's "no kernel flip" is older. MDP work in progress is in xnu-src only.

## Metal, sleep and the GPU's speed — 2026-11-06 about 12:00 (Metal session; RAM only; **all committed locally on instantnoodle at the owner's word, 「所有的先提交git」, not pushed**)
- **Row unchanged:** `metal-三次看`, kernel mdp-93fe105e, libvulkan d3451ca5.
- **Installed on the phone, the owner's word (「今天先寫到系統裡」), written by the supervisor; came up as generation 1793938158: boot slot 0, "Metal 3 2026-11-06", `out/instantnoodle-three-vol-bundle.img` 024141cb25b9c3e1** (0600, it holds the Wi-Fi file). Slot 1 keeps "IN2010 Camera 0.5x 1x 2x", the menu's previous version. Kernel mdp-93fe105e; boot volume `out/installthree-root.apfs` 8e0cce3852c2e611, the row's own libraries (HKMetal 07eb08d0, no pool code; libvulkan d3451ca5 and its seed), no photos, no test jobs, no probes. Its line is the row's plus the installed storage arguments and `buttons_voldown_pullup=1`, **with `syssleep=0 gpu_sleep=0`, so it never truly sleeps: drain unmeasured.** **Location on it (the owner: 「定位沒開」, then 「可以，我插線」):** `init` skips the skeleton on a kept `/var`, so nf-extra's location prefs were not installed, and Settings has no Location Services row here (next-features.md, run 015651). **Written to the kept `/var` by one timed boot from the Mac, run 123116, no slot touched:** kernel mdp-93fe105e, root `out/installfour-root.apfs` ae1cf7930532c4ce (the installed root with only `/sbin/init` a15c48d265cbf3ab: `init.c`'s `location_prefs()`, once per volume, stamp `/private/var/.hackios-location`), `wifi=0 ufs_write=1 hk_data=1 var_store=0`, 60 s. Log: `[ ok ] Location Services' switch on a kept data volume`, 33 checks 0 failed, `ufs: 4911 writes into darwin, 118181888 bytes, read back the same; 0 refused`, no hang. `xl health` the same before and after (17 starts). The sync's own line is not in the kept log, as in earlier runs. **Not yet proven: Maps' prompt on Metal 3, the owner's to see.** The bundle `out/instantnoodle-four-bundle.img` ("Metal 4") was built and **never written**; slot 1 keeps the Camera bundle. `userland/src/init.c`'s edit is uncommitted, and Metal 3's own init has no such step, so a `/var` made anew would come up with Location Services on from the skeleton, and any later install should carry this init. Not known yet: how Metal holds up on the kept `/var`. An entry in `/var/tmp/hk-air` has a magic number and no checksum, so one cut by a hard reset could crash the compositor (inferred, never seen).
- **Sleep with Wi-Fi off holds (runs 110920, 112641; K10):** after a locked sleep the screen lit and Camera gave pictures, twice. One timed wake from an apparently unlocked phone stayed dark, reason inferred only. **Wi-Fi across sleep still ends the system (T02); boards default to `wifi=1`, so no row may try sleep.**
- **GPU flags (`gpuirq dcvs gpuhwcg`): no gain, not proposed (112106 against 113329).** Under `gpu_dcvs` the display's `GPU busy` reads 0: DCVS's reader takes the same count (`HKGPU.cpp`).
- **`gpu_top=587`, the owner's word: runs at 587 MHz, no rate gain (115324 against 113329; gpu.md).** Off by default, kernel `out/kernels/mdp-5302e45d.mach`, not in a row. Whether to use it is the owner's, after seeing the comparison.
- **No-finger Maps recipe:** `out/rc.jobs.mapsgpu`, `location_test enable` before Maps instead of a blind tap (0 fingers, no prompt). Baseline 113329: gestures 20, 52, 26, 45 map frames a second.
- **Built, never booted to the end: libvulkan 33ee0e40 (`out/vk-new`, `tools/mesa/turnip-hk-bos.patch`)** with `rwa_buffer_busy` (the pool asks it instead of waiting a second) and `rwa_vk_memory` (K40's counts; hkmetal's `MEMACCOUNT=1`). The render pipeline cache is compiled in and off (`share/probes/gfx_cache`). A row stays on d3451ca5 until its seed passes `functions to 70 s`. **Next, in order: harvest (`out/rc.jobs.harvestn`; run 114959 was stopped at the start, 0 files), the leak run (`out/rc.jobs.leakrot`, 12 minutes), `mapspool` against 113329.** Run 104515's successor with `hk-slow` (root `out/mapsslow-root.apfs`, the edit in `hkmetal.m` left uncommitted on purpose) was stopped at the start, ticket #104, no result.
- **K40 re-read:** no-finger Maps levels off at 366 MB in backboardd with 166 MB in live objects, so it does not reproduce the growth; the leak job opens the apps the owner opened.

## Metal, the owner back — 2026-11-06 about 11:00 (Metal session; RAM only, kernel committed by the supervisor as 1d6e83c, **hkmetal, libvulkan, tools, docs uncommitted**, boots held for the owner's cold restart)
- **Row on the page: `metal-三次看`,** out/eye-three-root.apfs a04dd891b34474cd (0600, the owner's two photos; Wi-Fi by the file), kernel mdp-93fe105e, libvulkan d3451ca5 in `out/vk-row` with its seed `out/air-cache/d3451ca5`, media library `out/camfast-media`. Speakers and `mic=4` on, not yet heard. Never booted in this form. Below it `metal-二次看` (the owner: 「目前這一版比較流暢」). Hidden rows: `out/eye/versions-hidden.json`.
- **Fixed, proven:** camera loop (K38), vertex textures and refused-pipeline retries in Maps (K35), mediaserverd's 1 GB a second (K38). **Fixed, not yet clean:** Maps' gesture rate.
- **Broken, known:** the compositor's memory (K40, S1); Wi-Fi across sleep ends the system (T02). Sleep is not in any row (`sleepon` is the flag that allows it).
- **Roots are built with the assignments on `mk_root.sh`'s command line.** `env $COMMON` in zsh passes one word and silently gives no seed and the wrong library (run 091048). `VKLIB=`, `MEDIALIB=`, `NOMEMO=1`, `BIPLANAR_ONCE=1`, `MEMLOG=1` are `mk_root.sh`'s; `cam_fast=1`, `cam_mem=1`, `sleepon`, `gpuhwcg` are flags.
- **Heat rules (the supervisor; `docs/TESTING.md`, "Reading heat"):** a session run that throttles: tickets out, report, resume only on their word; a minute's reading of 75 or more with no throttle: report, normal rest. Report both figures.
- **Next, the supervisor's order:** phone back, then: buffer pool; a no-finger gesture number (the location prompt needs a blind tap about 30 s in); GPU comparison; `nowifi` sleep; K40.

## Metal: shaders, CoreImage, HDR, Maps, Camera — 2026-11-06 about 06:30 (Metal session; RAM only, **nothing committed, nothing installed**, owner asleep; all boots through `tools/bootq`)
- **Shaders built before the boot (the owner's 「开机前能不能build着色器」), proven by log:** a seed at `/hackios/share/air-cache`, harvested over the console (`cache_dump`, `tools/metal/loop/cache_take.py`), keyed on libvulkan's UUID: a new libvulkan misses all of it and needs a new harvest. Current: `out/air-cache/d3451ca5`, 274 files, 3.6 MB. Cold 052222 against seeded 053937: backboardd's translations 26.7 s to 2.8 s, all processes 28.4 to 3.4 s. First Control Center 2959 to 162 ms, Search 2508 to 167, Today 913 to 23 (043628 against 045055, an earlier library). Graphics pipelines have no cache yet.
- **CoreImage: function stitching done (K37), verified by bytes:** all sizes right, 8 orientations 0 off at scale 1, gain maps through real CoreImage. `inject_gainmap.c` is in no new root.
- **HDR, portrait, tap to base and gain map at the layer, by script:** 15.7 s to 8.21 s (`hk_specialize` memo, pipelines kept, cold) to 4.15 s (seeded). Landscape 1.01 s. Client side only: the panel has not been timed, and **the owner's eye has not seen this build**.
- **Maps opens on Metal in a test root with `com.apple.fairplayd.H2.plist` dropped** (next-features' find, their section 17 of docs/research/next-features.md; `NF=1` in mk_root.sh does it; the installed system is untouched). Three boots, 060829, 061301, 061838: first commit about 2.1 s, What's New draws with its icons right, the location prompt draws. No 0x8BADF00D, but Maps was never terminated in them, and the old one read "failed to terminate gracefully". Others that need fairplayd now get failing lookups. Home and Work in Favorites are empty circles, unexplained, perhaps network resources. FairPlay itself stays out of bounds: fail fast is fine, no forging.
- **Maps' pinch, a valid rate at last (061838, scripted, no network, blank grid):** Maps' map frames 28 and 25 a second; compositor 66 and 57; GPU busy about 50%; texture making 0.3 s per 5 s. Why Maps stays near 28 with GPU to spare is open. Shots show its map black because of its own pipe, so **the panel's map is unverified**. K35.
- **Camera preview (K38; dark room, script):** 15 a second from the sensor, AE stretching the frame (060013). With `cam_fast=1` the sensor holds 30 and the compositor does 24-29 (073342, 064117), darker; that choice is the owner's. **The light is not measured.**
- **mediaserverd's 1 GB a second during any preview: fixed in test roots, proven by log (071949):** footprint at frame 300 from 19.8 GB to 171 MB, free memory from 3 to 653 MB. The cause, Apple's audio node finding about 468 s of silence missing at every round, is **not explained**; the node's own switch turns the filling off (`inject_media.c`). Free memory was **not** the dark room's stutter: gaps unchanged. Whether 171 MB levels off is not measured. A video has no sound: no `/dev/hk-mic`.
- **Heat, a correction:** the night's "hottest" figures were the minute's sample. 14 of 55 runs had brief core peaks of 85-90.7 C (the owner's 041029 the highest), no throttling. docs/TESTING.md, "Reading heat". Phone work stopped at about 07:40 under the supervisor's 75 C rule, which they have since made precise: docs/TESTING.md, "Reading heat". **This session's work waits for the owner's look at the row.**
- **Row for the owner:** `metal-一次看` on the eye page, out/eye-once-root.apfs db76d86d75cc40f8 (0600, the owner's two photos), kernel out/kernels/mdp-93fe105e.mach; its text lists what to look at, Maps and Camera in the light included. Booted once by the queue, run 072508: home, photos imported, no panic, no SMMU refusal, not slowed, 86.8 C at a moment's peak. Maps, the camera and the memory fix are not in its job: they were proven on the mapsq and camem roots.
- **Uncommitted, this round, userland repo:** `tools/mesa/air/air_to_nir.c`, `nir_air.h`, `air_vk.c`, `air_metal.h`, `tools/mesa/turnip-darwin.patch`, `userland/src/hkmetal.m`, `hkdrawable.m`, `hk_drawable.h`, `inject_figerr.c`, `inject_gainmap.c`, `hk_gain_image.h`, `ci_gain.m`, `cache_dump.c`, `vt_test.c`, `camera_device.c`, `inject_media.c`, `tools/metal/loop/mk_root.sh`, `build_m5.sh`, `eye.py`, `cache_take.py`. Beside the repos: `../oneplus8-audit/camera-native/run_cam_probe.py` lets `cam_fast=` and `cam_mem=` through. The row's media library is `out/camfast-media` (`MEDIALIB=`), not `out/nf-media`. Untracked: `out/` (roots, seeds, `out/rc.jobs.*`, `out/eye/versions.json`, `out/hdr-photos`). The owner decides when the photos and their 0600 roots go.
- **Next, in order:** the owner's eye on the row, Camera in the light first; the 468 s in the audio time stamps; Maps' rate (pacing, then the push waits, 275 ms in 11 s); a graphics pipeline cache; the remaining 4 s of HDR; `hkBiplanarTexture` by the GPU. TODO M5g.

## Metal: the picture and the pacing — 2026-11-06 about 02:20 (Metal session; RAM only, **nothing committed**, owner asleep)
- **Verified by shots on the phone (run 020936, current libraries, kernel mdp-11824ed0):** App Library with its background and sharp icons, Maps' sheet icons straight. Widgets without yellow on earlier builds of these fixes. By the owner's eye: the triangle gone on every wait, 「黃條正常了」 (Stocks' bars). **Not yet looked at by the owner: `fb_wait=13`, row `metal-all2` on the eye page** (kernel out/kernels/mdp-11824ed0.mach, root out/eye-all2-root.apfs; `python3 tools/metal/loop/eye.py`, running as a background task of that session and not to be restarted during a run). The row's own root (2e34836dfcb2f2f0) has not been booted; a root with the same libraries and kernel has (020936). Touch latency with three surfaces is not measured.
- **Maps' pinch, the answer so far:** Metal, in texture making: two new memoryless whole-screen 4x targets every frame, really allocated here, 35-76 ms each on Maps' main thread. An image pool of 256 MB (`air_vk.c`) cuts that from 2.3 s to about 0.4 s in 5 s. Measured with no network, a blank grid. **Not judged by the owner; no clean baseline; no valid hk-gl comparison.** Top lead if it is not enough: a shared image for memoryless targets. docs/research/mdp-driver-plan.md 9.
- **Numbers and reasoning:** docs/research/mdp-driver-plan.md 9, TODO M5g, K35, K36.
- **Uncommitted:** in `xnu-src`: `iokit/Kernel/MiniIOMobileFramebuffer.cpp` (fb_wait, the push's overlap counters), `HKGPU.cpp`, `MiniIOSurface.h`, `MiniIOSurface.cpp` (padding to rows of 4), with the MDP session's work; do not run `xnu/export.sh` as it stands. In the userland repo: `userland/src/hkmetal.m`, `tools/mesa/air/air_vk.c`, `air_metal.h`, `tools/mesa/hackios-drm/hk_drm.c`, `tools/mesa/turnip-darwin.patch` (and by hand the Mesa tree's `tu_hackios_exports.txt`), `tools/metal/loop/*`. Runner: `../oneplus8-audit/camera-native/run_cam_probe.py` passes `fb_wait=`, `mdp_overlap=`, `fb_hold_push=`, `mdp_trace=`, `touch_lat=`.
- **Boots:** `GOAL_KERNEL=out/kernels/mdp-11824ed0.mach GOAL_ROOT=eye-heavyc GOAL_TRACE= GOAL_FLAGS="mdp_shots=0 novoldown" SHOT_COUNT=0 bash tools/metal/loop/goal.sh TAG 150`. For hk-gl add `GOAL_METAL=`. A script that boots must check that a new run log appeared: when another session holds the phone goal.sh returns and `/tmp/goal-all.txt` is the last run's (this happened).
- **Device lost at a run's end** (VK_ERROR_DEVICE_LOST, OUT_OF_DEVICE_MEMORY, within seconds of the end, after `lx: the firmware is asked for mode 4`) reads as the kernel going down, not a freeze; the ordering is what supports it, not more.
- **Rows for the owner (older; the current one is `metal-一次看`, section above):** `metal-all2` (smoothness, fb_wait=13, the pinch) and `metal-heic` (out/eye-heic-root.apfs, 0600, the owner's two photos, with the gain map stand-in; the phone imports them by itself for about 2.5 minutes first; that root itself has not been booted, the same libraries with a watcher have, 035422).
- **HDR highlights appear for the first time (test roots only):** Photos' gain map was empty because CoreImage on Metal makes empty images (K37: no function stitching). `libhackios-gainmap.dylib` makes it on the CPU, picture up to 255 beside a UI white of 188 in the same frame (run 035422). K33. **Not judged by the owner.**
- **Next (done, see the section above):** function stitching for CoreImage (K37, TODO M5g), then VTMTSComputeFunction's second a session.

## An .ipa on the phone (T38 item 13) — 2026-11-06 about 12:30 (next-features; the owner: 「可不可以幫我裝ipa，比如telegram」; nothing committed, nothing run on the phone)
- **Built, tested on the Mac only:** `userland/src/hkinstall.m`, `tools/ipa/`. Registers through LaunchServices, no installd; fetches over Wi-Fi; first proof on a RAM `/var`, not logged in. **Kept `/var` needs the owner's own word.** `docs/research/ipa-install.md`.
- **Held, written down, not started:** auto-brightness (the supervisor has the plan), the modem's option A (kernel ec3396a4, in the private tree).

## The modem runs its image, and Maps' white screen — 2026-11-06 about 07:20 (next-features; RAM only, **nothing committed, nothing installed**, owner asleep)
- **Verified on the phone (run 071723, kernel 19c17424, `out/kernels/nf-19c17424.mach`): `modem=3` brings the SDX55 to its mission:** EE 2, MDM2AP status 1, stable for the 60 s watched, no CrashDump, once-a-minute AP reading 51.5 C (no peak recorded, **no sensor on the modem itself**). Nothing was asked of it: no SIM, registration, RF or QMI. **That run ended with PERST low only, which does not stop the chip: it may have run on, unattended, for up to 14 m 46 s.** Since run 073746 a warm reset ends every clean stop (status 1 to 0 within 150 ms; Android has nothing stronger). **A hung kernel still leaves it running. Mission runs only with the owner there, through the supervisor; boots are stopped for now after run 100158 throttled. `modem=3` stops before image 8 unless `modem_mission=1`. The run's end stopping running firmware is shown (093504, 100040); QMI LOC answers two questions that start nothing (`modem_loc=1`, run 102633: standalone; lock state not known); the owner's first GNSS try (run 110304) was refused as locked for non-framework clients; the second, with Client Type AFW (run 111823), was accepted and **the modem crashed (errfatal) about 70 s later, with no satellites listed; its failure reason (run 113829): `remotefs_sahara.c:850`, an assertion on a Sahara transfer nobody serves in the mission**; a further try needs the owner's word.** Section 17 of next-features has the evidence. **GNSS is not reached**: QRTR on `IPCR` works (run 100040: 47 services, QMI LOC at node 3 port 84), and LOC answers (run 102633); a session was refused as locked (run 110304).
- **Not landed:** the code is in `tools/nf/modem/` and the private tree `xnu-src/nf-tree/xnu-8796.121.2` (its `BUILD_NF`); `xnu-src` still has only `modem=1,2`. To run it: `env NF_BUILD=xnu-src/nf-tree/xnu-8796.121.2/BUILD_NF NF_LOUD=1 NF_DONE='modem: level 3 is done' NF_ROOT=out/nf10-root.apfs bash tools/nf/boot.sh TAG 'modem=3 wifi=0 adsp=0 sound=0 haptic=0 cam=0 bt=0 torch=0' out/rc.jobs.nf10 220`, as one quoted string after `tools/bootq run ... --`. **Whether it runs beside Wi-Fi, audio and the camera has not been tried: every modem run had them off.**
- **Maps' white screen with `locationd` on a root: found.** `locationd`'s queue waits on `fairplayd.H2`, which exits every 10 s without answering. **Interim, a choice for the owner to see:** RAM test roots with `locationd` leave out `LaunchDaemons/com.apple.fairplayd.H2.plist` (`tools/nf/mk_extra.sh loc`), so the lookup fails at once. Nothing is bypassed, FairPlay stays unavailable as it was, the installed system is untouched. Seen on GL (055706) and Metal (060829). **Limit from the supervisor: no forged Mescal signatures, no fairplayd crypto, no hooks around FairPlay.**
- **Wi-Fi location works by the owner's eye (run 083507, about 08:43: 「map可以加载了，定位正常」; RAM root, Wi-Fi joined by hand, no fairplayd job).** The earlier expectation of no fix without Mescal was wrong. Accuracy not measured; path inferred (Apple's Wi-Fi positioning from `locationd`'s scans); next-features section 17.
- **Sensors:** step 1 done in the private tree: the audio DSP at `0x91b00000`, off the SLPI's memory (run 081133, kernel 41c061b7; not listened to; `xnu-src` still has `0x8fe00000`). **Step 2 needs the owner there:** an SLPI attempt requires a cold reset after it, and a reset with the cable in needs a hand on the phone.
- **Waiting for the owner:** whether to allow mission runs while watching; system sleep with the flashlight on, before any install (T39; `hk_torch_sleep()` has never run). Details: docs/research/next-features.md section 17, K12, T06, T14.

## Flash, Bluetooth, location, modem — 2026-11-06, about 02:00 (the owner: 「1，2，3，6先做吧」, 「連 modem 一起做」; RAM only, nothing committed or installed; the owner asleep from about 00:50)
- **Owner-confirmed on the combined build (「闪光灯和蓝牙OK了」, relayed 2026-11-06): the flash and Bluetooth.** They also said the camera's video stream still has a problem: with the Metal session. **Verified by logs and numbers:** flashlight by Control Center's button; the camera's flash On and Auto at 500 mA a channel; Camera's icon and the flashlight with no `cam=` or `torch=` on the line (the kernel writes those nodes now, the loader need not change); Bluetooth up beside everything; Location Services' switch on from `var.skel`, `locationd` running. Details and what is not known: `docs/research/next-features.md` section 17, `subsystems/input-peripherals.md`, `subsystems/bluetooth.md`.
- **In `xnu-src`, exported and committed 2026-11-06 as 1d6e83c with the rest of the kernel tree (the private `nf-tree` is not in it):** new `bsd/dev/instantnoodle_modem.c` (+ `bsd/conf/files`, `hk_audio.c`, `sys/hk_audio.h`); `bsd/dev/hk_torch.c` (flash, `hk_torch_sleep()`, no idle wake-ups); `iokit/Kernel/GenericARMPE.cpp` (`cameraNodes()`); `pexpert/arm/pe_bootargs.c` (**`torch=3 bt=1` in the defaults**); `bsd/dev/hk_bt.c` (no power with no firmware); `bsd/dev/hk_battery.c`, `sys/hk_input.h`, `bsd/dev/hk_power.c`. Userland: `userland/src/camera_device.c`, new `photo_info.c`, `location_test.m` (+ `.entitlements`). Tools: `tools/nf/` (`boot.sh`, `mk_root.sh`, `mk_extra.sh`, `mk_media.sh`), `tools/bt/mac_peer.swift`, `../oneplus8-audit/firmware-reader/read_bt_fat.py`, `list_modem_fat.py`. Objects in `BUILD_NF`, kernel `out/kernels/nf-48bfb6bf.mach`, firmware read to `fw-bt/` (not in git).
- **Before any install:** `torch=3` is now on with `syssleep` on, and **system sleep has not been run on these defaults at all** (also T39: not with the MDP pipes). The light-off before sleep (`hk_torch_sleep()`) is built and has never run: the torch has no hardware timer. Run it with the owner there. The install's root needs `tools/nf/mk_extra.sh`'s pieces (`BLUETOOTH=1`, `fw-bt/`, `locationd`'s job and `var.skel` lines) and this `libhackios-media`.
- **Waits for the owner, in order:** a system sleep with the flashlight on. (`modem=3` was approved and ran, and mission runs are stopped for now: the section above.)
- **Modem (the owner: 「modem 继续」):** `modem=2` passed (run 035821, kernel 0e5d50d3): reset, link at 5 GT/s x2, `17cb:0306` in PBL, no CrashDump. `modem=3`: BHI and Sahara, with bus master and an SMMU stream for the first time: **it has since run, the section above; use the private tree's latest build, not this one or 19c17424: older ones end a run with PERST low only.** Built first as kernel `out/kernels/nf-b974035d.mach` from a private copy of the tree, `xnu-src/nf-tree/` (made so as not to change `xnu-src` under the Metal session's build; its `BUILD_NF`, `NF_BUILD=` for `tools/nf/boot.sh`). The new files are `tools/nf/modem/` (`modem_fat`, `modem_sahara`, `modem_mhi`, `modem_boot.inc`; `run.sh` tests the first two on the Mac): **to be put into `xnu-src/bsd/dev` with their `bsd/conf/files` lines and the small change to `instantnoodle_modem.c` when the tree is free.** Which Sahara id is which file was inferred, then tried one boot at a time (`modem_boot.inc`'s table): an unknown id ends the run and says which.
- **No picture leaves the phone:** a Camera screen is never screenshotted (`xl screenshot` copies it to the Mac); `photo_info` prints a photograph's numbers there.

## Our own MDP driver — 2026-11-05 (the owner: 「我们要switch over 自己写mdp」, 「可不可以1-8都做了然后回来test」; RAM only, nothing committed)
- **Verified on the phone by logs, and by the owner's use: 「試過了，很不錯」, wrong colours, a line at the middle or stutter 「完全沒有」 (a detached layer's picture not asked about):** `mdp=8` and `edr=1`, now **this board's defaults in `xnu-src`** (pe_bootargs.c; `mdp=0` off, verified). `edr=1` because CoreAnimation is told 0 nits without it, which refuses most detaches (`docs/research/mdp-driver-plan.md` 8). Frames straight from CoreAnimation's surfaces through the MDP's scaler, no GPU scale; the display's S2CR on a context bank of ours; CoreAnimation's detached layers in their own pipes on the Metal compositor. Numbers and limits: `docs/research/mdp-driver-plan.md` section 8; design: `docs/subsystems/display.md`. Kernel `out/kernels/mdp-070cd902.mach`, runs 182621-204521: 0 underruns, 0 SMMU faults, no CrashDump; HBM, the EDR step and the brightness slider seen working with frames on the pipes.
- **A correction everyone needs:** the display's stream is match register 6, context 5 now, not SMR4 / CB4. Never read a context by number.
- **Uncommitted, in `xnu-src` only, not exported:** new `bsd/dev/instantnoodle_mdp.c` and `.h` (+ `bsd/conf/files`); `iokit/Kernel/MiniIOMobileFramebuffer.cpp`, `MiniIOSurface.{cpp,h}`, `HKGPU.cpp` (`hk_gpu_busy_ticks`), `pexpert/arm/pe_bootargs.c`, and `used[]` in `bsd/dev/hk_wifi.c`, `hk_sound.c`, `instantnoodle_camera.c`. Also `userland/src/inject.c` (`ca_nits=1`; the shared root's library does not have it, `out/mdp-media/libhackios.dylib` does), `tools/mdp/boot.sh` and the docs. **`xnu/export.sh` must not be run as it stands** (`docs/BUILD.md`). Build: `OBJROOT` in `BUILD_MDP`. Test roots `out/mdp-root.apfs`, `out/mdp-metal-root.apfs`, jobs `out/rc.jobs.mdp`, `mdpm`.
- **Kernels built from `xnu-src` now flip the S2CR on every boot of this board.** After a hang with no clean return, an older kernel's Wi-Fi or sound scan could read the display's old context: a CrashDump. A clean return is fine, and so is this kernel after a hang.
- **The owner's 「現在卡卡的」 was on the Metal compositor (run 201040), slow with or without the pipes.** On hk-gl, the default, frame times match the GPU scale (the plan's section 8). Its 'how it feels' on the last kernel has been asked for.
- **The owner's flashing in Maps (run 202548): my bug, fixed in the log's terms: 44 frames of the composite's hole alone in run 204134, 0 in run 204521; the owner's eye on the fixed kernel not yet heard.** A swap names only the layers that change.
- **Metal's speed, HDR, VideoToolbox and the flash are now one round, led by the Metal session (the owner: 「這幾個是一起的問題吧，一起修了」).** `edr=1` feeds CoreAnimation its nits; the dim Photos is not that (plan page, section 8). No more MDP comparisons are planned.
- **Next: T39 in TODO.** A detached layer's picture; the NV12 path has never run; flips; three failed pushes and a panic with the pipes on; system sleep.

## DSP round, stopped for MDP — 2026-11-05 (the owner: 「等隔壁看看收尾，收尾后做 DSP」, then 「跑完了跑MDP」; RAM only, nothing committed)
- **Kernel to start from: `out/kernels/cam-6329a3e9.mach`, built from `xnu-src/xnu-8796.121.2` as it stands (BUILD_CAM's binary has the same SHA, no source newer than it).** Build: in the container, `./xnu-build.sh OBJROOT=/build/xnu-src/xnu-8796.121.2/BUILD_CAM/obj -j11 -k`; it ends with an error by design, judge by the binary's date. **Everything for it is in xnu-src and NOT in xnu/:** pexpert/arm/pe_bootargs.c, bsd/kern/kern_sysctl.c and bsd/dev/hk_sound.c are not exported (hk_sound.c was already behind since 2026-11-04). Do not recreate the tree with xnu/apply.sh, and run xnu/export.sh before any commit. Two sessions sharing this tree: say so before starting a build.
- **The board's defaults (verified on phone, runs 180414, 180530, two boots of one power-on):** on board=instantnoodle, `wifi=1 wifi_bdf_native=1 adsp=1 sound=4 sound_amps=1 haptic=5 cam=5 cam_after=10` are added to the kernel's boot line unless that name is given (pe_bootargs.c), so arg=0 is off and user space's kern.bootargs agrees. Each came up by its own log line. No such args: 550 of the loader's 607 bytes with exctrace and ramhfs. Touch not touched. **Install dependency: `oneplus8-audit/wifi-native/stable-plan.json` keeps `wifi=1 wifi_bdf_native=1` until the installed kernel has these defaults; an older kernel without them would lose Wi-Fi.** The runner detects such a kernel by the string in its binary; flags `nowifi`, `nosound`, `nocam`, `silent` (sound_amps=0), `touchlat`, `mediastacks`, `medialog`, `sound_periods=N`, `sound_period_frames=N`.
- **Sound, measured from logs with the amplifiers down (run 180858 baseline, 181527 with `sound_periods=4`):** see TODO T38 (18). Post of a touch to the queue's end about 108 ms with 4 x 10 ms, about 130 ms with 8 x 10 ms. Not measured at the speaker. **Camera speed not started.** The tone-and-microphone reference waits for the supervisor's word.
- **Boots:** `tools/metal/loop/goal.sh` with `GOAL_METAL= GOAL_OFF= GOAL_TRACE= GOAL_KERNEL=out/kernels/cam-6329a3e9.mach GOAL_ROOT=dsp1 GOAL_FLAGS="silent touchlat sound_periods=4"`; job list out/rc.jobs.dsp1; `tools/metal/loop/latency.py LOG`.

## HEIC and HDR on Metal over Vulkan, stopped on the owner's word — 2026-11-05 (Metal session; RAM only, nothing committed)
**Stopped by 「我们保存结果接下来做dms」; the supervisor is confirming what comes next. To pick HEIC up again, this is the state.** Details and causes: docs/TODO.md, M5e. The target is the owner's: HEIC and HDR photos as iOS does them.
- **Verified on the phone (RAM):**
  - Wallpaper back with VideoToolbox's Metal path served, no decline (run 164610, `outputs/m5f5-164610-wallpaper-heic-through-vt-metal.png`). The 420f pictures were still converted on the CPU in that run; that proves the path no longer blocks the wallpaper, not that the wallpaper is converted on the GPU.
  - VideoToolbox's BGRA scale on the GPU: worst off by 1 against the pattern, as the CPU path (run 162255). Generic HEICs decode with the right colours through ImageIO.
  - Photos imports the two HDR photos, opens the grid and a full view, in colour and not the gain map's grey, with no crash (runs 165643, 170709). **Not HDR, soft, and dimmer than it should be: see below.**
  - Rows: 111 ok, 0 differ on the phone, 2 refused by the test itself (surface-540-*); real Metal 113 ok. Corpus vk 271 of 271 identical at the last frontend change (run 165643). 0 SMMU, 0 IS, 0 DEVICE_LOST in the last boots.
- **Open, in the order they stop things:**
  1. **~~CMPhoto still answers -12914~~: origin found and the failing one gone (2026-11-06, runs 021440-025340), TODO M5g.** It is a small preview's quarter turn with a scale during the import, not the full view. Verified by bytes and by the log, not by the owner's eye.
  2. **~~VideoToolbox's next refusals are destination imports~~: served, A PLANE, BY ITS BYTES (TODO M5g; a 16-byte texel with rows the GPU cannot have goes through a copy, no kernel work).** As it stood: format 123 (RGBA32Uint) over BGRA at a quarter of the width, and R8Uint and R32Uint per plane on a 420f ("surface N has 2 planes", "is 504x378 and the descriptor asks for 126x378"). It falls back to the CPU on each. Next fix: `rwd_texture_for_surface` in userland/src/hkdrawable.m (plane offset and row from IOSurface; compare bytes and not widths). A 16-byte texel needs a row of 256, which is kernel work.
  3. **The dim screen: corrected, see K33.** The brightness does rise to 140 nits, in those runs too, which keeps SDR white where it was by the numbers; what is missing is highlights (highest pixel 189-197, run 025340). As it stood, and wrong about the rise: Photos' white is 187 of 255 (about half in linear light), the picture's highest pixel is 191, and brightness-nits stays at 70-83, so the screen is dimmed for HDR with no extra brightness and no highlights. It is not hbm=1 (same with and without). **The comparison with the compositor on hk-gl has not been done;** run 231531's notes say factor 0.5 with nits rising to 140 there. If Metal's route makes Photos dimmer than SDR, this must not reach an install.
  4. **Size at full view: 1126x1500 is the screen's width (rotation seen by the watcher, run 022942), so it is not short by itself.** As it stood: **Size at full view unchanged by 540:** BGRA 1512x1134 and 420f 1496x1122 imported, gain maps up to L008 2016x1512. The photo's real size has not been read from its header.
  5. **Wallpaper colour:** vt_test's 420f of Y 105, Cb 141, Cr 83 gives BGRA 129 124 34 on the phone by the GPU and the CPU alike, 128 133 42 on this Mac. It reads as VideoToolbox's own matrix on this boot and is not settled.
  6. **Rows surface-540-* need a test fix:** the phone refuses them with "an inline sampler whose state is not a constant", from this Mac's compiler. Format 540 is verified on the phone only through vt_test's results.
  7. No RGBA16F surface appears in Photos yet, so the kernel round for its rows has not been reached. IOSurfaceSharedEventAddEventListener fails with e00002c7 in SpringBoard and Photos (no shared events in MiniIOSurface); its effect is not known.
- **Default-on (the owner's rule: what is proven takes no boot-arg):** the runner has Wi-Fi and sound on by default, with `nowifi`, `nosound`, `nocam`. **No kernel flip is done.** The line is 607 bytes: all on with ramhfs is 656, so goal boots run `nocam nosound`. Gates and bytes: TODO, M5e.
- **To boot the goal configuration:** `tools/metal/loop/` holds this session's scripts (they name their scratchpad; set `S`): `build_m5.sh` in the container, `mk_root.sh JOB` (env `TRACE=1`, `PHOTOS=1`, `FRAMEDUMP=1`, `VTDECLINE=1`), `goal.sh TAG SECS` (env `GOAL_ROOT`, `GOAL_FLAGS`, `GOAL_OFF`, `GOAL_TRACE`, `SHOT_DELAY`, `SHOT_COUNT`, `SHOT_GAP`). Last used: `GOAL_TRACE= GOAL_ROOT=m5f7 GOAL_FLAGS="datadefault novoldown nowifi photos edr2 hbm slideshowlog"`. Job list: out/rc.jobs.m5f7.
- **The owner's photos:** test roots out/m5f4-root.apfs, m5f5, m5f6, m5f7 and out/hdr-photos/ hold them (0600); the owner decides when they go. Copies of shots showing them were deleted. They go into a root under generic names only; a shot showing one goes to the owner only, through the supervisor, never into docs or outputs.

## The whole system on Metal over Vulkan — 2026-11-05 (Metal session; RAM only, nothing committed)
- **The owner's direction (relayed): 「我就是要和iOS 一样metal +vulcan」, 「你不用这样测，要朝着目标测」.** Every boot is `metal=2 metal_profile=1 metal_backend=vk camtl=1`; what stops the system decides the next fix.
- **Verified on the phone:** home screen complete and right with the compositor on Metal over Vulkan (run 141516, `outputs/m5d2-141516-home-complete-compositor-on-metal-vk.png`), 30 a second while it moves. Settings, Safari, Clock, Photos and Maps' first-run sheet open and draw (runs 141836-143528). Last boot 151633: `metal_drv_test --m5c` 101 ok, 0 differ, 0 refused; corpus vk 271 of 271 identical to the baseline of run 124507; 0 SMMU, 0 IS, 0 DEVICE_LOST, no trap.
- **Not done:** docs/TODO.md's M5d passage, "Open from that" ([since then] VideoToolbox's Metal path is served, see the section above): VideoToolbox's Metal path was declined as a stopgap, the 420 and misaligned-row copies do not follow a changing surface, video and the map itself and HDR Photos are untried, Siri's orb at 4 samples is refused, the trace lines are still on.
- **Tools for a wrong picture:** `metal_drv_test --ca-fill` (CoreAnimation's own library and constants), `air2nir --constants I=HEX,...` (what hkmetal's `spec` line says, as NIR, on the host), hkmetal's per-draw probe and thread sample (files /hackios/share/probes/metal_probe and sample in the root), `hk-trap` lines (a SIGTRAP's stack, always on).
- libhackiosair stays 6ef66c30 and was not touched. Before anything of this goes into a system image it needs its rebuild and regression.

## HDR (item 17) closed for the display side; Photos and HDR photos — 2026-11-04 21:3x-2026-11-05 00:1x (camera/backlog session; RAM only)
- **Verified on the phone:**
  - The full EDR ramp: CoreAnimation's factor went 0.993 -> 0.500 in about 2 s while brightness-nits rose 70 -> 140 (edr=2,
    run 231531). The headroom is about 2 at 70 and at 20 % brightness, so the photo's own headroom limits it. No HBM was
    needed (nothing over 430).
  - Photos no longer crashes on HDR photos: GL library a5074b37 put eagl.c's EAGLContext table under a lock. Two decode
    threads had taken one place; CI::GLContext::init then called address 0 (exctrace, run 230437).
- **Open, waits for Metal (KNOWN_ISSUES K27, TODO T38 (17)):** a full-size HDR photo shows its grey gain map. Photos'
  PHFigDecoder gets -12914 from CMPhoto's accelerated decode (it probes the M2 scaler and links Metal and MPS).
  VideoToolbox's rotations and scales and ImageIO's decode all work (userland/src/vt_test.c, run 234735). There is no
  supported switch (per-request flags in Photos).
- **Uncommitted, mine:**
  - userland/src/eagl.c: the table lock (32 places); each move of the EDR factor logged; two-plane plane stats; a hand-written
    glUniformMatrix3fv; CoreAnimation's YCbCr shader text logged once. GL library 17af903e.
  - userland/src/vt_test.c (new).
  - out/mk-current-root.sh copies vt_test.
  - ../oneplus8-audit/camera-native/run_cam_probe.py has `slideshowlog` (applog_name=MobileSl).
  - docs: TODO, KNOWN_ISSUES K27.
- **The owner's photos:** the 15 test roots that held them were deleted on 2026-11-05 (the supervisor's privacy rule), along
  with the screenshots from those runs. Only out/hdr-photos/ (untracked, 0600) remains, kept for the HDR content view after
  Metal; delete it after that. The shared root has no photos (27dbc8a2).
- **Scripted Photos runs on a new /var:** the first launch puts "What's New in Photos" with a notifications alert on top;
  answer the alert (Don't Allow, about 0.32 0.59) before Continue (0.5 0.895).
- **Next: Metal over Vulkan** (the owner, 2026-11-05, relayed: "Metal first"). Items 11/20 (fps) come after Metal.
  - **M1 passed on the phone** (run 002111): turnip on HKGPU fills a 1 MB buffer, reads back right, no fault.
  - Files (uncommitted): tools/mesa/turnip-darwin.patch (new), tools/mesa/hackios-drm/{hk_drm.c,xf86drm.h},
    userland/src/vk_test.c (new); docs/BUILD.md (the turnip build). Built in the container's
    xnu-deps/build/mesa/build-turnip (glslang-tools installed there).
  - The next gallium build carries the new shim (DRM_CAP_SYNCOBJ still 0 there): check OpenGL ES once before installing it.
  - **M2 passed** (run 002902, a SPIR-V triangle in sysmem, sysmem+UBWC and GMEM). **M3a passed** (run 012447):
    Metal through turnip, AIR -> NIR. Both back ends in one boot (HK_METAL_BACKEND, metal_drv_test --backend,
    air_metal_test_gl/_vk); fragPos matches Gallium.
  - Files for M3a (uncommitted): tools/mesa/air/air_vk.c (new); in the Mesa tree, tu_hackios.cc and
    tu_hackios_exports.txt (in turnip-darwin.patch, now 304 lines); userland/src/hkmetal.m (mesa_available);
    userland/src/metal_drv_test.m (--backend, --rtfmt, --clear, the digest line, rwa_vk_counts);
    tools/mesa/air/air_metal_test.c (--gpu).
  - The Mesa tree's .redwood-patches no longer matches tools/mesa/*.patch, so the next tools/mk-mesa.sh run
    re-extracts the tree and deletes both build dirs. Rebuild build-turnip after it (docs/BUILD.md).
  - **M3b passed** (runs 020036 + 020900): buffers, textures and samplers through turnip (one push-descriptor set).
    The whole AppleMetalGLRenderer corpus, 271 fragment shaders, on both back ends: 259 the same, 1 different only
    in channels the shader leaves undefined, 0 real differences, 3 refused by vk alone (3D textures), 8 failed on both
    (multisample, [[depth]]). Details in TODO T38 (12).
  - Files for M3b (uncommitted): tools/mesa/air/air_vk.c (4104 lines; the copy in the Mesa tree is the same file);
    userland/src/metal_drv_test.m (drawall-result/-summary lines); tools/mesa/air/air_metal_test.c (16 pixels and
    FNV-1a); out/rc.jobs.m3b, out/rc.jobs.m3b2. libvulkan_freedreno 7fe7b0bbd4094b20.
  - **M3c-1 passed** (run 023037): 3D textures on vk (the three M3b refusals now match Gallium), and
    userland/src/hkmetal.m gives a command buffer's fence back at completion (vk in use during --draw-all: 0, was 208).
    Files: air_vk.c (4284 lines), hkmetal.m (-hkWaitFence), metal_drv_test.m; out/rc.jobs.m3c1. libvulkan_freedreno
    30c9f9e25fd55ea0, libhackiosmetal bf0c6896dd530a86, HKMetal 3b3c6d02947261c3, metal_drv_test 2ae7f9676a4a55dc.
    The hkmetal change is on the Gallium path too: the installed system has the old library until the next install.
  - **M3c-2 passed on vk** (runs 030433 + 030853): depth and stencil. Corpus 271/271: 262 same, 1 undefined-only,
    0 different, 2 failed on Gallium alone (a [[depth]] store times the GPU out there: KNOWN_ISSUES K28), 6 multisample
    on both. libvulkan_freedreno f50f681dbb6fd686, metal_drv_test f6a04ecd38f91d69; out/rc.jobs.m3c2a/b.
  - **M3c-3 passed** (run 034547): multisample and resolve. vk draws all 271 corpus shaders (266 the same as Gallium,
    2 undefined-only, 0 different, 3 that fail on Gallium alone: K28). K28 is the Metal path only: GLES gl_FragDepth
    works (gl_test fragdepth, new). libvulkan_freedreno 99b1e65b4fe1eb2d, metal_drv_test 3418bb2b11972a7e, gl_test
    025b1a917eb93e03; out/rc.jobs.m3c3. Files: air_vk.c (5813 lines), metal_drv_test.m, gl_test.c.
  - Next: compute, then IOSurface (texture_from_surface), then argument buffers, then M4.
  - **M3d run** (runs 043116 + 043536; TODO T38 (12)): compute on vk, 45/52 the same as Gallium, 6 3D pixel-coordinate
    kernels refused by Vulkan's rule (M3d-2 lowers them), fbs_pcg_iter2 racy on both; sysvals to re-run (stale test build). air_vk.c (6653 lines; the Mesa tree's copy is the same),
    metal_drv_test.m (compute-result lines, --compute-all), air_metal_test.c (compute-result, GROUPS THREADS),
    tools/air/sysvals.ll (new; userland/obj/sysvals.bc must be at /hackios/share/probes). hkmetal.m unchanged.
    libvulkan_freedreno fc6f028849b2093f, metal_drv_test 50c63ea159289eda, air_metal_test_gl f0235a027076be7e,
    air_metal_test_vk 1a431295fb7c3c66. Jobs out/rc.jobs.m3d1, out/rc.jobs.m3d2.
  - **M3d-2 passed** (run 050912): sysvals and tools/air/sample3d.ll equal the host values on both back ends; the six 3D
    pixel-coordinate kernels get a pipeline. **Erode settled** (run 054947): offsets right on both (sample3d_off);
    half overflow is +inf on vk, 65504 on Gallium (KNOWN_ISSUES K29).
  - **M3e run** (run 054947): IOSurface textures on vk (dma-buf import by surface ID). Imports, clears, copies,
    refusals and drawable_test match on both; one open item, a draw into an RGBA16Float imported surface differs on vk.
    CPU reads after GPU writes were seen stale on both back ends (a kernel item, TODO T38 (17), with the RGBA16F row
    alignment). libvulkan_freedreno 51efa75f271a052d, libhackiosmetal 60092d1c9c6d47c0, HKMetal ff931e59eec9277a,
    air_metal_test_gl/_vk 77a969cb/6776441c, drawable_test 7288672b; turnip-darwin.patch 333 lines. Jobs out/rc.jobs.m3e.
  - **M3e passed on vk** (runs 060507-062853): the RGBA16F mismatch was the CPU's stale cache, not vk. air_vk now maps
    an imported surface once so the kernel treats it as seen (as Gallium's are): 9/9 right. iOS's sys_dcache_flush is a
    no-op here (a barrier and a return), so the kernel must keep GPU-shared memory coherent: TODO T38 (17), kernel
    round. libvulkan_freedreno a2b44a1c63233269 (7376-line air_vk.c), air_metal_test_gl/_vk 6aea8d8e/852e457b.
  - **M4 first numbers** (run 071517, 525 MHz, cpufreq_gov=0): in steady drawing vk uses 0.57-0.81x Gallium's GPU time,
    but its CPU sys is far higher: every vk wait is a busy poll (a clock mismatch) and writes and destroys submit on
    their own. **SMMU faults on vk found and fixed** (a second thread recording via rwa_finish on empty command
    buffers; verified run 080405; libvulkan_freedreno 9f625b63388cc528, metal_drv_test 61fcc2ab463f2c3e). TODO T38 (12).
  - run_cam_probe.py (oneplus8-audit/camera-native) now also takes cpufreq_gov=[01] (lines 27 and 46).
  - **M4-1 verified, M4 closed** (runs 084829-091116): vk really sleeps now, and in steady drawing it uses 0.57-0.8x
    Gallium's GPU time, CPU no worse apart from the seen surface's kernel clean. libvulkan_freedreno cba626e5552d5aa9,
    metal_drv_test c28bc6e6687e45c4. **The owner: Metal goes over Vulkan; no more cost comparison.** Correctness checks
    stay. VkPipelineCache stays in TODO. The hyp dump (partdump, out/hypdump-root.apfs) is built, not run, shelved by
    the owner.
  - Next: M5 (complete Metal on vk, TODO T38 (12)): M5a blending, vertex input, indexed draws, geometry and pass state.
  - **M4-1, the cost part: built, NOT run** (TODO T38 (12), its entry has the design): the wait clock (tools/mesa/hackios-drm/{hk_drm.c,xf86drm.h}, the
    Mesa tree's tu_knl_drm.h, tools/mesa/turnip-darwin.patch), deferred writes, deferred destroys and a staging ring (tools/mesa/air/air_vk.c), --order
    (userland/src/metal_drv_test.m). libvulkan_freedreno cba626e5552d5aa9, metal_drv_test c28bc6e6687e45c4. Jobs out/rc.jobs.m4r1, m4r2, m4r3. Uncommitted.
    A pristine copy for the patch is in the session's scratchpad (mesa-pristine, now with tu_knl_drm.h).


## Sound delay (item 18), screenshots (item 7), display (items 11/17) — 2026-11-04 19:3x-20:0x (camera/backlog session; RAM only)
- **Item 18, done in RAM** (TODO T38 (18)): `xnu-src/.../bsd/dev/hk_sound.c` -- 8 x 10 ms periods (80 ms queued at the DSP),
  amplifiers off after 30 s of silence, no silence kept while parked (the first sound clears the ring), silence over 43 ms at
  the ring's front dropped, 60 ms of silence leading the first sound after a park (UNPARK_LEAD_PERIODS). The 0.68 s backlog
  of a parked ring was the real delay (run 193817). Kernel `BUILD_CAM` 9b50e69b (20:0x). **Export hk_sound.c** with the rest
  before an install. New log lines: "a sound after silence (N) written at T, with M ms of the ring ahead of it", "N underflows
  at the DSP so far", the feeder's stop line with the ring's longest wait and what was dropped.
- **Item 7 closed in RAM:** chord and physical VolUp+Power both save a PNG (runs 192253, 192832). rwtouch has `chord`.
- **init splits an rc.jobs line into 63 words at most** (the rest becomes one word, which rwtouch refuses): keep job lines
  short (run 193817 stopped at its k10).
  - **M5c, built, rows pass on real Metal, phone runs waiting on the notice** (TODO T38 (12)): argument buffers, a draw's stores, arrays of descriptors and
    function constants on vk. out/rc.jobs.m5c1-3, tools/air/m5c.metal, tools/air/m5c_rows.h, tools/air/mtlref/airdump.m; out/msl/m5c.metallib is made by
    airdump on this Mac. libvulkan_freedreno dc58c35cad96014b, 51 exports. Uncommitted.
  - **M5b, run: results in TODO T38 (12)** (runs 111109-112410, no fault; the sRGB rows wait for the curve): textures, samplers and formats of air_vk.c: levels, slices, every type, a pass into a level or slice, views
    (`rwa_texture_view_create`, the one new export), border colours, generate_mipmaps, the packed 32-bit, ETC2/EAC and ASTC formats. Jobs out/rc.jobs.m5b1-4.
    draw_model.h's blend model now quantizes first, as the hardware does: run 094931's 92 rgba8/bgra8/rgba16f/rgba32f blend pictures reproduced whole on the host.
  - **M5a, built, NOT run** (TODO T38 (12) has the design): the draw path of air_vk.c (blending, vertex input, indexed draws, cull,
    winding, viewport, scissor, primitive types, per-attachment clears). Uncommitted: tools/mesa/air/{air_vk.c,air_metal.h,air_metal_test.c,
    draw_model.h}, tools/air/m5a_*.ll, userland/src/{hkmetal.m,metal_drv_test.m}; jobs out/rc.jobs.m5a1, m5a2, m5a3. userland/obj/m5a_*.bc
    go to /hackios/share/probes. Exports still 48; Gallium's library not rebuilt (it reads rwa_draw's old prefix).
- **Item 8 deferred until after Metal** (the owner); prepared out/rc.jobs.item8. **Items 11 + 17 in progress:** the panel's
  TE is 90 Hz; the owner's browser 45 is probably Safari's PreferPageRenderingUpdatesNear60FPSEnabled (60 on 90 -> 45), being
  measured (run 70, out/rc.jobs.item11, `touchlat`).
- Run flags added to run_cam_probe.py: `touchlat` (touch_lat=1), `secs=N` (loader_return_after), `hbm`, `edr`, `photos`;
  `CAM_KERNEL=out/kernels/cam-<sha>.mach`: every run now sends a kept copy of the kernel and quotes its SHA (a build
  during a run had changed what was sent, run 203539).
- **Item 17 (HDR), 20:0x-21:1x:** HBM verified by the owner (bsd/dev/hk_hbm.c, `hk_display_hbm()`, boot-arg hbm=1, limits
  approved by the supervisor: 30 s, 60 s cooldown, 180 s per 10 min, battery 38 C, hottest 60 C, off on dark/sleep/
  loader/panic -- the last unconditionally with hbm=1). The EDR step in eagl.c verified (255 -> 188). CoreBrightness's
  new architecture behind edr=1 (MiniAppleARMBacklight.cpp): taken, but its slider is broken (TODO T38 (17)); edr stays
  test-only. **For an install:** the shared root now carries libhackios-gl with the EDR step (inert at factor 1),
  libhackios-sb with a PhotoKit importer that registers only with photos_import=1 (keep it off), power_test hbm, prefs_set,
  and com.hackios.prefs (Safari's near-60 switch off at every boot: the owner's choice, item 11). Kernel files to export:
  hk_hbm.c (+ bsd/conf/files), MiniIOMobileFramebuffer.cpp (HBM; another session's uncommitted AMB655 changes are in the
  same file), MiniAppleARMBacklight.cpp, hk_thermal.c (hk_thermal_now), hk_sound.c.
- Wi-Fi in test images: runs needing the network use the kept UFS /var (ufs_write=0: read-only there, EROFS); a ramhfs
  /var has no known network. mk-current-root.sh installs fw-wifi/network (0600) when the owner has made it; never read it. An image with it holds the passphrase in the clear, so mkapfs.py makes that image 0600; never share one.

## Sound (backlog item 2) and the io_map panic — 2026-11-04 18:5x (camera/backlog session; RAM only)
**Verified by the owner in RAM with the installed combination** (run 185457: wifi=1 + cam=5 + sound, "聲音正常"; vibration OK
in 183207); io_map with everything on: **9136 KB** of the 64 MB (over Apple's 8192 KB: the cause confirmed).
**The eventual install (not now; the owner: no UFS writes) must carry, together with items 1/16:** kernel with hk_sta.c,
hk_apple80211.c, lx_80211.c (items 1), hk_adsp.c (fw lookup), loader_return.c (amps before Wi-Fi), io_map.c (64 MB) -- export
hk_adsp.c, loader_return.c, io_map.c first; root = camera-mgfix-root + fw/ with fw-audio/* (adsp.mdt, adsp.b00-b18,
tfa98xx.cnt) and fw-haptic/aw8697_haptic_170.bin, rc.jobs the stable one; cmdline = out/camera-install-cmdline.txt minus
`gpu_pt=1 gpu_recover=1 var_data_format=0` (defaults) plus `sb_clock=28 wifi_dwell=1 adsp=1 sound=4 sound_amps=1 sound_below=96
haptic=5 haptic_trim=11` = 584 B of the loader's 607 (with its 187 B prefix).
Also for that install (items 5/6, 2026-11-04 19:2x): the root's var/db/timed/com.apple.timed.plist and init with timed_prefs()
("Set Automatically" + NTP; item 6, verified in RAM run 191643 on a new /var; the kept-/var path of timed_prefs() is
untested until the install; Time Zone keeps spinning without Location Services, known, left); item 5 (Bold Text) needed nothing beyond gpu_pt/gpu_recover on.

No sound / no vibration on the installed bundle: its cmdline has none of the profile's `adsp=1 sound=4 sound_amps=1
sound_below=96 haptic=5 haptic_trim=11`, and its image no audio firmware. Done in RAM (runs 183207-184851): vibration works with
the args alone; the speakers need the audio DSP's image (`fw-audio/adsp.mdt`, `adsp.b00-b18`, read only from `modem_a`'s FAT
/image with the loader's `oem fwread`: `../oneplus8-audit/firmware-reader/read_adsp_fat.py`) and `tfa98xx.cnt` (the ODM
capture's /firmware), both untracked; `hk_adsp_fw_file()` (hk_adsp.c) now looks in the small image's fw/ first (with the iOS
volume as the root there is no /fw). Sound verified by the owner ("有聲音", run 184551, wifi=0 no cam). With the camera too it
panicked: `kmem_alloc(..., 1048576, ...): failed with 3` from camera_thread -> instantnoodle_gpio_cfg -> pin_base -> io_map:
**`osfmk/arm/io_map.c` IO_MAP_SIZE 8 MB (Apple's) was full** -- our drivers' cached ml_io_map() register maps (TLMM tiles of
1 MB, GCC, DSPs, mics, camera, PCIe) share it, KMA_NOFAIL. Now 64 MB, and loader_return prints "io_map: N KB of the io submap's
65536 KB mapped". **Export note:** io_map.c becomes a new entry of hackios.patch (Apple file); every session's drivers get the
room. Longer term: map only the TLMM pages used, not 1 MB tiles. Profile args fit the installed cmdline when gpu_pt=1
gpu_recover=1 var_data_format=0 (defaults) are dropped (584 B of 607). Test image `out/sound-test-root.apfs` (rc.jobs = the sound
check); run flags `lean`, `sound`, `nocam` in run_cam_probe.py.

## For the Wi-Fi session: a panic with wifi=1 loses the panic text — 2026-11-04 18:4x (camera/backlog session)

Run 184025 (sound test, wifi=1): "psci: cpu 2 ends the system" then "wifi: UNCLEAN PCIe DMA OWNERSHIP; loader return halted,
cold recovery required" -- `hk_wifi_quiesce()` (`pci_diag_halt()`, hk_wifi.c, uncommitted) halts on loader_return after a panic
(the orderly Wi-Fi shutdown not run) before the panic's text is copied into the loader's log: no message, and the phone hangs
until a cold restart. You may want to copy the panic text (or the loader log) out before halting. Meanwhile sound/other
panic hunts run with wifi=0. Also: `osfmk/arm64/loader_return.c` (it had uncommitted changes of another session) now calls
`hk_sound_quiesce()` before `hk_wifi_quiesce()`, so the speakers' amplifiers are held in reset even when the Wi-Fi halt
follows (one line added, nothing else touched).

## Wi-Fi list, 5 GHz delay, status-bar bars; the clock — 2026-11-04 18:2x, verified on phone in RAM (the owner: "掃描改善了", "格子黑了，時間 OK")

The owner's backlog item 1 (Wi-Fi) and item 16 (the clock under the camera hole). Found (runs 155842, 175404, 181543, 182409; RAM):
(a)/(c) wifid scans 2.4 GHz (11 channels) and 5 GHz (25) apart, and each scan's results replaced the list (lx clears its heard list
per scan): networks a scan missed dropped out, 5 GHz came seconds late. Fix `bsd/dev/hk_sta.c`: what was heard in the last
`wifi_scan_keep` s (120; 0: as before) stays in what is read, with its real age -- wifid reads 24-55 per scan (3-22 before).
Scan time: ath11k's own dwell (50 ms active, 150 passive; 16 of the 25 5 GHz channels are DFS) and 50 ms rest per channel
when joined. Fix `wifi/lx/src/lx_80211.c` station_scan: `wifi_dwell=1` (or wifi_dwell_active/passive=) makes one scan two
driver scans, the channels it may send on at 40 ms, the DFS ones at 110 (Android's WCNSS_qcom_cfg.ini values): 5 GHz 2.96->2.26 s
unjoined, 4.2->3.8 s joined, 2.4 GHz 0.63->0.5 s; the list keeps 12 DFS networks (ath11k itself untouched).
(b) the bars: always 0 (grey). wifid's bars come from its RSSI cache (`__WiFiDeviceProcessRSSIEvent`: scaled = 0.5 + x/(2*sqrt(x^2+450)),
x = rssi+77.5; SpringBoard 3 x it, 1..3), fed by event 39; NOISE/RSSI_BOUNDS do not matter (subagent, 20F66 disassembly; the
Apple Wiki's MobileWiFi page agrees on kWiFiScaledRSSIKey). Fix `bsd/dev/hk_apple80211.c`: event 39 (0xb4 bytes: [0]=1, [4]=RSSI
dBm, the rest 0) 1 s after a join and every 3 s while joined. Also: its request log and the station's events now go into the
loader's log (the kernel's printf is paused once iOS runs). Clock: `sb_clock=28` (inject_sb.c; the profile had 24, the
installed cmdline none). Exported (xnu/export.sh: only hk_sta.c, hk_apple80211.c changed). Diagnostic image (not for install):
`out/wifi-diag-root.apfs` = camera-install-root + OS_ACTIVITY_DT_MODE for SpringBoard and wifid.
**Install candidate** `out/instantnoodle-wifi-bars.img` gen 1793788085 cd0743a2...: kernel `out/wifi-install-kernel` 10fd7b0d...,
root `out/camera-mgfix-root.apfs` cf335620 (+ the camera stamp init), resident flashboot.apfs 3fb7b93a, cmdline
`out/wifi-install-cmdline.txt` = the installed one + `sb_clock=28 wifi_dwell=1`. Uncommitted: xnu/files/bsd/dev/hk_sta.c,
hk_apple80211.c, wifi/lx/src/lx_80211.c, userland/src/init.c, run_cam_probe.py (sb_clock=, wifi_scan_keep=, wifi_dwell= pass-through).

## Installed loader: a quiet, faster boot from the flash — 2026-11-04 17:4x, verified on phone (the owner: "Yes, fast and quiet", Camera and Wi-Fi "都正常")

The owner's backlog item 15 ("走內部UFS啟動的話，會print 日誌…因為日誌刷新efifb很卡，導致開機很慢"). Cause: the loader drew its whole
log (~300 lines, 20 KB before the kernel) on the firmware's text console a character a call, a 1080x2400 scroll a line (the
UEFI itself prints nothing: its FD is Mu-Silicium's RELEASE build). Fix (`xnuloader/loader.c`, `xnuloader/flashboot.c`, uncommitted;
loader.c also carries another session's earlier uncommitted changes, which the installed loader already had): the installed
boot's screen has the menu and one "starting <label>" line (with a framebuffer; QEMU's serial keeps the log), the screen gets a
line a call, `time: N ms since power-on` lines in the log, and `oem readpart boot_a|boot_b` (read only: the first 63 MB staged,
the whole partition's SHA-256 logged). QEMU `common/flash/qemu-test.sh boot previous guard` 3/3 (LOADER_EFI= new; sgdisk from
the build container). **The UEFI is in `boot_a`** (ABL: current-slot a; `boot_b` = OxygenOS's boot): docs and the profile fixed.
The loader must be built with this phone's userdata GUID (`LOADER_DEFS`, out/backup/loader-build-defs.txt, not in git): a RAM
boot without it had no HackiOS in the menu. Image `out/instantnoodle-uefi-fast.img` 8156abcc6df0e57b... (= boot_a's image with
only the loader payload: `out/repack_uefi.py`), RAM-tested (`fastboot boot`: quiet, HackiOS, Developer mode), then
`xl flashpart boot_a` (EXPECT 6c9c5e3a..., read back) at 17:39. boot_a now: whole SHA-256 e286be3d8e8f0e6e738b3c41050a1929a10261e73b04f019359db0a97f9a1f66.
Backups (never delete): `out/backup/boot_a-20261104-6c9c5e3a.img` (before), `boot_b-20261104-a13126e8.img`, `boot_a-20261104-after-fast.img`,
copies in `../../outputs/boot-backup-20261104/`. **Undo:** `./xl flashpart boot_a out/backup/boot_a-20261104-6c9c5e3a.img e286be3d8e8f0e6e738b3c41050a1929a10261e73b04f019359db0a97f9a1f66`.
Unplugged boot (logs/lastlog-ufs-after-fast.txt): the loader at 13678 ms after power-on, the menu 13684, chosen 13788 (by
hand), the bundle read and checked 14463, the kernel next 17876 (3.4 s of run_kernel: not looked into). Before: no time
lines (the old loader; logs/lastlog-ufs-before-fast.txt, 447 lines on screen). Open: the stock fastboot's key combination
from power-off is not confirmed (the menu's "Bootloader (fastboot)" works).

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
- CURRENT 2026-11-03: exact hardware target audit passed (instantnoodle FD, payload and loader verified). An owner photo proves direct `ios-instantnoodle` probe runs (`BOARD instantnoodle`, live FDT, 30s countdown); ADB returned at 36s. That probe carries no XNU payload. Header-normalized UEFI still crashed; diagnosis below supersedes speculative header/USB explanations.
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
