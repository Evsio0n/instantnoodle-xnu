# TrustZone-backed key wrapping on OnePlus 8

Publication: 2026-11-17. Device observations below are from 2026-11-11,
RAM boots only. **This is not enabled in Daily 2026-11-15.**

## Implementation

The cellular overlay includes `tools/nf/cell/hk_tz.c`, `hk_keywrap.c/.h` and
`overlay/xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp`. The main-tree
`MiniAppleKeyStore.cpp` remains the original stand-in. `build.py` links the
cellular overlay; `tz_keys=1` opts into the root-key path.

The SCM/QSEE client discovers the already-loaded Qualcomm keymaster64 secure
application and uses a shared-memory bridge for its request/response buffers.
The ordinary kernel buffer experiment crashed; the subsequent bridged,
below-4-GB buffer call returned normally. Both changes were made together,
so that experiment does not isolate which constraint was decisive.

Key generation and use are performed by the vendor keymaster. A bounded time
listener serves the request observed during key generation. The kernel does
not load a replacement secure application or alter the phone's TrustZone
firmware. Secure logs and actual key records are private runtime output and
are not part of this publication.

A random root key is encrypted through keymaster and persisted as a versioned
record under `/private/var/db/hackios-root.tz`. On a later boot keymaster opens
that record. The root key then lives in normal kernel memory for that boot;
this is not an implementation in which each keychain operation stays inside
the secure world. Class-derived keys wrap keychain keys with AES key wrap
(RFC 3394). The old reversible stand-in format remains readable.

The opt-in path may fall back to the old format when no root key is available
or an input length is unsupported. A damaged or refused root record is left
intact; an item requiring an unavailable root key returns “not permitted”.
Consequently this is **not** a fail-closed passcode-backed keychain.

## Recorded hardware validation

| RAM run | Observation |
| --- | --- |
| 357 | Generated the root record; wrapped/unwrapped 32- and 64-byte test keys and system keychain keys. |
| 358 | Opened the earlier record after reboot; unwrapped earlier keys; a keychain item was added, read back and deleted. |
| 359 | A modified record failed authentication and remained unchanged. Earlier root-wrapped keys were refused; the old format remained usable. |
| 360 | A higher configured patch level required upgrade. Keymaster upgraded its wrapping; the root key reopened and earlier wrapped keys still worked. |
| 361 | The upgraded record reopened on the next RAM boot without a further upgrade. |
| 362 | An older configured patch level was refused; the upgraded record was preserved. |

Record upgrade retains the original file as
`hackios-root.tz.before-upgrade`. The new record is written only after
keymaster opens the root key using the upgraded wrapping. These observations
cover the test configuration, not every boot header, firmware revision or
power-loss scenario. `tz_patch=YYYYMM` is a diagnostic override, not a
recommended everyday setting.

## Security limits

- Daily does not enable `tz_keys`; publishing the code does not change the installed keychain.
- There is no completed passcode policy, lock-state enforcement, Gatekeeper attempt counter or fingerprint integration.
- The kernel can access the root key and permits applications to ask the key store; a person able to boot/control this system can read its keychain.
- Legacy items are not automatically migrated. Fallback items do not gain hardware protection.
- Persistent-volume operation, physical power-off between boots, changed boot-image headers and long-term recovery have not been qualified.
- This provides no Apple factory identity, Apple attestation, Secure Enclave replacement, FairPlay support or Apple account/push service access.

## Host checks and dependencies

`clang -DHK_KEYWRAP_TEST -fsanitize=address,undefined hk_keywrap.c` exercises
the FIPS 197 AES-256 example, RFC 3394 vectors, unwrap round trips and negative
cases. Those tests were rerun for this publication. They validate the wrapping
primitive, not the secure world's implementation or the complete security of
the phone. No new phone test or installation was performed for publication.

The transport follows the documented structure of Linux Qualcomm SCM/SMC,
Android QSEECom/shared-memory bridge and CodeLinaro's bootloader Keymaster
client. Vendor firmware, wrapped records, root keys, factory calibration and
secure-world log dumps must be supplied/kept privately; none are shipped.
