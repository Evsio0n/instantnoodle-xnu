# QMI radio read-only plan — 2026-11-07

This is a host-only design note for the next SDX55 checks. It does not add a
modem boot path, send a QMI request to the phone, read SIM files, or change any
NV/EFS state.

## IN2010 results on November 10

RAM runs 022044 and 022817 returned all three original status fields through the 120-byte iOS ABI. DMS replied offline (3), NAS replied interface none (0), and WDS replied disconnected (1). On the second run the consumer observed endpoint invalidation after modem teardown. The DMS response omitted both optional offline reason and hardware restriction fields. GNSS run 022044 produced 150 position-status reports without a fix. The antenna environment was not recorded.

The level-2 private diagnostic additionally requests NAS Get Serving System (0x0024) once. Only a validated registration enum, CS/PS attachment state and counts/mask of radio interfaces enter the log. The public 120-byte ABI is unaffected. The primary schema is [libqmi NAS](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/1.32.0/data/qmi-service-nas.json).

## PCIe cellular data format on the IN2010

The captured device tree labels MHI channels 100/101 as `IP_HW0`. The corresponding data port has an MRU of 32768 bytes and uses event rings 7 and 8. The original OnePlus driver recognizes IPv4 and IPv6 at the start of a received buffer and otherwise passes data to RMNET as MAP. Both raw IP and multiplexed framing are possible. [OnePlus MHI data driver](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8250/blob/cad09e061ef6cf689a4a1e54d27562e0f042236a/drivers/bus/mhi/devices/mhi_netdev.c)

Run 071154 advertised WDA service 26. Diagnostic `modem_radio=4` adds a Get Data Format request for the Qualcomm PCIe endpoint (type 3, interface 4). It retains the QoS flag, link protocol and separate uplink/downlink aggregation modes, including presence for each field. Kernel c19024b4 is linked and the exact request passes the host codec tests. Ticket 227 confirmed acceptance and reported Raw IP, QoS off and no aggregation in either direction. An active WDS bearer, data-channel event/doorbell support, and a working iOS data interface remain necessary for application traffic. [libqmi WDA schema](https://github.com/linux-mobile-broadband/libqmi/blob/1.32.0/data/qmi-service-wda.json) [ModemManager PCIe endpoint selection](https://github.com/linux-mobile-broadband/ModemManager/blob/1.22.0/src/mm-port-qmi.c)

## Card reset evidence

After the owner inserted one SIM, ticket 230 (053345) obtained three consistent UIM reports over 27.379 seconds. A logical slot reported error code 3, *no ATR received*. Another reported absent. The modem had no ready applications or carrier registration. This status cannot itself distinguish card contact, slot activation and electrical initialization. The next read uses Get Slot Status (0x0047) to retain physical card state, slot activity, logical slot, card protocol and only the presence of ATR bytes. The decoder skips the identifier and ATR contents. [libqmi UIM schema](https://github.com/linux-mobile-broadband/libqmi/blob/1.32.0/data/qmi-service-uim.json) [Card error definitions](https://github.com/linux-mobile-broadband/libqmi/blob/1.32.0/src/libqmi-glib/qmi-enums-uim.h)

Real modem service announcements include TS 23 v1 instance 100. The IN2010 device tree names `modem_tsens` for that instance. The candidate enumerates the modem's list before making immediate, sequence-tagged temperature requests without thresholds. An accepted request and a matching current-temperature indication from that endpoint are both required to report a value. [Native OnePlus thermal client](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8250/blob/cad09e061ef6cf689a4a1e54d27562e0f042236a/drivers/thermal/qcom/qmi_sensors.c)

## What the existing client can support

`sys/hk_qmi.h` already provides table-driven QMI encoding/decoding,
transactions, explicit QRTR transports and service lookup. The LOC work proves
that a service-specific table file plus an ASan/UBSan fixture is the smallest
safe unit in this tree. The public schema reference is libqmi's service data:

- `qmi-service-dms.json`
- `qmi-service-nas.json`
- `qmi-service-wds.json`
- `qmi-service-uim.json`

The current tree should copy only the field shapes needed by a diagnostic
table. It should not copy a userspace daemon or expose identifiers in logs.

## First read-only messages

These requests have no modem-side write and are useful to establish that the
service is present and returning decodable data. The service numbers are the
standard QMI service identifiers used by the public schemas.

| Service | Message | Request | Response fields to retain | Why first |
|---|---:|---|---|---|
| DMS (2) | Get Model `0x0022` | none | common result; TLV `0x01` model string | establishes DMS and firmware-facing response coding; raw model must stay out of normal logs |
| DMS (2) | Get Operating Mode `0x002D` | none | common result; TLV `0x01` mode; optional `0x11` hardware restricted | tells whether the modem is online/offline without changing the mode |
| NAS (3) | Get Signal Strength `0x0020` | TLV `0x10` request mask (`uint16`) | common result; TLV `0x01` primary strength (`int8`) and radio interface (`int8`); optional lists can be added later | gives a bounded RF/registration-adjacent read without network registration |
| WDS (1) | Get Packet Service Status `0x0022` | none | common result; TLV `0x01` connection status | confirms the data service state without starting a bearer |

For the first tables, unknown TLVs must remain skippable and every response
must require the common result TLV. A missing optional field is a valid
response; a malformed length, overlong string, or an out-of-range array count
is a decode failure. The model string should be retained only in a bounded
buffer for a caller that explicitly asks for it.

## Why UIM waits

UIM Get Card Status (`service 11`, message `0x002F`) is read-only, but its
response contains nested card, application and PIN-state arrays. A partial
table could silently misinterpret slot/application state, and a careless
diagnostic could print ICCID/IMSI-like values. The first UIM step should be a
complete bounded card-status table with a summary-only caller:

1. decode all count prefixes with strict caps;
2. report only slot count, card state and application-state enums;
3. never print ICCID, IMSI, ATR, PIN retries or raw application identifiers;
4. add a fixture for one physical SIM and an empty/absent slot before any phone
   request is allowed.

Do not implement UIM Read Transparent, Change PIN, or any provisioning request
as part of this milestone.

## Host-only gate before phone use

The implementation, when started, must remain behind these checks:

1. add one schema source/header and one fixture per service, without changing
   `modem_boot.inc` or the modem boot arguments;
2. test request bytes and response decoding with ASan/UBSan;
3. test malformed common-result TLVs, truncated strings, unknown TLVs and
   oversized arrays;
4. run `bash tools/qmi-test/run.sh` and `./iphont test host`;
5. review logs to ensure no model, SIM or subscriber identifier is printed;
6. only then add a diagnostic query behind an explicit boot argument, with a
   timeout and a clean modem stop.

The QMI handle must be attached to the modem's existing QRTR/MHI transport
after mission and service discovery. Do not create another QRTR userspace
daemon or route EFS-Sahara through QMI. A phone run is not justified by a
passing fixture alone; it needs an owner-present, notified RAM-only run and a
fresh modem startup state.

## Deferred data path

WDS Start Network (`service 1`, message `0x0020`) is deliberately deferred.
Although it is the public path to a packet-data handle, it takes an APN and
may carry credentials. A future data run must receive an explicit APN from the
owner, keep credentials out of source and logs, and add the RMNET/IP-HW0 data
path before sending the request. No WDS start/stop request belongs in this
read-only milestone.

Reference schemas: [libqmi DMS](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-dms.json),
[NAS](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-nas.json),
[WDS](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-wds.json),
and [UIM](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-uim.json).

The separate raw-IP network boundary in `../oneplus8-audit/cellular-data-next/raw-ip-ifnet/` cross-compiles as an actual NF ARM64 XNU object. It can accept real family-bound WDS settings and raw datagrams only after matching WDA, a connected WDS family/MTU/local address, and proven IP_HW0 100/ER7 and 101/ER8 readiness. The #227 WDS disconnected snapshot yields no ifnet. It is not attached or linked in the active modem image; no IP transfer or cellular route has been demonstrated. See its README for ownership, lifecycle, isolated host tests and future binding requirements.
