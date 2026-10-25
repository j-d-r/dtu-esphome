#pragma once

#include "cmt2300a.h"
#include "esphome/components/hoymiles_dtu_base/dtu_base.h"
#include "esphome/core/gpio.h"

namespace esphome {
namespace hoymiles_dtu {

// DTU hub for HMS-series inverters, which use a CMT2300A sub-GHz transceiver instead of the nRF24
// the HM series uses. The application protocol is the same (same request framing, same fragment
// assembly, same CRCs), so everything above the radio lives in HoymilesDtuBase. What differs:
//
//   * one fixed frequency instead of hopping across five 2.4 GHz channels,
//   * no hardware auto-ACK or address filter: a transmission is simply sent, and every received
//     packet has to be matched against our DTU serial in software,
//   * an inverter that has lost its DTU listens on a band-specific boot frequency until it is told
//     to move, so an unreachable inverter is first sent a channel-change command (opcode 0x56).
class HoymilesDtuCmt : public HoymilesDtuBase {
 public:
  void set_radio_pins(GPIOPin *sclk, GPIOPin *sdio, GPIOPin *csb, GPIOPin *fcsb) {
    radio_.set_pins(sclk, sdio, csb, fcsb);
    sclk_pin_ = sclk;
    sdio_pin_ = sdio;
    csb_pin_ = csb;
    fcsb_pin_ = fcsb;
  }
#ifdef USE_ESP32
  void set_spi_host(spi_host_device_t host, bool fallback) { radio_.set_spi_host(host, fallback); }
#endif
  // Optional CMT2300A GPIO3 (INT2 = packet received). Only wakes the loop earlier; reception works
  // without it because the loop polls the chip's interrupt flags anyway.
  void set_gpio3_pin(GPIOPin *pin) {
    gpio3_pin_base_ = pin;
    if (pin != nullptr && pin->is_internal()) {
      gpio3_pin_ = static_cast<InternalGPIOPin *>(pin);
    }
  }
  void set_region(HmCmtRegion region) {
    region_ = region;
    radio_.set_region(region);
  }
  // Working frequency in Hz (a multiple of 250 kHz inside the region's band). 0 keeps the region
  // default, which is the frequency Hoymiles' own DTUs use.
  void set_frequency(uint32_t frequency_hz) { frequency_hz_ = frequency_hz; }
  void set_tx_power(int8_t dbm) { radio_.set_tx_power_dbm(dbm); }

  void setup() override;
  void update() override;
  void loop() override;
  void dump_config() override;

 protected:
  enum class RequestState : uint8_t {
    IDLE,
    TX_WAIT,
    RX_ACTIVE,
  };

  // What the in-flight transmission is. A poll of an inverter that is currently offline starts with
  // CHANNEL_CHANGE (sent on the boot frequency) and continues with REQUEST once that is on air.
  enum class TxStep : uint8_t {
    CHANNEL_CHANGE,
    REQUEST,
  };

  void start_request_(HoymilesDtuInverter *inverter, uint32_t now);
  void start_power_limit_request_(uint32_t now);
  void transmit_channel_change_(uint32_t now);
  void transmit_request_(uint32_t now);
  void poll_tx_(uint32_t now);
  void begin_rx_window_(uint32_t now);
  void poll_rx_(uint32_t now);
  void read_available_packets_(uint32_t now);
  void finish_request_(bool success, const char *error, uint32_t now);
  void log_exchange_summary_(bool success, const char *error, uint32_t now);
  // Passive, receive-only detection of foreign DTU traffic while idle. The CMT2300A has no address
  // filter, so this simply classifies everything that decodes on our working frequency.
  void poll_monitor_(uint32_t now);
  void exit_monitor_();
  static void gpio3_handler_(HoymilesDtuCmt *arg);

  Cmt2300a radio_;
  GPIOPin *sclk_pin_{nullptr};
  GPIOPin *sdio_pin_{nullptr};
  GPIOPin *csb_pin_{nullptr};
  GPIOPin *fcsb_pin_{nullptr};
  GPIOPin *gpio3_pin_base_{nullptr};
  InternalGPIOPin *gpio3_pin_{nullptr};
  bool gpio3_attached_{false};
  volatile bool packet_irq_{false};
  HmCmtRegion region_{HM_REGION_EU};
  uint32_t frequency_hz_{0};
  uint8_t work_channel_{0};
  uint8_t boot_channel_{0};
  RequestState request_state_{RequestState::IDLE};
  TxStep tx_step_{TxStep::REQUEST};
  bool monitoring_{false};
  uint8_t tx_attempt_{0};
  uint8_t record_attempt_{0};
  int8_t last_rssi_dbm_{0};
  bool got_rssi_{false};
  const char *tx_result_{"not sent"};
  HighFrequencyLoopRequester high_freq_;
};

}  // namespace hoymiles_dtu
}  // namespace esphome
