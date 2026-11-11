# Boot arguments, sysctls and related knobs

Boot-args are given on the kernel command line: `XL_CMDLINE="..." ./xl run`, the profile's `BOOTARGS`/`FLASH_BOOTARGS`
(`common/devices/*.conf`), `CMDLINE=` for QEMU, or `--cmdline` of `xnuloader/mkpayload.py`. The kernel reads them with
`PE_parse_boot_argn()`; user space reads `sysctl kern.bootargs`. This page is **generated from the source** (202 kernel
names via `PE_parse_boot_argn`/`PE_parse_boot_arg_str`, plus the userland ones) and annotated by hand; names marked
"see the header comment" are diagnostic or tuning knobs whose only documentation is the comment at the top of the named file.
**The OnePlus 8's own defaults (2026-11-05, pexpert/arm/pe_bootargs.c, kernels from cam-5440d572):** `wifi=1 wifi_bdf_native=1 adsp=1 sound=4 sound_amps=1 haptic=5 cam=5 cam_after=10`, since 2026-11-05 `mdp=8 edr=1` and since 2026-11-06 `torch=3 bt=1` (in `xnu-src`, RAM kernels) are added to the kernel's boot line unless that name is given, so `wifi=0` and the like are off; `kern.bootargs` shows them. board=instantnoodle only.

**When you add a boot-arg, add it here** (see [../AGENTS.md](../AGENTS.md)). Regenerate the name list with:

```sh
grep -rhoE 'PE_parse_boot_arg(n|_str)\( *"[A-Za-z_0-9]+"' xnu/files | sort -u
```

Rules of thumb: a bare `./xl enter` or a bad arg can hang a phone; **never pass `touchscreen=1` on the OnePlus 8**; `wifi_again=1` /
`adsp_again=1` have reset the phone; `sound_amps=1` makes the speakers audible. Per-device defaults live in the profile.

## Parsed by the loader (not the kernel)

| Arg | Meaning |
|---|---|
| `cpus=N`, `cpumask=M` | also read by the loader to know the kernel will start more cores (`cpumask` picks cores by position, bit 0 first) |
| `gamut=p3` | test: leave the iPhone's "P3" display gamut in the device tree (default sRGB, see display.md) |
| `ufs_probe=1` | loader probes Block I/O 2 behaviour before the kernel starts |
| `board=NAME` | the loader adds `board=` for the profile's board; `board_<block>=0x...` overrides come from `BOARD_ARGS` (`bsd/sys/hk_board.h`: `epss`, `cmd_db`, `rsc`, `tsens0/1`, `gcc`, `dispcc`, `intf`, `ufshc`, `spmi_*`, `thermal_gpuss_0/1`, `cpu_domains`, `small_cores`) |

## Kernel

### `bsd/dev/hk_adreno.c`

| Boot-arg | Meaning |
|---|---|
| `adreno` | Run bring-up steps 1..N and stop. |
| `adreno_blind` | Step 20 with microcode nowhere (diagnostic). |
| `adreno_dirty` | Leave the GPU as a panic leaves it. |
| `adreno_fine` | Wait before every single access. |
| `board_gpu_zap` | GPU zap shader reserved physical base; 4 KiB aligned. Historical defaults: kona 0x8c51a000, redwood 0x8b71a000. Must match live FDT reg and lie outside allocatable RAM; see subsystems/gpu.md. |
| `adreno_keep` | With adreno=21: keep the GPU for the system (HKGPU). |
| `adreno_only` | Run step K alone. |
| `adreno_pause` | ms the newest step waits after announcing (default 2200). |
| `adreno_return` | Return to the loader when steps are over. |
| `adreno_skip` | Bitmask of steps to skip. |
| `adreno_speed` | Speed level asked of the GMU in step 17. |
| `adreno_stream1` | Second stream handling in step 18 (diagnostic). |
| `adreno_watch` | Seconds of "still alive" lines after the last step. |
| `display_probe` | Read what the firmware left the display controller doing. |
| `gpu` | 1: start the Adreno for the window server (needs fw-gpu and a GL=1 image). |
| `gpu_cycle` | Boot test: N GPU stop/start cycles before anything uses it. |
| `gpu_dcvs` | GPU speed follows load (default on kona; 0 = 525 MHz). |
| `gpu_exact` | *diagnostic/tuning knob: see the header comment of this file* |
| `gpu_top` | 587: the GPU's fourth speed, 587 MHz, above 305, 400 and 525 (the owner's word, 2026-11-06). OnePlus 8, speed bin 0 only; anything else is refused and says so. Off by default, not in a row. Nothing slows the GPU itself with `gpu_dcvs=0`. Run 115324. |
| `gpu_hot` | DCVS holds 305 MHz from this temperature (default 85.0 C). |
| `gpu_hwcg` | 0 turns hardware clock gating off. |
| `gpu_irq` | GPU fences raise an interrupt (GIC 332; default on kona). |
| `gpu_log` | *diagnostic/tuning knob: see the header comment of this file* |
| `gpu_pt` | Per-program GPU page tables (default on kona). |
| `gpu_recover` | Reset and restart a hung GPU (default on kona). |
| `gpu_va` | *diagnostic/tuning knob: see the header comment of this file* |

### `bsd/dev/hk_adsp.c`

| Boot-arg | Meaning |
|---|---|
| `adsp` | 1: start the audio DSP and its services. |
| `adsp_apr_wait` | APR wait. |
| `adsp_listen` | Seconds step 10 listens for QRTR services. |
| `adsp_new` | Steps from K on are unproven; pause after each. |
| `adsp_pause` | ms pause after announcements. |
| `adsp_step` | Run DSP steps 1..N and stop. |
| `adsp_wait` | Seconds step 7 watches for "up". |
| `battmgr` | 0: no DSP battery manager. |
| `battmgr_notify` | DSP battery manager notifications. |
| `battmgr_xm` | DSP battery manager extra mode. |

### `bsd/dev/hk_apfs_check.c`

| Boot-arg | Meaning |
|---|---|
| `apfs_check` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_chunk` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_contents` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_depth` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_dev` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_hash` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_max` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_mnt` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_skip` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_store_len` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_store_note` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_store_seed` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_subtree` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_threads` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_volume` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_volume_at` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_walkers` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_write_at` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_write_byte` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_write_len` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_write_path` | *diagnostic/tuning knob: see the header comment of this file* |

### `bsd/dev/hk_audio.c`

| Boot-arg | Meaning |
|---|---|
| `rec_sound` | Record the first N seconds of played sound (xl sound). |
| `rec_sound_fed` | Record what the guard passed. |
| `rec_sound_mic` | Record what the main mic hears. |

### `bsd/dev/hk_battery.c`

| Boot-arg | Meaning |
|---|---|
| `battery` | redwood: 1 Qualcomm UEFI charger state shared by the loader; instantnoodle: 0 turns the gauge off. |
| `battery_shared_pa` | Physical address of the loader's charger page. |

### `bsd/dev/hk_bt.c`, `instantnoodle_bt.c`

| Boot-arg | Meaning |
|---|---|
| `bt=1` | QCA6390 Bluetooth on instantnoodle, **on by default there since 2026-11-06 (`bt=0` off)**; other boards remain unchanged. The chip stays off unless the small image has `fw/htnv20.bin`. Native jobs also need `BLUETOOTH=1` when building userland. |
| `bt_test=1` | Before client handoff, compare legacy/extended command selection, read capabilities, scan BLE for four seconds, and run classic inquiry. Diagnostic only. |
| `bt_probe=1` | Log UART/pin/clock state during startup. |
| `bt_trace=1` | Log HCI commands/events; 2 also logs ACL/SCO headers (`}`: SCO as the pacing really sends it). Logs can contain device identifiers; keep local. |
| `bt_sco_hci=0` | Send iOS's legacy Setup/Accept Synchronous Connection unchanged (default: as Enhanced with the HCI data path). |
| `rt_clamp=0` | (`osfmk/kern/thread_policy.c`) Refuse real-time requests with computation over 50 ms, as stock (default: cut to 50 ms and log `thread_policy:`). |
| `bt_sco_pace=0` | Send iOS's SCO data as it comes (default: chip-sized packets, at most 2 ahead of the chip's; avoids the CTS stall). |

Read `kern.hackios.bt` for firmware, power, client, UART, IBS and SCO pacing counters. `kern.hackios.bt_trace` changes trace level live.
See [subsystems/bluetooth.md](subsystems/bluetooth.md) for tested scope and firmware requirements.

### `bsd/dev/hk_buttons.c`

| Boot-arg | Meaning |
|---|---|
| `board` | Device board. `instantnoodle` enables the OnePlus 8 drivers (touch, battery, slider, speakers, Wi-Fi PCIe, ...); the loader passes it. |
| `buttons` | redwood: 1 power/volume key driver. |
| `buttons_steps` | Stop key bring-up after N steps. |
| `buttons_wake` | Power-key wake mode. |
| `touchscreen` | redwood: 1 starts the Goodix/FocalTech driver. NEVER on the OnePlus 8. 0 turns off the OnePlus driver. |

### `bsd/dev/hk_camera.c`

| Boot-arg | Meaning |
|---|---|
| `cam` | 1..5 camera bring-up levels (5 = for iOS, redwood). |
| `cam_after` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_fake` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_fake_fps` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_frames` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_gain` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_keep_mb` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_mode` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_pattern` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_sensor` | wide selects the wide sensor in early modes. |
| `cam_settle` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_shutter` | *diagnostic/tuning knob: see the header comment of this file* |
| `cam_vblank` | *diagnostic/tuning knob: see the header comment of this file* |

### `bsd/dev/instantnoodle_camera.c` (OnePlus 8: `hk_camera.c` hands `cam=N` to it on `board=instantnoodle`)

| Boot-arg | Meaning |
|---|---|
| `cam` | 1 = a look, reading only (GCC/camcc clocks, every sensor pin, the PM8009, every camera rail on its PMIC); 2 = and each sensor powered up as Android does, its chip ID read, powered down; 3..5 = as 2 (no frame path on this board yet). Verified on the phone (2026-11-04: `cam=1` run 010516; `cam=2` run 044740, three of four sensors answer; run 081247, all four). |
| `cam_sensor` | `wide` (IMX586), `ultra` (IMX481), `front` (IMX471) or `macro` (GC2375); without it `cam=2` does all four in turn. With `cam=3`, `wide` takes the frames' path of the wide IMX586 (CSIPHY 0, three C-PHY trios; `cam_mode=6` its 2x crop) and `ultra` the ultra-wide IMX481's (CSIPHY 3, four D-PHY lanes, mode 0 4656x3496, its whole field; step 5's 20 MB frame in a piece taken at boot) instead of the front sensor's. Verified on phone: wide steps 2 and 5 (runs 122836, 124047); ultra steps 2 and 5 (runs 141639, 142526). `cam=5` serves whichever the program asks for with `HK_CAMERA_SENSOR` (1 wide, 2 front, 3 ultra-wide: run 142958, camera_test). |
| `cam_quickmode` | (`cam=5`) 1 (the default) = the wide IMX586's mode 1 <-> 6 (1x <-> 2x) changed in the session: stream off at a frame's end, the mode's table and the exposure in standby, stream on, ~265-315 ms without frames; 0 = a new session each time (2.6-2.7 s). Verified on phone (runs 133840, 134921). |
| `cam_fast` | Read by userland (`camera_device.c`), a measurement: 1 = auto exposure may not stretch a frame, so the rate holds and the dark stays darker. Off by default. In a test media library only (K38). |
| `cam_mem` | Read by userland (`camera_device.c`, `inject_media.c`), a measurement: 1 = every 300 frames mediaserverd's memory by VM tag, the phone's free pages and libmalloc's large blocks by who asked; the audio buffers at each node. Off by default. |
| `cam_focus` | (`cam=5` and `cam=3`) the wide IMX586's lens motor (LC898217XC at 0x72): 2 (the default) = driven (HK_CAMERA_FOCUS, codes 50-930, a move of at most 1/8 a frame, read back, parked to 50 before its power goes, on the way out by hand); 1 = its registers only read; 0 = not touched. Verified on phone (runs 134921, 135326, 140301's logdump). |
| `cam_after` | seconds after boot before the camera thread starts (default 20). |
| `cam_scan` | 1 = (with `cam=2`) a sensor that does not answer: its bus listed while it is powered, every address 0x08-0x77 read once (one byte, no register: nothing written). Verified on phone (run 080458: the wide sensor's bus, EEPROM 0x54 and lens motor 0x72/0x73 answering). |
| `cam_step` | (with `cam=3`) the frames' path's step; 1 (the default) = the camera subsystem's clocks and supplies up, CSIPHY 2's and CSID-lite 0's versions read, all down; 2 = and the front sensor streaming mode 1 through CSIPHY 2 into CSID-lite 0's RX, its packet state and counters read (no memory written). 3 = what the apps SMMU and the GIC have for the camera, read only; 4 = the SMMU set up for the IFEs' streams with a frame's memory, nothing streamed, all given back; 5 = one frame of the front sensor's into that memory through CSID-lite 0 / IFE-lite 0, handed to the loader (`xl camera`). Steps 1-5 verified on phone (runs 085649, 090634, 091514, 092704, 094343). |
| `cam=5` (OnePlus 8) | the front IMX471's frames for iOS through `/dev/hk-camera` (served by `instantnoodle_camera.c`, redwood's protocol); `cam_gain` there is linear (1024 = 1x). Verified on phone (run 101751: camera_test 60/60 frames, mediaserverd's capture stack). |
| `cam_gain`, `cam_shutter` | (OnePlus 8, `cam=3` steps 2 and 5) the front IMX471's analog gain code (SMIA 0x0204, Sony's 1024/(1024-code): 0x380 = 8x, at most 0x3c0) and coarse integration in lines (0x0202, at most the frame's lines less 10), written inside its hold on/off after the mode's table. Verified on phone (`cam_gain=0x380`, run 094952). (Redwood's driver has its own of the same names.) |
| `cam_pll2` | 1 = (with `cam=2`) a module file's 24 MHz MCLK (the macro GC2375's) from camcc's PLL2, started as Linux does (1440 MHz; RCG /10, M/N 1/6), put back as found after; without it 19.2 MHz from the crystal. Verified on phone (run 082733). |

### `bsd/dev/hk_cpufreq.c`

| Boot-arg | Meaning |
|---|---|
| `cpufreq` | 1: raise every used EPSS group to its top row and enable the governor/thermal service. |
| `cpufreq_gov` | Governor on (default with cpufreq=1); 0 = groups stay at their cap. |
| `cpuload` | *diagnostic/tuning knob: see the header comment of this file* |
| `ddr` | *diagnostic/tuning knob: see the header comment of this file* |
| `ddr_dark` | Memory vote with the screen off (1024). |
| `ddr_idle` | Memory vote after a second idle (1024). |
| `ddr_rest` | Memory-vote rest value (see hk_cpufreq.c). |
| `ddr_sleep` | Memory vote in system sleep (0 = none, floor). |
| `ddr_vote` | RPMh memory vote while busy with the screen lit (default 16383). |
| `gov_start` | Seconds after boot the governor takes over (default 45; tests use 30). |
| `membench` | Expose kern.hackios.membench (all cores writing, ~1 s). |

### `bsd/dev/hk_perfctl.c`, `hk_sched_check.c`, and the scheduler (`osfmk/kern/sched_*.c`, `osfmk/arm64/machine_routines.c`) — see [subsystems/scheduler.md](subsystems/scheduler.md)

| Boot-arg | Meaning |
|---|---|
| `sched` | `edge` (default where the cores fall in more than one frequency domain: both phones) or `clutch` (the previous scheduler, one processor set: the regression baseline). `edge` on a one-domain board (QEMU) runs Edge on one cluster. |
| `board_cpu_domains` | Frequency domain (= cluster) of each core number, 4 bits a core, lowest first. Default `0x21110000` when the loader names small cores (both phones), else one domain. Also used by `hk_cpufreq.c`. |
| `board_small_cores` | Mask of core numbers that are E cores, beside the loader's `small-core` (QEMU tests: `0x0f`). |
| `board_cpu_dpc<D>` | Energy model coefficient of domain D, µW/MHz/V² (default SM8250's DT: 100, 533, 642). |
| `perfctl` | 0: no performance controller (Edge with its static graph; every group prefers the boot cluster). |
| `perfctl_log` | 1: log every recommendation change (`perfctl: group ID NAME: class LEVEL -> LEVEL ...`). |
| `sched_check` | 1: the kernel's scheduler checks a few seconds after every core is up (`sched_check: PASS|FAIL ...`). |
| `sched_edge_bg_up` | 1: background-QoS threads may migrate/be stolen from E to P (Apple's code); default 0 keeps them on E. |
| `sched_edge_promote` | 0: no cross-core priority inheritance (a turnstile-promoted thread keeps its group's E preference). |
| `small_cores` | Under `sched=clutch` only: 0 all cores alike, 1 Apple's SMT-style pairing, 2 (default) slower cores as secondaries of their own. |
| `small_cores_after` | Seconds before the slower (E) cores start (default 28; 0 at once). |

### `bsd/dev/hk_display.c`

| Boot-arg | Meaning |
|---|---|
| `fb_scan` | redwood: tear-free copy by panel scan, 120 Hz. |

### `bsd/dev/hk_goodix.c`

| Boot-arg | Meaning |
|---|---|
| `touch_flip` | redwood touch axis flip. |
| `touch_lat` | 1: log latency lines for every touch sample. |
| `i2c_geni` | OnePlus 8 I2C buses (touch, gauge, amplifiers, slider): 0 (default) pins by hand; 1 also logs each bus's serial engine once (protocol, FIFO interface, clocks; read only, GCC clock votes only); 2 the engine itself in FIFO mode (Linux `i2c-qcom-geni.c`) where its FIFO interface exists, else by hand. **Verified on the phone 2026-10-27** (`024827`): touch, gauge and slider engines have FIFO (`if_disable 0`), boot-time reads 0 errors, touch 8-byte read 2082 -> 368 us, slider 1802 -> 210 us, gauge 1878 -> 1009 us (100 kHz). Touch under a real finger and the amplifiers' bus (needs `adsp=1 sound=4`) not yet run. |
| `touch_poll_us` | redwood: interrupt-line poll period. |
| `touch_rate` | redwood touch rate. |
| `touch_spi_hz` | redwood: touch SPI clock. |

### `bsd/dev/hk_haptic.c`

| Boot-arg | Meaning |
|---|---|
| `haptic` | 1..4 probe/play, 5 = for iOS. |
| `haptic_effect` | Effect number for haptic=4. |
| `haptic_trim` | OnePlus 8: the AW8697's factory f0 trim (`/mnt/vendor/persist/engineermode/f0_cali_data`, decimal, 0..63) written into TRIM_LRA (0x5b) as OnePlus's driver does; in the profile (`11` for this phone). Absent: the chip's reset value. |

### `bsd/dev/hk_mic.c`

| Boot-arg | Meaning |
|---|---|
| `mic` | 1 look, 2 codec on SoundWire, 3 listen, 4 feed iOS. |
| `mic_adc` | Analog ADC selection. |
| `mic_input` | Mic input selection. |
| `mic_main` | Main mic selection. |
| `mic_secs` | Seconds for mic=3. |
| `mic_tap` | Test: mic hears the haptic motor. |
| `mic_test` | Mic test mode. |

### `bsd/dev/hk_net.c`

| Boot-arg | Meaning |
|---|---|
| `net_names` | en0 naming. |
| `net_trace` | Trace en0 frames. |

### `bsd/dev/hk_power.c`

| Boot-arg | Meaning |
|---|---|
| `gpu_sleep` | 0 keeps the GPU powered through system sleep. |
| `sleep_max` | Cap every system sleep to S seconds (timed runs need it). |
| `sleep_freeze` | 0 (diagnostic): `mach_absolute_time()` runs on through a system sleep instead of standing still as an iPhone's. Bisects clock-related wake bugs; `power_test sleep` then reports "did not stand still". |
| `syssleep` | 0 refuses system sleep (default on with board=instantnoodle, off elsewhere). |

### `bsd/dev/hk_sound.c`

| Boot-arg | Meaning |
|---|---|
| `sound` | 1 port, 2 tone, 3 audible tone with amplifiers, 4 what iOS plays. |
| `sound_amps` | 0: amplifiers set up but never started (OnePlus default 0; profile passes 1). |
| `sound_below` | Amplifier level in eighths of a dB under Android max (default 240; profile 96). |
| `sound_gain` | Gain before the guard (default/max 96). |
| `sound_guard_as` | Test: guard reckons amps N under. |
| `sound_periods` | Periods queued at the DSP, 2 to 8 (default 8). Measuring; only the default has been listened to. |
| `sound_period_frames` | Frames in a period, 120 to 1024 (default 480, 10 ms). Measuring. |
| `sound_park` | 0: do not park the audio path when idle. |
| `sound_pause` | Bring-up step pause (ms). |
| `sound_secs` | Seconds for sound=1..3. |

### `bsd/dev/hk_sparse.c`

| Boot-arg | Meaning |
|---|---|
| `apfs_sparse` | Offset of a sparse image. |

### `bsd/dev/hk_speaker.c`

| Boot-arg | Meaning |
|---|---|
| `speaker_probe` | 1 pins, 2 amplifier IDs, 3 amplifiers set up alone. |

### `bsd/dev/hk_hbm.c`

| Boot-arg | Meaning |
|---|---|
| `hbm` | 1: the panel's high brightness mode may be asked for (`kern.hackios.hbm`: write N for a period of N s, 0 off; `power_test hbm N`). Default 0: never. Limits: 30 s a period, 60 s cooldown, 180 s in any 10 min; refused or ended at a battery of 38 C, a hottest zone of 60 C, a thermal pressure above nominal; always ended when the screen goes dark, iOS sets a brightness, and on the way to the loader or a panic. |

### OnePlus 8 touch and fuel-gauge observations

| Boot argument | Behavior |
|---|---|
| `touch_wake=1` | The IN2010 771 controller enters gesture mode at screen-off with `touch_irq=1 syssleep=0`. RAM #206 verified the controller's arm and exit readbacks during a lock and timed key wake. A physical tap and a wake from SoC sleep still require separate evidence. |
| `battery_trace=1` | Emits the existing five-second fuel-gauge sample with continuous time, signed mA, remaining whole mAh and native USB-present status. Each read has an explicit validity result. |

### `bsd/dev/hk_thermal.c`

| Boot-arg | Meaning |
|---|---|
| `thermal` | 0 off; 2 watch and report without cpufreq. Default on with cpufreq=1. |

### `bsd/dev/instantnoodle_modem.c`

| Boot-arg | Meaning |
|---|---|
| `modem` | The OnePlus 8's SDX55. 1 a look, reading only (pins, `pcie_2_gdsc`, the arbiter's channels of its PMIC). 2 and the warm reset and PCIe 2 to the device's boot loader (run 035821), no firmware, no bus master. 3 and its firmware up to, not including, image 8; with `modem_mission=1` on to its mission for 60 s (run 071723). At the end the link goes down and a warm reset stops its firmware (run 073746). **Only in the private tree's kernel for now**, `tools/nf/modem/`. |
| `modem_gnss` | With `modem=3 modem_mission=1 modem_loc=1`: 1 injects UTC, registers events and starts GNSS through the generic QMI LOC tables for 180 s, then stops. It logs counters, signal and uncertainty only, never coordinates or satellite identities. Run `015925` (bootq #156) accepted generic Start and delivered 120 position reports; it was indoors with zero satellites/fix. An owner-present outdoor RAM-only run is still required for RF evidence. Private tree. Mode 2 adds a volatile real-fix observer and requires explicit `wifi=0`. It accepts only the acknowledged generic LOC session, a satellite technology flag, valid coordinates and circular uncertainty, and a fresh UTC measurement. Its metadata is available in `kern.hackios.modem_fix_quality`. The kernel clears raw data on expiry, endpoint loss and teardown. |
| `ui_qmux` | 3 selects the OnePlus NF single-poller IOKit frontend. Requires `modem=3 modem_mission=1 wifi=0 modem_radio=0 modem_loc=0 modem_gnss=0`, with WDS settings and both IP_HW0 test profiles disabled. Only the real current NAS peer can back the two private protocol entries. Ordinary userspace opens are denied; no NAS diagnostic request is sent by default. Values 1/2 belonged to earlier private test kernels. |
| `modem_rfs` | 1 selects the NF read-only AP firmware service with `ui_qmux=3 modem=3 modem_mission=1 modem_radio=0 modem_loc=0 modem_gnss=0 wifi=0`. Keep the frontend test reads, family probe, WDS settings and IP_HW0 profiles off. The poll owner offers QRTR service 4096/raw instance 3 and accepts RRQ only for the approved modem_a FAT firmware subtree. WRQs are denied. The same owner observes normal UIM card/slot registration and readbacks, DMS, NAS and WDS. Default 0. See [ticket 276](../../oneplus8-audit/cellular-data-next/rfs-uim/hardware.json). |
| `ui_qread` | With `ui_qmux=3`, 1 opts in to one bounded kernel client exercise. It cancels an unsent token on qmux1, then submits up to one read-only NAS Get Serving System request per channel. Each token stays with its own publication generation. Defaults to 0. |
| `modem_radio` | In the private NF tree, `modem=3 modem_mission=1 wifi=0` enables bounded observations. Level 1 reads DMS mode, NAS signal and WDS packet state. USB RAM runs #207 and #208 delivered all three to iOS. Level 2 adds a NAS Get Serving System request for registration, circuit/packet attachment and radio-interface counts (phone run #215). Level 3 adds an optional read-only UIM Get Card Status request. The summary contains counts only. Level 4 additionally asks WDA for the PCIe data format and records the presence of the link protocol, QoS flag and bidirectional aggregation modes. Level 5 observes non-identifying UIM card error reasons three times. Level 6 also reads physical slot status and requests current temperatures from the external modem, using the advertised TS service 23 v1 instance 100. The thermal query does not set thresholds. Each request has a 1 s timeout. |
| `modem_wds_settings=1` | Off by default. Within an active NF `modem_radio` diagnostic, a successful WDS connected-state read permits one bounded WDS Get Current Settings for that sample on the same client and peer. On a valid reply, the packet field of `kern.hackios.modem_radio_status` reports `detail = (family << 16) \| mtu` for the observed IPv4 or IPv6 address. Zero means unavailable. Endpoint removal or mission teardown clears it. The diagnostic never starts a bearer or creates a data interface. Built and replay-tested offline. |
| `modem_iphw0=1` | Private NF SDX55 command-only test. Requires `modem=3 modem_mission=1 wifi=0` with radio, WDS settings, LOC and GNSS off. Configures empty 100/101 transfer rings, ER7/8 and 17 event contexts, waits for the mission event and M0, then checks START/RESET acknowledgments. The data channels receive zero payload buffers and post zero credits. Ambiguous ownership keeps the arena pinned for the lifetime of that kernel. Host simulation and retained hardware receipts are distinguished in the [board test notes](../../oneplus8-audit/cellular-data-next/ip-hw0/BOARD.md). |
| `modem_iphw0_rx=1` | Opt-in NF receive-credit trial, exclusive with `modem_iphw0=1`. Requires `modem=3 modem_mission=1 wifi=0` with radio, WDS settings, LOC and GNSS off. Channel 101 posts one 32,680-byte receive buffer to ER8 for 500 ms. The board logs completion metadata and retires both channels. Ticket 266 posted and safely reclaimed the credit, recorded zero matched completions and returned to stock FASTBOOT. [Physical receipt](../out/eye/mhi-rx-credit-ticket-266/result.json) |
| `cell` | 1: the OnePlus 8's modem through the ported driver ([subsystems/cellular.md](subsystems/cellular.md); with `modem=5`, which does the warm reset and hands over). Its options have their defaults with it: data, the IMS data service, a call's sound. Only in the cellular kernels (`tools/nf/cell/build.py`), which is what the installed "Cellular" bundle runs. |
| `cell_dcm` | The IMS data service the modem asks this side for (QMI service 0x302). On with `cell=1`; 0: off, and VoLTE does not register. |
| `voice` | The sound of a call, in steps: 1 asks the DSP's voice service its version, 2 and maps the mailbox for the DSP and the modem, 3 and sets up the sessions while a call is up. Not given: 3 with `cell=1`, 0 without. `voice=0` turns it off. Sysctls: `kern.hackios.voice_call`, `voice_speaker`, `voice_mute`, `voice_earpiece_gain`. |
| `voice_sat` | The DSP's own road to the modem (GLINK `mhi_sat`). Follows `voice` (on from 3); 0 or 1 says otherwise. `voice_bridge=1` is a road that does not work and stays off. |
| `modem_loc` | With `modem=3 modem_mission=1`: 1 asks QMI LOC two questions that start nothing (engine lock, operation mode); run 102633. Private tree. |
| `modem_mission` | With `modem=3`: 1 sends image 8, so the modem's firmware starts. Off unless given. **With the owner there, and through the supervisor:** a hung kernel leaves the firmware running, and nothing here reads the modem's temperature. |
| `modem_gap` | With `modem=3`, a test: this offset's page of the boot loader's image has no translation, so the SMMU says whether the device reaches it. BHI then fails. Private tree too. |

### `bsd/dev/hk_torch.c`

| Boot-arg | Meaning |
|---|---|
| `torch` | 1 look, 2 light briefly, 3 for iOS (and the camera's flash). **3 is the OnePlus 8's default; `torch=0` is off.** |
| `torch_max_s` | Longest continuous light (default 30, max 120). |
| `torch_flash_ma` | The camera's flash, a channel: default 500, at most 1000 (Android's). |
| `torch_secs` | Seconds for torch=2. |

### `bsd/dev/hk_ufs.c`

| Boot-arg | Meaning |
|---|---|
| `darwin_blocks` | Size of darwin in blocks. |
| `darwin_file` | darwin as a file of userdata (extent map). |
| `darwin_lba` | First LBA of darwin. |
| `darwin_part` | Name of the port partition. |
| `hk_data` | 1: /dev/hk_data over userdata blocks 3145728..end; 2: RAM disk. |
| `hk_data_mb` | RAM-disk size for hk_data=2. |
| `ufs_bench` | Storage benchmark. |
| `ufs_check` | Hash partitions with reference images. |
| `ufs_native` | 1: kernel's own read-only UFS command list (hk_ufshc.c). |
| `ufs_native_bench` | Native benchmark. |
| `ufs_native_check` | Native driver self-check. |
| `ufs_shared_pa` | Physical address of the loader's storage page (loader passes it). |
| `ufs_write` | 1: allow writes to /dev/rufs_darwin past BLOCK_WRITE_FIRST. |
| `var_save_ms` | Kernel-side wait for the var saver on return (ms). |

### `bsd/dev/hk_ufshc.c`

| Boot-arg | Meaning |
|---|---|
| `ufs_native_buffer` | Native data buffer size. |
| `ufs_native_hang` | Native hang test. |
| `ufs_native_leave` | Leave native list in (diagnostic). |
| `ufs_native_slots` | Native slots used. |
| `ufs_native_spin` | Native spin test. |

### `bsd/dev/hk_virtio_blk.c`

| Boot-arg | Meaning |
|---|---|
| `virtio_blk` | QEMU virtio block. |

### `bsd/dev/hk_virtio_input.c`

| Boot-arg | Meaning |
|---|---|
| `virtio_input` | QEMU virtio pointer/keyboard. |

### `bsd/dev/hk_virtio_net.c`

| Boot-arg | Meaning |
|---|---|
| `virtio_net` | QEMU virtio network (en0). |

### `bsd/dev/hk_virtio_sound.c`

| Boot-arg | Meaning |
|---|---|
| `virtio_sound` | QEMU virtio sound. |

### `bsd/dev/hk_wifi.c`

| Boot-arg | Meaning |
|---|---|
| `wifi` | 1: bring up the Wi-Fi radio and station (stands for wifi_irq, wifi_ba, wifi_wide). **On by default on the OnePlus 8 (`HK_BOARD_DEFAULTS`, pexpert/arm/pe_bootargs.c); `wifi=0` is off.** |
| `wifi_again` | DANGEROUS: reset the phone twice out of two. |
| `wifi_ask` | Ask the service its capabilities. |
| `wifi_dirty` | Leave the DSP running at the end. |
| `wifi_file` | 0: do not join fw/network at start. |
| `wifi_fine` | Pause before every access. |
| `wifi_listen` | Seconds step 14 waits for the firmware service. |
| `wifi_loud` | Say the whole bring-up. |
| `wifi_lx` | 1/2: ath11k itself instead of the stand-in. |
| `wifi_mac` | The radio address for the driver (normally from fw/mac). |
| `wifi_new` | Unproven steps from K. |
| `wifi_pause` | ms pause. |
| `wifi_radio_read` | Step 16 reads radio registers. |
| `wifi_return` | Return to loader after steps. |
| `wifi_step` | Run Wi-Fi steps 1..N and stop. |
| `wifi_wait` | Wait. |
| `wifi_watch` | Seconds of alive lines. |

### `bsd/dev/hk_wlfw.c`

| Boot-arg | Meaning |
|---|---|
| `wifi_cal` | Step 15 cold-boot calibration. |
| `wifi_mission` | Step 15 goes on to mode mission. |
| `wifi_ready` | Seconds to wait for "firmware ready". |

### `bsd/dev/instantnoodle_battery.c`

| Boot-arg | Meaning |
|---|---|
| `battery_empty_mv` | Empty-battery threshold in mV (default 3300). |
| `battery_shutdown` | 0: only log an empty battery. |
| `charge_icl` | 0: leave the charger input limit to the firmware. |

### `bsd/dev/instantnoodle_slider.c`

| Boot-arg | Meaning |
|---|---|
| `slider` | 0: do not read the alert slider. |
| `slider_cal` | Factory calibration D,U,D,U,D,U of the alert slider. |
| `sensor_probe` | OnePlus 8, default 0: when the existing slider path is enabled, log GPIO4/5 and do one read-only ACK plus four-byte raw read at ODM candidate I2C address `0x46` after the slider's existing `ldoc10` rail is on; no sensor register write or SLPI start. One failed ACK gets one bus recovery and retry. |

### `bsd/dev/instantnoodle_touch.c`

| Boot-arg | Meaning |
|---|---|
| `touch_irq` | 0: poll the OnePlus touch chip every 10 ms. |

### `bsd/hk_hfs/hk_hfs_iokit.c`

| Boot-arg | Meaning |
|---|---|
| `hfs_corruption_panics` | Panic on HFS corruption. |

### `bsd/miscfs/apfs/apfs_vfsops.c`

| Boot-arg | Meaning |
|---|---|
| `apfs_cheap` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_meta_cluster` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_par` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_root_c` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_root_v` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_round` | *diagnostic/tuning knob: see the header comment of this file* |
| `apfs_ubc_read` | *diagnostic/tuning knob: see the header comment of this file* |

### `iokit/Kernel/GenericARMPE.cpp`

| Boot-arg | Meaning |
|---|---|
| `epoch` | Override the time-of-day epoch. |
| `machine_name` | Override the machine name iOS sees. |
| `sleep_test` | QEMU test: sleep S seconds repeatedly in GenericARMPE::start (tools/sleep-test). |
| `sleep_test_count` | Number of test sleeps. |
| `small_cores_after` | Seconds before the slow cores start (default 28; 0 = at once). |

### `iokit/Kernel/HKGPU.cpp`

| Boot-arg | Meaning |
|---|---|
| `gpu_boost` | ms of top GPU level after a touch (default 300; 0 off). |
| `gpu_cached` | 1: GPU buffers are ordinary cached memory. |

### `iokit/Kernel/MiniIOMobileFramebuffer.cpp`

| Boot-arg | Meaning |
|---|---|
| `fb_hz` | 60: switch the OnePlus panel back from 90 Hz. |
| `fb_probe` | 1: dump the OnePlus 8 display pipeline. |
| `mdp` | OnePlus 8, our own MDP layers (`bsd/dev/instantnoodle_mdp.c`, docs/research/mdp-driver-plan.md). **8 is this board's default (pe_bootargs.c)**; other boards 0. 0: nothing of it runs, the GPU scales as before. 1: read only, the firmware's pipeline decoded. 2: DMA0 as a solid fill over the frame, then everything back. 3: the same, then DMA and VIG pipes fetching from the framebuffers (nothing outside what scanout has proven mapped). 6: the display's stream through a context bank of ours (S2CR changed), frames as today. 7: and the window server's frames straight to the panel, the MDP scaling them: no GPU scale, no copy. 8: and every layer of a swap in its own pipe, the limits published to CoreAnimation. |
| `edr` | OnePlus 8, CoreBrightness's architecture (MiniAppleARMBacklight.cpp). 0: the old one, `backlight-level`, and CoreAnimation is told 0 nits. **1, this board's default (pe_bootargs.c)**: the new one, by nits, with no ExtrabrightEDR. 2: and ExtrabrightEDR, for HDR. |
| `ca_nits` | 1: in the window server, each `setNits:` and every 5 s what CoreAnimation takes the SDR white for (userland/src/inject.c). |
| `mdp_trace` | With `mdp=8`: the first N swaps of more than one layer, with set and cleared bits, kept layers and pixels sampled from memory. Default 0. |
| `fb_wait` | With the pipes (`mdp=7` and up): how the frame the panel is being sent is kept from being drawn into. A sum, **default 13**: 1 a GPU submission that writes a surface being fetched waits for the push's end (the guard; also what covers a detached surface); 2 SwapWait does not return while the surface drawn into next is being fetched (superseded by 4); 4 SwapWait answers for the swap and options it is asked about, kIOReturnBusy when it would have to wait and may not, so CoreAnimation makes itself a third surface; 8 and it does not answer until the swap before last has started its push, which keeps the frame's start in phase with the pushes. 0: none of it, the old behaviour, with the black triangles. Replaces the short-lived `fb_fetch_wait`. docs/research/mdp-driver-plan.md 9. |
| `fb_hold_push` | 1: the presenter does not go on until the push has ended, so no swap's surface is drawn into meanwhile. Removes the triangles, costs frames (the owner: 「卡」). For comparison only. Default 0. |
| `mdp_overlap` | 1: the first 40 submissions that name a surface being fetched are said, to write or to read. `fb_wait` other than 0 turns the counting on anyway, in the 5-second `mdp: swaps named` line. Default 0. |
| `mdp_shots` | With `mdp=7` and up: at most every N ms, and once after the frames stop, the latest is also scaled into a framebuffer on the CPU, for `xl screenshot` (which reads memory and cannot see the pipes). 0: none. Default 2000 in a run from the computer (`loader_return_after=`), otherwise 0. Each costs the presenter 10 to 25 ms. |
| `mdp_reads` | With `mdp`: reads never made before, one bit each, default 3: 1 dispcc's MDP clock, 2 the VIG pipes' scaler and CSC, 4 the SID block, 8 VBIF. |
| `return_dark` | 1: the OnePlus 8 panel goes back to the loader dark -- DCS display off (0x28), as the screen lock sends it, after the last push -- so no still loader screen stays on the OLED between runs. Every kernel built since 2026-11-04 sends display on (0x29) before its first push; under an older kernel the power key twice lights it (or a reset). `run_cam_probe.py` passes it. Verified on phone (run 083542: "back to the loader dark (display off sent)" in the full returned log; the owner confirmed the screen dark in the loader and lit again by the next run, 2026-11-04). |
| `metal` | 1 Metal driver for test programs; 2 whole system (with metal_profile=1). |

### `iokit/Kernel/MiniIOSurface.cpp`

| Boot-arg | Meaning |
|---|---|
| `surftrace_of` | Trace a surface. |

### `osfmk/arm64/loader_return.c`

| Boot-arg | Meaning |
|---|---|
| `bootcpu` | Run the kernel on this core (profiles pass 7) instead of the firmware default; the loader watches from the first core. |
| `hv_wdt_pet` | Pet the Qualcomm hypervisor watchdog from the return channel. |
| `loader_return_after` | Return to the loader after S seconds of wall time. |
| `return_cpu` | Core the return runs on. |
| `return_panic` | Return to the loader on panic (see loader_return.c). |

### `osfmk/arm64/user_exception_trace.c`

| Boot-arg | Meaning |
|---|---|
| `ustacks` | Dump user thread stacks (qemu/touch.py stacks NAME). |
| `ustacks_at` | *diagnostic/tuning knob: see the header comment of this file* |
| `ustacks_count` | *diagnostic/tuning knob: see the header comment of this file* |
| `ustacks_ms` | *diagnostic/tuning knob: see the header comment of this file* |
| `ustacks_threads` | *diagnostic/tuning knob: see the header comment of this file* |
| `ustacks_tname` | *diagnostic/tuning knob: see the header comment of this file* |

## Userland (read through `sysctl kern.bootargs`, userland repo)

| Boot-arg | File | Meaning |
|---|---|---|
| `gl=0/1` | `inject.c`, `inject_sb.c` | 0: never load the OpenGL ES library; 1: also without a GPU (Mesa software) |
| `glyes=N,..`, `glno=N,..` | `inject.c` | tell CoreAnimation's renderer it has/has not those shader functions. **`glyes=40` crashes the window server** (see research/ios-userland.md) |
| `ca_fps=N`, `ca_vbl=1`, `fb_hz` | `inject.c`, `inject_sb.c` | frame-rate range / VSync-driven frame clock (automatic where the panel is faster than 60) |
| `ca_ambient=N` | `inject_sb.c` | frame rate of slow ambient display links (default 30; 0 off) |
| `ca_rates=1`, `ca_links=1` | `inject.c`, `inject_sb.c` | diagnostics: list rate methods / display links |
| `cc_fade=MS` | `inject_sb.c` | check-only workaround: eased Control Center fade |
| `sb_clock=N` | `inject_sb.c` | status-bar clock centred N points further right (OnePlus camera hole) |
| `wifi_scan_keep=S` | `hk_sta.c` | a network heard in the last S seconds stays in the scan results wifid reads (default 120; 0: the last scan only) |
| `wifi_dwell=1`, `wifi_dwell_active=A`, `wifi_dwell_passive=P` | `wifi/lx/src/lx_80211.c` | a scan as two driver scans: channels it may send on A ms each, DFS/no-IR ones P ms (wifi_dwell=1: 40/110; off: the driver's 50/150) |
| `sb_delay=S` | `inject_sb.c` | SpringBoard start delay so apps register in a stable order (default 8 under `xl run`) |
| `autounlock=0` | `inject_sb.c` | keep the lock screen (default: dismiss after 10 s) |
| `siri_orb=0` | `inject_sb.c` | skip Siri orb shader prewarm (diagnostic) |
| `touch_lat=1` | `inject.c`, `inject_sb.c` | latency lines |
| `hkprof=1/2` | `hkprof.h` | userland sampling profiler (2: from process start) |
| `gltrace*`, `glcensus`, `glshaders`, `glshow*`, `glskip`, `glplain`, `glwait`, `glcopy`, `glfetch`, `glgroup`, `vblur`, `glenv`, `glslow` | `eagl.c` | OpenGL ES debugging; see the header of `eagl.c` |
| `metal_profile=1` | `hkmetal.m` | with `metal=2`: whole system on Metal; `metal_sync=1` fence per draw |
| `metal_backend=vk` | `hkmetal.m` (`mesa_available`) | Metal over Vulkan: the driver opens `/hackios/lib/libvulkan_freedreno.dylib` (turnip with `tools/mesa/air/air_vk.c`) instead of `libhackiosair.dylib` (Gallium). The environment variable `HK_METAL_BACKEND=vk|gallium` wins over it, for test programs (`metal_drv_test --backend`). M3a only (TODO T38 (12)): draws with no resources |
| `softpipe`, `llvmpipe` | `gl_osmesa.c` | Mesa software driver choice |
| `root_apfs`, `var_store`, `var_data`, `var_data_format`, `var_save_every` | `init.c`, `var_store.c` | root and persistence (see storage.md) |
| `exctrace=1 applog=1 applog_name=APP`, `systrace=1/2 systrace_name=NAME` | kernel `user_exception_trace.c` (`applog` is only mentioned in its comment) | trace failing syscalls / crashes (`usertrap:`, `userabort:` in the kernel log: on the OnePlus 8 read them from the logdump, the loader's live log missed them) |
| `campattern=1` | userland `camera_gpu.c` (OnePlus 8, `cam=5`) | the GPU draws moving grey stripes instead of the camera's picture: two pictures mixed in one break them (a diagnostic; screenshots break them anyway, see display.md) |
| `log_launchd=0` | kernel `bsd/dev/arm/km.c` `loader_log_console()` (the patch); used by `tools/t` (`T_LAUNCHD=1` drops it), README, `../oneplus8-audit` stable-plan.json | keeps launchd's lines ("com.apple.xpc.launchd\|...") out of the loader's log; the screen keeps them. `log_launchd=1` (the default) keeps them: launchd says why each daemon exited ("exited due to SIGSEGV \| sent by exc handler[PID]", used for the OnePlus 8's mediaserverd, 2026-11-04) |

## Kernel sysctls (`kern.hackios.*`)

56 exist in the source (`bsd/dev/*.c`, `iokit/Kernel/*`); most are probes. Used by tests/docs: `thermal`, `thermal_pressure`,
`thermal_hottest`, `thermal_throttles`, `sleeps`, `sleep_seconds`, `sleep_last`, `sleep_timer`, `sleep_timer_key`, `display_on`,
`backlight`, `backlight_sent`, `top`, `cpuspeed`, `memlat`, `membench`, `frames`, `kprof_ctl`, `kprof`, `slider`, `charger`,
`battery_soc`, `battery_charging`, `var_save`, `var_busy`, `adsp_services`. List them on a running system with `sysctl kern.hackios`.
The others are not yet documented individually (TODO). Scheduler: `amp` (text: clusters, graph, Edge's and the controller's counts,
latency histograms, energy model, busiest groups; `sched_test amp LABEL`), `sched_check` (write 1: run the kernel's scheduler checks now;
read: passed << 16 | failed) — [subsystems/scheduler.md](subsystems/scheduler.md).

## iphont development agent (built; physical transport validation pending)

`iphont=1` enables kernel diagnostics and creates root-only `/dev/iphont`; default is 0. `iphont_control=1` additionally authorizes input and userspace requests for that development boot; default is 0. `iphont_selftest=1` runs native read-only kernel probes and an authorization-denial check at startup; it still needs `iphont=1`. Runtime `iphont trace LEVEL` selects event severity 0..4. See [TOOLKIT.md](TOOLKIT.md) for protocol, validation evidence, logging overhead, and the owner's deferred RAM reload. No flag is added to the normal installed profile.

`fb_panel_rate=0` keeps the firmware panel timing when enabling instantnoodle frame pushes: skips the ANA6705-specific DCS rate sequence. Default1 preserves historical behavior. Set `fb_hz` explicitly to the actual existing rate; this flag does not measure or change it. Local amb655uv01 display validation in progress.

`fb_panel=amb655uv01` selects the native three-packet LP-mode 60/90Hz timing-switch sequence for that panel when `fb_panel_rate=1`. It preserves packet types and the 16ms wait for the 60Hz sequence. Other nonempty names send no rate commands; an absent name preserves the historical ANA6705 path. Native90 hardware validation pending.

`wifi_pcie_probe=1` on instantnoodle performs bounded GCC/TLMM/commandDB reads and suppresses normal Wi-Fi startup. `=2` additionally owns a PCIe-link-only trial (no firmware/MHI/RF), checks endpoint17cb:1101, watches20s, and powers down; existing active/poisoned domains are refused. Stages1/2 verified on currentIN2010; normal Wi-Fi firmware/association remains unverified.

`buttons_voldown_pullup=1`: opt-in IN2010 firmware handoff repair for PM8150 GPIO7. Only when the mapped down key is GPIO7/input/VIN1/enabled/pull-down4, writes pull register0x42 low3bits to0 (native FDT pull-up) and verifies. All other modes/pins stay untouched; hardware key-event validation pending.

Local IN2010 diagnostic update (2026-11-09): `slpi_glink_probe=1` inventories IPCRTR/QRTR after the owned one-shot native SLPI boot; `=2` sends one default-only `ambient_light` SUID lookup; `=3` sends the same request with `default_only=false`; `=4` sends a bounded sequential inventory for `registry`, `timer`, `accel`, `gyro`, `ambient_light`, `proximity` and `mag` over the one discovered QRTR/QMI client. Modes2-4 observe the service and capture bounded replies; they do not configure sensors, write registry data, claim calibrated lux, or expose an iOS ALS service. The inventory records the QMI response transaction so a late reply cannot be attributed to another datatype. These require the cold-provenance/held-proxy gates and a full reset after each DSP trial. Default0 leaves this path inactive.

On instantnoodle, `slpi_dma_arena=1` builds and verifies the private FastRPC argument page and its three translation tables in XNU memory, then frees them. `slpi_bank_ro=1` inspects CB6 only when the captured MMU geometry matches, the display uses another bank, and valid global routes leave the SDSP streams and CB6 free. Both diagnostic paths return before SLPI startup. Run them via `tools/sensors/run.py dma-arena` and `bank-ro`.

IN2010 `slpi_dma_arena=2` (runner `dma-route`) temporarily enables CB6 with the private argument-page tables and verifies an exact 0x541 stream match. Value 3 (`dma-context`) exercises the bank without publishing a stream match. Both require isolated device arguments and cold provenance. DSP stays stopped. Read `enabled`, `mapped`, `restored` and `released` separately; an inactive stage-one CBAR can persist after verified memory reclamation.

IN2010 diagnostic values of `slpi_glink_probe` 7, 8 and 9 select `rpc-attach`, `rpc-listener` and `rpc-next`. All three require a fresh owned SLPI start and a verified SDSP mapping. The sender waits for service 400 and FastRPC channel/intent readiness. Mode 8 follows a successful attach with init2; mode 9 follows that with one bounded next2 receive. A pending next2 at shutdown records complete=0. No simulated host service replies are generated.

On IN2010, values 10 and 11 of `slpi_glink_probe` select `rpc-module` and `rpc-module-data`. They pair a pending next2 call with a separate foreground FastRPC transaction for opening/starting adsp_default_listener. Each call has its own argument page and transaction context. Mode11 supplies 256 bytes of input-blob receive capacity and decodes a bounded apps_remotectl.open callback.

In the private IN2010 sensor RAM trial, `slpi_oem_board=1` requires the cold-owned `slpi_glink_probe=12` diagnostic. Before PAS AUTH it validates the existing 6792-byte generic SMEM item130, requires empty light/proximity records, then publishes 17 stock board-description words. A confirmed owned shutdown retires those words; an uncertain shutdown retains them for the established cold recovery. Stage1 read-only inventory uses its existing arguments and never publishes sensor data.

With that private board trial active, `slpi_oem_attrs=1` retrieves attributes from the separately discovered wise_light and proximity SUIDs. Level 2 additionally allows one bounded wise_light request after its SUID, datatype, availability and supported stream are confirmed. Its 1025 events carry vendor-reported lux, C channel and reported period. Neither level feeds automatic brightness.

`ui_qevents=1` enables the isolated IN2010 NAS notification fixture with
`ui_qmux=3`, Wi-Fi off, mission 1, and the ordinary radio, GNSS/LOC,
WDS-settings and IP_HW0 diagnostics off. It is mutually exclusive with
`ui_qread=1`. One published qmux1 lease performs a baseline GET and a
Serving System Events enable. The observation window lasts 12 seconds
after the enable ACK. An explicit clear precedes port retirement on the
normal path. The 15-second command deadlines and the existing mission
teardown apply. Default zero sends no fixture traffic.
