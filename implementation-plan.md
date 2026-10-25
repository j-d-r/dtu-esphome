# ESPHome Hoymiles DTU - Implementation Plan

This project targets Hoymiles **HM-series** microinverters over an nRF24L01+
radio and **HMS-series** microinverters over a CMT2300A radio. HMT (3-phase)
remains out of scope.

The communication references are Ahoy and OpenDTU. Keep this repo
ESPHome-native: use ESPHome SPI, component lifecycle, logging, sensors, and
config schemas rather than porting upstream application structure.

## Current State

Implemented as of 2026-07-01:

- `hoymiles_dtu` hub with ESPHome SPI, `cs_pin`, `ce_pin`, optional `irq_pin`,
  `update_interval`, `pa_level`, and one or more inverter definitions.
- nRF24 probe/init using 1 MHz SPI mode 0, 250 kbps RF, CRC16, dynamic payloads,
  5-byte addresses, and the HM channel set.
- Standalone link acquisition (Ahoy-style burst retransmit) + realtime telemetry
  request and parser, **verified on a live HM-1200**.
- All HM channel families like Ahoy: 1-channel HM-300/350/400, 2-channel
  HM-600/700/800, 4-channel HM-1000/1200/1500, via a table-driven per-model
  decoder (`hm_model_layout` in `protocol.cpp`).
- Active-power-limit DevControl command with inverter ack verification.
- `sensor` platform for AC values (incl. reactive power / power factor), DC
  channel values, and yields.
- `text_sensor` platform for status/last-seen and radio diagnostics.
- Native C++ unit tests (`tests/native/test_protocol.cpp`) exercising the real
  decoder for 1/2/4-channel layouts plus a real captured HM-1200 record, run via
  `pytest`; Python protocol/CRC and ESPHome config tests.
- Compile fixtures for ESP32/ESP32-S3 on Arduino/ESP-IDF, plus a multi-model
  fixture.
- Manual HIL config/checklist in `tests/manual/hm4_readonly/`.

Added 2026-09-30 (code complete, HIL pending - see Phase 2b):

- `radio:` selector on the hub: `nrf24` (HM) or `cmt2300a` (HMS). The
  radio-independent half of the hub is `HoymilesDtuBase` (`dtu_base.{h,cpp}`).
- CMT2300A driver (`cmt2300a.{h,cpp}`) over 3-wire SPI, with the
  per-band register banks, dBm TX power, RSSI, and a register self-test.
- CMT state machine (`dtu_cmt.{h,cpp}`): single-frequency exchange, software
  DTU-id filtering, retransmits, and channel-change binding for an inverter on
  its boot frequency.
- HMS model families and record layouts, the HMS power-limit control word, the
  channel-change request builder, and per-inverter `signal_strength` (RSSI).
- HMS compile fixtures (`test.hms-cmt{,-idf}.yaml`) and a manual HIL checklist
  in `tests/manual/hms_cmt/`.

Changed 2026-10-25:

- The single `hoymiles_dtu` component was split into three ESPHome components.
  ESPHome only compiles sources that sit directly in a component's folder, so
  they are siblings, not subfolders:
  - `hoymiles_dtu_base`: protocol, `HoymilesDtuBase`, sensor/text_sensor
    platforms, `radio_set_power_limit`.
  - `hoymiles_dtu_rf24`: the nRF24 hub.
  - `hoymiles_dtu_cmt`: the CMT2300A driver and hub.

  The `radio:` selector is gone. The YAML block names the radio instead
  (`hoymiles_dtu_rf24:` / `hoymiles_dtu_cmt:`). Entity platforms are
  `platform: hoymiles_dtu_base`, and the actions are
  `hoymiles_dtu_base.radio_set_power_limit` and
  `hoymiles_dtu_rf24.scan_inverters`. This is a breaking YAML change.
- The hub classes were renamed to match: `HoymilesDtuComponent` is now
  `HoymilesDtuRf24`, and `HoymilesDtuCmtComponent` is now `HoymilesDtuCmt`.
- The hub files were renamed the same way: `hoymiles_dtu.{h,cpp}` is now
  `hoymiles_dtu_rf24/dtu_rf24.{h,cpp}`, and `cmt_hub.{h,cpp}` is now
  `hoymiles_dtu_cmt/dtu_cmt.{h,cpp}`.

## Scope

In scope:

- HM-300, HM-350, HM-400, HM-600, HM-700, HM-800, HM-1000, HM-1200, HM-1500
  over nRF24L01+.
- HMS-300/350/400/450/500, HMS-450/500 v2, HMS-600/700/800/900/1000,
  HMS-1600/1800/2000 over CMT2300A.
- Read-only telemetry first.
- Later: power limit, inverter enable/disable, restart, and multi-inverter
  scheduling, if protocol testing confirms reliable command handling.

Out of scope:

- HMT (3-phase) inverters: they report six DC channels and three-phase AC
  voltages/currents, which neither `HmTelemetry` nor the sensor platform models.
  Adding them means extending both, not just a layout table.
- Zero-export control loops beyond exposing safe ESPHome entities/actions.
- Vendoring incompatible upstream source into this GPL-only repo without a
  separate licensing decision.

## Phase 1 - HM4 Read-Only HIL

Goal: prove the current HM-1200/HM-1500 4-channel implementation against the
ready ESP32-S2+nRF24 rig and a live inverter.

Tasks:

1. Flash `tests/manual/hm4_readonly/ahoy-dtu-hm4-readonly.yaml`.
2. Confirm boot logs report `nRF24 radio ready`.
3. Confirm all configured sensors publish within 60 seconds while producing.
4. Compare AC/DC values against Ahoy within +/-2%.
5. Force no production and confirm `status` becomes `offline` after timeout.
6. Run 100 reboot cycles without SPI deadlock.
7. Run a 12-hour soak and check heap/log stability.

Acceptance:

- HM4 live telemetry works on real hardware.
- Values are close enough to Ahoy for the same inverter/radio conditions.
- Failure modes are visible in logs and do not wedge the ESP.

## Phase 2 - Parser Coverage For HM Models

Goal: support the common HM 1-, 2-, and 4-channel model families with shared
protocol code and model-specific payload layouts.

Status (2026-07-01): done in code. All three families are modelled by
`hm_model_layout` in `protocol.cpp` (offsets from Ahoy `hm{1,2,4}chAssignment`)
and unit-tested in `tests/native/test_protocol.cpp`, including a real captured
HM-1200 record. Live HIL for a 1-channel and 2-channel inverter still pending
hardware.

Tasks:

1. Add model metadata for channel count and payload offsets.
2. Add parsers for 1-channel, 2-channel, and 4-channel HM realtime payloads.
3. Add captured-packet fixtures under `tests/fixtures/packets/`.
4. Add unit tests for every supported model layout.
5. Update README with the supported model list and per-model caveats.

Acceptance:

- Adding a model is mostly metadata and tests.
- Unit tests cover real captured payloads for each supported layout.

## Phase 2b - HMS / CMT2300A Support

Goal: poll HMS inverters over a CMT2300A radio with the same protocol core.

Status (2026-09-30): implemented in code, **not yet hardware-verified**.

- The hub component selects the implementation: `hoymiles_dtu_rf24`
  (`HoymilesDtuRf24`) or `hoymiles_dtu_cmt` (`HoymilesDtuCmt`).
  Everything radio-independent is `HoymilesDtuBase` in
  `hoymiles_dtu_base/dtu_base.{h,cpp}`, so request building, fragment
  collection, decoding, publishing and the power-limit queue have one
  implementation.
- `cmt2300a.{h,cpp}` is the chip driver: 3-wire SPI on a free ESP32 hardware
  SPI host (ESP-IDF half-duplex 3-wire mode) with a bit-bang fallback, the
  RFPDK register banks per band, the state machine, FIFO access, RSSI and the
  dBm TX-power table.
- `dtu_cmt.{h,cpp}` is the CMT state machine: single-frequency TX/RX with no
  link-layer ACK, software DTU-id filtering, time-based retransmits, and the
  channel-change (`0x56`) binding step for an inverter that is still on its boot
  frequency.
- HMS record layouts and the HMS power-limit control word are in
  `protocol.{h,cpp}` and unit-tested in `tests/native/test_protocol.cpp`.
- The nRF24 hub is its own component (`hoymiles_dtu_rf24`, which depends on
  `spi`), so a CMT-only configuration compiles none of it and needs no `spi:`
  bus. `tests/components/hoymiles_dtu/test.hms-cmt{,-idf}.yaml` compile that
  shape in CI.

Remaining tasks:

1. Work through `tests/manual/hms_cmt/README.md` against a real HMS inverter.
   The 3-wire SPI transport (step 1, both hardware and bit-banged) and the frequency binding (step 3) are the
   two parts with no upstream equivalent to fall back on.
2. Resolve the HMS-450/500 v2 layout disagreement between OpenDTU and Ahoy
   (reactive power and event-code offsets) from a real record.
3. Confirm the HMS persistent power-limit control word (`0x0003`) on hardware.
4. Decide whether passive scan detection on a single CMT frequency is worth
   keeping once its usefulness can be measured on air.
5. Consider per-inverter frequencies if a multi-HMS setup needs them (today the
   working frequency is hub-wide, as in OpenDTU).

Acceptance:

- An HMS inverter publishes live telemetry that matches its own app within ~2%.
- A power-cycled inverter is recovered by the channel-change step without a
  reboot of the ESP.
- No regression in the HM/nRF24 path.

## Phase 3 - Robust Radio Scheduling

Goal: make polling reliable enough for daily use.

Tasks:

1. Tune channel hop timing and response timeout from HIL logs.
2. Add retry behavior for incomplete frame sets.
3. Add round-robin multi-inverter scheduling.
4. Improve `dump_config()` diagnostics for radio, inverter, and last error.
5. Add manual HIL cases for two inverters when hardware is available.

Acceptance:

- Single inverter polling is stable over 24 hours.
- Multiple inverters do not starve each other.
- Incomplete responses recover without rebooting the ESP.

## Phase 4 - HM Command Entities

Goal: expose commands only after read-only telemetry is stable.

Tasks:

1. Implement active power limit as a `number` entity.
2. Implement enable/disable as a `switch`.
3. Implement restart as a `button`.
4. Add ESPHome action support for automations.
5. Require ack/verification before publishing command state.
6. Document command risks and non-optimistic behavior clearly.

Acceptance:

- Command state in Home Assistant reflects actual inverter acknowledgements.
- Failed commands time out, retry, and log a useful warning.

## Phase 5 - Release Hardening

Goal: make the HM-only component easy to install, test, and maintain.

Tasks:

1. Add complete README examples for basic HM setups.
2. Add troubleshooting docs for no radio, no response, incomplete response,
   wrong serial, and unstable values.
3. Keep CI compiling every public config shape.
4. Add a release checklist that includes HIL date, ESPHome version, board,
   framework, inverter model, and radio module used.
5. Retest against current ESPHome monthly or before any release.

Acceptance:

- New users can wire an HM+nRF24 setup and get telemetry from the docs.
- Release notes say exactly which HM models and ESPHome versions were tested.

## Testing Rules

- Unit tests cover serial validation, radio ID generation, CRCs, request
  framing, frame assembly, and payload decoding.
- ESPHome config tests cover schema and public YAML examples.
- ESPHome compile tests cover ESP32/ESP32-S3 and Arduino/ESP-IDF.
- HIL tests are required before tagging releases because radio timing cannot
  be proven by compile tests.

## Reference Policy

- Ahoy is the communication reference for HM/nRF24 behavior.
- Ahoy and OpenDTU are both references for HMS/CMT2300A behavior; where they
  disagree, record the disagreement and which one was followed.
- Store findings in `components/hoymiles_dtu_base/REFERENCE_NOTES.md`.
- Do not copy Ahoy or OpenDTU source into this repo unless licensing is
  revisited.
- When ESPHome APIs are uncertain, verify against current ESPHome docs/source
  before changing code.
