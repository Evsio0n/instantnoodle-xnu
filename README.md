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

Verified on **one** unit (IN2010, panel AMB655UV01) as of **2026-11-04**. "Installed" means it boots from the phone's
own flash with no computer attached.

### Works

| Feature | State | Notes |
|---|---|---|
| Boot to the home screen, standalone | ✅ installed | Boot menu with a USB developer mode; falls back to the previous version after 3 failed boots |
| Storage (UFS), user data on HFS+ | ✅ installed | Data survives reboots |
| Display, 90 Hz panel mode, no tearing | ✅ installed | Real frame rate is often ~45 fps (see Known issues) |
| GPU compositing (Adreno 650, Mesa freedreno) | ✅ installed | Metal is experimental |
| Touch, power key, volume down | ✅ installed | |
| Manual brightness | ✅ installed | No auto-brightness yet |
| Wi-Fi 2.4/5 GHz (QCA6390), Safari | ✅ installed | WPA2 verified |
| Front camera (IMX471) in Camera.app | ✅ installed | 30 fps preview, image processing on the GPU |
| Main camera (IMX586, 48 MP) as 1x, with autofocus | ✅ installed | Colour tuning from the phone's own calibration |
| 2x (IMX586 sensor crop; digital zoom in low light) | ✅ installed | Switching 1x ↔ 2x takes ~0.3 s |
| Ultra-wide (IMX481) as 0.5x, with autofocus | ✅ installed | The .5 / 1 / 2 buttons as on an iPhone 11 Pro; no lens-distortion correction yet |

### Not working yet

| Feature | Notes |
|---|---|
| Sound, vibration | Not working on this unit yet |
| Bluetooth | Partial |
| Sensors, auto-brightness, GPS | In progress / not started |
| Cellular | Not started |
| App Store, Apple ID, iCloud, push, FairPlay | Blocked (needs Secure Enclave behaviour) |
| Macro camera (GC2375) | Detected only |
| Portrait mode depth | Needs two cameras streaming at once |

### Known issues

- Real frame rate often ~45 fps instead of 90
- Slow boot from flash: the loader draws its log to the screen character by character
- Camera: switching lenses takes 0.5–1 s; Portrait is listed but has no depth effect
- Setting a wallpaper does not work
- Bold Text in Settings freezes
- Clock about 2 s behind
- Screenshots do not work when booted from flash
- Wi-Fi list incomplete on refresh, no signal bars in the status bar

---

## Building and running

See [docs/devices/instantnoodle.md](docs/devices/instantnoodle.md), [docs/BUILD.md](docs/BUILD.md) and
[docs/INSTALL.md](docs/INSTALL.md). In short: build the kernel and images on your computer, put the phone in the boot
menu's USB developer mode, and run it from RAM with `xl` (nothing is written). Installing to flash rewrites the
phone's `userdata` and removes Android.

> ⚠️ Flashing can leave the phone unusable. Know how to recover it ([docs/INSTALL.md](docs/INSTALL.md),
> [docs/research/boot-chain-and-recovery.md](docs/research/boot-chain-and-recovery.md)) before you install.

## Roadmap

1. The known issues above, sound, vibration, sensors, GPS, Metal, installing third-party apps
2. TrustZone-dependent services, the Titan 480 hardware ISP, camera start-up speed
