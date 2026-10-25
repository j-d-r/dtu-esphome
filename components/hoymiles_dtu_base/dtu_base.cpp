#include "dtu_base.h"

#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace esphome {
namespace hoymiles_dtu {

static const char *const TAG = "hoymiles_dtu";

// Rate-limit the scan report text/log so a continuous scan does not flood them.
static constexpr uint32_t SCAN_REPORT_THROTTLE_MS = 2000;

void HoymilesDtuInverter::set_serial(uint64_t serial) {
  serial_ = serial;
  radio_id_ = hm_radio_id_from_serial(serial);
}

void HoymilesDtuInverter::set_channel_dc_voltage_sensor(uint8_t channel, sensor::Sensor *sensor) {
  if (channel >= 1 && channel <= HM_MAX_CHANNELS) {
    channels_[channel - 1].dc_voltage = sensor;
  }
}

void HoymilesDtuInverter::set_channel_dc_current_sensor(uint8_t channel, sensor::Sensor *sensor) {
  if (channel >= 1 && channel <= HM_MAX_CHANNELS) {
    channels_[channel - 1].dc_current = sensor;
  }
}

void HoymilesDtuInverter::set_channel_dc_power_sensor(uint8_t channel, sensor::Sensor *sensor) {
  if (channel >= 1 && channel <= HM_MAX_CHANNELS) {
    channels_[channel - 1].dc_power = sensor;
  }
}

void HoymilesDtuInverter::set_channel_yield_today_sensor(uint8_t channel, sensor::Sensor *sensor) {
  if (channel >= 1 && channel <= HM_MAX_CHANNELS) {
    channels_[channel - 1].yield_today = sensor;
  }
}

void HoymilesDtuInverter::publish_telemetry(const HmTelemetry &telemetry, uint32_t now) {
  telemetry_ = telemetry;
  last_seen_ms_ = now;
  last_error_ = "";
  status_ = telemetry.ac_power > 0.1f ? PRODUCING : ONLINE;

  if (ac_power_sensor_ != nullptr) {
    ac_power_sensor_->publish_state(telemetry.ac_power);
  }
  if (ac_voltage_sensor_ != nullptr) {
    ac_voltage_sensor_->publish_state(telemetry.ac_voltage);
  }
  if (ac_current_sensor_ != nullptr) {
    ac_current_sensor_->publish_state(telemetry.ac_current);
  }
  if (ac_frequency_sensor_ != nullptr) {
    ac_frequency_sensor_->publish_state(telemetry.ac_frequency);
  }
  if (reactive_power_sensor_ != nullptr) {
    reactive_power_sensor_->publish_state(telemetry.reactive_power);
  }
  if (power_factor_sensor_ != nullptr) {
    power_factor_sensor_->publish_state(telemetry.power_factor);
  }
  if (temperature_sensor_ != nullptr) {
    temperature_sensor_->publish_state(telemetry.temperature);
  }
  if (yield_today_sensor_ != nullptr) {
    yield_today_sensor_->publish_state(telemetry.yield_today);
  }
  if (yield_total_sensor_ != nullptr) {
    yield_total_sensor_->publish_state(telemetry.yield_total);
  }
  const uint8_t channel_count = hm_model_channel_count(model_);
  for (uint8_t i = 0; i < HM_MAX_CHANNELS; i++) {
    const bool in_range = i < channel_count;
    const auto &data = telemetry.channels[i];
    const auto &sensors = channels_[i];
    if (sensors.dc_voltage != nullptr) {
      sensors.dc_voltage->publish_state(in_range ? data.dc_voltage : NAN);
    }
    if (sensors.dc_current != nullptr) {
      sensors.dc_current->publish_state(in_range ? data.dc_current : NAN);
    }
    if (sensors.dc_power != nullptr) {
      sensors.dc_power->publish_state(in_range ? data.dc_power : NAN);
    }
    if (sensors.yield_today != nullptr) {
      sensors.yield_today->publish_state(in_range ? data.yield_today : NAN);
    }
  }
  publish_status_();
}

void HoymilesDtuInverter::publish_signal_strength(int8_t rssi_dbm) {
  if (signal_strength_sensor_ != nullptr) {
    signal_strength_sensor_->publish_state(rssi_dbm);
  }
}

void HoymilesDtuInverter::mark_offline_if_expired(uint32_t now, uint32_t timeout_ms) {
  if (status_ == OFFLINE || last_seen_ms_ == 0 || now - last_seen_ms_ < timeout_ms) {
    return;
  }
  status_ = OFFLINE;
  last_error_ = "offline timeout";
  publish_status_();
}

void HoymilesDtuInverter::publish_status_() {
  if (status_text_sensor_ != nullptr) {
    status_text_sensor_->publish_state(hm_status_to_string(status_));
  }
  if (last_seen_text_sensor_ != nullptr && last_seen_ms_ != 0) {
    char buffer[24];
    snprintf(buffer, sizeof(buffer), "%us", static_cast<unsigned>(last_seen_ms_ / 1000));
    last_seen_text_sensor_->publish_state(buffer);
  }
}

void HoymilesDtuBase::radio_set_power_limit(uint16_t percent, bool persistent) {
  if (!radio_ok_) {
    publish_radio_error_("radio not ready");
    return;
  }
  if (inverters_.empty()) {
    publish_radio_error_("no inverter configured");
    return;
  }
  power_limit_percent_ = percent;
  power_limit_persistent_ = persistent;
  power_limit_pending_ = true;
}

void HoymilesDtuBase::begin_exchange_state_(ExchangeKind kind, HoymilesDtuInverter *inverter) {
  exchange_kind_ = kind;
  active_inverter_ = inverter;
  frame_count_ = 0;
  // Unknown until the inverter sends the fragment with the 0x80 "last" bit; until then never treat
  // the record as complete. (ingest_packet_ lowers this to the real count.)
  expected_frames_ = HM_MAX_FRAME_COUNT;
  rx_packet_count_ = 0;
  invalid_frame_count_ = 0;
  duplicate_frame_count_ = 0;
  dev_ctrl_got_response_ = false;
  dev_ctrl_confirmed_ = false;
  request_len_ = 0;
}

bool HoymilesDtuBase::build_telemetry_request_(HoymilesDtuInverter *inverter, uint32_t now) {
  uint32_t request_timestamp = now / 1000;
  const std::time_t epoch = std::time(nullptr);
  if (epoch > 1600000000) {
    request_timestamp = static_cast<uint32_t>(epoch);
  }
  request_len_ =
      hm_build_realtime_request(inverter->get_radio_id(), dtu_serial_, request_timestamp, request_, sizeof(request_));
  return request_len_ != 0;
}

bool HoymilesDtuBase::build_power_limit_request_(HoymilesDtuInverter *inverter) {
  request_len_ =
      hm_build_power_limit_request(inverter->get_radio_id(), dtu_serial_, power_limit_percent_, power_limit_persistent_,
                                   hm_model_is_hms(inverter->get_model()), request_, sizeof(request_));
  return request_len_ != 0;
}

void HoymilesDtuBase::ingest_packet_(const uint8_t *packet, uint8_t len) {
  rx_packet_count_++;
  if (last_rx_payload_text_sensor_ != nullptr) {
    last_rx_payload_text_sensor_->publish_state(format_hex_pretty(packet, len));
  }
  if (active_inverter_ == nullptr) {
    invalid_frame_count_++;
    return;
  }
  if (exchange_kind_ == ExchangeKind::DEV_CONTROL) {
    // DevControl confirmation (0xD1) doesn't fit the 0x95 realtime-frame shape hm_parse_frame
    // expects; check the raw layout directly (Ahoy parseDevCtrl: [10]==[11]==0 accepted).
    if (len >= 14 && packet[0] == (HM_TX_REQ_DEVCONTROL | HM_ALL_FRAMES) && packet[12] == HM_DEV_CTRL_ACTIVE_POWER) {
      dev_ctrl_got_response_ = true;
      dev_ctrl_confirmed_ = (packet[10] == 0x00 && packet[11] == 0x00 && packet[13] == 0x00);
    }
    return;
  }
  if (frame_count_ >= HM_MAX_FRAME_COUNT) {
    invalid_frame_count_++;
    return;
  }
  HmFrame frame;
  if (!hm_parse_frame(packet, len, active_inverter_->get_radio_id(), &frame)) {
    invalid_frame_count_++;
    ESP_LOGW(TAG, "Ignored invalid HM frame len=%u data=%s", len, format_hex_pretty(packet, len).c_str());
    return;
  }
  for (uint8_t i = 0; i < frame_count_; i++) {
    if (frames_[i].id == frame.id) {
      frames_[i] = frame;
      duplicate_frame_count_++;
      return;
    }
  }
  frames_[frame_count_++] = frame;
  ESP_LOGD(TAG, "Accepted HM frame id=%u len=%u count=%u/%u", frame.id, frame.len, frame_count_, expected_frames_);
  // The high bit of the fragment counter (packet[9] & 0x80) marks the final fragment; its id is
  // the total fragment count for this inverter's record (varies, e.g. 4 here, not a fixed 7).
  if ((packet[9] & 0x80) != 0) {
    expected_frames_ = frame.id;
  }
}

bool HoymilesDtuBase::process_response_(uint32_t now) {
  if (active_inverter_ == nullptr || frame_count_ < expected_frames_) {
    return false;
  }
  uint8_t payload[HM_MAX_PAYLOAD_SIZE];
  uint8_t payload_len = 0;
  if (!hm_assemble_payload(frames_.data(), expected_frames_, payload, sizeof(payload), &payload_len)) {
    return false;
  }
  HmTelemetry telemetry;
  if (!hm_parse_realtime_payload(active_inverter_->get_model(), payload, payload_len, &telemetry)) {
    active_inverter_->set_last_error("payload parse failed");
    return false;
  }
  float dc_power = 0.0f;
  for (uint8_t c = 0; c < hm_model_channel_count(active_inverter_->get_model()); c++) {
    dc_power += telemetry.channels[c].dc_power;
  }
  ESP_LOGI(TAG,
           "Telemetry %012llX: AC %.1fV %.2fHz %.1fW %.2fA %.1fvar pf=%.2f | DC %.1fW | %.1f°C | "
           "YieldToday %.0fWh YieldTotal %.3fkWh | evt=%u",
           active_inverter_->get_serial(), telemetry.ac_voltage, telemetry.ac_frequency, telemetry.ac_power,
           telemetry.ac_current, telemetry.reactive_power, telemetry.power_factor, dc_power, telemetry.temperature,
           telemetry.yield_today, telemetry.yield_total, telemetry.event_code);
  active_inverter_->publish_telemetry(telemetry, now);
  return true;
}

void HoymilesDtuBase::publish_radio_error_(const char *error) {
  ESP_LOGW(TAG, "Radio error: %s", error);
  if (last_radio_error_text_sensor_ != nullptr) {
    last_radio_error_text_sensor_->publish_state(error);
  }
}

void HoymilesDtuBase::report_scan_(const HmSniffResult &sniff, uint8_t channel, uint32_t now) {
  scan_count_++;
  const char *kind = "radio activity";
  switch (sniff.kind) {
    case HM_SNIFF_SEARCH_ID:
      kind = "Search-ID scan";
      break;
    case HM_SNIFF_COLLECT_INFO:
      kind = "info probe";
      break;
    case HM_SNIFF_FOREIGN_POLL:
      kind = "foreign poll";
      break;
    case HM_SNIFF_FOREIGN_CONTROL:
      kind = "FOREIGN DevControl";
      break;
    case HM_SNIFF_NONE:
      break;
  }
  const char *severity = hm_severity_to_string(sniff.severity);
  ESP_LOGD(TAG, "Scan packet: %s %s opcode=0x%02X dtu=0x%08X ch=%u targets_us=%d", severity, kind, sniff.opcode,
           static_cast<unsigned>(sniff.sender_dtu_serial), channel, sniff.targets_our_inverter);
  // Publish the numeric severity on every classified packet (un-throttled, de-duplicated) so an
  // escalation to a more serious opcode is surfaced immediately, not hidden behind the text throttle.
  if (scan_severity_sensor_ != nullptr) {
    const float value = static_cast<float>(sniff.severity);
    if (!scan_severity_sensor_->has_state() || scan_severity_sensor_->state != value) {
      scan_severity_sensor_->publish_state(value);
    }
  }
  // Rate-limit the WARN/ERROR log and the published text so a continuous scan does not flood them;
  // the running count still climbs so Home Assistant sees each throttled update advance.
  if (scan_last_report_ms_ != 0 && now - scan_last_report_ms_ < SCAN_REPORT_THROTTLE_MS) {
    return;
  }
  scan_last_report_ms_ = now;
  char buffer[96];
  snprintf(buffer, sizeof(buffer), "%s | %s | DTU 0x%08X | ch %u | #%u", severity, kind,
           static_cast<unsigned>(sniff.sender_dtu_serial), channel, static_cast<unsigned>(scan_count_));
  if (sniff.severity >= HM_SEVERITY_CRITICAL) {
    ESP_LOGE(TAG, "Radio scan detected: %s", buffer);
  } else {
    ESP_LOGW(TAG, "Radio scan detected: %s", buffer);
  }
  if (scan_detected_text_sensor_ != nullptr) {
    scan_detected_text_sensor_->publish_state(buffer);
  }
}

void HoymilesDtuBase::expire_offline_inverters_(uint32_t now) {
  for (auto *inverter : inverters_) {
    inverter->mark_offline_if_expired(now, this->get_update_interval() * 4);
  }
}

void HoymilesDtuBase::queue_poll_cycle_() {
  for (auto *inverter : inverters_) {
    inverter->queue_poll();
  }
}

HoymilesDtuInverter *HoymilesDtuBase::next_due_inverter_() {
  for (auto *inverter : inverters_) {
    if (inverter->poll_pending()) {
      return inverter;
    }
  }
  return nullptr;
}

void HoymilesDtuBase::log_dtu_config_() {
  LOG_UPDATE_INTERVAL(this);
  ESP_LOGCONFIG(TAG, "  DTU serial: 0x%08X", static_cast<unsigned>(dtu_serial_));
  for (auto *inverter : inverters_) {
    ESP_LOGCONFIG(TAG, "  Inverter serial=%012llX model=%s status=%s last_seen=%us error=%s", inverter->get_serial(),
                  hm_model_to_string(inverter->get_model()), hm_status_to_string(inverter->get_status()),
                  static_cast<unsigned>(inverter->get_last_seen() / 1000), inverter->get_last_error());
  }
}

}  // namespace hoymiles_dtu
}  // namespace esphome
