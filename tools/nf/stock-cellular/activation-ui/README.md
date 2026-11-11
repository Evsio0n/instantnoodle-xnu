# Cellular Monitor

The foreground App reads only `kern.hackios.cellular_observation`.
The modem owner publishes the fixed-size versioned snapshot under its lock.
The App checks the structure size, phase, field validity flags and field
timestamps before displaying physical/logical slot, card/USIM application
state, primary GW mapping, the activation outcome, NAS registration and
attachment, or WDS session state. Time labels use the producer's monotonic
clock. Fields become unknown when the owner clears validity or stops.
The UI separates local submission, firmware acknowledgement and a later
independent Get Card Status confirming the actual USIM binding.

The build uses the verified developer identity/package slot previously used
for the RAM-only monitor. Its displayed name is Cellular Monitor, its
bundle ID remains com.hackios.gnss-monitor, and the executable has standard
development entitlements. Its sole button invokes UIKit's ordinary app
Settings URL. It links UIKit and Foundation, never writes a sysctl and
includes no activation, modem-control or location client. The native
Preferences wireless group is inspected separately; its contents cannot
be inferred from this App.

From the kernel checkout, with the existing local SDK/signing materials:

    python3 tools/nf/stock-cellular/activation-ui/check_home_vfs.py
    python3 tools/nf/stock-cellular/activation-ui/build_app.py
    python3 tools/nf/stock-cellular/build_root.py --activation-ui

The retained signature, projected ABI hash, VFS object, signed bundle and
verified RAM root manifest are under ../oneplus8-audit/stock-cellular-ui/activation-ui.
Only the delegated physical ticket owner uses the visual watcher; normal
touches open the Home icon and a genuine system Settings page. The watcher
never sends a modem request or selects a privacy or cellular switch.

#277 / 162018 returned normally to stock FASTBOOT, terminal rc 0. Cellular Monitor sample 49 displayed primary GW slot 1/application 1/USIM READY, one submitted request, ACK 0/0 and two independent verified reads. DMS was OFFLINE, NAS unregistered with CS/PS detached, and WDS disconnected; field ages on this screen were 92–93 seconds. The stock Settings wireless group proceeded from Airplane Mode, Wi-Fi Off and Bluetooth Unavailable to Notifications with no Cellular row. Actual frames and the reviewed values are in out/eye/activation-ui-frames-ticket-277-resume/visual-review.json, separately from out/eye/uim-primary-ui-once-ticket-277/result.json. The initial gesture was rejected by the host before USB delivery; the corrected navigation ran in the same boot and did not replay activation.

#278 / 170831 reused exactly the #277 signed App (ead80935) and RAM root (f6a8342e) for the replacement SIM. Its live Sample 51 showed slot 1/application 1/USIM READY after the one conditional primary activation and two independent reads, with DMS offline, NAS unregistered, CS/PS detached and WDS disconnected (23 s old). Sample 56 showed the source stopped and all current fields unknown. Stock Settings again showed no Cellular row in the fully captured wireless group. The modem asserted ERRFATAL before any ONLINE submission. The radio objective therefore failed (terminal rc 1), despite verified UIM rebinding, original UI screenshots, safe teardown and stock FASTBOOT. See out/eye/activation-ui-frames-ticket-278/visual-review.json and out/eye/cold-ready-online-ui-once-ticket-278/result.json. No roaming state was established.

#279 / 172743 reached the real DMS ONLINE path before the App opened. One primary GW bind succeeded (ACK 0/0) and was verified twice. The separate single DMS request returned DEVICE_NOT_READY (52); an independent read returned OFFLINE. Across 53 actual App frames from 17:30:16 to 17:31:24 the same live-phase sample 49 remained on screen: slot 1/application 1/USIM READY, DMS offline, NAS unregistered, CS/PS detached, WDS disconnected. Radio field age increased from 94 to 161 seconds. No App frame showed ONLINE, registration or an actual roaming status; the firmware refusal itself came from the kernel log, not the UIM ACK in the App. Full native Settings group again ended at Bluetooth with Notifications next. See out/eye/activation-ui-frames-ticket-279/{visual-review,screen-timeline}.json and out/eye/early-ready-radio-ui-once-ticket-279/result.json.

#283 / 183555 uses the signed v0.8 monitor (b388e45e) and root 888873f6.
Its v1 live view is unchanged. A separate, locked, read-only kernel
exporter retains this boot's last valid fields, their individual owner
clock timestamps, the matched DMS ONLINE response and the observed
ERRFATAL edge. No state is written to a disk or sent to the modem.
Build with `build_app.py --history` and `build_root.py --activation-history`
from their normal directories. The isolated kernel patch entry is
`history/patch.py:apply`; its specific old-boot inputs stay immutable.
`history/check.py` feeds the real producer code with retained #282 field
values and synthetic timestamps, and checks stale-ACK rejection and reset.

The same physical boot captured both the live page and the stopped view.
After fault, current readings were explicitly unknown. The history card
showed slot 1/app 1/USIM ready, UIM ACK 0/0 with two independently verified
reads, offline/unregistered/disconnected as of owner clock 862.375 s,
a **separate** DMS ONLINE reply result 1 / error 52 at 780.443 s,
and ERRFATAL at 958.291 s. First stopped screenshot had ages of 98, 180
and 2 seconds respectively; later captures aged those readings normally.
The stock FASTBOOT return was verified. Cellular registration/data remained
unavailable and the overall ticket ended rc 1. See
`out/eye/pro-donor-app-frames-ticket-283/history-visual-review.json`.
