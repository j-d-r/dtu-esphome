// Native unit test for the production protocol decoder (components/hoymiles_dtu_base/protocol.cpp).
//
// This compiles and links the real component code off-device so the request framing, CRCs, frame
// parsing/assembly, and the 1-/2-/4-channel realtime decoders are tested directly (no Python
// re-implementation to drift out of sync). Run via `pytest tests/unit/test_native_protocol.py`,
// which compiles this file together with protocol.cpp.

#include "protocol.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace esphome {
// Deterministic stand-in for the on-device MAC helper referenced by hm_generate_dtu_serial.
void get_mac_address_raw(uint8_t *mac) {
  const uint8_t fake[6] = {0x24, 0x6F, 0x28, 0xAB, 0xCD, 0xEF};
  memcpy(mac, fake, sizeof(fake));
}
}  // namespace esphome

using namespace esphome::hoymiles_dtu;

static int g_failures = 0;

#define CHECK(cond)                                               \
  do {                                                            \
    if (!(cond)) {                                                \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      g_failures++;                                               \
    }                                                             \
  } while (0)

static bool near(float a, float b, float eps = 0.001f) { return std::fabs(a - b) <= eps; }

#define CHECK_NEAR(a, b)                                                                                         \
  do {                                                                                                           \
    if (!near((a), (b))) {                                                                                       \
      std::printf("FAIL %s:%d: %s (%.4f) != %s (%.4f)\n", __FILE__, __LINE__, #a, (double)(a), #b, (double)(b)); \
      g_failures++;                                                                                              \
    }                                                                                                            \
  } while (0)

static void put16(uint8_t *buf, size_t off, uint16_t v) {
  buf[off] = (v >> 8) & 0xFF;
  buf[off + 1] = v & 0xFF;
}

static void put32(uint8_t *buf, size_t off, uint32_t v) {
  buf[off] = (v >> 24) & 0xFF;
  buf[off + 1] = (v >> 16) & 0xFF;
  buf[off + 2] = (v >> 8) & 0xFF;
  buf[off + 3] = v & 0xFF;
}

// Wrap raw record field bytes into one on-air fragment packet the way the inverter sends them:
// [0]=0x95, [1..4]=inverter id, [9]=fragment id (|0x80 marks the last), [10..]=field bytes, +CRC8.
static uint8_t build_fragment(uint64_t radio_id, uint8_t frame_id, bool last, const uint8_t *field, uint8_t field_len,
                              uint8_t *out) {
  memset(out, 0, 32);
  out[0] = 0x95;
  for (uint8_t i = 0; i < 4; i++) {
    out[1 + i] = (radio_id >> (8 * (i + 1))) & 0xFF;
  }
  out[9] = frame_id | (last ? 0x80 : 0x00);
  memcpy(&out[10], field, field_len);
  const uint8_t len = 10 + field_len + 1;
  out[len - 1] = hm_crc8(out, len - 1);
  return len;
}

static void test_crc_vectors() {
  const uint8_t v8[] = {0x15, 0x01, 0x02, 0x03, 0x04};
  CHECK(hm_crc8(v8, sizeof(v8)) == 0x11);
  const uint8_t v16[] = {0x0b, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
  CHECK(hm_crc16(v16, sizeof(v16)) == 0x7BAF);
}

static void test_radio_id_and_address() {
  // Printed (hex) serial 0x116182806989 -> address 01 82 80 69 89 (proven against the live HM-1200).
  const uint64_t radio_id = hm_radio_id_from_serial(0x116182806989ULL);
  CHECK(radio_id == 0x8969808201ULL);
  uint8_t addr[5];
  hm_radio_id_to_address(radio_id, addr);
  const uint8_t expected[5] = {0x01, 0x82, 0x80, 0x69, 0x89};
  CHECK(memcmp(addr, expected, 5) == 0);
}

static void test_realtime_request_byte_order() {
  const uint64_t radio_id = hm_radio_id_from_serial(0x116182806989ULL);
  uint8_t req[32];
  const uint8_t len = hm_build_realtime_request(radio_id, 0x83915460UL, 0x12345678UL, req, sizeof(req));
  CHECK(len == 27);
  CHECK(req[0] == 0x15);
  // [1..4] = LSB-first of (radio_id>>8); [5..8] = big-endian dtu_serial (Ahoy wire order).
  const uint8_t inv[4] = {0x82, 0x80, 0x69, 0x89};  // = inverter address[1..4], LSB-first of radio_id>>8
  const uint8_t dtu[4] = {0x83, 0x91, 0x54, 0x60};
  CHECK(memcmp(&req[1], inv, 4) == 0);
  CHECK(memcmp(&req[5], dtu, 4) == 0);
  CHECK(req[9] == 0x80 && req[10] == 0x0B && req[11] == 0x00);
  const uint8_t ts[4] = {0x12, 0x34, 0x56, 0x78};  // big-endian timestamp
  CHECK(memcmp(&req[12], ts, 4) == 0);
  const uint16_t crc16 = hm_crc16(&req[10], 14);
  CHECK(req[24] == ((crc16 >> 8) & 0xFF) && req[25] == (crc16 & 0xFF));
  CHECK(req[26] == hm_crc8(req, 26));
}

static void test_power_limit_request() {
  const uint64_t radio_id = hm_radio_id_from_serial(0x116182806989ULL);
  uint8_t pkt[32];
  const uint8_t len = hm_build_power_limit_request(radio_id, 0x83915460UL, 100, true, false, pkt, sizeof(pkt));
  CHECK(len == 19);
  CHECK(pkt[0] == 0x51);
  const uint8_t inv[4] = {0x82, 0x80, 0x69, 0x89};  // = inverter address[1..4], LSB-first of radio_id>>8
  CHECK(memcmp(&pkt[1], inv, 4) == 0);
  CHECK(pkt[9] == 0x81 && pkt[10] == 0x0B && pkt[11] == 0x00);
  CHECK(pkt[12] == 0x03 && pkt[13] == 0xE8);  // 100% -> 1000 (0x03E8)
  CHECK(pkt[14] == 0x01 && pkt[15] == 0x01);  // persistent
  const uint16_t crc16 = hm_crc16(&pkt[10], 6);
  CHECK(pkt[16] == ((crc16 >> 8) & 0xFF) && pkt[17] == (crc16 & 0xFF));
  CHECK(pkt[18] == hm_crc8(pkt, 18));
  // Non-persistent uses control 0x0001, and percent is rejected above 100.
  CHECK(hm_build_power_limit_request(radio_id, 0x83915460UL, 50, false, false, pkt, sizeof(pkt)) == 19);
  CHECK(pkt[15] == 0x01 && pkt[14] == 0x00);
  CHECK(hm_build_power_limit_request(radio_id, 0x83915460UL, 101, true, false, pkt, sizeof(pkt)) == 0);

  // HMS inverters encode RelativPersistent as 0x0003 instead of the HM 0x0101, and share the
  // 0x0001 non-persistent word (OpenDTU ActivePowerControlCommand).
  CHECK(hm_build_power_limit_request(radio_id, 0x83915460UL, 70, true, true, pkt, sizeof(pkt)) == 19);
  CHECK(pkt[12] == 0x02 && pkt[13] == 0xBC);  // 70% -> 700 (0x02BC)
  CHECK(pkt[14] == 0x00 && pkt[15] == 0x03);
  CHECK(pkt[18] == hm_crc8(pkt, 18));
  CHECK(hm_build_power_limit_request(radio_id, 0x83915460UL, 70, false, true, pkt, sizeof(pkt)) == 19);
  CHECK(pkt[14] == 0x00 && pkt[15] == 0x01);
}

static void test_channel_change_request() {
  const uint64_t radio_id = hm_radio_id_from_serial(0x114482806989ULL);
  uint8_t pkt[32];
  // EU: 865.00 MHz is channel 20 above the 860 MHz base.
  const uint8_t len = hm_build_channel_change_request(radio_id, 0x83915460UL, HM_REGION_EU, 20, pkt, sizeof(pkt));
  CHECK(len == 15);
  CHECK(pkt[0] == 0x56);
  const uint8_t inv[4] = {0x82, 0x80, 0x69, 0x89};
  CHECK(memcmp(&pkt[1], inv, 4) == 0);
  const uint8_t dtu[4] = {0x83, 0x91, 0x54, 0x60};
  CHECK(memcmp(&pkt[5], dtu, 4) == 0);
  CHECK(pkt[9] == 0x02 && pkt[10] == 0x15 && pkt[11] == 0x21);  // 860 MHz band magic
  CHECK(pkt[12] == 20 && pkt[13] == 0x14);
  // Only a CRC8 over the whole packet - no CRC16 over the control data.
  CHECK(pkt[14] == hm_crc8(pkt, 14));
  // The 900 MHz bands carry different band bytes.
  CHECK(hm_build_channel_change_request(radio_id, 0x83915460UL, HM_REGION_US, 72, pkt, sizeof(pkt)) == 15);
  CHECK(pkt[9] == 0x03 && pkt[10] == 0x17 && pkt[11] == 0x3C && pkt[12] == 72);
  CHECK(hm_build_channel_change_request(radio_id, 0x83915460UL, HM_REGION_BRAZIL, 72, pkt, sizeof(pkt)) == 15);
  CHECK(pkt[9] == 0x03);
  // Too small a buffer is rejected rather than truncated.
  CHECK(hm_build_channel_change_request(radio_id, 0x83915460UL, HM_REGION_EU, 20, pkt, 14) == 0);
}

// The CMT2300A has no hardware address filter, so replies are matched on the DTU serial the
// inverter echoes back in [5..8].
static void test_packet_addressed_to_dtu() {
  uint8_t pkt[16];
  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x95;
  pkt[5] = 0x83;
  pkt[6] = 0x91;
  pkt[7] = 0x54;
  pkt[8] = 0x60;
  CHECK(hm_packet_addressed_to_dtu(pkt, sizeof(pkt), 0x83915460UL));
  CHECK(!hm_packet_addressed_to_dtu(pkt, sizeof(pkt), 0x83915461UL));
  CHECK(!hm_packet_addressed_to_dtu(pkt, 8, 0x83915460UL));  // too short to hold the field
  CHECK(!hm_packet_addressed_to_dtu(nullptr, sizeof(pkt), 0x83915460UL));
}

// HMS records are laid out differently from HM records with the same channel count: the DC
// channels interleave, and the 4-channel model reports a voltage per input instead of per MPPT
// pair. Offsets per Ahoy hms{1,2,4}chAssignment / OpenDTU HMS_{1CH,2CH,4CH}.
static void test_decode_hms_1ch() {
  uint8_t p[32];
  memset(p, 0, sizeof(p));
  put16(p, 2, 3500);    // udc 350.0
  put16(p, 4, 400);     // idc 4.00
  put16(p, 6, 1400);    // pdc 140.0
  put32(p, 8, 250000);  // yt 250.000
  put16(p, 12, 900);    // yd 900
  put16(p, 14, 2295);   // uac 229.5
  put16(p, 16, 4998);   // f 49.98
  put16(p, 18, 1380);   // pac 138.0
  put16(p, 22, 60);     // iac 0.60
  put16(p, 26, 310);    // temp 31.0
  put16(p, 28, 1);      // evt
  HmTelemetry t;
  // An HMS 1-channel record is byte-for-byte an HM 1-channel record.
  CHECK(hm_model_channel_count(HMS_500) == 1);
  CHECK(hm_model_is_hms(HMS_500));
  CHECK(!hm_model_is_hms(HM_400));
  CHECK(hm_parse_realtime_payload(HMS_500, p, 30, &t));
  CHECK_NEAR(t.channels[0].dc_voltage, 350.0f);
  CHECK_NEAR(t.channels[0].dc_power, 140.0f);
  CHECK_NEAR(t.ac_voltage, 229.5f);
  CHECK_NEAR(t.ac_power, 138.0f);
  CHECK_NEAR(t.temperature, 31.0f);
  CHECK_NEAR(t.yield_today, 900.0f);
  CHECK_NEAR(t.yield_total, 250.0f);
  CHECK(t.event_code == 1);
}

static void test_decode_hms_1ch_v2() {
  uint8_t p[44];
  memset(p, 0, sizeof(p));
  put16(p, 2, 3600);     // udc 360.0
  put16(p, 6, 410);      // idc 4.10
  put16(p, 10, 1470);    // pdc 147.0
  put32(p, 14, 260000);  // yt 260.000
  put16(p, 22, 950);     // yd 950
  put16(p, 18, 7);       // evt (in the slot a 2-channel record uses for channel 2's yield total)
  put16(p, 20, 40);      // q 4.0
  put16(p, 26, 2301);    // uac 230.1
  put16(p, 28, 5002);    // f 50.02
  put16(p, 30, 1450);    // pac 145.0
  put16(p, 34, 63);      // iac 0.63
  put16(p, 36, 995);     // pf 0.995
  put16(p, 38, 322);     // temp 32.2
  HmTelemetry t;
  CHECK(hm_model_channel_count(HMS_500_V2) == 1);
  CHECK(hm_parse_realtime_payload(HMS_500_V2, p, 40, &t));
  CHECK_NEAR(t.channels[0].dc_voltage, 360.0f);
  CHECK_NEAR(t.channels[0].dc_current, 4.1f);
  CHECK_NEAR(t.channels[0].dc_power, 147.0f);
  CHECK_NEAR(t.ac_voltage, 230.1f);
  CHECK_NEAR(t.ac_power, 145.0f);
  CHECK_NEAR(t.reactive_power, 4.0f);
  CHECK_NEAR(t.power_factor, 0.995f);
  CHECK_NEAR(t.temperature, 32.2f);
  CHECK(t.event_code == 7);
  // A record one field shorter than the layout needs must be rejected, not decoded past the end.
  CHECK(!hm_parse_realtime_payload(HMS_500_V2, p, 39, &t));
}

static void test_decode_hms_2ch() {
  uint8_t p[44];
  memset(p, 0, sizeof(p));
  put16(p, 2, 3400);     // ch1 udc 340.0
  put16(p, 6, 480);      // ch1 idc 4.80
  put16(p, 10, 1630);    // ch1 pdc 163.0
  put32(p, 14, 300000);  // ch1 yt 300.000
  put16(p, 22, 700);     // ch1 yd 700
  put16(p, 4, 3450);     // ch2 udc 345.0
  put16(p, 8, 500);      // ch2 idc 5.00
  put16(p, 12, 1720);    // ch2 pdc 172.0
  put32(p, 18, 310000);  // ch2 yt 310.000
  put16(p, 24, 750);     // ch2 yd 750
  put16(p, 26, 2310);    // uac 231.0
  put16(p, 28, 5000);    // f 50.00
  put16(p, 30, 3300);    // pac 330.0
  put16(p, 32, 70);      // q 7.0
  put16(p, 34, 143);     // iac 1.43
  put16(p, 36, 990);     // pf 0.990
  put16(p, 38, 335);     // temp 33.5
  put16(p, 40, 2);       // evt
  HmTelemetry t;
  CHECK(hm_model_channel_count(HMS_800) == 2);
  CHECK(hm_parse_realtime_payload(HMS_800, p, 42, &t));
  CHECK_NEAR(t.channels[0].dc_voltage, 340.0f);
  CHECK_NEAR(t.channels[0].dc_current, 4.8f);
  CHECK_NEAR(t.channels[0].dc_power, 163.0f);
  CHECK_NEAR(t.channels[0].yield_total, 300.0f);
  CHECK_NEAR(t.channels[1].dc_voltage, 345.0f);
  CHECK_NEAR(t.channels[1].dc_current, 5.0f);
  CHECK_NEAR(t.channels[1].dc_power, 172.0f);
  CHECK_NEAR(t.channels[1].yield_today, 750.0f);
  CHECK_NEAR(t.ac_voltage, 231.0f);
  CHECK_NEAR(t.ac_power, 330.0f);
  CHECK_NEAR(t.reactive_power, 7.0f);
  CHECK_NEAR(t.ac_current, 1.43f);
  CHECK_NEAR(t.power_factor, 0.99f);
  CHECK_NEAR(t.temperature, 33.5f);
  CHECK(t.event_code == 2);
  CHECK_NEAR(t.yield_today, 1450.0f);
  CHECK_NEAR(t.yield_total, 610.0f);
  // The same bytes decoded as an HM 2-channel record must NOT match: the families differ.
  HmTelemetry hm;
  CHECK(hm_parse_realtime_payload(HM_800, p, 42, &hm));
  CHECK(!near(hm.channels[0].dc_current, t.channels[0].dc_current));
}

// HERF-300 per OpenDTU HERF_1CH.cpp: HMS-450/500 v2 DC offsets, 2-channel AC offsets, and both
// reactive power and the event code read from offset 40.
static void test_decode_herf_1ch() {
  uint8_t p[44];
  memset(p, 0, sizeof(p));
  put16(p, 2, 3300);     // udc 330.0
  put16(p, 6, 520);      // idc 5.20
  put16(p, 10, 1716);    // pdc 171.6
  put32(p, 14, 120000);  // yt 120.000
  put16(p, 22, 640);     // yd 640
  put16(p, 26, 2298);    // uac 229.8
  put16(p, 28, 4998);    // f 49.98
  put16(p, 30, 1650);    // pac 165.0
  put16(p, 34, 72);      // iac 0.72
  put16(p, 36, 998);     // pf 0.998
  put16(p, 38, 287);     // temp 28.7
  put16(p, 40, 25);      // q 2.5 and evt 25 (same slot)
  HmTelemetry t;
  CHECK(hm_model_channel_count(HERF_300) == 1);
  CHECK(!hm_model_is_hms(HERF_300));
  CHECK(strcmp(hm_model_to_string(HERF_300), "HERF-300") == 0);
  CHECK(hm_parse_realtime_payload(HERF_300, p, 42, &t));
  CHECK_NEAR(t.channels[0].dc_voltage, 330.0f);
  CHECK_NEAR(t.channels[0].dc_current, 5.2f);
  CHECK_NEAR(t.channels[0].dc_power, 171.6f);
  CHECK_NEAR(t.channels[0].yield_today, 640.0f);
  CHECK_NEAR(t.channels[0].yield_total, 120.0f);
  CHECK_NEAR(t.ac_voltage, 229.8f);
  CHECK_NEAR(t.ac_frequency, 49.98f);
  CHECK_NEAR(t.ac_power, 165.0f);
  CHECK_NEAR(t.ac_current, 0.72f);
  CHECK_NEAR(t.power_factor, 0.998f);
  CHECK_NEAR(t.temperature, 28.7f);
  CHECK_NEAR(t.reactive_power, 2.5f);
  CHECK(t.event_code == 25);
  CHECK(!hm_parse_realtime_payload(HERF_300, p, 41, &t));
}

static bool same_telemetry(const HmTelemetry &a, const HmTelemetry &b) {
  if (a.valid != b.valid || a.ac_voltage != b.ac_voltage || a.ac_current != b.ac_current || a.ac_power != b.ac_power ||
      a.reactive_power != b.reactive_power || a.ac_frequency != b.ac_frequency || a.power_factor != b.power_factor ||
      a.temperature != b.temperature || a.yield_today != b.yield_today || a.yield_total != b.yield_total ||
      a.event_code != b.event_code) {
    return false;
  }
  for (uint8_t i = 0; i < HM_MAX_CHANNELS; i++) {
    const HmChannelData &x = a.channels[i];
    const HmChannelData &y = b.channels[i];
    if (x.dc_voltage != y.dc_voltage || x.dc_current != y.dc_current || x.dc_power != y.dc_power ||
        x.yield_today != y.yield_today || x.yield_total != y.yield_total) {
      return false;
    }
  }
  return true;
}

// HERF-600/800 decode exactly like HMS-600..1000, and HERF-1600/1800 exactly like HM-1000..1500.
static void test_decode_herf_2ch_4ch() {
  uint8_t p[64];
  for (size_t i = 0; i < sizeof(p); i++) {
    p[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  HmTelemetry herf;
  HmTelemetry reference;
  CHECK(hm_model_channel_count(HERF_800) == 2);
  CHECK(!hm_model_is_hms(HERF_800));
  CHECK(hm_parse_realtime_payload(HERF_800, p, 42, &herf));
  CHECK(hm_parse_realtime_payload(HMS_800, p, 42, &reference));
  CHECK(same_telemetry(herf, reference));
  CHECK(!hm_parse_realtime_payload(HERF_600, p, 41, &herf));

  CHECK(hm_model_channel_count(HERF_1800) == 4);
  CHECK(!hm_model_is_hms(HERF_1800));
  CHECK(hm_parse_realtime_payload(HERF_1800, p, 62, &herf));
  CHECK(hm_parse_realtime_payload(HM_1500, p, 62, &reference));
  CHECK(same_telemetry(herf, reference));
  CHECK(strcmp(hm_model_to_string(HERF_1600), "HERF-1600") == 0);
}

static void test_decode_hms_4ch() {
  uint8_t p[68];
  memset(p, 0, sizeof(p));
  put16(p, 2, 3300);     // ch1 udc 330.0
  put16(p, 6, 300);      // ch1 idc 3.00
  put16(p, 10, 990);     // ch1 pdc 99.0
  put32(p, 14, 100000);  // ch1 yt 100.000
  put16(p, 22, 400);     // ch1 yd 400
  put16(p, 4, 3320);     // ch2 udc 332.0
  put16(p, 8, 310);      // ch2 idc 3.10
  put16(p, 12, 1020);    // ch2 pdc 102.0
  put32(p, 18, 110000);  // ch2 yt 110.000
  put16(p, 24, 420);     // ch2 yd 420
  put16(p, 26, 3280);    // ch3 udc 328.0
  put16(p, 30, 320);     // ch3 idc 3.20
  put16(p, 34, 1050);    // ch3 pdc 105.0
  put32(p, 38, 120000);  // ch3 yt 120.000
  put16(p, 46, 430);     // ch3 yd 430
  put16(p, 28, 3260);    // ch4 udc 326.0 (its own voltage, unlike the HM 4-channel layout)
  put16(p, 32, 330);     // ch4 idc 3.30
  put16(p, 36, 1080);    // ch4 pdc 108.0
  put32(p, 42, 130000);  // ch4 yt 130.000
  put16(p, 48, 440);     // ch4 yd 440
  put16(p, 50, 2320);    // uac 232.0
  put16(p, 52, 4999);    // f 49.99
  put16(p, 54, 4100);    // pac 410.0
  put16(p, 56, 90);      // q 9.0
  put16(p, 58, 177);     // iac 1.77
  put16(p, 60, 985);     // pf 0.985
  put16(p, 62, 341);     // temp 34.1
  put16(p, 64, 3);       // evt
  HmTelemetry t;
  CHECK(hm_model_channel_count(HMS_2000) == 4);
  CHECK(hm_parse_realtime_payload(HMS_2000, p, 66, &t));
  CHECK_NEAR(t.channels[0].dc_voltage, 330.0f);
  CHECK_NEAR(t.channels[1].dc_voltage, 332.0f);
  CHECK_NEAR(t.channels[2].dc_voltage, 328.0f);
  CHECK_NEAR(t.channels[3].dc_voltage, 326.0f);
  CHECK_NEAR(t.channels[2].dc_power, 105.0f);
  CHECK_NEAR(t.channels[3].dc_current, 3.3f);
  CHECK_NEAR(t.channels[3].yield_total, 130.0f);
  CHECK_NEAR(t.ac_voltage, 232.0f);
  CHECK_NEAR(t.ac_frequency, 49.99f);
  CHECK_NEAR(t.ac_power, 410.0f);
  CHECK_NEAR(t.temperature, 34.1f);
  CHECK(t.event_code == 3);
  CHECK_NEAR(t.yield_today, 1690.0f);
  CHECK_NEAR(t.yield_total, 460.0f);
  CHECK(!hm_parse_realtime_payload(HMS_2000, p, 65, &t));
}

static void test_frame_parse_and_assembly() {
  const uint64_t radio_id = hm_radio_id_from_serial(0x116182806989ULL);
  uint8_t f1[32], f2[32];
  const uint8_t d1[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  const uint8_t d2[3] = {0x11, 0x22, 0x33};
  const uint8_t l1 = build_fragment(radio_id, 1, false, d1, sizeof(d1), f1);
  const uint8_t l2 = build_fragment(radio_id, 2, true, d2, sizeof(d2), f2);

  HmFrame fr1, fr2;
  CHECK(hm_parse_frame(f1, l1, radio_id, &fr1));
  CHECK(fr1.id == 1 && fr1.len == 4 && memcmp(fr1.data.data(), d1, 4) == 0);
  CHECK(hm_parse_frame(f2, l2, radio_id, &fr2));
  CHECK(fr2.id == 2 && fr2.len == 3);

  // Wrong header, bad CRC, wrong inverter id, and too-short packets are rejected.
  HmFrame junk;
  uint8_t bad[32];
  memcpy(bad, f1, l1);
  bad[0] = 0x94;
  CHECK(!hm_parse_frame(bad, l1, radio_id, &junk));
  memcpy(bad, f1, l1);
  bad[l1 - 1] ^= 0xFF;
  CHECK(!hm_parse_frame(bad, l1, radio_id, &junk));
  CHECK(!hm_parse_frame(f1, l1, radio_id ^ 0xFF00ULL, &junk));
  CHECK(!hm_parse_frame(f1, 8, radio_id, &junk));

  // Assembly concatenates by ascending id and fails on a gap.
  HmFrame frames[2] = {fr2, fr1};  // out of order on purpose
  uint8_t payload[16];
  uint8_t plen = 0;
  CHECK(hm_assemble_payload(frames, 2, payload, sizeof(payload), &plen));
  CHECK(plen == 7);
  const uint8_t expected[7] = {0xAA, 0xBB, 0xCC, 0xDD, 0x11, 0x22, 0x33};
  CHECK(memcmp(payload, expected, 7) == 0);
  HmFrame gap[1] = {fr2};  // only id 2, id 1 missing
  CHECK(!hm_assemble_payload(gap, 1, payload, sizeof(payload), &plen));
}

static void test_decode_1ch() {
  uint8_t p[32];
  memset(p, 0, sizeof(p));
  put16(p, 2, 400);     // udc 40.0 V
  put16(p, 4, 850);     // idc 8.50 A
  put16(p, 6, 3400);    // pdc 340.0 W
  put32(p, 8, 888600);  // yt 888.600 kWh
  put16(p, 12, 1234);   // yd 1234 Wh
  put16(p, 14, 2382);   // uac 238.2 V
  put16(p, 16, 4999);   // f 49.99 Hz
  put16(p, 18, 3350);   // pac 335.0 W
  put16(p, 20, 100);    // q 10.0 var
  put16(p, 22, 141);    // iac 1.41 A
  put16(p, 24, 999);    // pf 0.999
  put16(p, 26, 313);    // temp 31.3 C
  HmTelemetry t;
  CHECK(hm_model_channel_count(HM_400) == 1);
  CHECK(hm_parse_realtime_payload(HM_400, p, 30, &t));
  CHECK_NEAR(t.channels[0].dc_voltage, 40.0f);
  CHECK_NEAR(t.channels[0].dc_current, 8.5f);
  CHECK_NEAR(t.channels[0].dc_power, 340.0f);
  CHECK_NEAR(t.channels[0].yield_today, 1234.0f);
  CHECK_NEAR(t.channels[0].yield_total, 888.6f);
  CHECK_NEAR(t.ac_voltage, 238.2f);
  CHECK_NEAR(t.ac_frequency, 49.99f);
  CHECK_NEAR(t.ac_power, 335.0f);
  CHECK_NEAR(t.reactive_power, 10.0f);
  CHECK_NEAR(t.ac_current, 1.41f);
  CHECK_NEAR(t.power_factor, 0.999f);
  CHECK_NEAR(t.temperature, 31.3f);
  CHECK_NEAR(t.yield_today, 1234.0f);
  CHECK_NEAR(t.yield_total, 888.6f);
  // Below the minimum record length must fail.
  CHECK(!hm_parse_realtime_payload(HM_400, p, 29, &t));
}

static void test_decode_2ch() {
  uint8_t p[44];
  memset(p, 0, sizeof(p));
  put16(p, 2, 3500);     // ch1 udc 350.0
  put16(p, 4, 500);      // ch1 idc 5.00
  put16(p, 6, 1750);     // ch1 pdc 175.0
  put32(p, 14, 400000);  // ch1 yt 400.000
  put16(p, 22, 600);     // ch1 yd 600
  put16(p, 8, 3480);     // ch2 udc 348.0
  put16(p, 10, 520);     // ch2 idc 5.20
  put16(p, 12, 1800);    // ch2 pdc 180.0
  put32(p, 18, 410000);  // ch2 yt 410.000
  put16(p, 24, 650);     // ch2 yd 650
  put16(p, 26, 2300);    // uac 230.0
  put16(p, 28, 5001);    // f 50.01
  put16(p, 30, 3550);    // pac 355.0
  put16(p, 32, 50);      // q 5.0
  put16(p, 34, 155);     // iac 1.55
  put16(p, 36, 980);     // pf 0.980
  put16(p, 38, 280);     // temp 28.0
  HmTelemetry t;
  CHECK(hm_model_channel_count(HM_800) == 2);
  CHECK(hm_parse_realtime_payload(HM_800, p, 42, &t));
  CHECK_NEAR(t.channels[0].dc_voltage, 350.0f);
  CHECK_NEAR(t.channels[0].dc_power, 175.0f);
  CHECK_NEAR(t.channels[1].dc_voltage, 348.0f);
  CHECK_NEAR(t.channels[1].dc_current, 5.2f);
  CHECK_NEAR(t.channels[1].yield_total, 410.0f);
  CHECK_NEAR(t.ac_voltage, 230.0f);
  CHECK_NEAR(t.ac_frequency, 50.01f);
  CHECK_NEAR(t.temperature, 28.0f);
  CHECK_NEAR(t.yield_today, 1250.0f);
  CHECK_NEAR(t.yield_total, 810.0f);
}

static void test_decode_4ch() {
  uint8_t p[64];
  memset(p, 0, sizeof(p));
  put16(p, 2, 3300);     // ch1 udc 330.0
  put16(p, 4, 250);      // ch1 idc 2.50
  put16(p, 8, 800);      // ch1 pdc 80.0
  put32(p, 12, 100000);  // ch1 yt 100.000
  put16(p, 20, 100);     // ch1 yd 100
  put16(p, 6, 260);      // ch2 idc 2.60 (udc shared with ch1)
  put16(p, 10, 850);     // ch2 pdc 85.0
  put32(p, 16, 110000);  // ch2 yt 110.000
  put16(p, 22, 110);     // ch2 yd 110
  put16(p, 24, 3400);    // ch3 udc 340.0
  put16(p, 26, 270);     // ch3 idc 2.70
  put16(p, 30, 900);     // ch3 pdc 90.0
  put32(p, 34, 120000);  // ch3 yt 120.000
  put16(p, 42, 120);     // ch3 yd 120
  put16(p, 28, 280);     // ch4 idc 2.80 (udc shared with ch3)
  put16(p, 32, 950);     // ch4 pdc 95.0
  put32(p, 38, 130000);  // ch4 yt 130.000
  put16(p, 44, 130);     // ch4 yd 130
  put16(p, 46, 2382);    // uac 238.2
  put16(p, 48, 4999);    // f 49.99
  put16(p, 50, 3550);    // pac 355.0
  put16(p, 52, 120);     // q 12.0
  put16(p, 54, 155);     // iac 1.55
  put16(p, 56, 990);     // pf 0.990
  put16(p, 58, 313);     // temp 31.3
  put16(p, 60, 7);       // evt
  HmTelemetry t;
  CHECK(hm_model_channel_count(HM_1200) == 4);
  CHECK(hm_parse_realtime_payload(HM_1200, p, 62, &t));
  CHECK_NEAR(t.channels[0].dc_voltage, 330.0f);
  CHECK_NEAR(t.channels[1].dc_voltage, 330.0f);  // shared with ch1
  CHECK_NEAR(t.channels[1].dc_current, 2.6f);
  CHECK_NEAR(t.channels[2].dc_voltage, 340.0f);
  CHECK_NEAR(t.channels[3].dc_voltage, 340.0f);  // shared with ch3
  CHECK_NEAR(t.channels[3].dc_power, 95.0f);
  CHECK_NEAR(t.channels[2].yield_total, 120.0f);
  CHECK_NEAR(t.ac_voltage, 238.2f);
  CHECK_NEAR(t.ac_frequency, 49.99f);
  CHECK_NEAR(t.reactive_power, 12.0f);
  CHECK_NEAR(t.power_factor, 0.99f);
  CHECK_NEAR(t.temperature, 31.3f);
  CHECK(t.event_code == 7);
  CHECK_NEAR(t.yield_today, 460.0f);
  CHECK_NEAR(t.yield_total, 460.0f);
  // Negative temperature (two's complement) decodes as a signed value.
  put16(p, 58, 0xFFCE);  // -50 -> -5.0 C
  CHECK(hm_parse_realtime_payload(HM_1200, p, 62, &t));
  CHECK_NEAR(t.temperature, -5.0f);
}

// Real 4-fragment HM-1200 realtime record captured on-air 2026-07-01 (serial 116182806989). Runs
// the full receive path (per-fragment CRC8 check -> assemble -> decode) and asserts the exact values
// the device logged for this exchange: "AC 236.8V 49.99Hz 15.2W 0.06A 19.8var pf=0.61 | DC 16.0W |
// 27.1C | YieldToday 18Wh YieldTotal 888.664kWh | evt=3".
static void test_decode_4ch_golden_capture() {
  const uint64_t radio_id = hm_radio_id_from_serial(0x116182806989ULL);
  const uint8_t fragments[4][27] = {
      {0x95, 0x82, 0x80, 0x69, 0x89, 0x82, 0x80, 0x69, 0x89, 0x01, 0x00, 0x01, 0x01, 0x70,
       0x00, 0x15, 0x00, 0x01, 0x00, 0x4D, 0x00, 0x05, 0x00, 0x06, 0xC6, 0xD6, 0xAE},
      {0x95, 0x82, 0x80, 0x69, 0x89, 0x82, 0x80, 0x69, 0x89, 0x02, 0x00, 0x00, 0x07, 0x58,
       0x00, 0x09, 0x00, 0x00, 0x01, 0x6D, 0x00, 0x01, 0x00, 0x14, 0x00, 0x05, 0xBD},
      {0x95, 0x82, 0x80, 0x69, 0x89, 0x82, 0x80, 0x69, 0x89, 0x03, 0x00, 0x49, 0x00, 0x00,
       0x33, 0x83, 0x00, 0x06, 0x8D, 0xA7, 0x00, 0x00, 0x00, 0x09, 0x09, 0x40, 0x03},
      {0x95, 0x82, 0x80, 0x69, 0x89, 0x82, 0x80, 0x69, 0x89, 0x84, 0x13, 0x87, 0x00, 0x98,
       0x00, 0xC6, 0x00, 0x06, 0x02, 0x63, 0x01, 0x0F, 0x00, 0x03, 0x86, 0x4B, 0x7C},
  };
  HmFrame frames[4];
  for (uint8_t i = 0; i < 4; i++) {
    CHECK(hm_parse_frame(fragments[i], 27, radio_id, &frames[i]));  // real per-fragment CRC8 must pass
    CHECK(frames[i].id == i + 1 && frames[i].len == 16);
  }
  uint8_t payload[HM_MAX_PAYLOAD_SIZE];
  uint8_t plen = 0;
  CHECK(hm_assemble_payload(frames, 4, payload, sizeof(payload), &plen));
  CHECK(plen == 64);
  HmTelemetry t;
  CHECK(hm_parse_realtime_payload(HM_1200, payload, plen, &t));
  CHECK_NEAR(t.ac_voltage, 236.8f);
  CHECK_NEAR(t.ac_frequency, 49.99f);
  CHECK_NEAR(t.ac_power, 15.2f);
  CHECK_NEAR(t.ac_current, 0.06f);
  CHECK_NEAR(t.reactive_power, 19.8f);
  CHECK_NEAR(t.power_factor, 0.611f);
  CHECK_NEAR(t.temperature, 27.1f);
  CHECK(t.event_code == 3);
  CHECK_NEAR(t.yield_today, 18.0f);
  CHECK(near(t.yield_total, 888.664f, 0.01f));
  CHECK_NEAR(t.channels[0].dc_voltage, 36.8f);
  CHECK_NEAR(t.channels[1].dc_voltage, 36.8f);  // shared with ch1
  CHECK_NEAR(t.channels[3].dc_voltage, 36.5f);  // shared with ch3
  const float dc_power =
      t.channels[0].dc_power + t.channels[1].dc_power + t.channels[2].dc_power + t.channels[3].dc_power;
  CHECK_NEAR(dc_power, 16.0f);
}

// Passive scan detector: classify DTU->inverter requests overheard on a monitored pipe. Uses the
// two real on-air captures from the CCC report so the offsets and CRC8 handling match the wild.
static void test_classify_sniffed() {
  const uint64_t our_inv = hm_radio_id_from_serial(0x116182806989ULL);  // address 01 82 80 69 89
  const uint32_t our_dtu = 0x83915460UL;
  HmSniffResult r;

  // Real Search-ID broadcast (report fig. 3), attacker DTU serial 0x80187264.
  const uint8_t search_id[] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x80, 0x18, 0x72, 0x64, 0x00, 0x8C};
  CHECK(hm_classify_sniffed_packet(search_id, sizeof(search_id), our_inv, our_dtu, &r));
  CHECK(r.kind == HM_SNIFF_SEARCH_ID && r.sender_dtu_serial == 0x80187264UL);
  CHECK(r.severity == HM_SEVERITY_MEDIUM);

  // Real 0x06 collect-info probe (report sec 4.3), serial one byte later at 0x80187265.
  const uint8_t collect[] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x18, 0x72, 0x65, 0x00, 0x89};
  CHECK(hm_classify_sniffed_packet(collect, sizeof(collect), our_inv, our_dtu, &r));
  CHECK(r.kind == HM_SNIFF_COLLECT_INFO && r.sender_dtu_serial == 0x80187265UL);
  CHECK(r.severity == HM_SEVERITY_MEDIUM);

  // A foreign DevControl aimed at OUR inverter -> flagged as the most serious case.
  uint8_t ctrl[32];
  uint8_t clen = hm_build_power_limit_request(our_inv, 0x11223344UL, 0, true, false, ctrl, sizeof(ctrl));
  CHECK(clen == 19);
  CHECK(hm_classify_sniffed_packet(ctrl, clen, our_inv, our_dtu, &r));
  CHECK(r.kind == HM_SNIFF_FOREIGN_CONTROL && r.targets_our_inverter && r.sender_dtu_serial == 0x11223344UL);
  CHECK(r.severity == HM_SEVERITY_CRITICAL);

  // A foreign telemetry poll (0x15) aimed at OUR inverter -> high severity.
  uint8_t poll[32];
  const uint8_t plen2 = hm_build_realtime_request(our_inv, 0x11223344UL, 0x64656667UL, poll, sizeof(poll));
  CHECK(plen2 == 27);
  CHECK(hm_classify_sniffed_packet(poll, plen2, our_inv, our_dtu, &r));
  CHECK(r.kind == HM_SNIFF_FOREIGN_POLL && r.targets_our_inverter && r.severity == HM_SEVERITY_HIGH);

  // Our own DevControl (sender == our DTU serial) is not an intrusion.
  clen = hm_build_power_limit_request(our_inv, our_dtu, 50, true, false, ctrl, sizeof(ctrl));
  CHECK(!hm_classify_sniffed_packet(ctrl, clen, our_inv, our_dtu, &r));

  // A control command to a DIFFERENT inverter is not our concern.
  const uint64_t other_inv = hm_radio_id_from_serial(0x114180000000ULL);
  clen = hm_build_power_limit_request(other_inv, 0x11223344UL, 0, true, false, ctrl, sizeof(ctrl));
  CHECK(!hm_classify_sniffed_packet(ctrl, clen, our_inv, our_dtu, &r));

  // Corrupted CRC8, an inverter response (bit7 set), and a runt packet are all rejected.
  uint8_t bad[11];
  memcpy(bad, search_id, sizeof(bad));
  bad[10] ^= 0xFF;
  CHECK(!hm_classify_sniffed_packet(bad, sizeof(bad), our_inv, our_dtu, &r));
  uint8_t resp[11];
  memcpy(resp, search_id, sizeof(resp));
  resp[0] = 0x95;
  resp[10] = hm_crc8(resp, 10);
  CHECK(!hm_classify_sniffed_packet(resp, sizeof(resp), our_inv, our_dtu, &r));
  CHECK(!hm_classify_sniffed_packet(search_id, 8, our_inv, our_dtu, &r));

  // Direct severity mapping + labels.
  CHECK(hm_sniff_severity(HM_SNIFF_SEARCH_ID) == HM_SEVERITY_MEDIUM);
  CHECK(hm_sniff_severity(HM_SNIFF_FOREIGN_POLL) == HM_SEVERITY_HIGH);
  CHECK(hm_sniff_severity(HM_SNIFF_FOREIGN_CONTROL) == HM_SEVERITY_CRITICAL);
  CHECK(hm_sniff_severity(HM_SNIFF_NONE) == HM_SEVERITY_NONE);
  CHECK(strcmp(hm_severity_to_string(HM_SEVERITY_CRITICAL), "CRITICAL") == 0);
  CHECK(strcmp(hm_severity_to_string(HM_SEVERITY_HIGH), "HIGH") == 0);
}

// Active scanner: build the two discovery requests and confirm they reproduce the exact on-air bytes
// captured in the CCC report (section 4). The transmit side must agree with the receive-side
// classifier, so a request we build is also what the passive detector recognises.
static void test_scan_requests() {
  const uint32_t dtu = 0x80187264UL;
  uint8_t sid[32];
  const uint8_t sid_len = hm_build_search_id_request(dtu, sid, sizeof(sid));
  CHECK(sid_len == 11);
  // Report fig. 3 TX: 02 00 00 00 00 80 18 72 64 00 8C.
  const uint8_t expect_sid[11] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x80, 0x18, 0x72, 0x64, 0x00, 0x8C};
  CHECK(memcmp(sid, expect_sid, sizeof(expect_sid)) == 0);

  uint8_t ci[32];
  const uint8_t ci_len = hm_build_collect_info_request(0x80187265UL, ci, sizeof(ci));
  CHECK(ci_len == 12);
  // Report section 4.3 TX: 06 00 00 00 00 00 80 18 72 65 00 89 (one wildcard byte longer, serial +1).
  const uint8_t expect_ci[12] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x18, 0x72, 0x65, 0x00, 0x89};
  CHECK(memcmp(ci, expect_ci, sizeof(expect_ci)) == 0);

  // Undersized buffers are rejected.
  CHECK(hm_build_search_id_request(dtu, sid, 10) == 0);
  CHECK(hm_build_collect_info_request(dtu, ci, 11) == 0);

  // Round-trip: what the scanner transmits is exactly what the passive detector classifies.
  const uint64_t inv = hm_radio_id_from_serial(0x116182806989ULL);
  HmSniffResult s;
  CHECK(hm_classify_sniffed_packet(sid, sid_len, inv, 0x83915460UL, &s));
  CHECK(s.kind == HM_SNIFF_SEARCH_ID && s.sender_dtu_serial == dtu);
  CHECK(hm_classify_sniffed_packet(ci, ci_len, inv, 0x83915460UL, &s));
  CHECK(s.kind == HM_SNIFF_COLLECT_INFO && s.sender_dtu_serial == 0x80187265UL);
}

// Active scanner: parse discovery replies, using the report's real HMS-600 captures.
static void test_scan_responses() {
  HmDiscoveredInverter d;
  // Report fig. 3 RX_raw: serial suffix 84 56 00 17 (hex), echoed DTU 80 18 72 64, PID 0x1144.
  const uint8_t sid_resp[] = {0x82, 0x84, 0x56, 0x00, 0x17, 0x80, 0x18, 0x72,
                              0x64, 0x11, 0x44, 0x84, 0x56, 0x00, 0x17, 0x59};
  CHECK(hm_parse_search_id_response(sid_resp, sizeof(sid_resp), &d));
  CHECK(d.serial_suffix == 0x84560017UL);
  CHECK(d.pid == 0x1144);
  CHECK(d.responder_dtu_serial == 0x80187264UL);

  // Report section 4.3 RX: 06 00 00 00 00 85 03 40 22 00 E2 — serial 85 03 40 22, no PID.
  const uint8_t ci_resp[] = {0x06, 0x00, 0x00, 0x00, 0x00, 0x85, 0x03, 0x40, 0x22, 0x00, 0xE2};
  CHECK(hm_parse_collect_info_response(ci_resp, sizeof(ci_resp), &d));
  CHECK(d.serial_suffix == 0x85034022UL);
  CHECK(d.pid == 0);

  // A collect-info reply that sets the response bit (0x86, the spec-correct form) is also accepted.
  uint8_t ci_ok[11];
  memcpy(ci_ok, ci_resp, sizeof(ci_ok));
  ci_ok[0] = 0x86;
  ci_ok[10] = hm_crc8(ci_ok, 10);
  CHECK(hm_parse_collect_info_response(ci_ok, sizeof(ci_ok), &d) && d.serial_suffix == 0x85034022UL);

  // Real Search-ID reply captured on-air from the live HM-1200 (2026-07-10 daylight unbound scan).
  // Unlike the report's HMS-600 (which echoes the DTU serial at [5..8]), this inverter echoes its
  // OWN serial there and repeats it again at [11..14]. The parser must still extract serial suffix
  // 82806989 (= serial & 0xFFFFFFFF = the low radio-address bytes) and PID 0x1161, with a valid CRC8.
  const uint8_t hm1200_resp[] = {0x82, 0x82, 0x80, 0x69, 0x89, 0x82, 0x80, 0x69,
                                 0x89, 0x11, 0x61, 0x82, 0x80, 0x69, 0x89, 0x10};
  CHECK(hm_crc8(hm1200_resp, 15) == hm1200_resp[15]);  // the real trailing CRC8 validates
  CHECK(hm_parse_search_id_response(hm1200_resp, sizeof(hm1200_resp), &d));
  CHECK(d.serial_suffix == 0x82806989UL);
  CHECK(d.serial_suffix == static_cast<uint32_t>(0x116182806989ULL & 0xFFFFFFFFUL));
  CHECK(d.pid == 0x1161);
  // This inverter echoes its own serial at [5..8], not the DTU serial — the reason the component's
  // scan-packet filter must accept a self-echo, not only a DTU-serial echo (handle_scan_packet_).
  CHECK(d.responder_dtu_serial == 0x82806989UL);
  uint8_t addr[5];
  hm_radio_id_to_address(hm_radio_id_from_serial(0x116182806989ULL), addr);
  CHECK(addr[1] == 0x82 && addr[2] == 0x80 && addr[3] == 0x69 && addr[4] == 0x89);

  // Corrupted CRC8, a request opcode instead of a reply, and runt packets are rejected.
  uint8_t bad[16];
  memcpy(bad, sid_resp, sizeof(bad));
  bad[15] ^= 0xFF;
  CHECK(!hm_parse_search_id_response(bad, sizeof(bad), &d));
  memcpy(bad, sid_resp, sizeof(bad));
  bad[0] = 0x02;  // the request opcode, not the 0x82 reply
  CHECK(!hm_parse_search_id_response(bad, sizeof(bad), &d));
  CHECK(!hm_parse_search_id_response(sid_resp, 8, &d));
  CHECK(!hm_parse_collect_info_response(ci_resp, 8, &d));
}

int main() {
  test_crc_vectors();
  test_radio_id_and_address();
  test_realtime_request_byte_order();
  test_power_limit_request();
  test_channel_change_request();
  test_packet_addressed_to_dtu();
  test_frame_parse_and_assembly();
  test_decode_1ch();
  test_decode_2ch();
  test_decode_4ch();
  test_decode_4ch_golden_capture();
  test_decode_hms_1ch();
  test_decode_hms_1ch_v2();
  test_decode_hms_2ch();
  test_decode_hms_4ch();
  test_decode_herf_1ch();
  test_decode_herf_2ch_4ch();
  test_classify_sniffed();
  test_scan_requests();
  test_scan_responses();
  if (g_failures == 0) {
    std::printf("OK: all native protocol tests passed\n");
    return 0;
  }
  std::printf("%d native protocol assertion(s) failed\n", g_failures);
  return 1;
}
