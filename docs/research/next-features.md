# Research: what to build next on the OnePlus 8 -- Bluetooth, flash, modem and the rest

**Status (2026-11-05): research and design only. No phone, no kernel or userland code, nothing run.** The owner's words: 「同时隔壁开一个研究下接下来功能点 比如 蓝牙 闪光灯 modem」. **Since 2026-11-06 the owner has picked flash, Bluetooth, Wi-Fi location, then modem and GNSS (「1，2，3，6先做吧」, 「連 modem 一起做」): what was found doing them is in section 17 and overrides what is above where they differ.**
Read [hardware-qualcomm.md](hardware-qualcomm.md) before any register named here. Boots that only the phone can answer are listed in section 13, not run.

Tags: **[observed]** seen on this phone, in its partitions or in its captured device tree (`../oneplus8-audit/device-tree.dts`), or in the iOS 16.5 files on the host (names only);
**[code]** read in this repository's code; **[source]** read in public code or pages, listed in section 16; **[inferred]** follows from those but was not seen; **[hypothesis]** a guess to test.
"Days" are working days at this project's pace, and they are estimates. For scale: the cameras went from the first register look (run 010516) to Camera.app on the IMX586 and the 0.5x in about 14 hours (T07). The telephony numbers have wide error bars.

## 0. The answers in brief

| Feature | State today | What the owner gets | Boots | Days | Depends on |
|---|---|---|---|---|---|
| Flashlight on by default | works with `torch=3`, which no default boot passes | Control Center's button back | 1-2 | hours | nothing |
| Camera flash | none | flash photos in the dark | 4-8 | 1 | the flashlight |
| Bluetooth on by default | buds, A2DP, HFP and BLE verified 2026-10-27, opt-in | headphones and BLE on every boot | 3-5 | 1 | BT firmware read again, the owner with buds for one boot |
| Location by Wi-Fi | `locationd` denied | Maps' position, Time Zone stops spinning | 2-4 | 0.5-1 | its job in the root |
| Sensors (SLPI) | SLPI boots, service 400 answers, its sensor list is empty | rotation, auto-brightness, proximity, compass, motion | 15-25 | 4-7 | audio DSP and SLPI running together |
| Modem up (SDX55) | nothing on this phone | a modem that registers, read by a test tool | 15-25 | 3-6 | QRTR services of ours (sensors build them first) |
| GPS (GNSS) | nothing | a real fix | 5-8 | 1-2 | modem up, `locationd` |
| Mobile data in apps | nothing | Safari on 4G/5G, no status-bar bars | 5-10 | 1-2 | modem up |
| Cellular as iOS sees it | CommCenter denied | bars, Settings > Cellular, SMS in Messages | 30+ | 2-4 weeks | modem up, a measured route (section 3.3) |
| Calls | nothing | calls | unknown | weeks, may stop at VoLTE | all of the above, voice path, earpiece, proximity |
| USB-C analog headphones | not done | wired audio by passive adapter | 8-12 | 2 | nothing, but USB-C is the development cable |
| Earpiece | not done | the receiver route | 1-2 | hours | useful only with calls |
| Macro camera | chip ID only | a 2 MP macro | 4-6 | 1 | a place in Camera.app's UI |
| Portrait depth | offered, no depth | real Portrait | unknown | 1+ weeks | Metal, a matte source |
| NFC | not done | tag reading in CoreNFC apps, no Apple Pay | 6-10 | 2-3 | nothing |
| Fingerprint | not done | nothing while there is no passcode | not estimated | TrustZone work | T15 |

**Six things this research changes:**

1. **The flashlight is off in every default boot, and the same gate may hide Camera.** `torch=3` is in neither `HK_BOARD_DEFAULTS` nor the installed command line [code]. The loader builds MobileGestalt's "flash" and the `/arm-io/isp` camera node from `torch=` and `cam=5` **on its own command line** (`xnuloader/loader.c`, `torch_requested`, `camera_requested`), and the kernel's defaults are added after the loader has run [code]. `run_cam_probe.py` leaves `cam=5` off that line for a defaults kernel [code]. So on a new `/var` such a boot should have no Camera icon [inferred, not seen: HANDOFF's check of the defaults was by log lines]. Check this before dropping `cam=5` from an installed line (section 2.4).
2. **Bluetooth is mostly done, and on this phone.** The request called it partly done on redwood. It is not: `devices/redwood.md` lists Bluetooth as not working, and all the evidence is the OnePlus 8's [observed]. What is left is turning it on and retesting it beside Wi-Fi on the current kernel.
3. **This phone has no in-SoC modem.** The `qcom,mss@4080000` in T06 and `subsystems/thermal.md` is redwood's. The device tree here has no `qcom,mss` node, it has `mdm0: qcom,ext-sdx55m` on PCIe 2, and the boot line says `androidboot.baseband=mdm` [observed]. The SDX55 is flashless: the AP hands it its firmware.
4. **The emulated iPhone is the Qualcomm variant.** `hw.machine` is iPhone10,3 [code], and the root carries both `Mav17.Release.bbfw` and `ICE17.Release.bbfw`, since this IPSW serves iPhone10,3 and 10,6 [observed]. iPhone10,3 being the Qualcomm model and Mav its firmware is outside knowledge, not read here [inferred]. CommCenter links `libQMIParserDynamic` beside `libARI` [observed]. So iOS's telephony already speaks QMI, and the SDX55 is a QMI modem. That does not make it easy, but it is the reason to measure the real CommCenter before writing a replacement (section 3.3).
5. **Sensors are much closer than T04 says, and they are not on the audio DSP.** They are on the SLPI (PAS 12, PID 3, its own GLINK edge), which already boots here. The missing part is the sensors PD and its reverse file system (FastRPC), and [hexagonrpc](https://github.com/linux-msm/hexagonrpc) is that part in open source [source].
6. **Location does not need the modem.** GNSS does. Apple's Wi-Fi positioning needs `locationd`, which is denied, and a working scan, which we have [inferred; section 5].

**Recommended order (section 14):** flashlight and flash, then Bluetooth, then Wi-Fi location, then sensors, then modem and GNSS, then mobile data, then iOS telephony. Fingerprint stays with TrustZone at the end of the owner's roadmap.

---

## 1. Bluetooth (QCA6390's BT half) -- T05

### 1.1 What exists on this phone

- **[observed]** Verified 2026-10-26/27, RAM runs: native BLE discovery before and after a sleep, a 22-byte GATT write and read-back (`213836`); CMF Buds Pro 2 paired through Settings with authentication and encryption (`221539`); AAC over A2DP heard by the owner (`010727`); HFP/SCO voice links, no stall (`010727`); Voice Memos recording through the buds (`012839`). Details: [../subsystems/bluetooth.md](../subsystems/bluetooth.md).
- **[code]** `bsd/dev/instantnoodle_bt.c` (power, QUP0 SE6 UART), `hk_bt.c` (firmware, H4, in-band sleep, control socket `com.apple.uart.bluetooth`), `iokit/Kernel/HKBluetooth.cpp`; userland `btboot.c`, `inject_bt.c`, `bt_scan.m`. Opt-in: kernel `bt=1`, image `BLUETOOTH=1`. The driver is in today's kernels [observed, string in `out/kernels/mdp-bb263517.mach`].
- **[observed]** Not seen working: BLE authenticated encryption, general foreground BLE discovery, reconnect after sleep, bonds across a reboot, Wi-Fi coexistence. **No Bluetooth run since 2026-10-27**, and since then Wi-Fi, sound and the camera became default-on.
- **[observed]** `fw-bt/` is not on this machine, and `firmware-reader/` has no reader for the `bluetooth_a` partition yet (the loader's `oem fwread` allows that partition).

### 1.2 How Android does it here

Same hardware path: QUP0 SE6 UART, `BT_EN` gpio21, in-band sleep, firmware `htbtfw20.tlv` and `htnv20.bin` from the `bluetooth` partition, Linux's `hci_qca.c`/`btqca.c` as the reference [source]. Vendor HAL `android.hardware.bluetooth@1.0-service-qti` [observed in `vendor_a`]. Nothing in Android's path is missing from ours.

### 1.3 What iOS 16.5 expects

Already answered [observed]: stock `bluetoothd` and the BTServer jobs, `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,3000000` instead of the Skywalk/`AppleConvergedIPC` path, our `btboot` under BlueTool's Mach service, `inject_bt.c`'s guarded init-table changes, `BTAudioHALPlugin.driver` kept beside HKAudio. A2DP goes over a Skywalk user pipe, and this kernel carries it. All hooks are guarded to 20F66.

### 1.4 What it needs

1. Read the two firmware files again from `bluetooth_a` (a small reader on the pattern of `read_adsp_fat.py`).
2. Add `bt=1` to `HK_BOARD_DEFAULTS` and `BLUETOOTH=1` to the root build. **Test it with everything on**: Wi-Fi, speakers, camera.
3. One boot with the owner and the buds: music with Wi-Fi in use, a Safari download at the same time, a sleep and a wake. Coexistence has never been run and T18 lists it as not done; the antenna switch line is gpio124 `SW_CTRL` [observed].
4. Bonds across a reboot need a kept `/var`, so that check waits for the next install the owner approves.
5. Decide sleep's behaviour with a link up. Whether a headset's reconnect wakes the phone is not designed yet [inferred].

**Hardware risk: low.** No CrashDump is on record from these runs [observed]. Rails are shared with Wi-Fi and already reference-counted, with 100 ms between transitions [code]. Known quirk: the first bring-up after power-on times out at 115200 baud and the existing power-cycle retry recovers it [observed].

### 1.5 What it gives

Headphones and speakers, BLE devices and later a headset for calls, on every boot. **3-5 boots, 1 day.** AirDrop, Handoff and the like are not part of this; they want AWDL and an Apple identity.

---

## 2. Flashlight and camera flash

### 2.1 What exists on this phone

- **[code]** `bsd/dev/hk_torch.c`: PM8150L flash module, SPMI slave 5 `0xd300`, both LEDs lit together, **at most 125 mA a channel** (Android's tree says 300, max 500), one checked write function, a 30 s limit, fault, over-temperature and battery checks 10 times a second. `/dev/hk-torch` takes 0-1000.
- **[observed]** `devices/instantnoodle.md` records it as working with `torch=3`. No uncompressed log under `logs/` on this machine has `torch=3`, so that result is quoted from the docs (the logs here are partial).
- **[code]** Two iOS paths, not yet run together: with no camera `inject_media.c` gives FigFlashlight a device of ours; with a camera `camera_device.c` handles the stream's `TorchLevel`. `inject_media.c` says of the second one: "not tried yet".
- **[code]** The default boot has no flashlight: `HK_BOARD_DEFAULTS` is `wifi=1 wifi_bdf_native=1 adsp=1 sound=4 sound_amps=1 haptic=5 cam=5 cam_after=10`, `out/camera-install-cmdline.txt` has no `torch=` either, and `run_cam_probe.py` passes none. Without the loader's "flash" Control Center has no button.
- **[code]** Camera flash: none. `StillImageCaptureNow` takes one frame and ignores the flash request. `FlashMode` is listed as a read-write property, always reads 0, and a set falls through to "set (not used)".

### 2.2 How Android does it here

**[observed, device tree]** `qcom,leds@d300` (`qcom,qpnp-flash-led-v2`) with `flash_0`/`flash_1` at 1000 mA each (max 1500, `duration-ms` 1280), `flash_2` disabled, `torch_0`/`torch_1` at 300 mA (max 500), `qcom,hdrm-auto-mode`, `qcom,vph-droop-det`, `qcom,thermal-derate-en`. Three `qcom,camera-flash` nodes share those sources and one switch.

**[source]** `cam_flash_core.c` drives it per request: low current for pre-flash, a fire on the requested frame, the switch prepared first (`qpnp_flash_led_prepare`). `leds-qpnp-flash-v2.c` adds the safety timer (`SAFETY_TMR` bit 7) and triggers LMH and charger mitigation (`MITIGATION_SW` `0x6f`, `trigger_lmh`, `trigger_chgr`). Mainline's `leds-qcom-flash.c` is a simpler reference for the same registers.

Register path is the same module `hk_torch.c` already writes. The reuse is nearly total on the kernel side.

### 2.3 What iOS 16.5 expects

- MobileGestalt's camera-flash from the device tree, which the loader writes [code].
- **[observed, names in the shared cache]** `kFigCaptureStreamProperty_FlashMode`, `kFigCaptureStreamProperty_FlashCalibrationData`, `FlashActive`, `flashActive`, `PreFlash`, `PreFlashComplete`, `PreFlashLux`, `PreFlashAFParameters`. On a real iPhone the ISP firmware runs the pre-flash sequence; here our `camera_device.c` has to play that part. **Which of these keys Camera.app actually waits for is not known** [hypothesis until logged]: `say_once` already prints `StillImageCaptureNow`'s dictionary, so the first boot with the flash set to On shows it.

### 2.4 What it needs

1. **The gate.** Either the loader also applies the board's defaults for `torch` and `cam`, or the line keeps `cam=5 torch=3` (14 bytes of the 607). The small fix is the line; the owner's rule ("what is proven takes no boot-arg") points at the loader. Add `torch=3` to `HK_BOARD_DEFAULTS` too. `init.c`'s `camera_answers()` stamp already resets MobileGestalt's cache when `flash=` changes [code].
2. **The flashlight's level.** Raise the cap from 125 to Android's 300 mA a channel if the owner finds it dim. All limits stay.
3. **The flash pulse in `hk_torch.c`:** a separate request carrying milliamps and milliseconds. Hard limits: at most 1000 mA a channel (start at 500), at most Android's `duration-ms`, the **hardware safety timer enabled** (never `0x13`, "no timer", as the torch has), refused below a battery level, cut at the module's fault flags, the mitigation register written as Android does. Same whitelist, plus the registers it adds.
4. **`camera_device.c`:** take `FlashMode`; on a still with flash, run a pre-flash at torch level for a few frames with the normal AE and AWB; fire; hold the flash for at least two frame times (rolling shutter: 66 ms+ at 30 fps); deliver the first fully lit frame with `FlashActive` in its metadata.

**Firmware from the phone:** none.

**Hardware risk.** Not CrashDump class: the same module and the same write function. New: **a supply droop at 2 A from the battery can reset the phone**, hence the start at 500 mA and the battery floor. LED heat is held by the hardware timer. Writes outside the module stay impossible by the whitelist [code].

### 2.5 What it gives

The Control Center button back, flash photos, and a Camera flash control that does something. **Button: 1-2 boots. Flash: 4-8 boots, under a day.** Depends only on the camera.

---

## 3. Modem and cellular (SDX55) -- T06

### 3.1 What exists on this phone

**Nothing runs.** No code touches PCIe 2, and nothing was ever observed from the modem. What can be reused [code]:

- `instantnoodle_wifi.c`: PCIe 0 brought up with Android's numbers (rails, GDSC, PHY sequence, PERST, link, iATU, SMMU stream). PCIe 2 is the same block at another address.
- `wifi/lx/src/mhi_host.c` + `lx_mhi.c`: an MHI host with BHI and BHIe, states and rings, 1128 lines. **No Sahara.**
- `hk_qmi.c` + `sys/hk_qrtr.h`: QMI client helpers on QRTR. They speak over GLINK today; MHI's `IPCR` channel would be a new transport.
- `hk_net.c`: a network interface in XNU.

**[observed]** Device tree: `mdm0: qcom,ext-sdx55m`, `qcom,mdm-link-info "0306_02.01.00"`; `qcom,pcie@1c10000` (PARF `0x1c10000`, PHY `0x1c16000`, DBI `0x64000000`, config `0x64100000`), PERST gpio85, WAKE gpio87, SMMU streams `0x1d00` and `0x1d01`, the PHY sequence in the tree. Sideband: AP2MDM status gpio56 and errfatal gpio57, MDM2AP status gpio3 and errfatal gpio1. The modem's PMIC: `qcom,pmxprairie@8` with `qcom,power-on@800` marked `qcom,modem-reset`, at SPMI slave 8. Dual SIM on the boot line (`opt_sim_count=2`).

**[observed]** `modem_a:/image` holds 328 files with subdirectories `sdx55m`, `sdx55`, `modem_pr`, `qca6390`. Partitions: `mdm1m9kefs1/2/3/c` (2 MB each where read), `mdmddr`, `mdm_oem_dycnvbk`, `mdm_oem_stanvbk`, `mdm1oemnvbktmp`, `modemst1/2`, `fsg`, `fsc`, `ssd`, `modemdump`.

### 3.2 How Android does it here

**Boot [source: OnePlus's kernel via LineageOS `drivers/esoc/`, `drivers/bus/mhi/controllers/`; observed: device tree and `vendor_a` names]:**

1. esoc's `sdx55m_pon_ops`: the reset is a **warm reset by SPMI** (`qpnp_pon_modem_pwr_off(PON_POWER_OFF_WARM_RESET)`), there is no reset GPIO. 150 ms later AP2MDM status (gpio56) goes high. There is no PBL-ready GPIO, so user space confirms the link.
2. PCIe 2 enumerates `17cb:0306`.
3. MHI's BHI hands the modem `sdx55m/sbl1.mbn` from `/vendor/firmware_mnt/image/sdx55m` (`mhi_qcom.c`: `.dev_id = 0x306`).
4. In the SBL's execution environment the `SAHARA` channels 2/3 (`mhi,ee = 2` in the tree) carry the rest. Vendor `mdm_helper` and `ks` do this.
5. ESOC_IMG_XFER_DONE, then MDM2AP status (gpio3) high, ESOC_BOOT_DONE, mission mode.

**Channels in mission mode [observed, device tree]:** `IPCR` 20/21 (QRTR), `QMI0` 14/15, `QMI1` 16/17, `DUN` 32/33 (AT commands), `DIAG` 4/5, `EFS` 10/11, `IP_HW0` 100/101, `RMNET_CTL` 109/110, `AUDIO_VOICE_0` 80, `ADSP_*` and `SLPI_*`. **There is no `NMEA` channel.** `IP_SW0` is absent too.

**AP-side services [observed names in `vendor_a`]:** `rmt_storage` (with `mdm1m9kefs1` in its strings), `tftp_server`, `pd-mapper`, `qrtr-ns`, `pm-service`/`pm-proxy`, `netmgrd`, `dpmQmiMgr`, `qcrild`, `port-bridge`, `ipacm`. Open equivalents exist for the first three: [rmtfs](https://github.com/linux-msm/rmtfs), [tqftpserv](https://github.com/linux-msm/tqftpserv), [pd-mapper](https://github.com/linux-msm/pd-mapper), and for the router [qrtr](https://github.com/linux-msm/qrtr) [source].

**Data:** Android goes through `rmnet` and IPA. Mainline uses `mhi_net` on the same `IP_HW0` 100/101 with no IPA (`pci_generic.c`, `modem_qcom_v1`) [source]. `mhi-netdev0` in this tree is also `IP_HW0` [observed], so a plain path exists without IPA [inferred].

**Voice:** `techpack/audio/dsp/voice_mhi.c` and the tree's `qcom,voice_mhi_gpr` set up a mailbox between the ADSP and the SDX over PCIe (`VSS_IPKTEXG_CMD_SET_MAILBOX_MEMORY_CONFIG`, `AUDIO_VOICE_0`, `mailbox_region`) [source, observed], so voice samples do not pass through the AP [inferred]. VoLTE on Android also uses `imsqmidaemon`, `imsdatadaemon`, `ims_rtp_daemon` [observed names].

**Public state.** Linux has run SDX55 as a **PCIe module** since 5.13 (`qcom/sdx55m/sbl1.mbn`, `edl.mbn`, Foxconn and Quectel entries) [source], but such modules normally boot from their own flash [inferred]. The postmarketOS page for this phone calls `sdx55m` unsupported with work in progress [source, search snippet only; the page itself refused our fetch]. **No public bring-up of the flashless phone case on non-Android was found.** Sahara over MHI is documented by a 2026 kernel series moving it out of QAIC [source], so the protocol is public.

### 3.3 What iOS 16.5 expects

**[observed]** launchd takes `com.apple.CommCenter` when the device tree has a `baseband` node, otherwise `-noncellular`. Today's noncellular one dies on `AppleIPAppender` (11 of 20) or AppleBaseband (9 of 20) [`ios-userland.md` §4]. Names in the files:

- IOKit: `AppleBasebandPCIControl`, `AppleBasebandPCIInterface`, `AppleBasebandPCIPDPManagerBase`, `AppleIPAppender`, `ApplePDPHelperInterface`.
- Libraries: `libTelephonyIOKitDynamic`, `libTelephonyBasebandDynamic`, `libQMIParserDynamic`, `libPCITransport`, `QMuxServerAccepter`, `QMuxClientIface`, `libATCommandStudioDynamic`, `libIPTelephony` (Apple's IMS client, in CommCenter), `libARI` (the Intel variant's).
- Boot and firmware: `AppleBasebandManager`, `abm-helper`, `abmlite`, `usr/standalone/update/Mav17.Release.bbfw`.
- Helpers: `CommCenterMobileHelper`, `CommCenterRootHelper`, `DumpBasebandCrash`, and the XPC names `com.apple.commcenter.coretelephony.xpc`, `.commcenter.xpc`, `com.apple.basebandd.xpc`.

**Routes.**

- **A. The real CommCenter on a pretend baseband.** Answer `AppleBaseband*` and `AppleIPAppender` just enough, skip Mav17's firmware load, and put a shim at the transport (`libPCITransport`/QMux, in-process, no iOS binary changed) that forwards QMI to the SDX55 and answers what it cannot. If it works you get all of iOS: bars, Settings, Messages, Phone. Unknown: how much of what CommCenter asks is Apple-only (research on iOS's QMI exists: a framework extracting its packet structures, CellGuard, Apple-specific services) [source, summaries only], how Mav17-era message versions fare against a stock X55, and how far `libIPTelephony` assumes Apple's modem side.
- **B. Our own daemon on the CoreTelephony XPC names.** Small for the status bar, and then every client feature (SMS through `imagent`, calls through `callservicesd`) is ours to rebuild, with each unknown selector a stall or a crash.
- **C. Data only, as an interim.** QMI WDS plus a network interface on `IP_HW0`, no CommCenter. iOS sees another interface. No bars, but Safari works on 4G/5G with Wi-Fi off. Worth doing first: cheap, and it proves the radio path [inferred].

**Recommendation: C once the modem is up, then measure A (2-3 boots, section 13) before choosing A or B.** The measuring needs no modem. My expectation is that A reaches SMS and data and has trouble at VoLTE [hypothesis].

### 3.4 What it needs

**M1, the modem boots (8-15 boots, 2-3 days).**
1. Read-only first (section 13): is the SDX55 powered, what do gpio1/3 say, does system 0 have an arbiter channel for slave 8 and may it write?
2. PCIe 2 from `instantnoodle_wifi.c`, parametrized. Rails shared with PCIe 0 (L9 is never switched off). Stream `0x1d01` by analogy with Wi-Fi's `0x1c01` [inferred]; a refusal reads back 0 and is harmless [observed for camera streams].
3. The warm reset through a new whitelisted write to slave 8's PON, AP2MDM status high, the link trained, `17cb:0306` seen. **That line is the first milestone.**
4. BHI with `sbl1.mbn` (`mhi_host.c`), then a Sahara server in the kernel on channels 2/3 feeding files from the small image's `fw/`.
5. MDM2AP status high, mission mode, errfatal watched. A modem crash must not take the AP down: stop it and log it.

**M2, the modem talks (7-10 boots, 1-3 days).**
1. `DUN` first: `ATI` and `AT+CSQ` are the cheapest proof of life.
2. QRTR over `IPCR`. Serve the service locator (shared with sensors, section 4), TFTP and rmtfs.
3. **rmtfs serves reads from the real `mdm1m9kefs*` partitions and keeps writes in RAM.** This is both the owner's no-UFS-writes rule and the protection of the unit's RF calibration and IMEI [inferred]. Writing those partitions needs the owner's fresh OK each time (AGENTS.md rule 1); there is no reason to ask for now.
4. A small `qmi_test`: DMS, UIM, NAS: SIM seen, registered, signal. QMI WDS for data. libqmi's definitions are the public reference [source].

**Data (5-10 boots, 1-2 days).** `IP_HW0` as an interface, QMAP framing, QMI WDS start, an APN from the owner, routes. `pdp_ip0` naming if iOS needs it.

**iOS (30+ boots).** Route A or B as measured. Dual SIM: slot 1 only first, the iPhone X has one.

**Calls (unknown).** The ADSP-to-SDX voice mailbox has no public non-Android implementation; postmarketOS's `q6voiced` covers MSM8916-era in-SoC modems [source]. Many networks have no 2G/3G left, so calls need IMS. The earpiece and proximity come with this.

**Firmware that must come from the phone:** `modem_a:/image/sdx55m/*` and possibly `sdx55/`, `modem_pr/` (carrier configs); the EFS partitions read live, never copied into docs; `mdmddr` if the boot asks for it (unknown).

**Hardware risk.**
- **CrashDump:** any PCIe 2 register before its GDSC and clocks, as PCIe 0 did [observed for PCIe 0]. Check GCC's `pcie_2_gdsc` state first and map the block only afterwards.
- **PMIC:** the write to slave 8 is a write to the modem's PMIC. One register, by whitelist, with read-back (hardware-qualcomm §9).
- **Calibration loss:** the rmtfs rule above. This is the only permanent risk here.
- **RF and heat:** the stock firmware keeps its own limits; Android adds thermal mitigation over QMI, which we would not have. The modem's sensors aren't in our thermal map (`mdmss-*` read nothing on SM8250) [code]. Watch battery temperature and the hottest zone on long sessions.
- **Sleep:** the modem and PCIe 2 through a system sleep are undesigned. First version: down before sleep.
- **Emergency calls must not be relied on** on this phone, ever. iOS's own emergency UI will need a look before SIM use is called done.

### 3.5 What it gives

M1 + M2: a registered modem and the base for GNSS. Data: mobile internet in apps. iOS: bars, SMS, Settings. Calls last. **Modem up 3-6 days. Telephony weeks.**

---

## 4. Sensors and auto-brightness (SLPI) -- T04, T38 (9)

### 4.1 What exists on this phone

Further along than T04's "Not Started" and on another DSP than `subsystems/thermal.md` says.

- **[observed]** `qcom,ssc@5c00000`, firmware name `slpi`, PAS 12, SMEM PID 3, reserved `0x90600000` + `0x1500000`, GLINK edge `dsps`, IPCC client 4, SMP2P 481/430.
- **[observed, 2026-11-03/04]** Run 184253: read-only SMEM. Run 225107: READY but GLINK refused a frame. Run 235547: QRTR hello, node 9, **8 services including 400 v1 inst 0 at port 12**. Run 000154: `slpi_glink_probe=2`, ambient_light lookup, QMI success, **an empty SUID list**. Then `slpi_glink_probe=3` with `default_only=false` and a 30 s window, **also empty** (`als-native/glink/all-lookup-result.json`; the older review text still says this request was not sent).
- **[code]** In the tree, default off: `hk_slpi_boot.c` (1434 lines), `slpi_glink_core.h`, `slpi_glink_probe.inc`, `instantnoodle_slpi_probe.inc`. **It will not run beside today's defaults:** it returns if `adsp` is set, and the board's defaults set it. It also retains its LCX/LMX/XO floors on purpose and requires a cold reboot.
- **[code] A real conflict:** `hk_adsp.c` loads the audio DSP at `0x8fe00000` + `0x2500000`, **overlapping the SLPI's reservation**. The captured tree's ADSP region is `0x91b00000` + `0x2500000`, directly after the SLPI's [observed]. `hk_adsp.c`'s comment quotes a tree where it was `0x8fe00000`.
- **[observed]** Host work, not on the phone: wire formats (`als-native/see_wire.py`), a registry bundle of the 13 ODM JSONs (`prepare_registry_bundle.py`), and an iOS ALS candidate (`als-ios/`: `als_bridge.c`, `hkals_cf.c`, `libhackios-als-candidate.dylib`, mock-tested).

**[observed]** ODM's 13 configs cover several alternatives: IMU `bmi26x`, `icm4x6xx`, `lsm6dso`; magnetometer `ak991x`, `mmc5603x`; `alsps` at bus 5 `0x46`, `cct` front and rear; SAR `sx9324`, `sx933x`; `stk2232`. **Which are fitted is not known** until the DSP says. None is a barometer.

### 4.2 How Android does it here

**[observed]** `vendor.sensors` runs `/vendor/bin/sscrpcd sensorspd`; `libssc_default_listener.so` over `libsdsprpc.so` gives the DSP `apps_std` (fopen, fread, stat, directories). Config in `/odm/etc/sensor/config`, `/vendor/etc/sensors/sns_reg_config`, output to `/mnt/vendor/persist/sensors/registry/registry`. HAL `sensors.ssc.so` plus `sensors.oplus.so`.

**[source]** Mainline does the same with free code:
- `hexagonrpcd -f /dev/fastrpc-sdsp ... -s`: `FASTRPC_IOCTL_INIT_ATTACH_SNS`, then the reverse tunnel (`adsp_listener_next2`), serving `/vendor/etc/sensors/config`, `sns_reg_config`, `/mnt/vendor/persist/sensors/registry/registry`, `/vendor/dsp` and `/sys/devices/soc0` from a directory.
- `libssc` is the QMI/protobuf client; `monitor-sensor` shows accelerometer, light, proximity on SDM845; iio-sensor-proxy lists newer Qualcomm phones on this path.
- `qcom_pd_mapper.c`'s `sm8250_domains`: `adsp_audio_pd`, `adsp_root_pd`, `cdsp_root_pd`, `slpi_root_pd`, `slpi_sensor_pd` (instance 90).
- `sm8250.dtsi`: SDSP's FastRPC at `fastrpcglink-apps-dsp`, streams `0x0541`...

Our node 9 announces service 66 instance 90 [observed], which fits that table's root PD: so **the empty list most likely means the sensors PD never started** [inferred; the review of the lookup found no fault in the request]. No public SM8250 run of this stack was found, only SDM845 and newer phones.

### 4.3 What iOS 16.5 expects

- **Light [observed, `als-ios/contract-and-integration.txt`]:** CoreBrightness's `BLControl`/AABC match page `0xff00` usage 4, `Built-In` a real `kCFBooleanTrue`, event type 12, integer lux in field `0xc0000`, `ReportInterval` in microseconds, 0 meaning stop. A virtual HID service, the touch path's pattern (`inject.c`, `make_service`).
- **Rotation:** `BKSAccelerometer` in BackBoardServices is the client API [observed names]; backboardd needs a HID accelerometer. Apple's public tables give `0xff00` usages accelerometer 3, proximity 8, gyro 9, with a later page `0xff03` for the accelerometer [source, older IOHIDFamily]. **The real match is unknown**: log backboardd's and `locationd`'s matching dictionaries.
- **CoreMotion lives in `locationd`** [observed: `CLIoHidInterface` in the cache, `com.apple.locationd.*` services], so motion needs section 5's first step. Real iPhones use `AppleSPU` (HID, compass plug-ins); we would stand virtual services in for them.

### 4.4 What it needs

1. **Coexistence.** ADSP moved to `0x91b00000` (re-verify sound; TrustZone took the ADSP at another address before, so the address looks free to choose [inferred]). SLPI's `adsp` refusal gone. Proxy votes released at the DSP's handover, as Linux's `qcom_q6v5_pas.c` does, so a run needs no cold reboot. One start per boot, an orderly stop on every way out.
2. **AP-side QRTR services:** the service locator from mainline's table. The modem wants the same thing.
3. **FastRPC to the SLPI:** GLINK channel `fastrpcglink-apps-dsp` on `dsps`, a free SMMU context and match registers for stream `0x541` under `hk_smmu_lock`, `INIT_ATTACH_SNS`, the invoke message format (mainline `drivers/misc/fastrpc.c`).
4. **The reverse tunnel:** read-only, by allowlist, `apps_std` from the small image's `fw/`. Registry writes to a RAM scratch first. Per-unit calibration from `persist` later, sensor groups only.
5. **SEE client in `hk_qmi.c`:** registry SUID, then each datatype's SUID, attributes (name, stream type, rates), stream config 513 or on-change 514, events 1025. Front and rear light sensors told apart by attribute. Timestamps converted, not receipt time.
6. **iOS:** `/dev/hk-sensors` as a ring, or the proposed `kern.hackios.als.sample`; virtual HID services in backboardd; ALS candidate first, then accelerometer, proximity, gyro, compass. Check a backlight change does not feed back into the light reading: the firmware has panel compensation paths [observed strings].

**Firmware from the phone:** `modem_a:/image/slpi.*` (in `firmware-reader/native-slpi`); `dsp_a`'s SDSP libraries (not read yet; the reader's allowlist has `dsp_a`); ODM configs (in `als-native/odm-sensor/config`); `sns_reg_config`; `persist`'s sensor registry (not in the reader's allowlist).

**Hardware risk.**
- Overlapping reservations are the known way to corrupt a DSP. Fix first.
- A second start of a stopped DSP reset the phone on the Wi-Fi and audio DSPs (`wifi_again`, `adsp_again`) [observed]; treat the SLPI the same.
- Stream `0x541` may be refused: harmless if so, and then there is no FastRPC and this plan stops [inferred].
- Never read an LPI or SSC register from the AP [observed for LPASS].

### 4.5 What it gives

Screen rotation, auto-brightness, proximity for calls, compass, motion and pedometer through CoreMotion. **15-25 boots, 4-7 days.** Depends on: sound re-verified at the new address. Builds the QRTR server the modem wants.

---

## 5. GPS and location -- T14, T38 (10)

**Exists.** Nothing. `locationd` is in `DENY` [code]. **Corrected 2026-11-06:** an earlier version of this page said it is not on the root. `ios165/system.files` lacks it, but that list is the small system image's; the phone's root is the complete iOS volume on `userdata` (`subsystems/storage.md`, `rootfs.slt` lists it at 31 MB) [code; to be seen on the phone]. So the job needs only to be let through. `gpsd` is there too.

**Android.** GNSS is in the SDX55. `android.hardware.gnss@2.1-service-qti`, `loc_launcher`, `xtra-daemon`, `slim_daemon`, `lowi-server` on the AP [observed names], QMI LOC (service 16) on QRTR [source; thermal.md]. libqmi and ModemManager speak LOC openly [source]. This tree has no `NMEA` MHI channel [observed], so **QMI LOC is the path**: it can stream NMEA indications [source].

**iOS.**
- `locationd` runs location and CoreMotion. `gpsd` loads a chip library by name (`/usr/lib/libfire2/3/6/7.dylib`, `libolaf.dylib`), a C++ `gnss::Device`, with transports `/dev/cu.gps`, SPI and GPIO through IOKit, and a "forcing gnssDevLibName" path [observed strings].
- `CLSimulationManager` is in the cache [observed]: the private API behind Xcode's simulated location.

**Plan.**
1. **Wi-Fi location now.** Take `locationd` out of `DENY` (nothing to add to the root), turn Location Services on in the skeleton as `timed_prefs()` did for time. It uses Wi-Fi scans and Apple's positioning service, which an unprivileged client can query for BSSID locations [source: Rye and Levin 2024]. **Whether it does so on an unactivated device with no Secure Enclave is unknown** [hypothesis]. It also stops Time Zone spinning (T38 (6)). `locationd` is large and interacts with everything: expect to look at what it costs after boot.
2. **GNSS after the modem.** QMI LOC start, XTRA and time injected from the network, then:
   - **first:** feed fixes in through `CLSimulationManager` from a small daemon. Maps cannot tell, though `CLLocationSourceInformation` marks them simulated [inferred];
   - **later, if wanted:** a `gnss::Device` library of ours under the real `gpsd`. More RE, a truer path.

**Firmware:** none beyond the modem's. **Risk:** nothing on Wi-Fi location except `locationd`'s load. GNSS as the modem.

**Gets.** Wi-Fi: Maps' position, time zone, location-using apps, **2-4 boots, 0.5-1 day**. GNSS: real fixes outdoors, **5-8 boots, 1-2 days after M2**.

---

## 6. USB-C analog headphones -- T08

- **Exists:** nothing on the output side. `hk_mic.c` brings the WCD938x up only for its mic bias on this phone (reset gpio32); redwood has the SoundWire path [code]. `fsa4480@42` sits on the speakers' bus and is deliberately not addressed [observed].
- **Android:** PM8150B `typec@1500` sees an audio accessory, FSA4480 switches the pins, WCD938x MBHC (`qcom,msm-mbhc-usbc-audio-supported`, `oplus,mbhc-use-usbc-detect`), output from `RX_CDC_DMA_RX_0` through the RX macro over SoundWire to `wcd938x-rx-slave` [observed tree, source]. Mainline: `wcd938x.c`, `lpass-rx-macro.c`, `usb/typec/mux/fsa4480.c` [source, pointers not read here]. Reuse: `hk_sound.c`'s ASM/ADM, `hk_mic.c`'s codec start.
- **iOS:** a new output device in `audio_plugin.c` with the headphone route, and a route change on plug-in [code, inferred]. Volume as speakers.
- **Needs:** TYPEC read-only in `instantnoodle_battery.c`, FSA4480 over the existing bus lock, RX macro clocks through the DSP's votes, codec RX, the guard's limits for headphones. LPASS's rules apply: votes before any macro register, never VA `0x30b` (hardware-qualcomm §1).
- **Practical snag:** the phone's USB-C port is also the development cable, so these tests run unplugged with logs read afterwards. USB-C **digital** dongles want USB host mode and an xHCI stack that does not exist here, and the port is held by the UEFI/loader: not planned [inferred].
- **Gets:** wired audio by passive adapter. **8-12 boots, 2 days.** Bluetooth has taken most of the need away.

## 7. Earpiece

**Exists:** the `tfa98xx_right` amplifier `0x34` has a receiver profile in `tfa98xx.cnt` [observed]. **Android:** the same amplifier with that profile. **iOS:** a receiver route in `audio_plugin.c`. **Needs:** the profile at setup, the guard's level for it. **Risk:** low, same amplifier and limits. **Gets:** only useful with calls (and proximity): **1-2 boots, hours**, done with calls.

## 8. Macro camera

**Exists:** GC2375, chip ID `0x2375` at 19.2 and 24 MHz (runs 081247, 082733), CCI0 M1, CSIPHY 1, one D-PHY lane [observed]. No frame path; T07 says "not planned". **Android:** a normal CamX sensor. **iOS:** has no macro device type, so it needs a UI seat: a fourth zoom button, a mode or auto-switch through `inject_camera.c`/`inject_media.c` [inferred]. **Needs:** `cam_sensor=macro` through CSID-lite as the others, its module file's mode, a tuning from `tools/camtuning.py` if the file has one. **Risk:** same as the other sensors. **Gets:** 2 MP close-ups. **4-6 boots, 1 day.** Do it when the owner wants it, not for its own sake.

## 9. Portrait depth

**Exists:** Portrait offered, no depth; its Core Image prewarm is skipped; the IFE-lite path streams one sensor at a time [observed, T07]. **Android:** dual-camera depth in closed Qualcomm/OEM algorithms. **iOS:** the iPhone X gets stereo depth from two sensors through its ISP firmware, so there is no Apple code to reuse for that part. PortraitFilters' Metal shaders are in the corpus [observed]. **Options:** (a) a person matte on one camera through Vision, and iOS's own blur on Metal; (b) a second stream on IFE-lite 1 (IMX481) and our own stereo on the GPU, which is much larger. **Needs:** Metal in mediaserverd (today its device is removed, `camera_gpu.c` uses GL) and (a) tested first. **Risk:** none to hardware. **Gets:** real Portrait photos. **1+ weeks, uncertain.** Cheap interim: hide the mode, one boot.

## 10. NFC -- T34

**Exists:** nothing; `nfcd` denied. **Tree:** `nq@28` `qcom,nq-nci` on `i2c@984000`, the slider's bus; IRQ gpio111, VEN gpio83, FIRM gpio110, CLKREQ gpio7 [observed]. **Android:** `nfc-service-nxp`, an NXP NCI chip, `esepowermanager` [observed names]. **iOS:** `nfcd`, `libnfshared`, `NearField.framework`, `CoreNFC`, IOKit `AppleStockholmControl` + `AppleStockholmControlUserClient`, `AppleStockholmSPMI`, firmware restore paths ("Stockholm only restore", Hammerfest), and "LogNCI" suggests NCI inside [observed names, inferred]. The iPhone X's chip is also NXP, so the protocol may be close [hypothesis]. **Needs:** a user-client stand-in, skip restore, I2C NCI on the slider's bus under its lock. **Risk:** a shared bus with the slider, otherwise low. **Gets:** tag reading in apps. Apple Pay and Wallet are impossible without the Secure Element and SEP. **6-10 boots, 2-3 days.** Low value.

## 11. Fingerprint -- T09

**Exists:** nothing. **Tree:** `goodix_fp` as a platform device with IRQ gpio23, reset gpio68, power gpio125 and **no SPI bus node**; `fingerprint_underscreen_support` in touch; `oplus,dsi-fod-brightness` in the panel [observed]. **Android:** HAL `android.hardware.biometrics.fingerprint@2.3-service.oplus`, Goodix libraries, `qseecomd`; a user log from an 8T shows the HAL failing at "QSEE TEE execute command" [source, weak]. So the sensor is driven from a TrustZone app [inferred]. **Hardware fact worth noting:** the known `touchscreen=1` CrashDump was at `0x0f144000` from GPIO 68, and GPIO 68 is this sensor's reset. **Corrected 2026-11-06:** an earlier version of this page read that as the pin being access-protected. On this SoC gpio68 is in the NORTH tile (`0x0f944000`; Linux `pinctrl-sm8250.c`), and `0x0f144000` is the same offset in the WEST tile, where redwood's one-tile address lands: more likely a register that is not there than a protected one [inferred]. **Still do not touch these pins.** **iOS:** `biometrickitd` (the pearl plist), `AppleBiometricServices`, `AppleBiometricSensor`, everything rooted in the SEP. **Needs:** QSEECOM, Goodix's TA and its closed protocol, plus a Touch ID or Face ID stand-in with no SEP. **Gets:** nothing while the phone has no passcode (K02, K13). **Not recommended.** It belongs with T15 at the end of the owner's roadmap.

## 12. Also open, in one line each

Video and Live Photo recording (T25: no hardware encoder path, possibly more missed than the items above; worth asking the owner) · Warp fast charging (T19, MCU protocol unknown) · the unverified charger and off-mode paths (T01, a safety item) · a drain figure (T13).

---

## 13. First boots to run (not run; the MDP session holds the phone)

Combined where nothing interferes, per the owner's taste for short runs. All RAM-only, no UFS writes.

**Boot A, loader only.** `oem fwinfo`/`oem fwread`: the file list of `modem_a:/image/sdx55m` and `sdx55`, `bluetooth_a`'s `image/`, `dsp_a`'s SDSP directory. Answers: which files the Sahara server must serve, and the BT and sensor firmware.

**Boot B, one boot for three small things**, image `BLUETOOTH=1` + `locationd` on the root + `ALLOW=locationd`, line `cam=5 torch=3 bt=1 exctrace=1 log_launchd=1`:
- Does Control Center show the flashlight, and does it light with `cam=5`? Does Camera show a flash control? Set it to On, take a photo: what does `StillImageCaptureNow`'s dictionary hold?
- Does the controller come up beside Wi-Fi and sound on today's kernel? A BLE scan of the known test UUID (`bt_scan`).
- Does `locationd` survive? Does it scan, reach Apple's service, give a position? With the same image, **drop `cam=5`** from the line once and see whether the Camera icon goes (question 1 of section 0).

**Boot C, read-only modem look** (needs a few lines of code, `mdm_probe=1`, nothing written):
- TLMM level of gpio1, 3, 56, 57, 85, 86, 87.
- GCC's `pcie_2_gdsc` state. Not one register of PCIe 2 itself.
- Arbiter: is there a channel for slave 8's `0x800` peripheral, who owns it, REVID and PON status read through it.

Answers: is the SDX55 powered at boot, and can the AP reset it at all? **This decides whether M1 is possible as designed.**

**Boot D, CommCenter measured** (no modem): a `baseband` node from the loader, `ALLOW=CommCenter`, an interposer logging `IOServiceMatching`, `IOServiceOpen`, `IOConnectCall*` and failed Mach lookups, for CommCenter and `abm-helper`. Repeat with stand-ins that answer until it asks for the transport. Answers: what route A costs. 2-3 boots.

**Boot E, two DSPs together** (after the ADSP move): sound plays, SLPI reaches READY and service 400, a clean stop, a second kernel without a cold reset. Then stream `0x541`: granted or refused?

**Also to log during Boot B:** the matching dictionaries of backboardd's and `locationd`'s HID clients.

---

## 14. Recommended order and why

1. **Flashlight, then flash.** The owner asked; a day at most; no dependency. **The loader-gate question comes first**, because it may also affect Camera on the next install.
2. **Bluetooth on by default.** The work is done and is rotting off the default path. One day, and the longer it waits the more the other drivers move under it.
3. **Wi-Fi location.** Half a day, in the same boots. Unlocks `locationd`, which CoreMotion needs, and tells us what it costs.
4. **Sensors.** Felt every minute of use (rotation, brightness), on the owner's list since the first day, closest to working of the big items, and it builds the two-DSP coexistence and the QRTR server the modem wants.
5. **Modem M1 + M2, GNSS.** The largest unknown of the hardware: start with Boot C, which can be done early since it is one read-only boot. If Boot C says the AP cannot reset the modem, this order changes.
6. **Mobile data.** Cheap once M2 stands, and it makes the phone useful away from Wi-Fi without iOS's telephony.
7. **iOS telephony, by Boot D's result.** Weeks.
8. **Calls with earpiece, by what 7 shows. Headphones whenever the owner asks.** Macro, Portrait, NFC after that. Fingerprint with TrustZone.

**Boot C and Boot D can run early**, ahead of their turn. They are cheap and they turn the largest estimate from a guess into a number. Everything waits for the phone lock: the MDP session has it now.

---

## 15. What could not be determined

**Without the phone:**
- Whether the SDX55 is powered at boot; whether system 0 may write slave 8 (Boot C).
- Whether `0x1d01` and `0x541` are granted by the hypervisor.
- The file names under `modem_a:/image/sdx55m`, the image IDs Sahara asks for, whether `mdmddr` is used. `mdm_helper` and `ks` are closed and only their names were read; the sequence is from esoc and the channel table.
- Whether EFS goes by rmtfs over QRTR or by the `EFS` MHI channel; both exist in Android's pieces.
- Which sensors are fitted; whether the sensors PD is the real reason for the empty list; whether SLPI and ADSP can run together.
- Whether `locationd` runs at all here, and whether Apple's service answers it.
- What Camera.app's flash control needs.

**From public sources:**
- **No public non-Android boot of a flashless phone SDX55 was found.** The postmarketOS page for this phone could not be read in full: it redirected to another host that refused the fetch; only a search snippet says unsupported, work in progress.
- No public non-Android path for the ADSP-to-SDX voice mailbox.
- How much Apple-only QMI CommCenter needs: research exists, read here only as summaries.
- LineageOS's device tree listing showed no gps, nfc or sensors folders on its front page; its deeper files were not read.

**From the files on this machine:**
- `locationd` was not read: this machine's `7z` cannot open `rootfs.apfs`, and the file is not in the extracted root. Its names above are from the shared cache.
- The boot logs here do not include the Bluetooth and torch runs (the older phone runs happened on another host), so those results are quoted from the docs.

## Docs that need correcting (not edited here, by this task's limits)

- `TODO.md` T06, `KNOWN_ISSUES.md` K06/K12, `subsystems/thermal.md`: `qcom,mss@4080000` and "the MSS" are not this phone's: it is an external SDX55 on PCIe 2.
- `TODO.md` T04, K07, `subsystems/thermal.md`, `subsystems/input-peripherals.md`: sensors are on the SLPI, not the audio DSP; T04 is not "Not Started".
- `TODO.md` T14: Wi-Fi location is not blocked by T06.
- `README.md` line 48: Bluetooth and the OnePlus 8's cameras are no longer "does not work yet".
- `devices/instantnoodle.md`: the flashlight's `torch=3` is not in the defaults.
- `hk_adsp.c`'s comment on `pil_adsp_region@8fe00000` disagrees with the captured tree.

---

## 17. Done and found, 2026-11-06 (RAM only, the owner asleep for most of it, nothing committed or installed)

**Runs** (`tools/nf/boot.sh`; images and job lists by `tools/nf/mk_extra.sh`, `mk_root.sh`, `mk_media.sh`; merged logs `logs/nf-*.txt`): nf1 005220, nf2 005752, nf3 010240, nf4 010913, nf5 011428, nf6 012611, nf7 013103, nf8 013625, nf9 014219, nf10 014823, Boot C 015309, nf11 015651, the gate 015855. No CrashDump in any of them.

**The gate of section 2.4: closed in the kernel, not the loader** [observed]. The kernel writes `/arm-io/isp` and `/product/camera`'s `flash` where the loader did not (`GenericARMPE.cpp`). The installed loader needs no change.

**Flashlight and flash: subsystems/input-peripherals.md has the design and the limits** [observed by logs and numbers on the phone; **owner-confirmed by eye on the combined build: 「闪光灯和蓝牙OK了」**]:
- What Camera asks: `FlashMode` is a **string**, `ForceFlashOn` or `ForceFlashOff`, set just before `StillImageCaptureNow`, whose value is empty. The pre-flash keys named in section 2.3 are never asked for on this path.
- Auto fires in the dark once `NormalizedSNR` is honest: the old formula said about 28 dB in a dark room, against thresholds of 14-22 in D22's `AVCaptureSession.plist`.
- With the camera present, the flashlight goes through the stream's `TorchLevel` and `HighCurrentTorchEnabled` (this page said "not yet run together"): it works, no `FigFlashlight` stand-in needed then.
- Not settled: section 2.4's "at least 66 ms" pulse was replaced by a light kept on for AE to find (about 840 ms from the ask), which costs that much shutter delay.

**Bluetooth: subsystems/bluetooth.md.** On by default in RAM kernels, up beside everything else; **owner-confirmed on the combined build (「闪光灯和蓝牙OK了」)**; traffic beside Wi-Fi not separately checked.

**Location by Wi-Fi: section 5's plan needs less, and stops at the network** [observed]:
- `locationd` **is on the phone's root**: only the job was missing. It runs for minutes with no crash, talks to wifid (`WifiMgr`, en0 powered, `no association`), and keeps its files in `/var/root/Library/Caches/locationd`.
- Its master switch ("BigSwitch") is `LocationServicesEnabledIn8.0` (found as a string in the binary). In `com.apple.locationd.plist` under both `/var/root/Library/Preferences` and `/var/mobile/Library/Preferences`, from `var.skel` before it starts, it reads 1, and "location disabled system-wide" is gone. Which of the two files does it was not separated.
- A client then gets `kCLErrorDomain 1` (not authorized): the next step is an app's own prompt, which is the owner's morning test with a network joined: Maps, Allow, the position. **Whether Apple's service answers an unactivated device with no Secure Enclave was unknown then; the owner has since seen a correct position (below, run 083507).**
- Open: **`+[CLLocationManager locationServicesEnabled]`, `authorizationStatusForBundleIdentifier:`, `setLocationServicesEnabled:` and `setAuthorizationStatusByType:` do not come back in 15 s** from a program in `/hackios/bin` (with the switch off: not retried with it on), while the asynchronous registration works. **Settings > Privacy & Security has no "Location Services" row** although `Privacy.plist` lists it and MobileGestalt says `location-services` true, `gps-capable` none (seen with the switch off). The row is missing with the switch on too (run 015651). Maps opens with the switch on and showed no prompt in 12 s (a blank page in the screenshot, which does not show a detached layer). Its cost in CPU and memory has not been measured. It logs "Device is not activated" for fitness, and `IoHidDevice is not ready`: no motion, as expected.

**Maps' white screen with `locationd` on: the daemon sits behind FairPlay** (runs 051919, 053616, 055706; default GL, no Metal):
- [observed] Maps' main thread waits in a synchronous NSXPC call into CoreLocation, from `-[MKCoreLocationProvider _updateAuthorizationStatus]`. In `locationd`, the main thread and another wait in `dispatch_sync`, and the queue's worker sits in a MIG `mach_msg` with no timeout, reached from `-[CLMescalSigner initInSilo:]`'s request for the hardware info SAPInit takes. `fairplayd.H2` meanwhile starts "because ipc (mach)", exits 0 and is throttled 10 s, over and over: the shape K11 found under the App Store.
- [observed] **One variable: the root without `com.apple.fairplayd.H2`'s job (run 055706, `locationd` and Bluetooth still on): Maps draws its "What's New" sheet at 20 s and 28 s, and none of `locationd`'s threads is left in a Mach call from its own code.** launchd logs `failed lookup: com.apple.fairplayd.versioned`, then `com.apple.fairplayd`, for `locationd`, so that is the service it asks for. The sheet is as far as this run went: nothing was tapped, and no network was joined.
- [observed] The same lookups fail, and so return at once, for `itunesstored` (8 of each), `WeatherWidget` (3), `amsaccountsd`, `amsengagementd`, `gamed`, `itunescloudd`, `passd`, `NewsTag`, `NewsToday2`. What each then does was not looked at [unverified]. Whether this is why Weather had no data is a guess.
- **Measured, first try (run 093102, phone left still, `location_test watch 90`, distances only):** Wi-Fi joined by itself from `fw/network` (the first boot of that path: it worked; the network's name is not logged here). **3 positions in the first seconds, each accuracy 39 m, altitude accuracy 30, 21 and 18 m, 0 m apart, private `type` 4.** Then the bundle's authorization fell from 3 to 4 ("when in use"), the program is not in front, and it got `kCLErrorDomain 1`: no more positions in the 90 s. So **this does not show the jumping the owner saw**: too few and too short. A 39 m radius and `type` 4 fit Wi-Fi positioning rather than IP [inferred; `type` is private, and its meaning is from memory of old headers], and neither is proof. The next run keeps Maps in front.
- **Measured with Maps in front (run 094755, phone left still, 120 s asked; distances only):** Wi-Fi joined by itself. 25 positions: **accuracy 35 to 92 m, alternating mainly between 35 and 92; furthest from the first 20 m; biggest step between two positions 35 m; every one `type` 4.** The sample ended early: about 84 s in, the system asked Maps and eight other processes to end to free space (`0xbaddd15c`, `CacheDeleteAppContainerCaches`), most likely because this root's 512 MB `/var` filled [inferred; the log has `No space left on device`, and the moment it filled was not seen], so not a Maps crash; the program was then refused. Give `/var` more room (`hk_data_mb`) to repeat this. So on a still phone the position moves by less than its own radius. **That does not explain a jump larger than this, if the owner saw one:** moving phone, Maps' own display, or a longer time are not covered. It also can't tell how far all of them are from the truth, since no coordinate is compared with anything. Radii this small fit Wi-Fi positioning and not IP [inferred]; `type` 4 points the same way [private, meaning from memory].
- **The limit:** Mescal is FairPlay's request signing. A call into it may fail fast, with the feature unavailable; signatures are not to be forged, fairplayd's crypto not emulated, FairPlay not hooked around (supervisor, 2026-11-06).
- **Interim, a choice the owner will see:** `tools/nf/mk_extra.sh loc` now leaves `com.apple.fairplayd.H2`'s job off RAM test roots that have `locationd`. It is a daemon that exits every 10 s here without answering; with no job the lookup fails at once. Nothing is bypassed, and whatever needs FairPlay stays unavailable, as it was. Nothing changes on the installed system. Seen on GL (run 055706) and on Metal (run 060829, the Metal session's: first commit 2060 ms after Maps began, Continue reaches the notifications page, then the location prompt comes up; no 0x8BADF00D in about 90 s, but Maps was not terminated in that run, so a failed graceful exit is not ruled out).
- **Wi-Fi location without Mescal: it works, by the owner's eye, and the earlier expectation here was wrong.** The owner, 2026-11-06 about 08:43, run 083507 (the Metal session's root, `locationd` on, fairplayd's job off, the owner's Wi-Fi joined by hand): 「map可以加载了，定位正常」. This page, K12 and the supervisor had said no fix was to be expected without Mescal. **What the log shows [observed]:** the network joined about 08:43:05 (149 s before the run's end: 22173 frames out, 53357 in); `locationd` asked `wifid` for 113 scans, 14 to 16 a minute from 08:35:44 until the join, 3 to 5 a minute after it; it looked fairplayd up once, at 08:35:43, failed at once, and did not hang. The modem was not started. **What the log cannot show:** `locationd` logs nothing of its positioning without `applog`, so there is no server name, no signed or unsigned, no position and **no accuracy** in it. **Inferred, not measured:** not an old cached position (`/var` is new each RAM boot); most likely Apple's Wi-Fi positioning, from those scans once a network let them out; if so, unsigned, since the signer never started, and the binary does carry a "signing disabled" path. An IP-based position is not excluded. How good it is rests on the owner's eye alone. `location_test`, which prints accuracy and age and never a position, would measure it, with the owner joining Wi-Fi once.

**Boot C ran (`modem=1`, read only, run 015309, 17 s, no CrashDump): the AP can reset the modem** [observed]:
- The modem's PMIC answers at slave 8: REVID `01 01 51 2a` (revision bytes, type, subtype). Slave 9 has no channel.
- Slave 8's PON (`0x808`) has **channel 105 written by system 0, which is us**, beside channel 9 (system 1) and 301 (system 4). REVID's `0x801` is channel 300, system 4: readable, not ours to write. So `qpnp_pon_modem_pwr_off()`'s writes can be made: `0x863 <- 0`, 500 us, `0x862 <- 0x01` (warm reset), `0x863 <- 0x80`, 500 us, `0x864 <- 0xa5` (OnePlus's `qpnp-power-on.c`) [source].
- `pcie_2_gdsc` `0x22f001`: off. Pins at their reset state, reading 0: gpio1, 3, 56, 85, 87 pull-down, gpio57 no pull, gpio86 pull-up. **Whether the SDX55 itself has power is not known** until a reset: MDM2AP status (gpio3) low says it is not up, nothing more.

**`modem=2` ran (the owner: 「modem 继续」; kernel 0e5d50d3, run 035821, 20 s, no CrashDump, no firmware, no bus master): the SDX55 answers, first try** [observed]:
- The warm reset through channel 105: all three written, the first two read back. AP2MDM status high 150 ms later.
- PCIe 2 from the tree and `gcc-sm8250.c`, as PCIe 0's driver: PHY ready; **`THE LINK IS UP after 15 ms: 5 GT/s, x2`**; **device `17cb:0306`**, 64-bit BAR 0 of 4 KB at `0x64300000`.
- MHI: `MHIREGLEN` 0x100, `MHICFG` 0x800080, BHI at 0x100; **BHI version 2, execution environment 0 (PBL), status 0, error 0**. The link stays up for the 3 s watched. MDM2AP status stays 0, as it should before firmware.
- `MHIVER` reads `0xdeadbeef` in the PBL: not a fault [inferred; the PBL has no MHI proper yet].
- So section 15's first question is answered: the chip has power at boot, and the AP can reset it. `PIN`s: all SOUTH tile as `pinctrl-sm8250.c` says.

**`modem=3`: THE SDX55 RUNS ITS IMAGE (kernel 19c17424, private tree, run 071723; the owner: modem at night, within the supervisor's bounds)** [observed]: EE 2 (AMSS), MDM2AP status 1, MHI state M0 for all 60 s watched, errfatal 0, no SMMU refusal, no CrashDump, thermal nominal (once-a-minute reading 51.5 C, slowed 0 times; core peaks were not recorded), PERST low and the stream given back at the end. **That was not a reset: see below.** 87 MB in 15 images by Sahara, all read at run time from `modem_a`'s FAT and the three EFS partitions, read only. In its mission it sends 36 bytes on channel 21 (`IPCR`), which nothing takes yet. **Nothing was asked of it: no registration, no SIM, no RF asked for; no QMI service has been spoken to, so this says nothing yet of GNSS.** Getting there took four things the plan did not have:
1. **MHI state 0xff is SYS_ERR, and it reads so after the warm reset** (`MHISTATUS 0xff04`, already in run 035821, misread then). With no soft reset BHI's doorbell does nothing (`BHI_STATUS 0`, run 060451). `MHICTRL.RESET` clears it, as Linux's power-up does, and `mhi_host_power_up()` did; my boot of its own had dropped that.
2. **BHI needs the device's MSI enabled.** One variable: without it the PBL reads the whole image, checks it, and answers `0xef0e0003` about 9 ms after the last page (runs 060805, 063433); with it the image is taken (run 064537). The address need not be an interrupt controller's: here it is a page of the DMA pool, which the device writes (`0x1220`), and the poll thread looks at the rings. Wi-Fi's chip has its MSI on, so this never showed. The code's meaning is not public [inferred from those runs].
   How that was found, for the next such error: `modem_gap=OFFSET` leaves one page of the image with no translation, and the SMMU names the address the device reached. Three runs placed the error: the DMA path works, the hash segment is read at 1 ms, the first loaded segment at 51 ms, the last page at 57 ms, the error at 66 ms. A DMA fault gives other codes (`0xef0a013b`, `0xef0a0105`).
3. **Sahara's command mode.** After `mdmddr.mbn` (40 bytes, no training data) the SBL trains its DDR and says hello in mode 3, then `CMD_READY` (0xb). Android keeps the numbers; here nothing is written, so the host sends `SWITCH_MODE` (0xc) to image transfer, skipping the data, and it goes on (run 065933). Not documented as allowed; observed to work. So **every boot trains the DDR again** [inferred], which costs time, not risk.
4. **A chained transfer longer than the ring.** `qdsp6sw.mbn` asks for more than 128 elements hold (7.9 MB) at once, and a chain's only event is at its end, so the ring filled and waited for ever (run 071137). Each piece now asks for an event (`IEOB`).

**What stops it: PERST low does not. Correction, 2026-11-06 about 07:40** [observed unless tagged]:
- Earlier notes and reports said the modem was "left in reset" at the end of a run. That was not shown, and **after run 071723 it was not done**: the run ended with PERST low alone, with the firmware possibly still running.
- Run 073746: 60 s in the mission, then PERST low, the PHY down: **MDM2AP status still 1**. Then a warm reset through the PON: **status 0 within 150 ms, still 0 a second later**.
- Run 074005 (`modem=1`, reads only, no reset, about a minute after that): status 0, errfatal 0, AP2MDM status 0. It stayed down.
- **Android has nothing stronger:** `sdx55m_power_down()` is the same warm reset, "cold reset is not supported" (`drivers/esoc/esoc-mdm-pon.c`) [source]. After it the chip waits in its primary boot loader for a host [inferred from runs that find it there].
- **What ran unattended:** after run 071723 ended (07:18:30) nothing stopped the firmware until the next modem run. At 07:33:16, before that run's own reset, status already read 0. **When it fell in those 14 m 46 s, and why, is not known; it may have run for all of it.** Nothing had asked it to use the radio, but that is not proof the radio stayed off. That next run's secondary boot loader then offered a crash's memory (Sahara mode 2, run 073315, declined), so it may have crashed once its link went [hypothesis]; run 073746 went straight on to the mission after that.
- **The heat figures do not cover this chip.** The kernel's 27 sensors are the AP's; the tree's `mdmss-*` zones have no sensor here. The figures quoted were once-a-minute readings of the AP, not peaks and not the modem. So there is no thermal mitigation for the SDX55 at all.
- **Now, in the private tree:** `hk_modem_quiesce()` does the warm reset as well, at the end of every run that returns cleanly and on a panic's way out. Seen on the run-end path, no firmware loaded: run 074159 (`modem=2`) left AP2MDM status high and returned, and run 074258 reads gpio56 low, which only the end of a completed `modem_stop()` writes (earlier boots read it high, retained). Its log line does not reach the log from there. The panic path has not been run, and the path has not been run with a firmware up. **A kernel that hangs never gets there and leaves the firmware running**, until something resets the phone. `modem=3` stops before image 8 unless `modem_mission=1` is on the line, so nothing of its firmware runs. No mission run until the supervisor says.
- **If a kernel hangs with its firmware up** (only possible with `modem_mission=1`, and those runs are stopped). **How it would show:** the screen frozen or black; in the log, the heartbeat lines stop and no `kernel returned:` comes; `xl` or `bootq` runs to its limit; the phone may stay warm or get warmer with nothing on it. There is no line to say the modem is still up, because nothing is left to print it. **What an owner should do:** treat it as still running. Hold Power+VolUp to reset the phone (AGENTS.md rule 8); if it does not come back or stays warm, keep it off the charger and leave it to cool. Then run `modem=1` once, which only reads: `MDM2AP status 1` means the firmware is still running, and `modem=2` then does the warm reset. **Not observed:** whether a phone reset itself takes the SDX55 down; it has its own PMIC, so do not assume so until that one read says status 0.
- **The run's end stops running firmware, shown (owner back, 2026-11-06):** run 093504 was 20 s in its mission, MDM2AP status 1, when the run ended and the kernel returned; 46 s later run 093614 (`modem=1`, reads only) read status 0, errfatal 0 and AP2MDM low, which only the end of a completed `modem_stop()` writes. With run 073746's status falling within 150 ms of the same reset, that is the evidence. It does not show the radio's state, and a hung kernel still never gets here.
- **Every firmware load after a mission meets Sahara mode 2:** runs 073315 and 083054, each the next load after a mission, were offered a crash's memory (declined, which ends that run in about 6 s), and the load after that went normally. A warm reset alone leaves whatever says so [inferred]. So after a mission, one boot is lost unless this is handled.
- **QRTR on IPCR works (run 100040, kernel cc936ae8, the owner there; no service spoken to, no registration):** its header is QRTR's second form, whose version byte is 3 (Linux's `QRTR_PROTO_VER_2`), 16 bytes. HELLO from node 2, ours answered, **47 services offered in the 60 s, none looked for on our side** (`QMI LOC` is **service 16, version 2, instance 0, at node 3 port 84**; it appears about 35 s into the mission, among the last). Node 2 is the SDX55's own processor, node 3 its modem DSP [inferred]. Node 2 offers 14 (the remote storage service's number), 15, 20, 43 and others itself. It never asked for rmtfs in 60 s, though Android's has one [observed strings]; so the plan's "rmtfs with writes in RAM" may not be needed to reach LOC [inferred, open]. **Heat:** minute reading 52.3 C, peak 86.4 C on cpu-1-6, no throttle. The AP's sensors say nothing direct about the SDX55.

**The Sahara ids, found one boot at a time; each image is answered with status 0 by the device (runs 064537 to 071723):** in the order it asks: 37 `multi_image.mbn`, 34 `mdmddr.mbn`, (DDR training, command mode), 40 `apdp.mbn`, 36 `multi_image_qti.mbn`, 41 `devcfg.mbn`, 25 `tz.mbn`, 33 `hyp.mbn`, 42 `sec.elf`, 23 `aop.mbn`, 16/17/20 partitions `mdm1m9kefs1/2/3`, 29 `acdb.mbn`, 8 `qdsp6sw.mbn`, 6 `apps.mbn`. `xbl_cfg.elf` is not asked for (the table's 38 for it is from qaic's, never used here). The other old entries for it (2, 12, 32) were not used either.

**Next for GNSS [plan]:** QRTR on channels 20/21, the host's name service, rmtfs with writes in RAM, then QMI LOC. Not started. **Not in `xnu-src`:** the code is `tools/nf/modem/` and the private tree `xnu-src/nf-tree/`; the tree is the Metal session's to build, so it lands when that session is ready for it.

**Sensors, step 1: the audio DSP moved off the SLPI's memory (private kernel 41c061b7, run 081133, quiet, no modem)** [observed]: `adsp_memory` is now `0x91b00000` + `0x2500000`, the tree's `pil_adsp_mem` `reg`, directly after the SLPI's `0x90600000` + `0x1500000`; the node's name says `@8a100000` and the old code's `0x8fe00000` lay over all of the SLPI's. TrustZone takes it: `THE AUDIO DSP IS UP AND LINKED`, 4462 ms, 8 services; AFE, ADM and ASM set up; iOS's sound went through (146432 frames from iOS, 146432 to the DSP, 2099 periods given back, 0 errors, 0 underflows). **Not heard:** the amplifiers were off, so nobody has listened to it. **Heat:** minute readings 60.1 C (first minute, boot) and 43.8 C, slowed 0 times; peak 81.4 C on cpu-1-6, in the first minute (a peak line from the new line in `hk_thermal.c`, private tree). A `backboardd` SIGSEGV in that log is after the run's end began (`t-mark: done`, then GPU submits fail with -5): the same Mesa errors are in older runs that ended this way, not this change [inferred]. **Not in `xnu-src`:** other kernels still use `0x8fe00000`.

**Sensors, step 2 waits for the owner:** the SLPI's code says a cold reset is needed after any attempt, before the next DSP attempt (`hk_slpi_boot.c`, "COLD REBOOT before next DSP attempt"; HANDOFF has a kernel after an attempt failing with that note, most likely cause, unproven). **A reset with the USB cable in makes the stock ABL power off, and the owner has to power on and choose Developer mode** (`boot-chain-and-recovery.md`). So one SLPI attempt can take the phone away from every session until the owner is there: not to be tried unattended. Also open before it runs beside the audio DSP: its `adsp` refusal is still in, and whether the two GLINK drivers can share the IPCC block has not been reviewed.

**QMI LOC answers (run 102633, kernel f9f6728b, the owner there; two questions only, nothing started, no registration):** service 16 appeared 28 s into the mission; both questions were answered at once, each with a response and an indication [observed]. Get Operation Mode `0x4b`: result 0, status 0, **mode 4, which libqmi names standalone** [its enum]. Nothing here set it. Get Engine Lock `0x3b`: result 0, status 0, then **a field `0x12` of 8 bytes where libqmi defines Lock Type as `0x10`**: only its size was logged, so **the lock's state is not known**. Qualcomm's public `location_service_v02.h` (a newer copy, Sony's `vendor-qcom-opensource-location`) defines `0x12` as `lockClient`, a deprecated 64-bit mask of locked clients (AFW, NFW, privileged, AFW proxy), and `0x10` as Lock Type, `0x14` as engine state [source; whether this firmware means the same is not shown]. An unknown 8 bytes could be an IMSI or IMEI, so it is printed only if both 32-bit halves are 255 or less, and otherwise as "not small" with nothing more. No position, no identifier, no engine start. At the end MDM2AP status 1, then 0 within 150 ms of the warm reset, as before. **Heat:** minute reading 49.6 C, peak 82.2 C on cpu-1-6, no throttle; the AP's sensors say nothing direct about the SDX55. The load before it (run 102613) met Sahara mode 2 as expected, the third time seen: too short for a minute reading or a peak.

**The owner's one GNSS try by a window (run 110304, kernel 117fa170, behind glass unless the owner says otherwise): the engine refused the session, so it says nothing of signal, glass or the receiver** [observed]:
- After the cold restart the load went normally: `MHISTATUS 0`, no Sahara mode 2, DDR training, 87 MB, the mission, LOC after about 30 s. MDM2AP status 1 throughout, errfatal 0.
- Get Engine Lock: status 0, **field `0x12` = 2, 0** (both small, so printed). Get Operation Mode: 4. Inject UTC Time (added by me, for a better chance; no network needed): result 0, status 0. Register Events: result 0.
- **Start: result 1 (failed), error `0x2e`, libqmi's General Error**, and a second such line with error 3, Internal, under one heading; one Start was sent, and why two is not known.
- **One position report: status 7**, which Qualcomm's header names `ENGINE_LOCKED` (libqmi's table stops at 6), no uncertainty, 0 satellites used. Then 149 s of nothing: **0 satellite reports, no engine state event, no first signal, no fix.** Stop: result 0.
- **Why, read from Qualcomm's public `location_service_v02.h` [source; that this firmware follows it is inferred]:** `0x12` is `lockClient`, and 2 is `QMI_LOC_LOCK_CLIENT_MASK_NFW`, non-framework clients locked. Register Events' Client Type defaults to NFW when not given, and it was not given. So this client was the kind that is locked.
- **So neither "no signal" nor "signal but no fix": the receiver was never asked to run.** A further try needs the owner's word. It would add Client Type AFW (1) to Register Events, the Android framework's kind, which is what this client stands in for. Not Set Engine Lock, and not the privileged type, whose header warns of privacy requirements. It might still fail for another reason.
- The firmware was stopped as before: MDM2AP status 1, then 0 within 150 ms of the warm reset, 0 a second later; no CrashDump. **Heat:** minute readings 49.6, 47.3, 46.9 C, no throttle; peak 81.4 C on cpu-1-6 in the first minute, and no later peak line appears; the AP's sensors say nothing direct about the SDX55. `torch=0` was left off the line, which was 5 bytes too long; the light was not asked for.

**The owner's second GNSS try, by the same window (run 111823, kernel 6904ed7b; the owner: 「可以，再试一次」): the session was accepted, and the modem crashed about 69 s later** [observed]:
- The one change: Register Events' Client Type (field `0x11`, 4 bytes, libqmi and Qualcomm's table agree) = 1, AFW, as Qualcomm's header defines it. **Start: result 0.** So the first try's refusal was the client type, as read [now observed].
- Engine State indication: value 2, which libqmi names off; only one came, and none saying on. 68 position reports, all status 1 (in progress), 0 satellites used, uncertainty 9999000.0 m, which looks like "no estimate" rather than a measurement [inferred]. **0 satellite reports: no list, no signal, no fix, in the 69 s it ran.**
- **Then `MDM2AP ERRFATAL is high`.** The mission loop ended there, Stop was sent with no answer seen, and the warm reset followed: status 1, then 0 within 150 ms, errfatal 0 a second later. No CrashDump on the AP. The first try's mission, with no session, ran its whole 180 s with no such thing, so **the session is the likely trigger [inferred, from two runs]; why is not known.** No crash text was read: its memory was not taken, and the host gives it no buffer for a failure reason yet. Heat cannot be ruled out, since nothing reads the chip's temperature; the AP read low.
- **So this is neither "no signal behind glass" nor "signal but no fix": the engine never reported on, and 69 s of a crashing modem says nothing about the window.**
- **Heat:** minute reading 49.6 C, peak 84.1 C on cpu-1-6, no throttle; the AP's sensors say nothing direct about the SDX55.
- The load before it (run 111759) met Sahara mode 2, as expected. A crash may leave the next load the same way.

**The owner's third GNSS try, for the crash's reason (run 113829, kernel 4902f043; the owner: 「先查 modem 崩潰」): the modem says why** [observed]. The failure reason's buffer was given on the way into the mission, as Android's `mhi_init_sfr()` does (command type 73, 256 bytes), and the text, cleaned by the rule (printable ASCII, at most 160 bytes, long numbers and anything like a coordinate replaced), is a source file and a line:

    ssr:AP:remotefs_sahara.c:850:Assertion remotefs_sahara_info.sahara_status == SAHARA_END_OF_IMG_TRANSFER fail:

- The same again otherwise: Start accepted, Engine State 2, 68 reports in progress, uncertainty 9999000.0 m, 0 satellite reports, errfatal 71 s after the start. No crash memory was read; the raw text was cleared. No CrashDump on the AP.
- **What it most likely means [inferred]:** the modem's side keeps its file system through the host, over a Sahara exchange that this host does not serve once the mission begins. Node 2 itself offers service 14, and nothing here answers what it asks of the host. The mission with no session ran 180 s without this (run 110304), so the session is what makes it want to save something, and with nobody there its assertion fails. OnePlus's own MHI driver singles out this file: `mhi_control_error()` panics the phone on a reason naming `remotefs_sahara.c` (`drivers/bus/mhi/core/mhi_pm.c`, LineageOS's copy) [source]. The device tree lists an `EFS` channel pair, 10 and 11, in the mission; this host does not start them. **Which channel it wants, and what it sends, is not yet known.**
- **So this is not heat, not glass and not a fault in the receiver, as far as the text goes.** The engine has still never reported on, so nothing is known of signal at that window.
- **What it would take:** serving that exchange during the mission. What the modem sends would be its own file system, which can hold identifiers and calibration: **it must not reach the log or the phone's storage**, and whether to hold it in RAM or refuse it cleanly is for the owner to decide. To be read first: Qualcomm's public kickstart tool and how Android's `mdm_helper` runs it.
- **Heat:** minute reading 55.8 C, peak 89.6 C on cpu-1-6, no throttle (at boot the prime core read 57.0 C, warmer than earlier runs); the AP's sensors say nothing direct about the SDX55.

### Plan, not run: serving the modem's file sync so GNSS can go on (2026-11-06; for the owner to decide, no boot was used)

Sources: [S1] Replicant's `qmi-ril` `sahara.c` (`efs_sync()`, a free implementation for an MDM9615 over a serial port, so another chip and another link); [S2] the libqmi-devel thread of February 2017 on it (`efsks`, "EFS kickstarter"; its author says not to take it all as truth); [S3] LineageOS's OnePlus `drivers/bus/mhi/core/mhi_pm.c`; [S4] Linux's Sahara documentation (2026 patches) and `qaic/sahara.c`. Android's own `mdm_helper` and `ks` are closed: nothing public shows what they do on this phone. [O] observed on this phone, [I] inferred.

1. **What it asks for, on which channel.** The exchange [S1], in the mission, device first: HELLO in mode 2 (memory debug); host HELLO response; device MEMORY_DEBUG (`0x9`), address and size of a table; host MEMORY_READ (`0xa`) of the table; device sends a raw table naming a file, its address and size; host MEMORY_READ of that; device sends the raw data; host RESET (`0x7`); device RESET response (`0x8`). So **the host pulls, and asks for each byte.** S1 caps it at its own buffer; S2 says whole partitions are rewritten this way: here each is 2 MB [O], so up to about that each way out of the modem [I]. **Channel: not known.** The tree's `EFS` pair, 10 out and 11 in, exists only in the mission, and 11 may wake the host [O, tree], which fits [I]; this host has never started them [O]. Order and timing on this chip: not seen; about 70 s after Start (two runs) [O], which may be a timer [I].
2. **A clean refusal, which keeps nothing: possible to try, and not known to work.** Sahara has no "no file system" answer that any source shows [S1, S4]. What exists: answer HELLO, take the MEMORY_DEBUG, and send RESET **without one MEMORY_READ**. Whether that ends with the status the assertion wants (`SAHARA_END_OF_IMG_TRANSFER`) is unknown: the name reads like "the transfer ended", not "the data was read" [I]. Doing nothing is already known to fail [O: `remotefs_sahara.c:850`, runs 111823 (time) and 113829 (text)].
3. **If it needs data taken.** The host only reads, so there is nothing to invent or fill with blanks: **read and throw away is the smallest thing** [I]. S1 does exactly that ("TODO: implement writing data"), and reports a finished sync on its chip [S1, not shown here]. Does it need the same back next boot? S2: two of three partitions take turns and one is a golden copy, and this host always sends the phone's unchanged partitions [O], so the modem starts each boot from Android's last saved state, as it would after a crash before a sync [I]. What it saved in the run is lost; that may make it redo first-run work each boot [I].
4. **What would be in RAM.** Option A (refusal): nothing of the modem's beyond the commands, and the table's address and size, which are logged as sizes only. Option B (read and discard): the table and the file pass through the MHI channel's receive buffers (at most 64 of them, about 64 KB each, in the DMA pool) and are overwritten in the buffer when received: not copied, assembled or hashed. The pool itself goes when the run ends, and is cleared on the way out. **Nothing is passed to `say()`** but command ids, lengths and counts: the code path never formats a payload byte, which can be checked by reading a few lines. No file: the kernel code has no file open for writing anywhere in `tools/nf/modem/`, and EFS partitions are opened read only [O]. Content is the modem's file system, so identifiers and calibration are likely in it [I]. A table naming anything but the expected files ends the exchange unread: what the expected names are comes from a source I could not fetch (S1's header), so it needs reading first.
5. **What it was given so far [O].** SBL asks for images 16, 17 and 20; each mission got `mdm1m9kefs1` and `mdm1m9kefs2` read in full (2 MB each) and `mdm1m9kefs3` with no bytes asked for, unchanged, from the phone's partitions, read only. `mdm1m9kefsc` and `mdm1oemnvbktmp` exist and were not asked for. In 47 services node 2 offers service 14 itself and nothing looks for an rmtfs on our side [O]. So Android's contents were loaded, and `rmtfs` over QRTR looks unneeded on this chip; the older plan's "rmtfs with writes in RAM" is probably not what this asks for [I].
6. **Risks.**
   - Modem: a wrong answer may crash it, which it does anyway; RESET might reset the chip and not just the exchange [I]. The host already watches errfatal and ends the run with the warm reset, and it does not panic the phone as Android's does [S3, O].
   - Phone: reads through a link the modem takes down are guarded [O], with the known race (`mdm_link_up()`). A second mission's worth of heat, with no sensor on the chip.
   - Across boots: nothing is written to the phone's storage by design; the modem's DDR holds whatever until the warm reset, and the next load meets Sahara mode 2 as it always does after a mission [O]. **Mode 2 is not read, still** (the crash memory rule). Note that mode 2 is the same Sahara mode as this exchange, at another stage, so the code must tell them apart: during SBL it stays refused.
   - What is left in the DMA pool: option B's data is in kernel memory until the run ends, which a panic's log does not include [I].
7. **What is not known, and the one boot that settles most of it.** Which channel; whether HELLO comes at all; whether RESET without a read satisfies it; whether the engine then runs; and what signal the window gets. **One mission, as the third try, plus channels 10 and 11 started and the option A answers, logging only channel, command ids and lengths.** If HELLO never comes on 10/11, the channel guess was wrong: that is the answer, and nothing was read. If the assertion still fires after a RESET, option A is dead and option B is the owner's next decision. If it does not fire, the 180 s also show for the first time whether the engine turns on and sees satellites. No build or queue until the owner has decided.

**Read, no phone (2026-11-06, boots stopped for heat):**

*SLPI beside the audio DSP and Wi-Fi: IPCC is not the obstacle [code].* All three drivers read the same `IPCC_RECEIVED` at `0x408000`. `hk_adsp.c` and `hk_slpi_boot.c` leave another client's ring in place and stop looking, since IPCC names the same one until it is cleared. **`hk_wifi.c` clears every ring it reads, whoever rang.** That would lose a doorbell for the others if anything waited on one, but all three poll their rings and SMP2P words and use doorbells only for counts, so it does not stop a link. It is why an interrupt-driven link could not simply be added beside them. The AOP mailbox and SMEM allocation are already under shared locks. What does stand in the way, in order:
1. `hk_slpi_boot_init()` returns if `adsp` or `adsp_step` is set. In the private tree's source, not yet built or run: `slpi_beside_adsp=1` skips that, only for kernels with the audio DSP at `0x91b00000`.
2. The root needs `fw/slpi.mdt` and `slpi.b02`-`b21` (`firmware-reader/native-slpi`, not in git): `tools/nf/mk_extra.sh slpi` adds them (not yet run). No test root has them yet.
3. It needs `slpi_boot_once=1 slpi_cold_provenance=1 slpi_hold_proxy=1`, and its LCX/LMX/XO floors stay held to the end of the phone's run, which **costs idle power on a phone that has just tripped the heat rule**.
4. Today it boots, probes and stops the DSP (`slpi_boot_stop_once()`), and its first SMEM check refuses to start if the DSP has run since the phone's last cold boot. **Read 2026-11-06 (run 104333, `slpi_step=1 slpi_smem_ro=1 adsp=0`, reads only): partition `0x17f000`, `0x30000` bytes, 0 of the items it looks for: nothing there says the SLPI has run since the phone's last cold boot, so that check would pass.** The code itself calls this an extra check, not proof the DSP is off. The read refuses to run with `adsp` set, so it says nothing of the two side by side. Too short for a minute reading or a peak (at boot the prime core read 50.4 C).
5. The empty sensor list needs what section 4.4 lists: the service locator for instance 90, FastRPC, the reverse tunnel. None of that exists yet, so the first attempt can only re-prove "READY" beside the audio DSP.

*What QMI LOC needs [source: libqmi's `qmi-service-loc.json`; not run].* Over QRTR, a QMI message is a type byte (0 request, 2 response, 4 indication), a 16-bit transaction, a 16-bit message, a 16-bit length, then fields. Questions that start nothing: Get Engine Lock `0x3b`, Get Operation Mode `0x4b`, Get NMEA Types `0x3f`; each answers twice, a response and then an indication. To get a fix: Register Events `0x21` (a 64-bit mask, field 1), Start `0x22` (session id, field 1), Stop `0x23`, indications Position Report `0x24`, Sv Info `0x25`, Engine State `0x2b`. **`modem_loc=1` asks only the first two (run 102633). Starting a session is a further step, for the owner to allow.** Limits that matter:
- **60 s will not do for a fix.** LOC appears about 35 s into the mission (run 100040), which leaves 25 s; a cold start with no assistance usually takes longer, and indoors may see no satellites at all [general knowledge, not measured here]. Sv Info's signal numbers would show progress sooner than a fix would.
- **Standalone only:** no SIM and no data, so no assistance over the network; Set Operation Mode `0x4a` would keep it from asking [not run]. Inject UTC Time `0x38` is the one aid that needs no network.
- **Logging:** Position Report's fields `0x10` and `0x11` are the coordinates and are never to be printed. The horizontal uncertainty (`0x12`), satellites' count and signal are enough. `mdm_loc_input()` prints fields only for the two questions above.
- **Reaching iOS is a separate, larger job,** not started: `locationd` has no way to take a position from this path as it stands.

**GPS.** There is no receiver outside the SDX55 [observed tree]. Without the modem, location is Wi-Fi's only. The owner: 「連 modem 一起做」. Minimum for GNSS: M1, QRTR on `IPCR`, rmtfs with writes in RAM, QMI LOC; no SIM, registration, data or CommCenter.

**Modem files** (`modem_a:/image/sdx55m`, names only, `firmware-reader/list_modem_fat.py`): 18 files including `sbl1.mbn`, `xbl_cfg.elf`, `aop.mbn`, `tz.mbn`, `hyp.mbn`, `devcfg.mbn`, `apdp.mbn`, `sec.elf`, `multi_image.mbn`, `multi_image_qti.mbn`, `apps.mbn` (2.6 MB), `qdsp6sw.mbn` (84 MB), `acdb.mbn`, `mdmddr.mbn` (40 bytes), `efs1-3.bin` (512 bytes each), `modemr.jsn`. `sdx55/` holds one `qdsp6m.qdb`, `modem_pr/` the carrier configs. 84 MB is a lot to carry in the small image: reading it from `modem_a` at run time is the better path [inferred].

**Wrong on this page before, corrected above:** `locationd` "not on the root"; gpio68 "access-protected" (it is in the NORTH tile, and `0x0f144000` is the WEST tile's same offset: more likely an absent register) [inferred].

**How a picture is kept off the Mac:** `xl screenshot` copies the screen, and a Camera screen holds the viewfinder. No shot is taken on a Camera screen; what Camera does is read from the log, and a photograph's numbers from `photo_info` on the phone. (`sips --cropOffset 0 0` centres the crop instead of taking the top: two earlier crops were of the viewfinder, deleted.)

---

## 16. Sources

**Repository:** [../subsystems/bluetooth.md](../subsystems/bluetooth.md), [bluetooth-ios.md](bluetooth-ios.md), [../subsystems/input-peripherals.md](../subsystems/input-peripherals.md), [../subsystems/audio.md](../subsystems/audio.md), [ios-userland.md](ios-userland.md) §4, [hardware-qualcomm.md](hardware-qualcomm.md), [../TODO.md](../TODO.md), `outputs/相機完成後待修.txt`, `../oneplus8-audit/als-native/README.md`, `odm-followup.md`, `als-review/lookup-review/REVIEW.txt`, `gpu-plan/slpi-*`.

**OnePlus's kernel, LineageOS branch lineage-22.2** ([android_kernel_oneplus_sm8250](https://github.com/LineageOS/android_kernel_oneplus_sm8250)):
[esoc-mdm-pon.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-22.2/drivers/esoc/esoc-mdm-pon.c),
[esoc-mdm-4x.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-22.2/drivers/esoc/esoc-mdm-4x.c),
[mhi_qcom.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-22.2/drivers/bus/mhi/controllers/mhi_qcom.c),
[leds-qpnp-flash-v2.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-22.2/drivers/leds/leds-qpnp-flash-v2.c),
[voice_mhi.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-22.2/techpack/audio/dsp/voice_mhi.c),
[cam_flash_core.c](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/lineage-22.2/techpack/camera/drivers/cam_sensor_module/cam_flash/cam_flash_core.c).
Device tree: [android_device_oneplus_sm8250-common](https://github.com/LineageOS/android_device_oneplus_sm8250-common).

**Linux mainline:**
[pci_generic.c](https://github.com/torvalds/linux/blob/master/drivers/bus/mhi/host/pci_generic.c),
[qcom_pd_mapper.c](https://github.com/torvalds/linux/blob/master/drivers/soc/qcom/qcom_pd_mapper.c),
[leds-qcom-flash.c](https://github.com/torvalds/linux/blob/master/drivers/leds/flash/leds-qcom-flash.c),
[sm8250.dtsi](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi),
[hci_qca.c](https://github.com/torvalds/linux/blob/master/drivers/bluetooth/hci_qca.c). Pointers not read in this pass: `drivers/misc/fastrpc.c`, `drivers/remoteproc/qcom_q6v5_pas.c`, `net/qrtr/mhi.c`, `sound/soc/codecs/wcd938x.c`, `sound/soc/codecs/lpass-rx-macro.c`, `drivers/usb/typec/mux/fsa4480.c`.

**Modem:**
[Linaro: Upstreaming the Qualcomm modems, SDX55 case study](https://static.linaro.org/connect/lvc21f/presentations/LVC21F-222.pdf),
[Linaro blog](https://old.linaro.org/blog/upstreaming-support-for-qualcomm-pcie-modems/),
[ModemManager on RM500Q over PCIe](https://lists.freedesktop.org/archives/modemmanager-devel/2021-November/008917.html),
[Sahara protocol series, LWN](https://lwn.net/Articles/1063691/),
[linux-msm/rmtfs](https://github.com/linux-msm/rmtfs), [tqftpserv](https://github.com/linux-msm/tqftpserv), [pd-mapper](https://github.com/linux-msm/pd-mapper), [qrtr](https://github.com/linux-msm/qrtr), [qdl](https://github.com/linux-msm/qdl),
[libqmi NEWS (LOC)](https://chromium.googlesource.com/chromiumos/third_party/libqmi/+/refs/tags/1.28.0/NEWS),
[ModemManager mm-shared-qmi.c](https://chromium.googlesource.com/chromiumos/third_party/modemmanager-next/+/e88e481ca3476acb5ecadae361d0eed9d10e6194/src/mm-shared-qmi.c),
[q6voiced, postmarketOS MR](https://gitlab.com/postmarketOS/pmaports/-/merge_requests/1233),
[postmarketOS: OnePlus 8](https://wiki.postmarketos.org/wiki/OnePlus_8_(oneplus-instantnoodle)) (snippet only).

**iOS baseband research (summaries only):**
[ARIstoteles](https://arxiv.org/abs/2604.23457),
[37C3: What your phone won't tell you](https://media.ccc.de/v/37c3-11868-what_your_phone_won_t_tell_you),
[Trace the Base](https://citation.thinkst.com/talk/94562),
[Troopers 2025, iPhone baseband firmware](https://troopers.de/troopers25/talks/8p3ft8/).

**Sensors:**
[hexagonrpc](https://github.com/linux-msm/hexagonrpc),
[libssc package](https://archlinuxarm.org/packages/aarch64/libssc),
[pmaports: SSC on SDM845](https://gitlab.com/postmarketOS/pmaports/-/merge_requests/4050),
[iio-sensor-proxy: libssc](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy/-/merge_requests/381),
[Qualcomm sensinghub](https://github.com/qualcomm/sensinghub/tree/8f23fc7fae0a93f0b938c5ed08ef3f4ba952f765/apis/proto),
[qualcomm/fastrpc](https://github.com/qualcomm/fastrpc),
Apple's [AppleHIDUsageTables.h, 606.1.7](https://opensource.apple.com/source/IOHIDFamily/IOHIDFamily-606.1.7/IOHIDFamily/AppleHIDUsageTables.h.auto.html) and [1035.60.3](https://opensource.apple.com/source/IOHIDFamily/IOHIDFamily-1035.60.3/IOHIDFamily/AppleHIDUsageTables.h.auto.html).

**Pins and clocks for Boot C:** [pinctrl-sm8250.c](https://github.com/torvalds/linux/blob/master/drivers/pinctrl/qcom/pinctrl-sm8250.c), [gcc-sm8250.c](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/gcc-sm8250.c).

**Location:** [Rye and Levin, Surveilling the Masses with Wi-Fi-Based Positioning Systems](https://www.arxiv.org/abs/2405.14975).

**Fingerprint:** [/e/ backlog, OnePlus 8T log](https://gitlab.e.foundation/e/backlog/-/issues/5536).
