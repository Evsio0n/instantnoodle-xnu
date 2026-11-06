# An .ipa on the phone with no installd (T38 item 13)

The owner: 「可不可以幫我裝ipa，比如telegram」, of Telegram's own GitHub release (which it calls "an unsigned build"; the file is in fact signed, below). **Status: built and tested on the Mac; not yet run on the phone.** Tags: [source], [observed], [inferred].

## The way
`userland/src/hkinstall.m`, `userland/src/hkinstall.entitlements`, `tools/ipa/pack.sh`, `tools/ipa/serve.py`, `tools/ipa/try.sh`; `tools/nf/mk_extra.sh ipa` puts it in a root.
1. `pack.sh` turns the .ipa into a ustar archive on the Mac: the `.app` unchanged, and each binary's entitlements as its signature lists them, as plists beside it. It refuses an encrypted binary.
2. A one-file HTTP server on the Mac, bound to its address on the local network, serves that for one ticket and stops. On the phone `hkinstall fetch` gets it over Wi-Fi (`fw/network` joins by itself, run 093102) and checks its SHA-256. The app does not fit the loader's 63 MiB window.
3. `unpack`: into `/var`, in RAM for a first proof, with `/var` raised by `NF_VAR_MB`.
4. `install`: a bundle container and data, plugin and group containers from `containermanagerd`, then `registerApplicationDictionary:` with the app and its extensions. This follows TrollStore's `RootHelper/uicache.m` [source].

**Not installd:** it checks signatures and profiles in user space, where the kernel's AMFI stand-in has no say [inferred; TrollStore exists for that reason]. Nothing hooks it.

**Not modified:** no byte of the app, no re-signing. The kernel holds whatever entitlements the signature lists, and has no signature policy (K13, `kernel-standins.md`) [source, ours].

**What the description leaves out:** TrollStore's also says `SignerIdentity` "Apple iPhone OS Application Signing", `SignerOrganization` "Apple Inc." and `IsAdHocSigned` true. Those are local fields in LaunchServices' database, and no Apple server sees them, but they are not what this file carries. **So they are off by default.** `hkinstall claim` (`HK_CLAIM=1` for `try.sh`) adds them, only if a launch is seen refused for want of them, and the report says so (the supervisor, 2026-11-06).

## The build tried first [observed from the file]
Telegram's GitHub release `build-26855`, 10.0.3 of September 2023, 81788822 bytes, SHA-256 `ed0681d8...`. All 46 Mach-O files `cryptid 0`, signature "Apple Distribution: Telegram FZ-LLC (C67CF9S4VU)", 6 extensions, `application-groups` and `aps-environment` among its entitlements. Its `embedded.mobileprovision` is an App Store profile that **expired 2023-10-27** (the supervisor's check). What reads it: installd's checks, which this path does not go through; the kernel has no signature or profile policy (K13). Whether FrontBoard or anything else on this path looks at it is not known: the first launch says [inferred]. **`~/Downloads/Telegram_12.9.2_*.ipa` is not to be used:** a third-party rebuild with injected libraries (the supervisor's check).

## Tested, on the Mac only [observed]
Fetch of 143523840 bytes with the right hash; a wrong hash and a wrong path refused and nothing kept; unpack identical to `tar -x`, 764 files, the signature still valid; a name going out of its directory refused.

## First boot on the phone (run 122900; kernel mdp-93fe105e, the row's root plus `hkinstall`; RAM `/var`, 1536 MB; no signer fields; not logged in) [observed]
- **Fetch:** Wi-Fi joined by the file; 143523840 bytes in 6 s, hash right. The Mac's server served once and stopped.
- **Registered, icon appears:** LaunchServices took it: type User, installed, 6 extensions, a data container. Telegram's icon is on the home screen.
- **Does not launch:** `openApplicationWithBundleID:` returns no, and SpringBoard shows **"Telegram" Is No Longer Available**. No process was started, so it was never loaded or drawn.
- **Why is not in the log.** The system's logs are off, so nothing states a reason. Other apps work; iOS's cache has `ProfileValidated`, `SignerIdentity` and a trust state [observed strings]. So a trust check on the missing signer fields, or on the profile that expired 2023-10-27, are the candidates [inferred].

**Second boot, with the signer fields and `applog=1 applog_name=lsd,Spring` (run 123316): no answer, not a try.** With those daemons' system logs on, the home screen took 388 s against 155 s, and the first two minute readings were 82.6 and 82.2 C (no throttle; later ones 47.7 to 50.8 C), so the run's 400 s ended while the fetch ran. Those logs are not cheap here. `hkinstall open` now asks FrontBoard's own service, which answers with a reason.

**Third boot, with the signer fields, no system logs (run 124145) [observed].** These fields say Apple's own signature; the file's real signature is Apple Distribution. Fetch and registration as before; LaunchServices then reports that signer, `profileValidated` 0, restricted 0.
- **A program's request to open is refused** by FrontBoard's service: `FBSOpenApplicationServiceErrorDomain 1`, denied by `SBMainWorkspace` for reason Security: "Unable to launch ph.telegra.Telegraph because it has an invalid code signature, inadequate entitlements or its profile has not been explicitly trusted by the user".
- **A finger on the icon starts it:** touches at 12:42:49 to 51, nobody's script, one exactly on Telegram's icon; launchd: `Successfully spawned Telegram[226] because launch job demand` at 12:43:00.
- **Drawing is not known:** 11 s later the screen still showed the home screen, with no alert; no failed lookup from it in the log. The run ended 18 s after the spawn, and that ended it (`backboardd respawn`). The job did not wait long enough.
- Whether the signer fields are what let the finger through is not shown: no finger touched it in run 122900.

**Fourth boot, the job's own tap, 100 s (run 124448), same file and fields [observed]: Telegram did not start, by my job's order.** `open` ran first and was refused as before, and the refusal puts up the alert "Is No Longer Available". The job's tap came 3 s later, with the alert up [inferred from the order and the shot]. About 50 touches the job did not make followed; they fit OK, long presses on the icon, then Remove and Delete [inferred]. At 12:46:41 `pkd` removed the extensions and the 12:47 shot has no icon. launchd never logged the app's process. So `try.sh` now taps first, and asks `open` after the last picture.

## Not known
Whether `containermanagerd` and `lsd` take this here; whether FrontBoard lets it launch; whether it draws; what its extensions do when loaded; what Telegram's servers say. One RAM boot answers most of it: `tools/ipa/try.sh`, with screenshots of the home screen and of its welcome screen, not logged in.

## What will not work, plainly
- **Push:** `aps-environment` is there and `apsd` has no courier (K11): messages come only while it is open and connected.
- Sign in with Apple, iCloud and CloudKit, Apple Pay, CarPlay: no account or service behind them. Calls, Siri intents, notification and widget extensions: unknown.
- **Login may be refused.** No official statement or minimum version was found. In 2024 servers answered old clients with `UPDATE_APP_TO_LOGIN` and `API_64BIT_LOGIN_APP_OUTDATED` (TDLib issue 2947; a Purism forum post) [source], and those were much older than this. Only a login attempt says, and that is the owner's to make.
- **The keychain protects nothing here (K13):** "wrapped" reversibly, and K13 says not to use this device for secrets. A Telegram session on this phone is not protected as on a real iPhone. On a RAM `/var` it is gone at each run's end. Writing it to the kept `/var` needs the owner's own word.

## Keeping accounts out of logs
`hkinstall` prints steps, counts, sizes, paths under `/var/containers` and results, and reads nothing in the app's data container. The archive and the .ipa stay in `out/`, untracked.
