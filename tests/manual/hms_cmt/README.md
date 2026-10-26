# HMS / CMT2300A HIL Checklist

**Nothing in the CMT2300A path has been confirmed against real hardware yet.**
The protocol, the register configuration and the record layouts come from two
independent reference implementations (OpenDTU and Ahoy) and are unit-tested
off-device, but the 3-wire SPI transport and the frequency binding
have never talked to an inverter. Work this list top to bottom: each step
isolates one layer, so a failure tells you which layer to look at.

Target hardware:

- Any ESP32 (the radio needs five GPIOs and no `spi:` bus; the hub claims a free
  hardware SPI host itself)
- A CMT2300A module: the one on a Hoymiles DTU-W/DTU-Pro-S, or the
  Ahoy/OpenDTU HMS boards, or a bare CMT2300A breakout
- An HMS inverter (`hms_*` model), producing, within a few metres for the
  first attempt

Before flashing:

- Wire and double-check **SCLK, SDIO, CSB, FCSB** and optionally **GPIO3**, then
  set them in the `substitutions:` block of `hms-cmt-readonly.yaml`. SDIO is
  bidirectional — one wire, not two.
- Put a capacitor across the module's VCC/GND at the module, same as for an
  nRF24.
- Set `serial:` and `model:` to your inverter. The first four printed digits
  must match the model family (see the table in the top-level README); the build
  warns if they do not.
- Set `region:` for where you are. It selects the band, the working frequency
  and the frequency an unbound inverter listens on.

Run:

```sh
esphome config tests/manual/hms_cmt/hms-cmt-readonly.yaml
esphome run tests/manual/hms_cmt/hms-cmt-readonly.yaml
esphome logs tests/manual/hms_cmt/hms-cmt-readonly.yaml
```

## Step 1 — the SPI transport

Acceptance: the boot log reports
`CMT2300A radio ready on 865.00 MHz, DTU serial 0x...`, and `dump_config`
shows `SPI transport: hardware (3-wire half-duplex)`. Then flash once more
with `interface: software` on the hub and check the same line comes back
with `SPI transport: bit-banged`, so that both transports are proven.

If instead you see `register readback failed - check sclk/sdio/csb/fcsb wiring`,
the chip is not answering register reads/writes and nothing above this matters.
That message comes from the chip's own self-test (write `0xAA` to an unused
register, read it back), so it is a wiring/transport problem, not a protocol one:

- swap SDIO and SCLK if they might be crossed,
- confirm CSB and FCSB are not swapped (CSB is register access, FCSB is FIFO),
- confirm 3.3 V and a common ground,
- if the module is on a long cable, try shortening it before anything else,
- if only one transport fails (hardware vs. `interface: software`), the wiring
  is fine. Report the failing one together with the boot log.

`chip did not enter standby` means register access works but the chip does not
change state — usually supply or crystal, not wiring.

## Step 2 — transmission

Acceptance: `dump_config` shows the frequencies, and each poll logs a
`TX request ... 865.00 MHz ... data=15 ...` line followed within ~150 ms by no
`did not report TX done` warning.

A `CMT2300A did not report TX done within 150ms` warning means the chip accepted
the payload but never raised TX_DONE: check `tx_power` is in range and the
antenna is connected.

## Step 3 — the inverter answers

Acceptance: `RX 865.00 MHz -nn dBm len=.. data=95 ...` lines appear, then
`Accepted HM frame id=1 ...` and finally a `Telemetry ...` line.

If TX looks healthy but nothing is received:

- The inverter may still be on its boot frequency. The hub sends the
  channel-change command (`TX channel-change ... on 868.00 MHz`) before every
  poll of an inverter it considers offline — confirm that line appears. If it
  does and the inverter still never answers, try setting `frequency:` to the
  boot frequency (868 MHz EU / 915 MHz elsewhere) so the DTU works where the
  inverter already listens; that bypasses the binding step entirely and isolates
  it as the failing part.
- If `RX` lines appear but no frame is accepted, the payload is being received
  and rejected: capture the hex from the log. `Ignored invalid HM frame` means
  the CRC8 or the inverter id did not match; no message at all means the packet
  was dropped as not addressed to this DTU (bytes `[5..8]`).
- Check the RSSI in the `RX` line. Below about -95 dBm, move closer before
  concluding anything.

## Step 4 — the decode

Acceptance: the `Telemetry` line and the published sensors match what the
inverter's own app (or an OpenDTU/Ahoy next to it) reports, within ~2%.

If AC power looks right but the DC channels look swapped, wrong by a factor of
ten, or one channel reads zero, the record layout for that model family is
wrong — note the raw record (`last_rx_payload` plus the accepted fragments at
DEBUG level) and the exact model. HMS-450/500 **v2** (serial prefix 1125/1400)
is the known-uncertain case: its reactive power and event code come from
offsets where OpenDTU and Ahoy disagree.

## Step 5 — steady state

- `status` becomes `producing`, and `signal_strength` publishes a plausible
  RSSI.
- Covering the panels makes `status` fall back to `offline` after the timeout,
  and uncovering them recovers without a reboot (this exercises the
  channel-change/binding path).
- A 100-cycle reboot loop never hangs in `setup()` (the state-machine waits are
  all bounded, so a hang would be a bug worth reporting).
- A 12-hour soak keeps free heap stable and keeps polling.

## Step 6 — the power limit (optional, writes to the inverter)

Only once steps 1-5 pass. Add a button to the fixture:

```yaml
button:
  - platform: template
    name: "HMS Limit 70%"
    on_press:
      - hoymiles_dtu_base.radio_set_power_limit:
          id: dtu
          percent: 70
          persistent: false
```

Use `persistent: false` first: it reverts on the next power cycle, so a wrong
control word cannot leave the inverter stuck. Acceptance is
`Power limit 70% (non-persistent): TX_DONE -> ACCEPTED by inverter` plus a
visible drop in AC power. HMS encodes the *persistent* variant differently from
HM (`0x0003` vs `0x0101`, per OpenDTU), so verify `persistent: true`
separately before relying on it.

Note that `-> no confirmation frame` with the limit visibly applied is a known
HM-side quirk (a single unhopped confirmation frame, see
[docs/rf-protocol.md](../../../docs/rf-protocol.md#the-power-limit-path))
and the confirmation check itself is the HM one — Ahoy uses the same check for
HMS. If it turns out HMS confirmations have a different shape, the raw `RX`
hex for the `D1` reply is what to capture.
