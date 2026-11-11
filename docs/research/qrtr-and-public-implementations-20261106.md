# QRTR, modem and public implementations — 2026-11-06

This is a read-only audit of the current HackiOS tree and public projects. No phone run or
partition write was performed for this audit.

## Current tree

The project now has reusable **QRTR/QMI** transport pieces; the modem path is also phone-proven
through generic LOC. The remaining gap is policy and iOS integration:

| Area | Current state | Gap |
|---|---|---|
| Wi-Fi | `hk_wifi.c` implements QRTR over GLINK and PCI-MHI, and supplies `hk_qrtr_ops` to `hk_qmi.c`; WLFW QMI is exercised | The parser and service table live inside the Wi-Fi driver |
| ADSP | `hk_adsp.c` can answer HELLO and inventory services, including service 400 | No generic `hk_qmi` client/port path |
| SLPI | `slpi_glink_core.h` has the strictest bounded QRTR/GLINK core and performs a diagnostic SEE SUID lookup | Sensor PD/FastRPC/registry/streaming and iOS HID are missing |
| SDX55 modem | MHI BHI/Sahara boots the modem; the reusable QRTR/MHI/QMI path finds LOC service 16; generic LOC Start and indications are phone-proven in run `015925` (#156); EFS-Sahara channels 10/11 complete | DMS/UIM/NAS/WDS, assistance/session policy, outdoor RF/fix evidence and the iOS/locationd bridge remain |

The original modem parser reproduction is in
`outputs/qrtr-audit/README.md` of that session's own workspace (outside this repository).
That seven-case host-only audit compiled with ASan/UBSan and reproduced five semantic mismatches
before the parser was changed:

- a legal QRTR v2 options field is advanced by `optlen * 4`, so the LOC announcement is misread;
- declared payload sizes are not checked against the received datagram;
- `DEL_SERVER` and `BYE` leave the cached LOC endpoint live;
- the service counter counts announcements, not live servers.

The current parser uses `hk_qrtr_wire_decode()` and a bounded service table. A follow-up host
harness covering the same options, declared-size, duplicate, `DEL_SERVER` and `BYE` cases now
passes under ASan/UBSan. The fixture assumed four-byte option-length units. See the later
[comparison with the Linux and OnePlus decoders](#qrtr-flow-control-and-option-length). Run `015925` (#156) then exercised the migrated generic LOC path on the
phone: service 16 at node 3/port 84, generic Start accepted, 120 position reports, zero satellites
indoors. The result is transport and transaction evidence, not an outdoor RF or locationd result.

These are real parser gaps, but the synthetic cases do not prove why run 200815 had zero
satellites. The LOC start TLVs and the SV/position field offsets match the public libqmi
descriptions. The remaining GNSS gaps are transaction/response gating, valid session and
satellite-mask handling, assistance data, and the absence of an iOS/locationd consumer.

EFS-Sahara is a separate MHI channel protocol. It must remain separate from QRTR; the current
read-only HELLO/table/read/RESET path should not be replaced by a QRTR abstraction.

## Public implementations found

1. **Linux QRTR over MHI** — upstream `net/qrtr/mhi.c` is the closest transport reference. It
   queues MHI downlink buffers, hands complete packets to the QRTR endpoint, recycles buffers,
   queues uplink skbs, and re-primes the buffers on resume. This is the design to follow for a
   modem `hk_qrtr_ops` transport; it does not provide GNSS policy or iOS integration.

2. **linux-msm/qrtr** — provides the userspace QRTR library and `qrtr-ns` service discovery. It
   is useful for protocol constants and name-service behaviour, but its userspace socket/daemon
   architecture cannot be copied directly into XNU's kernel.

3. **libqmi** — `qmicli` and `qmi-service-loc.json` provide the public LOC message schema and
   the normal start/stop, time injection, SV and position indication flow. The current hand-
   written LOC start fields match the basic flow; the generic QMI tables and transactions are
   still preferable for the next implementation.

4. **royka1/Xiaomi-Apollo-pmOS-packages** — this is the most relevant same-family reference:
   SM8250 plus an external SDX55. Its README reports LTE/5G registration and data/voice, but
   explicitly marks GPS unverified because LOC does not answer and QMI timeouts can drop the
   modem. `mhi_efs_sync.c` and `mdm_helper_native.c` are useful protocol references; the former
   is GPL-2.0-only and must not be copied into the kernel without a compatible design/licence.
   Its `tqftpserv-sdx55` is a separate BSD-3-Clause userspace reference. Its notes also show the
   AP-side pieces normally needed around QRTR: `tqftpserv`, `pd-mapper` descriptors, and a
   modem data bearer.

5. **Metal2Vulkan / Darling Indium** — public Metal-on-Vulkan/AIR projects exist, but they are
   different targets. Metal2Vulkan's current README labels the project Beta and reports runtime
   tests on M3 Max plus MoltenVK; that is not evidence for an A650/Turnip/iOS device. The
   repository's GPL-2.0-or-later/LGPL-3.0-or-later licensing also requires a separate reuse
   review. UniXNU/AdrenoMetal is MIT and targets SM8550/A740, but its public claims are not an
   independent OnePlus 8/A650 result. These are architecture references only; the existing
   HackiOS Metal→Turnip path remains the relevant implementation for this phone.

## Additional public audit — 2026-11-07

The follow-up review found two useful implementation references that were not in the
first pass:

1. **UniXNU/QcomBaseband** is a BSD-2-Clause XNU-oriented baseband stack with QMUX/QRTR,
   QMI control, Sahara, AppleBaseband IOKit services and an IPA/QMAP data path. Its
   published target is an SC7180/iOS 17 board, so its protocol and CommCenter bridge
   layering is reusable design evidence, not a OnePlus 8 SDX55 drop-in. Its IPA/GSI and
   board reset code must not be copied into this PCIe-2/MHI path.
2. **Darling Indium/Iridium** is 0BSD and useful as an AIR-to-SPIR-V/API design reference,
   but it explicitly does not provide an ABI-compatible Metal implementation. The
   existing A650 Turnip path remains the only target-specific implementation here.
3. **UniXNU/AdrenoMetal** is MIT and demonstrates an XNU Metal frontend around Turnip,
   but its validated hardware is SM8550/A740. It is useful for test structure and service
   boundaries, not as a replacement for the current SM8250/A650 driver path.
4. **metal2vulkan** has a GPL/LGPL license boundary and is therefore an architectural
   reference only for this tree. No source from it was copied.

The practical modem conclusion is unchanged: keep `hk_qrtr_core`/`hk_qrtr_mhi` in the
kernel, use Linux `qrtr/mhi.c` only to check buffer ownership and resume ordering, and add
read-only DMS/UIM/NAS/WDS transactions before attempting a cellular data or CommCenter
bridge. Do not start a second userspace QRTR daemon, and do not route EFS-Sahara through
QRTR.

## Recommended implementation order

1. **Done on the host:** a pure, bounded `hk_qrtr_core` now covers v1/v2 decode/encode,
   exact size/alignment checks, the control service table, HELLO/BYE/RESUME and client lifecycle.
   The modem parser also rejects malformed framing and removes dead LOC endpoints.
2. **Done on the host:** `hk_qrtr_mhi` adapts that core to one MHI/IPCR datagram send callback
   and the existing `hk_qrtr_ops`/QMI interface. It fail-closes while the link is down, emits
   service-gone callbacks, retains local ports/lookups for rediscovery after a link reset, and
   delivers initial service callbacks after each received datagram. `tools/qrtr-test/run.sh`
   covers the adapter under ASan/UBSan (14 MHI checks); the same adapter is phone-proven by run
   `015925` (#156). The adapter still
   uses one active instance because the legacy `hk_qrtr_ops` callbacks have no context; this is
   acceptable only while the modem transport is the sole MHI user.
3. **Phone integration is proven for the modem path:** the MHI receive callback and lifecycle now
   call the adapter around the link, and run `015925` accepted generic LOC Start and indications.
   Follow upstream's buffer ownership and resume ordering for later services; do not route
   EFS-Sahara through this adapter.
4. **Done on the host:** QMI handles can select a transport explicitly while the old
   `hk_qmi_set_transport()` factory remains compatible. This is required before Wi-Fi, ADSP/SLPI
   and modem can be initialized independently.
5. **Done on host and modem phone proof:** LOC's UTC, Register Events, Start/Stop, Start ACK,
   Position Report and GNSS SV Info tables are in `hk_qmi_loc.*`; the 0x2c satellites-used field
   is bounded as a uint8 count plus uint16 IDs. `tools/qmi-test/run.sh` passes 56,629 generic QMI
   checks, 13 LOC checks and 44 LOC-client checks under ASan/UBSan. Remaining work is session/
   validity/assistance policy and a consumer that keeps coordinates and identities out of logs.
6. Add only the AP services that a future cellular or locationd run proves necessary (`qrtr-ns`
   semantics, rmtfs/tqftp/pd-mapper). EFS-Sahara remains a separate read-only MHI service.
7. Keep the sensor work on the same QRTR core, but treat SLPI FastRPC/registry and real sensor
   discovery as independent gaps. Do not treat ODM JSON bus numbers as proof of an AP-side I2C
   device.

## Cellular reference evidence — 2026-11-10

- **Coachz, XNU on an HP Chromebook x2 11 tablet.** UniXNU `e2fdae4312d0acc3d8a52bc457101e7918095e6a` pins QcomBaseband `dd0199cf3300764ed52b23f4a47761f91fab21a2`. The bridge routes standard QMI requests to announced QRTR endpoints. It retains the modem's card/application state while adding eSIM capability information, and matches CommCenter's ICCID/IMSI reads to UIM replies. The author's runs distinguish SIM Ready on a card without service, LTE roaming with CS/PS attached on a working SIM, and IPv4 DNS/HTTP after a PDP reconnect. [Component pin](https://github.com/UniXNU/UniXNU/blob/e2fdae4312d0acc3d8a52bc457101e7918095e6a/components.toml#L535-L544) · [Routing](https://github.com/UniXNU/QcomBaseband/blob/dd0199cf3300764ed52b23f4a47761f91fab21a2/baseband/coachz_bb_qmux.h#L279-L321) · [Card status](https://github.com/UniXNU/QcomBaseband/blob/dd0199cf3300764ed52b23f4a47761f91fab21a2/baseband/coachz_bb_esim.h#L292-L312) · [Card reads](https://github.com/UniXNU/QcomBaseband/blob/dd0199cf3300764ed52b23f4a47761f91fab21a2/baseband/coachz_baseband.cpp#L423-L451) · [SIM and registration runs](https://github.com/chiba233/Trogdor-ipadOS/blob/58b5bc8bfc04482014ff403078435e581e58a5b0/docs/plans/modem.md#L117-L143) · [IPv4 run](https://github.com/chiba233/Trogdor-ipadOS/blob/58b5bc8bfc04482014ff403078435e581e58a5b0/docs/plans/modem.md#L198-L206).
- **POCO X5 Pro, XNU phone.** HakiOS run 30 already saw a present, ready USIM in physical slot 2. Later runs saw UMTS signal and a ready USIM on logical slot 1 with no primary GW application binding. The project subsequently reported full LTE service, with its data session still refused. These runs illustrate the distinction between discovery, subscription binding and registration. [Device runs](https://github.com/oosexgang/hakios-xnu-kernel/blob/63fc2b4633bac3af8e57dadab9f5a8d4cb940438/docs/ru/cellular.md#L230-L267) · [LTE and bearer status](https://github.com/oosexgang/hakios-xnu-kernel/blob/63fc2b4633bac3af8e57dadab9f5a8d4cb940438/docs/cellular.md#L70-L75).
- **Xiaomi Mi 10T/Pro (Apollo), postmarketOS phone.** This is a same-topology SM8250/external SDX55 Linux reference. Its maintainer reports LTE/5G registration, CS/PS attachment and bidirectional voice. Its data path uses an MHI network interface and binds WDS to the PCIe endpoint. [Hardware and AP services](https://github.com/royka1/Xiaomi-Apollo-pmOS-packages/blob/4c52c8bc8f8bd23a0e31dcd944a15f838777c3e2/README.md#L3-L17) · [Data path and hardware status](https://github.com/royka1/Xiaomi-Apollo-pmOS-packages/blob/4c52c8bc8f8bd23a0e31dcd944a15f838777c3e2/README.md#L131-L150).
- **OnePlus 8, existing XNU device receipts.** In #267 physical slot 1 reported ATR metadata, direct Get ATR returned UIM_UNINITIALIZED (37) on both slots, and extended logical status reported zero applications and no GW binding. In #275 separate IPv4/IPv6 WDS clients both reported disconnected, NAS reported unregistered and CS/PS detached, and WDA reported raw IP on PCIe. The run issued no Start Network and transferred zero packets. [#267 UIM receipt](../../../oneplus8-audit/cellular-data-next/uim-endpoints/hardware.json) · [#275 WDS receipt](../../../oneplus8-audit/cellular-data-next/family-owner/hardware.json).
- **TFTP/RFS, static comparison and open hypothesis.** The captured OnePlus slot-A vendor image contains `/vendor/bin/tftp_server` (SHA-256 `6ee43ee36de75ffd55f4d3af0a5c82bcca055b5169f038ea93d89b32d0983292`). Its init entry and executable support announcing QRTR service 4096. Raw instance field 3 maps to `/vendor/rfs/mdm/mpss`, whose `readonly/firmware` link targets `/vendor/firmware_mnt`. Apollo publishes the same service and raw instance. The Apollo maintainer reports that its older server ignored `seek` and `rsize`, returned bytes from the wrong offset, and left the modem reporting SIM absent, no selected software MCFG and DMS offline with `DeviceNotReady`. The OnePlus XNU handler replies to HELLO and records remote announcements. Whether its modem requests a file when that AP service is advertised, and whether a missing file service affects UIM startup, remain open. [Apollo offset-read report](https://github.com/royka1/Xiaomi-Apollo-pmOS-packages/commit/f41dac918bf66049e2ad2340d15a475e953591ea) · [Apollo publication](https://github.com/royka1/Xiaomi-Apollo-pmOS-packages/blob/4c52c8bc8f8bd23a0e31dcd944a15f838777c3e2/tqftpserv-sdx55/tqftpserv.c#L976-L984) · [OnePlus XNU handler](../../xnu/files/bsd/dev/hk_qrtr_core.c).

### What changed first

Coachz opened Cellular Settings while ABM still reported the modem unavailable. Its raw-IP `pdp_ip` interface came later. CommCenter also recognized the SIM before factory FSG was supplied. With this tablet’s FSG, it reported SIM Ready, IMEI and ICCID, while the test card still lacked CS/PS attachment. [Settings run](https://github.com/chiba233/Trogdor-ipadOS/blob/2844cb0b96f857215c483d962587922a74d5d955/docs/plans/modem.md#L116-L118) · [PDP implementation](https://github.com/chiba233/Trogdor-ipadOS/blob/c6794008f0362df9c5e41cf5078c7044b36951b7/platform/coachz-prelink/pending/baseband/coachz_bb_pdp.cpp#L105-L129) · [Before FSG](https://github.com/chiba233/Trogdor-ipadOS/blob/1203ad96b2201a8ccdbaef7ca8aba53381c73195/docs/plans/modem.md#L116-L118) · [After FSG](https://github.com/chiba233/Trogdor-ipadOS/blob/87522ac0208512f76a5798504464da8a807d7a74/docs/plans/modem.md#L118-L120).

On OnePlus, the #268 and #271 RAM test images excluded the stock CommCenter XPC job. These runs establish the absent Cellular row, MobileGestalt values and available IOKit providers. Normal CommCenter startup remains untested by these runs. [Device observations](../../tools/nf/stock-cellular/README.md) · [Job selection](../../userland/mkjobs.py) · [RAM image assertion](../../tools/nf/stock-cellular/build_root.py).

Coachz RMTFS exchanges 512-byte sectors over QMI service 14. In the IN2010 SBL run #274, the per-image send counters for KEFS1/2/3 printed 2047, 2047 and 0 KiB after truncation. All three IDs entered the counter on a successful nonempty send, putting KEFS3 between 1 and 1,023 bytes. Request offsets and content checksums were unrecorded. During modem operation, a TFTP read request (RRQ) would name a file. The captured OnePlus `/vendor/etc/fstab.qcom` (line 51) mounts the slot-selected modem FAT at `/vendor/firmware_mnt`, the target of the external modem’s `readonly/firmware` link. The retained text from tickets #233, #260–263, #267 and #273–275 contains neither an RRQ filename nor a TFTP error identifying a failed file. [Coachz RMTFS](https://github.com/chiba233/Trogdor-ipadOS/blob/58b5bc8bfc04482014ff403078435e581e58a5b0/platform/coachz-prelink/patches/xnu/coachz-prelink-wifi.patch#L1741-L1745) · [SBL transfer counts](../../logs/134521-run.log.live#L2174-L2186) · [Counter interpretation](next-features.md#L498) · [Vendor filesystem evidence](../../../oneplus8-audit/cellular-data-next/pdc-selected/ap-source-provenance.json).

### OnePlus 8 and OnePlus 8 Pro port evidence

On the **OnePlus 8 Pro** (`instantnoodlep`, XNU), the maintainer saw the SDX55 fail to resolve TFTP service 4096, raw instance 3, and saw the firmware search its MCFG catalog. The port corrected QRTR transmit flow control, supplied TFTP, and served that phone’s own KEFS partitions. Later runs showed LTE registration, HTTPS over cellular with Wi-Fi disconnected, and SMS sent and received. Several changes preceded recovery. Its captured UIM application was already visible in state 2 (PIN required) and later reached state 7 (ready). The transition from zero applications to the first visible application remains unobserved. A project-supplied telephony XPC service provided the iOS interface. [Pro UIM result](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L54-L56) · [Pro modem trace and data runs](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L49-L65) · [SMS receipt](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L68-L71) · [Telephony XPC service](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L96-L100).

The 8 Pro radio/data profiles (channel masks 139 and 2187) included mission-phase EFS on MHI 10/11, and the author observed an EFS sync during a five-minute download. IN2010 cellular runs #246 and #267 omitted this pair with `modem_gnss=0 modem_loc=0`, so those runs sampled different traffic after SBL. [Pro profiles](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/HANDOFF.md#L44-L45) · [Pro download observation](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L58-L60) · [#246 arguments](../../logs/072402-run.log.live#L298) · [#267 arguments](../../logs/111022-run.log.live#L298) · [IN2010 channel selection](../../tools/nf/modem/modem_boot.inc#L2547-L2556).

The IN2010 QRTR core already constructs a 20-byte `RESUME_TX`, and its Sahara receipts show KEFS1/2 data transfers. The Pro also served named files over QRTR during modem operation. The retained IN2010 windows show no such filename or service-resolution error. [IN2010 QRTR core](../../xnu/files/bsd/dev/hk_qrtr_core.c) · [IN2010 receipts](../../../oneplus8-audit/modem-power-next/discovery-window/READINESS.md) · [Pro TFTP publication](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/xnu/files/bsd/dev/instantnoodle_modem.c#L2258-L2266) · [Pro file lookup](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/xnu/files/bsd/dev/instantnoodle_modem.c#L2397-L2429).

For the **OnePlus 8** (`instantnoodle`), six LineageOS 23.2 init files and its device fstab matched the captured vendor/ODM files byte for byte. The product definition defaults to IN2015 and supplies the captured Android 13 fingerprint. The archived `androidboot.rf_version=11` corresponds to the OnePlus8 model override for IN2010 (CN). Separately, `simcardnum.doublesim=1` selects DSDS. With the QCRILD family selected, the radio script starts a second instance. A public Android radio log for this model family records home-network CS/PS registration on LTE. Its exact SKU, early UIM/PDC sequence and selected MBN are unreported. [Product fingerprint](https://github.com/LineageOS/android_device_oneplus_instantnoodle/blob/78968fcd1ba37e69436b1855c99a7e3da8738162/lineage_instantnoodle.mk#L17-L32) · [Model override](https://github.com/LineageOS/android_device_oneplus_sm8250-common/blob/d772af0971b01a6b4f9b0d265a9476df494ef280/init/init_oplus.cpp#L40-L55) · [DSDS input](https://github.com/LineageOS/android_device_oneplus_sm8250-common/blob/d772af0971b01a6b4f9b0d265a9476df494ef280/init/init.oplus.sh#L7-L10) · [Radio instance choice](https://github.com/LineageOS/android_device_oneplus_sm8250-common/blob/d772af0971b01a6b4f9b0d265a9476df494ef280/init/init.class_main.sh#L86-L125) · [Captured init and modem evidence](../../../oneplus8-audit/cellular-data-next/pdc-selected/ap-source-provenance.json) · [LineageOS issue 9806 and radio log](https://gitlab.com/LineageOS/issues/android/-/issues/9806).

### QRTR flow control and option length

**Confirmed DATA.** For an incoming datagram from modem node 3, port 58 to AP node 1, port `P`, the IN2010 ABP worker and the Pro both return `RESUME_TX` from `1:P` to `3:58`. Its 20-byte payload is five little-endian words, `[7, 1, P, 0, 0]`. Linux uses the received destination node and port in that payload to identify the flow. A datagram addressed to the AP nameserver therefore identifies the AP control port in the payload and returns the reply to the original sender's port. The Pro also sends a copy to node 2 when the original sender is another node. The published runs do not isolate its effect on modem flow control. The IN2010 legacy fallback fixes `P` at `0x4000`. The ABP worker uses the received destination port. [IN2010 ABP worker](../../xnu/files/bsd/dev/hk_abp_nas_worker.c#L195) · [IN2010 core](../../xnu/files/bsd/dev/hk_qrtr_core.c#L347-L351) · [Legacy fallback](../../tools/nf/modem/modem_boot.inc#L2239-L2246) · [Pro reply](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/xnu/files/bsd/dev/instantnoodle_modem.c#L2576-L2591) · [Linux reply](https://github.com/torvalds/linux/blob/b3dfbde62cf1981c4638939c159dfd087b7a72e9/net/qrtr/af_qrtr.c#L1110-L1134).

**QRTR v2 options.** In the compact wire header (version byte 3), Linux, the OnePlus Android 13.1 kernel and the Pro locate the payload at `16 + optlen` bytes. The decoder frozen for IN2010 ticket #276 used `16 + 4 * optlen`. Its exact-length check rejected nonzero options framed by those references. After #276, the maintained decoder and both XNU source trees adopted the byte count and four-byte datagram alignment. Host checks placed the payload at offsets 16, 20 and 24 for 0, 4 and 8 option bytes. The existing wire suite passed all 18 checks and the QRTR core compiled with the NF ARM64 flags. The corrected decoder has host verification only. In #274 the adapter counted 51 packets in, four out, 47 live services and zero drops. The trace provides no evidence that a nonzero option length accounted for the missing PDC software reply. [Linux decoder](https://github.com/torvalds/linux/blob/b3dfbde62cf1981c4638939c159dfd087b7a72e9/net/qrtr/af_qrtr.c#L493-L505) · [OnePlus Android decoder](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8250/blob/1d2678a3548f4825447f17b25a5cc1754c4cd1ae/net/qrtr/qrtr.c#L845-L855) · [Pro decoder](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/xnu/files/bsd/dev/instantnoodle_modem.c#L2485-L2496) · [IN2010 decoder](../../xnu/files/bsd/sys/hk_qrtr_wire.h#L105-L133) · [IN2010 fixture](../../tools/qrtr-test/test_wire.c#L35-L43) · [#274 adapter counters](../../../oneplus8-audit/cellular-data-next/pdc-native-selected/hardware.json#L389).

### Data format and WDS client binding

IN2010 runs #227 and #275 queried the existing format with WDA Get Data Format (0x21) on PCIe interface 4. Both returned raw IP (link 2), QoS off and zero aggregation in both directions. In #275 each WDS client acknowledged Set IP Family (0x4d) and reported disconnected. The run issued neither Bind Mux Data Port (0xa2) nor Start Network (0x20). Track the selected endpoint/mux and its ACK separately from the family ACK. The current XNU raw-IP interface accepts only unaggregated traffic. These bearer checks take place downstream of UIM application discovery; the zero-application result remains unexplained. [#227 format](../../out/eye/data-format-ticket-227/result.json) · [#275 exchanges](../../../oneplus8-audit/cellular-data-next/family-owner/hardware.json) · [Client state machine](../../tools/nf/modem/mdm_family_owner.inc) · [Interface admission](../../tools/nf/modem/hk_cell_rawip.inc).

On the **OnePlus 8 Pro**, WDA Set Data Format (0x20) succeeded with link 2 and QMAP aggregation 5 in both directions. WDS then bound PCIe interface 4/mux 1, selected IPv4 and started the bearer. The maintainer observed four-byte QMAP headers and fetched HTTPS over cellular. **Apollo** reported Start Network error 70 without a bind and a connected bearer after binding the same WDS client to PCIe interface 4/mux 0. A separate Apollo report describes raw-IP downlink stalling after about 10 MB and sustained transfers with QMAP multiplexing. The mux-0 transcript omits its WDA negotiation. **Coachz** translates the requested endpoint to its embedded IPA port and negotiates QMAPv4 for that transport. [Pro sequence and HTTPS](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L58-L64) · [Apollo bind comparison](https://github.com/royka1/Xiaomi-Apollo-pmOS-packages/blob/4c52c8bc8f8bd23a0e31dcd944a15f838777c3e2/modemmanager-patches/UPSTREAM-NOTES.md#L20-L42) · [Apollo sustained transfers](https://github.com/royka1/Xiaomi-Apollo-pmOS-packages/blob/4c52c8bc8f8bd23a0e31dcd944a15f838777c3e2/apollo-modem-support/zz-apollo-multiplex.conf#L2-L14) · [Coachz endpoint and format](https://github.com/UniXNU/QcomBaseband/blob/dd0199cf3300764ed52b23f4a47761f91fab21a2/baseband/coachz_bb_data.h#L4-L15).

### Receive paths on the OnePlus ports

**Mobile data.** The Pro loops over complete QMAP records in each channel 101 buffer, strips padding, skips command frames and delivers IPv4 or IPv6 to one `en1` without selecting a bearer by mux. The maintainer saw one record per transfer in that test. On Orange’s IPv6-only profile, starting IPv6 on mux 2 first gave iOS both address families through NAT64/CLAT46 on one bearer. Coachz IPA decodes each record using its configured data format and uses the mux to select a `pdp_ip` interface. IN2010 accepts one complete raw IP packet per EOT. Its #266 receive buffer returned no packet. [Pro RX](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/xnu/files/bsd/dev/instantnoodle_modem.c#L4124-L4151) · [Pro transfer count](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L60-L65) · [Pro IP families](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L79-L81) · [Coachz unpacking](https://github.com/UniXNU/QcomBaseband/blob/dd0199cf3300764ed52b23f4a47761f91fab21a2/ipa/coachz_ipa_modem.c#L70-L89) · [Coachz mux selection](https://github.com/UniXNU/QcomBaseband/blob/dd0199cf3300764ed52b23f4a47761f91fab21a2/baseband/coachz_bb_pdp.cpp#L101-L109) · [IN2010 RX](../../tools/nf/modem/hk_cell_rawip.inc#L701-L716) · [#266 receipt](../../../oneplus8-audit/cellular-data-next/ip-hw0-rx/README.md#L21).

**SMS.** The Pro received a bare 3GPP TPDU inside WMS event TLV `0x11`. The alternative TLV `0x10` identifies a stored message by location and index for a subsequent Raw Read. IN2010’s #263 observer passes the inline bytes directly to a matcher that expects a leading SMSC length, so it could miss the text of an inline message. The event counter runs before the matcher and stayed at zero in #263. [Pro WMS record](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L68-L75) · [IN2010 event handling](../../../oneplus8-audit/cellular-data-next/late-sms/wms.inc#L26-L45) · [Marker format](../../../oneplus8-audit/cellular-data-next/late-sms/marker.h#L1-L10) · [#263 receipt](../../../oneplus8-audit/modem-power-next/discovery-window/READINESS.md#L41).

**Voice.** The Pro established a 3G call before it had working audio. With the ADSP–modem MHI path running, the owner heard one call on the loudspeaker. The record leaves open whether the far end heard the caller, whether the earpiece produced sound and whether VoLTE registered. The IN2010 device tree describes the voice mailbox and ADSP offload channels. Its XNU ADSP driver lists IPCRTR and audio APR, with no corresponding `mhi_sat` channel or demonstrated call audio. [Pro call and ADSP record](https://github.com/dekefake/hackios-op8pro/blob/e5631cd2523a8a7a897618a3535fe2a08b1672dc/docs/research/modem-sdx55.md#L109-L122) · [Android voice mailbox](https://github.com/LineageOS/android_kernel_oneplus_sm8250/blob/078365e5318de0a30a41f9707070b610bcd20f13/techpack/audio/dsp/voice_mhi.c#L355-L403) · [IN2010 ADSP channels](../../../oneplus8-audit/device-tree.dts#L11844-L11864) · [IN2010 voice mailbox](../../../oneplus8-audit/device-tree.dts#L25682-L25689) · [IN2010 XNU channel table](../../xnu/files/bsd/dev/hk_adsp.c#L1918-L1924).
