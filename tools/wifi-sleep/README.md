# Wi-Fi sleep: source-backed host contracts, not phone validation

## Reproduce (no installation, no kernel build)

From the repository root, with the existing Python 3 and C compiler:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/wifi-sleep/run.py
```

The runner makes an absent unique `run-*` directory **inside this directory** and preserves it. Compiler `TMPDIR`, generated sources, binaries and evidence are there. It deletes nothing, invokes no existing runner, and contacts no device or network. ASan and UBSan are enabled and errors fail the run. `CC` may select an already-installed compiler executable (not shell flags). No packages are installed. Do not stage generated binaries/directories; whitelist ignores can hide all new files, so the controller must deliberately name the authored source/document files if later authorized to stage.

## What is actually exercised

This is not a parallel hand-written implementation claiming the driver passes. `run.py` extracts these **current real C function definitions verbatim** and compiles them with deliberately small mock dependencies:

- `wifi/lx/src/lx_bus.c`: `lx_suspend`, `lx_resume`, and `lx_stop`.
- `wifi/lx/src/lx_pci.c`: `lx_pci_restore`, `lx_pci_resumed`.
- `wifi/lx/src/lx_mhi.c`: `mhi_sync_power_up`, `mhi_async_power_up`.
- `wifi/lx/src/lx_base.c` is hash-bound because its DMA free path now refuses
  to recycle radio memory while MHI ownership is dirty; that path is reviewed
  statically and is deliberately not executed by the mock harness.

The extraction lexer counts braces outside C strings/comments, requires one definition, and fails on missing/ambiguous definitions; it is not a full C parser. Compilation fails rather than silently falling back to a model when interfaces change. The result hashes source inputs before/after to reject a concurrent source change. The templates contain assertions and mocks, not replacement bodies for those selected functions.

Assertions cover:

1. Resume restores PCI configuration before early resume; success does not stop; early and late negative errors latch ownership, retain the mocked driver publication and become positive errno; early failure skips late resume. No-driver/non-PCI no-op.
2. Suspend rejects a running radio and non-PCI; short-circuits early failure; propagates late error; accepts the successful mock path.
3. PCI restore first writes ordinary marked halfwords, then COMMAND/MSI control. Low-only and high-only shadow bytes preserve their unmarked live neighbor; untouched words stay untouched; absent MSI capability and unprobed device covered. **Current code writes COMMAND before MSI control in pass 1.** This suite does not assert that ordering is safe on hardware: only ordinary address/vector fields precede both enable words.
4. Positive errors from the MHI host become negative Linux errors, QRTR is detached and polling stopped on error, async delegates to sync, unallocated controller rejected.
5. `lx_stop` refuses removal while the MHI ownership latch is dirty and proceeds only after the latch is clean.

Separately `reset.c.in` includes the existing `tools/mhi-test/test_mhi.c` software-device fixture but **never invokes its runner** (which deletes an external build directory). It links the entire real `wifi/lx/src/mhi_host.c`, runs the existing fixture suite, and overrides only the device MMIO-write callback for a stuck RESET. The new case requires production power-up to return `EIO`, make no BHI image request, leave `powered=false`, and unlock. The real driver is under test; the QCA6390 is still a **software device model, not firmware or phone evidence**. Existing fixture check counts vary with polling/time.

The `lx_stop` mock supplies only the station/removal callbacks and asserts that a dirty
ownership latch retains the published driver; the clean case then permits removal. This
does not prove DMA is quiet, cleanup/retry is safe, or that ath11k's full teardown is
recoverable. Kernel `hk_wifi.c` is hashed as context only; its power worker,
cancellation/teardown concurrency and full ath11k are not executed.

## Risk gate: HOLD, even when all host tests pass

Known phone evidence in `docs/KNOWN_ISSUES.md` K05 / `docs/TODO.md` T02: run 105300 crashed after mission-mode resume and cleanup could not prove DMA ownership, requiring cold recovery. `docs/subsystems/wifi.md` still says sleep was not tried: stale; prefer K05/T02. These tests do not erase that failure.

Before the controller offers a Wi-Fi sleep scenario on the existing 8765 page:

- Preserve normal page behavior; do not replace/start a server. This subagent only checked read-only GET `/` and `/status`; no `/go` POST, no boot, no click.
- Identify the exact kernel/source generation; match it to successful host evidence. A host PASS is not kernel-built or phone-tested.
- Review failed-resume ownership: `lx_resume` now retains the published driver and
  returns the error to `pci_wake_now`; the outer retry must gain an explicit serialized
  power-cycle/restart authority before it can safely start again.
- `mhi_host_power_up` and power-down failures, SYS_ERR/RDDM and inaccessible registers now latch dirty ownership; software `powered=false` remains neither an observed reset nor proof DMA stopped. The adapter retains the controller and refuses removal/free until a later successful power-up clears the latch. There is still no phone-proven retained-object recovery path.
- PCI restore software ordering does not establish register accessibility, host link/SMMU/iATU readiness, interrupt masking, posted-write completion or firmware reuse safety. Hardware readbacks/recovery authority remain controller-owned.
- A future phone run needs owner present, fresh explicit RAM-only scenario authorization, boot queue ownership, real sleep enabled (not `syssleep=0`), known-good `wifi=0` control, private RAM credentials, bounded duration, joined-before and traffic/rejoin-after evidence, panic retention and cold-recovery plan. No repeated blind retries; no install/flash from this suite.

### Minimum suggestions for the controller (not implemented here)

1. Capture the actual panic/removal failure before treating fresh-start fallback as recovery. A failed resume now returns a typed error while retaining the driver and ownership; implement a serialized power-cycle/restart authority before allowing a second start.
2. Keep the dirty latch through every upstream void teardown boundary. The host tests prove the fail-closed state and recovery clear, but do not prove hardware DMA quiescence or a phone wake.
3. If changing PCI enable order, keep BAR/MSI-address/vector programming before enable and independently validate interrupt masking/COMMAND-versus-MSI control requirements; this test records current COMMAND-first, not a hardware requirement.

## Results

2026-11-06, Mac host, existing `cc`, ASan+UBSan:

- Initial harness run failed an assertion because two fixture byte marks occupied the same halfword; corrected the test vector to use distinct halfwords. Not a production defect. Failed generated evidence remains in its own run directory.
- Subsequent runs passed extracted contracts and full real-MHI software-device suite plus stuck-RESET fault. See `RESULTS.md` for the final run receipt.
- Read-only existing page: GET `http://127.0.0.1:8765/` -> 200 HTML, GET `/status` -> 200 JSON. Response contents/token/state were not copied or changed. This is service availability only, **not browser-rendering or Wi-Fi sleep validation**.
- No changes to kernel/driver/docs outside this owned directory, no commit, no installation, no hardware commands, no kernel build. Existing destructive host runners and `tools/t host` were intentionally not used under this narrower write boundary.
