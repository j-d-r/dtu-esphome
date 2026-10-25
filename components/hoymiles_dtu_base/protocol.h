#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace esphome::hoymiles_dtu {

static constexpr uint8_t HM_MAX_CHANNELS = 4;
static constexpr uint8_t HM_MAX_FRAME_COUNT = 8;
static constexpr uint8_t HM_MAX_PAYLOAD_SIZE = 96;
static constexpr uint8_t HM_RF_CHANNELS[5] = {3, 23, 40, 61, 75};

static constexpr uint8_t HM_TX_REQ_INFO = 0x15;
static constexpr uint8_t HM_TX_REQ_DEVCONTROL = 0x51;
// Discovery/probe opcodes an attacker uses to harvest inverter serials over the air (see the CCC
// "Wireless Interface Vulnerabilities of Hoymiles Microinverters" report, docs/).
static constexpr uint8_t HM_TX_SEARCH_ID = 0x02;     // "Search ID" (Gongfa) broadcast discovery
static constexpr uint8_t HM_TX_COLLECT_INFO = 0x06;  // Collect RF hw/sw info (leaks serial too)
// The HM "global" Search-ID listening address (report section 4.2). A DTU broadcasts Search-ID
// requests to this address to enumerate inverter serials; listening on it detects such scans.
static constexpr uint8_t HM_SEARCH_ID_ADDRESS[5] = {0x05, 0x64, 0x64, 0x64, 0x64};
static constexpr uint8_t HM_ALL_FRAMES = 0x80;
static constexpr uint8_t HM_SINGLE_FRAME = 0x81;
static constexpr uint8_t HM_REAL_TIME_RUN_DATA_DEBUG = 0x0B;
static constexpr uint8_t HM_DEV_CTRL_ACTIVE_POWER = 0x0B;  // ActivePowerContr (=11)
// HMS/HMT only: tell the inverter which CMT2300A frequency channel to use from now on. Sent on the
// inverter's boot frequency; it never answers (see hm_build_channel_change_request).
static constexpr uint8_t HM_TX_CHANNEL_CHANGE = 0x56;

// Sub-GHz band/region of a CMT2300A radio. The base frequency is the register-bank frequency the
// channel index counts up from in HM_CMT_CHANNEL_STEP_HZ steps; the boot frequency is where an HMS
// inverter listens after power-up (or after ~15 min without a DTU) until it is told to switch.
enum HmCmtRegion : uint8_t {
  HM_REGION_EU,      // base 860 MHz, legal 863-870, work default 865, inverter boot 868
  HM_REGION_US,      // base 900 MHz, legal 905-925, work default 918, inverter boot 915
  HM_REGION_BRAZIL,  // base 900 MHz, legal 915-928, work default 918, inverter boot 915
};

static constexpr uint32_t HM_CMT_CHANNEL_STEP_HZ = 250000;

// Supported microinverters, grouped by the realtime-record layout family they use. The HM families
// mirror Ahoy's nRF24 grouping by DC-input count (1-channel HM-300/350/400, 2-channel
// HM-600/700/800, 4-channel HM-1000/1200/1500). The HMS families are the CMT2300A (sub-GHz) models;
// their records use different field offsets than the same-channel-count HM records, so each HMS
// family is its own layout (see hm_model_layout). HMT (3-phase) is not supported.
//
// HMS model <-> printed serial prefix (OpenDTU HMS_*::isValidSerial, cross-checked against Ahoy
// hmSystem.h): 1124 -> HMS_300..HMS_500, 1125/1400 -> HMS_450_V2/HMS_500_V2,
// 1143/1144/114A/1410 -> HMS_600..HMS_1000, 1164/1166/1420 -> HMS_1600..HMS_2000.
enum HmModel : uint8_t {
  HM_300,
  HM_350,
  HM_400,
  HM_600,
  HM_700,
  HM_800,
  HM_1000,
  HM_1200,
  HM_1500,
  HMS_300,
  HMS_350,
  HMS_400,
  HMS_450,
  HMS_500,
  HMS_450_V2,
  HMS_500_V2,
  HMS_600,
  HMS_700,
  HMS_800,
  HMS_900,
  HMS_1000,
  HMS_1600,
  HMS_1800,
  HMS_2000,
};

// True for the HMS families, i.e. the models that talk over a CMT2300A sub-GHz radio instead of an
// nRF24. Also selects the HMS variant of the DevControl power-limit control word.
bool hm_model_is_hms(HmModel model);

enum HmStatus : uint8_t {
  OFFLINE,
  ONLINE,
  PRODUCING,
};

struct HmChannelData {
  float dc_voltage{0.0f};
  float dc_current{0.0f};
  float dc_power{0.0f};
  float yield_today{0.0f};
  float yield_total{0.0f};
};

struct HmTelemetry {
  bool valid{false};
  float ac_voltage{0.0f};
  float ac_current{0.0f};
  float ac_power{0.0f};
  float ac_frequency{0.0f};
  float reactive_power{0.0f};
  float power_factor{0.0f};
  float temperature{0.0f};
  float yield_today{0.0f};
  float yield_total{0.0f};
  uint16_t event_code{0};
  std::array<HmChannelData, HM_MAX_CHANNELS> channels{};
};

struct HmFrame {
  uint8_t id{0};
  uint8_t len{0};
  std::array<uint8_t, 32> data{};
};

uint8_t hm_crc8(const uint8_t *data, size_t len);
uint16_t hm_crc16(const uint8_t *data, size_t len);
uint64_t hm_radio_id_from_serial(uint64_t serial);
void hm_radio_id_to_address(uint64_t radio_id, uint8_t *address);
uint32_t hm_generate_dtu_serial();
uint8_t hm_build_realtime_request(uint64_t inverter_radio_id, uint32_t dtu_serial, uint32_t timestamp, uint8_t *buffer,
                                  size_t buffer_len);
// Active-power-limit DevControl request. percent is 0..100; persistent=true survives an inverter
// restart (RelativPersistent), false is RelativNonPersistent. hms selects the control-word encoding:
// HM uses 0x0101/0x0001, HMS uses 0x0003/0x0001 (OpenDTU ActivePowerControlCommand
// _powerLimitControlTypeValues; Ahoy sends the HM words to HMS inverters too, which OpenDTU treats as
// a bug). Returns packet length or 0 on error.
uint8_t hm_build_power_limit_request(uint64_t inverter_radio_id, uint32_t dtu_serial, uint16_t percent, bool persistent,
                                     bool hms, uint8_t *buffer, size_t buffer_len);

// HMS/HMT channel-change request (opcode 0x56): tells the inverter to move to CMT2300A frequency
// channel `channel`. Transmitted on the inverter's boot frequency, carries only the app-layer CRC8
// (no CRC16), and is never answered. Layout per OpenDTU ChannelChangeCommand:
// [9..11] are region-specific magic bytes, [12] the channel, [13]=0x14. Returns the length (15) or 0.
uint8_t hm_build_channel_change_request(uint64_t inverter_radio_id, uint32_t dtu_serial, HmCmtRegion region,
                                        uint8_t channel, uint8_t *buffer, size_t buffer_len);

// True when a received packet carries our DTU serial in its [5..8] router-address field. The
// CMT2300A has no hardware address filter (unlike the nRF24's pipes), so a CMT receiver must drop
// foreign traffic in software before treating it as a reply to our own request.
bool hm_packet_addressed_to_dtu(const uint8_t *packet, uint8_t len, uint32_t dtu_serial);
bool hm_parse_frame(const uint8_t *packet, uint8_t len, uint64_t inverter_radio_id, HmFrame *frame);
bool hm_assemble_payload(const HmFrame *frames, uint8_t frame_count, uint8_t *payload, size_t payload_len,
                         uint8_t *assembled_len);
// Number of DC channels this model reports (1, 2, or 4).
uint8_t hm_model_channel_count(HmModel model);
// Decode an assembled realtime record into telemetry. Field byte offsets follow the model's channel
// count (Ahoy hm{1,2,4}chAssignment). Returns false on a null/too-short payload or unknown model.
bool hm_parse_realtime_payload(HmModel model, const uint8_t *payload, uint8_t len, HmTelemetry *telemetry);
const char *hm_model_to_string(HmModel model);
const char *hm_status_to_string(HmStatus status);

// --- Passive scan/intrusion detection (receive-only) ---------------------------------------------
// Classification of a passively-overheard DTU->inverter request. Every such request must reach the
// inverter over its own (publicly derivable) address, so a monitor tuned to that address and to the
// global Search-ID address can overhear an attacker's traffic without transmitting anything.
enum HmSniffKind : uint8_t {
  HM_SNIFF_NONE = 0,
  HM_SNIFF_SEARCH_ID,        // 0x02 broadcast discovery — the mass-scan signature
  HM_SNIFF_COLLECT_INFO,     // 0x06 RF info probe — leaks the serial even while a DTU is bound
  HM_SNIFF_FOREIGN_POLL,     // 0x15 telemetry request aimed at our inverter by a foreign DTU
  HM_SNIFF_FOREIGN_CONTROL,  // 0x51 DevControl aimed at our inverter by a foreign DTU (most serious)
};

// Severity of a detected foreign request on a 0-4 scale. Published verbatim by the scan_severity
// sensor so a Home Assistant automation can threshold on it (e.g. >= HM_SEVERITY_HIGH).
enum HmSeverity : uint8_t {
  HM_SEVERITY_NONE = 0,
  HM_SEVERITY_LOW = 1,
  HM_SEVERITY_MEDIUM = 2,    // reconnaissance: Search-ID scan or info probe
  HM_SEVERITY_HIGH = 3,      // a foreign DTU is actively polling our inverter
  HM_SEVERITY_CRITICAL = 4,  // a foreign DTU is sending control commands to our inverter
};

struct HmSniffResult {
  HmSniffKind kind{HM_SNIFF_NONE};
  HmSeverity severity{HM_SEVERITY_NONE};
  uint8_t opcode{0};
  uint32_t sender_dtu_serial{0};  // the attacker's DTU serial embedded in the request (0 if unknown)
  bool targets_our_inverter{false};
};

// Classify a passively-received packet as a foreign DTU request (scan/probe/control), or reject it.
// Validates the trailing app-layer CRC8 so RF noise and inverter responses (opcode bit7 set) are
// ignored, and drops requests that carry our own DTU serial. Returns true and fills *out only for a
// recognised foreign request.
bool hm_classify_sniffed_packet(const uint8_t *packet, uint8_t len, uint64_t our_inverter_radio_id,
                                uint32_t our_dtu_serial, HmSniffResult *out);

// Map a sniff kind to its severity, and a severity to a short upper-case label
// ("MEDIUM"/"HIGH"/"CRITICAL"). Shared by the classifier and the report/log formatting.
HmSeverity hm_sniff_severity(HmSniffKind kind);
const char *hm_severity_to_string(HmSeverity severity);

// --- Active discovery / scanner (transmit) -------------------------------------------------------
// The transmit-side counterpart to hm_classify_sniffed_packet: build the discovery requests the CCC
// report (section 4) documents for harvesting inverter serials, and parse the replies. Intended for
// discovering the serial/model of an inverter you own (so you need not read the packed-BCD label by
// eye) and for auditing your own RF exposure. It exploits the same unauthenticated-protocol flaw the
// report describes; only run it against inverters you own or are authorised to assess.
struct HmDiscoveredInverter {
  // Last 8 printed serial digits, packed BCD. This equals serial & 0xFFFFFFFF and the low 4 radio
  // address bytes; the printed 4-digit model prefix is not carried over the air.
  uint32_t serial_suffix{0};
  uint16_t pid{0};                   // model/product ID (Search-ID reply only; 0 for a collect-info reply)
  uint32_t responder_dtu_serial{0};  // the DTU address the inverter echoed back (our scan sender id)
};

// Build the "Search ID" (Gongfa, opcode 0x02) broadcast discovery request. Transmitted to the HM
// global Search-ID address (HM_SEARCH_ID_ADDRESS); an in-range inverter not currently bound to a DTU
// replies with its serial (see hm_parse_search_id_response). Discovery requests carry only the
// app-layer CRC8, no CRC16. Returns the packet length (11) or 0 on error.
uint8_t hm_build_search_id_request(uint32_t dtu_serial, uint8_t *buffer, size_t buffer_len);

// Build the "Collect RF hw/sw info" (opcode 0x06) discovery request. It leaks the serial even while
// the inverter is bound to a DTU, so it is the Search-ID fallback. Returns the length (12) or 0.
uint8_t hm_build_collect_info_request(uint32_t dtu_serial, uint8_t *buffer, size_t buffer_len);

// Parse a reply to a Search-ID request (opcode 0x82 = 0x02 | all-frames). Fills *out with the
// discovered serial suffix, echoed DTU serial, and (on the full-length reply) model PID. Validates
// the trailing CRC8. Returns false on a malformed reply.
bool hm_parse_search_id_response(const uint8_t *packet, uint8_t len, HmDiscoveredInverter *out);

// Parse a reply to a Collect-Info request. The observed reply echoes opcode 0x06 (a firmware quirk;
// a spec-correct reply would set the response bit, 0x86) — both are accepted. Fills the serial
// suffix; pid stays 0 (a collect-info reply carries no PID). Validates CRC8.
bool hm_parse_collect_info_response(const uint8_t *packet, uint8_t len, HmDiscoveredInverter *out);

}  // namespace esphome::hoymiles_dtu
