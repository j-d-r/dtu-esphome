#pragma once

#include "esphome/core/defines.h"

#include "esphome/components/hoymiles_dtu_base/dtu_base.h"
#include "esphome/components/spi/spi.h"
#include "esphome/core/gpio.h"

namespace esphome {
namespace hoymiles_dtu {

enum HmPaLevel : uint8_t {
  PA_MIN = 0,
  PA_LOW = 1,
  PA_HIGH = 2,
  PA_MAX = 3,
};

// Discovery commands the active scanner may transmit, as a bitmask (see scan_inverters). Kept in
// sync with the Python `commands:` option mapping in __init__.py.
static constexpr uint8_t HM_SCAN_CMD_SEARCH_ID = 0x01;
static constexpr uint8_t HM_SCAN_CMD_COLLECT_INFO = 0x02;

// DTU hub for HM-series inverters on an nRF24L01+ radio.
class HoymilesDtuRf24 : public HoymilesDtuBase,
                             public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                   spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  void set_ce_pin(GPIOPin *pin) { ce_pin_ = pin; }
  void set_irq_pin(GPIOPin *pin) {
    irq_pin_base_ = pin;
    if (pin != nullptr && pin->is_internal()) {
      irq_pin_ = static_cast<InternalGPIOPin *>(pin);
    }
  }
  void set_pa_level(HmPaLevel pa_level) { pa_level_ = pa_level; }
  // Enable a single discovery scan fired once, delay_ms after boot (never recurring). commands is a
  // bitmask of HM_SCAN_CMD_*; 0 means both. Configured via the scan_on_boot: hub option.
  void set_scan_on_boot(uint32_t delay_ms, uint32_t duration_ms, uint8_t commands) {
    scan_on_boot_ = true;
    scan_on_boot_delay_ms_ = delay_ms;
    scan_on_boot_duration_ms_ = duration_ms;
    scan_on_boot_commands_ = commands;
  }

  // Start an active over-the-air discovery scan. Broadcasts Search-ID (0x02) and/or collect-info
  // (0x06) requests across the HM channels for duration_ms and reports every inverter that answers.
  // commands is a bitmask of HM_SCAN_CMD_*; 0 means both. User-initiated only; never runs on a timer.
  void scan_inverters(uint32_t duration_ms, uint8_t commands);

  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;

 protected:
  enum class RequestState : uint8_t {
    IDLE,
    WAIT_TX_IRQ,
    RX_ACTIVE,
  };

  bool setup_radio_();
  void configure_radio_();
  void start_request_(HoymilesDtuInverter *inverter, uint32_t now);
  void start_power_limit_request_(uint32_t now);
  void transmit_request_(uint32_t now);
  void begin_exchange_();
  void end_exchange_();
  void consume_irq_();
  static void irq_handler_(HoymilesDtuRf24 *arg);
  void poll_tx_(uint32_t now);
  void poll_rx_(uint32_t now);
  void finish_request_(bool success, const char *error, uint32_t now);
  void log_exchange_summary_(bool success, const char *error, uint32_t now);
  void read_available_packets_();
  // Passive scan detection: while idle, listen for foreign DTU requests to our inverter and for
  // Search-ID broadcasts, then report them. Receive-only — never transmits.
  void poll_monitor_(uint32_t now);
  void enter_monitor_(uint32_t now);
  void exit_monitor_();
  void read_monitor_packets_(uint32_t now);
  // Active scanner (transmit-side discovery). A scan runs as a sequence of probes: each probe sends
  // one discovery command on one HM channel, then listens for replies on the DTU address before
  // advancing. Reuses the same non-blocking TX/RX machinery as telemetry (ExchangeKind::SCAN).
  void start_scan_(uint32_t now);
  void transmit_scan_probe_(uint32_t now);
  void poll_scan_tx_(uint32_t now);
  void begin_scan_rx_window_(uint32_t now);
  void poll_scan_rx_(uint32_t now);
  void advance_scan_probe_(uint32_t now);
  void finish_scan_(uint32_t now);
  void handle_scan_packet_(const uint8_t *packet, uint8_t len, uint32_t now);
  void report_discovered_(const HmDiscoveredInverter &inverter, bool via_search_id, uint32_t now);
  void begin_rx_window_(uint32_t now);
  void start_listening_(uint8_t channel);
  void stop_listening_();
  void set_channel_(uint8_t channel);
  void set_rx_mode_(bool enabled);
  void set_ce_(bool level);
  void clear_status_(uint8_t mask);
  bool tx_payload_(const uint8_t *payload, uint8_t len);
  bool rx_payload_(uint8_t *payload, uint8_t *len);
  uint8_t get_dynamic_payload_size_();
  uint8_t read_register_(uint8_t reg);
  void read_register_(uint8_t reg, uint8_t *data, uint8_t len);
  void write_register_(uint8_t reg, uint8_t value);
  void write_register_(uint8_t reg, const uint8_t *data, uint8_t len);
  uint8_t command_(uint8_t command);
  uint8_t command_(uint8_t command, const uint8_t *tx, uint8_t *rx, uint8_t len);
  static bool plausible_status_(uint8_t status);

  GPIOPin *ce_pin_{nullptr};
  GPIOPin *irq_pin_base_{nullptr};
  InternalGPIOPin *irq_pin_{nullptr};
  HmPaLevel pa_level_{PA_LOW};
  bool scan_on_boot_{false};
  uint32_t scan_on_boot_delay_ms_{45000};
  uint32_t scan_on_boot_duration_ms_{20000};
  uint8_t scan_on_boot_commands_{0};
  bool monitoring_{false};
  uint8_t monitor_channel_index_{0};
  uint32_t monitor_last_hop_ms_{0};
  // Active scanner runtime. A scan is requested by the action (scan_pending_) and then driven probe
  // by probe from loop() until scan_duration_ms_ elapses; discovered serials are deduplicated in
  // scan_seen_serials_ so each inverter is reported once.
  static constexpr uint8_t SCAN_MAX_RESULTS = 32;
  bool scan_pending_{false};
  uint32_t scan_duration_ms_{0};
  uint8_t scan_commands_{0};
  uint32_t scan_started_ms_{0};
  uint8_t scan_probe_channel_index_{0};
  bool scan_probe_collect_info_{false};
  uint32_t scan_rx_started_ms_{0};
  uint8_t scan_rx_channel_index_{0};
  uint32_t scan_rx_last_hop_ms_{0};
  uint8_t scan_found_count_{0};
  uint32_t scan_seen_serials_[SCAN_MAX_RESULTS]{};
  uint8_t detected_status_{0xFF};
  uint8_t detected_config_{0xFF};
  uint8_t detected_setup_aw_{0xFF};
  uint8_t detected_rf_setup_{0xFF};
  RequestState request_state_{RequestState::IDLE};
  uint32_t last_channel_switch_ms_{0};
  uint8_t tx_channel_index_{0};
  uint8_t tx_attempt_{0};      // acquisition burst attempt counter (0-based) within one poll
  uint8_t record_attempt_{0};  // whole-record re-request counter when fragments are missing
  uint8_t rx_channel_index_{0};
  uint8_t current_rx_channel_index_{0};
  bool rx_loop_channels_{false};
  bool rx_pendular_{false};
  uint8_t tx_status_{0};
  const char *tx_result_{"not sent"};
  volatile bool irq_pending_{false};
  bool irq_attached_{false};
  HighFrequencyLoopRequester high_freq_;
};

template <typename... Ts>
class ScanInvertersAction : public Action<Ts...>, public Parented<HoymilesDtuRf24> {
 public:
  TEMPLATABLE_VALUE(uint32_t, duration)
  TEMPLATABLE_VALUE(uint8_t, commands)

  void play(const Ts &...x) override {
    this->parent_->scan_inverters(this->duration_.value(x...), this->commands_.value(x...));
  }
};

}  // namespace hoymiles_dtu
}  // namespace esphome
