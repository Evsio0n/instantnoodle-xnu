# instantnoodle-xnu

**iOS 16.5 on the OnePlus 8.** Apple's open-source XNU kernel (`xnu-8796.121.2`) and the unmodified iOS 16.5 user
space, running on a OnePlus 8 (IN2010, codename *instantnoodle*, Snapdragon 865).

[中文說明](README.zh.md)

> This is a research project. It is not affiliated with, authorised or endorsed by Apple Inc., OnePlus or Qualcomm.
> **No Apple files, keys or device dumps are in this repository.** The build scripts take what they need from your
> own IPSW download and your own phone. See [NOTICE](NOTICE) for the third-party files that are included.

Based on [HackiOS](https://github.com/oosexgang/hakios-xnu-kernel) by OOSEX GANG (POCO X5 Pro *redwood* and QEMU) and
the OnePlus 8 work in [haxi0/iphont-xnu-kernel](https://github.com/haxi0/iphont-xnu-kernel). The original HackiOS
README is kept as [README.hackios.md](README.hackios.md). Licensed under the Apple Public Source License 2.0
([LICENSE](LICENSE)).

This is a single repository: the kernel side and the user space (`userland/`) are both here. The two-git-directory
setup the upstream documentation describes (`common/setup.sh`, `common/ugit`) is not needed.

---

## Status

Verified on **one** unit (IN2010, panel AMB655UV01) as of **2026-11-11**. "Installed" means it is in the build that boots
from the phone's own flash with no computer attached. "RAM" means it has only run in a test boot from RAM. Most checks
were made on RAM boots of the same kernel and libraries as the installed build; the notes say where that matters.

### Works

| Feature | State | Notes |
|---|---|---|
| Boot to the home screen, standalone | ✅ installed | Boot menu with a USB developer mode; falls back to the previous version after 3 failed boots |
| Storage (UFS), user data on HFS+ | ✅ installed | Data survives reboots |
| Metal over Vulkan (Mesa turnip), whole system | ✅ installed | The default now: compositor, home screen, Settings, Safari, Photos, Maps, Camera. Stutter and memory growth remain, see Known issues |
| Own display driver (MDP layers), 90 Hz | ✅ installed | Hardware planes instead of GPU composition where possible; no tearing triangles; 0 underruns and 0 SMMU faults in the runs read |
| Touch, power key, volume down | ✅ installed | |
| Manual brightness, EDR and HBM | ✅ installed | No auto-brightness yet |
| HDR photos | ✅ installed | Gain maps through real CoreImage on Metal; checked by eye in RAM boots |
| Maps | ✅ installed | Real map data draws correctly; gestures lag. Checked by eye in RAM boots |
| Wi-Fi 2.4/5 GHz (QCA6390), Safari | ✅ installed | WPA2; joins a network saved in the build's `fw-wifi/network` by itself. That file is yours and is not in this repository |
| All three back cameras' and the front camera's preview | ✅ installed | The preview's repeating frames are fixed. Preview is 15 fps in the dark |
| Flashlight and camera flash | ✅ installed | On by default |
| Bluetooth | ✅ installed | On by default; the controller comes up. Audio to a device not rechecked on this build |
| Cellular: carrier, signal, mobile data | ✅ installed | Snapdragon X55 modem. The driver is the OnePlus 8 Pro port's ([dekefake/hackios-op8pro](https://github.com/dekefake/hackios-op8pro)), moved to this phone: [tools/nf/cell](tools/nf/cell/README.md). Data works beside Wi-Fi |
| SMS | ✅ installed | Both ways in Messages: Chinese, emoji, long messages, the lock-screen notification. No MMS |
| Phone calls (VoLTE) | ✅ installed | Both ways from iOS's own Phone app, with sound at the earpiece. The call volume cannot be changed yet; the speaker button and mute are not confirmed by ear |
| Speakers, main microphone, vibration | 🧪 installed, partly rechecked | The microphone and the earpiece carry real calls on this build. iOS's own sounds through the speakers worked in earlier RAM boots and were not rechecked |
| Wi-Fi location | 🧪 RAM | Maps located correctly in a RAM boot. On a kept `/var` the switch was set once by hand; not confirmed on the installed build. Needs Wi-Fi: no GPS |

### Not working yet

| Feature | Notes |
|---|---|
| System sleep | **Off in the installed build**, so the power key only locks the screen and battery drain is higher. With Wi-Fi off, sleep and wake work in RAM. With Wi-Fi on, its firmware crashes on resume and the phone must be restarted by hand |
| GPS | The modem's location service answers and reports, but there has been no fix outdoors and nothing feeds iOS's location service yet |
| Sensors, auto-brightness | Need the sensor DSP (SLPI) and its whole stack; the DSP comes up and is being probed, no sensor data yet. With no proximity sensor **the screen stays on at the ear during a call**; no rotation either |
| The rest of telephony | Hold, call waiting, keypad tones, MMS, voicemail and Personal Hotspot are not done. A call over a Bluetooth headset and emergency calls have not been tried |
| Third-party apps | First steps: an unencrypted app can be fetched, registered and its icon shown, and it has been seen to start. It has not been seen to draw. Encrypted (FairPlay) apps will not run |
| App Store, Apple ID, iCloud, push, FairPlay | Blocked: Apple's servers want a signature made with keys inside a real iPhone's Secure Enclave, and this project does not forge a device's identity. So no iMessage or FaceTime, and an app gets no notifications while it is closed. Calls, SMS and the phone's own notifications do not depend on it |
| Macro camera (GC2375) | Detected only |
| Portrait mode depth | Needs two cameras streaming at once |

### Known issues

- **The compositor's memory grows and does not come back**, about 2.5 GB in 12 minutes of heavy use. After about 10 minutes of that, background apps get killed, and the screen can restart once
- **About 1 frame in 10 is late in daily use** (90 Hz panel), and it was so before the cellular build too. The GPU is mostly idle (busy 3% at the median); the best lead is render pipelines compiled while drawing, with no cache on disk. Analysed from logs, not fixed: [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) K35, plan in [docs/TODO.md](docs/TODO.md) T40
- **Gestures in Maps do not follow the finger closely.** A GPU frame of Maps and the compositor together takes about 15 ms against 90 Hz's 11 ms, and first use of a screen stalls while pipelines are built
- Heat: the first minutes after boot read up to about 87 °C on the CPU sensors, and heavy use has throttled once or twice at about 95 °C
- Camera: 15 fps in the dark, autofocus can walk out of range in the dark, Portrait has no depth effect
- Setting a wallpaper does not work. The lock screen's own wallpaper draws, but its long-press picker is turned off by this port: it came out black before the move to Metal and has not been tried since
- Two of Maps' pipelines (`GlowAlpha`, `PatternedRibbon`) and Siri's orb are refused (framebuffer fetch at 4 samples), so those effects do not draw. What else Metal refuses or stands in for: K42
- The cellular kernel is not made by the normal build yet. It is an overlay build that needs objects from the developer's own build container ([tools/nf/cell/README.md](tools/nf/cell/README.md))
- The keychain protects nothing (no Secure Enclave, stand-ins instead): do not keep important accounts on this phone

---

## Building and running

See [docs/devices/instantnoodle.md](docs/devices/instantnoodle.md), [docs/BUILD.md](docs/BUILD.md) and
[docs/INSTALL.md](docs/INSTALL.md). In short: build the kernel and images on your computer, put the phone in the boot
menu's USB developer mode, and run it from RAM with `xl` (nothing is written). Installing to flash rewrites the
phone's `userdata` and removes Android.

> ⚠️ Flashing can leave the phone unusable. Know how to recover it ([docs/INSTALL.md](docs/INSTALL.md),
> [docs/research/boot-chain-and-recovery.md](docs/research/boot-chain-and-recovery.md)) before you install.

## Roadmap

1. Metal: the stutter of daily use and the compositor's memory growth (pipeline cache first; measurement before any fix)
2. Wi-Fi across system sleep, so the installed build can sleep
3. Sensors through the SLPI: proximity first now that calls work, then auto-brightness and rotation
4. Cellular: the driver into the normal build; call volume, hold, keypad tones, MMS
5. GPS: a fix outdoors, then iOS's location service
6. Third-party apps; then a TrustZone-backed keychain and the Titan 480 hardware ISP
