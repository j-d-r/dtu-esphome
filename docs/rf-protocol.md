# How the RF protocol works

This page is for anyone extending the component or debugging a link that
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

## The physical link

HM inverters use a Nordic nRF24L01+ compatible radio at 250 kbps, CRC16,
5-byte addresses, and dynamic (auto-sized) payloads — the same config Ahoy
uses. Instead of a single fixed channel, the link hops across five 2.4 GHz
channels — `{3, 23, 40, 61, 75}` — so both sides need to agree on *where* to
be listening at any given moment, not just how to encode a byte.

## Addressing

Both the DTU and every inverter have a 5-byte nRF radio address derived from
a serial number: byte 0 is always `0x01`, and bytes 1-4 are the serial's low
32 bits, byte-reversed (`hm_radio_id_from_serial` in `protocol.cpp`). The
important, easy-to-get-wrong part: the *printed* inverter serial is
**hexadecimal**, not a decimal integer — e.g. printed serial `116182806989`
produces radio address `01 82 80 69 89`, but naively parsing it as
decimal-116182806989 computes a completely different (wrong) address that the
inverter will never answer to. This bit the project early on; `validate_serial`
in `__init__.py` and `parse_serial` in `protocol.py` exist specifically to keep
it correct.

## The telemetry exchange

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

## Never blocks the ESPHome loop

All of the above — acquisition burst with channel rotation, the RX pendulum/
sweep search for the reply, fragment reassembly, whole-record re-request —
runs as an explicit state machine (`RequestState::{IDLE,WAIT_TX_IRQ,RX_ACTIVE}`)
driven a step at a time from `loop()`, with `update()` (the `PollingComponent`
tick) only responsible for starting the next telemetry exchange when idle and
due. Nothing in the radio path calls `delay()`.

## The power-limit path

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

## HMS over CMT2300A

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
