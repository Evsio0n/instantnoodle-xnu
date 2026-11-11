# Known issues — what does not work on this device (right now)

Answers **"what currently doesn't work?"** Entries are things a user or developer will notice; work to fix them is in [TODO.md](TODO.md) (`T..`), not repeated here.
Primary device: **OnePlus 8 (instantnoodle)**; redwood and QEMU are marked where different. Audited 2026-11-07 against the source and the project's records; the cellular, call audio, installed build and Metal entries (K04, K06, K11, K15, K21, K35, K41, K42) revised 2026-11-11.

**Kind:** *Unsupported* (no implementation) · *Bug* (implemented, wrong; cause known or not) · *Partial* · *Unreliable* · *Unverified* (implemented, never seen working).
**Severity:** **S1** blocks basic use or safety · **S2** a major iOS feature missing/broken · **S3** noticeable defect · **S4** minor/cosmetic.
Nothing is marked "works" merely because a driver exists: evidence is stated. For the list of what *does* work see [devices/](devices/README.md).

## Summary

| ID | Area | Issue | Kind | Sev | OnePlus 8 | Redwood |
|---|---|---|---|---|---|---|
| K01 | Graphics | Metal is experimental; most Metal features and apps untested | Partial | S2 | off by default | not applicable |
| K02 | Security | No fingerprint / biometric unlock | Unsupported | S2 | yes | yes |
| K03 | Camera | OnePlus 8: Camera.app on the front sensor only (`cam=5`, RAM runs), no tuning, warm; redwood: no video | Partial | S2 | partly | works (no video) |
| K04 | Audio | No headphones/USB-C audio; a call's sound only in the cellular system, its level not adjustable; one mic only | Unsupported / Partial | S2 | yes | partly |
| K05 | Connectivity | Partial Bluetooth; Wi-Fi gaps and unverified sleep | Partial / Unverified | S2 | yes | yes |
| K06 | Telephony | OnePlus 8: data, SMS and VoLTE calls work (K41); no hold, DTMF, MMS, hotspot, voicemail. Redwood: nothing | Partial | S2 | partly | yes |
| K07 | Sensors | No sensor service/data; AP candidate probe unclassified | Partial | S2 | yes | yes |
| K08 | Battery | Charger/shutdown paths unverified; no drain figure; no fast charging | Unverified / Unsupported | S1 | yes | partly |
| K09 | Display | Brightness optical effect unverified; no ambient sensor service; rendering gaps | Unverified / Bug | S3 | yes | |
| K10 | Power | Sleep is shallow; drain asleep unknown; heavy wake CPU | Partial | S2 | yes | no sleep |
| K11 | Services | App Store LOADING; no Apple ID, iCloud, push, FairPlay | Unsupported | S2 | yes | yes |
| K12 | Location | Wi-Fi location works by eye; generic GNSS transport reports but no outdoor fix or locationd bridge | Partial | S2 | yes | yes |
| K13 | Security | Security stand-ins accept everything; keychain protects nothing | By design | S3 | yes | yes |
| K14 | Persistence | RAM-only runs lose all data; Wi-Fi password not kept | By design | S3 | RAM runs | RAM runs |
| K15 | UI | Poster switcher turned off by our hook (black before Metal, not tried since); shadows under switcher cards; CC glyph strokes | Bug | S3 | yes | |
| K16 | Boot | Stock ABL breaks with USB cable in; short menu countdown; no recovery image in a fresh clone | Limitation | S2 | yes | |
| K17 | Media | No video/Live Photo recording; portrait mode skipped | Unsupported | S3 | yes | yes |
| K18 | Thermal | Throttling never observed | Unverified | S3 | yes | yes |
| K19 | Testing | Scripted gestures do not reach system gestures; switcher not openable by script | Limitation | S4 | yes | |
| K20 | Redwood | Not re-verified | Unverified | S3 | | yes |
| K21 | Installed build | "Cellular 2026-11-11" is installed; its telephony daemon is one revision behind the source | Limitation | S4 | yes | |
| K22 | Agent toolkit | New USB mailbox/control path awaits owner-approved RAM loading | Unverified | S3 | yes | |
| K23 | Scheduling | Edge: rare 10-11 ms render frame under heavy DF load; BG starved when UT saturates E; energy model unvalidated | Partial | S4 | yes | untested |
| K24 | Testing | `sleep` check can fail a correct sleep (continuous clock 0.4 s short once) | Bug (test) | S4 | yes | |
| K27 | Photos | An HDR photo opened full size shows its grey gain map (no Metal) | Partial | S3 | test roots | |
| K28 | Metal (Gallium) | A Metal fragment shader that stores [[depth]] times the GPU out on the Gallium back end (recovered); GLES gl_FragDepth is fine; vk draws it | Bug | S3 | test runs | |
| K29 | Metal (Gallium) | A half-precision result that overflows is stored as 65504, not +inf (vk: +inf) | Bug | S4 | test runs | |
| K30 | Metal (Gallium) | Metal lines and points draw nothing on the Gallium back end (vk draws them) | Bug | S3 | test runs | |
| K31 | Metal (Gallium) | Packed 16-bit formats mirrored against real Metal; the source of the exchange the wallpaper showed is open | Bug or unexplained | S3 | system with metal= | |
| K32 | Metal (Vulkan) | An argument buffer is read at the command buffer's commit, not when the GPU runs it | Limitation | S3 | metal_drv_test --m5c | |
| K35 | Metal (Vulkan) | Compositor on Metal stutters: about 1 frame in 10 late in daily use; Maps' pinch stutters | Open | S2 | metal=2 | |
| K36 | Metal (Vulkan) | A surface of rows this GPU cannot draw reaches other readers a second late | Limitation | S4 | metal=2 | |
| K37 | Metal (Vulkan) | CoreImage on Metal made empty images: no function stitching | Fixed (test roots) | S2 | metal=2 | |
| K38 | Camera | Back camera's preview at 15 frames a second in the dark, from the sensor; the light not measured. mediaserverd's 1 GB a second: fixed in test roots | Open | S2 | cam=5 | |
| K39 | Maps | Maps opens only with the fairplayd.H2 job dropped from a test root; the installed system unchanged | Open | S2 | test roots | |
| K40 | Metal (Vulkan) | The compositor's memory grows to 2.5 GB in 12 minutes and never comes back; the screen can restart | Open | S1 | metal=2 | |
| K41 | Cellular | What is left of calls and SMS: level, hold, DTMF, MMS, hotspot; the screen stays on at the ear | Partial | S2 | yes | |
| K42 | Metal (Vulkan) | What Metal still refuses, drops or stands in for, as the system's own processes ask for it | Partial | S3 | metal=2 | |

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

## K34 Our own MDP layers: what has not been seen to work — OnePlus 8 · Display · **Unverified** · S3
- **On (RAM, `mdp=8` the board's default in `xnu-src`), logs clean; the owner saw no wrong colours, line at the middle or stutter.** A detached layer's picture has not been asked about. Off with `mdp=0`.
- **`xl screenshot` shows a CPU copy of the frame, at most every 2 s and when the frames stop, in a run from the computer:** not the panel, and of two layers only the composite with its hole. Each copy delays the presenter 10-25 ms [estimated]. Installed boots keep none.
- **Swaps a refresh late: 21-39 in 5 s against the GPU scale's 8-16 while moving, cause unknown;** frame rate and frame times match.
- **NV12 layers have never run** (T39): a video detached by CoreAnimation is the first time, and its colours are unproven.
- Evidence: `docs/research/mdp-driver-plan.md` section 8.

## K35 The compositor on Metal feels slower than on hk-gl — OnePlus 8 · Display · **Open** · S2
- **The owner:** 「opengl很快，沒metal這麼卡」, 「地圖伸縮卡頓」; of `fb_wait` 1, 3, 5 rows 「普通」, 「尚可」; the default 13 has not been looked at.
- **Measured, heavy script (runs 005452, 010021):** GPU per frame about 15-25% above hk-gl's, frame gaps close. docs/research/mdp-driver-plan.md 9.
- **Daily use of the installed system (2026-11-11; the owner: 「卡頓、掉幀」, and that Metal 3 had it too). Read from the kernel's 5-second display statistics of that day's four boots, 50 busy windows, and from the source; nothing was changed and nothing new was run for it:**
  - **The numbers** [measured, by log]: 54 frames a second at the median when busy, 87 at best; 85% of the gaps between frames on the 90 Hz beat; about 10% of the frames one refresh or more late; the longest pause in a 5 s window mostly about 50 ms, 87 ms at worst.
  - **Not the GPU's speed, not the install** [measured / source]: the GPU is busy 3% at the median and 53% at most while the phone is used. The installed kernel's display and GPU drivers are Metal 3's source but for one option that is not on.
  - **Pipelines built while drawing** [measured, a correlation]: windows in which a render pipeline was built have twice the late frames (median 19 against 9) and longer pauses (71 ms against 49). There is no pipeline cache on disk, the same pipeline is built more than once, and it is built on the thread that draws. A cache was written once and never verified or installed. iOS's own compositor looks for a precompiled pipeline library at its start and finds none for this GPU.
  - **What an image costs on its way to the screen** [measured, by log, unless said]: no pixel format is converted on the CPU (BGRA, sRGB, 10-bit, ETC2 and ASTC are Vulkan's own formats, mipmaps are made by the GPU; PVRTC, BC and ASTC HDR are refused). An IOSurface is imported without a copy in 0.35-1.1 ms, but never cached: every use imports again. At every submission the kernel cleans the imported surfaces out of the CPU's cache, about 11 MB and 0.5 ms under the GPU's one lock (`HKGPU.cpp`): 71-95 ms in 5 s. YUV becomes RGB on the CPU (`hkBiplanarTexture`): 51 ms for the first image (a scalar loop), 3.3-4.5 ms a frame after it; the camera's preview and HEIC wallpaper layers use it. A whole-screen image is 11 MB and takes 8-9 ms to allocate, most of a frame; 27 to 71 of them in each boot that day. Not timed: HEIC is always decoded in software; app snapshots are kept as JPEG or PNG, decoded and uploaded again when used.
  - **30-74 passes a frame is this layer's shortcut, not the GPU's limit** [source, not run on the phone]: a draw that reads the target it draws to ends the pass and starts another (`draw_stands_apart`). The Turnip driver this port uses declares `VK_KHR_dynamic_rendering_local_read`, `VK_EXT_rasterization_order_attachment_access` and `VK_EXT_attachment_feedback_loop_layout`, which let such a draw stay in its pass. An earlier attempt to merge them, without the barriers it needs, broke the blur and was taken back. For the compositor the saving was once estimated at about 1 ms a frame; for a game that shades in one pass (reading its own target in every light's draw) it is the difference between a few passes and hundreds.
  - **Not established:** which of these is what the owner feels. The compositor's own time by stage in daily use has not been measured: hkmetal has the counters, and the probe file that turns them on (`metal_stats`) is not in the installed root. The plan, measurement first: T40.
- **Maps' pinch (runs 015419, 020307, scripted, no network):** the part found is Metal's: Maps asks for two new memoryless whole-screen 4x targets every frame, which this port really allocates, 35-76 ms each. An image pool (256 MB) took that from 2.3 s to about 0.4 s in a 5 s window. Compositor frames over 18 ms in the pinch are now about 5%; the earlier 47-60% runs lost Maps to a fault of the sampler's, so there is no clean baseline. A comparison that stood here (76 frames against hk-gl's 182) was **wrong**: first-run sheets covered the map. No valid hk-gl comparison of the pinch exists.
- **Maps' rate in motion (run 061838, kernel mdp-93fe105e, seeded, scripted, no network, blank grid; verified on phone by log):** Maps' map frames 28 and 25 a second, compositor 66 and 57, half the gaps under 12 ms, longest 56 and 80 ms, GPU busy about 50%, textures 0.3 s per 5 s. Why Maps stays near 28 is open. Submissions waited for a push 40 times, 275 ms in about 11 s: a lead.
- **Real tiles (with the network), the owner: 「不跟手，有時候還會黑色」, 「旋轉不跟手」.** Two faults found, neither shown on a blank grid. (1) The vertex stage's textures were dropped in hkmetal: VectorKit's ground, icons and labels failed their draws and took their command buffers with them. Fixed since `metal-二次看`: 0 failed draws, 「地圖可以正確渲染」 (run 093635). Black is **not called gone** until the owner says. (2) 99.9% of Maps' pipeline requests are retries of two refused ones (`GlowAlpha`, `PatternedRibbon`: framebuffer fetch at 4 samples), about 15 a frame, up to 1.4 s in 5 s: 42442 of 42497 (run 102841). A memo of the refusal since `metal-三次看`: Maps at rest with no fingers from about 7.3 map frames a second to about 17 (run 104515); with fingers, best window 36 to 58. **A clean no-finger gesture number does not exist yet.** The two pipelines still don't draw; what they are is known by name only. Next in this: buffers, up to 1914 made per 5 s, about 1 s of the main thread, about 70% freed again soon.
- **Where the time goes in a pinch, from logs only (no-finger runs 113329 at 525 MHz and 115324 at 587; owner's run 104515; 2026-11-06):**
  - **The display path is not it** [measured]: 0 underruns, 0 SMMU faults, nearly every frame through the pipes, the driver 2-6 ms a frame, submissions waiting for a push about 4 ms each, 66-163 ms in all.
  - **The GPU's time adds up to more than a 90 Hz frame** [measured by timestamps, sampled]: a Maps frame 9.3 ms (one pass, 300-450 draws), a compositor frame 6.3 ms, so about 15.6 ms against 11.1 ms; 104515's Maps frames reach 19 ms at the 90th percentile. Frames come out at uneven gaps: in the owner's use 23% under 10 ms, 37% under 18, 18% under 26, 18% under 35, 4% longer, longest 89 ms. That this unevenness is 「卡卡」 is inferred. At 587 MHz the same sum is about 13 ms; the gaps are somewhat tighter (half under 12-16 ms against 14-21; one run each) while the map's rate does not rise.
  - **The compositor makes 30-74 Vulkan passes a frame** [measured]: most under 150 us, about 2.4 of 8.8 ms in a sampled frame, and each costs recording too (about 3 ms a commit in gestures). In 5 s windows 3000-4300 draws read their target and stand apart, because `draw_stands_apart` gives such a draw a pass of its own.
  - **First use, on whoever is drawing** [measured]: the first pinch 34 pipelines 186 ms, 17 render pipelines 218 ms, 1435 buffers 476 ms in one window. At Maps' opening the compositor spends 859-863 ms on 7 render pipelines, far above a seeded one's cost, so probably not in the seed [inferred]; which ones is what `hk-slow` is for (root `out/mapsslow-root.apfs`, not run yet).
  - **Maps' 4x targets still miss the pool now and then** [measured]: 40-56 ms each, up to 350 ms in 5 s.
- **Not established:** that any of these is what is felt: the pool is **unverified by the owner's eye** (row `metal-一次看`). Nothing was measured with real map tiles. No touch latency numbers.

## K36 Surfaces drawn through a stand-in — OnePlus 8 · Metal (Vulkan) · **Limitation** · S4
- A surface whose maker asked for rows the GPU cannot draw (4512 bytes a row for 1125 wide, not a multiple of 64) is drawn into through a private texture (DRAWN AND PUT BACK, hkmetal.m), which this process reads too (one texture per surface). Any other reader gets the picture at once on the first draw after a rest of over a second, then a second after the last draw of a run. A read texture this process made before the stand-in is a copy that follows the surface. Whose surface it is has not been found: the creator's log line did not reach the captured log.

## K33 HDR photo on Metal over Vulkan — OnePlus 8 · Photos · **Fixed in test roots, verified by the owner's eye and by numbers; not installed** · S2
- **The owner's eye, run 041029 (`metal-一次看`, the first with a gain map that had data):** 「HDR有效果但是加载要好几秒才从模糊变清晰」, 「hdr效果等了10s才完整加载出来」. The look at 03:54, 「显示出来了但是没有hdr效果」, was run 034930 or the start of 035422, which had the fix too; the owner's own reading: 「我觉得你旧的那次可能只是等太短了」. Not retested by eye since the speed-up.
- **Cause [observed]:** Photos' gain map image was empty. `+[PFImageMetadata createImageRefFromAuxiliaryImagePixelBuffer:applyingOrientation:scaleFactor:]` makes it with CoreImage, and CoreImage on Metal made empty images (K37). Everything after that was right: the `edrGain` filter, the gain map sublayer, the compositor's multiply (census, runs 031334, 031904). Numbers from shots then deleted, UI white 187-188 in the same frame: picture's highest 189-197 and nothing over 191 with the empty one (025340), highest 255 and 9-10% of the picture band over 191 with data in it (035422, through the CPU stand-in `libhackios-gainmap.dylib`, which real CoreImage has since replaced: gain maps with the same 176 and 240 highest, 052222).
- **The seconds [observed, run 051210]:** one thread of Photos used 10.2 s of the 12 s sampled, nearly all in this driver: `hk_specialize` re-read 464 KB of bitcode for every VideoToolbox transfer, and each transfer made a new pipeline. Both kept now (`hkmetal.m`). **Portrait photo, tap to base and gain map handed to the layer: 15.7 s, 8.21 s after that fix, 4.15 s with shaders built before the boot** (043628, 052222, 053937). Landscape 1.95, 2.28, 1.01 s. By the watcher (`inject_figerr.c`), client side; time on the panel not measured. What is left of the 4 s is not explained.
- **Corrected:** this page once said the brightness stays at 70-83 nits and the screen is dimmer than SDR. The kernel's `display: brightness` reaches 140 nits at full view, in the runs cited too; white at half in linear light keeps SDR white where it was. The owner did not report dimness.
- **Headroom:** Photos asks for 2x here (`requestedEDRHeadroomFactor` 2.0, the filter's inputAmount 2.0): one stop. Whether a real iPhone asks for more was not read.

## K37 CoreImage on Metal made empty images — OnePlus 8 · Metal (Vulkan) · **Fixed, verified by bytes on the phone (test roots, 2026-11-06)** · S2
- **What it was (runs 033712, 034024, 034446; `userland/src/ci_gain.m`, no photos):** an L008 pixel buffer through `CIImage` and `-createCGImage:fromRect:`: on this Mac's real Metal the pattern, on the phone 0 of 1024 points not zero, status fine, no crash. Two causes. (1) CoreImage's kernels are stitched graphs, and this driver had no `-newLibraryWithStitchedDescriptor:error:`. (2) A bitmap of a whole number of 16 KB pages (128x128, 256x256, 512x512, 1024x768) is rendered straight into its memory through `-newBufferWithBytesNoCopy:`, answered with nil. (2)'s mechanism is inferred from which sizes failed; the fix is what confirmed it.
- **Fixed by:** `air_stitch_to_nir` (`air_to_nir.c`) and `HKStitchedLibrary`/`HKStitchedFunction` (`hkmetal.m`); no-copy buffers are a buffer of ours, the bytes copied in and what the GPU wrote copied back before the work is reported done. **Verified, 052222 and 053937:** 128 to 2016 wide, turned too, all 1024 points right; 8 orientations against a CPU copy measured on the Mac against real CoreImage: 0 off at scale 1, 1 at 0.5; 0 "Metal library creation failed".
- **Not covered:** only compute graphs of `ci_stdlib_stitchable` were run: a filter's own kernels, fragment and imageblock graphs (`_ci_write_block_h` and such), a graph with an argument node, are unproven. Real no-copy (the kernel mapping the caller's pages) is not done, so large use copies.
- **What was silently blank until now, static read only (nothing observed on the phone but gain maps):** nothing on the plain home screen, default wallpaper or app switching. Photo edits and filters, camera filters and zoom, Memories, CI-backed UIImages, effect and generic-folder icon compositing, the wallpaper's inactive treatment, Messages' send preview, notification digest colour, subject lift, Safari's custom start page.

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

## K38 Camera preview stutters — OnePlus 8 · Camera · **Open** · S2
- **The owner:** 「相机是卡顿」, 「相机感觉视频流还有点问题」, then what it was: 「相机预览还是loop」, 「幾張畫面反覆來回」.
- **The loop: fixed in test roots since `metal-二次看`, proven by hashes on the phone, seen right by the owner (「#66的相機工作正常」).** It was hkmetal's: format 500 (CoreAnimation's 420 texture) was copied once when a surface was first asked for, so the pool's three surfaces in rotation came round for ever as the pictures they first held. Run 091048 (old behaviour): order `324324…`, each texture's hash fixed while its surface's changes, seeds moving every look. Sensor, ISP and mediaserverd were all right, the pipes not involved. Fix: `hkBiplanarLook`, converted again when the seed moves, 3.3-4.5 ms a frame, about 7% of a core on the compositor's thread (run 095327: seeds moved = copied, shown = held). A note about method: "420 on the CPU 0" read as clearing this was backwards; a stale copy is exactly what costs nothing.
- **Measured (run 060013, script, back camera, dark room, 59 C, no fingers; verified on phone by log):** compositor 15 commits a second, half the gaps under 65 ms, longest about 80 ms. The kernel gave 386 sensor frames in about 26 s, all delivered. Picture 20 ms by the GPU, frame stats and focus about 8 ms. No surfaces made and no 420 on the CPU in steady state.
- **Cause as far as seen [observed for this run]:** AE at gain 16x and digital 4x, exposure 66.4 ms: it stretched the frame to `camera_isp.c`'s `longest`, twice the shortest, by design. The thermal drop to 15 (camera_device.c) did not fire.
- **Not established:** that this is what the owner felt. Their run 041029 opened at 91.3 ms, also dim, about 11 a second, below the graph's minimum of 15. 28 of a 33 ms budget is spent per frame, so 30 is **unproven** even in the light. Nothing has been measured in the light.
- **With the frame held at the shortest (`cam_fast=1`, a measurement; dark room, script; verified on phone by log):** the sensor and mediaserverd hold 30 a second, exposure 33.1 ms, none written over. The compositor: 28-29 commits a second in run 064117, half the gaps under 35 ms, longest 57; 24 in run 073342, half under 44-45, longest 57. One run each, so 24-29. Run 064117 had the memory flood below and AF stuck (focus 5.6 ms a frame), run 073342 neither (focus 10-11 ms); what made the difference is not shown. The cost of the setting: half the exposure at the same gains, how dark needs the panel. **Which the dark should have, brighter at 15 or darker at 24-29, is the owner's choice;** it stays off.
- **Next:** the owner's eye in the light (row `metal-一次看`); no measurement in the light exists.
- **mediaserverd's memory (fixed in test roots, verified on phone by log, runs 064622, 070021, 071235 before, 071949 after).** During any preview: about 1 GB a second of malloc-large (VM tag 3), 19.8 GB at frame 300, 38 GB at 600, stopping at 58.8 GB (the "56116" of run 125906, so old; why it stops there is inferred, the address space), the phone's free memory at 2-3 MB throughout, the compressor holding it in about 120 MB. No jetsam kill, no frame lost, in a dark room; in bright light nobody has measured. Time-based, not per frame. *Cause [source: 20F66, read in words; observed]:* `BWAudioSourceNode`'s render callback compares each time stamp with the one it expected and turns a gap into silence to prepend. Here it finds about 468 s missing at every round: buffers go from 940 samples to 20,671,759 samples, 41.3 MB, about 23 times a second, and the nodes after it hold them. **Why 468 s is not known;** the HAL's clock (`audio_plugin.c`) is the first place to look, a 64-bit wrap of a small negative difference does not give that number. *Fix:* the node's own switch, `_silenceFramesFillingDisabled`, set in our `start:` hook (`inject_media.c`). A first try, replacing `_generateSamples`, could not work: it is a direct method, in no method table, and the guard refused it (070620). At frame 300 after: footprint 171 MB, compressed 0, free 653 MB, buffers 941 samples, 571 of 571 frames given. Whether 89 to 171 MB in the first 20 s levels off has not been measured.
- **Free memory was not the dark room's stutter:** gaps with no `cam_fast`, before (060013) and after (071949), are the same: 15 a second, half under 65 ms, longest 79-82.
- **A video has no sound:** the HAL offers a microphone, and with no `/dev/hk-mic` it gives nothing. The audio unit starts, so `audio_source_start`'s "no capture audio" line does not print; what says so is `the microphone is listened to (/dev/hk-mic: No such file or directory)`, `the microphone stops: ... 0.00 s of it from the kernel`, and `the audio source's filling of silence off`. The cost of the fix: a working microphone's real gap would get no silence.
- **`IOServiceMatchingfailed for: AppleM2ScalerCSCDriver`** prints about 28 times a second in the `cam_fast` runs, 853 lines in 064117, and hardly at all at 15 a second. Who asks is not known.
- **Also seen, focus in the dark (run 063507; left for the owner to decide, not fixed: a clamp means new writes to the lens motor):** AF's sweep reached its near end, 16 (`AF_NEAR`, camera_device.c), and from then on the kernel refused the position every frame, 1843 lines of `the lens's position 4294967277 refused (out of 50 to 930)`. The value is below the lens's range after the kernel's mapping, and the refusal comes before any bus write, so nothing reaches the motor; AF is stuck until the stream ends. Not a stutter cause as far as seen. Earlier runs show it far less: 2 lines in 060013.

## K39 Maps opens only with the fairplayd.H2 job dropped — OnePlus 8 · Maps · **Open** · S2
- **Observed (next-features' nf15 on hk-gl; on Metal runs 060829, 061301, 061838):** with `com.apple.fairplayd.H2.plist` dropped from a RAM root, Maps commits about 2.1 s after it begins, and its first-run pages, sheet and location prompt draw. With the job, no commit (a white screen). docs/research/next-features.md 17.
- **Rows:** every row since `metal-一次看` has it. Camera's first launch now also shows a location prompt, since locationd answers. With the network, real tiles load (the owner, run 084652).
- **Limits:** a test root only; about ten other clients get failing lookups. The 0x8BADF00D at termination is not tested. Real map tiles and a location fix are **unverified**: no boot here joined a network. Home and Work in Favorites are empty circles, unexplained. On a shot the map itself is black: it has its own pipe (docs/research/mdp-driver-plan.md 9), so its picture needs the panel.
- **Boundary:** a blocked call may fail fast. No forged signatures, no emulated fairplayd, no hooks around FairPlay.

## K40 The compositor's memory grows and doesn't come back — OnePlus 8 · Metal (Vulkan) · **Open** · S1

The SpringBoard subrange mapping case now has a matched treatment. 222 / 050211 reduced residual IOSurface map bytes from 167608320 to zero at rest after five Safari/Home cycles. In 231 / 053731 the matched 12-minute workload and all 30 foreground phases passed, with retained SpringBoard growth 984964544 -> 1376448 bytes. Preserve the distinction from the original free-use backboardd incident below.

Hardware ticket #222 (050211) compared five Safari/Home transitions with #216 (025724). SpringBoard retained 0 versus 167,608,320 bytes of IOSurface backing maps after rest. VM tag 21 grew by 16,384 versus 167,624,704 bytes. Both had five foreground and Home handoffs with physical panel frames. The existing subrange lookup correction is in `xnu/hackios.patch`. Corrected RAM ticket #231 (053731) completed the matched 720.036-second rotation with 30 foreground/panel phases and no reported display give-ups or GPU faults. SpringBoard retained footprint growth was 1,376,448 versus 984,964,544 bytes in #210; the observed VM tag 21 peak was 32 versus 794 MiB. Verification of the exact daily pair and the historical manual backboardd workload remain separate.

RAM ticket 210 on November 10 (023202) completed six rounds of Maps, Camera, News, Weather and Safari. All 30 launches were accepted. Over 720.035 s SpringBoard footprint grew from 86.1 to 1025.4 MiB. Its private GEM objects stayed at 51.4 MiB. VM tag 21 (IOKit) reached 794 MiB in the last detailed telemetry sample. Backboardd peaked at 216.1 MiB and ended at 161.6 MiB. This reproduces sustained memory growth in a different process from the earlier backboardd incident. A specific map or allocation lifetime has not been identified. Wi-Fi was off and Maps had a fresh RAM data volume.
- **Measured (`memlog`; verified on phone):** backboardd from 38 MB at boot to 1555 MB, SpringBoard to 501, wired 1.9 to 4.7 GB in 11 minutes by script (run 091520, no network); backboardd to 2523 MB, wired 4.4 GB, in 12 minutes of the owner's use (run 100158). In steps as apps and screens open, flat at rest, never down. Free memory near zero from about 2-4 minutes.
- **What it does:** idle processes are jetsammed from about 10 minutes of heavy use (VM page shortage, code 4). In runs 041029, 083507, 084652 backboardd itself was killed, the screen restarting; in 100158 it survived 2.5 GB, seemingly because nothing more was opened after memory ran out (two runs, an inference).
- **Not it:** Maps' 4x pool (256 MB at most, one process's), the camera's flood (K38, fixed), a full `/var` (835 of 1536 MB, though its RAM disk takes 1536 MB of the phone's about 6.1 GB at boot on RAM runs only, `hk_ufs.c`).
- **Where:** hkmetal's live textures and buffers stay flat at about 175 MB while the footprint climbs past 600 (run 093635, with fingers; to re-read with none): below hkmetal, in `air_vk.c`, Turnip or the HKGPU side, most likely GPU memory (inferred from wired and the footprints growing together). Next: counters there, a new libvulkan and a new seed.

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
- **Works:** speakers (heard), UI sounds/ringtones/alerts/notification sounds, volume keys, main microphone (clap test). **In the cellular system (2026-11-11, the owner on real calls):** a call's sound both ways at the earpiece; the speaker button switches the amplifiers (seen in the log of a real call, not yet confirmed by ear). K41.
- **Unsupported:** USB-C analog headphones (no detection, no RX path), secondary microphones, noise suppression. **Untested:** a cellular call over a Bluetooth headset. A call's level cannot be changed (the DSP refuses a volume step without Android's calibration).
- **Caveats:** amplifiers default off in the kernel (`sound_amps=0`; the profile passes 1); speaker protection is a software guard, not the DSP algorithm; level is 12 dB under stock maximum (`sound_below=96`); the first sound after an unpark has not been listened to;
  the amp health polarity is only checked via MANALARM/MANSTATE. Work: T08, T24. Evidence: [audio.md](subsystems/audio.md).

## K05 Wi-Fi and Bluetooth — OnePlus 8
- **Bluetooth: Partial.** Native service-filtered BLE discovery and initial GATT transfer verified. Post-wake reconnect fails at a MITM security request: no supported pairing session (4815/705). Later supervised Settings pairing of CMF Buds Pro 2 succeeds with authentication/encryption and AAC A2DP negotiation; owner reports silent audio and failed Voice Memos recording. Missing native Bluetooth HAL bundle found and packaging fixed; corrected image reaches A2DP STREAMING. Headset recording still fails: HFP route selected, format error and audio-server restart observed, fatal stack missing. Built-in recording passes in isolated `003355`; heard output and reliable reconnect remain unverified. See T05 and [bluetooth.md](subsystems/bluetooth.md). **2026-10-27:** A2DP heard by the owner (`010727`); the HFP/SCO UART stall that restarted mediaserverd/bluetoothd is fixed (SCO pacing + enhanced set-up, `hk_bt.c`). Voice Memos recording through the buds owner-verified in `012839` (crash in `011859` was CoreAudio's refused real-time thread; `rt_clamp` added, unproven). Not verified: calls, sleep reconnect, persistent bonds.
- **Wi-Fi: Partial, verified in RAM runs only** (scan, join from Settings, DHCP, Safari; WPA2, WPA2/TKIP-mixed, WPA3 SAE).
  - Unsupported: 802.11r (iOS is told FT is absent and joins with PSK), TKIP pairwise, roaming, WPA3 other groups/password identifiers. T18.
  - **Unverified:** radio through system sleep and wake (T02), per-network private MAC address, WPA3 transition mode, firmware-crash recovery.
  - A RAM run starts with a new `/private/var`: networks are not remembered (`fw/network` joins one at start). Installed boot keeps `/private/var` but auto-join across reboots has not been checked.
- **Cause of the TKIP/-3903 failure** was found and fixed (wifi.md). Evidence level high for the listed joins.

## K06 Cellular, calls, SMS, voicemail — all · Telephony · **Partial** · S2
**OnePlus 8 (2026-11-11, verified on the phone, installed):** the carrier's name, signal, mobile data, SMS both ways and VoLTE calls both ways with sound, through the ported modem driver (`cell=1`) and a stand-in for CommCenter. What works, how, and where the code is: [subsystems/cellular.md](subsystems/cellular.md); what is left: K41.
CommCenter itself still cannot run here (it crashes on missing `AppleIPAppender`/`AppleBasebandPCI` and is denied), so iOS gets only the answers the stand-in gives: no voicemail (`vmd` denied: it spun a core), no Personal Hotspot row. The tree's first modem path (`modem=3`, `tools/nf/modem`) stays a diagnostic: it holds the modem in mission mode and QMI LOC is phone-proven on it, but it never registered. **Redwood:** no modem work. Work: T06. Also NFC (`nfcd` denied): T34.

## K07 Sensors — all · **Partial** · S2
No screen rotation, motion, compass, proximity, light, barometer; no auto-brightness (`BLControl` waits 5 s for an ALS at window-server start and gives up). The OnePlus 8's SLPI/SEE path still has no sensor data or HID bridge. A separate AP-side read-only probe shares the slider I2C bus and, after `ldoc10`, found no ACK at ODM candidate `0x46` in run `020629`; this does not establish the fitted part or rail. Work: T04.

## K08 Battery and charging — OnePlus 8 · **Unverified / Unsupported** · S1 (safety paths)
- **Works (seen):** level/voltage/current/charging reported to iOS; status bar percentage; charger type via diagnostics lines.
- **Unverified (built, never run on the phone):** raising the charger input limit on a wall charger (2000 mA) / charging port (1500 mA); **off-mode charging** (a phone switched off with the cable in should stay off and charge); **empty-battery orderly shutdown**; the loader's charger-limit save/restore.
  Until verified: on a computer's USB port (SDP, 500 mA) a busy phone drains while plugged in; the firmware's limit is what you get.
- **Unsupported:** OnePlus Warp fast charging, PD/PPS. No drain figure (asleep or awake). Work: T01, T13, T19. Evidence: [power.md](subsystems/power.md).

## K09 Display and brightness — OnePlus 8 · **Unverified / Bug (minor)**
- **Brightness:** the Settings/Control Center slider reaches the panel as DCS `0x51` commands (verified at command level); **that the light actually changes, and the byte order, have not been verified** (no light measurement, no DSI read). There is no ambient sensor service; a hardware candidate remains unverified (K07). T16.
- **Late swaps:** ~2% of swaps come a refresh late (one push in flight); the first gesture set after boot is ~7% late; the unlock-after-wake blur costs 14 ms of GPU per frame. T23.
- **Never send DCS 0x10** (panel static until reset): this is a developer hazard, not a user setting.
- 60 Hz fallbacks exist (`fb_hz=60`, `fb_push=0`) if 90 Hz misbehaves. Resolved: tearing, shadows "too large" (owner).

## K10 Sleep, wake and power — OnePlus 8 · **Partial** · S2
- **Works:** screen off/on, system sleep (6 of 7 cores off, clocks frozen), wake by power key, timer, cable, `sleep_max`; GPU block and audio path and touch chip powered down; iOS's own idle sleep with maintenance wakes.
- **Limits:** the SoC's lowest state is **not reached** (architecturally blocked: T12), so sleep draw is higher than an iPhone's and **unmeasured** (T13). Awake idle cores sit in WFI, not power-collapsed (T20).
- **Fixed 2026-10-26:** Control Center, status bar items and app content missing after a wake (VSync times in the wrong clock; [power.md](subsystems/power.md#ui-missing-after-a-wake-fixed-2026-10-26)). Not yet in the installed build (T03).
- **After a wake** closing Control Center runs at ~52 fps with 38% slow frame steps (before sleep ~87 fps); `tools/t power`'s "frames after sleep" fails on p95 or passes by margin. Uninvestigated.
- **After a wake** widget extensions and chronod burn ~6 s CPU in 14 s (T17). (The 40%-of-a-core `vmd` spin that made the phone slow after wake is fixed.)
- **Wi-Fi across sleep fails, and ends the system (run 105300, 2026-11-06; RAM, kernel mdp-93fe105e, Wi-Fi joined by the root's file):** sleep and wake themselves went as they should (6 of 7 cores off, woken by iOS's own RTC timer after 12 s, GPU back in 150 ms, every processor back). Then, on resume, the firmware crashes right after being asked for mission mode (`MHI_CB_EE_RDDM`, `SYS_ERROR`), the driver is taken down, mode 4 fails, the system ends with no reason in the log and `hk_wifi_quiesce` halts it: `UNCLEAN PCIe DMA OWNERSHIP; loader return halted, cold recovery required`. USB gone, the owner's cold restart needed. The panic's text is lost (HANDOFF, "a panic with wifi=1"); `pci_normal_fail` at removal (`hk_wifi.c`) is the likely one, inferred. The compositor and Camera after a wake were not seen: the log stops too soon. **Don't sleep with Wi-Fi running.** Every RAM boot through `run_cam_probe.py` has `syssleep=0` from its plan unless given `sleepon`; a sleep test on such a boot tests nothing (run 103645).
- **With Wi-Fi off, sleep and wake hold (runs 110920, 112641, 2026-11-06; RAM, kernel mdp-93fe105e, `wifi=0`, speakers and microphone up, no fingers until the last wake):** three timed sleeps of 15 s passed (6 of 7 cores off, GPU back in about 150 ms, 0 underruns, 0 SMMU faults, the pipes in use from the first frames), and iOS then slept on its own three times, the last ended by a real press. Camera after the second wake: first frame 1014 ms after it was asked for, 65 of 65 given, microphone up in 0.12 s. **One timed wake left the screen dark, and the first reading of why was wrong:** the third timed wake passed iOS the press as the second did, but SpringBoard answered `backlight-off + pocketTouches`, no unblank, and iOS slept again 0.5 s later. I took that to be a sleep forced with the screen lit; run 112641 refuted it: from a lit lock screen the timed wake lit it. **Run 112641** (`wifi=0`, no fingers until after the job): two sleeps after a lock, screen lit after both, Camera after both (170 of 170 and 162 of 162 pictures, microphone up); and the lit control, screen lit too. **What fits all seven wakes of the two runs [inferred, unproven; SpringBoard's own log does not reach the console]:** SpringBoard's state as the sleep began. With `pocketTouches` in it, 5 of 5 lit; without (`SB + backlight-off`), 0 of 2, one of them the test sleeping again at once. Both of those came after the job's Home press on the lock screen or before any lock, which I read as the phone unlocked, the press then spent on locking it. iOS does not sleep on its own from there as far as these runs show, so it would be `power_test`'s forced sleep that reaches it. Camera after the dark wake itself was never tried (`battery_test launch` refused, the phone locked [inferred]). Audible speaker sound after a wake: samples reached the driver, nobody has said it was heard. First-minute reading 53.9 C against 77-86 C with the Wi-Fi file (one run; inferred).
- **Unverified:** volume-key interrupts; slider wake behaviour. The thermal service ticks every 250 ms even asleep by design.

## K11 App Store, Apple ID, iCloud, push, FairPlay — all · **Unsupported (blocked)** · S2
App Store shows LOADING for ever (the bag request is built but never resolved; an AMS thread waits on `fairplayd.H2`, which exits 0 immediately every 10 s). No Apple ID sign-in, no `apsd` push courier (identity needs the Secure Enclave), `mobileactivationd` answers "Device is not activated".
Safari, Settings, Sileo/Procursus (apt, tweaks) work. `nsurlsessiond` must run or Sileo downloads sit "Queued". Work: T15.
- **Why it is blocked:** activation, Apple ID and the push courier need the device to sign with keys that Apple put into a real iPhone's Secure Enclave and registered on its servers. This phone has neither the chip nor the keys, and forging a device's identity is not done in this project. Qualcomm's TrustZone could give the keychain and a passcode real local protection (not started, T28); it cannot stand in for those keys.
- **What it means in use:** no iMessage or FaceTime, and a third-party app gets its messages only while it is open. Calls, SMS and notifications made on the phone itself (alarms, reminders, the SMS banner) do not pass through Apple's push and work.

## K12 Location — all · **Partial** · S2
**The default images:** no `locationd` job, so no position there. The RAM diagnostic root now brings the external SDX55 to mission, discovers QMI LOC service 16 over the reusable QRTR/QMI path, and receives generic LOC position reports; run `015925` (#156) still had zero satellites and no fix indoors. No iOS/locationd bridge exists, and outdoor RF evidence is still missing. Work: T14/T06.
**With `locationd`'s job on a root (`tools/nf/mk_extra.sh loc`, not the default image's) Maps opens to a white screen** and is killed by the watchdog: `locationd`'s queue waits on `fairplayd.H2`, which never answers (K11). Without fairplayd's job on the root Maps draws its sheet (run 055706, one variable, on GL; run 060829 on Metal, up to the location prompt). **Interim, a choice the owner will see:** `tools/nf/mk_extra.sh loc` leaves that job off RAM test roots with `locationd`, so the lookup fails at once; what needs FairPlay stays unavailable, as it was, and the installed system is not changed. **Wi-Fi location works by the owner's eye (2026-11-06, run 083507: 「map可以加载了，定位正常」), on that RAM root, with Wi-Fi joined by hand; the earlier expectation of no fix without Mescal was wrong. Mescal is not forged or emulated: it is simply absent. Accuracy has not been measured and the path is inferred (Apple's Wi-Fi positioning from `locationd`'s scans).** GNSS has transport-level reports but no satellite/fix evidence and no `locationd` bridge. Diagnosis: research/next-features.md section 17.

## K13 Security properties — all · **By design (stand-ins)** · S3
AMFI and Sandbox answer "allowed" to everything; entitlements come from the signature; no trust cache. `AppleKeyStore` models a device with no passcode and never locked: keychain items are "wrapped" with a reversible transformation that protects nothing, and the keychain lives in `/private/var`.
Do not use this device for secrets. Work: T28. Evidence: [kernel-standins.md](research/kernel-standins.md).

## K14 Persistence — RAM-only runs · **By design** · S3
`/private/var` is a memory file system remade empty each boot (capped at ~2 GB): home-screen layout, app data, Settings, networks and the Keychain do not survive; apps are registered anew and may appear on a different page unless `sb_delay` is set.
The installed boot keeps `/private/var` on flash (HFS+ on the OnePlus 8). Starting a persistent RAM run needs `var_store=1 ufs_write=1` and the owner's permission (it writes flash).
Occasionally a daemon fails on the fresh `/var`: run 060507 (2026-11-05, `ramhfs`, 1 boot of 11 that night) had ScreenTimeAgent abort with "Failed to load local persistent store" (CoreData, `-[STDaemonPersistenceController _loadLocalPersistentStore]`); launchd restarts it. Not seen in the other ten.
Also seen once (run 085644, 2026-11-05, 1 boot of 14): mobiletimerd[120] jumped into a read-write page of the shared cache ("Data/Stack execution not permitted ... protections were read-write", exception 1 code 0x2, then a corpse), after another instance had logged "Settings is nil". Unexplained; whether it comes from the fresh `/var` is an inference from its neighbour above, not established.

## K15 UI rendering gaps — OnePlus 8 · **Bug** · S3
- The lock screen's wallpaper itself draws: the default one, by CollectionsPoster through Metal, in all four boots of 2026-11-11 (log). Long-press on it (the poster switcher) is turned off by our own hook (`poster_switcher_allowed()` in `inject_sb.c`), because PosterBoard's screen came out black; cause unknown. That was seen before the compositor moved to Metal and **has not been tried since**. A lead, by timing only: when PosterBoard takes its snapshot after the screen goes off it asks Metal for a texture layout object, gets nil (K42), and draws nothing afterwards.
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
Installed since 2026-11-11: the bundle "Cellular 2026-11-11" in boot slot 1 (kernel `cell-0a663690`, Metal over Vulkan for the whole system, Wi-Fi, Bluetooth, the cameras, the modem); "Metal 3 2026-11-06" stays in slot 0 as the menu's previous version. It is behind the source by one revision of `hk_commcenter` (K41) and by whatever lands after it. Its line has `syssleep=0` (K10: Wi-Fi does not survive a system sleep), so the phone never sleeps deeply and its drain is unmeasured. The next install goes into slot 0, over "Metal 3". T03.

---

### How to add an entry
Verify against code and evidence, state Kind, Severity, device, expected vs actual, cause if established, evidence, workaround only if known, the TODO id. Fixed? Change the kind to *Fixed (date)*, keep the diagnosis, and update the summary table.

## K22 Agent toolkit activation — developers · Unverified · S3
The physical phone still runs loader 0.5 without the new `getvar:iphont` extension. New agent commands correctly return `old_loader`; deep/control features cannot be considered working there yet. The owner deferred a RAM-only reload on 2026-10-26. No flash operation is needed to validate the extension. See [TOOLKIT.md](TOOLKIT.md), TODO T36.

## K41 Cellular: what is left — OnePlus 8 · **Partial** · S2
- **Works (verified on the phone, 2026-11-11; installed as "Cellular 2026-11-11"):** carrier and signal, mobile data, SMS both ways with notifications, VoLTE calls both ways with sound, the speaker button. Details: [subsystems/cellular.md](subsystems/cellular.md). The kernel side is in `tools/nf/cell/`, iOS's side in `userland/src/hk_commcenter.m` and `inject_call.m`.
- **Not done or unverified:** a call's level (no calibration: the DSP refuses a volume step), the speaker and mute as a person hears them (the route switch is in the log of a real call), hold and call waiting, DTMF on the air, MMS, long messages over eight parts, a call over a Bluetooth headset, Personal Hotspot (the row does not appear), emergency calls (not tried), a modem restart with IMS up, sleep with the modem up.
- **The screen stays on at the ear:** there is no proximity sensor yet (K07), so a cheek can press what is on the screen during a call.
- **Fixed on the way, in every kernel built before 2026-11-11:** closing `/dev/oslog_stream` panicked (`kfree: ... found in heap data.* instead of *`): xnu's own `bsd/kern/subr_log_stream.c` frees with `kfree_type` what it allocated with `kalloc_data`. Anything that reads iOS's live log (`oslog`, `os_activity_stream`) ended the system when it stopped; with Wi-Fi on that is a cold-recovery hold. Fixed in `xnu-src` (not exported to `xnu/` yet: whoever runs `xnu/export.sh` next carries it) and in the cellular kernels (`tools/nf/cell/overlay/xnu/files/bsd/kern/subr_log_stream.c`).
- **The installed bundle's `hk_commcenter` is the first daily build:** if MobileGestalt ever says "no baseband" again it renames the kept file itself, too late to help. The next bundle should carry the newer daemon, which marks `init`'s stamp instead.

## K42 Metal on Vulkan: what is not implemented — OnePlus 8 · Metal (Vulkan) · **Partial** · S3
Read from the logs of the four boots of 2026-11-11 and from the source; nothing was run for it. Those boots had only the system's own interface
processes in them (no Maps, Safari, Photos or third-party app), so what apps would ask for is not in this list; T10 keeps the older, longer one.
The libvulkan on the phone is a day older than the source that was read.

| Kind | What | Who asks |
|---|---|---|
| Visible | Framebuffer fetch at 4 samples: the pipeline is refused | Siri's orb (refused in every boot), two of Maps' pipelines (K35) |
| Visible | `newTextureLayoutWithDescriptor:` returns nil | SpringBoard, PosterBoard (K15) |
| Visible | A vertex attribute that is read but was never described: the pipeline is refused | Messages' app view |
| Visible | A draw with no pipeline state set is dropped | the Fitness widget |
| Stand-in | A 4:2:0 surface goes through a CPU copy; a memoryless target really gets memory | the compositor, wallpapers, Maps |
| Absent | Tessellation, mesh shaders, indirect command buffers, ray tracing, sparse textures | nobody in these boots |
| Ignored | Depth bias, depth clamp, fill mode, more than one viewport, indirect draws | nobody in these boots; a 3D app would show it (inferred) |
| Speed | No precompiled pipeline library is found for this GPU; a refused pipeline is asked for again every frame | the compositor, chronod |
