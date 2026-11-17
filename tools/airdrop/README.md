## Frozen result — 2026-11-15 (published 2026-11-17)

This directory is experimental source, excluded from Daily. Native Mac small-file
reception and file/hash equality were verified; real iPhone photo finalization
and stable discovery remain unresolved. Earlier entries below are dated research
history, not the current acceptance result. No tests are automatically scheduled.
See [public status](../../docs/STATUS-2026-11-17.md).

# AirDrop on the IN2010 full-desktop RAM image

`build_check.sh` builds the read-only 20F66 front-end check. Run the executable
on the phone with `--lookup` to request Mach port names without sending service
messages. It reports the stock AirDrop Settings resources, sender/receiver
extension bundles, daemon identities and whether en0, awdl0, llw0 and p2p0 exist.
The iPhone10,3 Sharing framework's SFDeviceSupportsAirDrop returns true
unconditionally (0x1a1c42248). A true result establishes no radio capability.

`mode.m` runs as mobile. With no arguments it reads the stock managed-policy
gate, then subscribes only to the same controller-status operation as the
Settings pane. It reports the mode only after a settings delegate callback.
The optional `--saved` checks the mobile user's saved sharingd preference
without starting the controller. A missing preference, restrictions and a
ten-minute Everyone interval can leave the saved and effective states different.

`build_test_app.py` produces an ARM64 ad-hoc signed share-sheet source and a
USTAR with the executable's own entitlements. The single Share test file button
generates a 58-byte text file containing a test prefix and a fresh UUID. It
presents UIActivityViewController with only that local file URL. The sender
shows its SHA-256 in the app and logs it; receiving the same file and checking
its hash is the transfer test. Recipient selection and acceptance are manual.

`root.py` packs the readers and sender into a separate image, verifies the
preserved base, and checks the USB payload budget including 64 KiB overhead.
The first candidate added a normal user-app install to the end of rc.jobs.

* **#415, run 100856.** The device confirmed the six expected daemon paths,
  the six tested bootstrap names, the Settings row and three choices, and the
  sender/receiver extension bundles. The mobile policy allowed AirDrop, but its
  controller returned no mode callback inside seven seconds. The effective mode
  was unknown. en0 was up with IPv6 link-local and none of awdl0/llw0/p2p0
  appeared in the captured interface list. See `out/airdrop/baseline-check.json`,
  `out/airdrop/network.json` and `out/airdrop/mode-baseline.json`.
* **#416, run 103210.** `com.hackios.airdrop-test` registered as a User app
  with a data container, signer identity none and profileValidated=0. Its
  ad-hoc build contained no provisioning profile. Launchd spawned AirDropTest
  at 10:33:16.876804 and the original AirDrop extension at 10:33:21.818283.
  The owner saw “No People Found” on the actual sender panel. No transfer was
  established by these observations. See `out/airdrop/test-app-state.json`
  and `logs/103210-run.log.live.56`.

Use `python3 tools/airdrop/trace.py LOG` on one full or live log for the app
and extension launches, generated-file hash if logged, Wi-Fi virtual-interface
create/delete calls, terminal call counts and radio receive counters. Counts
from overlapping live snapshots must not be added together.
The decoded return log `out/airdrop/observer-run.txt` recorded two observed
radio action frames and zero recognized PSF/MIF frames at teardown. It
contains no 94/95 request record. The default scanner was left unchanged
when the passive observer ran. The two documented launches survive in the
separate live snapshot above. `out/airdrop-ui/416-transport.json` is the
redacted reduction of the decoded return log.


Current native transfer status (2026-11-14): OnePlus→iPhone owner receipt
and OnePlus→Mac file/SHA equality are verified in RAM. Reverse receiving
has reached the original incoming UI and acceptedAsk200 but not a complete
file; MacUpload timedout. See HANDOFF for exact evidence and current run.

`airdrop_native_client --interface-only` remains a bounded540s diagnostic.
`--interface-service` uses the same guarded native CREATE and stock IPv6
configuration but keeps its owned interface until signal, replacement or
provider withdrawal. It does not launch a test App, transmit probe traffic,
change receiving mode or bypass native acceptance. This avoids deletingawdl0
under a live receiver when the old probe timer expires. Interface presence
past that boundary was observed in RAM#529; receiveUpload is still unresolved.
The RAM radio/kernel test has its own finite lifetime, so this does not yet
establish installed daily-use persistence or idle-power behavior.


RAM#530 subsequently verifiedMac→OnePlus originalAirDrop receipt, saved65
bytes in the originalreceivingAppInbox anddirectly matched the Macsource
bybytes/SHA256. Accepted→Upload200 was2.174s. The earlier#529timeout remains
areliabilityfailure; onepassingretrydoesnotprovealltimingissuesfixed. Both
Macdirectionsnowhave file/hashproof; separateiPhonereverse/multi-recipient
cases remainunverified. Exactreceipts/currentpins are inHANDOFF.
