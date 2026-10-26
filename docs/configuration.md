# Configuration reference

## Hub: `hoymiles_dtu_rf24:` or `hoymiles_dtu_cmt:`

The hub owns the radio and polls each configured inverter in turn. It is a
standard ESPHome `PollingComponent`, so it accepts the usual `update_interval:`
alongside its own options. Choose the component that matches the radio you
wired up: `hoymiles_dtu_rf24` for HM inverters, `hoymiles_dtu_cmt` for HMS
inverters. The configured inverter models must belong to that radio's family,
otherwise the config is rejected. Options common to both:

- `update_interval` (*Optional*, default `15s`): how often each inverter is
  polled for telemetry.
- `scan_detection` (*Optional*, boolean, default `false`): enable passive,
  receive-only detection of nearby scanning/probing of Hoymiles inverters.
  See [Security: detecting scans](scanning.md#detecting-scans).
- **`inverters`** (**Required**, list, at least one): the inverters this hub
  talks to. See below.

### `hoymiles_dtu_rf24:` (HM)

- **`ce_pin`** (**Required**, pin schema): radio chip-enable GPIO.
- `irq_pin` (*Optional*, pin schema): radio IRQ GPIO. Must be an internal
  GPIO to get interrupt-driven wakeup; any other pin (or omitting it) falls
  back to polling.
- `pa_level` (*Optional*, one of `min` / `low` / `high` / `max`, default
  `low`): nRF24 PA/LNA transmit power. Standalone (no Ahoy) links have been
  validated on hardware at `max` for a marginal link; raise it if telemetry
  polls are failing to acquire.
- `scan_on_boot` (*Optional*, boolean or mapping, default `false`): run **one**
  active discovery scan a short delay after boot — a one-shot, never recurring.
  `true` uses defaults; a mapping accepts `delay` (default `45s`, wait for
  wifi/time to settle), `duration` (default `20s`), and `commands` (default both).
  Because a scan transmits, there is deliberately no interval/periodic form. See
  [Security: scanning for inverters](scanning.md#scanning-for-inverters).
- Also accepts the standard SPI device options (`spi_id`, `cs_pin`,
  `data_rate`, `spi_mode`, ...).

### `hoymiles_dtu_cmt:` (HMS)

- **`sclk_pin`**, **`sdio_pin`**, **`csb_pin`**, **`fcsb_pin`**
  (**Required**, pin schemas): the chip's 3-wire SPI lines and its two chip
  selects. All four must be real (internal) GPIOs. With hardware SPI any GPIO
  works, because the signals go through the GPIO matrix.
- `interface` (*Optional*, default `any`): how the 3-wire SPI is driven. Same
  values as the `spi:` component's `interface`:
  - `any`: use a hardware SPI host that no `spi:` bus has claimed. Bit-bang if
    there is none, if a pin is `inverted`, on non-ESP32 targets, or if the host
    fails to initialise at boot.
  - `hardware`: require a free hardware host. Config validation fails if
    there is none.
  - `spi2` / `spi3` (whichever names the chip has): require that host.
    Validation fails if a `spi:` bus already uses it.
  - `software`: always bit-bang (~330 kHz).

  `dump_config` logs which transport is in use.
- `gpio3_pin` (*Optional*, pin schema): the chip's GPIO3/INT2 "packet
  received" line. Only affects wake-up latency.
- `region` (*Optional*, one of `eu` / `us` / `brazil`, default `eu`): which
  sub-GHz band to configure. This selects the radio's base frequency and the
  legal band, and it also picks the frequency an unbound inverter is expected
  to listen on (868 MHz in the EU, 915 MHz elsewhere).
- `frequency` (*Optional*, frequency, default 865 MHz for `eu`, 918 MHz
  otherwise): the working frequency to poll on, and the frequency inverters are
  told to move to. Must be a multiple of 250 kHz (the chip's channel spacing)
  and inside the region's band. The defaults are what Hoymiles' own DTUs use.
- `tx_power` (*Optional*, -10 to 20, default `13`): transmit power in dBm. The
  default matches the radio's stock configuration; check what your region
  permits before raising it.

There is no `pa_level`, `ce_pin`, `irq_pin` or `spi_id` on this radio, and no
`scan_on_boot`/`scan_inverters`: the active discovery scanner is nRF24-only.

## `inverters:` (list, under the hub)

- **`id`** (**Required**, ID): used to attach `sensor:`/`text_sensor:`
  entities to this inverter.
- **`serial`** (**Required**, quoted string): the inverter's printed serial
  number, **12 hexadecimal digits** — usually all decimal-looking digits, but
  some models print letters (e.g. HMS prefix `114A`). Quote it
  (`serial: "116182806989"`): unquoted, YAML would read an all-digit serial as
  a decimal number, and the config is rejected. The digits are read as **hex**,
  exactly like Ahoy — they map straight onto the radio address bytes. Parsing
  the serial as a plain decimal number would compute the wrong radio address
  and the inverter would never respond. Just copy the digits off the
  inverter's label. (HMS serials work exactly the same way — the CMT2300A
  carries the address inside the packet instead of in a hardware address
  register, but the derivation is identical.)
- **`model`** (**Required**, one of the `model:` values in the
  [model tables](../README.md#supported-models)).
  It must belong to the hub's radio family (HM for `hoymiles_dtu_rf24`, HMS
  for `hoymiles_dtu_cmt`). The build warns if
  the model and the serial prefix disagree.

## `sensor:` platform

Keyed by `inverter_id`. All keys are optional — only declare the sensors you
want:

```yaml
sensor:
  - platform: hoymiles_dtu_base
    inverter_id: hm1200
    ac_power: {name: "HM1200 AC Power"}
    ac_voltage: {name: "HM1200 AC Voltage"}
    ac_current: {name: "HM1200 AC Current"}
    ac_frequency: {name: "HM1200 AC Frequency"}
    reactive_power: {name: "HM1200 Reactive Power"}
    power_factor: {name: "HM1200 Power Factor"}
    temperature: {name: "HM1200 Temperature"}
    yield_today: {name: "HM1200 Yield Today"}
    yield_total: {name: "HM1200 Yield Total"}
    signal_strength: {name: "HM1200 Signal"}   # HMS/CMT2300A only, see below
    channels:
      - number: 1
        dc_voltage: {name: "HM1200 CH1 DC Voltage"}
        dc_current: {name: "HM1200 CH1 DC Current"}
        dc_power: {name: "HM1200 CH1 DC Power"}
        yield_today: {name: "HM1200 CH1 Yield Today"}
```

`channels` is a list of per-DC-input entries; `number` (1-4, **Required**)
should cover the inverter model's DC inputs (1, 2 or 4, as OpenDTU defines them
per model). The build warns, without failing, when they don't match:

- a `number` beyond the model's inputs: its sensors only ever publish `NaN`;
- some channels declared but not all of the model's inputs: the missing ones
  are never published. Declaring no channels at all is taken as deliberate and
  does not warn.

Channels may be spread over several `sensor:` entries for the same
`inverter_id` (the `configs/` packages do that); the check looks at all of them
together and reports once per inverter.

`signal_strength` is the RSSI (dBm) of the last accepted reply. Only the
CMT2300A reports per-packet RSSI, so on an nRF24 (HM) hub the sensor is created
but never publishes. It is the quickest way to judge whether an HMS link has
headroom, or whether a different `frequency:` would do better.

## `text_sensor:` platform

Two independent groups, selected by which ID you key the block on:

```yaml
text_sensor:
  - platform: hoymiles_dtu_base
    inverter_id: hm1200
    status: {name: "HM1200 Status"}       # "offline" / "online" / "producing"
    last_seen: {name: "HM1200 Last Seen"} # seconds since the last good telemetry

  - platform: hoymiles_dtu_base
    dtu_id: dtu_rf24
    last_rx_payload: {name: "DTU Last RX Payload"}   # hex dump, diagnostic
    last_radio_error: {name: "DTU Last Radio Error"} # diagnostic
    scan_detected: {name: "DTU Scan Detected"}       # last detected scan/probe
    scan_result: {name: "DTU Scan Result"}           # last discovered inverter (active scan)
```

`status`/`last_seen` require `inverter_id`; `last_rx_payload`/
`last_radio_error`/`scan_detected`/`scan_result` are hub-level radio diagnostics
and require `dtu_id`. At least one of `inverter_id`/`dtu_id` is required per
block. `scan_detected` only publishes when `scan_detection` is enabled on the hub
(see [Security: detecting scans](scanning.md#detecting-scans)); `scan_result` is
populated by the `scan_inverters` action (see
[Security: scanning for inverters](scanning.md#scanning-for-inverters)).

## Actions

**`hoymiles_dtu_base.radio_set_power_limit`** — sets the inverter's active-power
limit via the DevControl protocol path. Non-blocking: it queues onto the same
request state machine telemetry polling uses, so calling it never stalls the
ESPHome loop, and it's automatically deferred if a telemetry exchange is
already in flight.

- `id` (**Required**): the hub to target (either radio).
- `percent` (*Optional*, 0-100, default `100`, templatable): relative power
  limit.
- `persistent` (*Optional*, boolean, default `true`, templatable): `true`
  survives an inverter restart/grid dropout; `false` reverts on the next
  power cycle.

```yaml
button:
  - platform: template
    name: "Limit to 80%"
    on_press:
      - hoymiles_dtu_base.radio_set_power_limit:
          id: dtu_rf24
          percent: 80
          persistent: false
```

Only the first configured inverter can currently be targeted by this action
(single-inverter limitation, matching how the DevControl exchange is wired
today).

**`hoymiles_dtu_rf24.scan_inverters`** — starts an active over-the-air discovery scan.
It **transmits** the discovery requests the CCC report describes (see
[Security: scanning for inverters](scanning.md#scanning-for-inverters)), so it is
only ever an on-demand action — there is no config option that would run it on a
timer. Non-blocking: it runs on the same radio state machine as telemetry, a
probe at a time, and returns the radio to normal polling when the scan window
ends.

- `id` (**Required**): the `hoymiles_dtu_rf24:` hub to target.
- `duration` (*Optional*, time period, default `20s`): how long to keep probing.
- `commands` (*Optional*, list of `search_id` / `collect_info`, default both):
  which discovery command(s) to send.

```yaml
button:
  - platform: template
    name: "Scan for inverters"
    on_press:
      - hoymiles_dtu_rf24.scan_inverters:
          id: dtu_rf24
          duration: 20s
          commands: [search_id, collect_info]
```
