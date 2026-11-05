# Known issues — what does not work on this device (right now)

Answers **"what currently doesn't work?"** Entries are things a user or developer will notice; work to fix them is in [TODO.md](TODO.md) (`T..`), not repeated here.
Primary device: **OnePlus 8 (instantnoodle)**; redwood and QEMU are marked where different. Audited 2026-10-26 against the source and the project's records.

**Kind:** *Unsupported* (no implementation) · *Bug* (implemented, wrong; cause known or not) · *Partial* · *Unreliable* · *Unverified* (implemented, never seen working).
**Severity:** **S1** blocks basic use or safety · **S2** a major iOS feature missing/broken · **S3** noticeable defect · **S4** minor/cosmetic.
Nothing is marked "works" merely because a driver exists: evidence is stated. For the list of what *does* work see [devices/](devices/README.md).

## Summary

| ID | Area | Issue | Kind | Sev | OnePlus 8 | Redwood |
|---|---|---|---|---|---|---|
| K01 | Graphics | Metal is experimental; most Metal features and apps untested | Partial | S2 | off by default | not applicable |
| K02 | Security | No fingerprint / biometric unlock | Unsupported | S2 | yes | yes |
| K03 | Camera | OnePlus 8: Camera.app on the front sensor only (`cam=5`, RAM runs), no tuning, warm; redwood: no video | Partial | S2 | partly | works (no video) |
| K04 | Audio | No headphones/USB-C audio, earpiece, call audio; one mic only | Unsupported / Partial | S2 | yes | partly |
| K05 | Connectivity | Partial Bluetooth; Wi-Fi gaps and unverified sleep | Partial / Unverified | S2 | yes | yes |
| K06 | Telephony | No cellular, calls, SMS, voicemail | Unsupported | S1 | yes | yes |
| K07 | Sensors | No accelerometer/gyro/compass/light/proximity/barometer | Unsupported | S2 | yes | yes |
| K08 | Battery | Charger/shutdown paths unverified; no drain figure; no fast charging | Unverified / Unsupported | S1 | yes | partly |
| K09 | Display | Brightness optical effect unverified; rendering gaps; ~2% late swaps | Unverified / Bug | S3 | yes | |
| K10 | Power | Sleep is shallow; drain asleep unknown; heavy wake CPU | Partial | S2 | yes | no sleep |
| K11 | Services | App Store LOADING; no Apple ID, iCloud, push, FairPlay | Unsupported | S2 | yes | yes |
| K12 | Location | No location services | Unsupported | S2 | yes | yes |
| K13 | Security | Security stand-ins accept everything; keychain protects nothing | By design | S3 | yes | yes |
| K14 | Persistence | RAM-only runs lose all data; Wi-Fi password not kept | By design | S3 | RAM runs | RAM runs |
| K15 | UI | Poster switcher black; shadows under switcher cards; CC glyph strokes | Bug | S3 | yes | |
| K16 | Boot | Stock ABL breaks with USB cable in; short menu countdown; no recovery image in a fresh clone | Limitation | S2 | yes | |
| K17 | Media | No video/Live Photo recording; portrait mode skipped | Unsupported | S3 | yes | yes |
| K18 | Thermal | Throttling never observed | Unverified | S3 | yes | yes |
| K19 | Testing | Scripted gestures do not reach system gestures; switcher not openable by script | Limitation | S4 | yes | |
| K20 | Redwood | Not re-verified | Unverified | S3 | | yes |
| K21 | Installed build | Installed bundle lags the source | Limitation | S3 | yes | |
| K22 | Agent toolkit | New USB mailbox/control path awaits owner-approved RAM loading | Unverified | S3 | yes | |
| K23 | Scheduling | Edge: rare 10-11 ms render frame under heavy DF load; BG starved when UT saturates E; energy model unvalidated | Partial | S4 | yes | untested |
| K24 | Testing | `sleep` check can fail a correct sleep (continuous clock 0.4 s short once) | Bug (test) | S4 | yes | |
| K27 | Photos | An HDR photo opened full size shows its grey gain map (no Metal) | Partial | S3 | test roots | |
| K28 | Metal (Gallium) | A Metal fragment shader that stores [[depth]] times the GPU out on the Gallium back end (recovered); GLES gl_FragDepth is fine; vk draws it | Bug | S3 | test runs | |
| K29 | Metal (Gallium) | A half-precision result that overflows is stored as 65504, not +inf (vk: +inf) | Bug | S4 | test runs | |
| K30 | Metal (Gallium) | Metal lines and points draw nothing on the Gallium back end (vk draws them) | Bug | S3 | test runs | |
| K31 | Metal (Gallium) | Packed 16-bit formats mirrored against real Metal; the source of the exchange the wallpaper showed is open | Bug or unexplained | S3 | system with metal= | |
| K32 | Metal (Vulkan) | An argument buffer is read at the command buffer's commit, not when the GPU runs it | Limitation | S3 | metal_drv_test --m5c | |

---

## K23 Scheduling (Edge) — OnePlus 8 · **Partial** · S4
- **Observed (2026-10-26, RAM runs, `sched_test`):** a 120 Hz render loop with 7 busy default-QoS threads misses about 1 frame in 1400 with a
  10-11 ms frame under Edge (Clutch: 1 in 1440, ≤ 8.7 ms), while Edge's p50/p99 are better; background-QoS threads get ~5% of the E cluster when
  utility threads saturate it (by design: BG does not go up to P, `sched_edge_bg_up=1` restores Apple's code); the controller's energy figures are
  a C·f·V² model, not measured power. Frames after 5 rapid sleeps drop to ~66 fps on **both** schedulers (pre-existing, see K10).
- **Workaround:** `sched=clutch` boots the previous scheduler. **Work:** T11. **Evidence:** [scheduler.md](subsystems/scheduler.md#measured-oneplus-8-ram-runs-same-kernel-schedclutch-vs-default-edge).

## K24 `sleep` check false failure — OnePlus 8 · **Bug (test)** · S4
- **Observed once (2026-10-26 04:4x, Edge):** `t-sleep: FAIL n=1 want=3 cont=2.60 ... the continuous clock did not run the time asked for`, the
  other sleep of the run passing; other runs gave 3.3-4.1 s. The continuous clock after a sleep is corrected from the PMIC RTC (1 s resolution),
  so a short reading of up to ~1 s is possible; the check allows -0.3 s. **Work:** widen the lower bound or correct with sub-second time. Not a scheduler effect.

## K25 A run's log loses the kernel's message buffer — tools · **Bug** · S3
- **Observed (2026-10-27, `023834`):** the kernel returned 1.26 MB of log, `logs/<time>-run.log` came back with 24 KB and no `== KERNEL LOG ==`
  section; the last log that had one is `000342` (2026-10-26). Kernel `printf` lines (any driver without its own `say()`) are only in that section.
- **Cause (source):** `xnuloader/loader.c` `oem log` while not busy calls `compose_logs(NULL, 0)`, replacing what `write_logs(klog)` staged at the return.
- **Workaround:** drivers print to the loader's log too (`say()`, as `hk_buttons.c`); `tools/t --keep` keeps the live copies. **Fix:** loader (reload deferred by the owner).

## K26 `pods` fails while iOS shows "Low Battery" — OnePlus 8 · **Bug (test)** · S4
- **Observed (2026-10-27, `024633`, `024827`):** battery 19-20 %: the alert covers the App Library, `pods` reads 202 against 212. Charge, or close the alert.

## K27 Photos shows an HDR photo's gain map at full size — OnePlus 8 · Photos · **Partial** · S3
- **Observed (2026-11-04, runs 231531-233935, test roots only, verified on phone):** with libhackios-gl in assetsd and
  MobileSlideShow, Photos imports and lists HDR HEICs in colour, but a photo opened full size is grey (the owner: "顯示黑白了"):
  what shows is its HDR gain map (a monochrome auxiliary image). A non-HDR JPEG's thumbnail is in colour; its full-size view was not
  opened.
- **Cause, as far as found:** Photos' PHFigDecoder decodes the base through CMPhoto's accelerated path and gets -12914
  (kVTImageRotationNotSupportedErr). Its options include AccelerationMode, TiledDownsampling, ForceHighSpeed and a crop. CMPhoto probes
  AppleM2ScalerCSCDriver and links Metal and MetalPerformanceShaders; neither the scaler nor Metal exists here. VideoToolbox's own
  rotations, scales and ImageIO's full decode all work (userland/src/vt_test.c, run 234735), so it is that path, not VideoToolbox as
  such. The other HEIC logged no decode error yet showed grey too, so the HDR composite step without Metal may also fall back.
- **No supported switch:** the decoder's choices are per-request flags in Photos' code, not preferences.
- **Workaround:** none. Forcing Photos to its PHImageIODecoder (a hook, not done) would at best show the SDR base in colour;
  the HDR highlights need Tungsten's Metal renderer ("Metal Device is nil ... degraded experience"). **Work:** T38 (17), after Metal (12).


- **Since, on Metal over Vulkan (2026-11-05, runs 165643, 170709, test roots only, verified on phone):** the photo at full view is in colour and no longer the gain map, with no crash. **Not HDR:** the picture's highest pixel is at SDR white. PHFigDecoder still gets -12914. See K33 and TODO, M5e.

## K33 HDR photo on Metal over Vulkan: the screen is dimmed with no highlights — OnePlus 8 · Photos · **Open** · S2
- **Observed (2026-11-05, runs 165643 and 170709, RAM, shots):** at full view Photos' white is 187 of 255, about half in linear light, as CoreAnimation does for EDR; the picture's highest pixel is 191; brightness-nits stays at 70-83 where run 231531's notes (OpenGL ES compositor) have it rising to 140. Same with and without hbm=1.
- **Not known:** whether the compositor on hk-gl does the same with this photo: that boot has not been done. The drawable is BGRA8 and no RGBA16F surface appears in Photos.
- **Consequence:** dimmer than SDR and no HDR. Not to reach an install until explained.

## K28 Metal on Gallium: a fragment [[depth]] store times the GPU out — OnePlus 8 · Metal · **Bug** · S3
- **Observed (2026-11-05, run 030433, RAM test root, verified on phone):** metal_drv_test on the Gallium back end
  (libhackiosair, the default Metal path) with AppleMetalGLRenderer's texturedFragment2DDepth and texturedFragment3DDepth, each
  given a Depth32Float attachment: the submission never finishes ("adreno: gpu: no answer: submission 2901 ... the core has done
  2888", hkmetal "THE GPU DID NOT FINISH ... within two seconds"); the kernel resets and recovers the 3D core each time (4 times in
  that boot) and the jobs after it, the display included, go on. The same two shaders draw right on the Vulkan back end
  (air_vk.c over turnip, same ir3 compiler): the stored depth reads back as sampled.
- **What it is not:** a depth attachment alone works on Gallium (--draw-depth with clearFragment20: the test discards as on vk,
  Depth16/32/32_S8), and 3D sampling alone works (texturedFragment3DFloat). texturedFragment3DDepth binds an ordinary 3D colour
  texture and hangs too, so the common factor is the fragment stage writing depth on freedreno's Gallium path. Until M3c-2
  (2026-11-05) both back ends refused these shaders at pipeline creation, so the path had never run.
- **Not the driver (run 034547):** the same store through OpenGL ES, `gl_test fragdepth z32f|z16` (GLSL ES 3.00
  gl_FragDepth through libOSMesa, the window server's FD650 driver), reaches the depth buffer and finishes in 1.8 ms. So
  the window server, WebKit and GLES applications are not affected; the fault is in what the Metal path (air_metal.c,
  the AIR frontend's NIR) hands Gallium. metal_drv_test's sweeps refuse such a shader on Gallium by name since M3c-3.
- **Workaround:** none on Gallium; no system UI shader known so far writes [[depth]]. The Vulkan back end (`metal_backend=vk`,
  TODO T38 (12)) draws them. Not investigated further, because Gallium is to be replaced by it.

## K29 Metal on Gallium: half overflow saturates instead of giving +inf — OnePlus 8 · Metal · **Bug** · S4
- **Observed (2026-11-05, run 054947, RAM test root):** PortraitFilters' MetalFaceMask_ErodeCube* multiply three half
  samples; where the product passes 65504, the Gallium back end (libhackiosair, freedreno) stores 0x7BFF (65504) and the
  Vulkan back end stores 0x7C00 (+inf). A host model reproduces both back ends' digests exactly, and every product that
  fits is identical on both. IEEE binary16, and Apple's GPUs, give +inf. Likely freedreno's fp16 conversion saturating
  (inferred, not traced). Matters only to shaders that overflow half; the Vulkan back end does not have it.
- **Also a Gallium deviation (read from air_metal.c, 2026-11-05):** primitive restart is off there, while Metal always restarts on
  strips (third-party sources in air_vk.c's THE DRAW PATH; no Apple statement found), so a strip with a restart index is refused or wrong on Gallium.

## K30 Metal on Gallium: lines and points draw nothing — OnePlus 8 · Metal · **Bug** · S3
- **Observed (2026-11-05, run 094931, RAM test root):** air_metal_test --draw-vertex --pixels on the Gallium back end: line list, line strip
  and point list leave the target empty (the coverage map has no pixel). air_metal.c's rasterizer state has line_width 0 and
  point_size_per_vertex 0 (read; the likely cause, not yet confirmed by a fix). So Metal's lines and points have always been broken on the Gallium
  path, and the Vulkan back end, which draws all three, is the right one. Not fixed, since Gallium is to be replaced.

## K32 Metal on Vulkan: an argument buffer is read at commit, not when the GPU runs — OnePlus 8 · Metal · **Limitation** · S3
Real Metal's GPU reads the handles when it runs, so a set encoded again after a draw is encoded and before the command buffer runs has its new contents for both (measured on an M1 Max, `argbuf-reencode-one-cb`). hkmetal.m's deferred work records the contract draws at `-commit`, so the same row gives the same pixels on the phone (verified on phone, run 124507): a set encoded again any time before commit is seen. What is left is a set changed after commit and before the GPU reaches it, which real Metal would see and this does not; no row and nothing in the corpus does that. Handles written by the processor, a kernel or a blit have no record at all and are refused by name (docs/air.md 8.2).

## K31 Metal on Gallium: the packed 16-bit formats are mirrored against real Metal — OnePlus 8 · Metal · **Bug or unexplained** · S3
- **Measured (2026-11-05, public Metal API on an Apple M1 Max, not A11):** in Metal's packed 16-bit formats the first-named channel is in the low bits:
  B5G6R5 0x001F b, 0x07E0 g, 0xF800 r; A1BGR5 0x0001 a, 0x003E b, 0x07C0 g, 0xF800 r; ABGR4 0x000F a, 0x00F0 b, 0x0F00 g, 0xF000 r; BGR5A1 0x001F b, 0x03E0 g,
  0x7C00 r, 0x8000 a. MoltenVK's table agrees.
- **What the Gallium back end does (air_metal.c):** the mirror of that, red low and blue high, chosen because the window server's wallpaper came out
  (255,12,12) where it is blue under the first reading (a picture no longer in this tree, docs/air.md). Removing the mirror would turn today's wallpaper red.
- **Measured (run 124103, `metal=2 metal_profile=1 camtl=1`, Gallium library, the compositor on Metal):** hkmetal's packed16 lines are one format and no swizzle: 43 BGR5A1, descriptor and view routes, swizzle r g b a on both, no surface, usage 0x5 (shader read and render target). So the compositor renders into it and reads it back, and the swizzle hypothesis below is refuted for that boot. The wallpaper is the right colour. Most of the rest of the home screen is missing (icons, labels, status bar): one small rectangle of the Clock icon is there, in all 7 shots by file size and in the one viewed.
- **On Metal over Vulkan (M5d, verified on phone, run 141516):** the wallpaper is right with the measured table and no mirror, and no format-43 texture of 100 pixels or more is bound in any draw. A red shape did appear on vk, and it was not this: it is the stock wallpaper's `Head Mask.HEIC`, red by design, showing because the layer it masks is a 420f surface that was being refused. So nothing says the mirror is needed on vk; whether the old (255,12,12) picture was the same mask is not known, and its run is gone.
- **Open:** who does the exchange. A B,G,R,A swizzle in the compositor would look exactly like it, and hkmetal ignored swizzles until M5b (hypothesis, unverified).
  The Vulkan back end now follows the measured table; a swizzling compositor could double-exchange there until this is settled. To be found before metal=2 on vk (M5d).
## K01 Metal graphics — OnePlus 8 · Graphics · **Partial** (experimental) · S2
- **Expected:** apps and the system draw with Metal like an iPhone. **Actual:** OpenGL ES (Mesa) draws the whole UI and is solid. Metal is an experimental driver, off by default (`metal=1` test programs;
  `metal=2 metal_profile=1` hands it the whole system and reaches the home screen). See the exact breakdown in [gpu.md](subsystems/gpu.md#metal-work-in-progress-instantnoodle-only).
- **Works (measured):** blits, clears, 4x MSAA resolve, fences/events/shared events (in-process), heaps (no suballocation), 263 of 271 system fragment shaders draw (258 compared agree with softpipe), compute kernels; Apple Maps' pipelines build and its command buffers complete (no picture checked).
- **Broken / missing:** mesh and object shaders, tessellation, indirect command buffers, tile shading/imageblocks, function tables, indirect draw/dispatch, multiple viewports, vertex amplification, counters, texture views, wide lines; cross-process shared events; chronod widgets still have 2 pipelines refused under `metal=2`;
  135 AIR library modules are not entry points; no third-party Metal app has been tried; Weather, Photos and Measure never opened a Metal device. Siri's orb once aborted SpringBoard on Metal (48 times in the 2026-10-21 boot); current behaviour under `metal=2` not recorded.
- **Cause:** work in progress, not a defect of one component. **Workaround:** leave Metal off. **Evidence:** `docs/air.md`, `docs/air-report.md`, `metal_sync_test` 10/10. **Work:** T10. **Not completed functionality:** do not describe this as "Metal works".

## K02 Fingerprint / Touch ID-equivalent — all · Security · **Unsupported** · S2
No driver, `biometrickitd`/`pearld` denied, no Secure Enclave. Passcode is not supported either (the keybag models a never-locked device; K13). Workaround: none; the lock screen is swipe-to-unlock. Work: T09.

## K03 Cameras — OnePlus 8 **Partial**; redwood **Partial** · Camera · S2
- OnePlus 8 (`cam=5`, RAM runs only, 2026-11-04, verified on phone): **Camera.app shows a live preview, focuses on a tap, takes a 4032x3024
  HEIC photograph and changes modes (Photo, Video, Portrait)** with no mediaserverd death (runs 104112, 105351). Limits: only the front
  sensor (IMX471) has a frame path, so the Back Camera is the wide IMX586 since run 124917 (its lens motor not driven: fixed focus at
  its rest), the ultra IMX481 and the macro GC2375 power up and give their chip IDs only (`cam=2`); no colour tuning for the IMX471 (grey-world white balance, no colour matrix or
  lens shading); the filter thumbnails are blank (no Metal device in mediaserverd); video/Live Photo recording untried; the preview is made by the GPU (`camera_gpu.c`): five minutes of it
  at 58-67 C, no throttling, 30 frames/s (run 120049; on the processor it was 95 C and throttled); never installed (no `cam=5` on the installed
  bundle). Work: T07.
- Redwood (per README, unverified recently): front/rear preview, focus, HEIC photos; no video or Live Photo recording; processing is CPU-only (grey-world white balance, no noise reduction). Work: T25.

## K04 Audio — OnePlus 8 · **Partial**
- **Works:** speakers (heard), UI sounds/ringtones/alerts/notification sounds, volume keys, main microphone (clap test).
- **Unsupported:** USB-C analog headphones (no detection, no RX path), earpiece, call audio, Bluetooth audio, secondary microphones, noise suppression.
- **Caveats:** amplifiers default off in the kernel (`sound_amps=0`; the profile passes 1); speaker protection is a software guard, not the DSP algorithm; level is 12 dB under stock maximum (`sound_below=96`); the first sound after an unpark has not been listened to;
  the amp health polarity is only checked via MANALARM/MANSTATE. Work: T08, T24. Evidence: [audio.md](subsystems/audio.md).

## K05 Wi-Fi and Bluetooth — OnePlus 8
- **Bluetooth: Partial.** Native service-filtered BLE discovery and initial GATT transfer verified. Post-wake reconnect fails at a MITM security request: no supported pairing session (4815/705). Later supervised Settings pairing of CMF Buds Pro 2 succeeds with authentication/encryption and AAC A2DP negotiation; owner reports silent audio and failed Voice Memos recording. Missing native Bluetooth HAL bundle found and packaging fixed; corrected image reaches A2DP STREAMING. Headset recording still fails: HFP route selected, format error and audio-server restart observed, fatal stack missing. Built-in recording passes in isolated `003355`; heard output and reliable reconnect remain unverified. See T05 and [bluetooth.md](subsystems/bluetooth.md). **2026-10-27:** A2DP heard by the owner (`010727`); the HFP/SCO UART stall that restarted mediaserverd/bluetoothd is fixed (SCO pacing + enhanced set-up, `hk_bt.c`). Voice Memos recording through the buds owner-verified in `012839` (crash in `011859` was CoreAudio's refused real-time thread; `rt_clamp` added, unproven). Not verified: calls, sleep reconnect, persistent bonds.
- **Wi-Fi: Partial, verified in RAM runs only** (scan, join from Settings, DHCP, Safari; WPA2, WPA2/TKIP-mixed, WPA3 SAE).
  - Unsupported: 802.11r (iOS is told FT is absent and joins with PSK), TKIP pairwise, roaming, WPA3 other groups/password identifiers. T18.
  - **Unverified:** radio through system sleep and wake (T02), per-network private MAC address, WPA3 transition mode, firmware-crash recovery.
  - A RAM run starts with a new `/private/var`: networks are not remembered (`fw/network` joins one at start). Installed boot keeps `/private/var` but auto-join across reboots has not been checked.
- **Cause of the TKIP/-3903 failure** was found and fixed (wifi.md). Evidence level high for the listed joins.

## K06 Cellular, calls, SMS, voicemail — all · Telephony · **Unsupported** · S1 for a phone
No modem start, no baseband IOKit drivers; CommCenter (non-cellular variant) is denied after it crashes on missing `AppleIPAppender`/`AppleBasebandPCI`; `vmd` denied (spun a core). Phone/Messages apps cannot place or receive. Work: T06. Also NFC (`nfcd` denied): T34.

## K07 Sensors — all · **Unsupported** · S2
No screen rotation, motion, compass, proximity, light, barometer; no auto-brightness (`BLControl` waits 5 s for an ALS at window-server start and gives up). The sensors sit behind the audio DSP (SEE, QMI 400) and the path is not built. Work: T04.

## K08 Battery and charging — OnePlus 8 · **Unverified / Unsupported** · S1 (safety paths)
- **Works (seen):** level/voltage/current/charging reported to iOS; status bar percentage; charger type via diagnostics lines.
- **Unverified (built, never run on the phone):** raising the charger input limit on a wall charger (2000 mA) / charging port (1500 mA); **off-mode charging** (a phone switched off with the cable in should stay off and charge); **empty-battery orderly shutdown**; the loader's charger-limit save/restore.
  Until verified: on a computer's USB port (SDP, 500 mA) a busy phone drains while plugged in; the firmware's limit is what you get.
- **Unsupported:** OnePlus Warp fast charging, PD/PPS. No drain figure (asleep or awake). Work: T01, T13, T19. Evidence: [power.md](subsystems/power.md).

## K09 Display and brightness — OnePlus 8 · **Unverified / Bug (minor)**
- **Brightness:** the Settings/Control Center slider reaches the panel as DCS `0x51` commands (verified at command level); **that the light actually changes, and the byte order, have not been verified** (no light measurement, no DSI read). There is no ambient sensor (K07). T16.
- **Late swaps:** ~2% of swaps come a refresh late (one push in flight); the first gesture set after boot is ~7% late; the unlock-after-wake blur costs 14 ms of GPU per frame. T23.
- **Never send DCS 0x10** (panel static until reset): this is a developer hazard, not a user setting.
- 60 Hz fallbacks exist (`fb_hz=60`, `fb_push=0`) if 90 Hz misbehaves. Resolved: tearing, shadows "too large" (owner).

## K10 Sleep, wake and power — OnePlus 8 · **Partial** · S2
- **Works:** screen off/on, system sleep (6 of 7 cores off, clocks frozen), wake by power key, timer, cable, `sleep_max`; GPU block and audio path and touch chip powered down; iOS's own idle sleep with maintenance wakes.
- **Limits:** the SoC's lowest state is **not reached** (architecturally blocked: T12), so sleep draw is higher than an iPhone's and **unmeasured** (T13). Awake idle cores sit in WFI, not power-collapsed (T20).
- **Fixed 2026-10-26:** Control Center, status bar items and app content missing after a wake (VSync times in the wrong clock; [power.md](subsystems/power.md#ui-missing-after-a-wake-fixed-2026-10-26)). Not yet in the installed build (T03).
- **After a wake** closing Control Center runs at ~52 fps with 38% slow frame steps (before sleep ~87 fps); `tools/t power`'s "frames after sleep" fails on p95 or passes by margin. Uninvestigated.
- **After a wake** widget extensions and chronod burn ~6 s CPU in 14 s (T17). (The 40%-of-a-core `vmd` spin that made the phone slow after wake is fixed.)
- **Unverified:** Wi-Fi radio across sleep (T02); volume-key interrupts; slider wake behaviour. The thermal service ticks every 250 ms even asleep by design.

## K11 App Store, Apple ID, iCloud, push, FairPlay — all · **Unsupported (blocked)** · S2
App Store shows LOADING for ever (the bag request is built but never resolved; an AMS thread waits on `fairplayd.H2`, which exits 0 immediately every 10 s). No Apple ID sign-in, no `apsd` push courier (identity needs the Secure Enclave), `mobileactivationd` answers "Device is not activated".
Safari, Settings, Sileo/Procursus (apt, tweaks) work. `nsurlsessiond` must run or Sileo downloads sit "Queued". Work: T15.

## K12 Location — all · **Unsupported** · S2
No GNSS (modem not started); `locationd` denied; Maps has no position. Work: T14 (blocked by T06).

## K13 Security properties — all · **By design (stand-ins)** · S3
AMFI and Sandbox answer "allowed" to everything; entitlements come from the signature; no trust cache. `AppleKeyStore` models a device with no passcode and never locked: keychain items are "wrapped" with a reversible transformation that protects nothing, and the keychain lives in `/private/var`.
Do not use this device for secrets. Work: T28. Evidence: [kernel-standins.md](research/kernel-standins.md).

## K14 Persistence — RAM-only runs · **By design** · S3
`/private/var` is a memory file system remade empty each boot (capped at ~2 GB): home-screen layout, app data, Settings, networks and the Keychain do not survive; apps are registered anew and may appear on a different page unless `sb_delay` is set.
The installed boot keeps `/private/var` on flash (HFS+ on the OnePlus 8). Starting a persistent RAM run needs `var_store=1 ufs_write=1` and the owner's permission (it writes flash).
Occasionally a daemon fails on the fresh `/var`: run 060507 (2026-11-05, `ramhfs`, 1 boot of 11 that night) had ScreenTimeAgent abort with "Failed to load local persistent store" (CoreData, `-[STDaemonPersistenceController _loadLocalPersistentStore]`); launchd restarts it. Not seen in the other ten.
Also seen once (run 085644, 2026-11-05, 1 boot of 14): mobiletimerd[120] jumped into a read-write page of the shared cache ("Data/Stack execution not permitted ... protections were read-write", exception 1 code 0x2, then a corpse), after another instance had logged "Settings is nil". Unexplained; whether it comes from the fresh `/var` is an inference from its neighbour above, not established.

## K15 UI rendering gaps — OnePlus 8 · **Bug** · S3
- Long-press on the lock-screen wallpaper (poster switcher) would show black; it is disabled instead (`poster_switcher_allowed()`); cause unknown.
- Shadows under cards of the app switcher are missing (function `sb3` refused). The switcher's wallpaper wash uses a placeholder image.
- Control Center glyph strokes appear only after the fade settles (`cc_fade` workaround exists, off).
- Notification banner material has not been compared with a real iOS.
- The app switcher cannot be opened by script (a test limitation, K19). Work: T22. Evidence: research/ios-userland.md.

## K16 Boot and recovery limits — OnePlus 8 · **Limitation** · S2
- A reset with the **USB cable in** makes the stock ABL take the port for a wall charger: fastboot never appears on USB and the phone switches off. Unplug, enter the mode, replug. Menu entries wait up to 60 s for the cable to come out.
- The 5 s menu countdown is too short to pick Developer mode by hand.
- There is no Android; recovery is the stock bootloader + TWRP. **The stock `boot_b` backup is outside this repository** (`../ios-instantnoodle/backup/partitions/boot_b.img`, ignored) and `out/boot_b-restore.img` is ignored by git: a fresh clone cannot restore.
- A bare `./xl enter` hangs the phone (loads redwood's UEFI); `touchscreen=1` and `wifi_again=1`/`adsp_again=1` crash or reset it.
- Details: [research/boot-chain-and-recovery.md](research/boot-chain-and-recovery.md). Work: T27, T31.

## K17 Media — all · **Unsupported** · S3
No video or Live Photo recording; portrait-mode Core Image prewarm is skipped (it crashed mediaserverd); video stabilisation refused; HEIC decoding is software (VideoToolbox's `VCPHEVC.videocodec`), performance not measured. T25.

## K18 Thermal — all · **Unverified** · S3
27 TSENS sensors read (33-38 C idle on redwood); the governor has never been seen slowing cores. Emergency path ends the run with a panic past 115 C. T33.

## K19 Test-harness limitations — developers · S4
Scripted touches do not reach iOS system gestures (swipe-up home/switcher) except from the bottom edge path; `com.hackios.switcher` returns but changes nothing with three apps running. QEMU logs print every line twice. T32.

## K20 Redwood (POCO X5 Pro) — **Unverified recently** · S3
Documented capabilities come from the upstream README and earlier sessions. The owner asked not to run POCO checks. Lacks GPU page tables, DCVS, fence IRQ, sleep, interrupt touch. T30.

## K21 Installed build lags the source — OnePlus 8 · **Limitation** · S3
The installed bundle/loader date from 2026-10-25 14:20; GPU collapse, brightness, Wi-Fi, `ca_ambient`, vmd fix and later work are only in RAM runs. T03.

---

### How to add an entry
Verify against code and evidence, state Kind, Severity, device, expected vs actual, cause if established, evidence, workaround only if known, the TODO id. Fixed? Change the kind to *Fixed (date)*, keep the diagnosis, and update the summary table.

## K22 Agent toolkit activation — developers · Unverified · S3
The physical phone still runs loader 0.5 without the new `getvar:iphont` extension. New agent commands correctly return `old_loader`; deep/control features cannot be considered working there yet. The owner deferred a RAM-only reload on 2026-10-26. No flash operation is needed to validate the extension. See [TOOLKIT.md](TOOLKIT.md), TODO T36.
