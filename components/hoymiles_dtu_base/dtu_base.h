#pragma once

#include "protocol.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include <array>
#include <string>
#include <vector>

namespace esphome::hoymiles_dtu {

struct HmChannelSensors {
  sensor::Sensor *dc_voltage{nullptr};
  sensor::Sensor *dc_current{nullptr};
  sensor::Sensor *dc_power{nullptr};
  sensor::Sensor *yield_today{nullptr};
};

class HoymilesDtuInverter {
 public:
  void set_serial(uint64_t serial);
  void set_model(HmModel model) { model_ = model; }
  void set_ac_power_sensor(sensor::Sensor *sensor) { ac_power_sensor_ = sensor; }
  void set_ac_voltage_sensor(sensor::Sensor *sensor) { ac_voltage_sensor_ = sensor; }
  void set_ac_current_sensor(sensor::Sensor *sensor) { ac_current_sensor_ = sensor; }
  void set_ac_frequency_sensor(sensor::Sensor *sensor) { ac_frequency_sensor_ = sensor; }
  void set_reactive_power_sensor(sensor::Sensor *sensor) { reactive_power_sensor_ = sensor; }
  void set_power_factor_sensor(sensor::Sensor *sensor) { power_factor_sensor_ = sensor; }
  void set_temperature_sensor(sensor::Sensor *sensor) { temperature_sensor_ = sensor; }
  void set_yield_today_sensor(sensor::Sensor *sensor) { yield_today_sensor_ = sensor; }
  void set_yield_total_sensor(sensor::Sensor *sensor) { yield_total_sensor_ = sensor; }
  void set_status_text_sensor(text_sensor::TextSensor *sensor) { status_text_sensor_ = sensor; }
  void set_last_seen_text_sensor(text_sensor::TextSensor *sensor) { last_seen_text_sensor_ = sensor; }
  void set_signal_strength_sensor(sensor::Sensor *sensor) { signal_strength_sensor_ = sensor; }
  void set_channel_dc_voltage_sensor(uint8_t channel, sensor::Sensor *sensor);
  void set_channel_dc_current_sensor(uint8_t channel, sensor::Sensor *sensor);
  void set_channel_dc_power_sensor(uint8_t channel, sensor::Sensor *sensor);
  void set_channel_yield_today_sensor(uint8_t channel, sensor::Sensor *sensor);

  uint64_t get_serial() const { return serial_; }
  uint64_t get_radio_id() const { return radio_id_; }
  HmModel get_model() const { return model_; }
  HmStatus get_status() const { return status_; }
  uint32_t get_last_seen() const { return last_seen_ms_; }
  const char *get_last_error() const { return last_error_; }

  // One telemetry poll per update interval: update() queues every inverter, and the hub's loop
  // drains the queue one exchange at a time.
  bool poll_pending() const { return poll_pending_; }
  void queue_poll() { poll_pending_ = true; }
  void mark_poll_started() { poll_pending_ = false; }
  void publish_telemetry(const HmTelemetry &telemetry, uint32_t now);
  void mark_offline_if_expired(uint32_t now, uint32_t timeout_ms);
  void set_last_error(const char *error) { last_error_ = error; }
  // Received signal strength of the last accepted reply, in dBm. Only the CMT2300A reports it; the
  // nRF24 has no per-packet RSSI, so its hub never calls this.
  void publish_signal_strength(int8_t rssi_dbm);

 protected:
  void publish_status_();

  uint64_t serial_{0};
  uint64_t radio_id_{0};
  HmModel model_{HM_1500};
  HmStatus status_{OFFLINE};
  bool poll_pending_{false};
  uint32_t last_seen_ms_{0};
  const char *last_error_{"never polled"};
  HmTelemetry telemetry_{};
  sensor::Sensor *ac_power_sensor_{nullptr};
  sensor::Sensor *ac_voltage_sensor_{nullptr};
  sensor::Sensor *ac_current_sensor_{nullptr};
  sensor::Sensor *ac_frequency_sensor_{nullptr};
  sensor::Sensor *reactive_power_sensor_{nullptr};
  sensor::Sensor *power_factor_sensor_{nullptr};
  sensor::Sensor *temperature_sensor_{nullptr};
  sensor::Sensor *yield_today_sensor_{nullptr};
  sensor::Sensor *yield_total_sensor_{nullptr};
  sensor::Sensor *signal_strength_sensor_{nullptr};
  text_sensor::TextSensor *status_text_sensor_{nullptr};
  text_sensor::TextSensor *last_seen_text_sensor_{nullptr};
  std::array<HmChannelSensors, HM_MAX_CHANNELS> channels_{};
};

// Radio-independent half of the DTU hub: the inverter list, the poll scheduler, the request
// builders, fragment collection/decoding, the queued power-limit command, and every published
// entity. The radio-specific halves derive from it and drive their own TX/RX state machine:
// HoymilesDtuRf24 (nRF24, HM models) and HoymilesDtuCmt (CMT2300A, HMS models).
class HoymilesDtuBase : public PollingComponent {
 public:
  void set_scan_detection(bool enabled) { scan_detection_ = enabled; }
  void set_dtu_serial(uint32_t dtu_serial) {
    dtu_serial_ = dtu_serial;
    dtu_serial_configured_ = true;
  }
  void add_inverter(HoymilesDtuInverter *inverter) { inverters_.push_back(inverter); }
  void set_last_rx_payload_text_sensor(text_sensor::TextSensor *sensor) { last_rx_payload_text_sensor_ = sensor; }
  void set_last_radio_error_text_sensor(text_sensor::TextSensor *sensor) { last_radio_error_text_sensor_ = sensor; }
  void set_scan_detected_text_sensor(text_sensor::TextSensor *sensor) { scan_detected_text_sensor_ = sensor; }
  void set_scan_severity_sensor(sensor::Sensor *sensor) { scan_severity_sensor_ = sensor; }
  void set_scan_result_text_sensor(text_sensor::TextSensor *sensor) { scan_result_text_sensor_ = sensor; }
  void set_scan_found_count_sensor(sensor::Sensor *sensor) { scan_found_count_sensor_ = sensor; }

  // Queue an active-power-limit DevControl exchange for the first configured inverter. The radio's
  // state machine picks it up on the next idle loop, so this never blocks or transmits inline.
  void radio_set_power_limit(uint16_t percent, bool persistent);

 protected:
  enum class ExchangeKind : uint8_t {
    TELEMETRY,
    DEV_CONTROL,
    SCAN,
  };

  // Clear the per-exchange fragment/counter state and adopt `inverter` as the exchange target.
  void begin_exchange_state_(ExchangeKind kind, HoymilesDtuInverter *inverter);
  // Build the realtime-data request for `inverter` into request_/request_len_. Uses wall-clock time
  // when it is available, else uptime seconds. Returns false when the build failed.
  bool build_telemetry_request_(HoymilesDtuInverter *inverter, uint32_t now);
  // Build the queued active-power-limit request for `inverter` into request_/request_len_.
  bool build_power_limit_request_(HoymilesDtuInverter *inverter);
  // Account for one received packet: publish the raw-payload diagnostic, then either check it as a
  // DevControl confirmation or parse/deduplicate it as a realtime fragment.
  void ingest_packet_(const uint8_t *packet, uint8_t len);
  // Assemble the collected fragments, decode them for the active inverter's model, and publish.
  // Returns true only once a complete, decodable record has been published.
  bool process_response_(uint32_t now);
  void publish_radio_error_(const char *error);
  // Report a passively-overheard foreign request. `channel` is the radio channel it was heard on
  // (an nRF24 RF channel, or a CMT2300A frequency channel index).
  void report_scan_(const HmSniffResult &sniff, uint8_t channel, uint32_t now);
  void expire_offline_inverters_(uint32_t now);
  // Queue a poll of every inverter; called once per update interval.
  void queue_poll_cycle_();
  HoymilesDtuInverter *next_due_inverter_();
  void log_dtu_config_();

  std::vector<HoymilesDtuInverter *> inverters_;
  uint32_t dtu_serial_{0};
  bool dtu_serial_configured_{false};
  bool radio_ok_{false};
  bool scan_detection_{false};
  uint32_t scan_count_{0};
  uint32_t scan_last_report_ms_{0};
  ExchangeKind exchange_kind_{ExchangeKind::TELEMETRY};
  HoymilesDtuInverter *active_inverter_{nullptr};
  uint32_t request_started_ms_{0};
  uint32_t rx_started_ms_{0};
  uint8_t expected_frames_{HM_MAX_FRAME_COUNT};
  uint8_t frame_count_{0};
  uint8_t rx_packet_count_{0};
  uint8_t invalid_frame_count_{0};
  uint8_t duplicate_frame_count_{0};
  uint8_t request_[32]{};
  uint8_t request_len_{0};
  bool power_limit_pending_{false};
  uint16_t power_limit_percent_{100};
  bool power_limit_persistent_{true};
  bool dev_ctrl_got_response_{false};
  bool dev_ctrl_confirmed_{false};
  std::array<HmFrame, HM_MAX_FRAME_COUNT> frames_{};
  text_sensor::TextSensor *last_rx_payload_text_sensor_{nullptr};
  text_sensor::TextSensor *last_radio_error_text_sensor_{nullptr};
  text_sensor::TextSensor *scan_detected_text_sensor_{nullptr};
  sensor::Sensor *scan_severity_sensor_{nullptr};
  text_sensor::TextSensor *scan_result_text_sensor_{nullptr};
  sensor::Sensor *scan_found_count_sensor_{nullptr};
};

template <typename... Ts>
class RadioSetPowerLimitAction : public Action<Ts...>, public Parented<HoymilesDtuBase> {
 public:
  TEMPLATABLE_VALUE(uint16_t, percent)
  TEMPLATABLE_VALUE(bool, persistent)

  void play(const Ts &...x) override {
    this->parent_->radio_set_power_limit(this->percent_.value(x...), this->persistent_.value(x...));
  }
};

}  // namespace esphome::hoymiles_dtu
