# Security: scan detection and scanning

Both features are configured on the hub; see the
[configuration reference](configuration.md) for the
options and entities.

## Detecting scans

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
    dtu_id: dtu_rf24
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
    dtu_id: dtu_rf24
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

## Scanning for inverters

The active counterpart to detection. Where `scan_detection` listens, the
**`hoymiles_dtu_rf24.scan_inverters`** action *transmits* the discovery requests
themselves and reports the serials that answer — the same thing AhoyDTU,
OpenDTU, and the CCC report's proof-of-concept scanner do. Two legitimate uses,
both on hardware you own or are authorised to assess:

- **Onboarding** — discover the serial and model of your own inverter over the
  air instead of reading the label by eye.
- **Exposure auditing** — enumerate the Hoymiles inverters within radio range of
  your site to see what an attacker with a cheap module would see.

Stated plainly: this **transmits the exact requests the CCC report describes as
the attack**, and exploits the same firmware flaw. It is therefore restricted to
**explicit one-shot triggers** — the `scan_inverters` action, a button, or a
single `scan_on_boot` scan — and **never runs on a recurring timer**; it only
*reads* serials: it sends no on/off, power-limit, or firmware command, so it
cannot change an inverter's operating state. Run it only against inverters you own
or are authorised to assess. Full design: [`scanner-spec.md`](scanner-spec.md).

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
    dtu_id: dtu_rf24
    scan_found_count: {name: "DTU Scan Found"}   # unique inverters found this scan

text_sensor:
  - platform: hoymiles_dtu_base
    dtu_id: dtu_rf24
    scan_result: {name: "DTU Scan Result"}       # each newly discovered inverter, one line
```

Each `scan_result` line reads like
`serial ...82806989 | PID 0x1144 | via search-id | ch 40 | #1`: the `serial` is
the discovered **last 8 printed digits** (hexadecimal — enough to address the
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
  id: dtu_rf24
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
