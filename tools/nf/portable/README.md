# IN2010 portable GNSS

Use the private USB-only loader, kernel and RAM overlay through the one shared FIFO. A normal loader writes the logdump partition during its flushes, even when XNU receives `ufs_write=0`. The portable loader builds its log in RAM, denies its normal flash/installed-boot commands and permits exactly one XNU boot. XNU has physical data writes disabled. A complete power loss discards the on-device result.

## Host verification

From the kernel checkout run the following hardware-free checks.

```sh
python3 tools/nf/portable/transport.py
python3 -m unittest discover -s tools/nf/portable
clang -Wall -Wextra -Werror -fsanitize=address,undefined tools/nf/portable/test_http.c -o /tmp/portable-http-test && /tmp/portable-http-test
clang -Wall -Wextra -Werror -Wno-unused-variable -fsanitize=address,undefined tools/nf/portable/test_gate.c -o /tmp/portable-gate-test && /tmp/portable-gate-test
tools/t host
```

Ticket 253 physically confirmed the rendered page, arm, USB disappearance, original-port fresh re-enumeration, same boot identification, clean timed return and verified stock recovery. The owner reported a rejected Start and that run had no completion line. Its original handler did not preserve the individual refusal reason. The radio remained off. Ticket 256 then physically measured USBIN absent while the UEFI still reported connected. The corrected foreground button waited for physical USBIN and accepted the detached tap. The host recovered the completion in a fresh enumeration of the same port, obtained an orderly XNU return and verified fresh stock ABL. Bootq ended with rc 0. Both receipts live under `out/portable-gnss/ticket-N/`. The modem remained off in both rehearsals. A satellite result requires a separately armed outdoor run.

The current versions also retain enumerated control/HTTP rejection reasons when an action fails.

The C tests exercise the actual phone gate and HTTP parser. The Python tests use synthetic endpoints, retained logs, the real packet builder and a private bootq with a dead producer and an owner-priority waiter. The local Linux builder links the arm64 XNU, builds the loopback helper, and checks all 575 files using two APFS readers. Rebuild independently with `build_loader.py` and `build_phone.py`. Check the resulting private hashes against the pinned profile before scheduling.

## One shared hardware rehearsal

1. Coordinate with the other workers and the owner. The prerequisite is the actual IN2010 in stock ABL FASTBOOT on its usual USB port. Leave an existing authorized ordinary recovery at stock or have the owner enter stock using its physical controls. An ordinary Developer session, another stock serial, multiple devices or an already running kernel cannot be used as the starting state. Run `python3 tools/nf/portable/transport.py --run rehearsal --after-stock N` outside bootq. N identifies a successful stock transition ticket or a previous accepted portable ticket that produced this exact stock USB session. To make the separate ordinary transition after a designated preceding hardware case, use `python3 tools/nf/portable/stock_handoff.py --after PREDECESSOR`. It performs one verified ordinary reset and leaves stock FASTBOOT. The portable worker submits and owns its own ordinary FIFO ticket.
2. Leave USB attached until the **current** ticket prints the Chinese unplug cue. A private 128-bit boot identifier, matched effective boot arguments, loader identity and a ready marker from a visible Safari page are required. Safari opens the phone's `http://127.0.0.1:8765/`. The host and the phone have separate loopbacks. The helper's status and page GETs cannot start GNSS. Page visibility can only arm the controls.
3. For the first rehearsal, unplug, tap **开始测量** on the phone, check the explicit rehearsal-complete message, then reconnect to the **same USB port**. Wait for **已确认拔线，可以点击开始**. The start button stays disabled while the board reports cable presence or the status is unavailable. A second fresh reading of the existing PM8150B USBIN status is required at Start. The UEFI event generation is independently recorded for diagnosis. Rehearsal uses `modem=0` and never creates a modem worker. The XNU run has a five-minute limit. The computer must have witnessed absence between the two USB enumerations.
4. The host accepts the same run's retained marker in either the live loader or its returned log. It verifies the original port, a fresh USB session/registry entry, the portable loader and exact boot nonce. A still-running matched phone returns early only after its completed rehearsal or finished modem teardown. A phone that returns while disconnected keeps the final log in the loader's RAM. A verified fresh stock ABL endpoint releases the queue. The host stores the coordinate-free result with mode 0600 in `out/portable-gnss/ticket-N/result.json`.

## Outdoor run and recovery

After a passing physical rehearsal, schedule `python3 tools/nf/portable/transport.py --run gnss --after-stock N`. Wait for its own fresh unplug cue, carry the phone outside and tap **开始测量** only once ready. Start is available for about thirteen minutes from kernel initialization. Allow up to three minutes in modem mission plus firmware start/teardown. The XNU run ends by its twenty-minute timer. **提前结束** requests an orderly stop. A valid session can finish with no fresh fix. The device and host summarize acknowledged Start, completion, report count, and whether a validated satellite-source fix arrived. Neither interface exports coordinates. The host does not infer the outdoor environment from the USB state.

The host durably reserves the shared phone *before* staging the measurement. The existing kernel heartbeat/watchdog stays armed. No-answer, power loss, a stale ID, unsafe shutdown, or an outstanding hardware reset retain the hold. The initial observation ends after the XNU limit plus four minutes. If USB stays out beyond that deadline, or the host process died, run `python3 tools/nf/portable/transport.py --resume N --seconds 300` after reconnecting. Resume can collect only this original ticket. It cannot stage another measurement. If a live result is captured before the return, it is retained privately in `session.json`. Acceptance still requires a clean terminal state and verified stock reset. If missing or damaged end evidence prevents collection, the existing queue operation `tools/bootq recover --ticket N` performs its separately scoped established verified cold recovery. That operation loads the **ordinary** Developer image with its ordinary diagnostic logging. `bootq cool off` cannot release an evidence-bearing hold.

Source paths are `xnuloader/loader.c` (USB event pump, USB-only loop, staged logs, kernel heartbeat monitoring), `xnu-src/nf-tree/xnu-8796.121.2/osfmk/arm64/loader_return.c` (continuous-time return and quiescence), the private patch and private modem build in this directory. The portable loader composes its returned log once and the dedicated read command preserves it. Ordinary `oem log` on an idle loader composes a loader-only slice. `oem lastlog` reads a prior logdump and is unsuitable for this procedure.
