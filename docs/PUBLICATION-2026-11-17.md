# Source publication review — 2026-11-17

This follow-up synchronizes source after the initial documentation-only update.
It does not boot, install or modify a phone.

## Included

- Cellular SIM state handling and native Settings carrier row.
- Ambient-light acquisition, curves, preference bridge, smooth display updates and daily boot-scoped calibration completion.
- Cellular TrustZone/keymaster transport, AES key wrapping, AppleKeyStore overlay and upgrade handling. Opt-in RAM experiment; not enabled in Daily.
- Userland rendering, installation and system-integration changes.
- AWDL transport and native AirDrop diagnostic/receiver source, explicitly experimental and excluded from Daily.
- Minimal pinned ALS baseline and read-only calibration parser source, so the host tests do not require the original audit tree. The historical cellular baseline was checked byte-identical to public commit `e33c90b` before replacing the private commit reference.

## Excluded

Private loader owner-key headers; firmware and Apple files; calibration values
and receipts; secure-world logs and root/wrapped-key records; Wi-Fi credentials;
phone dumps, screenshots, captures, logs and installed images; whole audit,
backup, build-object and donor checkout directories. Donor source remains a
pinned download through `fetch-ref.sh`, under its existing licence and credit.
No directory deletions from the local migration were propagated.

## Screening

- Source candidates were fetched into a separate staging directory and matched against source SHA-256 before publication edits.
- The explicit staged path list passes `tools/publication/check.py`: no denied private/generated path, key header, binary content, symlink, local home path or high-confidence credential pattern.
- Gitleaks **8.30.1**, with its downloaded release checksum verified, scanned the source tree. Two findings were reviewed: a prose algorithm name in the pre-existing `README.hackios.md` and a 64-hex source SHA-256 in `als/sources.json`. Neither is a credential. No unresolved credential finding remains.
- The private-path/network literal review found no user home path, RFC1918 address, MAC literal or quoted credential assignment in the incoming source set. The IPv6 literals found were test fixtures and the standard link-local router example.
- The staged diff preserves original pinned source and patch whitespace; changing it would invalidate source hashes or historical patch context. It is not a whitespace-normalization commit.
- The final staged-file gate and a scan of the outgoing commit range are checked before push. Screening reduces publication risk; it is not a formal proof that arbitrary source cannot encode a secret.

## Validation rerun on the host

- `tools/t host`: **5/5**.
- ALS curve/input/handoff/SMMU and stream-lifetime tests passed; includes 50,000 front reports and 10,000 listener replies.
- SIM decoder/refresh tests passed; regenerated source exactly matched the published driver, using the pinned upstream reference download.
- Native carrier row: **14 assertions**.
- Calibration receipt reader and boot-session completion: ASan/UBSan passed.
- AES-256/FIPS 197 and RFC 3394 wrap/unwrap/negative vectors: ASan/UBSan passed.
- AirDrop receiver ownership and real CF memory-stream/EOF/header-whitelist conformance: ASan/UBSan passed.
- Incoming Python source syntax passed (148 files).

No new hardware acceptance or complete kernel/root rebuild was performed for
this publication. Existing build scripts still require generated linker
objects, the private SDK/build environment and per-device inputs. Some AWDL
historical replay scripts require their original build receipts/container
stages, which are intentionally not published. The tests above do not turn
AirDrop or the TrustZone experiment into a qualified installed feature.
