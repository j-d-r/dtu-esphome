import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import pins
from esphome.components import spi
from esphome.components.hoymiles_dtu_base import (
    BASE_SCHEMA,
    CMT,
    HoymilesDtuBase,
    hoymiles_dtu_ns,
    register_hub,
    validate_inverter_models,
)
from esphome.const import (
    CONF_FREQUENCY,
    CONF_ID,
    CONF_INVERTED,
    CONF_TX_POWER,
)
from esphome.core import CORE

# DTU hub for HMS-series inverters on a CMT2300A sub-GHz transceiver. Everything
# radio-independent is in hoymiles_dtu_base.
CODEOWNERS = ["@swifty99"]
AUTO_LOAD = ["hoymiles_dtu_base"]

# 3-wire SPI pins, the register/FIFO chip selects, and the optional packet-received
# interrupt line.
CONF_CSB_PIN = "csb_pin"
CONF_FCSB_PIN = "fcsb_pin"
CONF_GPIO3_PIN = "gpio3_pin"
CONF_INTERFACE = "interface"
CONF_REGION = "region"
CONF_SCLK_PIN = "sclk_pin"
CONF_SDIO_PIN = "sdio_pin"

HoymilesDtuCmt = hoymiles_dtu_ns.class_(
    "HoymilesDtuCmt", HoymilesDtuBase
)
HmCmtRegion = hoymiles_dtu_ns.enum("HmCmtRegion")

# Sub-GHz band per region, with the legal band edges and the frequency Hoymiles'
# own DTUs use by default (OpenDTU HoymilesRadio_CMT countryDefinition).
REGION_OPTIONS = {
    "eu": HmCmtRegion.HM_REGION_EU,
    "us": HmCmtRegion.HM_REGION_US,
    "brazil": HmCmtRegion.HM_REGION_BRAZIL,
}

REGION_BANDS = {
    "eu": (863_000_000, 870_000_000),
    "us": (905_000_000, 925_000_000),
    "brazil": (915_000_000, 928_000_000),
}

CMT_CHANNEL_STEP_HZ = 250_000


# CMT2300A radio (HMS models). The chip's 3-wire SPI (one shared SDIO line, one chip
# select for registers and another for the FIFO) is something ESPHome's `spi:` component
# cannot express, so the hub drives it itself: on ESP32 through a hardware SPI host in
# half-duplex 3-wire mode that no `spi:` bus uses, otherwise bit-banged on plain GPIOs.
# `interface:` follows the `spi:` component's vocabulary - `any` (default) takes a free
# host and falls back to bit-banging, `hardware` or a host name insist on one,
# `software` always bit-bangs.
CMT_INTERFACE_ANY = "any"
CMT_INTERFACE_HARDWARE = "hardware"
CMT_INTERFACE_SOFTWARE = "software"
_HUB_SCHEMA = BASE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(HoymilesDtuCmt),
        cv.Required(CONF_SCLK_PIN): pins.internal_gpio_output_pin_schema,
        # SDIO carries data in both directions; the driver flips its mode per transfer.
        cv.Required(CONF_SDIO_PIN): pins.internal_gpio_output_pin_schema,
        cv.Required(CONF_CSB_PIN): pins.internal_gpio_output_pin_schema,
        cv.Required(CONF_FCSB_PIN): pins.internal_gpio_output_pin_schema,
        cv.Optional(
            CONF_INTERFACE, default=CMT_INTERFACE_ANY
        ): spi.one_of_interface_validator(
            [CMT_INTERFACE_ANY, CMT_INTERFACE_HARDWARE, CMT_INTERFACE_SOFTWARE]
        ),
        # CMT2300A GPIO3 (INT2, packet received). Optional: the driver polls the chip's
        # interrupt flags anyway, the pin only shortens the wake-up latency.
        cv.Optional(CONF_GPIO3_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_REGION, default="eu"): cv.enum(REGION_OPTIONS, lower=True),
        cv.Optional(CONF_FREQUENCY): cv.frequency,
        cv.Optional(CONF_TX_POWER, default=13): cv.int_range(min=-10, max=20),
    }
)


def _validate_frequency(config):
    # A CMT2300A channel is an integral number of 250 kHz steps above the band base,
    # and the inverter only listens inside its region's band.
    if CONF_FREQUENCY not in config:
        return config
    frequency = int(config[CONF_FREQUENCY])
    region = config[CONF_REGION]
    low, high = REGION_BANDS[region]
    if frequency % CMT_CHANNEL_STEP_HZ != 0:
        raise cv.Invalid(
            f"frequency must be a multiple of {CMT_CHANNEL_STEP_HZ // 1000} kHz",
            path=[CONF_FREQUENCY],
        )
    if not low <= frequency <= high:
        raise cv.Invalid(
            f"frequency must be within the {region} band "
            f"({low / 1e6:.0f}-{high / 1e6:.0f} MHz)",
            path=[CONF_FREQUENCY],
        )
    return config


CONFIG_SCHEMA = cv.All(
    _HUB_SCHEMA,
    _validate_frequency,
    validate_inverter_models(CMT),
)


def _cmt_spi_host_index(config, full_config):
    """Hardware SPI host index for the CMT2300A transport, or None to bit-bang.

    `spi:` buses get their hosts assigned while the `spi` config is validated, so by
    now `interface_index` on each of them says which hosts are taken.
    """
    interface = config[CONF_INTERFACE]
    if interface == CMT_INTERFACE_SOFTWARE:
        return None
    path = [CONF_INTERFACE]
    if not CORE.is_esp32:
        if interface == CMT_INTERFACE_ANY:
            return None
        raise cv.Invalid("hardware 3-wire SPI is only available on ESP32", path=path)
    # The peripheral drives the pins through the GPIO matrix without inversion.
    pin_keys = (CONF_SCLK_PIN, CONF_SDIO_PIN, CONF_CSB_PIN, CONF_FCSB_PIN)
    if any(config[key][CONF_INVERTED] for key in pin_keys):
        if interface == CMT_INTERFACE_ANY:
            return None
        raise cv.Invalid(
            "hardware SPI cannot drive inverted sclk/sdio/csb/fcsb pins", path=path
        )
    taken = {
        bus[spi.CONF_INTERFACE_INDEX]
        for bus in full_config.get("spi", [])
        if spi.CONF_INTERFACE_INDEX in bus
    }
    free = [
        index
        for index in range(len(spi.get_hw_interface_list()))
        if index not in taken
    ]
    if interface == CMT_INTERFACE_ANY:
        return free[0] if free else None
    if interface == CMT_INTERFACE_HARDWARE:
        if not free:
            raise cv.Invalid(
                "no hardware SPI host left for the CMT2300A: every one is used by "
                "a `spi:` bus (use interface: software, or move a `spi:` bus to "
                "software)",
                path=path,
            )
        return free[0]
    index = spi.get_spi_index(interface)
    if index in taken:
        raise cv.Invalid(
            f"SPI host '{interface}' is already used by a `spi:` bus", path=path
        )
    return index


def _final_validate(config):
    _cmt_spi_host_index(config, fv.full_config.get())
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await register_hub(var, config)

    sclk_pin = await cg.gpio_pin_expression(config[CONF_SCLK_PIN])
    sdio_pin = await cg.gpio_pin_expression(config[CONF_SDIO_PIN])
    csb_pin = await cg.gpio_pin_expression(config[CONF_CSB_PIN])
    fcsb_pin = await cg.gpio_pin_expression(config[CONF_FCSB_PIN])
    cg.add(var.set_radio_pins(sclk_pin, sdio_pin, csb_pin, fcsb_pin))
    if (index := _cmt_spi_host_index(config, CORE.config)) is not None:
        # Only `any` may degrade to bit-banging when the host cannot be claimed at
        # runtime.
        fallback = config[CONF_INTERFACE] == CMT_INTERFACE_ANY
        cg.add(
            var.set_spi_host(
                cg.RawExpression(spi.get_spi_interface(index)), fallback
            )
        )
    if gpio3_pin_config := config.get(CONF_GPIO3_PIN):
        gpio3_pin = await cg.gpio_pin_expression(gpio3_pin_config)
        cg.add(var.set_gpio3_pin(gpio3_pin))
    cg.add(var.set_region(config[CONF_REGION]))
    if (frequency := config.get(CONF_FREQUENCY)) is not None:
        cg.add(var.set_frequency(int(frequency)))
    cg.add(var.set_tx_power(config[CONF_TX_POWER]))
