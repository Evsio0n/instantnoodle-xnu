# Daily 2026-11-15 — OnePlus 8 / instantnoodle

Installed to UFS bundle slot1 at the owner's explicit request on2026-11-15.
Current authoritative receipt: `out/daily-installed.json`.

## Contents and validation

- Known C533 cellular/ALS/native Settings lineage; fresh control reproduces C533 exactly.
- Daily kernel adds boot-scoped calibration completion for kept HFS data. A previous boot's marker cannot authorize this boot's acquisition. The source persist partition is opened read-only; its exact calibration digest and stable blocks are still verified.
- Owner-accepted root8013 includes the known bootfix init, normal system apps, Wi-Fi/Bluetooth, media/cameras, native carrier/No-SIM Settings, and TrollStoreLite bootstrap.
- Daily root changes only the calibration helper and job list, removes the diagnostic light observer and bundled iSH testIPA. The installer remains; no existing user-installed apps/data are removed.
- No AirDrop experiment or upload observer is included. System sleep remains disabled as in the known daily baseline, pending Wi-Fi/power qualification.
- `var_data_format` is absent. Kept HFS was mounted and verified, never formatted. Existing `boot_a`/`boot_b` and ABL slot selection were not written.

Host validation: fresh baseline SHA match; session-record ASan/UBSan; actual generated reader-scope ASan/UBSan; root entry readback; bundle header/payload SHA; tools/t host5/5.
Hardware validation: kept-data preview showed native lock screen, owner accepted and requested install; fresh factory read, current-boot marker, accepted optical samples and smooth DSI DMA writes; native installer registration. Installer wrote/readback slot1; original slot0 metadata stayed the same. Actual UFS boot log confirms newgeneration and bootok; calibration freshly acquired again after installation.

The final installed profile has no time limit and no USB control agent. A dark screenshot after idle is not a request to stop the running installed system. Do not auto-run `xl stop` or rearm a RAM test.

## Artifacts

`out/daily-20261115/` contains kernel/root/bundle receipts, exact argument lines, built `daily.img`, preview evidence and installed boot log. The image contains private handset firmware/network configuration and stays0600; do not publish it.

The legacy `out/cell-kernel-installed.txt` remains the immutable November11 control for older builders. It is not the current installed pointer. New current pins are `out/daily-{kernel,root,bundle}-installed.txt`; see `out/daily-installed.json`.

This directory's builders prepare this release only. Building does not install. Any future UFS write needs the owner's new authorization and serialized bootq installation.
