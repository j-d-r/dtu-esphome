#pragma once

#include "esphome/components/hoymiles_dtu_base/protocol.h"
#include "esphome/core/defines.h"
#include "esphome/core/gpio.h"

#include <cstdint>

#ifdef USE_ESP32
#include "driver/spi_master.h"
#endif

namespace esphome {
namespace hoymiles_dtu {

// --- CMT2300A registers -------------------------------------------------------------------------
// Register map and bit masks per the CMT2300A datasheet and CMOSTEK's reference driver
// (cmt2300a_defs.h, as shipped with OpenDTU). Only the registers this driver touches are listed.
static constexpr uint8_t CMT_REG_CMT10 = 0x09;      // xosc_aac_code[2:0]
static constexpr uint8_t CMT_REG_SYS2 = 0x0D;       // LFOSC calibration enables
static constexpr uint8_t CMT_REG_CMT4 = 0x03;       // bit0 doubles the TX power setting
static constexpr uint8_t CMT_REG_PKT15 = 0x46;      // payload length [7:0]
static constexpr uint8_t CMT_REG_PKT17 = 0x48;      // node id [7:0] - used as the presence self-test
static constexpr uint8_t CMT_REG_TX8 = 0x5C;        // TX power word, high byte
static constexpr uint8_t CMT_REG_TX9 = 0x5D;        // TX power word, low byte
static constexpr uint8_t CMT_REG_MODE_CTL = 0x60;   // go_* state requests
static constexpr uint8_t CMT_REG_MODE_STA = 0x61;   // chip state + CFG_RETAIN / RSTN_IN_EN
static constexpr uint8_t CMT_REG_EN_CTL = 0x62;     // LOCKING_EN
static constexpr uint8_t CMT_REG_FREQ_CHNL = 0x63;  // frequency channel index
static constexpr uint8_t CMT_REG_FREQ_OFS = 0x64;   // channel step, in 2.5 kHz units
static constexpr uint8_t CMT_REG_IO_SEL = 0x65;     // GPIO1..4 function select
static constexpr uint8_t CMT_REG_INT1_CTL = 0x66;   // INT1 source + interrupt polarity
static constexpr uint8_t CMT_REG_INT2_CTL = 0x67;   // INT2 source
static constexpr uint8_t CMT_REG_INT_EN = 0x68;     // interrupt enables
static constexpr uint8_t CMT_REG_FIFO_CTL = 0x69;
static constexpr uint8_t CMT_REG_INT_CLR1 = 0x6A;
static constexpr uint8_t CMT_REG_INT_CLR2 = 0x6B;
static constexpr uint8_t CMT_REG_FIFO_CLR = 0x6C;
static constexpr uint8_t CMT_REG_INT_FLAG = 0x6D;
static constexpr uint8_t CMT_REG_FIFO_FLAG = 0x6E;
static constexpr uint8_t CMT_REG_RSSI_DBM = 0x70;
static constexpr uint8_t CMT_REG_SOFT_RESET = 0x7F;

static constexpr uint8_t CMT_GO_STBY = 0x02;
static constexpr uint8_t CMT_GO_RX = 0x08;
static constexpr uint8_t CMT_GO_SLEEP = 0x10;
static constexpr uint8_t CMT_GO_TX = 0x40;

static constexpr uint8_t CMT_STA_SLEEP = 0x01;
static constexpr uint8_t CMT_STA_STBY = 0x02;
static constexpr uint8_t CMT_STA_RX = 0x05;
static constexpr uint8_t CMT_STA_TX = 0x06;
static constexpr uint8_t CMT_MASK_CHIP_MODE_STA = 0x0F;

static constexpr uint8_t CMT_MASK_RSTN_IN_EN = 0x20;
static constexpr uint8_t CMT_MASK_CFG_RETAIN = 0x10;
static constexpr uint8_t CMT_MASK_LOCKING_EN = 0x20;
static constexpr uint8_t CMT_MASK_INT_POLAR = 0x20;
static constexpr uint8_t CMT_MASK_INT1_SEL = 0x1F;
static constexpr uint8_t CMT_MASK_INT2_SEL = 0x1F;
static constexpr uint8_t CMT_INT_SEL_PKT_OK = 0x07;
static constexpr uint8_t CMT_INT_SEL_TX_DONE = 0x0A;
static constexpr uint8_t CMT_GPIO2_SEL_INT1 = 0x00;
static constexpr uint8_t CMT_GPIO3_SEL_INT2 = 0x20;

// INT_EN / INT_CLR1 bits
static constexpr uint8_t CMT_MASK_TX_DONE_EN = 0x20;
static constexpr uint8_t CMT_MASK_PREAM_OK_EN = 0x10;
static constexpr uint8_t CMT_MASK_SYNC_OK_EN = 0x08;
static constexpr uint8_t CMT_MASK_CRC_OK_EN = 0x02;
static constexpr uint8_t CMT_MASK_PKT_DONE_EN = 0x01;
static constexpr uint8_t CMT_MASK_SL_TMO_FLG = 0x20;
static constexpr uint8_t CMT_MASK_RX_TMO_FLG = 0x10;
static constexpr uint8_t CMT_MASK_TX_DONE_FLG = 0x08;
static constexpr uint8_t CMT_MASK_TX_DONE_CLR = 0x04;
static constexpr uint8_t CMT_MASK_SL_TMO_CLR = 0x02;
static constexpr uint8_t CMT_MASK_RX_TMO_CLR = 0x01;

// INT_FLAG bits / INT_CLR2 clear bits
static constexpr uint8_t CMT_MASK_LBD_FLG = 0x80;
static constexpr uint8_t CMT_MASK_COL_ERR_FLG = 0x40;
static constexpr uint8_t CMT_MASK_PKT_ERR_FLG = 0x20;
static constexpr uint8_t CMT_MASK_PREAM_OK_FLG = 0x10;
static constexpr uint8_t CMT_MASK_SYNC_OK_FLG = 0x08;
static constexpr uint8_t CMT_MASK_NODE_OK_FLG = 0x04;
static constexpr uint8_t CMT_MASK_CRC_OK_FLG = 0x02;
static constexpr uint8_t CMT_MASK_PKT_OK_FLG = 0x01;
static constexpr uint8_t CMT_MASK_LBD_CLR = 0x20;
static constexpr uint8_t CMT_MASK_PREAM_OK_CLR = 0x10;
static constexpr uint8_t CMT_MASK_SYNC_OK_CLR = 0x08;
static constexpr uint8_t CMT_MASK_NODE_OK_CLR = 0x04;
static constexpr uint8_t CMT_MASK_CRC_OK_CLR = 0x02;
static constexpr uint8_t CMT_MASK_PKT_DONE_CLR = 0x01;

// FIFO_CTL / FIFO_CLR / FIFO_FLAG bits
static constexpr uint8_t CMT_MASK_FIFO_RX_TX_SEL = 0x04;
static constexpr uint8_t CMT_MASK_FIFO_MERGE_EN = 0x02;
static constexpr uint8_t CMT_MASK_SPI_FIFO_RD_WR_SEL = 0x01;
static constexpr uint8_t CMT_MASK_FIFO_CLR_RX = 0x02;
static constexpr uint8_t CMT_MASK_FIFO_CLR_TX = 0x01;
static constexpr uint8_t CMT_MASK_TX_FIFO_NMTY_FLG = 0x02;

// A complete HM/HMS packet fits in 32 bytes; the merged FIFO holds 64.
static constexpr uint8_t CMT_MAX_PACKET_LEN = 32;
// "Receive OK": preamble, sync word, CRC and packet all valid. Reading the FIFO on anything weaker
// yields partial or corrupt frames (Ahoy Cmt2300a::getRx uses the same gate).
static constexpr uint8_t CMT_MASK_RX_OK =
    CMT_MASK_PREAM_OK_FLG | CMT_MASK_SYNC_OK_FLG | CMT_MASK_CRC_OK_FLG | CMT_MASK_PKT_OK_FLG;

// CMT2300A transceiver used by HMS inverters, driven over the chip's 3-wire SPI (shared SDIO line,
// one chip select for registers and a second one for the FIFO).
//
// On ESP32 the transport is normally a hardware SPI host in half-duplex 3-wire mode, as in Ahoy and
// OpenDTU: two devices on one bus, one using CSB and one using FCSB as its hardware chip select, with
// the peripheral turning SDIO around for the data phase of a read. The host is one ESPHome's own
// `spi:` buses do not use (assigned at config validation). Where no host is free - or on non-ESP32
// targets - the same frames are bit-banged on plain GPIOs. Either way frame timing on air is
// unaffected, because the radio itself does the modulation.
//
// Register values, the configuration banks and the state-machine sequences follow CMOSTEK's
// reference driver plus the RFPDK-generated 860/900 MHz parameter sets that ship with OpenDTU
// (identical to the ones Ahoy carries), which is what Hoymiles' own DTUs use on air.
class Cmt2300a {
 public:
  void set_pins(GPIOPin *sclk, GPIOPin *sdio, GPIOPin *csb, GPIOPin *fcsb) {
    sclk_ = sclk;
    sdio_ = sdio;
    csb_ = csb;
    fcsb_ = fcsb;
  }
#ifdef USE_ESP32
  // Use this hardware SPI host instead of bit-banging. With `fallback` a host that cannot be claimed at
  // runtime (bus init fails) degrades to bit-banging instead of failing setup().
  void set_spi_host(spi_host_device_t host, bool fallback) {
    spi_host_ = host;
    use_hardware_spi_ = true;
    hardware_spi_fallback_ = fallback;
  }
#endif
  // True when setup() brought the transport up on a hardware SPI host.
  bool using_hardware_spi() const { return hardware_spi_active_; }
  void set_region(HmCmtRegion region) { region_ = region; }
  void set_tx_power_dbm(int8_t dbm) { tx_power_dbm_ = dbm; }

  // Reset, probe and fully configure the chip, leaving it in SLEEP. Returns false when the chip does
  // not answer or refuses a state transition.
  bool setup();
  // Register write/read self-test (writes 0xAA to the unused node-id register and reads it back).
  bool chip_present();

  void set_channel(uint8_t channel);
  uint8_t get_channel() const { return channel_; }
  bool set_tx_power();  // applies set_tx_power_dbm(); false when the value has no table entry

  // Frequency helpers. The channel index counts HM_CMT_CHANNEL_STEP_HZ steps up from the band's
  // base frequency; `work`/`boot` are the frequencies a Hoymiles DTU and a freshly powered HMS
  // inverter respectively use by default, and `legal_min`/`legal_max` bound the region's band.
  static uint32_t base_frequency_hz(HmCmtRegion region);
  static uint32_t work_frequency_hz(HmCmtRegion region);
  static uint32_t boot_frequency_hz(HmCmtRegion region);
  static uint32_t legal_min_frequency_hz(HmCmtRegion region);
  static uint32_t legal_max_frequency_hz(HmCmtRegion region);
  static uint8_t channel_from_frequency(HmCmtRegion region, uint32_t frequency_hz);  // 0xFF on error
  uint32_t frequency_hz(uint8_t channel) const {
    return base_frequency_hz(region_) + static_cast<uint32_t>(channel) * HM_CMT_CHANNEL_STEP_HZ;
  }

  bool start_listening();
  bool stop_listening();
  bool in_rx_mode() const { return rx_mode_; }
  // True once a complete, CRC-valid packet sits in the FIFO.
  bool packet_available();
  // Read one packet (length-prefixed in the FIFO) and clear the receive flags. Returns its length,
  // or 0 when the FIFO held nothing usable.
  uint8_t read_packet(uint8_t *buffer, uint8_t max_len);
  int8_t rssi_dbm() { return static_cast<int8_t>(static_cast<int>(read_register(CMT_REG_RSSI_DBM)) - 128); }

  // Load the FIFO and start transmitting. Completion is polled with transmit_done(), so a TX never
  // blocks the ESPHome loop for the ~30 ms a packet takes on air.
  bool begin_transmit(const uint8_t *payload, uint8_t len);
  bool transmit_done() { return (read_register(CMT_REG_INT_CLR1) & CMT_MASK_TX_DONE_FLG) != 0; }
  // Leave TX: clear the flags and park in SLEEP (the caller normally calls start_listening() next).
  void end_transmit();

  void clear_rx_fifo();
  uint8_t read_register(uint8_t addr);
  void write_register(uint8_t addr, uint8_t value);
  uint8_t chip_status() { return read_register(CMT_REG_MODE_STA) & CMT_MASK_CHIP_MODE_STA; }

 protected:
  bool switch_state_(uint8_t go_command, uint8_t wait_state);
  void configure_banks_();
  void config_interrupt_(uint8_t int1_sel, uint8_t int2_sel);
  uint8_t clear_interrupt_flags_();
  void enable_read_fifo_();
  void enable_write_fifo_();
  void clear_tx_fifo_();

  // 3-wire SPI transport: hardware SPI host when active, otherwise bit-banged.
  bool setup_transport_();
  void setup_bitbang_();
#ifdef USE_ESP32
  bool setup_hardware_spi_();
#endif
  void sdio_output_();
  void sdio_input_();
  void send_bits_(uint16_t value, uint8_t bits);
  uint8_t receive_bits_();
  void write_fifo_(const uint8_t *data, uint8_t len);
  void read_fifo_(uint8_t *data, uint8_t len);

  GPIOPin *sclk_{nullptr};
  GPIOPin *sdio_{nullptr};
  GPIOPin *csb_{nullptr};
  GPIOPin *fcsb_{nullptr};
  HmCmtRegion region_{HM_REGION_EU};
  int8_t tx_power_dbm_{13};
  uint8_t channel_{0};
#ifdef USE_ESP32
  spi_host_device_t spi_host_{};
  spi_device_handle_t spi_register_{nullptr};
  spi_device_handle_t spi_fifo_{nullptr};
#endif
  bool use_hardware_spi_{false};
  bool hardware_spi_fallback_{false};
  bool hardware_spi_active_{false};
  bool sdio_is_output_{true};
  bool rx_mode_{false};
};

}  // namespace hoymiles_dtu
}  // namespace esphome
