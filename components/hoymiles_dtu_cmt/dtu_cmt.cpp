#include "dtu_cmt.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace hoymiles_dtu {

static const char *const TAG = "hoymiles_dtu";

// A packet is ~65 bytes on air at 20 kbps (30 preamble + 4 sync + payload + CRC), so ~30 ms; allow
// generous margin before declaring the transmission stuck.
static constexpr uint32_t CMT_TX_TIMEOUT_MS = 150;
// Ahoy budgets DURATION_TXFRAME (85 ms) + 50 ms per expected fragment + 115 ms reserve for CMT
// inverters. The longest HMS record (4-channel, 66 bytes) arrives in five fragments, so ~450 ms;
// round up for a slow or retransmitting inverter.
static constexpr uint32_t CMT_RX_TIMEOUT_MS = 700;
// Retransmits of the whole request when nothing at all came back, within one poll.
static constexpr uint8_t CMT_MAX_TX_ATTEMPTS = 3;
// Re-requests when the inverter answered but some fragments are missing (already-received fragments
// are kept).
static constexpr uint8_t CMT_MAX_RECORD_ATTEMPTS = 2;

void HoymilesDtuCmt::setup() {
  if (sclk_pin_ == nullptr || sdio_pin_ == nullptr || csb_pin_ == nullptr || fcsb_pin_ == nullptr) {
    ESP_LOGE(TAG, "CMT2300A pins are not configured");
    mark_failed();
    return;
  }
  if (!dtu_serial_configured_) {
    dtu_serial_ = hm_generate_dtu_serial();
  }

  const uint32_t work_frequency = frequency_hz_ != 0 ? frequency_hz_ : Cmt2300a::work_frequency_hz(region_);
  work_channel_ = Cmt2300a::channel_from_frequency(region_, work_frequency);
  boot_channel_ = Cmt2300a::channel_from_frequency(region_, Cmt2300a::boot_frequency_hz(region_));
  if (work_channel_ == 0xFF || boot_channel_ == 0xFF) {
    ESP_LOGE(TAG, "%.2f MHz is not a valid channel for this region", work_frequency / 1e6f);
    mark_failed();
    return;
  }

  if (gpio3_pin_ != nullptr) {
    gpio3_pin_->setup();
    gpio3_pin_->attach_interrupt(&HoymilesDtuCmt::gpio3_handler_, this, gpio::INTERRUPT_RISING_EDGE);
    gpio3_attached_ = true;
  } else if (gpio3_pin_base_ != nullptr) {
    gpio3_pin_base_->setup();
    ESP_LOGW(TAG, "GPIO3 pin is not an internal GPIO; polling the chip's interrupt flags instead");
  }

  radio_ok_ = radio_.setup();
  if (!radio_ok_) {
    ESP_LOGE(TAG, "CMT2300A radio probe failed");
    mark_failed();
    return;
  }
  // Park on the working frequency so an inverter that is already switched over is heard immediately.
  radio_.set_channel(work_channel_);
  ESP_LOGCONFIG(TAG, "CMT2300A radio ready on %.2f MHz, DTU serial 0x%08X", radio_.frequency_hz(work_channel_) / 1e6f,
                static_cast<unsigned>(dtu_serial_));

  for (auto *inverter : inverters_) {
    if (!hm_model_is_hms(inverter->get_model())) {
      ESP_LOGE(TAG, "Inverter %012llX is an %s: HM models use an nRF24 radio, not a CMT2300A", inverter->get_serial(),
               hm_model_to_string(inverter->get_model()));
    }
  }
}

void HoymilesDtuCmt::update() {
  if (!radio_ok_) {
    return;
  }
  // Only one exchange can be in flight, so queue every inverter and let loop() poll them back to
  // back. A poll still queued from the previous cycle (radio busy the whole time) simply stays queued.
  queue_poll_cycle_();
}

void HoymilesDtuCmt::loop() {
  if (!radio_ok_) {
    return;
  }
  const uint32_t now = millis();
  expire_offline_inverters_(now);
  // The GPIO3 interrupt only wakes the loop promptly; poll_rx_ reads the chip's own flags, so there
  // is no per-interrupt work to do here beyond clearing the ISR flag.
  packet_irq_ = false;

  switch (request_state_) {
    case RequestState::IDLE:
      if (power_limit_pending_) {
        if (monitoring_) {
          exit_monitor_();
        }
        power_limit_pending_ = false;
        start_power_limit_request_(now);
      } else if (HoymilesDtuInverter *inverter = next_due_inverter_()) {
        start_request_(inverter, now);
      } else if (scan_detection_) {
        poll_monitor_(now);
      }
      break;
    case RequestState::TX_WAIT:
      poll_tx_(now);
      break;
    case RequestState::RX_ACTIVE:
      poll_rx_(now);
      break;
  }
}

void HoymilesDtuCmt::dump_config() {
  ESP_LOGCONFIG(TAG, "Hoymiles DTU (HMS / CMT2300A):");
  LOG_PIN("  SCLK Pin: ", this->sclk_pin_);
  LOG_PIN("  SDIO Pin: ", this->sdio_pin_);
  LOG_PIN("  CSB Pin: ", this->csb_pin_);
  LOG_PIN("  FCSB Pin: ", this->fcsb_pin_);
  LOG_PIN("  GPIO3 Pin: ", this->gpio3_pin_base_);
  ESP_LOGCONFIG(TAG, "  SPI transport: %s", radio_.using_hardware_spi() ? "hardware (3-wire half-duplex)" : "bit-banged");
  ESP_LOGCONFIG(TAG, "  Packet-received wake-up: %s", gpio3_attached_ ? "GPIO3 interrupt" : "flag polling");
  ESP_LOGCONFIG(TAG, "  Radio: %s", radio_ok_ ? "CMT2300A connected" : "not connected");
  const char *region = region_ == HM_REGION_EU
                           ? "EU (860 MHz band)"
                           : (region_ == HM_REGION_US ? "US (900 MHz band)" : "Brazil (900 MHz band)");
  ESP_LOGCONFIG(TAG, "  Region: %s", region);
  ESP_LOGCONFIG(TAG, "  Frequency: %.2f MHz (channel %u), inverter boot frequency %.2f MHz (channel %u)",
                radio_.frequency_hz(work_channel_) / 1e6f, work_channel_, radio_.frequency_hz(boot_channel_) / 1e6f,
                boot_channel_);
  // Receive-only. Unlike the nRF24 monitor this ACKs nothing at all (the CMT2300A has no auto-ACK),
  // so it is fully passive - but it also has no address filter, hence the software DTU-id check.
  ESP_LOGCONFIG(TAG, "  Scan detection: %s",
                scan_detection_ ? "enabled (passive; classifies all traffic on the working frequency)" : "disabled");
  log_dtu_config_();
}

void HoymilesDtuCmt::start_request_(HoymilesDtuInverter *inverter, uint32_t now) {
  if (monitoring_) {
    exit_monitor_();
  }
  begin_exchange_state_(ExchangeKind::TELEMETRY, inverter);
  inverter->mark_poll_started();
  tx_attempt_ = 0;
  record_attempt_ = 0;
  got_rssi_ = false;
  high_freq_.start();

  if (!build_telemetry_request_(inverter, now)) {
    finish_request_(false, "request build failed", now);
    return;
  }
  // An inverter we have not heard from may have fallen back to its boot frequency (it does so after
  // ~15 minutes without a DTU). Tell it to come to ours first; that command is never answered, so it
  // costs one transmission and the telemetry request follows immediately after.
  if (inverter->get_status() == OFFLINE) {
    transmit_channel_change_(now);
  } else {
    transmit_request_(now);
  }
}

void HoymilesDtuCmt::start_power_limit_request_(uint32_t now) {
  HoymilesDtuInverter *inverter = inverters_.front();
  begin_exchange_state_(ExchangeKind::DEV_CONTROL, inverter);
  tx_attempt_ = 0;
  record_attempt_ = 0;
  got_rssi_ = false;
  high_freq_.start();

  if (!build_power_limit_request_(inverter)) {
    finish_request_(false, "power-limit build failed", now);
    return;
  }
  transmit_request_(now);
}

void HoymilesDtuCmt::transmit_channel_change_(uint32_t now) {
  uint8_t packet[32];
  const uint8_t len = hm_build_channel_change_request(active_inverter_->get_radio_id(), dtu_serial_, region_,
                                                      work_channel_, packet, sizeof(packet));
  if (len == 0) {
    transmit_request_(now);
    return;
  }
  tx_step_ = TxStep::CHANNEL_CHANGE;
  request_started_ms_ = now;
  tx_result_ = "not sent";
  // The inverter is listening on its boot frequency, so the command has to go out there.
  radio_.set_channel(boot_channel_);
  ESP_LOGD(TAG, "TX channel-change inverter=%012llX -> channel %u (%.2f MHz) on %.2f MHz data=%s",
           active_inverter_->get_serial(), work_channel_, radio_.frequency_hz(work_channel_) / 1e6f,
           radio_.frequency_hz(boot_channel_) / 1e6f, format_hex_pretty(packet, len).c_str());
  if (radio_.begin_transmit(packet, len)) {
    request_state_ = RequestState::TX_WAIT;
  } else {
    // Could not even start the transmission; skip straight to the request.
    radio_.set_channel(work_channel_);
    transmit_request_(now);
  }
}

void HoymilesDtuCmt::transmit_request_(uint32_t now) {
  if (active_inverter_ == nullptr) {
    finish_request_(false, "no inverter", now);
    return;
  }
  tx_step_ = TxStep::REQUEST;
  request_started_ms_ = now;
  tx_result_ = "not sent";
  radio_.set_channel(work_channel_);
  ESP_LOGD(TAG, "TX request inverter=%012llX %.2f MHz attempt=%u len=%u data=%s", active_inverter_->get_serial(),
           radio_.frequency_hz(work_channel_) / 1e6f, tx_attempt_, request_len_,
           format_hex_pretty(request_, request_len_).c_str());
  if (radio_.begin_transmit(request_, request_len_)) {
    request_state_ = RequestState::TX_WAIT;
  } else {
    finish_request_(false, "transmit failed", now);
  }
}

void HoymilesDtuCmt::poll_tx_(uint32_t now) {
  const bool done = radio_.transmit_done();
  if (!done && now - request_started_ms_ <= CMT_TX_TIMEOUT_MS) {
    return;
  }
  radio_.end_transmit();
  tx_result_ = done ? "TX_DONE" : "tx timeout";
  if (!done) {
    ESP_LOGW(TAG, "CMT2300A did not report TX done within %ums", static_cast<unsigned>(CMT_TX_TIMEOUT_MS));
  }
  if (tx_step_ == TxStep::CHANNEL_CHANGE) {
    // Nothing answers a channel change; carry on with the actual request on the working frequency.
    transmit_request_(now);
    return;
  }
  if (!done) {
    finish_request_(false, "tx timeout", now);
    return;
  }
  begin_rx_window_(now);
}

void HoymilesDtuCmt::begin_rx_window_(uint32_t now) {
  rx_started_ms_ = now;
  if (!radio_.start_listening()) {
    finish_request_(false, "receive mode failed", now);
    return;
  }
  request_state_ = RequestState::RX_ACTIVE;
}

void HoymilesDtuCmt::poll_rx_(uint32_t now) {
  read_available_packets_(now);

  if (exchange_kind_ == ExchangeKind::DEV_CONTROL) {
    if (dev_ctrl_confirmed_) {
      finish_request_(true, "", now);
      return;
    }
    if (now - rx_started_ms_ >= CMT_RX_TIMEOUT_MS) {
      // Silence can mean the request itself was lost - there is no link-layer ACK to tell us - so
      // retransmit before giving up. Re-sending the same limit is idempotent. A reply that was
      // heard and rejected is final, though, and must not be retried.
      if (!dev_ctrl_got_response_ && ++tx_attempt_ < CMT_MAX_TX_ATTEMPTS) {
        transmit_request_(now);
        return;
      }
      finish_request_(false, dev_ctrl_got_response_ ? "power-limit rejected" : "no confirmation", now);
    }
    return;
  }

  if (process_response_(now)) {
    if (got_rssi_ && active_inverter_ != nullptr) {
      active_inverter_->publish_signal_strength(last_rssi_dbm_);
    }
    finish_request_(true, "", now);
    return;
  }
  if (now - rx_started_ms_ < CMT_RX_TIMEOUT_MS) {
    return;
  }

  if (frame_count_ > 0 && frame_count_ < expected_frames_ && ++record_attempt_ <= CMT_MAX_RECORD_ATTEMPTS) {
    // The link is up but fragments are missing: ask for the record again, keeping what we have.
    ESP_LOGD(TAG, "Incomplete record %u/%u - re-requesting (attempt %u)", frame_count_, expected_frames_,
             record_attempt_);
    transmit_request_(now);
    return;
  }
  if (frame_count_ == 0 && ++tx_attempt_ < CMT_MAX_TX_ATTEMPTS) {
    // Nothing came back at all. There is no link-layer ACK on a CMT2300A, so a retransmit is the
    // only way to tell "inverter asleep/out of range" from "one lost packet".
    transmit_request_(now);
    return;
  }
  finish_request_(false, frame_count_ == 0 ? "no response" : "incomplete response", now);
}

void HoymilesDtuCmt::read_available_packets_(uint32_t now) {
  while (radio_.packet_available()) {
    uint8_t packet[CMT_MAX_PACKET_LEN];
    const int8_t rssi = radio_.rssi_dbm();
    const uint8_t len = radio_.read_packet(packet, sizeof(packet));
    if (len == 0) {
      break;
    }
    ESP_LOGD(TAG, "RX %.2f MHz %d dBm len=%u data=%s", radio_.frequency_hz(radio_.get_channel()) / 1e6f, rssi, len,
             format_hex_pretty(packet, len).c_str());
    // No hardware address filter: drop anything that is not addressed to this DTU. During an
    // exchange that also keeps a neighbour's traffic out of the fragment buffer.
    if (!hm_packet_addressed_to_dtu(packet, len, dtu_serial_)) {
      HmSniffResult sniff;
      const uint64_t inverter_radio_id = active_inverter_ != nullptr
                                             ? active_inverter_->get_radio_id()
                                             : (inverters_.empty() ? 0 : inverters_.front()->get_radio_id());
      if (scan_detection_ && hm_classify_sniffed_packet(packet, len, inverter_radio_id, dtu_serial_, &sniff)) {
        report_scan_(sniff, radio_.get_channel(), now);
      }
      continue;
    }
    last_rssi_dbm_ = rssi;
    got_rssi_ = true;
    ingest_packet_(packet, len);
  }
}

void HoymilesDtuCmt::finish_request_(bool success, const char *error, uint32_t now) {
  log_exchange_summary_(success, error, now);
  if (exchange_kind_ == ExchangeKind::DEV_CONTROL) {
    if (!success) {
      publish_radio_error_(error);
    }
  } else if (!success && active_inverter_ != nullptr) {
    active_inverter_->set_last_error(error);
  }
  radio_.stop_listening();
  active_inverter_ = nullptr;
  request_state_ = RequestState::IDLE;
  high_freq_.stop();
}

void HoymilesDtuCmt::log_exchange_summary_(bool success, const char *error, uint32_t now) {
  if (exchange_kind_ == ExchangeKind::DEV_CONTROL) {
    const char *result =
        success ? "ACCEPTED by inverter" : (dev_ctrl_got_response_ ? "REJECTED" : "no confirmation frame");
    ESP_LOGI(TAG, "Power limit %u%% (%s): %s -> %s", power_limit_percent_,
             power_limit_persistent_ ? "persistent" : "non-persistent", tx_result_, result);
    return;
  }
  const uint32_t duration_ms = now - request_started_ms_;
  const uint64_t serial = active_inverter_ == nullptr ? 0 : active_inverter_->get_serial();
  if (success) {
    ESP_LOGI(TAG,
             "Exchange %012llX complete: %.2f MHz tx=%s packets=%u accepted=%u invalid=%u duplicate=%u rssi=%ddBm "
             "duration=%ums",
             serial, radio_.frequency_hz(work_channel_) / 1e6f, tx_result_, rx_packet_count_, frame_count_,
             invalid_frame_count_, duplicate_frame_count_, last_rssi_dbm_, static_cast<unsigned>(duration_ms));
  } else {
    ESP_LOGW(TAG,
             "Exchange %012llX failed: reason=%s %.2f MHz tx=%s attempts=%u packets=%u accepted=%u invalid=%u "
             "duplicate=%u duration=%ums",
             serial, error, radio_.frequency_hz(work_channel_) / 1e6f, tx_result_, tx_attempt_ + 1, rx_packet_count_,
             frame_count_, invalid_frame_count_, duplicate_frame_count_, static_cast<unsigned>(duration_ms));
  }
}

void HoymilesDtuCmt::poll_monitor_(uint32_t now) {
  if (!monitoring_) {
    radio_.set_channel(work_channel_);
    if (!radio_.start_listening()) {
      return;
    }
    monitoring_ = true;
    ESP_LOGD(TAG, "Scan monitor active on %.2f MHz", radio_.frequency_hz(work_channel_) / 1e6f);
    return;
  }
  const uint64_t inverter_radio_id = inverters_.empty() ? 0 : inverters_.front()->get_radio_id();
  while (radio_.packet_available()) {
    uint8_t packet[CMT_MAX_PACKET_LEN];
    const uint8_t len = radio_.read_packet(packet, sizeof(packet));
    if (len == 0) {
      break;
    }
    HmSniffResult sniff;
    if (hm_classify_sniffed_packet(packet, len, inverter_radio_id, dtu_serial_, &sniff)) {
      report_scan_(sniff, radio_.get_channel(), now);
    }
  }
}

void HoymilesDtuCmt::exit_monitor_() {
  radio_.stop_listening();
  monitoring_ = false;
}

void IRAM_ATTR HoymilesDtuCmt::gpio3_handler_(HoymilesDtuCmt *arg) {
  arg->packet_irq_ = true;
  arg->enable_loop_soon_any_context();
}

}  // namespace hoymiles_dtu
}  // namespace esphome
