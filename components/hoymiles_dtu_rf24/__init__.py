import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.components import spi
from esphome.components.hoymiles_dtu_base import (
    BASE_SCHEMA,
    NRF24,
    HoymilesDtuBase,
    hoymiles_dtu_ns,
    register_hub,
    validate_inverter_models,
)
from esphome.const import CONF_DELAY, CONF_ID

# DTU hub for HM-series inverters on an nRF24L01+ (2.4 GHz): an ESPHome SPI device plus
# CE and an optional IRQ line. Everything radio-independent is in hoymiles_dtu_base.
CODEOWNERS = ["@swifty99"]
DEPENDENCIES = ["spi"]
AUTO_LOAD = ["hoymiles_dtu_base"]

CONF_CE_PIN = "ce_pin"
CONF_COMMANDS = "commands"
CONF_DURATION = "duration"
CONF_IRQ_PIN = "irq_pin"
CONF_PA_LEVEL = "pa_level"
CONF_SCAN_ON_BOOT = "scan_on_boot"

HoymilesDtuRf24 = hoymiles_dtu_ns.class_(
    "HoymilesDtuRf24", HoymilesDtuBase, spi.SPIDevice
)
HmPaLevel = hoymiles_dtu_ns.enum("HmPaLevel")
ScanInvertersAction = hoymiles_dtu_ns.class_("ScanInvertersAction", automation.Action)

# Discovery-command bitmask, kept in sync with HM_SCAN_CMD_* in dtu_rf24.h.
SCAN_COMMAND_BITS = {
    "search_id": 0x01,
    "collect_info": 0x02,
}

# `commands:` list shared by the scan_inverters action and scan_on_boot (>=1 opcode).
SCAN_COMMANDS_SCHEMA = cv.All(
    cv.ensure_list(cv.one_of(*SCAN_COMMAND_BITS, lower=True)), cv.Length(min=1)
)

# A single bounded discovery scan fired once, a short delay after boot. It is
# deliberately a one-shot, never a recurring timer: the scanner transmits, so it only
# runs on an explicit trigger (this boot one-shot, the scan_inverters action, or a
# button). `scan_on_boot: true` enables it with defaults; a mapping overrides the
# delay/duration/commands.
SCAN_ON_BOOT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_DELAY, default="45s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_DURATION, default="20s"): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_COMMANDS, default=["search_id", "collect_info"]
        ): SCAN_COMMANDS_SCHEMA,
    }
)


def _scan_on_boot(value):
    # Accept a bare boolean (enable/disable with defaults) or a full mapping. Normalise
    # to either None (disabled) or a validated dict so to_code has one shape to handle.
    if isinstance(value, dict):
        return SCAN_ON_BOOT_SCHEMA(value)
    return SCAN_ON_BOOT_SCHEMA({}) if cv.boolean(value) else None


PA_LEVEL_OPTIONS = {
    "min": HmPaLevel.PA_MIN,
    "low": HmPaLevel.PA_LOW,
    "high": HmPaLevel.PA_HIGH,
    "max": HmPaLevel.PA_MAX,
}

# nRF24L01+ radio (HM models): ESPHome SPI bus plus CE and an optional IRQ line.
_HUB_SCHEMA = BASE_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(HoymilesDtuRf24),
        cv.Required(CONF_CE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_IRQ_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_PA_LEVEL, default="low"): cv.enum(
            PA_LEVEL_OPTIONS, lower=True
        ),
        # One-shot discovery scan on boot (never recurring). See _scan_on_boot.
        cv.Optional(CONF_SCAN_ON_BOOT, default=False): _scan_on_boot,
    }
).extend(
    spi.spi_device_schema(
        cs_pin_required=True, default_data_rate="1MHz", default_mode="MODE0"
    )
)

CONFIG_SCHEMA = cv.All(
    _HUB_SCHEMA,
    validate_inverter_models(NRF24),
)

FINAL_VALIDATE_SCHEMA = spi.final_validate_device_schema(
    "hoymiles_dtu_rf24", require_miso=True, require_mosi=True
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await register_hub(var, config)
    await spi.register_spi_device(var, config)

    ce_pin = await cg.gpio_pin_expression(config[CONF_CE_PIN])
    cg.add(var.set_ce_pin(ce_pin))
    if irq_pin_config := config.get(CONF_IRQ_PIN):
        irq_pin = await cg.gpio_pin_expression(irq_pin_config)
        cg.add(var.set_irq_pin(irq_pin))
    cg.add(var.set_pa_level(config[CONF_PA_LEVEL]))

    if (scan_boot := config[CONF_SCAN_ON_BOOT]) is not None:
        bitmask = 0
        for name in scan_boot[CONF_COMMANDS]:
            bitmask |= SCAN_COMMAND_BITS[name]
        cg.add(
            var.set_scan_on_boot(
                scan_boot[CONF_DELAY].total_milliseconds,
                scan_boot[CONF_DURATION].total_milliseconds,
                bitmask,
            )
        )


# Active over-the-air discovery scan. Transmits, so it only runs on an explicit
# one-shot trigger (this action, a button, or scan_on_boot) - never a recurring timer.
# nRF24 (HM) only; the CMT2300A hub has no scanner. See docs/scanner-spec.md.
SCAN_INVERTERS_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(HoymilesDtuRf24),
        cv.Optional(CONF_DURATION, default="20s"): cv.positive_time_period_milliseconds,
        cv.Optional(
            CONF_COMMANDS, default=["search_id", "collect_info"]
        ): SCAN_COMMANDS_SCHEMA,
    }
)


@automation.register_action(
    "hoymiles_dtu_rf24.scan_inverters",
    ScanInvertersAction,
    SCAN_INVERTERS_ACTION_SCHEMA,
    # play() only queues the scan for the radio state machine and returns, so
    # play_next_() always runs before play_complex() returns.
    synchronous=True,
)
async def scan_inverters_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    duration = await cg.templatable(
        config[CONF_DURATION].total_milliseconds, args, cg.uint32
    )
    cg.add(var.set_duration(duration))
    bitmask = 0
    for name in config[CONF_COMMANDS]:
        bitmask |= SCAN_COMMAND_BITS[name]
    commands = await cg.templatable(bitmask, args, cg.uint8)
    cg.add(var.set_commands(commands))
    return var
