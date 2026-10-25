#include "dtu_rf24.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#include <cstdio>
#include <cstring>

namespace esphome::hoymiles_dtu {

static const char *const TAG = "hoymiles_dtu";

static constexpr uint8_t NRF_R_REGISTER = 0x00;
static constexpr uint8_t NRF_W_REGISTER = 0x20;
static constexpr uint8_t NRF_R_RX_PAYLOAD = 0x61;
static constexpr uint8_t NRF_W_TX_PAYLOAD = 0xA0;
static constexpr uint8_t NRF_FLUSH_TX = 0xE1;
static constexpr uint8_t NRF_FLUSH_RX = 0xE2;
static constexpr uint8_t NRF_ACTIVATE = 0x50;
static constexpr uint8_t NRF_R_RX_PL_WID = 0x60;
static constexpr uint8_t NRF_NOP = 0xFF;

static constexpr uint8_t NRF_REG_CONFIG = 0x00;
static constexpr uint8_t NRF_REG_EN_AA = 0x01;
static constexpr uint8_t NRF_REG_EN_RXADDR = 0x02;
static constexpr uint8_t NRF_REG_SETUP_AW = 0x03;
static constexpr uint8_t NRF_REG_SETUP_RETR = 0x04;
static constexpr uint8_t NRF_REG_RF_CH = 0x05;
static constexpr uint8_t NRF_REG_RF_SETUP = 0x06;
static constexpr uint8_t NRF_REG_STATUS = 0x07;
static constexpr uint8_t NRF_REG_RX_ADDR_P0 = 0x0A;
static constexpr uint8_t NRF_REG_RX_ADDR_P1 = 0x0B;
static constexpr uint8_t NRF_REG_TX_ADDR = 0x10;
static constexpr uint8_t NRF_REG_FIFO_STATUS = 0x17;
static constexpr uint8_t NRF_REG_DYNPD = 0x1C;
static constexpr uint8_t NRF_REG_FEATURE = 0x1D;

static constexpr uint8_t NRF_CONFIG_EN_CRC = 0x08;
static constexpr uint8_t NRF_CONFIG_CRCO = 0x04;
static constexpr uint8_t NRF_CONFIG_PWR_UP = 0x02;
static constexpr uint8_t NRF_CONFIG_PRIM_RX = 0x01;
static constexpr uint8_t NRF_STATUS_RX_DR = 0x40;
static constexpr uint8_t NRF_STATUS_TX_DS = 0x20;
static constexpr uint8_t NRF_STATUS_MAX_RT = 0x10;
static constexpr uint8_t NRF_FIFO_RX_EMPTY = 0x01;

static constexpr uint32_t TX_TIMEOUT_MS = 100;
static constexpr uint32_t RX_TIMEOUT_MS = 520;
static constexpr uint32_t RX_INITIAL_PENDULUM_MS = 85;
static constexpr uint32_t RX_CHANNEL_DWELL_MS = 5;
// Acquisition burst: on no-ACK we retransmit immediately on the next channel (non-blocking, one
// attempt per loop iteration) up to this many times before giving up for this poll. ~MAX/5 full
// channel sweeps; with a tight update_interval this approximates Ahoy's steady retransmit cadence.
static constexpr uint8_t MAX_TX_ATTEMPTS = 30;
// On a partial record (link is up, some fragments missing), re-request the whole record this many
// times before failing the exchange. Each re-request keeps already-received fragments.
static constexpr uint8_t MAX_RECORD_ATTEMPTS = 4;
// Passive scan monitor: dwell this long per channel before hopping, so a channel-hopping attacker's
// bursts overlap our listen window.
static constexpr uint32_t MONITOR_CHANNEL_DWELL_MS = 20;
// Active scanner tunables. Each probe transmits one discovery command on one channel, then listens
// SCAN_RX_WINDOW_MS for replies on the DTU address, sweeping the HM channels (the reply channel
// offset for a broadcast discovery is not documented, so we sweep rather than park). Defaults chosen
// so one full round of 5 channels x up-to-2 commands takes ~1s; a scan runs for its whole duration.
static constexpr uint32_t SCAN_RX_WINDOW_MS = 120;
static constexpr uint32_t SCAN_RX_CHANNEL_DWELL_MS = 6;
static constexpr uint32_t SCAN_DEFAULT_DURATION_MS = 20000;

void HoymilesDtuRf24::setup() {
  if (ce_pin_ == nullptr) {
    ESP_LOGE(TAG, "CE pin is not configured");
    mark_failed();
    return;
  }
  ce_pin_->setup();
  ce_pin_->digital_write(false);
  if (irq_pin_ != nullptr) {
    irq_pin_->setup();
    irq_pin_->attach_interrupt(&HoymilesDtuRf24::irq_handler, this, gpio::INTERRUPT_FALLING_EDGE);
    irq_attached_ = true;
  } else if (irq_pin_base_ != nullptr) {
    irq_pin_base_->setup();
    ESP_LOGW(TAG, "IRQ pin is not an internal GPIO; using high-frequency polling fallback during exchanges");
  } else {
    ESP_LOGW(TAG, "IRQ pin is not configured; using high-frequency polling fallback during exchanges");
  }

  spi_setup();
  if (!dtu_serial_configured_) {
    dtu_serial_ = hm_generate_dtu_serial();
  }
  radio_ok_ = setup_radio_();
  if (!radio_ok_) {
    ESP_LOGE(TAG, "nRF24 radio probe failed: STATUS=0x%02X CONFIG=0x%02X SETUP_AW=0x%02X", detected_status_,
             detected_config_, detected_setup_aw_);
    mark_failed();
    return;
  }
  ESP_LOGCONFIG(TAG, "nRF24 radio ready, DTU serial 0x%08X", static_cast<unsigned>(dtu_serial_));

  if (scan_on_boot_) {
    // Fire one bounded discovery scan a short delay after boot (let wifi/time and the first poll
    // settle). set_timeout runs the callback exactly once, so this can never become a recurring
    // scan — the only ways to scan are this boot one-shot, the scan_inverters action, and a button.
    this->set_timeout("scan_on_boot", scan_on_boot_delay_ms_, [this]() {
      ESP_LOGI(TAG, "scan_on_boot: starting one-shot discovery scan");
      this->scan_inverters(scan_on_boot_duration_ms_, scan_on_boot_commands_);
    });
  }
}

void HoymilesDtuRf24::update() {
  if (!radio_ok_) {
    return;
  }
  // Only one exchange can be in flight, so queue every inverter and let loop() poll them back to
  // back. A poll still queued from the previous cycle (radio busy the whole time) simply stays queued.
  queue_poll_cycle_();
}

void HoymilesDtuRf24::loop() {
  if (!radio_ok_) {
    return;
  }
  const uint32_t now = millis();
  expire_offline_inverters_(now);
  consume_irq_();

  switch (request_state_) {
    case RequestState::IDLE:
      if (power_limit_pending_) {
        if (monitoring_) {
          exit_monitor_();
        }
        power_limit_pending_ = false;
        start_power_limit_request_(now);
      } else if (scan_pending_) {
        if (monitoring_) {
          exit_monitor_();
        }
        scan_pending_ = false;
        start_scan_(now);
      } else if (HoymilesDtuInverter *inverter = next_due_inverter_()) {
        start_request_(inverter, now);
      } else if (scan_detection_) {
        poll_monitor_(now);
      }
      break;
    case RequestState::WAIT_TX_IRQ:
      poll_tx_(now);
      break;
    case RequestState::RX_ACTIVE:
      poll_rx_(now);
      break;
  }
}

void HoymilesDtuRf24::dump_config() {
  ESP_LOGCONFIG(TAG, "Hoymiles DTU:");
  LOG_PIN("  CS Pin: ", this->cs_);
  LOG_PIN("  CE Pin: ", this->ce_pin_);
  LOG_PIN("  IRQ Pin: ", this->irq_pin_base_);
  ESP_LOGCONFIG(TAG, "  IRQ mode: %s", irq_attached_ ? "falling-edge interrupt" : "high-frequency polling fallback");
  ESP_LOGCONFIG(TAG, "  Radio: %s", radio_ok_ ? "nRF24 connected" : "not connected");
  ESP_LOGCONFIG(TAG, "  STATUS=0x%02X CONFIG=0x%02X SETUP_AW=0x%02X RF_SETUP=0x%02X", detected_status_,
                detected_config_, detected_setup_aw_, detected_rf_setup_);
  log_dtu_config_();
  // Scan detection is passive/receive-only. Note: HM dynamic-payload reception is coupled to
  // hardware auto-ACK on the nRF24, so the monitor does ACK what it overhears; this leaks nothing
  // beyond what the (already unauthenticated) protocol exposes, but it is not perfectly stealthy.
  ESP_LOGCONFIG(TAG, "  Scan detection: %s",
                scan_detection_ ? "enabled (passive; listens on inverter + Search-ID address)" : "disabled");
  // The active scanner is available on-demand via the scan_inverters action (it transmits discovery
  // requests, so it never runs on a timer). Reported here only as an availability note.
  if (scan_on_boot_) {
    ESP_LOGCONFIG(TAG, "  Active scan: on-demand via scan_inverters action; one-shot on boot (+%ums, %ums)",
                  static_cast<unsigned>(scan_on_boot_delay_ms_), static_cast<unsigned>(scan_on_boot_duration_ms_));
  } else {
    ESP_LOGCONFIG(TAG, "  Active scan: on-demand via scan_inverters action");
  }
}

void HoymilesDtuRf24::scan_inverters(uint32_t duration_ms, uint8_t commands) {
  if (!radio_ok_) {
    publish_radio_error_("radio not ready");
    return;
  }
  if (scan_pending_ || exchange_kind_ == ExchangeKind::SCAN) {
    ESP_LOGW(TAG, "Scan already requested or running; ignoring");
    return;
  }
  scan_duration_ms_ = duration_ms == 0 ? SCAN_DEFAULT_DURATION_MS : duration_ms;
  scan_commands_ = commands == 0 ? (HM_SCAN_CMD_SEARCH_ID | HM_SCAN_CMD_COLLECT_INFO) : commands;
  scan_pending_ = true;
}

bool HoymilesDtuRf24::setup_radio_() {
  delay(20);
  detected_status_ = command_(NRF_NOP);
  detected_config_ = read_register_(NRF_REG_CONFIG);
  detected_setup_aw_ = read_register_(NRF_REG_SETUP_AW);
  const uint8_t restore_setup_aw = detected_setup_aw_;
  write_register_(NRF_REG_SETUP_AW, 0x03);
  detected_setup_aw_ = read_register_(NRF_REG_SETUP_AW);
  write_register_(NRF_REG_SETUP_AW, restore_setup_aw);

  if (!plausible_status(detected_status_) || detected_setup_aw_ != 0x03) {
    return false;
  }
  configure_radio_();
  detected_config_ = read_register_(NRF_REG_CONFIG);
  detected_rf_setup_ = read_register_(NRF_REG_RF_SETUP);
  return true;
}

void HoymilesDtuRf24::configure_radio_() {
  set_ce_(false);
  write_register_(NRF_REG_CONFIG, NRF_CONFIG_EN_CRC | NRF_CONFIG_CRCO | NRF_CONFIG_PWR_UP);
  write_register_(NRF_REG_EN_AA, 0x3F);
  write_register_(NRF_REG_EN_RXADDR, 0x03);
  write_register_(NRF_REG_SETUP_AW, 0x03);
  write_register_(NRF_REG_SETUP_RETR, 0x3F);
  write_register_(NRF_REG_RF_SETUP, 0x20 | (static_cast<uint8_t>(pa_level_) << 1));
  write_register_(NRF_REG_DYNPD, 0x3F);
  write_register_(NRF_REG_FEATURE, 0x04);
  if ((read_register_(NRF_REG_FEATURE) & 0x04) == 0) {
    const uint8_t activate = 0x73;
    command_(NRF_ACTIVATE, &activate, nullptr, 1);
    write_register_(NRF_REG_FEATURE, 0x04);
    write_register_(NRF_REG_DYNPD, 0x3F);
  }
  command_(NRF_FLUSH_RX);
  command_(NRF_FLUSH_TX);
  clear_status_(NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
  delay(5);
}

void HoymilesDtuRf24::begin_exchange_() { high_freq_.start(); }

void HoymilesDtuRf24::end_exchange_() { high_freq_.stop(); }

void HoymilesDtuRf24::consume_irq_() {
  // The nRF IRQ only wakes the loop promptly; poll_tx_/poll_rx_ read STATUS directly, so there is no
  // per-IRQ work to do here beyond clearing the ISR flag.
  irq_pending_ = false;
}

void IRAM_ATTR HoymilesDtuRf24::irq_handler(HoymilesDtuRf24 *arg) {
  arg->irq_pending_ = true;
  arg->enable_loop_soon_any_context();
}

void HoymilesDtuRf24::start_request_(HoymilesDtuInverter *inverter, uint32_t now) {
  if (monitoring_) {
    exit_monitor_();
  }
  begin_exchange_state_(ExchangeKind::TELEMETRY, inverter);
  inverter->mark_poll_started();
  record_attempt_ = 0;
  tx_status_ = 0;
  tx_result_ = "no ack";
  irq_pending_ = false;
  tx_attempt_ = 0;
  begin_exchange_();

  // The inverter TX/ACK address is stable for the whole exchange; program it once.
  uint8_t inverter_address[5];
  hm_radio_id_to_address(inverter->get_radio_id(), inverter_address);
  write_register_(NRF_REG_TX_ADDR, inverter_address, sizeof(inverter_address));
  write_register_(NRF_REG_RX_ADDR_P0, inverter_address, sizeof(inverter_address));

  if (!build_telemetry_request_(inverter, now)) {
    finish_request_(false, "request build failed", now);
    return;
  }

  transmit_request_(now);
}

void HoymilesDtuRf24::start_power_limit_request_(uint32_t now) {
  HoymilesDtuInverter *inverter = inverters_.front();
  begin_exchange_state_(ExchangeKind::DEV_CONTROL, inverter);
  record_attempt_ = 0;
  tx_status_ = 0;
  tx_result_ = "no ack";
  irq_pending_ = false;
  tx_attempt_ = 0;
  begin_exchange_();

  uint8_t inverter_address[5];
  hm_radio_id_to_address(inverter->get_radio_id(), inverter_address);
  write_register_(NRF_REG_TX_ADDR, inverter_address, sizeof(inverter_address));
  write_register_(NRF_REG_RX_ADDR_P0, inverter_address, sizeof(inverter_address));

  if (!build_power_limit_request_(inverter)) {
    finish_request_(false, "power-limit build failed", now);
    return;
  }

  transmit_request_(now);
}

// Build and transmit one acquisition request. Called for the first attempt and for each
// burst retransmit. Mirrors Ahoy's acquisition: advance the TX channel each attempt (cold-start
// equivalent of its per-channel quality heuristic), transmit with auto-ack, and wait for the IRQ.
// The request payload is built once per exchange (start_request_/start_power_limit_request_) and
// reused for every retransmit in the burst, same as Ahoy keeps one payload for a whole burst.
void HoymilesDtuRf24::transmit_request_(uint32_t now) {
  if (active_inverter_ == nullptr) {
    finish_request_(false, "no inverter", now);
    return;
  }

  // Round-robin {3,23,40,61,75}. RX reply lands rxOffset=3 indices away (HM).
  tx_channel_index_ = (tx_channel_index_ + 1) % 5;
  rx_channel_index_ = (tx_channel_index_ + 3) % 5;

  request_started_ms_ = now;
  stop_listening_();
  set_channel_(HM_RF_CHANNELS[tx_channel_index_]);

#ifdef ESPHOME_LOG_HAS_DEBUG
  if (tx_attempt_ == 0) {
    uint8_t inverter_address[5];
    hm_radio_id_to_address(active_inverter_->get_radio_id(), inverter_address);
    ESP_LOGD(TAG, "TX request inverter=%012llX ch=%u addr=%s len=%u data=%s", active_inverter_->get_serial(),
             HM_RF_CHANNELS[tx_channel_index_], format_hex_pretty(inverter_address, sizeof(inverter_address)).c_str(),
             request_len_, format_hex_pretty(request_, request_len_).c_str());
  }
#endif

  tx_result_ = "no ack";
  if (tx_payload_(request_, request_len_)) {
    request_state_ = RequestState::WAIT_TX_IRQ;
  } else {
    begin_rx_window_(now);  // can't wait for a TX IRQ; fall back to listening
  }
}

void HoymilesDtuRf24::poll_tx_(uint32_t now) {
  if (exchange_kind_ == ExchangeKind::SCAN) {
    poll_scan_tx_(now);
    return;
  }
  const uint8_t status = read_register_(NRF_REG_STATUS);
  if ((status & NRF_STATUS_TX_DS) != 0) {
    // ACK from the inverter: it received the request and will stream data frames. Listen now.
    set_ce_(false);
    tx_status_ = status;
    tx_result_ = "TX_DS";
    clear_status_(NRF_STATUS_TX_DS);
    ESP_LOGD(TAG, "TX_DS ch=%u attempt=%u", HM_RF_CHANNELS[tx_channel_index_], tx_attempt_);
    begin_rx_window_(now);
    return;
  }

  const bool max_rt = (status & NRF_STATUS_MAX_RT) != 0;
  const bool timed_out = now - request_started_ms_ > TX_TIMEOUT_MS;
  if (max_rt || timed_out) {
    // No ACK on this channel. Ahoy-style acquisition: immediately retransmit on the next channel
    // rather than waiting a whole poll interval, so we sweep the inverter's listen window fast.
    set_ce_(false);
    tx_status_ = status;
    tx_result_ = max_rt ? "MAX_RT" : "timeout";
    clear_status_(NRF_STATUS_MAX_RT);
    command_(NRF_FLUSH_TX);
    if (++tx_attempt_ < MAX_TX_ATTEMPTS) {
      transmit_request_(now);
    } else {
      finish_request_(false, "no ack", now);
    }
  }
}

void HoymilesDtuRf24::poll_rx_(uint32_t now) {
  if (exchange_kind_ == ExchangeKind::SCAN) {
    poll_scan_rx_(now);
    return;
  }
  read_available_packets_();

  if (exchange_kind_ == ExchangeKind::DEV_CONTROL) {
    // Single confirmation frame (0xD1): no re-request, no channel hopping — just wait out the
    // window on the parked channel (begin_rx_window_ already put us on tx_channel+3).
    if (dev_ctrl_confirmed_) {
      finish_request_(true, "", now);
      return;
    }
    if (now - rx_started_ms_ >= RX_TIMEOUT_MS) {
      finish_request_(false, dev_ctrl_got_response_ ? "power-limit rejected" : "no confirmation", now);
    }
    return;
  }

  if (process_response_(now)) {
    finish_request_(true, "", now);
    return;
  }
  if (now - rx_started_ms_ >= RX_TIMEOUT_MS) {
    // Partial record: the inverter is answering but we missed fragment(s). Re-request the whole
    // record (keeping fragments we already have — read_available_packets_ dedups by id) up to a
    // small budget, instead of giving up. Mirrors Ahoy's missing-fragment retransmit.
    if (frame_count_ > 0 && frame_count_ < expected_frames_ && ++record_attempt_ <= MAX_RECORD_ATTEMPTS) {
      ESP_LOGD(TAG, "Incomplete record %u/%u - re-requesting (attempt %u)", frame_count_, expected_frames_,
               record_attempt_);
      tx_attempt_ = 0;  // give the re-request a fresh acquisition burst
      transmit_request_(now);
      return;
    }
    finish_request_(false, frame_count_ == 0 ? "no response" : "incomplete response", now);
    return;
  }

  if (now - last_channel_switch_ms_ < RX_CHANNEL_DWELL_MS) {
    return;
  }

  if (!rx_loop_channels_ && now - rx_started_ms_ > RX_INITIAL_PENDULUM_MS) {
    rx_loop_channels_ = true;
  }

  if (rx_loop_channels_) {
    current_rx_channel_index_ = (current_rx_channel_index_ + 4) % 5;
  } else {
    rx_pendular_ = !rx_pendular_;
    current_rx_channel_index_ = (rx_channel_index_ + (rx_pendular_ ? 4 : 0)) % 5;
  }
  set_channel_(HM_RF_CHANNELS[current_rx_channel_index_]);
  last_channel_switch_ms_ = now;
}

void HoymilesDtuRf24::finish_request_(bool success, const char *error, uint32_t now) {
  log_exchange_summary_(success, error, now);
  if (exchange_kind_ == ExchangeKind::DEV_CONTROL) {
    if (!success) {
      publish_radio_error_(error);
    }
  } else if (!success && active_inverter_ != nullptr) {
    active_inverter_->set_last_error(error);
  }
  stop_listening_();
  active_inverter_ = nullptr;
  request_state_ = RequestState::IDLE;
  end_exchange_();
}

void HoymilesDtuRf24::log_exchange_summary_(bool success, const char *error, uint32_t now) {
  const uint8_t tx_channel = HM_RF_CHANNELS[tx_channel_index_];
  if (exchange_kind_ == ExchangeKind::DEV_CONTROL) {
    const char *result =
        success ? "ACCEPTED by inverter" : (dev_ctrl_got_response_ ? "REJECTED" : "no confirmation frame");
    ESP_LOGI(TAG, "Power limit %u%% (%s): TX_DS on ch=%u -> %s", power_limit_percent_,
             power_limit_persistent_ ? "persistent" : "non-persistent", tx_channel, result);
    return;
  }

  const uint32_t duration_ms = now - request_started_ms_;
  const uint8_t rx_channel = HM_RF_CHANNELS[rx_channel_index_];
  const char *result = success ? "complete" : error;
  const uint64_t serial = active_inverter_ == nullptr ? 0 : active_inverter_->get_serial();
  if (success) {
    ESP_LOGI(TAG,
             "Exchange %012llX complete: tx_ch=%u tx=%s rx_start=%u packets=%u accepted=%u invalid=%u "
             "duplicate=%u duration=%ums",
             serial, tx_channel, tx_result_, rx_channel, rx_packet_count_, frame_count_, invalid_frame_count_,
             duplicate_frame_count_, static_cast<unsigned>(duration_ms));
  } else {
    ESP_LOGW(TAG,
             "Exchange %012llX failed: reason=%s tx_ch=%u tx=%s status=0x%02X rx_start=%u packets=%u accepted=%u "
             "invalid=%u duplicate=%u duration=%ums",
             serial, result, tx_channel, tx_result_, tx_status_, rx_channel, rx_packet_count_, frame_count_,
             invalid_frame_count_, duplicate_frame_count_, static_cast<unsigned>(duration_ms));
  }
}

void HoymilesDtuRf24::read_available_packets_() {
  uint8_t status = read_register_(NRF_REG_STATUS);
  if ((status & NRF_STATUS_RX_DR) == 0 && (read_register_(NRF_REG_FIFO_STATUS) & NRF_FIFO_RX_EMPTY) != 0) {
    return;
  }
  while ((read_register_(NRF_REG_FIFO_STATUS) & NRF_FIFO_RX_EMPTY) == 0) {
    uint8_t packet[32];
    uint8_t len = 0;
    if (!rx_payload_(packet, &len)) {
      break;
    }
    ESP_LOGD(TAG, "RX payload ch=%u len=%u data=%s", read_register_(NRF_REG_RF_CH), len,
             format_hex_pretty(packet, len).c_str());
    ingest_packet_(packet, len);
  }
  clear_status_(NRF_STATUS_RX_DR);
}

void HoymilesDtuRf24::begin_rx_window_(uint32_t now) {
  rx_started_ms_ = now;
  last_channel_switch_ms_ = now;
  current_rx_channel_index_ = rx_channel_index_;
  rx_loop_channels_ = false;
  rx_pendular_ = false;
  start_listening_(HM_RF_CHANNELS[current_rx_channel_index_]);
  request_state_ = RequestState::RX_ACTIVE;
}

void HoymilesDtuRf24::start_listening_(uint8_t channel) {
  command_(NRF_FLUSH_RX);
  clear_status_(NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
  set_channel_(channel);
  uint8_t dtu_address[5];
  hm_radio_id_to_address(hm_radio_id_from_serial(dtu_serial_), dtu_address);
  write_register_(NRF_REG_RX_ADDR_P1, dtu_address, sizeof(dtu_address));
  set_rx_mode_(true);
  set_ce_(true);
  delayMicroseconds(130);
}

void HoymilesDtuRf24::stop_listening_() {
  set_ce_(false);
  set_rx_mode_(false);
  delayMicroseconds(130);
}

// Passive scan monitor. Runs only while the exchange state machine is IDLE. It listens on our
// inverter's address (pipe 0) and the HM global Search-ID address (pipe 1), hopping the HM channels,
// and reports any foreign DTU request it overhears. It never transmits a request; the radio's own
// auto-ACK is the only emission (see dump_config note). Reuses the exchange RX config (2-byte HW
// CRC, dynamic payload) so packets decode the same way telemetry does.
void HoymilesDtuRf24::poll_monitor_(uint32_t now) {
  if (!monitoring_) {
    enter_monitor_(now);
    return;
  }
  read_monitor_packets_(now);
  if (now - monitor_last_hop_ms_ >= MONITOR_CHANNEL_DWELL_MS) {
    monitor_channel_index_ = (monitor_channel_index_ + 1) % 5;
    set_channel_(HM_RF_CHANNELS[monitor_channel_index_]);
    monitor_last_hop_ms_ = now;
  }
}

void HoymilesDtuRf24::enter_monitor_(uint32_t now) {
  command_(NRF_FLUSH_RX);
  clear_status_(NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
  if (!inverters_.empty()) {
    uint8_t inverter_address[5];
    hm_radio_id_to_address(inverters_.front()->get_radio_id(), inverter_address);
    write_register_(NRF_REG_RX_ADDR_P0, inverter_address, sizeof(inverter_address));
  }
  write_register_(NRF_REG_RX_ADDR_P1, HM_SEARCH_ID_ADDRESS, sizeof(HM_SEARCH_ID_ADDRESS));
  monitor_channel_index_ = 0;
  set_channel_(HM_RF_CHANNELS[monitor_channel_index_]);
  set_rx_mode_(true);
  set_ce_(true);
  delayMicroseconds(130);
  monitor_last_hop_ms_ = now;
  monitoring_ = true;
  ESP_LOGD(TAG, "Scan monitor active (inverter + Search-ID address, hopping HM channels)");
}

void HoymilesDtuRf24::exit_monitor_() {
  // Mirror stop_listening_. The next exchange reprograms pipe 0/1 and the channel, so nothing else
  // needs restoring here.
  set_ce_(false);
  set_rx_mode_(false);
  delayMicroseconds(130);
  monitoring_ = false;
}

void HoymilesDtuRf24::read_monitor_packets_(uint32_t now) {
  const uint8_t status = read_register_(NRF_REG_STATUS);
  if ((status & NRF_STATUS_RX_DR) == 0 && (read_register_(NRF_REG_FIFO_STATUS) & NRF_FIFO_RX_EMPTY) != 0) {
    return;
  }
  const uint64_t inverter_radio_id = inverters_.empty() ? 0 : inverters_.front()->get_radio_id();
  while ((read_register_(NRF_REG_FIFO_STATUS) & NRF_FIFO_RX_EMPTY) == 0) {
    uint8_t packet[32];
    uint8_t len = 0;
    if (!rx_payload_(packet, &len)) {
      break;
    }
    HmSniffResult sniff;
    if (hm_classify_sniffed_packet(packet, len, inverter_radio_id, dtu_serial_, &sniff)) {
      report_scan_(sniff, HM_RF_CHANNELS[monitor_channel_index_], now);
    }
  }
  clear_status_(NRF_STATUS_RX_DR);
}

// Active scanner. Broadcasts the CCC report's discovery requests (Search-ID 0x02 / collect-info 0x06)
// to the HM global address and reports every inverter that answers. Unlike the passive monitor this
// transmits, so it is user-initiated only (the scan_inverters action), disabled by default, and
// bounded in duration. It only reads serials — it sends no on/off, power-limit, or firmware command.
void HoymilesDtuRf24::start_scan_(uint32_t now) {
  exchange_kind_ = ExchangeKind::SCAN;
  scan_started_ms_ = now;
  scan_found_count_ = 0;
  scan_probe_channel_index_ = 0;
  // Start with Search-ID when it is enabled, otherwise the other command; advance_scan_probe_ then
  // alternates only between the enabled commands.
  scan_probe_collect_info_ = (scan_commands_ & HM_SCAN_CMD_SEARCH_ID) == 0;
  begin_exchange_();
  // The TX address and the pipe-0 (auto-ACK) address are the HM global Search-ID address for the
  // whole scan; only the channel and payload change per probe. Replies come back on pipe 1 (the DTU
  // address), programmed later by start_listening_.
  write_register_(NRF_REG_TX_ADDR, HM_SEARCH_ID_ADDRESS, sizeof(HM_SEARCH_ID_ADDRESS));
  write_register_(NRF_REG_RX_ADDR_P0, HM_SEARCH_ID_ADDRESS, sizeof(HM_SEARCH_ID_ADDRESS));
  ESP_LOGI(TAG, "Scan started: duration=%ums commands=%s%s dtu=0x%08X", static_cast<unsigned>(scan_duration_ms_),
           (scan_commands_ & HM_SCAN_CMD_SEARCH_ID) ? "search-id " : "",
           (scan_commands_ & HM_SCAN_CMD_COLLECT_INFO) ? "collect-info" : "", static_cast<unsigned>(dtu_serial_));
  transmit_scan_probe_(now);
}

void HoymilesDtuRf24::transmit_scan_probe_(uint32_t now) {
  request_started_ms_ = now;
  stop_listening_();
  set_channel_(HM_RF_CHANNELS[scan_probe_channel_index_]);
  if (scan_probe_collect_info_) {
    request_len_ = hm_build_collect_info_request(dtu_serial_, request_, sizeof(request_));
  } else {
    request_len_ = hm_build_search_id_request(dtu_serial_, request_, sizeof(request_));
  }
  if (request_len_ == 0) {
    finish_scan_(now);
    return;
  }
  ESP_LOGD(TAG, "Scan probe: %s ch=%u data=%s", scan_probe_collect_info_ ? "collect-info" : "search-id",
           HM_RF_CHANNELS[scan_probe_channel_index_], format_hex_pretty(request_, request_len_).c_str());
  if (tx_payload_(request_, request_len_)) {
    request_state_ = RequestState::WAIT_TX_IRQ;
  } else {
    begin_scan_rx_window_(now);
  }
}

void HoymilesDtuRf24::poll_scan_tx_(uint32_t now) {
  const uint8_t status = read_register_(NRF_REG_STATUS);
  const bool tx_ds = (status & NRF_STATUS_TX_DS) != 0;
  const bool max_rt = (status & NRF_STATUS_MAX_RT) != 0;
  // A TX_DS here means an inverter in range auto-ACKed the discovery request (report footnote 17).
  // Whether or not one did, listen for a reply: the serial comes back as a separate packet, and ACK
  // timing on a broadcast is not something to rely on.
  if (tx_ds || max_rt || now - request_started_ms_ > TX_TIMEOUT_MS) {
    set_ce_(false);
    // A TX_DS on a broadcast probe means an inverter in range ACKed it (report footnote 17) and a
    // serial reply should follow; MAX_RT/timeout means nobody answered on this channel. Logged so a
    // bench run can tell "the inverter hears the broadcast" from "it ignores it".
    ESP_LOGD(TAG, "Scan probe TX %s ch=%u", tx_ds ? "ACK(TX_DS)" : (max_rt ? "MAX_RT" : "timeout"),
             HM_RF_CHANNELS[scan_probe_channel_index_]);
    clear_status_(NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
    command_(NRF_FLUSH_TX);
    begin_scan_rx_window_(now);
  }
}

void HoymilesDtuRf24::begin_scan_rx_window_(uint32_t now) {
  scan_rx_started_ms_ = now;
  scan_rx_last_hop_ms_ = now;
  // HM replies land rxOffset=+3 channels from the request channel — the same offset telemetry and
  // devcontrol rely on. Start there so a single fast discovery reply is caught immediately, then keep
  // sweeping (the reply channel for a broadcast discovery is not separately documented).
  scan_rx_channel_index_ = (scan_probe_channel_index_ + 3) % 5;
  start_listening_(HM_RF_CHANNELS[scan_rx_channel_index_]);  // programs pipe 1 = DTU address
  request_state_ = RequestState::RX_ACTIVE;
}

void HoymilesDtuRf24::poll_scan_rx_(uint32_t now) {
  while ((read_register_(NRF_REG_FIFO_STATUS) & NRF_FIFO_RX_EMPTY) == 0) {
    uint8_t packet[32];
    uint8_t len = 0;
    if (!rx_payload_(packet, &len)) {
      break;
    }
    // Log every raw reply during a scan (even ones that do not parse) so a bench test can tell "the
    // inverter answered but we misparsed" from "nothing came back at all".
    ESP_LOGD(TAG, "Scan RX ch=%u len=%u data=%s", HM_RF_CHANNELS[scan_rx_channel_index_], len,
             format_hex_pretty(packet, len).c_str());
    handle_scan_packet_(packet, len, now);
  }
  clear_status_(NRF_STATUS_RX_DR);

  if (now - scan_rx_started_ms_ >= SCAN_RX_WINDOW_MS) {
    advance_scan_probe_(now);
    return;
  }
  if (now - scan_rx_last_hop_ms_ >= SCAN_RX_CHANNEL_DWELL_MS) {
    scan_rx_channel_index_ = (scan_rx_channel_index_ + 1) % 5;
    set_channel_(HM_RF_CHANNELS[scan_rx_channel_index_]);
    scan_rx_last_hop_ms_ = now;
  }
}

void HoymilesDtuRf24::handle_scan_packet_(const uint8_t *packet, uint8_t len, uint32_t now) {
  HmDiscoveredInverter inverter;
  if (hm_parse_search_id_response(packet, len, &inverter)) {
    // The reply echoes a serial at [5..8]. Some firmwares echo the DTU serial we transmitted (the
    // CCC report's HMS-600 capture); the live HM-1200 echoes its own serial there instead. Accept
    // either — the 0x82 opcode and the trailing CRC8 (both verified by hm_parse_search_id_response)
    // already authenticate the reply as a genuine answer to our scan. Only drop a reply whose echo
    // is neither our DTU serial nor the inverter's own serial, which would mean it was not ours.
    if (dtu_serial_ == 0 || inverter.responder_dtu_serial == dtu_serial_ ||
        inverter.responder_dtu_serial == inverter.serial_suffix) {
      report_discovered_(inverter, true, now);
    }
    return;
  }
  if (hm_parse_collect_info_response(packet, len, &inverter)) {
    report_discovered_(inverter, false, now);
  }
}

void HoymilesDtuRf24::report_discovered_(const HmDiscoveredInverter &inverter, bool via_search_id, uint32_t now) {
  for (uint8_t i = 0; i < scan_found_count_; i++) {
    if (scan_seen_serials_[i] == inverter.serial_suffix) {
      return;  // already reported this scan
    }
  }
  if (scan_found_count_ >= SCAN_MAX_RESULTS) {
    return;
  }
  scan_seen_serials_[scan_found_count_++] = inverter.serial_suffix;
  const uint8_t channel = HM_RF_CHANNELS[scan_probe_channel_index_];
  char buffer[96];
  if (via_search_id) {
    snprintf(buffer, sizeof(buffer), "serial ...%08X | PID 0x%04X | via search-id | ch %u | #%u",
             static_cast<unsigned>(inverter.serial_suffix), inverter.pid, channel, scan_found_count_);
  } else {
    snprintf(buffer, sizeof(buffer), "serial ...%08X | PID - | via collect-info | ch %u | #%u",
             static_cast<unsigned>(inverter.serial_suffix), channel, scan_found_count_);
  }
  ESP_LOGI(TAG, "Scan found: %s", buffer);
  if (scan_result_text_sensor_ != nullptr) {
    scan_result_text_sensor_->publish_state(buffer);
  }
  if (scan_found_count_sensor_ != nullptr) {
    scan_found_count_sensor_->publish_state(static_cast<float>(scan_found_count_));
  }
}

void HoymilesDtuRf24::advance_scan_probe_(uint32_t now) {
  const bool has_search_id = (scan_commands_ & HM_SCAN_CMD_SEARCH_ID) != 0;
  const bool has_collect_info = (scan_commands_ & HM_SCAN_CMD_COLLECT_INFO) != 0;
  if (has_search_id && has_collect_info) {
    scan_probe_collect_info_ = !scan_probe_collect_info_;
    if (!scan_probe_collect_info_) {  // completed both commands on this channel
      scan_probe_channel_index_ = (scan_probe_channel_index_ + 1) % 5;
    }
  } else {
    scan_probe_channel_index_ = (scan_probe_channel_index_ + 1) % 5;
  }
  if (now - scan_started_ms_ >= scan_duration_ms_) {
    finish_scan_(now);
    return;
  }
  transmit_scan_probe_(now);
}

void HoymilesDtuRf24::finish_scan_(uint32_t now) {
  stop_listening_();
  ESP_LOGI(TAG, "Scan complete: %u inverter(s) found in %ums", scan_found_count_,
           static_cast<unsigned>(now - scan_started_ms_));
  if (scan_found_count_sensor_ != nullptr) {
    scan_found_count_sensor_->publish_state(static_cast<float>(scan_found_count_));
  }
  if (scan_result_text_sensor_ != nullptr && scan_found_count_ == 0) {
    scan_result_text_sensor_->publish_state("no inverters found");
  }
  exchange_kind_ = ExchangeKind::TELEMETRY;
  request_state_ = RequestState::IDLE;
  end_exchange_();
}

void HoymilesDtuRf24::set_channel_(uint8_t channel) { write_register_(NRF_REG_RF_CH, channel); }

void HoymilesDtuRf24::set_rx_mode_(bool enabled) {
  uint8_t config = NRF_CONFIG_EN_CRC | NRF_CONFIG_CRCO | NRF_CONFIG_PWR_UP;
  if (enabled) {
    config |= NRF_CONFIG_PRIM_RX;
  }
  write_register_(NRF_REG_CONFIG, config);
}

void HoymilesDtuRf24::set_ce_(bool level) {
  if (ce_pin_ != nullptr) {
    ce_pin_->digital_write(level);
  }
}

void HoymilesDtuRf24::clear_status_(uint8_t mask) { write_register_(NRF_REG_STATUS, mask); }

bool HoymilesDtuRf24::tx_payload_(const uint8_t *payload, uint8_t len) {
  if (payload == nullptr || len == 0 || len > 32) {
    return false;
  }
  command_(NRF_FLUSH_TX);
  clear_status_(NRF_STATUS_RX_DR | NRF_STATUS_TX_DS | NRF_STATUS_MAX_RT);
  set_rx_mode_(false);
  command_(NRF_W_TX_PAYLOAD, payload, nullptr, len);
  ESP_LOGD(TAG, "TX loaded fifo=0x%02X status=0x%02X config=0x%02X", read_register_(NRF_REG_FIFO_STATUS),
           read_register_(NRF_REG_STATUS), read_register_(NRF_REG_CONFIG));
  set_ce_(true);
  return true;
}

bool HoymilesDtuRf24::rx_payload_(uint8_t *payload, uint8_t *len) {
  const uint8_t width = get_dynamic_payload_size_();
  if (width == 0 || width > 32) {
    command_(NRF_FLUSH_RX);
    return false;
  }
  memset(payload, 0, 32);
  command_(NRF_R_RX_PAYLOAD, nullptr, payload, width);
  *len = width;
  return true;
}

uint8_t HoymilesDtuRf24::get_dynamic_payload_size_() {
  uint8_t width = 0;
  command_(NRF_R_RX_PL_WID, nullptr, &width, 1);
  return width;
}

uint8_t HoymilesDtuRf24::read_register_(uint8_t reg) {
  uint8_t value = 0xFF;
  read_register_(reg, &value, 1);
  return value;
}

void HoymilesDtuRf24::read_register_(uint8_t reg, uint8_t *data, uint8_t len) {
  command_(NRF_R_REGISTER | (reg & 0x1F), nullptr, data, len);
}

void HoymilesDtuRf24::write_register_(uint8_t reg, uint8_t value) { write_register_(reg, &value, 1); }

void HoymilesDtuRf24::write_register_(uint8_t reg, const uint8_t *data, uint8_t len) {
  command_(NRF_W_REGISTER | (reg & 0x1F), data, nullptr, len);
}

uint8_t HoymilesDtuRf24::command_(uint8_t command) { return command_(command, nullptr, nullptr, 0); }

uint8_t HoymilesDtuRf24::command_(uint8_t command, const uint8_t *tx, uint8_t *rx, uint8_t len) {
  enable();
  const uint8_t status = transfer_byte(command);
  for (uint8_t i = 0; i < len; i++) {
    const uint8_t tx_byte = tx == nullptr ? NRF_NOP : tx[i];
    const uint8_t rx_byte = transfer_byte(tx_byte);
    if (rx != nullptr) {
      rx[i] = rx_byte;
    }
  }
  disable();
  return status;
}

bool HoymilesDtuRf24::plausible_status(uint8_t status) {
  return status != 0x00 && status != 0xFF && (status & 0x80) == 0;
}

}  // namespace esphome::hoymiles_dtu
