# Input and peripherals: touch, keys, slider, haptics, flashlight, battery, camera

Everything that is a small device on the phone. None of Apple's drivers exist here, so each is a kernel driver that
feeds a character device (or an IOKit stand-in) and a library of ours inside a stock iOS daemon that turns it into
what iOS expects.

## Touch and buttons: `/dev/hk-touch`

iOS expects touches from Apple's multitouch driver through IOHIDFamily; neither exists. A producer (a kernel driver,
`virtio-input` under QEMU, or a script via `rwtouch`) posts `struct hk_touch` records (`xnu/files/bsd/sys/hk_input.h`:
`HK_TOUCH_DOWN/MOVE/UP`, `HK_BUTTON_DOWN/UP`, `HK_SLIDER`; coordinates 0..65535) to `/dev/hk-touch`
(`hk_input.c`, `hk_touch_post()`). `userland/src/inject.c` inside **backboardd** reads them and synthesises the
events: a "virtual" HID service (built in, one usage) turns samples into the events Apple's driver would send.
Findings behind it:

- backboardd takes for its touch screen a service that exists when its touch processing starts; one that turns up
  later is not noticed. CoreBrightness's five-second wait for an ambient light sensor
  (`-[BLControl waitForALSArrival]`) used to hide this race by accident; the wait is kept (a "brightness_fixes()"
  attempt to remove it made backboardd stop adopting the service: "still waiting for a main screen digitizer"). The
  service is therefore created repeatedly until the server has opened it (`make_service()`).
- UIKit's screen-edge recognizers (swipe up to leave an app, swipe back, pull down) accept only touches that Apple's
  controller marked as coming in over an edge, from the shape of the contact. All we have is where the finger landed,
  so a finger landing within 16/375 of the width or 16/812 of the height of an edge is marked as coming in over it.
- `TouchDetectionMode` from the window server (low byte): 1 report touches, `0xfe` wake mode (display off, a tap is to
  wake it), `0xff` off.
- Scripted touches (`rwtouch`) do not reach iOS's *system* gestures (swipe up for the app switcher, going home).
  Use `rwtouch swipe` from the bottom edge for the real gesture path and `rwtouch notify com.hackios.switcher` for
  the switcher. As of the last notes the switcher notification returns but does not change the screen when three apps run.
- Latency: `touch_lat=1` prints a line per sample (`tl: i TYPE posted US read US sent US`) and per SpringBoard
  run-loop turn; analyse with `tools/latency.py`, `tools/frames.py`, `tools/pipeline-latency.py`.

### Drivers

| Device | Chip | File | Notes |
|---|---|---|---|
| redwood | Goodix GT9916 ("Berlin-D") or FocalTech FT3680, SPI of serial engine 13 | `bsd/dev/hk_goodix.c` | `touchscreen=1`. GPIO96=1 and GPIO68=0 identify the FocalTech variant. No SPI framework: the driver does power (PM8350C LDO2 through RPMh), clocks, pins and the GENI engine in FIFO mode itself, or bit-bangs the four pins. **No interrupt**: a thread looks at the line 125 times a second (`touch_poll_us`). FocalTech firmware upload is `hk_focaltech_fw.h` (generated from `fw-touch/focaltech_ts_fw_m20.bin`, ~114 KB over SPI). |
| instantnoodle | Samsung S6SY761 ("SY771"), I2C `0x48` | `bsd/dev/instantnoodle_touch.c` | `board=instantnoodle`, `touchscreen=0` off. Pins gpio36/37 driven by hand in `instantnoodle_i2c.c` (open drain: low = output 0, high = input). The reason given, "the engine's FIFO path is disabled", was measured on the OnePlus 7 Pro (SM8150, `ios-instantnoodle`), not here; this phone's QUP 0 SE 6 (Bluetooth) has its FIFO. `i2c_geni=1` logs each bus's engine, `i2c_geni=2` uses the engine in FIFO mode (Linux `i2c-qcom-geni.c`) where it exists. **Verified on the phone 2026-10-27** for touch, gauge and slider (FIFO there on all three; touch read 2.1 -> 0.37 ms, no CPU spin); not yet the default, and not yet tried under a real finger. Reset gpio38, report line gpio39 (active low, TLMM summary interrupt 208+32). Supplies PM8150A LDO1 (1.8 V) and PM8150 LDO13 (3 V) through RPMh. Panel coordinates 1080 x 2400 scaled to 0..65535. Configuration commands are not acknowledged on this phone yet the chip reports touches after its boot-complete event. Interrupt-driven (`touch_irq=0`: 10 ms polling); worker at priority 92. Held in reset with the screen off. Since 2026-10-27 (built, unverified), as OnePlus's `sec_drivers_s6sy761.c`: the chip's own restart (boot complete 0x20) gets sense on again, a full event queue releases every finger, ESD resets the chip, an overflowing stack is cleared (0x62), ten failed reads reset the chip; and charger mode (`0x32` 2 cable in / 1 out, from `instantnoodle_battery.c`) is sent on a change and after every restart (seen on the phone 2026-10-27: "charger mode 2" with the cable in; the error paths have not occurred). |
| QEMU | virtio-input | `hk_virtio_input.c` | `virtio_input=1`; mouse with the left button is a finger |

**Never pass `touchscreen=1` on the OnePlus 8.** That argument selects redwood's Goodix driver; it touched GPIO68 and sent
the OnePlus to a NoC CrashDump. The OnePlus path is guarded now, but the profile does not pass it.

### Buttons (`hk_buttons.c`)

None of the three keys is a SoC pin: they are inputs of the PMICs on the SPMI bus, reached through the SoC's **PMIC
arbiter** (version 5), which gives each system on the chip a set of registers per PMIC peripheral ("channel").
Redwood: power = KPDPWR_N and volume down = RESIN_N of the PMK8350's PON block, volume up = GPIO6 of the PM7325
(pulled up). OnePlus 8: PM8150 PON and GPIO6/7. The driver **reads only** (the one thing written is the arbiter's
command word); the firmware reads the same keys itself (fastboot/recovery choice) and has left them configured; the
driver verifies that before it believes them and a key that reads as held before it was ever seen released is not
believed. On the OnePlus 8 the power key and volume keys have interrupts (arbiter, PDC, SPI 481; see
[power.md](power.md)). Bring-up steps `buttons_steps=N`. The phone has what the iPhone X (whose userland runs here)
has: side button (`HK_BUTTON_LOCK`) and two volume keys, no home button. `hk_spmi_lock()` serialises all PMIC
transfers (two at once and the arbiter drops one).

### Alert slider (OnePlus 8, `instantnoodle_slider.c`)

A magnet in the slider and two iSentek IST8801 hall sensors on serial engine 5 (GPIO 4/5), "up" `0x19` and "down"
`0x18`, powered by PM8150A LDO10 (3.0 V). The factory calibration (`/mnt/vendor/persist/engineermode/tri_state_hall_data`,
three pairs `down,up`; boot-arg `slider_cal=D,U,D,U,D,U`, in the profile) decides the position (nearest of three within
22); without it OxygenOS's rule (above 30 up, below -30 down). A position counts after two equal reads. Interrupts
not used: both sensors are read every 100 ms (300 ms while dark). Reported as `HK_SLIDER` records and
`kern.hackios.slider`. iOS side: backboardd keeps ring/silent (middle = silent, down and up = ring) and SpringBoard shows
its ringer HUD; "up" drives **Do Not Disturb** through an assertion of SpringBoard's HID client
(`com.apple.springboard.donotdisturb.hid`, named in its entitlements), dressing the ringer pill (`SBRingerPillView`)
with a moon and "Do Not Disturb". The first DND call is deferred 20 s or SpringBoard SIGSEGVs. Verified on the phone by
the owner.

### Haptics (`hk_haptic.c`, `haptic=N`)

Redwood: Awinic AW86927 (ID 0x9270, Xiaomi's `aw8692x` 1.7.0), I2C of serial engine 9 (GPIO 36/37), address `0x5a`,
reset GPIO 35; the motor's f0 is 175.5 Hz. OnePlus 8: Awinic AW8697 with the waveforms of the phone's
`/vendor/firmware/aw8697_haptic.bin` (`aw8697_haptic_170.bin` is used; 11 waveforms: 1..10 short, 11 looped). The factory's f0 trim (`haptic_trim=`, persist `engineermode/f0_cali_data` = 11 here: measured f0 175 Hz against OnePlus's 170, 10000·5/(25·175)) goes into TRIM_LRA as OnePlus's driver writes it, so the motor resonates at the 170 Hz the waveforms are made for (2026-10-27: written and read back `0x0b` on the phone; the feel not judged yet). `haptic=1..4`
progress from reading to playing an effect; **`haptic=5` is the mode for iOS**: what iOS plays on its "Actuator" device
(`audio_plugin.c`) is written to `/dev/hk-haptic` as one-channel samples and each waveform becomes a tap (waveform 1) or,
if longer than a tap, the looped waveform until it ends (at most `HAPTIC_MAX_MS`, then rest for as long). Gain follows the
loudness (Android's own 0x80 from a third of full scale; five eighths for the softest; never above). Limits: Android's
own values only; any fault flag, failed read-back or unacknowledged byte puts the chip in reset for the run. Verified
on the OnePlus 8 (system feedback, CoreHaptics).

### Flashlight (`hk_torch.c`, `torch=N`)

Redwood: the PM8350C flash LED module (SPMI slave 2, `0xee00`), channel 0 at Android's torch current 125 mA
(12.5 mA resolution). OnePlus 8: PM8150L, both LEDs. `torch=1` looks, `2` lights for `torch_secs`, **`3` is the mode for
iOS**: `/dev/hk-torch` takes a level 0..1000 (thousandths of Android's current, never above it). Limits: the light on at
most `torch_max_s` (default 30, max 120) at a time; the module's fault and over-temperature flags, the PMIC's
temperature alarm and the battery's temperature (45 C) are checked ten times a second and any trouble switches it off for
the run. Every write goes through one function that takes only this module's registers, with read-back. iOS side:
the loader tells MobileGestalt the camera has a flash (`/product/camera "flash"`) and `libhackios-media.dylib` in
mediaserverd gives `FigFlashlight` (which finds no camera) a device of ours whose torch level is written to
`/dev/hk-torch` (Control Center's button and AVFlashlight). Verified on both.

**On by default on the OnePlus 8 since 2026-11-06 (`torch=3` in the board's defaults).** The kernel writes `/arm-io/isp` and `/product/camera`'s `flash` itself when the loader has not (`GenericARMPE.cpp`, `cameraNodes()`): the loader only reads its own command line, the defaults come after it, and the installed loader is not to change. **Verified on the phone by logs, RAM, new `/var`, no `cam=`, `torch=` or `bt=` on the line (runs 005220 and 013103): the Camera icon and Control Center's button are there.** With the camera present, FigFlashlight goes through the camera stream's `TorchLevel`, the path `inject_media.c` calls "not tried yet" (run 005752: 850 of 1000, 4.25 s, off, every write read back). The module's flags said it ramped up in those runs. **Owner-confirmed (the owner, by eye on the combined build 「metal-一次看」, 2026-11-06: 「闪光灯和蓝牙OK了」).**
A system sleep turns the light off first (`hk_torch_sleep()`; the limits are kept by a thread that stops with the system): built, not exercised.

**The camera's flash (`HK_TORCH_FLASH` on `/dev/hk-torch`, `camera_device.c`'s `flash_frame()`).** iOS sets the stream's `FlashMode` to the string `ForceFlashOn` or `ForceFlashOff` just before `StillImageCaptureNow`, whose value is empty (run 010240). Without a pre-flash or a computed jump of exposure: the LEDs light when the shutter is asked for, the usual AE brings the picture to that light, and the frame at most 800 ms later is the photograph; the LEDs go off as soon as that frame has been read. Kernel limits: `torch_flash_ma` a channel (default 500, never above Android's 1000), at most 1280 ms with the **module's own safety timer on**, 3 s of rest, refused below 15 % or 3600 mV or when those are unknown or the flashlight is on, off at any fault flag or supply droop.
**Verified by logs and numbers on the phone, RAM (no picture left it; `photo_info` reads them there):** runs 010913, 012611: 500 mA, ramped up, flags 0; in run 012611 off 840 and 866 ms after the ask (run 010913, before the off was moved ahead of the picture's rendering, ran to the 1200 ms asked for); in the same dark room the mean light of a photograph is 10.3 of 255 at 66 ms without the flash (run 011428) and 100-110 at 33 ms with it, none clipped. Camera's Auto fires it in the dark (`kFigCaptureStreamMetadata_NormalizedSNR` now from the frame's signal and gain; 6 electrons a count at 1x is an assumption).
**Owner-confirmed (the owner, by eye on the combined build 「metal-一次看」, 2026-11-06: 「闪光灯和蓝牙OK了」).**
**Not done or not known:** AE has not settled when the 800 ms are up (the picture is well exposed all the same); white balance differed between the two flash photographs of run 012611; the EXIF flash tag says 0 with the flash and 0x10 with it forced off (`FlashActive`, `FlashFired`, `LEDMainFlash`, `IsLEDMainFlashforAWB` in the metadata change nothing); Auto has not been tried in bright light; nothing above 500 mA has been tried.

### Battery and charger (`hk_battery.c`, `instantnoodle_battery.c`, `HKBattery.cpp`)

iOS finds every `IOPMPowerSource` (the first is "InternalBattery-0"). `HKBattery.cpp` is that service; values reach it
through `hk_battery_report()` with a `struct hk_battery_state` (percent in hundredths, voltage, current, temperature,
present, external, status, charges in uAh, cycles, USB type). `CurrentCapacity` is the percentage and `MaxCapacity`
100 (newer-battery convention); mAh values are `AppleRawCurrentCapacity` etc. Everything else (status bar, "Charging"
on the lock screen, Batteries widget, Settings > Battery) follows from powerd. The status-bar percentage switch is
turned on by the image (`SBShowBatteryPercentage`).

- **Redwood:** values from the audio DSP's battery manager (`hk_adsp.c`, `adsp=1`, `battmgr=0` off). Charger limits and
  JEITA are firmware-owned. Without `adsp=1` the loader owns the firmware charger context on core 0 (`battery=1`).
  Verified charging with `adsp=1` (+0.4 A from a computer's port); `sysctl kern.hackios.charger` has health and type.
- **OnePlus 8:** a TI **bq27541** fuel gauge (bq27411 variant if the type register says so) at I2C `0x55` on serial
  engine 16 (gpio48/49, pull-up and clock stretching needed), OnePlus's driver `bq27541_fuelgauger.c` as the register map.
  Charger presence from the PM8150B (SPMI slave 2, USBIN `0x1310` bit 4, state `0x1006` [2:0]). Reads every 5 s. The Warp
  fast-charge MCU (`0x26`) is left alone. Details of charge limits, off-mode charging and the empty-battery shutdown:
  [power.md](power.md).
- A UIDevice fallback in SpringBoard (`inject_sb.c`) still reads `kern.hackios.battery_soc/charging` when no power source
  exists; once `HKBattery` is there the hooks hand everything to iOS's own code.

### Cameras (redwood only: `hk_camera.c`, `cam=N`)

Redwood has four sensors: macro OmniVision OV02B10 (CCI 1 master 0, `0x3c`), wide Samsung S5KHM2SD 108 MP (CCI 0
master 1, `0x20`, chip ID `0xfad2`), front OmniVision OV16A1Q 16 MP (CCI 0 master 0, `0x10`, ID `0x1641`), ultra-wide
Samsung S5K4H7 8 MP (CCI 1 master 1, `0x2d`, ID `0x487b`), with the PM8008i supplying the LDOs. The driver was brought
up step by step: `cam=1` look (reads only), `2` power the sensor as Android does and read its ID, `3` the CSID test
pattern through RDI 0 / IFE-lite 0 into memory (`xl camera` fetches it), `4` the sensor's own frames through its CSIPHY,
**`5` the wide camera for iOS** (`/dev/hk-camera`; `cam_sensor=wide` selects it in the early modes). Limits: only Android's
voltages; every write read back; anything wrong switches all off.

iOS side (userland repo): `inject_media.c` gives mediaserverd's capture-device vendor the create function of our
capture device (`camera_device.c`), a `FigCaptureDevice` with two `FigCaptureStream`s (wide as the back camera, front
as the front), laid out like H10ISP's in iOS 16.5, formats named as the iPhone X's in CMCapture's
`D22/AVCaptureSession.plist`. Frames: RAW10 read from `/dev/hk-camera`, made into pictures by the CPU
(`camera_isp.c`: black level, lens shading, grey-world white balance and exposure gains, the camera's own colour matrix,
sRGB gamma, zoom; no noise reduction). Camera.app shows preview, focus (by contrast, continuous, and on a tap), and
takes HEIC photos (README claim). The portrait-mode Core Image prewarm that crashed mediaserverd is skipped.
No video or Live Photo recording. See [../KNOWN_ISSUES.md](../KNOWN_ISSUES.md).

### Cameras, OnePlus 8 (`instantnoodle_camera.c`, `cam=1|2`; all four chip IDs verified on phone, 2026-11-04)

`hk_camera.c` hands `cam=N` to `instantnoodle_camera.c` on `board=instantnoodle` (before 2026-11-04 `cam=N` on this phone ran
redwood's driver at redwood's addresses). The four sensors, from the live device tree and the module files in the phone's
`/odm/lib64/camera/` (Android's `vendor_a` has none):

| Name | Sensor (module file) | DT cell | CCI master (pins) | slave, ID | MCLK (RCG/branch, pin) | reset | CSIPHY | supplies (power kinds) |
|---|---|---|---|---|---|---|---|---|
| wide | Sony IMX586 48 MP (`semco_imx586`) | cam-sensor@0 | CCI0 M0 (101/102) | 0x1a, 0x0586 @0x16 | MCLK0 0x5000/0x5018, gpio94 | gpio145 | 0, 3 C-PHY trios | VANA S5 2.04 V + gpio26; CUSTOM_REG1 BOB; CUSTOM_GPIO1 gpio156; VAF PM8009 L5 2.8 V; VDIG L1 1.104 V; CUSTOM_REG2 L2 1.1 V; VIO L7 |
| ultra | Sony IMX481 16 MP (`ofilm_imx481`) | cam-sensor@1 | CCI1 M1 (107/108) | 0x1a, 0x0481 @0x16 | MCLK3 0x5054/0x506c, gpio97 | gpio78 | 3, 4 D-PHY lanes | VANA BOB + PM8009 GPIO1; VDIG L4 1.056 V; VIO L7; CUSTOM_GPIO1 gpio159 |
| front | Sony IMX471 16 MP (`truly_imx471`) | cam-sensor@2 | CCI1 M0 (105/106) | 0x10, 0x0471 @0x16 | MCLK2 0x5038/0x5050, gpio96 | gpio90 | 2, 4 D-PHY lanes | VANA BOB + gpio24; VDIG L3 1.056 V; VIO L7 |
| macro | GalaxyCore GC2375 2 MP (`shine_gc2375`) | cam-sensor@3 | CCI0 M1 (103/104) | 0x17, 0x2375 @0xf0 | MCLK1 0x501c/0x5034, gpio95 | gpio144 (power-down pin, high = off) | 1, 1 D-PHY lane | VANA BOB + gpio139; VIO L7 |

Which module sits in which cell was inferred (slave addresses, supplies, the actuator); `cam=2`'s chip IDs confirm it for
all four cells (below). Supplies are RPMh votes (`instantnoodle_supply()`). The camera rails' PMIC
peripherals have no arbiter channel of the application processor (`cam=1`, run 010516), so RPMh's completion of the vote
is the read-back, then 2 ms to settle. S5 (shared with Wi-Fi) is voted 2040 mV and left at 1900 mV; BOB is voted 3312 mV
(its `qcom,init-voltage`) and left there; neither is ever switched off. TLMM writes keep the configuration's bits 10 and up, but
for an eGPIO's ownership: gpio146-179 are eGPIOs (cfg bit 11; while bit 12 is clear the pin is the low-power island's and
the TLMM's level never reaches the pad), and gpio156/159 read `0x801` at reset -- the driver sets bit 12 as Linux's
`msm_pinmux_set_mux()` does, puts the found register back at the end, and reads every driven pad back by its input. MCLK is 19.2 MHz from the crystal; the GC2375's module asks for 24 MHz from camcc's
PLL2, which `cam_pll2=1` starts as Linux does (a Zonda PLL at 0x2000, `cam_cc_pll2_config`: L 0x4b = 1440 MHz) with the
MCLK's RCG on its early output (source 5) divided by 10, M/N 1/6 -- every register read back, the RCG and then PLL2 put
back as found (without the boot-arg the GC2375 runs, and answers, at 19.2).

**On the phone** (RAM run 044740, 2026-11-04 04:47, `cam=2`, all four in turn, `wifi=0`, nothing flashed; verified on
phone): ultra IMX481 `0x0481`, front IMX471 `0x0471`, macro GC2375 `0x2375` -- each "as Android expects", at the first
read; every rail vote taken, the titan top GDSC up (camcc's GDSC clock was found on), every pin and clock put back, the
PM8009's GPIO 1 back, normal return (881500-byte log). **The wide IMX586 did not answer** at 0x1a on CCI 0 master 0 after
its full 9-step sequence (S5+gpio26, BOB, gpio156, L5, L1, L2, L7, MCLK0, gpio145; every register read back). Run 080458
(08:05, `cam=2 cam_sensor=wide cam_scan=1`): a NACK with SDA and SCL high before and after, and on that bus (read-byte
probes only) 0x54 and 0x72/0x73 answering -- the module file's EEPROM (`0xa8`) and lens motor (`0xe4`) -- while the OIS
(`0x7c`, an LC898124 whose `cam_vaf` PM8009 L6 is not switched on) and the sensor did not. So the bus and VIO were fine
and the sensor itself unpowered: its CAM_VANA2 enable, gpio156, is an eGPIO that was still the island's (above), so its
2.8 V never came. **Run 081247** (08:12, kernel SHA 4ee4b55d..., `cam=2`, all four, eGPIO claimed, every pad read back;
verified on phone): **wide IMX586 `0x0586`**, ultra `0x0481`, front `0x0471`, macro `0x2375`, each "as Android expects" at
the first read; everything put back (gpio156/159 to `0x801`), S5 left 1900 mV and BOB 3312 mV, normal return (884317
bytes), no panic. L6 was not needed. **Run 082733** (08:27, kernel SHA c2726e82..., `cam=2 cam_pll2=1`, all four;
verified on phone): the same four IDs, the macro's now at **24 MHz from PLL2** -- PLL2 as found mode 0, L 0, USER_CTL `0x8`,
CONFIG_CTL `0x08200920` (Linux's already), CONFIG_CTL_U `0x05008001`; locked at once (mode `0xe0000007`), MCLK1's RCG cfg
`0x2513` with M `0x1`, N `0xfa`, D `0xf9`; afterwards both back as found, normal return (852029 bytes), no panic. (The line "camera AHB clock off again, as it was found (0)" is the CBCR read right
after its enable bit is cleared, before CLK_OFF sets.)
GCC's camera AHB clock and camcc's GDSC clock (always on under Linux) are switched on first if found off and put back at
the end. Host-tested with a model of the PMICs, camcc, TLMM (eGPIO ownership included) and the sensors (scratch harness,
not in the repository): all four IDs read in the module files' order, everything restored, shared rails never switched
off, a supply fault or a pad that does not follow stops the run, `cam=1` writes nothing; the pre-fix driver reproduces the
wide sensor's silence there. (Written then: "not built for this board: CSIPHY v1.2.1, CSID 480 / IFE-lite 480, SMMU, frames,
iOS" -- all of it is now there for the front sensor, below.)

**Frames and iOS (`cam=3 cam_step=1..5`, `cam=5`; the front IMX471 only; verified on phone, 2026-11-04).** The frame path is
CSIPHY 2 (4 D-PHY lanes) into CSID-lite 0 RDI 0 and IFE-lite 0's write master 0, writing RAW10 through the apps SMMU (a free
context and match registers of our own, Android's IFE stream pairs 0x800/0x840 -- the hypervisor refuses 0x801/0x841) into
memory with an unmapped guard page after each place; the steps and their runs are in [../TODO.md](../TODO.md) T07. `cam=5` serves
`/dev/hk-camera` with redwood's protocol (`sys/hk_camera.h`) from `instantnoodle_camera.c` itself (redwood's `hk_camera.c` is not
touched): a 3-place ring, polled every 1 ms (GIC SPIs 500/501 stay off), the sensor's exposure and gain from userland's AE
(Sony's gain code), the DMA stopped first on every way out. Userland is redwood's (`libhackios-media.dylib` in mediaserverd,
`camera_device.c` + `camera_isp.c`) with OnePlus 8 changes: the Back Camera stream asks for sensor 1 (the wide IMX586; until run 124917 it was given the front sensor's frames, `turned`), and tuning files are looked for by the OnePlus names (`camera-imx471.isp` etc.; none is built).
**Camera.app** (runs 104112 and 105351, image `out/camera-app-root.apfs` = the camera probe root + this dylib +
`userland/rc.jobs.camera-modes`): live preview at 1504x1128, a tap shows the focus square (nothing moves: the IMX471's DT cell has no
lens motor and the kernel takes FOCUS as a no-op), the shutter gives a 4032x3024 HEIC (3.4 s to encode in software), Photo / Video / Portrait modes change with no
mediaserverd death; every kernel session ended with 0 places written over and the guard pages intact. Two mediaserverd crashes
had to be fixed first (both userland, found with `exctrace=1` and `log_launchd=1`: [../research/ios-userland.md](../research/ios-userland.md) §3):
the filter thumbnails' node writing through a nil Metal filter, and the stream keeping the graph's "no handler" value as a
handler block. The Back Camera's picture is `turned` (the front sensor is mounted the other way round: DT roll 270 against the
back ones' 90; upright since run 110059). **The moving pictures are made by the GPU** (`camera_gpu.c`, OpenGL ES 3 through the
image's Mesa in mediaserverd, run 112756): the same picture as the processor's (at most 1 apart), 12-13 ms a frame on one
thread mostly waiting where the processor's took 90-140 ms of CPU, 30 frames/s, no throttling; the photograph and lens
shading stay on the processor; on a failure or a picture over 50 ms it gives the GPU up for the process. On the processor the
OnePlus 8 halves the preview to 15 a second at heavy thermal pressure. The wide IMX586 has a frames' path up to CSID-lite 0
(`cam=3 cam_sensor=wide cam_step=2`, run 122836): CSIPHY 0 on three C-PHY trios (redwood's CSIPHY v1.2.1 C-PHY settings),
4000x3000 RAW10 packets with no ECC or CRC error; not yet into memory or to iOS. Boot-arg `campattern=1` makes the GPU draw moving grey stripes
instead (a diagnostic: two pictures mixed in one break them). Open (TODO T07): the kernel's 1 ms polling and the per-frame copies, the other three sensors' paths (the wide IMX586 is C-PHY),
an IMX471 tuning, recording.

### Sensors, ambient light, location

On the OnePlus 8, the sensor processor (SLPI) runs SEE and exposes QMI service 400 through QRTR. RAM run 030448
(ticket 218) supplied the OEM light/proximity board records. Both `wise_light` and `proximity` then advertised a
sensor identifier. No standard `ambient_light` or accelerometer appeared in that run. The captured OnePlus board
description selects the BMI260 and its orientation. Actual motion samples, near/far transitions, front lux, automatic
panel brightness and iOS rotation each require separate device evidence. GPS runs through the external SDX55 modem.
Ticket 207 received 150 position reports with zero fixes. Subsequently 225 captured two native proximity states and their transition, and 226 brought up genuine BMI26x accel and gyro streams and matched six timestamped vectors each. Ticket 232 captured a changing OEM light signal and matched its native userspace readback. These are independent of final Core Motion and automatic screen brightness behavior. See [the current handoff](../HANDOFF.md).

## Files

`xnu/files/bsd/dev/hk_input.c`, `hk_goodix.c`, `hk_virtio_input.c`, `instantnoodle_touch.c`, `instantnoodle_i2c.c`,
`hk_buttons.c`, `instantnoodle_slider.c`, `hk_haptic.c`, `hk_torch.c`, `hk_battery.c`, `instantnoodle_battery.c`,
`hk_camera.c`, `instantnoodle_camera.c`; `xnu/files/iokit/Kernel/HKBattery.cpp`; `xnu/files/bsd/sys/hk_input.h`, `instantnoodle.h`. Userland repo:
`userland/src/inject.c`, `inject_sb.c`, `inject_media.c`, `camera_device.c`, `camera_isp.c`, `rwtouch.c`, `battery_test.c`,
`torch_test.c`.
