# Sensor AP-side audit — 2026-11-07

This began as a host-only audit. The resulting read-only AP probe was later run on the phone as
`020629` (bootq #157); its evidence is recorded below.

## What the ODM data actually says

The parsed files under `../oneplus8-audit/als-native/odm-sensor/config/` contain several alternatives for Kona/SM8250. The candidate with `bus_instance=5` and `slave_config=70` (decimal `0x46`) is present in:

- `kona_qrd_alsps.json`, as `alsps_platform`;
- `kona_stk2232_0.json`, as `stk2232_0_platform`.

Both use `bus_type=0`, 400 kHz, IRQ 136, and two rails named `sensor_vdd` and `sensor_vddio`. This is an ODM candidate, not proof of the fitted part. The repository has no live chip-ID read for this address.

The BMI26x alternatives are different: `bus_type=1`, `bus_instance=3`, `slave_config=0`, and up to 9600 kHz. They must not be treated as the same I2C device. The matching Kona FDT has `qupv3_se3_spi` at `spi@98c000` with GPIO119..122, but both the SPI and matching I2C nodes are disabled; the AP tree has no SPI transfer implementation and no BMI node. `bus_type=1` therefore strongly maps to the SLPI/SEE SE3 logical bus, but it is not enough to invent an AP-side SPI probe. Its two rails (`/pmic/client/sensor_vdd` and `/pmic/client/sensor_vddio`) are SEE/PMIC registry names, not AP-owned regulators.

## Existing AP-side bus support

`xnu/files/bsd/dev/instantnoodle_i2c.c` already supports the OnePlus 8 serial engine used by the alert slider. The matching entry is QUP 0 SE 1 at `0x984000`, GPIO 4 (SDA), GPIO 5 (SCL), SPI 602, 400 kHz. The source calls this bus `se5`; this is the physical bus that the ODM `bus_instance=5` candidate most likely names.

`instantnoodle_slider.c` owns a static `struct instantnoodle_i2c` for those pins and uses `instantnoodle_supply_on("ldoc10", 3000)`. It also owns the bus lock. A new sensor file must not create a second struct for GPIO 4/5: that would create a second lazy mutex and allow concurrent transfers on one physical bus. The probe should share the slider bus owner, or the bus object/lock should be factored into a single common owner first.

The I2C implementation has GENI FIFO support behind `i2c_geni=2`, clock voting, and nine-clock recovery. Those paths are host-built and already used by the slider; no new controller implementation is needed for the first probe.

## Safe first probe

The first phone probe should be a boot-argument-gated, RAM-only diagnostic that:

1. does not start SLPI/SEE;
2. does not write a sensor register or issue reset/configuration commands;
3. records the GPIO4/5 configuration and idle levels;
4. does not claim or change a rail; if the existing slider path later owns `ldoc10`, that remains its responsibility;
5. performs bounded read-only transactions at `0x46` using only registers whose identity is established from the selected chip's public driver/datasheet;
6. logs address, register, ACK/error, and returned byte values only;
7. performs at most one bus recovery after a failed transaction and restores the bus/pins before exit.

The host tree now contains this first diagnostic as `sensor_probe=1` in
`xnu/files/bsd/dev/instantnoodle_slider.c`. It shares the slider's static bus object, waits until the
existing slider thread has enabled its `ldoc10` rail, logs GPIO state, sends an address-only ACK at
`0x46`, and reads four raw bytes starting at register zero. It deliberately does not name the returned
bytes or change a rail. The owner-present run `020629` executed this ordering and still got no ACK,
including after one recovery retry; the result is recorded below.

The static contract check passes 8/8 (boot-arg gate, address, ACK path, raw read, one recovery,
no rail call, no sensor-register write, and shared bus owner). `git diff --check` is clean. A full
private XNU object build is not available on this macOS checkout because the generated Makefile points
at the Linux `/build` path; this is a host-build limitation, not phone evidence.

The owner-present RAM-only run `020629` (bootq #157) executed the probe after `ldoc10` was enabled.
GPIO4/5 reported the expected idle/configuration values, but address `0x46` returned error 6; one
recovery and retry also returned error 6. The established slider devices at `0x18` and `0x19`
continued to answer. This is evidence against this candidate under the current AP bus and rail
sequence, not proof that the fitted ALS/PS part is absent. The next decision depends on identifying
ODM `bus_type=1` for the BMI26x alternatives; no arbitrary-address scan is justified.

Offline SLPI firmware strings provide the missing bus-type link: `sns_com_port_spi.c`,
`spi_full_duplex`, `/dev/spi`, `sns_bmi26x_sensor.c` and `spi4` appear in the shipped SLPI image,
while the matching `sns_com_port_i2c.c` is a separate path. This makes ODM `bus_type=1` a SEE SPI
com-port selection, not the AP's existing I2C driver. The same firmware contains
`sns_pwr_rail_service.c`, `/see/rail/eLDO`, `sensor_vdd` and `sensor_vddio`, plus BMI26x island
power-state handling; the SEE rail service must own those votes and its delayed power-off sequence.
The host-side bus audit found no AP-safe follow-up: `qupv3_se3_spi` and `qupv3_se3_i2c` are both
disabled in the Kona FDT, and GPIO123 is used by an unrelated `ch101_1_irq` group. The next sensor
step must therefore be a controlled SEE/SLPI-side read-only probe after the SMEM stage, with the
owner present for the required cold-reset handling. Do not toggle the named rails from AP code.

Do not label a returned byte as BMI260 or STK33502 until a chip-specific ID value is read and matched. If the 0x46 transaction NACKs, that is useful evidence that the selected ODM alternative is not fitted or is unpowered; it is not a reason to scan arbitrary addresses.

## Current blocker

The SLPI path still requires a cold-reset/owner-present hardware run after any SLPI attempt. The host QRTR/SLPI/registry checks pass, but the prior phone SEE lookup returned an empty SUID list. Therefore the AP-side probe should remain independent and read-only until the bus and chip identity are proven.

## Native ALSPS dependency inspection, 2026-11-10

IN2010 SLPI segment 16 SHA-256 `f8dca2513f7c8169355265b09fc8023fa478afdf6786e606233f01499c9bcfc0` contains the ALSPS sensor's SUID request sequence at VA `b21fcfac..b21fcffc`. The five actual calls name interrupt, timer, accel, resampler and registry. Each length includes the terminating NUL. A separate return dispatcher recognizes all five at `b21fbb30..b21fbbac`. This establishes that the driver requests these dependencies. It does not establish which are mandatory, whether the physical chip was probed, or whether every internal service is exposed to AP clients. The six adjacent native registry READs cover platform, ALS and proximity configs plus calibration groups. Both the HAL and captured firmware also contain the distinct name wise_light. A returned SUID alone cannot identify its physical backing or lux units.

Hardware #205 completed four correctly named registry reads over the bound client. Registry/timer were visible and the late accel, gyro, ambient_light, proximity and mag lists were empty. The private rpc-std RAM candidate adds a bounded post-registry observation for interrupt, resampler and wise_light. It reuses the live binding, subscribes to all SUIDs for these three names, and records accepted requests, explicit failures and timestamped lists independently. See `../oneplus8-audit/als-native/physical-init-next/` for the repeatable instruction/literal check, independent raw log decoder, exact image identities and host tests.

## OPlus SMEM board description, hardware 212–213

Run 024850 found the interrupt and resampler SUIDs; wise_light returned an empty list. The native hardware-start path depends on the timer SUID and internal platform-registry decode. The subsequent OPlus hardware-identification helper expects a global item130 containing the selected board's sensor vectors. The running DSP expects three 184-byte sources per 556-byte vector and checks its type ID before selecting a chip. These dimensions independently match the OP8 13.1 OEM source. Run 025014 (read-only) found one generic item of the expected 6792 bytes. Both ALS and proximity vector IDs and all inspected source headers were zero, so the firmware's match fails on that snapshot. The private producer trial applies only the 17 words obtained by running the matched OEM ALS/prox parsers against the captured DTB. It is separately opt-in, preserves neighboring groups, and restores its changed words after confirmed shutdown. See `../oneplus8-audit/als-native/physical-init-next/item130/EVIDENCE.md`.

### IMU board and coordinate declarations

The IN2010 DT selects BMI260 via the original gsensor@0 (name=8, direction=1, type=0). In the captured BMI26x implementation the direction-1 table at b2333040 contains the inversion and target-axis selectors equivalent to register entries X=−Y, Y=+X, Z=+Z. This source-derived map awaits controlled physical orientation verification. Original #213 measured the type0 headers zero; #218 returned empty accel and gyro lists. Private one-word SMEM130 RAM pins 03e34b73 (attributes) and ae746c70 (minimum advertised rates, capped at six XYZ events per source) are described in the sensor audit's item130/imu directory. Only measured raw-correlated per-source samples are eligible for the experimental matrix row. No UI rotation has been demonstrated by the source analysis or host fixtures.

### Physical light and motion receipts, #221, #225, #226

#221 (050026) identified STK33502, available and physical, and twelve real five-float wise_light events all zero in all fields. The bounded kernel retained six. Native producer emits fixed HIGH and two reserved zero fields. #225 (051505) observed both high-accuracy near/far states from the same package's proximity source. #226 (051812) read both physical BMI26x XYZ streams on distinct UIDs, six samples each; requested minimum rates 12.5 and 25 Hz, both firmware-configured at 25 Hz. The one-word SMEM provision and teardown verified. The first accel norm was 9.7733, but controlled movement and iOS rotation remain unverified. The original #226 matrix attempt refused promotion after the overall packet cap filled, though all retained samples matched the raw prefix. The next light qualification pin b698bd56 with APFS ca89a81e correlates an iOS read-only observation, true coefficient consumption and two actual factory registry groups.
