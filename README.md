# ESPHome Hoymiles DTU

> [!NOTE]
> This is a fork of [swifty99/dtu-esphome](https://github.com/swifty99/dtu-esphome),
> and most of the work here is swifty99's: the protocol codec, the HM/nRF24
> support and the scan features. Many thanks for it. This fork builds on that
> foundation to add HMS inverters on a CMT2300A radio, which are out of scope
> for the upstream project.

External ESPHome component that talks directly to Hoymiles microinverters over
their own radio link: HM-series over a 2.4 GHz nRF24L01+, HMS-series over a
sub-GHz CMT2300A. It reads live AC/DC telemetry and can send an
active-power-limit command.

## Status

Alpha-stage. Tested on hardware so far:

- **HM (nRF24):** standalone telemetry (link acquisition, fragment reassembly,
  per-model decode) and the active-power-limit command, against a live
  **HM-1200** (tested by swifty99).
- **HMS (CMT2300A):** link acquisition, with several inverters on one hub.

On/off, restart, and multi-inverter scheduling beyond "one at a time" are not
implemented yet.

### Supported models

`model:` also selects which radio the inverter needs, so it decides which hub
component it goes under: HM and HERF models talk to an nRF24
(`hoymiles_dtu_rf24:`), HMS models to a CMT2300A (`hoymiles_dtu_cmt:`).

**HM and HERF (nRF24, `hoymiles_dtu_rf24:`)** — the HM families Ahoy handles,
grouped by DC-input (channel) count, which is what the realtime payload layout
depends on, plus the HERF models OpenDTU supports:

| Channels | Serial prefix | Models (`model:` value) |
| --- | --- | --- |
| 1 | 1121, 1122, 1022 | HM-300 (`hm_300`), HM-350 (`hm_350`), HM-400 (`hm_400`) |
| 2 | 1141, 1142, 1042 | HM-600 (`hm_600`), HM-700 (`hm_700`), HM-800 (`hm_800`) |
| 4 | 1161, 1162, 1062 | HM-1000 (`hm_1000`), HM-1200 (`hm_1200`), HM-1500 (`hm_1500`) |
| 1 | 2841 | HERF-300 (`herf_300`) |
| 2 | 2821 | HERF-600 (`herf_600`), HERF-800 (`herf_800`) |
| 4 | 2801 | HERF-1600 (`herf_1600`), HERF-1800 (`herf_1800`) |

The 10xx HM prefixes are MI 3rd-generation inverters, which both Ahoy and
OpenDTU treat as HM. HERF support follows OpenDTU only (Ahoy has none) and is
not hardware-verified: HERF-600/800 records are laid out like the HMS 2-channel
ones and HERF-1600/1800 like the HM 4-channel ones, while HERF-300 has its own
layout. OpenDTU marks HERF-300's reactive power, temperature and event code "to
be verified" and reads reactive power and the event code from the same field,
so treat those three values as unconfirmed on that model.

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

A CMT2300A module (as used on Hoymiles' own DTU-W/DTU-Pro-S) wired to five
GPIOs. The chip has its own 3-wire SPI:
**one** data line used in both directions, and two separate chip selects — one
for register access, one for the FIFO. ESPHome's `spi:` component cannot
express that, so the hub drives the chip itself and no `spi:` bus is
configured for it. On ESP32 it claims a hardware SPI host that no `spi:` bus
uses and runs it in half-duplex 3-wire mode. If every host is taken
(ESP32-C3/C6/H2 have only one), it bit-bangs the same frames on plain GPIOs
instead. See `interface` in the
[configuration reference](docs/configuration.md#hoymiles_dtu_cmt-hms).

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

GPIO2 is unused: leave it unconnected. The chip drives it as INT1 ("TX done"),
but the driver reads that from the interrupt flags instead.

## Installation

```yaml
external_components:
  - source: github://j-d-r/dtu-esphome
    components: [hoymiles_dtu_base, hoymiles_dtu_cmt, hoymiles_dtu_rf24]
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

The snippets below show only the hub and the packages. For complete device
configurations (Wi-Fi, API, OTA, logger, time and external components included),
see [docs/example-hm.yaml](docs/example-hm.yaml) for HM inverters on an
nRF24, [docs/example-hms.yaml](docs/example-hms.yaml) for HMS inverters on a
CMT2300A, and [docs/example-hm-hms.yaml](docs/example-hm-hms.yaml) for both
radios on one ESP32. To try one, copy
[docs/dummy-secrets.yaml](docs/dummy-secrets.yaml) to `secrets.yaml` next to
it and fill in your Wi-Fi credentials, an API encryption key
(`openssl rand -base64 32`) and your inverters' printed serials, quoted.

`configs/dtu.yaml` adds the hub-level diagnostic entities; include it once per
hub. `configs/inverter.yaml` adds one inverter to the hub and creates its
entities. Its `channels:` must be the model's DC-input count (1, 2 or 4, the
"Channels" column of the [model tables](#supported-models)), because it selects
how many DC channel entities are created.

### HM (nRF24)

```yaml
spi:
  id: dtu_spi
  clk_pin: GPIO18
  mosi_pin: GPIO11
  miso_pin: GPIO16

hoymiles_dtu_rf24:
  id: dtu_rf24
  spi_id: dtu_spi
  cs_pin: GPIO9
  ce_pin: GPIO12
  irq_pin: GPIO17
  update_interval: 15s
  pa_level: max
  scan_detection: true

packages:
  dtu_rf24:
    url: https://github.com/j-d-r/dtu-esphome
    files:
      - path: configs/dtu.yaml
        vars:
          type: rf24
          dtu_id: dtu_rf24
          name: DTU24

      - path: configs/inverter.yaml
        vars:
          type: rf24
          dtu_id: dtu_rf24
          id: hm1000
          name: HM1000
          serial: !secret hm1000_sn1
          model: hm_1000
          channels: 4

      - path: configs/inverter.yaml
        vars:
          type: rf24
          dtu_id: dtu_rf24
          id: hm1200
          name: HM1200
          serial: !secret hm1200_sn1
          model: hm_1200
          channels: 4
```

### HMS (CMT2300A)

```yaml
hoymiles_dtu_cmt:
  id: dtu_cmt
  sclk_pin: GPIO5
  sdio_pin: GPIO8
  csb_pin: GPIO6
  fcsb_pin: GPIO7
  gpio3_pin: GPIO1
  region: eu
  frequency: 865MHz
  tx_power: 13
  update_interval: 15s

packages:
  dtu_cmt:
    url: https://github.com/j-d-r/dtu-esphome
    files:
      - path: configs/dtu.yaml
        vars:
          type: cmt
          dtu_id: dtu_cmt
          name: DTU_CMT

      - path: configs/inverter.yaml
        vars:
          type: cmt
          dtu_id: dtu_cmt
          id: hms1000
          name: HMS1000
          serial: !secret hms1000_sn1
          model: hms_1000
          channels: 2

      - path: configs/inverter.yaml
        vars:
          type: cmt
          dtu_id: dtu_cmt
          id: hms2000
          name: HMS2000
          serial: !secret hms2000_sn1
          model: hms_2000
          channels: 4
```

## Configuration reference

Every hub, inverter, sensor and action option is described in
[docs/configuration.md](docs/configuration.md).

## Security

The Hoymiles radio protocol is unauthenticated: anyone in range can enumerate
inverter serials and send them commands. The hub can passively report such
traffic (`scan_detection`), and the nRF24 hub can run an on-demand discovery
scan itself (`scan_inverters`). Both are described, with their limits, in
[docs/scanning.md](docs/scanning.md).

## How the RF protocol works

For anyone extending the component or debugging a link that won't acquire,
[docs/rf-protocol.md](docs/rf-protocol.md) walks through the HM/nRF24 link
(addressing, the telemetry exchange, the power-limit path) and what differs on
HMS over CMT2300A. [docs/hoymiles-protocol.md](docs/hoymiles-protocol.md) is
the byte-level reference: packet layouts, checksums, discovery, the realtime
record fields and the control commands, with annotated hex examples.

## Development

Local checks, unit tests, compiling the test fixtures and linting are described
in [docs/development.md](docs/development.md).

## License

GPL-3.0-only. See [LICENSE](LICENSE).
