import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID

from .const import (
    CONF_INVERTERS,
    CONF_MODEL,
    CONF_PERCENT,
    CONF_PERSISTENT,
    CONF_SCAN_DETECTION,
    CONF_SERIAL,
)
from .protocol import parse_serial_bcd

# Radio-independent half of the Hoymiles DTU: inverter bookkeeping, request building,
# fragment assembly, decoding, publishing and the power-limit queue. It has no YAML
# block of its own - the radio component (hoymiles_dtu_rf24 for HM) auto-loads it
# and builds its hub on top of HoymilesDtuBase. The `sensor` and `text_sensor`
# platforms live here, so they do not depend on the radio.
CODEOWNERS = ["@swifty99"]
AUTO_LOAD = ["sensor", "text_sensor"]

hoymiles_dtu_ns = cg.esphome_ns.namespace("hoymiles_dtu")
HoymilesDtuBase = hoymiles_dtu_ns.class_("HoymilesDtuBase", cg.PollingComponent)
HoymilesDtuInverter = hoymiles_dtu_ns.class_("HoymilesDtuInverter")
HmModel = hoymiles_dtu_ns.enum("HmModel")
RadioSetPowerLimitAction = hoymiles_dtu_ns.class_(
    "RadioSetPowerLimitAction", automation.Action
)

# Keyed by DC-input count: 1-channel HM-300/350/400, 2-channel HM-600/700/800,
# 4-channel HM-1000/1200/1500 - the same grouping Ahoy uses.
MODEL_OPTIONS = {
    "hm_300": HmModel.HM_300,
    "hm_350": HmModel.HM_350,
    "hm_400": HmModel.HM_400,
    "hm_600": HmModel.HM_600,
    "hm_700": HmModel.HM_700,
    "hm_800": HmModel.HM_800,
    "hm_1000": HmModel.HM_1000,
    "hm_1200": HmModel.HM_1200,
    "hm_1500": HmModel.HM_1500,
}


def validate_serial(value):
    # Hoymiles serials are packed BCD: the printed digits map directly to the
    # radio-address bytes (exactly like Ahoy, which parses them as base-16).
    return parse_serial_bcd(value)


INVERTER_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HoymilesDtuInverter),
        cv.Required(CONF_SERIAL): validate_serial,
        cv.Required(CONF_MODEL): cv.enum(MODEL_OPTIONS, lower=True),
    }
)

BASE_SCHEMA = cv.Schema(
    {
        # Passive, receive-only detection of nearby scanning/probing.
        cv.Optional(CONF_SCAN_DETECTION, default=False): cv.boolean,
        cv.Required(CONF_INVERTERS): cv.All(
            cv.ensure_list(INVERTER_SCHEMA), cv.Length(min=1)
        ),
    }
).extend(cv.polling_component_schema("15s"))


async def register_hub(var, config):
    """Code shared by radio hubs: component registration and the inverter list."""
    await cg.register_component(var, config)
    cg.add(var.set_scan_detection(config[CONF_SCAN_DETECTION]))
    for inverter_config in config[CONF_INVERTERS]:
        inverter = cg.new_Pvariable(inverter_config[CONF_ID])
        cg.add(inverter.set_serial(inverter_config[CONF_SERIAL]))
        cg.add(inverter.set_model(inverter_config[CONF_MODEL]))
        cg.add(var.add_inverter(inverter))


RADIO_SET_POWER_LIMIT_ACTION_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(HoymilesDtuBase),
        cv.Optional(CONF_PERCENT, default=100): cv.templatable(
            cv.int_range(min=0, max=100)
        ),
        cv.Optional(CONF_PERSISTENT, default=True): cv.templatable(cv.boolean),
    }
)


@automation.register_action(
    "hoymiles_dtu_base.radio_set_power_limit",
    RadioSetPowerLimitAction,
    RADIO_SET_POWER_LIMIT_ACTION_SCHEMA,
    synchronous=True,
)
async def radio_set_power_limit_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    percent = await cg.templatable(config[CONF_PERCENT], args, cg.uint16)
    persistent = await cg.templatable(config[CONF_PERSISTENT], args, cg.bool_)
    cg.add(var.set_percent(percent))
    cg.add(var.set_persistent(persistent))
    return var
