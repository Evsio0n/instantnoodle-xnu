# Native cellular UI on iOS 16.5

The stock Preferences wireless group declares `MOBILE_DATA_SETTINGS_ID` in
`PreferencesUI.framework/Settings.plist`. Its label is `CELLULAR`, it
requires the MobileGestalt capability `cellular-data`, and its value getter
is `cellularDataStatusForSpecifier:`. The installed pane is
`SettingsCellular.bundle`, bundle ID `com.apple.telephony.CellularSettings`,
principal class `PSUICellularController`. These are on-disk declarations.
Only a framebuffer showing the row or the pane proves that the running
Preferences app presents it.

## What the phone showed

Private RAM #268 / 112433 used the byte-identical #267 SDX55 kernel and the
ordinary stock SpringBoard and Preferences on the read-only 20F66 system
volume. The modem reached mission and returned actual DMS, NAS and WDS
query results. NAS reported registration 0, CS 2 and PS 2. At this time
the mobile process queried MobileGestalt and received cellular-data=no,
telephony=yes and HasBaseband=no. IOResources was present. AppleBaseband,
PCIControl, PCIInterface and PDPManagerBase class enumeration returned
zero. This revision checked AppleIPAppender by **name** only. There was
no verified running stock CommCenter process, so the observer skipped
both CT calls. It never interpreted that as an empty subscription list.
The actual Home icon opened the normal Settings app. The captured wireless
group contained Airplane Mode, Wi-Fi (Off) and Bluetooth (Unavailable),
followed immediately by Notifications. There was no cellular row to open.
Native carrier and signal bars have not been demonstrated.

The device records are in
`out/eye/stock-cell-ui-ticket-268/result.json` and
`out/eye/stock-cell-ui-frames-ticket-268/visual-review.json`; the verified
frame SHA-256 is
`259118360b9814154059bad561b465d7d4374fb1c041467d4ab0b1c01b583e34`.
#266 had established an MHI RX credit and safe retirement without any packet
completion. That transport test contains no stock UI observation.

## Source-to-UI contract

UniXNU checkout e2fdae4312d0acc3d8a52bc457101e7918095e6a describes a
Coachz SC7180/IPA v4.2 example using iPadOS 17.7.11 (21H461). Its manifest
pins QcomBaseband dd0199cf3300764ed52b23f4a47761f91fab21a2. The local checkout
has the gitlink and porting notes but no populated component sources.
Those notes describe allocating a local QRTR port per QMUX client, announcing
and withdrawing real firmware services, routing messages per client, and
queueing async channel completions. The reference then provides baseband
control, named PCI transport channels and PDP management to stock ABM and
CommCenter. CoreTelephony subscriptions, RAT and network registration are
distinct upper-layer observations. Settings activation and status-bar
rendering each need their own verification. On the IN2010 the physical
control path is QRTR over the SDX55 IPCR MHI channel. Raw-IP transport
uses IP_HW0; Coachz's on-SoC IPA/GSI datapath is board-specific.

On 20F66, channel matching uses class AppleBasebandPCIInterface with
a top-level **CFString** ABPInterfaceProtocol property. Per-interface
enumeration and a matching service with the right class name alone do
not establish this. `abp_match.c` checks the actual matching dictionary,
re-reads the returned property and uses an unsupported protocol as a
negative control. The CFString lookup and subsequent class-based
AppleIPAppender check first appear in the later bf9867fe root.
The stock cellular CommCenter plist selects a real baseband ADT node via
`_LimitLoadToDeviceTree`. The noncellular sibling uses the opposite rule.
Declaring presence does not implement the selected modem ABI. A driver
must offer real open, availability, input, output, asynchronous completions
and endpoint loss before registering an Apple class the OS can open.

The maintained first QMUX path lives in `xnu/files/bsd/sys/hk_qmux_nas.h`
and `xnu/files/bsd/dev/hk_qmux_nas.inc`, compiled as part of
`hk_qrtr_mhi.c`. An owner-serialized channel locally allocates and releases
real QRTR ports for independently scoped CIDs. Only the NAS v1, instance 0
Get Serving System request has a service path. Matching responses, including
firmware errors, are returned byte-for-byte with the appropriate QMUX header.
A valid indication is delivered only to its addressed local client and never
consumes that client's pending transaction. An endpoint loss requires a
fresh CID binding; release/reopen before retrying a timed-out request uses a
new QRTR port, even when the new CID and transaction numbers coincide.
There is no Apple IOUserClient in this adapter. It does not publish a
carrier, registration state, signal strength, subscription, or data bearer.

## Reproducing the checks

`python3 tools/nf/stock-cellular/build_root.py` cross-compiles the ordinary
mobile observer, rebuilds the channel matcher, constructs the RAM root
from the established desktop recipe and verifies all entries independently.
The desktop source is #258. The effective combined modem source is #267.
No custom Wi-Fi/HTTP/Safari/permission job is in the stock observation
profile, and the radio remains subject to its original bounded cleanup.

The stock observer reads only the three reported MobileGestalt booleans and
aggregate interface, subscription and RAT counts. It checks the actual
CommCenter executable before asking its normal XPC endpoint anything;
child queries time out after seven seconds. The operator can run
`watch.py` inside its existing FIFO ticket. It recognizes the real Home
Settings icon and actual Settings text twice before a normal touch and
stops at a system dialog. Its negative OCR result is only a bracketed
wireless-group capture pending visual review.

Run `tools/qrtr-test/run.sh` for ASan/UBSan model coverage and
`python3 tools/qrtr-test/build_phone_nas.py` to reproduce the #267 control
and link the opt-in trial. In the isolated physical profile `ui_qmux=1`,
the #267 modem owner can issue up to two read-only NAS 0x0024 requests on
different CIDs and QRTR local ports with the same transaction. At its
deadline or original modem teardown it releases both ports. Current-run
validation is `python3 tools/qrtr-test/verify_phone_nas.py RUN`. It
requires both attributed replies and never assumes successful network
registration. Physical phone operations belong to the cellular ticket owner.

### Physical transport and its corrected ownership

RAM #269 / 113947 received two 35-byte successful NAS replies from node
3 port 58. QMUX CIDs 1 and 2 used actual local QRTR ports 16391 and 16392
with the same transaction 30115. Each reply reported registration 0, CS 2
and PS 2. Both ports were released, all four modem shutdown pins were
observed down and stock FASTBOOT was verified. The integration probe
submitted and closed in the mission thread while the original MHI poller
could run receive callbacks. Its two real replies verify routing in this
run. They do not establish correct ownership under concurrency. Keep the
original kernel 9f997271 and its evidence separate.

The corrected candidate is generated by
`python3 tools/qrtr-test/build_phone_nas_owner.py`. Its controls reproduce
#267 and #269 byte-for-byte. With `ui_qmux=2`, require radio/LOC/GNSS
clients and Wi-Fi disabled. Even IPCR received recursively during an MHI
transfer is copied into the bounded ingress. The single poller opens its
real NAS-backed qmux1 binding, processes accepted frames in order and issues
at most two NAS reads. At STOP it closes admission, retires or discards
remaining copies, detaches all clients and watchers, takes QRTR down and
acknowledges. Mission and loader cleanup require that acknowledgement.
Lost admission prevents new requests. A timeout holds ownership for
independent cold recovery. The host harness exercises the same worker code
under ASan/UBSan and TSan, including an early announcement, nested receive,
STOP during a callback, failed startup, timeout and overflow:
`tools/qrtr-test/owner_tests.sh`.

RAM #270 / 115809 verified the poller-owned path with the unchanged transaction
30115 on local ports 16385/16386. Both returned 22-byte firmware results
`INFORMATION_UNAVAILABLE (74)`. Their optional field bytes are not usable
registration or attachment values. All 50 admitted datagrams were retired,
with zero discards, overflows or rejections. The poller returned from callbacks,
detached, took the link down and acknowledged before modem cleanup. Kernel
return and stock FASTBOOT were verified. The original strict serving-data
gate finished with rc 1. Unlike #269, this trial sent on endpoint availability
and ran with the other radio clients disabled, so there is no single-change
explanation for the different firmware result.

### Maintained upper interface

`xnu/files/bsd/sys/hk_abp_nas_worker.h` and
`xnu/files/bsd/dev/hk_abp_nas_worker.c` expose an internal interface for a
normal board adapter. The standard BSD file lists compile the MHI/QRTR,
protocol-binding and worker units. Before the board starts producers,
arm persistent storage with the actual MHI adapter, the inbox mutex
operations and an owner-thread identity predicate. Capture the current
worker generation for each registered RX callback. RX copies data to the
bounded queue. Publish READY only on real mission entry. After each outer
MHI dispatch, the sole poller calls `hk_abp_worker_poll`. It may then
explicitly open `qmux1` or `qmux2`, write the supported QMUX request, or
close a lease. Open alone allocates no firmware service client. There is no
automatic NAS read on arm, ready or idle poll. The future Apple user-client
must dispatch its submissions to this owner and copy replies into its own
bounded completion queue; the existing emit callbacks borrow their bytes.

STOP closes producer admission. Each admitted producer holds a reference
**before** attempting the inbox mutex. The owner retries finish on EAGAIN
until these references are gone. A timed-out HELD state cannot become
DRAINED even if an old owner later finishes. Disconnect and join upstream
callback sources and stop the worker before freeing or rearming storage:
a callback stalled before the function's first admission operation still
belongs to its external source. `hk_abp_worker_rearm_after_disconnect`
requires a clean drain, increments the generation and retains the backend
lease serial. Both stale RX and stale leases are rejected. No rearm from
quarantine is allowed.

The extracted ordinary API follows #270's ownership design, with an
additional admission reference and a generation token for external
producers. The extracted code itself was host-tested under ASan/UBSan
and ThreadSanitizer and cross-compiled/linked into an ARM64 XNU kernel
(`12926d6c…93ba4`); it has no phone run. Rebuild with
`python3 tools/qrtr-test/build_nas_backend.py`. A functioning
20F66 PCI control/channel IOUserClient, exact async completion ABI, real
PDP provider and a working stock CommCenter/CoreTelephony consumer still
stand between this backend and the stock Cellular row or signal bars.

### IOKit discovery and completion boundary

The normal NF source lists now include the BSD front end and the IOKit C++
class `HKCellularQMUXChannel`. The opt-in board owner calls
`hk_qmux_frontend_poll` after a successful owner poll and
`hk_qmux_frontend_stop` before final backend detach. Each service contains
the verified CFString protocol, publication generation, actual NAS peer
and local QRTR port and allocated CID. Both named transports have to open
and resolve real local ports before initial publication. When the peer is
lost or ambiguous, matching entries are withdrawn. A retained registry
object carries immutable scalar metadata and no pointer to the worker.
Apple's PCI interface and control classes are not published.

The kernel-facing API in `hk_qmux_frontend.h` supports only an explicit
NAS Get Serving System read. Each channel bounds its pending work to one
token and 15 seconds. A queued cancellation sends nothing. Cancellation
after submission or a timeout retires that channel's local port before a
retry. A delayed old reply cannot satisfy a new token, and the sibling's
pending operation and generation remain intact. A successful modem result
and valid serving TLV are required before exposing registration or CS/PS
as usable. The IOKit object provides discovery and ordinary read-only
registry properties; its user connection is closed by policy. In this port
the code-signing compatibility layer does not provide a trusted way to
distinguish a stock entitled task from a self-declared claim. The two
entitlements present in Apple's original signed CommCenter file are
evidence about that binary, not runtime caller proof. The project frontend
therefore does not dispatch private Apple external methods or export buffers
or addresses. The stock Settings pane and signal status still require a
trusted, ABI-complete native consumer path.

Host regression uses the real worker, QRTR transport and typed front end
with test modem responses and a test registry. It covers separate channel
generations, wrongful or stale tokens, late data after cancel, a sibling
request in flight, timeout, firmware refusal, duplicate remote endpoints
and matching removal/reappearance. Run `tools/qrtr-test/frontend/test.sh`.
`python3 tools/qrtr-test/build_nas_backend.py --with-frontend` verifies the
ordinary BSD and iokitcpp file declarations and cross-compiles and links
the six units into ARM64 XNU. This ordinary integration artifact is
`35897965…de193`, host-only. The board's independently pinned RAM test
kernel and its device receipt have their own provenance. Standalone
observer packaging uses `python3 tools/nf/stock-cellular/build_root.py
--frontend`.

### Physical IOKit frontend evidence

#271 / 124937, kernel ee8b8796 and root 95bbb41e, completed with rc 0
and verified stock FASTBOOT. In this ordinary 20F66 mobile process, actual
IOKit FirstMatch notifications and later enumeration identified project
qmux1 (entry 4294967667, generation 1, QRTR local 16385) and qmux2
(entry 4294967668, generation 2, local 16386). Each scoped channel had
CID 1; both reached the same physical NAS service at node 3 port 58.
The unsupported protocol did not match. IOServiceOpen at type 0 on both
published project entries returned 0xe00002e2 and no handle, as designed.

The explicitly enabled kernel fixture cancelled one queued token without
sending it. The two independent submitted reads, each with transaction 1,
returned 35-byte successful modem replies. Registration was 0, CS 2 and
PS 2, meaning unregistered and detached. Terminated notifications carried
the same entry IDs and generations as FirstMatch; a later enumeration
found both absent. The owner retired 50/50 admitted frames, with no loss,
and the modem's normal cleanup and SMMU restoration passed. Wi-Fi, GNSS,
SMS and bearer activation were not enabled.

The genuine Apple PCI interface remained absent. The reviewed native
Settings screen showed Airplane Mode, Wi-Fi Off, Bluetooth Unavailable,
then Notifications. There was no Cellular row or opened cellular pane.
The underlying successful NAS reads came from the kernel fixture;
the mobile observer inspected IOKit and confirmed open denial. Neither
observation implies that CommCenter opened a native baseband connection.
The combined run receipt is `out/eye/qmux-front-ui-ticket-271/result.json`
and its native Settings frame is indexed in
`out/eye/qmux-front-ui-frames-ticket-271/visual-review.json`.

### Passive serving-system notifications after #271

The frontend now accepts addressed NAS Serving System indications (0x0024,
QMI flags 4) through the existing modem worker. A kernel reader calls
`hk_qmux_frontend_read_event(index, generation, after_sequence, out)` with
an initial cursor of zero. Each channel stores its latest event with its
sequence, monotonic receive time, node, peer/local ports, scoped CID and
registration/CS/PS values. Advance the cursor after reading. EAGAIN means
there is no newer report, and an explicit registration value of zero means
unregistered. A generation change clears the slot and returns ESTALE for
old readers. Successful and failed query replies use the separate token
API. Silence leaves both the event sequence and receive time unchanged.

The NAS-only worker also checks the destination node for ordinary service
data. This preserves broadcast discovery and control aliases while
preventing another node's identically numbered port from contributing an
event. The archived libqmi NAS schema defines explicit notification enable
as Register Indications (0x0003) with Serving System Events boolean TLV
0x13. This receive-only path sends zero subscriptions. The board's default
notification behavior and use by a stock native consumer still await
physical evidence.

The maintained checkout passed ASan/UBSan, TSan and the shared host suites.
The isolated full ARM64 relink produced 8f0d298b and reproduced the original
#271 kernel ee8b8796 from its frozen sources. #271 exercised explicit
queries. The newer image has host evidence for passive events. See the
[replay and build receipt](../../../../oneplus8-audit/stock-cellular-ui/nas-serving-events/EVIDENCE.md).

### Explicit NAS Serving System subscription

The kernel frontend now supports an explicit client-scoped enable on a live
publication. `hk_qmux_frontend_subscribe` returns a token. Poll
`hk_qmux_frontend_subscription` for STARTING, ACTIVE, REFUSED, STOPPING or
CLOSED. An ACTIVE status includes the modem's enable ACK and the event cursor
at that ACK. `hk_qmux_frontend_read_subscription_event` returns only newer
addressed Serving System indications for that token. A caller can issue an
initial GET separately. Every normal channel starts without a subscription.

Only NAS Register Indications (0x0003), with a single one-byte Serving System
Events TLV 0x13, is admitted for this operation. Enable and clear may each
be sent at most once per allocated CID and local port. An accepted explicit
stop immediately cuts off subscription reads. With an acknowledged enable
and an idle transport, the owner sends the same TLV with value zero and
records its independent ACK. Take or cancel an already completed GET before
stopping (EBUSY). An outstanding exchange, timeout, peer loss or shutdown
can require local port retirement while the firmware clear is unconfirmed.
A CLOSED status retains separate enable/disable receipts and its terminal
errno. The original local port and lease retire before the channel can be
reused. Publication generation and token also bind each status read. The
last closed status is retained per channel.

The bounded opt-in board fixture uses qmux1 and issues one baseline GET, one
enable, a twelve-second indication window, then one clear. The wire receipts
record MHI submission, numeric result, real indication fields, port retirement
and DEL_CLIENT submission independently. A zero-report window remains valid
evidence of silence. Host ASan/UBSan and TSan exercised successful/refused
and timed-out exchanges, cross-operation TID collisions, reordered optional
TLVs, duplicate results, stale peers/ports, cancellation, explicit clear,
and complete board replay. The full board ARM64 link reproduced
8f0d298b then produced 991605ab. [Immutable candidate and source hashes](../../../../oneplus8-audit/stock-cellular-ui/nas-subscription/kernel.json).
