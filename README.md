# ESPHome Hoymiles DTU

External ESPHome component that talks directly to Hoymiles microinverters over
their own radio link: HM-series over a 2.4 GHz nRF24L01+, HMS-series over a
sub-GHz CMT2300A. It reads live AC/DC telemetry and can send an
active-power-limit command.

## Status

Alpha-stage. Standalone telemetry (link acquisition, fragment reassembly,
per-model decode) and the active-power-limit command are verified on hardware
against a live **HM-1200**. On/off, restart, and multi-inverter scheduling
beyond "one at a time" are not implemented yet.

> [!IMPORTANT]
> **HMS support (CMT2300A) is not yet hardware-verified.** The protocol, the
> register configuration and the record layouts come from two independent
> reference implementations (OpenDTU and Ahoy) and are unit-tested off-device,
> but no HMS inverter has been polled with this code yet. In particular the
> 3-wire SPI transport and the frequency binding are untested on
> real hardware. Expect to work through `tests/manual/hms_cmt/` before
> trusting it. HM support is unaffected by this.

### Supported models

`model:` also selects which radio the inverter needs, so it decides which hub
component it goes under: HM models talk to an nRF24 (`hoymiles_dtu_rf24:`), HMS
models to a CMT2300A (`hoymiles_dtu_cmt:`).

**HM (nRF24, `hoymiles_dtu_rf24:`)** — the families Ahoy handles, grouped by DC-input
(channel) count, which is what the realtime payload layout depends on:

| Channels | Serial prefix | Models (`model:` value) |
| --- | --- | --- |
| 1 | 1121, 1122, 1022 | HM-300 (`hm_300`), HM-350 (`hm_350`), HM-400 (`hm_400`) |
| 2 | 1141, 1142, 1042 | HM-600 (`hm_600`), HM-700 (`hm_700`), HM-800 (`hm_800`) |
| 4 | 1161, 1162, 1062 | HM-1000 (`hm_1000`), HM-1200 (`hm_1200`), HM-1500 (`hm_1500`) |

The 10xx HM prefixes are MI 3rd-generation inverters, which both Ahoy and
OpenDTU treat as HM.

**HMS (CMT2300A, `hoymiles_dtu_cmt:`)** — the HMS record layouts differ from the
HM ones even at the same channel count, so each family is its own decoder. The
serial prefix column is the first four printed digits of the label; pick the
model that matches your serial, since a mismatched family decodes to
plausible-looking nonsense (the component warns at build time if the pair looks
wrong):

| Channels | Serial prefix | Models (`model:` value) |
| --- | --- | --- |
| 1 | 1124 | HMS-300 (`hms_300`), HMS-350 (`hms_350`), HMS-400 (`hms_400`), HMS-450 (`hms_450`), HMS-500 (`hms_500`) |
| 1 | 1125, 1400 | HMS-450 v2 (`hms_450_v2`), HMS-500 v2 (`hms_500_v2`) |
| 2 | 1143, 1144, 1146, 114A, 1410 | HMS-600 (`hms_600`), HMS-700 (`hms_700`), HMS-800 (`hms_800`), HMS-900 (`hms_900`), HMS-1000 (`hms_1000`) |
| 4 | 1164, 1166, 1420, 1421 | HMS-1600 (`hms_1600`), HMS-1800 (`hms_1800`), HMS-2000 (`hms_2000`) |

The `v2` models (serial prefix 1125/1400) report a single DC input in a
2-channel-shaped record. Their DC and AC fields follow OpenDTU's layout; Ahoy
decodes reactive power and the event code from different offsets for this
prefix, and that disagreement is unresolved — those two values may be wrong on
these models.

HMT (3-phase) inverters are out of scope: they report six DC channels and
three-phase AC, neither of which this component models.

## Hardware

### HM series: nRF24L01+

Any nRF24L01+ module wired to your ESP's SPI bus, plus two dedicated GPIOs:

- **SCK / MOSI / MISO** — shared SPI bus, declared once under the standard
  ESPHome `spi:` component.
- **CS** — chip-select, declared under `spi_id:`/`cs_pin:` like any other SPI
  device.
- **CE** (`ce_pin:`) — radio chip-enable, required.
- **IRQ** (`irq_pin:`) — optional. If wired to an internal GPIO, RX/TX
  completion is interrupt-driven; otherwise the component falls back to
  high-frequency polling during an exchange. Either way, the ESPHome loop is
  never blocked waiting on the radio.

> [!WARNING]
> **Add a capacitor across the nRF24L01+ module's VCC/GND, right at the
> module's pins.** The cheap breakout boards sold everywhere have little to no
> onboard decoupling, and the radio draws current spikes on TX that a long or
> thin 3.3V supply line can't keep up with. Without it you'll typically still
> see the module respond to SPI register reads/writes, but over-the-air comms
> will be flaky or dead — intermittent ACKs, garbled or missing packets, or no
> link at all. A 10–100 µF capacitor (electrolytic or tantalum work; add a
> 100 nF ceramic in parallel if you have one) directly across VCC/GND at the
> module fixes this in almost all cases. Do not rely on decoupling elsewhere
> on the board — it needs to be at the module itself.

### HMS series: CMT2300A

A CMT2300A module (as used on Hoymiles' own DTU-W/DTU-Pro-S and on the
Ahoy/OpenDTU HMS boards) wired to five GPIOs. The chip has its own 3-wire SPI:
**one** data line used in both directions, and two separate chip selects — one
for register access, one for the FIFO. ESPHome's `spi:` component cannot
express that, so the hub drives the chip itself and no `spi:` bus is
configured for it. On ESP32 it claims a hardware SPI host that no `spi:` bus
uses and runs it in half-duplex 3-wire mode, as Ahoy and OpenDTU do. If every
host is taken (ESP32-C3/C6/H2 have only one), it bit-bangs the same frames on
plain GPIOs instead. See `interface` below.

- **SCLK** (`sclk_pin:`) — serial clock, required.
- **SDIO** (`sdio_pin:`) — bidirectional data, required. Must be a real
  (internal) GPIO; the driver switches its direction per transfer.
- **CSB** (`csb_pin:`) — register chip-select, required.
- **FCSB** (`fcsb_pin:`) — FIFO chip-select, required.
- **GPIO3** (`gpio3_pin:`) — optional. The chip's INT2 line, configured as
  "packet received". Wiring it only shortens the wake-up latency; the driver
  polls the chip's interrupt flags either way.

Nothing else is needed (no CE-style enable line, no external reset). The same
supply-decoupling advice as for the nRF24 applies: put a capacitor across the
module's VCC/GND at the module.

## Installation

```yaml
external_components:
  - source: github://swifty99/dtu-esphome
    components: [hoymiles_dtu_base, hoymiles_dtu_rf24, hoymiles_dtu_cmt]
```

The code is split into three ESPHome components:

- `hoymiles_dtu_base`: the radio-independent part. It covers the protocol codec,
  polling, decoding, the `sensor`/`text_sensor` platforms and the
  `radio_set_power_limit` action. It has no YAML block of its own; both radio
  components load it automatically.
- `hoymiles_dtu_rf24`: the hub for HM inverters on an nRF24L01+.
- `hoymiles_dtu_cmt`: the hub for HMS inverters on a CMT2300A.

If you filter with `components:`, list `hoymiles_dtu_base` plus the radio
component(s) you use.

## Minimal configuration

### HM (nRF24)

```yaml
spi:
  id: dtu_spi
  clk_pin: GPIO5
  mosi_pin: GPIO11
  miso_pin: GPIO16

hoymiles_dtu_rf24:
  id: dtu
  spi_id: dtu_spi
  cs_pin: GPIO9
  ce_pin: GPIO12
  irq_pin: GPIO7
  inverters:
    - id: hm4
      serial: "114180000000"
      model: hm_1500

sensor:
  - platform: hoymiles_dtu_base
    inverter_id: hm4
    ac_power:
      name: "HM4 AC Power"
```

### HMS (CMT2300A)

```yaml
hoymiles_dtu_cmt:
  id: dtu
  sclk_pin: GPIO12
  sdio_pin: GPIO14
  csb_pin: GPIO27
  fcsb_pin: GPIO26
  gpio3_pin: GPIO34
  region: eu
  inverters:
    - id: hms2
      serial: "114400000000"
      model: hms_800

sensor:
  - platform: hoymiles_dtu_base
    inverter_id: hms2
    ac_power:
      name: "HMS AC Power"
```

## Configuration reference

### Hub: `hoymiles_dtu_rf24:` or `hoymiles_dtu_cmt:`

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
  See [Security: detecting scans](#security-detecting-scans).
- **`inverters`** (**Required**, list, at least one): the inverters this hub
  talks to. See below.

#### `hoymiles_dtu_rf24:` (HM)

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
  [Security: scanning for inverters](#security-scanning-for-inverters).
- Also accepts the standard SPI device options (`spi_id`, `cs_pin`,
  `data_rate`, `spi_mode`, ...).

#### `hoymiles_dtu_cmt:` (HMS)

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

### `inverters:` (list, under the hub)

- **`id`** (**Required**, ID): used to attach `sensor:`/`text_sensor:`
  entities to this inverter.
- **`serial`** (**Required**, string): the inverter's printed serial number,
  as **12 decimal digits**. It is interpreted as **packed BCD**, exactly like
  Ahoy — the printed digits map straight onto the radio address bytes. This
  is *not* the same as parsing the serial as a plain decimal number; doing
  that would compute the wrong radio address and the inverter would never
  respond. Just copy the digits off the inverter's label. (HMS serials work
  exactly the same way — the CMT2300A carries the address inside the packet
  instead of in a hardware address register, but the derivation is identical.)
- **`model`** (**Required**, one of the `model:` values in the tables above).
  It must belong to the hub's radio family (HM for `hoymiles_dtu_rf24`, HMS
  for `hoymiles_dtu_cmt`). The build warns if
  the model and the serial prefix disagree.

### `sensor:` platform

Keyed by `inverter_id`. All keys are optional — only declare the sensors you
want:

```yaml
sensor:
  - platform: hoymiles_dtu_base
    inverter_id: hm4
    ac_power: {name: "HM4 AC Power"}
    ac_voltage: {name: "HM4 AC Voltage"}
    ac_current: {name: "HM4 AC Current"}
    ac_frequency: {name: "HM4 AC Frequency"}
    reactive_power: {name: "HM4 Reactive Power"}
    power_factor: {name: "HM4 Power Factor"}
    temperature: {name: "HM4 Temperature"}
    yield_today: {name: "HM4 Yield Today"}
    yield_total: {name: "HM4 Yield Total"}
    signal_strength: {name: "HM4 Signal"}   # HMS/CMT2300A only, see below
    channels:
      - number: 1
        dc_voltage: {name: "HM4 CH1 DC Voltage"}
        dc_current: {name: "HM4 CH1 DC Current"}
        dc_power: {name: "HM4 CH1 DC Power"}
        yield_today: {name: "HM4 CH1 Yield Today"}
```

`channels` is a list of per-DC-input entries; `number` (1-4, **Required**)
should match the inverter's model channel count (1/2/4) — there is no
separate validation error for a mismatch, but any `number` beyond what the
model actually reports publishes `NaN` instead of a real reading.

`signal_strength` is the RSSI (dBm) of the last accepted reply. Only the
CMT2300A reports per-packet RSSI, so on an nRF24 (HM) hub the sensor is created
but never publishes. It is the quickest way to judge whether an HMS link has
headroom, or whether a different `frequency:` would do better.

### `text_sensor:` platform

Two independent groups, selected by which ID you key the block on:

```yaml
text_sensor:
  - platform: hoymiles_dtu_base
    inverter_id: hm4
    status: {name: "HM4 Status"}       # "offline" / "online" / "producing"
    last_seen: {name: "HM4 Last Seen"} # seconds since the last good telemetry

  - platform: hoymiles_dtu_base
    dtu_id: dtu
    last_rx_payload: {name: "DTU Last RX Payload"}   # hex dump, diagnostic
    last_radio_error: {name: "DTU Last Radio Error"} # diagnostic
    scan_detected: {name: "DTU Scan Detected"}       # last detected scan/probe
    scan_result: {name: "DTU Scan Result"}           # last discovered inverter (active scan)
```

`status`/`last_seen` require `inverter_id`; `last_rx_payload`/
`last_radio_error`/`scan_detected`/`scan_result` are hub-level radio diagnostics
and require `dtu_id`. At least one of `inverter_id`/`dtu_id` is required per
block. `scan_detected` only publishes when `scan_detection` is enabled on the hub
(see [Security: detecting scans](#security-detecting-scans)); `scan_result` is
populated by the `scan_inverters` action (see
[Security: scanning for inverters](#security-scanning-for-inverters)).

### Actions

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
          id: dtu
          percent: 80
          persistent: false
```

Only the first configured inverter can currently be targeted by this action
(single-inverter limitation, matching how the DevControl exchange is wired
today).

**`hoymiles_dtu_rf24.scan_inverters`** — starts an active over-the-air discovery scan.
It **transmits** the discovery requests the CCC report describes (see
[Security: scanning for inverters](#security-scanning-for-inverters)), so it is
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
          id: dtu
          duration: 20s
          commands: [search_id, collect_info]
```

## Security: detecting scans

The Hoymiles DTU protocol has no encryption, integrity, or authentication. As
documented in the CCC report [*"Wireless Interface Vulnerabilities of Hoymiles
Microinverters"*](https://www.ccc.de/system/uploads/382/original/hoymiles_dtu_vuln.pdf)
(plain-language [writeup](https://www.ccc.de/updates/2026/blinkenlights-hoymiles)),
an attacker with a cheap 2.4 GHz
module (or a sub-GHz one, for HMS inverters) can broadcast a **Search-ID**
request to enumerate the serial numbers of every inverter in radio range, then
send valid commands to any of them. **This
flaw lives in the inverter firmware — no DTU-side software (this component,
OpenDTU, or AhoyDTU) can close it; only a Hoymiles firmware fix can.**

What this component *can* do is **tell you when it is happening near you**.
Setting `scan_detection: true` makes the hub listen while it is otherwise idle
between telemetry polls. On an nRF24 hub it listens on the HM global Search-ID
address and on your inverter's own address, hopping the HM channels; on a
CMT2300A hub it listens on the configured working frequency (the chip has no
address filter at all, so every packet that decodes there is classified).
Anything it overhears that is a foreign DTU **request** is reported to the
`scan_detected` text sensor:

- **`Search-ID scan`** — a broadcast serial-enumeration scan (opcode `0x02`).
  Severity **MEDIUM**.
- **`info probe`** — an RF info request that also leaks the serial (`0x06`).
  Severity **MEDIUM**.
- **`foreign poll`** — a telemetry request aimed at *your* inverter by another
  DTU (`0x15`). Severity **HIGH**.
- **`FOREIGN DevControl`** — a control command (power limit / on-off) aimed at
  *your* inverter by another DTU (`0x51`). Severity **CRITICAL** — this is the
  serious one.

Each report reads like `CRITICAL | FOREIGN DevControl | DTU 0x80187264 | ch 40 | #7`,
where the leading word is the severity (below), the `DTU` value is the attacker's
own DTU serial (embedded in every request), and `#` is a running count.

```yaml
text_sensor:
  - platform: hoymiles_dtu_base
    dtu_id: dtu
    scan_detected: {name: "DTU Scan Detected", id: dtu_scan}
```

### Severity

Every detection carries a severity, so an automation can react to *how bad* the
traffic is instead of parsing the text. It rises with how far an attacker has
progressed against your specific inverter. The underlying unauthenticated-protocol
flaw these map onto is described in the CCC report [*"Wireless Interface
Vulnerabilities of Hoymiles Microinverters"*](https://www.ccc.de/system/uploads/382/original/hoymiles_dtu_vuln.pdf).

| Severity | Value | Kinds | Meaning |
|----------|-------|-------|---------|
| MEDIUM   | `2`   | `Search-ID scan`, `info probe` | reconnaissance — enumerating or probing serials nearby |
| HIGH     | `3`   | `foreign poll` | a foreign DTU is actively **reading** your inverter |
| CRITICAL | `4`   | `FOREIGN DevControl` | a foreign DTU is **commanding** your inverter (power limit / on-off) |

The severity prefixes the `scan_detected` text, and is also published as a number
by the optional hub-level **`scan_severity`** sensor — the value to threshold on in
automations. It updates on every classified packet (not throttled), so an
escalation is never hidden behind the text-report rate limit, and a `CRITICAL` also
logs at `ERROR`.

```yaml
sensor:
  - platform: hoymiles_dtu_base
    dtu_id: dtu
    scan_severity: {name: "DTU Scan Severity", id: dtu_scan_severity}

# automation (Home Assistant): alert when severity reaches HIGH (3) or above,
# e.g. trigger on numeric_state above 2 of sensor.dtu_scan_severity
```

This is **detection only — there are no countermeasures.** It never transmits a
request and cannot control the inverter. Honest limitations:

- **Probabilistic.** On the nRF24, monitor and attacker both hop channels, so it
  will not catch *every* packet — but scanners transmit repeatedly, so a real
  scanning run is very likely to be seen over a few seconds. Absence of a report
  is not proof of absence of scanning. A CMT2300A hub does not hop (HMS links use
  one frequency), so it only hears a scanner working on *its* frequency.
- **Blind during its own polls.** One radio cannot listen while it is actively
  polling telemetry. A longer `update_interval` leaves more time to monitor.
- **Range-bound.** It only hears transmitters within RF range.
- **Not perfectly stealthy on the nRF24.** HM dynamic-payload reception is
  coupled to the nRF24's hardware auto-ACK, so the monitor does ACK what it
  overhears. This leaks nothing beyond what the (already unauthenticated)
  protocol exposes, but it is not a silent sniffer. The CMT2300A has no auto-ACK,
  so on an HMS hub the monitor is genuinely silent.
- **Targeted-request detection covers the first configured inverter;**
  Search-ID broadcast detection is inverter-independent.
- **Validated on hardware (nRF24), but RF-dependent.** Confirmed on-air against a
  real AhoyDTU — both `foreign poll` and `FOREIGN DevControl` were detected. It is
  receive-only and cannot cause harm, but whether it catches a *given* scanner in
  *your* RF environment depends on range and timing, so confirm on your own bench.
  The CMT2300A path has not been verified on hardware yet.

## Security: scanning for inverters

The active counterpart to detection. Where `scan_detection` listens, the
**`hoymiles_dtu_rf24.scan_inverters`** action *transmits* the discovery requests
themselves and reports the serials that answer — the same thing AhoyDTU,
OpenDTU, and the CCC report's proof-of-concept scanner do. Two legitimate uses,
both on hardware you own or are authorised to assess:

- **Onboarding** — discover the serial and model of your own inverter over the
  air instead of reading the packed-BCD label by eye.
- **Exposure auditing** — enumerate the Hoymiles inverters within radio range of
  your site to see what an attacker with a cheap module would see.

Stated plainly: this **transmits the exact requests the CCC report describes as
the attack**, and exploits the same firmware flaw. It is therefore restricted to
**explicit one-shot triggers** — the `scan_inverters` action, a button, or a
single `scan_on_boot` scan — and **never runs on a recurring timer**; it only
*reads* serials: it sends no on/off, power-limit, or firmware command, so it
cannot change an inverter's operating state. Run it only against inverters you own
or are authorised to assess. Full design: [`docs/scanner-spec.md`](docs/scanner-spec.md).

A scan broadcasts two discovery commands across the HM channels for its
`duration`:

- **`search_id`** — the Search-ID (Gongfa, `0x02`) broadcast. An inverter that
  is not currently being polled by a DTU answers with its serial and model. It
  goes quiet a few minutes after a DTU last polled it.
- **`collect_info`** — the RF-info (`0x06`) probe. It leaks the serial **even
  while the inverter is bound to a DTU**, so it is the fallback that works in a
  live install (where your own DTU is polling continuously).

Results are published to two hub-level entities:

```yaml
sensor:
  - platform: hoymiles_dtu_base
    dtu_id: dtu
    scan_found_count: {name: "DTU Scan Found"}   # unique inverters found this scan

text_sensor:
  - platform: hoymiles_dtu_base
    dtu_id: dtu
    scan_result: {name: "DTU Scan Result"}       # each newly discovered inverter, one line
```

Each `scan_result` line reads like
`serial ...82806989 | PID 0x1144 | via search-id | ch 40 | #1`: the `serial` is
the discovered **last 8 printed digits** (packed BCD — enough to address the
inverter; the printed 4-digit model prefix is not transmitted, complete it from
the label), `PID` is the model/product ID (`search_id` only; `-` for
`collect_info`), and `#` is a running count. A scan that finds nothing publishes
`no inverters found`.

A scan is started by exactly one of three one-shot triggers — never a recurring
timer:

- the **`hoymiles_dtu_rf24.scan_inverters`** action (from any ESPHome automation);
- a **template button** that calls it (press it when you physically add hardware);
- the hub's **`scan_on_boot`** option, which runs a single scan a short delay
  after boot — handy for one-time onboarding of a new node:

```yaml
hoymiles_dtu_rf24:
  id: dtu
  # ...
  scan_on_boot: true            # one scan ~45s after boot, then never again
  # or, for control:
  # scan_on_boot:
  #   delay: 45s
  #   duration: 20s
  #   commands: [search_id, collect_info]
```

Note that an inverter already bound to your own polling DTU answers `search_id`
only after it has been left unpolled for a few minutes; `collect_info` still works
while bound. On boot the component has not yet polled, so a `scan_on_boot` with a
short delay catches that unbound window.

Limitations mirror detection: HM (2.4 GHz) only, probabilistic and range-bound,
and — because it transmits — it will trip other detectors in range, including
another node's own `scan_detection`.

## How the RF protocol works

This section is for anyone extending the component or debugging a link that
won't acquire. The protocol itself — framing, CRCs, fragment assembly, record
decoding — is in `components/hoymiles_dtu_base/protocol.{h,cpp}` (pure codec,
unit-tested, no ESPHome or radio-hardware dependency) and is shared by both
radios. Everything above the radio (poll scheduling, fragment collection,
publishing) lives next to it in `dtu_base.{h,cpp}`. The radio-specific halves
are `components/hoymiles_dtu_rf24/dtu_rf24.{h,cpp}` (nRF24 driver + async
state machine) and `components/hoymiles_dtu_cmt/` with `dtu_cmt.{h,cpp}` and
`cmt2300a.{h,cpp}` (CMT2300A driver + its state machine).

The sections below describe the HM/nRF24 link, which is the verified one;
[HMS over CMT2300A](#hms-over-cmt2300a) then lists what differs.

### The physical link

HM inverters use a Nordic nRF24L01+ compatible radio at 250 kbps, CRC16,
5-byte addresses, and dynamic (auto-sized) payloads — the same config Ahoy
uses. Instead of a single fixed channel, the link hops across five 2.4 GHz
channels — `{3, 23, 40, 61, 75}` — so both sides need to agree on *where* to
be listening at any given moment, not just how to encode a byte.

### Addressing

Both the DTU and every inverter have a 5-byte nRF radio address derived from
a serial number: byte 0 is always `0x01`, and bytes 1-4 are the serial's low
32 bits, byte-reversed (`hm_radio_id_from_serial` in `protocol.cpp`). The
important, easy-to-get-wrong part: the *printed* inverter serial is **packed
BCD**, not a decimal integer — e.g. printed serial `116182806989` produces
radio address `01 82 80 69 89`, but naively parsing it as decimal-116182806989
computes a completely different (wrong) address that the inverter will never
answer to. This bit the project early on; `validate_serial` in `__init__.py`
and `parse_serial_bcd` in `protocol.py` exist specifically to keep it correct.

### The telemetry exchange

1. The DTU picks a channel and transmits a realtime-data request: command
   byte `0x15`, with a request-type byte `0x0B` further into the payload,
   both the inverter's and the DTU's radio-address bytes embedded (so the
   inverter can verify it's being addressed and knows where to reply), a
   timestamp, and a CRC16 + CRC8 trailer (`hm_build_realtime_request`).
2. The inverter's radio auto-ACKs at the hardware level (`TX_DS`) — this
   confirms delivery but not that the inverter accepted the request.
3. The inverter then transmits its reply as a run of fragments, one per
   packet, each stamped `command = 0x95` (`0x15 | 0x80`, "all frames") and a
   1-based fragment ID in the low 7 bits of a header byte; the *last*
   fragment additionally sets that byte's top bit so the DTU knows the
   record is complete without needing a fixed fragment count (`hm_parse_frame`).
   These land on a channel offset **+3** from the request channel, so after
   sending, the DTU jumps there to listen.
4. The DTU reassembles fragments by ID (`hm_assemble_payload`) and, once it
   has every fragment up to the one with the "last" bit set, decodes the
   payload with a table keyed by the inverter's channel-count family
   (1/2/4-channel HM models each have a different byte layout for AC/DC
   fields — see `hm_model_layout` in `protocol.cpp`).

If the inverter never ACKs, the DTU retransmits on the next channel in the
hop sequence rather than waiting out a full poll interval — an acquisition
burst, up to 30 attempts, mirroring Ahoy's own retransmit cadence. If the ACK
lands but fragments are incomplete when the RX window closes, the DTU
re-requests the whole record (up to 4 times) rather than giving up — every
frame it already has is kept, only the missing ones need to reappear.

### Never blocks the ESPHome loop

All of the above — acquisition burst with channel rotation, the RX pendulum/
sweep search for the reply, fragment reassembly, whole-record re-request —
runs as an explicit state machine (`RequestState::{IDLE,WAIT_TX_IRQ,RX_ACTIVE}`)
driven a step at a time from `loop()`, with `update()` (the `PollingComponent`
tick) only responsible for starting the next telemetry exchange when idle and
due. Nothing in the radio path calls `delay()`.

### The power-limit path

`radio_set_power_limit` builds a DevControl request instead (command `0x51`,
a single-frame flag rather than "all frames", the `ActivePowerContr` sub-code,
the requested percentage ×10, and a persistent/non-persistent flag —
`hm_build_power_limit_request`). It reuses the exact same TX burst and
channel-rotation machinery as telemetry (`ExchangeKind::DEV_CONTROL` instead
of `ExchangeKind::TELEMETRY`), but the RX side is deliberately simpler: the
inverter replies with exactly one confirmation frame (`0xD1`), so the DTU
parks on the `+3`-offset channel and waits — no pendulum, no sweep, because
there's only one frame and one chance to be on the right channel. Acceptance
is decided the same way Ahoy's `parseDevCtrl` does (specific bytes in the
confirmation frame both zero).

Because that confirmation is a single unhopped frame, it's structurally less
redundant than the multi-fragment telemetry exchange. In hardware testing,
`TX_DS` (radio-level ACK — the inverter definitely received the command) has
been 100% reliable, while the RX confirmation occasionally doesn't arrive
within the listen window even though the command almost certainly landed;
you may see a log line like `... -> no confirmation frame` on an otherwise
healthy link. That's read as an inherent limitation of a single unhopped
confirmation frame, not a bug — nothing about the change itself is rolled
back by a missed confirmation.

### HMS over CMT2300A

HMS inverters speak the *same* application protocol — identical request
framing, identical `0x01`+serial radio id in bytes `[1..4]`, identical CRC8/CRC16,
identical fragment assembly — over a completely different radio. What changes:

- **Sub-GHz, one frequency.** The CMT2300A runs GFSK at 20 kbps in the 860 or
  900 MHz band. Channels are 250 kHz steps above the band's base frequency
  (`region:`), and a link uses a *single* frequency: there is no channel
  hopping, no `rxOffset`, no pendulum. EU DTUs work at 865 MHz, US/Brazil ones
  at 918 MHz.
- **No hardware addressing and no ACK.** The nRF24 does address matching and
  auto-ACK in hardware; the CMT2300A does neither. Every received packet is
  therefore matched in software against our own DTU serial (which the inverter
  echoes in bytes `[5..8]`), and a lost request shows up only as silence, so a
  failed poll is simply retransmitted up to three times.
- **Frequency binding.** An HMS inverter that has lost its DTU (power-up, or
  ~15 minutes without being polled) listens on a band-specific *boot* frequency
  — 868 MHz in the EU, 915 MHz elsewhere. Before polling an inverter that is
  currently offline, the hub therefore sends a channel-change command (opcode
  `0x56`) on that boot frequency telling the inverter to move to the working
  frequency, then immediately sends the telemetry request there. The command is
  never answered, which is why it costs one transmission and no waiting.
- **Different record layouts.** An HMS record interleaves its DC channels
  differently than an HM record with the same channel count, and the 4-channel
  HMS models report a separate voltage per input instead of one per MPPT pair.
  Each family has its own offsets in `hm_model_layout()`.
- **A different power-limit control word.** `RelativPersistent` is `0x0003` on
  HMS instead of the HM `0x0101` (the non-persistent `0x0001` is shared). This
  follows OpenDTU; Ahoy sends the HM words to HMS inverters too.
- **RSSI.** The chip reports per-packet RSSI, published via the optional
  `signal_strength` sensor.
- **3-wire SPI owned by the hub.** The chip's register/FIFO interface shares
  one data line (SDIO) and has two chip selects. ESPHome's `spi:` component
  cannot express that: its devices never set `SPI_DEVICE_3WIRE`. So the hub
  calls the ESP-IDF SPI master driver directly. It uses half-duplex 3-wire mode
  at 1 MHz, with one device per chip select, like both reference
  implementations. It takes the host that no `spi:` bus was assigned during
  config validation. When no host is free, it bit-bangs the same frames instead
  (~330 kHz). The radio does the modulation, so on-air timing is the same on
  either transport. On boot the driver runs the chip's own register read/write
  self-test, so a mis-wired module is reported as
  `register readback failed - check sclk/sdio/csb/fcsb wiring` rather than
  silently not working.

## Development

Run local checks with:

```sh
pre-commit run --all-files
```

Run the unit tests (native C++ decoder tests, protocol tests, and — with
`esphome` installed — config validation) with:

```sh
pytest tests/unit
```

Compile the minimal fixture (nRF24/HM) and the HMS one (CMT2300A, no `spi:`
bus) with:

```sh
esphome compile tests/components/hoymiles_dtu/test.minimal.yaml
esphome compile tests/components/hoymiles_dtu/test.hms-cmt.yaml
```

Format the C++ sources, and lint them with clang-tidy using ESPHome's
`.clang-tidy`:

```sh
clang-format -i components/*/*.{h,cpp}
scripts/clang-tidy.py          # add --fix to apply fix-its, --no-compile to reuse builds
```

`scripts/clang-tidy.py` compiles the ESP-IDF fixtures (`test.esp32-idf.yaml`
and `test.hms-cmt-idf.yaml`) to get real compiler flags, then runs clang-tidy
on the repo sources. It needs `esphome` and clang-tidy 22 (the version ESPHome
pins) on the `PATH`. It uses `clang-tidy-22` if present, otherwise
`clang-tidy` with a warning when that is another version. `--clang-tidy PATH`
overrides it.

Hardware-in-the-loop checklists live in `tests/manual/`:
`hm4_readonly/` for HM telemetry and `hms_cmt/` for the unverified HMS path.

## License

GPL-3.0-only. See [LICENSE](LICENSE).
