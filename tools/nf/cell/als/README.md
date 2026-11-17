## Public source packaging — 2026-11-17

`base/` contains the 16 pinned, project-authored ALS baseline source files;
`reader-base/` contains the read-only ext4 calibration parser/reader source.
They contain no firmware or factory calibration values. `sources.json` pins
source SHA-256 hashes, not API keys. The builder defaults to this local base
(`HACKIOS_ALS_SOURCE` can override it) and uses public commit `e33c90b`, whose
cellular subtree was checked byte-identical to the private historical base.
The tests run without the original audit directory. Full linking still needs
the existing private build environment, generated objects and firmware inputs.

# IN2010 front ambient light on the cellular kernel

This optional RAM profile starts the IN2010 sensor processor from the ordinary input init when the command line contains `cell=1 als=1`. Its control build reproduces the installed cellular kernel SHA-256 `0a6636902ab9411bf45cf7767c994a0e77f1061412c25bd393538304cce0fa08`. `build.py` relinks that source snapshot with the ALS changes into a separate kernel pin. The matching root starts the factory calibration reader and a read-only bridge for the stock Accessibility Settings preference, with the existing cellular init and userland jobs.

The client discovers the signed DSP's available physical `oplus/stk33502/wise_light` service, requests the stream advertised by that service and checks increasing SEE timestamps. A panel sample requires native HIGH status, the verified front calibration path and factory registry values, and a successful DSP read of the handset's coefficient. The first returned float is already in lux. The AP software uses it directly.

The curve uses both checked instantnoodle board tables, with 25 illuminance-to-nits points and 65 nits-to-backlight points. The integer code interpolates linearly between points, smooths the input over 250 ms, requires a sustained threshold crossing for 1.5 s and slews toward the 11-bit DCS value. Screen-off, HBM, missing or stale sensor reports, unknown or disabled stock Accessibility Settings preference and manual priority all affect admission. The panel layer submits under the existing display lock and records fresh DMA completion. Real manual adjustments take priority. The current kernel waits for an acknowledged OS brightness request and its normalized slider observation to settle before it takes over. The existing input for panel fades is the OS request for physical nits. An independent, read-only observation of the normalized slider is used to identify manual input. The 19 slider reports from ticket 247 yielded 17 real changes, while all 25 reports during the ticket 399 idle fade stayed within numerical noise. Ticket 400 additionally captured the complete backboardd fade, including 0.499663 just before display-off and a zero after actual display-off. The current slider deadband is 1/1024 of the normalized range. It ignores slider state while the panel is dark. A system fade yields the panel back to the OS. The OS keeps immediate ownership of physical nits and the slider throughout the current lit session. A real dark-to-lit transition clears that session hold, with a fresh Settings-On heartbeat and a newly acknowledged OS brightness before automatic control resumes. Turning the stock preference off and on also starts a new session. The slider observation reports an owner change; a reported zero alone cannot establish that a person touched the slider. Ticket 401 recorded the actual IOKit endpoint zero immediately before the panel went dark.

The stock iOS 20F66 Accessibility Settings control reads the unsigned-byte value from libAccessibility’s `_AXSAutoBrightnessEnabled()`. AXDisplayController's setter calls `BKSDisplayBrightnessSetAutoBrightnessEnabled` and posts `kAXSAutoBrightnessChangedNotification`. The mobile UID 501 bridge polls that exact getter and allows pending notifications to run. The earlier trial used the generic CoreBrightness `DisplayBrightnessAuto` property. Ticket 398 recorded backboardd receiving a generic write and returning success while an independent client continued to read Off. The generic property therefore cannot be used to decide panel ownership.

The separate `stock_enable_once.m` RAM test client accepts only `--enable-once`. It reads the stock preference, waits for ordinary services, and calls the same setter and notification only if the ordinary getter reports Off. It then observes the stock preference for up to 12 seconds. The daily bridge remains read-only.

Acceptance requires **Settings → Accessibility → Display & Text Size → Auto-Brightness** to be On in the actual stock getter. The kernel switch admits the driver; the observed Settings state authorizes panel control. Tickets 403 and 404 both recorded state 2 from the mobile Settings getter. The normal profile only reads the setting.

The RAM build accepted on the handset in ticket 404 is kernel `77014e379c16ba5fc80d7ede6dc9e213419fec9f1b5ea55d5b3b72098bffe042` and root `ea3fce12939024fa52c4116d9766fa519e1d0db7ff9af9c143a8a213f4389bb6`. The normal build writes its path to `out/cell-als-kernel.txt`.

## Host verification

From the root of iphont-kernel, run the following commands. The first build checks the base against the installed control. The native sensor C and framebuffer C++ translation units and the scoped ADSP rail handling link through the normal cellular builder.

```sh
python3 tools/nf/cell/als/build.py --control
python3 tools/nf/cell/als/test.py
python3 tools/nf/cell/als/build.py
python3 tools/nf/cell/als/build_reader.py --verified-current
python3 tools/nf/cell/als/mk_root.py out/cell-boot-debug-init-rebuilt-root.apfs --output out/cell-als-candidate-root.apfs
python3 tools/nf/cell/als/build_enable.py --stock
python3 tools/nf/cell/als/evidence.py
```

The policy tests use software fixtures and replay the 1,196 actual optical samples from run 074253. With the 20 ms actuator, the replay ends at the same 945 DBV after 94 modeled steps. The historical phone trial measured steps only to 272. The historical on-device receipt pairs a recorded selection with the matching target. The smooth driver logs each acknowledged physical step with its source SEE timestamp and independent AP clock, and the collector requires a fresh DMA completion and an incrementing send counter.

## Automatic transition cadence

Ticket 403's qualified optical stream arrived approximately every 200 ms. The old policy wrote the panel only from that callback. The old trace contains 24 matched steps in both directions, with a maximum single write of 96 DBV. The sensor now selects the same board-curve goal using the existing 250 ms filter and 1.5 s hold. The existing GLINK pump, nominally every 20 ms after subscription, applies the accepted goal between sensor reports. Its separate clock never banks a long idle interval. A fractional remainder preserves 384 DBV/s, with a 24 ms catch-up cap. A normal 20 ms interval yields 7 or 8 DBV. One delayed interval can yield up to 10 DBV. The manual owner, display power and native auto preference gates are checked before each transfer; stale input cancels the ramp. Each `light-ramp` record identifies the latest genuine sensor sequence, AP time, physical DBV and fresh completion.

The host replay of ticket 403's recorded target/timestamp pairs produced 168 small changes, at most 8 DBV each, at a modeled 20 ms cadence. Ticket 404 recorded 197 confirmed upward writes (maximum 10 DBV) and 52 downward writes (maximum 9 DBV). The 243 consecutive intervals to the same unfinished goal ranged from 20.117 to 30.902 ms, with a median of 22.718 ms. Of these, 218 advanced the panel using the same last genuine SEE sample. Under bright illumination the goal reached 1326 DBV. At 44–45 lux it moved down in 46 confirmed writes to 929 DBV, then at 26 lux it moved in six further writes to 881. The device's sparse front snapshots and the distinct optical samples appearing in curve or ramp logs are separate ranges in the receipt. Neither range is asserted to contain the full optical stream. The owner confirmed both automatic directions worked and reported that the steps were no longer visible. The normal manual IOKit setter was unchanged by the cadence update.

## Sensor service lifetime

Ticket 406 showed the native Auto-Brightness control in stock Settings and accepted its Off/On changes. After 1,999 valid front samples the diagnostic transport rejected QRTR packet 2,049. The run logged 11,435 GLINK frames, a broken link, zero malformed sensor events and no stop request. This independently removed the Settings light service while the OS was still running.

The fix keeps the bounded startup inventory and bounded packet capture. While the qualified sensor subscription is active, valid transport packets, delivered samples and completed listener callbacks no longer have a lifetime quota. Per-frame sizes, source identity, receipt of the coefficient, sample health, ownership and controlled shutdown still apply. The host test uses the captured ticket 406 QMI layout through the production GLINK/QRTR and SEE parser for 50,000 reports and crosses the old listener cutoff with 10,000 completed calls. Radio/Settings integration needs a new on-device measurement beyond packet 2,049. The sensor log now labels the cause whenever the session exits.

## Factory calibration and ownership

The first cellular ticket accepted 1,321 front samples and matched both factory registry reads. The original file reader refused this handset's current `persist` volume. Ticket 396 then measured EXT4 compat `0x3c`, incompat `0x46`, read-only compat `0x7b` and state `1`. Incompat bit `0x4` requires journal recovery. The typed reader keeps the real UFS node read-only. For this exact combination it reads only the target path, compares the live four bytes to the SHA-256 of this handset's original ticket 249 capture, then re-reads the exact path metadata and data blocks for stability. The digest is supplied as a root-owned binary asset extracted by the host from the original receipt. Only the current file contents are delivered to the DSP. The filesystem journal is left intact. [Linux EXT4 journal format](https://docs.kernel.org/filesystems/ext4/journal.html)

Ticket 397 matched the current factory file and independently observed the DSP serve it. All 1,720 accepted samples reached the panel callback. The generic CoreBrightness read returned Off and the trial performed zero automatic brightness writes. The stock Accessibility Settings preference was not yet measured in that run. Ticket 399 separately measured the stock Settings preference already On, with zero preference writes, and five fresh native DCS writes from the startup panel level 399 down to 26 DBV at 0 lux. Its first OS brightness write then displaced the startup panel owner. Ticket 400 then waited for the actual OS owner at 334 DBV, waited for it to settle and delivered four acknowledged steps down to 26 DBV. Its first deadband was too small for the end of the system fade and falsely marked a manual hold. The follow-up build replays that complete 34-report fade without marking a manual change, while retaining genuine changes from the ticket 247 manual trial. Ticket 402 used the actual OS owner at 334 DBV, reached 26 DBV in four fresh writes, then released the panel during the normal OS fade. Following a physical DCS-off/on cycle it observed a fresh OS replay of 1 DBV and resumed automatic control with another confirmed write to 26 DBV. Ticket 403 then confirmed on-device that changing the front illumination makes the panel brighten and dim, and the user reported that only the automatic transitions appeared stepped. Host curve replay and the older handset source measurements are separate evidence. The per-run counts, observed lux snapshots, panel steps, and teardown are in `out/cell-als-bringup-evidence.json`.

The board's sensor carveout ends at the start of the cellular build's audio-DSP carveout. The driver reserves an unoccupied non-display SMMU context from banks 6–9, retains the installed stream mapping while allowing the shared allocator to run, and verifies ownership before teardown. The three runs used bank 9, reclaimed the mapping and restored the seventeen OEM words. The stopped context retained its inert CBAR type, and the known proxy floors remain until a full hardware restart. Normal suspend, full rail release and persistent daily installation still require hardware qualification.

### Native switch visibility on 20F66

The 20F66 AccessibilitySettings bundle (SHA-256 `258b9ce9…43dbf`) checks
`MGGetBoolAnswer("ambient-light-sensor")` at offset 0x894f0 in
`-[AXDisplayController specifiers]`. A false answer skips the entire group.
The true branch creates the switch and assigns its enabled property from
`BKSDisplayBrightnessIsBrightnessLevelControlAvailable()` (0x895ec).
The native getter and setter are at 0x89860 and 0x89828 respectively.
In the iPhone10,3 20F66 libMobileGestalt cache image the capability table
at 0x1d733e840 identifies the handler at 0x19c547058. Its numeric
IOPropertyMatch compares PrimaryUsagePage 0xff00 and PrimaryUsage 4.
It calls IOServiceGetMatchingService at 0x19c54714c and returns whether a
service was found. These addresses are static host observations.

`build.py --settings` adds HKFrontLightSensor to the smooth physical ALS
kernel, with its own `out/cell-als-settings-kernel.txt` pin. The board path
publishes the service before scheduling the sensor worker. CalibratedSample
appears only after a genuine HIGH calibrated event and is removed on stale
input or shutdown. Its 24 bytes contain host uptime in microseconds, SEE
ticks, sequence and the original IEEE-754 lux bits. The registered service
does not dispatch a second stream of brightness commands.
`settings_root.py [FULL_RAM_ROOT]` adds a read-only mobile observer and
verifies the original files, modes and owners after packaging. The initial
ARM64 candidate is aa926b2b with root f6077a20, derived from smooth 77014e37
and root ea3fce12. This candidate has been built and verified on the host.
Hardware acceptance requires the visible enabled switch at the bottom of
Settings → Accessibility → Display & Text Size, then genuine Off and On
interactions, native getter transitions and matching panel ownership.

In #406 the ordinary mobile observer saw the physical service, a calibrated
sample, `MG_ambient=1` and `AX_value=1`. Its historical `BKS_enabled=0`
leaves the switch's operability unknown. In 20F66, backboardd checks
`com.apple.backboard.displaybrightness` against the caller's audit token
before reading CoreBrightness's `CBBrightnessControlAvailable`. The BKS
wrapper returns its default zero after a rejected query. The stock
Preferences executable has this entitlement. The #406 observer has none.
The current observer records `BKS_enabled=unknown_unentitled`. Use a
visible switch and successful taps in Preferences to verify operability.
