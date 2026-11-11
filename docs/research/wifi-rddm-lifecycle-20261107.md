# Wi-Fi RDDM / suspend-resume lifecycle review — 2026-11-07

This is a host-side review only. It does not start a phone, post to the 8765
page, or claim a Wi-Fi sleep fix. The comparison uses the checked-in port under
`wifi/lx/src` and the local Linux 7.3.1 ath11k/MHI source at
`linux-7.3.1` in another workspace on the development machine (outside this repository).

## Difference table

| Phase | Mainline ath11k/MHI behavior | Current port | Consequence for run 105300 / next gate |
| --- | --- | --- | --- |
| Normal suspend | `ath11k_core_suspend_late()` disables HIF and CE IRQs, then calls `ath11k_hif_power_down(ab, true)` (`drivers/net/wireless/ath/ath11k/core.c:1267-1283`). `ath11k_mhi_stop(is_suspend=true)` uses `mhi_power_down_keep_dev()` so resume can reuse the channel devices (`drivers/net/wireless/ath/ath11k/mhi.c:455-466`). | `lx_suspend()` calls the same two core callbacks (`wifi/lx/src/lx_bus.c:1013-1033`). `pci_sleep_now()` then drops QRTR, drains the link worker, powers the PCI function off, and sets `pci_slept` (`xnu/files/bsd/dev/hk_wifi.c:4637-4651`). | The sleep-side order is close enough for a host contract. Keep this path unchanged while reviewing the failed-wake path. |
| Resume success | `ath11k_core_resume_early()` reinitializes the restart completion and powers HIF up; `ath11k_core_resume()` waits for the restart worker (`drivers/net/wireless/ath/ath11k/core.c:1287-1305`). | `lx_resume()` restores PCI config, calls early and late resume, and returns success only when both pass (`wifi/lx/src/lx_bus.c:1037-1054`). | The success order is covered by `tools/wifi-sleep/run.py`, but this is software ordering only; it does not prove link, SMMU, or firmware recovery. |
| Resume failure / RDDM | Mainline crash recovery keeps the device/driver object and performs a reset cycle: after recovery preparation it disables HIF/CE IRQs, calls `ath11k_hif_power_down(ab, false)`, then `ath11k_hif_power_up(ab)` (`drivers/net/wireless/ath/ath11k/core.c:2533-2605`). It does not turn an RDDM event into PCI removal. | `lx_resume()` calls `lx_stop()` on either resume error (`wifi/lx/src/lx_bus.c:1046-1050`). In the outer wake path, `pci_slept` is cleared before resume, then an error enters `pci_cleanup_locked("wake retry", false)` (`xnu/files/bsd/dev/hk_wifi.c:4654-4671`); that cleanup invokes the versioned diagnostic removal before attempting one fresh start (`xnu/files/bsd/dev/hk_wifi.c:4421-4443`). | This is the key semantic mismatch. An RDDM followed by removal can run ath11k teardown against a device whose DMA/firmware state is not known to be quiescent. **Do not run Wi-Fi-on sleep until this branch has a host ordering contract.** |
| MHI reset during power-down | Mainline MHI transitions its PM state, flushes the state worker, and only then disables the controller IRQ (`drivers/bus/mhi/host/pm.c:1231-1281`). Recovery code powers down, restores PCI state, checks the device, and powers up again (`drivers/bus/mhi/host/pci_generic.c:1273-1294`). | `mhi_host_power_down()` writes `MHICTRL.RESET`; if the reset times out it logs only, then marks every channel stopped and clears `powered`, mission and doorbells (`wifi/lx/src/mhi_host.c:967-984`). The API is `void` (`wifi/lx/src/mhi_host.h:132`), and `mhi_power_down()` discards the result (`wifi/lx/src/lx_mhi.c:366-375`). | A stuck reset is indistinguishable from a clean reset to the caller. The existing `reset.c` fixture proves the analogous power-up timeout; it does not cover this power-down ownership loss. |
| Removal / memory release | Mainline `ath11k_pci_remove()` cancels reset/dump work, runs core deinit, then destroys firmware, unregisters MHI and frees IRQ/MSI/PCI resources (`drivers/net/wireless/ath/ath11k/pci.c:1177-1207`). | `lx_pci_remove()` calls the driver's remove callback and unconditionally clears `radio_probed` (`wifi/lx/src/lx_pci.c:838-846`). `pci_cleanup_locked()` then stops the link, powers the radio off, and may release SMMU/IOVA ownership (`xnu/files/bsd/dev/hk_wifi.c:4424-4441`). | There is no explicit MHI-reset-clean/dirty result reaching `pci_cleanup_locked()`. A failed reset therefore cannot fail closed before the radio mapping is released. |
| PCI configuration restore | Mainline PCI PM restores saved state before the MHI/driver resume path. | `lx_pci_restore()` writes ordinary shadowed words first, then COMMAND and MSI control (`wifi/lx/src/lx_pci.c:144-171`); the host contract checks only this software order. | Keep as a separate review item. Register accessibility, posted writes, interrupt masking, and SMMU/iATU readiness remain unproven. |

## Next executable host-only point

Add a fault-injected software-device contract before touching the phone:

1. Boot the existing `tools/mhi-test/test_mhi.c` fixture to mission.
2. Make the fixture leave `MHICTRL.RESET` asserted during **power-down** (the
   existing `tools/wifi-sleep/run-a6yuz8xp/reset.c` already injects this on
   power-up).
3. Assert that the production host reports a dirty/failed reset to its caller;
   a clean-looking `powered=false` result must not be accepted as proof that
   the device stopped DMA.
4. Exercise the failed-resume ordering at the port boundary: the recovery
   action must be a power-down/reset plus power-up attempt while the driver
   object and DMA ownership are still retained. Removal/SMMU release must be
   refused until that ownership is explicitly clean.

The smallest reviewable implementation is now in
`wifi/lx/src/mhi_host.{h,c}` and `wifi/lx/src/lx_mhi.c`: `mhi_host_power_down()`
returns `EIO` and marks DMA ownership dirty when `MHICTRL.RESET` times out;
`mhi_ownership_clean()` stays false until a later power-up succeeds. The outer
`lx_bus.c` cleanup gate refuses driver removal/SMMU release while ownership is
dirty. This is a host ordering guard; it does not claim that firmware recovery is
safe until the phone proves it.

## Host evidence run for this review

The existing source-backed contracts remain the only executed validation:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/wifi-sleep/test_control.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/wifi-sleep/run.py
```

These cover extracted `lx_suspend`/`lx_resume`, PCI restore, MHI power-up
errno propagation, and a stuck-reset **power-down plus recovery** fixture. The
latest run reports `mhi-test` 26,197/0, `run.py` 25,993/0 and the reset-timeout
contract 26,814/0 under ASan/UBSan. They do not run `pci_wake_now()`, ath11k
removal, real firmware, or a phone. Wi-Fi-on sleep therefore remains HOLD.
