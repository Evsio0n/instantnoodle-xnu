# Host result receipt — 2026-11-07

Commands (repository root):

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/wifi-sleep/test_control.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/wifi-sleep/run.py
```

Both returned exit 0, existing Mac `cc`, address+undefined sanitizers, no installation or kernel build.

- Extracted current real suspend/resume, PCI restore/resumed, `lx_stop` and MHI sync/async adapter function bodies: PASS with mock dependencies. Resume failures retain the published driver and latch ownership; `lx_stop` refuses removal while dirty and permits it after a clean recovery.
- Entire production MHI host + software-device fixture: 30,793 checks, 0 failed in the standalone run. Polling-based counts are not deterministic.
- Added stuck-RESET plus SYS_ERR ownership injection: 32,077 cumulative checks, 0 failed in the source-backed run. Reset timeout returns EIO, SYS_ERR latches dirty ownership, and only a later successful power-up clears it. This still does not prove real reset or DMA quiescence.
- Digest-bound receipt: `run-a6yuz8xp/result.json` (generated, not for staging); generated sources and binaries preserved beside it. Input hashes unchanged before/after run.
- Actual supervisor-authored `control.c` included by the independent host fixture: seven forked command steps. Every one of seven simulated exec failures and seven SIGKILL child failures prevented all later commands (14 cases). At each of three numeric panel checks, sysctl error, short returned size, unknown -1, and opposite valid state all prevented later commands (12 cases). One normal path passed. **execv and sysctlbyname mocked; no /hackios command executed, no actual sysctl or sleep.** No fork failure or waitpid EINTR injection; those paths inspected only. Fixture uses existing Mac sysctl header; this helper test is host-Mac specific.
- Read-only existing page GET `/`: HTTP 200, HTML 4,273 bytes; GET `/status`: HTTP 200, JSON 4,924 bytes. No response data/token retained, no POST, no browser click, no hardware action. Availability only.

## Gate and residuals

**Wi-Fi-on system sleep remains HOLD.** Host contract PASS is neither phone success nor safe crash recovery. A failed resume now retains the driver/controller and refuses removal/free while MHI ownership is dirty; there is still no retained-object power-cycle/restart authority proven on a phone. `lx_pci_restore` restores COMMAND before MSI control in the last pass; this remains tested software ordering, not a hardware sufficiency claim. See README for evidence and residual review.

The supervisor's numeric display checks address the reported `waitdisplay` unknown-as-off weakness; host failure-order tests confirm those explicit checks fail closed, not that the sensor/sysctl or sleep actually works on iOS. Only the controller may publish or operate the existing page's Wi-Fi-OFF control with its own safety review.

The authored host-contract files are `run.py`, `contracts.c.in`, `reset.c.in`, `test_control.py`, `README.md` and `RESULTS.md`; generated run directories remain untracked. `control.c` and `prepare_control.py` are supervisor-owned, read-only reviewed. No phone, installation, partition write or commit was performed.
