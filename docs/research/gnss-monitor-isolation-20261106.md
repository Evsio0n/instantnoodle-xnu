# GNSS monitor isolation — 2026-11-06

## Host workflow and USB access

The Mac workflow page is `python3 tools/metal/loop/eye.py`, bound to
`127.0.0.1:8765`. The phone's `hkhttpd` uses the same address on the phone;
these are different loopback interfaces. The host page was restarted and
both `/` and `/status` returned HTTP 200. Its queue showed the actual new tickets.

The restricted execution environment returned an empty `fastboot devices`
and `xl state=absent`, while an unrestricted read immediately returned
`xnuloader fastboot` and `loader`. Treat this as an access failure, not evidence
that the owner disconnected the phone. Ticket #137 ended without a new run.

## Correcting the proposed headless control

`tools/nf/boot.sh` accepts a JOBS argument but only prints it; the job list must
already be in the APFS image. The earlier `work/gnss-headless-run.sh` still used
`out/nf-gnss-web-root.apfs`, whose `/etc/rc.jobs` starts hkhttpd and Safari.

An isolated copy of `out/nf-root` was made at `out/nf-gnss-headless-root`.
Only `/etc/rc.jobs` was changed to the two waits in `out/rc.jobs.gnss-headless`.
The runner now uses `out/nf-gnss-headless-root.apfs`. All 572 paths in the original
and new APFS images were compared with the repository's read-only APFS reader:
file modes/owners and every content item matched except `/etc/rc.jobs`.
There is no second launchd entry starting hkhttpd or opening the GNSS page.
This control still runs SpringBoard and the ordinary system services.

## Observed headless result

Ticket #138, run `200815`, completed normally with rc 0 and returned to the
loader after 186 seconds. Kernel SHA-256 starts `c237217b`; root SHA-256 is
`bf5da04cbdf974d660036964c8cdd14796002669d66efc61e8110c7ec80a398f`.
The image was booted in RAM with `ufs_write=0`, `var_store=0`, `hk_data=2`,
Wi-Fi/audio/ADSP/haptic/camera/Bluetooth off. No install or flash was performed.

- Sahara completed 15 images / 87 MB. QMI LOC was available at node 3, port 84.
- Mission status remained 1 and errfatal 0 through 170 seconds.
- EFS channel 11 delivered HELLO and a 52-byte table with one entry. The
  read-and-discard exchange completed and the modem answered RESET.
- GNSS position report count reached at least 120; the reports still used zero
  satellites. No first-signal or first-fix marker was observed. This is not an
  outdoor reception test and does not establish an RF hardware failure.
- No hkhttpd requests appeared. The job reached `t-mark: done`.
- Minute samples were 44.9, 41.8 and 42.2 C, with zero throttling. The separately
  logged core peak was 77.9 C.

Evidence: `logs/nf-gnss-headless.txt`, the preserved `logs/200815-run.log.live.*`
snapshots, and the bootq ticket receipt. The final run log alone is truncated;
do not infer missing events from that file or reuse it for a later ticket.

## Lower-frequency phone page

`tools/nf/gnss-monitor/index.html` now waits 10 seconds after a completed request
before polling again, permits one request at a time, aborts after 8 seconds,
pauses while hidden, and exposes manual refresh and the last successful update
time. The no-JavaScript iframe probe is inside `noscript`, so it does not add a
second connection while JavaScript runs. The server and kernel are unchanged.

JavaScript syntax and a request-lifecycle smoke check passed. `tools/t host`
passed 4 of 5 checks; the screenshot self-test could not start because this Mac
lacks `nix-shell`.

The isolated low-frequency image is `out/nf-gnss-lowpoll-root.apfs`, SHA-256
`a1868397739672950c302c1ea65a478050fbe3e0a017a713063803ef73b8c853`.
All 572 paths were compared against the original web image: only the monitor
HTML differs. Its jobs still start hkhttpd and open Safari after 45 seconds.

The earlier correlation between Safari/polling and silence is a hypothesis.
The GNSS sysctl handler reads counters and formats a short string; source review
found no modem transaction or wait in that handler. A headless pass alone does
not identify the cause of the earlier whole-system failures.

## Historical generic-seam discovery run (superseded by `015925`)

Run 010125 (bootq #150, November 7, 2026) used the rebuilt
`out/nf-gnss-web-root.apfs` and the private NF kernel. It completed Sahara's 15
images, held mission status for 170 seconds, and emitted both
`modem: qrtr adapter: first packet accepted` and
`modem: qmi loc: generic service at node 3 port 83`. The phone's own `hkhttpd`
served the current 5993-byte page and multiple `gnss.json` requests. The legacy
LOC transaction path supplied this historical run's requests and indications, so
it proves the adapter lifecycle and generic service discovery only. Generic LOC
table encoding and indication handling were phone-proven later in `015925` (#156).
GNSS reached 120 reports with zero satellites and no fix.

The same run completed the separate EFS-Sahara channel-11 read-only
HELLO/table/read/RESET exchange, reached `t-mark: done`, and returned normally.
The follow-up headless run 010452 (bootq #151) reproduced the known pre-mission
Sahara RDDM case after only images 37 and 40; it never reached mission, QRTR or
GNSS and is not a counterexample to the adapter result.

## Low-frequency web result

Ticket #139 used the low-frequency root and the same RAM-only modem arguments.
It returned normally with rc 0, but it is **not a valid GNSS web comparison**:
the modem entered Sahara RDDM while starting (`image 37`, `image 40`, then
Sahara mode 2) before the mission, QMI LOC, EFS or GNSS counters began. The
phone's ordinary iOS services continued and `hkhttpd` started, but no GNSS
feed was available. This is a separate modem-start failure, not evidence that
the 10-second page caused or fixed the earlier silence. Do not retry blindly;
the next modem run needs a fresh diagnosis of the warm-reset/startup state.

## Generic LOC phone proof after table migration

Run `015925` (bootq #156, November 7, 2026) used the rebuilt web root and kernel
SHA-256 `787a859873438431d64e6acd543e4bd73b473555e2344da756b94aeee719fe68`.
It completed Sahara's 15 images / 87 MB, kept mission status 1 with `errfatal=0`
for about 170 seconds, and discovered LOC service 16 at node 3 port 84. The log
contains `generic GNSS start accepted (session 1)` followed by 120 position reports,
so the generic request, response and indication tables are phone-proven. The run
was indoors and every report had zero satellites and no fix; this remains an RF
test gap, not a transport failure. The separate EFS-Sahara channel-11
HELLO/table/read/RESET exchange also completed, and the phone returned normally.

The next GNSS run should happen only while the owner is outside and watching the
on-phone `127.0.0.1:8765` counter page. Keep the same RAM-only arguments and do
not change the monitor polling policy at the same time; otherwise a satellite/fix
result cannot be attributed to the modem path.

## Radio-query attempt with the wrong default — ticket #161 (2026-11-07)

The first run containing `modem_radio=1` entered the phone with kernel
`c655aa75`, but its command line did not include `wifi=0`. The OnePlus default
therefore logged `wifi=1`; this is visible in `logs/nf-gnss-radio.txt` before
the modem boot. The run reached the modem MHI boot-loader stage but produced no
mission, QMI LOC, GNSS or DMS/NAS/WDS result before the existing Wi-Fi dirty
ownership path halted loader return. Bootq recorded `rc=76` and a cold-recovery
hold. This ticket is invalid evidence for RF, GNSS or radio-service presence.

After the owner performs a cold reset, repeat only with the explicit `wifi=0`
override. The read-only radio code itself remains host/build tested and
phone-unproven.

## Outdoor-run attempt after generic LOC proof — run `023740` (2026-11-07)

The notified RAM-only run used the already phone-proven generic LOC kernel
`787a859873438431d64e6acd543e4bd73b473555e2344da756b94aeee719fe68`, the web root
`out/nf-gnss-web-root.apfs` (SHA-256 prefix `6c83a91f`), and the same isolated arguments:
`modem=3 modem_mission=1 modem_loc=1 modem_gnss=1`, with Wi-Fi, ADSP, sound, haptic,
camera and Bluetooth off, `ufs_write=0` and `var_store=0`.

The phone returned normally after 186 seconds and reached `t-mark: done`, but this
boot never reached mission. The SDX55 accepted Sahara image 37 (`multi_image.mbn`)
and image 40 (`apdp.mbn`) and then offered Sahara mode 2 (RDDM):
`the device has crashed and offers its memory: not taken`. No MDM2AP mission status,
QRTR service discovery, EFS exchange, generic LOC Start or GNSS position report was
observed. `hkhttpd` did start and the phone served `/gnss.json`, so the on-phone page
path itself was present. This is the known pre-mission RDDM state seen after a prior
mission, not an outdoor RF result and not evidence against the generic LOC path.

The run ended with the normal modem warm-reset cleanup and the phone returned to the
resident `xnuloader` loader (`./iphont state`: `state=loader`, no run held). A fresh cold-reset/startup state is required before repeating
the outdoor RF run; do not retry this exact state blindly or interpret its missing
satellites as a receiver failure.

## November 10 private measurement

The IN2010 NF source now accepts `modem_gnss=2` with explicit Wi-Fi off. It filters an accepted generic LOC session by the actual peer and destination port, session ID, successful measured status, latitude/longitude range, circular uncertainty, satellite technology and recent UTC. Injected and reference-only location reports cannot produce a validated satellite fix. Coordinates occupy one volatile kernel slot with a 15-second age bound; the public quality ABI exports state, count, uncertainty and age. Timestamps and coordinate values are excluded from the host transcript. This profile was compiled and host-tested only.

The native app uses the public CLLocationManager delegate and the system When In Use authorization prompt. Apple's client API does not identify Wi-Fi versus satellite delivery from a normal callback. No public iOS 16.5 API for publishing this modem as a system-wide locationd provider was identified. The hardware ticket therefore records modem evidence and public Core Location evidence separately. [Apple authorization guidance](https://developer.apple.com/documentation/corelocation/requesting-authorization-to-use-location-services), [QMI LOC wire types](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/3454c2c619d516fd46a08fe16c6eebdee061857d/data/qmi-service-loc.json), [Qualcomm field units](https://android.googlesource.com/platform/hardware/qcom/gps/+/android-4.2.2_r1/loc_api/loc_api_v02/location_service_v02.h).

## November 10 launch attribution

**Observed on phone.** #228 / 052622 validated the kernel, app bytes visible under /Applications, 38 quality samples, an accepted LOC session, 154 reports and zero fresh satellite fixes. The private SpringBoardServices launch request returned 9. No foreground scene or Core Location callback was recorded. The original test never queried the installed app proxy.

**Observed in retained artifacts.** The actual 20F66 SpringBoardServices error-string function identifies error 9 as “security policy error”. The launcher in #228 has no code signature or launch entitlement. The launcher in the successful Safari controls #210/#216 is a different binary. This explains why #228 alone cannot establish a missing app or a target-signature refusal.

**Built and host-verified.** The separate discovery case packages the project’s own GNSS Monitor with a genuine Apple Development certificate matched to an unexpired embedded profile permitting the bundle ID. Its code signature carries only the standard application and team identifiers. The helper reads LaunchServices state and the registered app’s actual hashes; it does not edit a registry, grant privileges or issue the old launch RPC. The system’s foreground activation, physical-device profile eligibility and public location client are still subject to on-phone verification. [Apple registered-device distribution](https://developer.apple.com/documentation/xcode/distributing-your-app-to-registered-devices).

## November 10 native Settings location control, RAM ticket 257

**Observed on IN2010.** In run 083454, GNSS Monitor's foreground request displayed the system alert asking the owner to turn on Location Services in Settings. The owner opened Settings and reported that the location option was missing. Frame 3065484 shows an empty search for “Location”. Frames 3065496 and 3065508 show the Settings home list with Privacy & Security. The last frame, 3065520, shows the Privacy & Security page with Calendars at the upper edge and the scroll bar below its top position. The captures therefore establish the page identity but leave its first group outside the recorded viewport. The system-wide switch and a per-app grant are separate user choices. See `out/eye/gnss-permission-frames-ticket-257/` and `out/eye/gnss-permission-ticket-257/evidence.json`.

**Observed in the 20F66 system.** The first group in `PrivacySettingsUI.framework/Privacy.plist` declares LOCATION, with detail class `PUILocationServicesListController`, above Tracking and Contacts. REA's host plist inspection returned evidence ID `ev_6a49e8d4f75cf0a2679d8ca2ca9ec26658df86e9aeeda14d3c35a59beead2927`. Separate, read-only inspection of the local 20F66 dyld cache with `ipsw dyld disass` identified a dynamic filter in `PUIPrivacyController`. Its `refreshLocationServicesAvailable` asks CoreTelephony for subscription information. A subscription query error exits before setting the cached availability. A successful empty subscription list takes the default-true path. Where a subscription exists it also evaluates the carrier bundle's LocationServicesAvailable value. The `specifiers` method removes LOCATION if cached availability evaluates to false, which includes a missing value. Thus a genuine response indicating no subscription and a transport error take different paths. These findings describe stock code, independent of which branch a particular Settings process took.

**Matched evidence and limits.** In the #257 RAM root `f35921c1ee8d1579990b4e1a23a89cd153c51917ca85169cd50b11faf173c71d`, none of the 418 active launch job entries owns `com.apple.commcenter.coretelephony.xpc`. Both stock CommCenter definitions and their shared executable exist on the full iOS system volume. The boot overlays the launch job directory. In `logs/083454-run.log.live.177`, Preferences PID 261 received “No such process” for that endpoint at 08:40:55.379444 and again at 08:40:56.065743 during navigation. This gives a concrete explanation for the owner's report. The retained logs do not identify the exact failing Settings method at those instants.

**Repair and verification boundary.** The stock noncellular job uses the same executable as the cellular job, selected according to the presence of the baseband device-tree node. Earlier tests of that stock variant obtained real IPC responses and then recorded 20 crashes, 11 after an AppleIPAppender lookup failure and 9 after an AppleBasebandPCI failure. See section 4 of [iOS userland](ios-userland.md). Restoring a job plist alone has no stable result on this port. A native repair needs a functioning CoreTelephony service with real platform support. Verify that the same service process stays alive and answers while Settings opens the page, then capture two stationary frames at the very top of Privacy & Security and verify the Location Services entry ahead of Tracking. Opening the entry should display the system-wide switch in its existing state. Any subsequent setting change belongs to the owner.

**System navigation.** The English title and Settings button in frame 3065331 match the 20F66 CoreLocation localization keys LOCATION_DISABLED and LOCATION_DISABLED_SETTINGS. In Apple's original `locationd` (UUID 9D82FA3B-F3C7-355C-B696-E235718BEA1C), the response handler for that system-wide alert passes the native Privacy → LOCATION destination to the daemon's preferences-opening routine. The camera capture establishes only that Settings reached its home list. The #257 logs contain no captured route-delivery result; a failed lookup of the filtered row remains an inference.

**A second owner-operated route to inspect.** On the local device, Apple's Screen Time settings construct their own Location Services row under Content & Privacy Restrictions. The 20F66 `STAllowLocationRestrictionsController` inherits `PUILocationServicesListController`. Its specifier builder calls the superclass, which reads the global status and constructs the actual system-wide switch. It then inserts the extra policy choices. The disconnected port also stalled a synchronous global read in #254, so the existence of this separate entry does not prove the pane will finish loading. The ScreenTimeAgent job is present in #257. If the normal primary page lacks the row, inspect this existing Screen Time route only when enabled and accessible. A pass requires seeing the native Location Services switch on screen. No completed run here has established that result. Leave the restriction policy and the location choice with the owner.

**Offline reproduction and passive hardware diagnostic.** From the kernel checkout, run `python3 ../oneplus8-audit/gnss-settings-next/native-controls/check.py 257` to reproduce the root hash, five stock controller checks, service ownership and five failed Settings lookups from 177 snapshots. The companion `commcenter_inventory.c` enumerates IOKit matches for AppleIPAppender by name and the three observed AppleBasebandPCI classes, with IOResources as a positive control. Its ordinary macOS build exercised both positive and empty queries. The arm64 iOS 16.5 diagnostic has SHA-256 `341ffab48b7c9f4f30a2d6a7ad9ef4ca7a081f2406dbd26d162d6945216891e2` and awaits a same-boot phone observation. It opens no user client and transmits no modem requests. QMI ticket #227's raw-IP format and #246's physical SIM/firmware refusal do not provide the IOKit user-client or a successful CoreTelephony subscription reply.
