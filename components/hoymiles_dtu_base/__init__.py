import logging

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

_LOGGER = logging.getLogger(__name__)

# Radio-independent half of the Hoymiles DTU: inverter bookkeeping, request building,
# fragment assembly, decoding, publishing and the power-limit queue. It has no YAML
# block of its own - the radio components (hoymiles_dtu_rf24 for HM, hoymiles_dtu_cmt
# for HMS) auto-load it and build their hub on top of HoymilesDtuBase. The `sensor`
# and `text_sensor` platforms live here, so they work with either radio.
CODEOWNERS = ["@swifty99"]
AUTO_LOAD = ["sensor", "text_sensor"]

hoymiles_dtu_ns = cg.esphome_ns.namespace("hoymiles_dtu")
HoymilesDtuBase = hoymiles_dtu_ns.class_("HoymilesDtuBase", cg.PollingComponent)
HoymilesDtuInverter = hoymiles_dtu_ns.class_("HoymilesDtuInverter")
HmModel = hoymiles_dtu_ns.enum("HmModel")
RadioSetPowerLimitAction = hoymiles_dtu_ns.class_(
    "RadioSetPowerLimitAction", automation.Action
)

# HM families (nRF24), keyed by DC-input count: 1-channel HM-300/350/400, 2-channel
# HM-600/700/800, 4-channel HM-1000/1200/1500 - the same grouping Ahoy uses.
HM_MODEL_OPTIONS = {
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

# HMS families (CMT2300A). Each entry maps to its record layout; MODEL_SERIAL_PREFIXES
# below lists the serial prefixes the family is sold with, because pairing the wrong
# model with a serial decodes to plausible-looking nonsense.
HMS_MODEL_OPTIONS = {
    "hms_300": HmModel.HMS_300,
    "hms_350": HmModel.HMS_350,
    "hms_400": HmModel.HMS_400,
    "hms_450": HmModel.HMS_450,
    "hms_500": HmModel.HMS_500,
    "hms_450_v2": HmModel.HMS_450_V2,
    "hms_500_v2": HmModel.HMS_500_V2,
    "hms_600": HmModel.HMS_600,
    "hms_700": HmModel.HMS_700,
    "hms_800": HmModel.HMS_800,
    "hms_900": HmModel.HMS_900,
    "hms_1000": HmModel.HMS_1000,
    "hms_1600": HmModel.HMS_1600,
    "hms_1800": HmModel.HMS_1800,
    "hms_2000": HmModel.HMS_2000,
}

MODEL_OPTIONS = {**HM_MODEL_OPTIONS, **HMS_MODEL_OPTIONS}

# Printed serial prefixes each model family ships with (OpenDTU HMS_*::isValidSerial and
# Ahoy hmSystem.h). Used for a warning-level sanity check on the configured pair.
MODEL_SERIAL_PREFIXES = {
    "hm_300": (0x1121, 0x1122),
    "hm_350": (0x1121, 0x1122),
    "hm_400": (0x1121, 0x1122),
    "hm_600": (0x1141, 0x1142),
    "hm_700": (0x1141, 0x1142),
    "hm_800": (0x1141, 0x1142),
    "hm_1000": (0x1161, 0x1162),
    "hm_1200": (0x1161, 0x1162),
    "hm_1500": (0x1161, 0x1162),
    "hms_300": (0x1124,),
    "hms_350": (0x1124,),
    "hms_400": (0x1124,),
    "hms_450": (0x1124,),
    "hms_500": (0x1124,),
    "hms_450_v2": (0x1125, 0x1400),
    "hms_500_v2": (0x1125, 0x1400),
    "hms_600": (0x1143, 0x1144, 0x114A, 0x1410),
    "hms_700": (0x1143, 0x1144, 0x114A, 0x1410),
    "hms_800": (0x1143, 0x1144, 0x114A, 0x1410),
    "hms_900": (0x1143, 0x1144, 0x114A, 0x1410),
    "hms_1000": (0x1143, 0x1144, 0x114A, 0x1410),
    "hms_1600": (0x1164, 0x1166, 0x1420),
    "hms_1800": (0x1164, 0x1166, 0x1420),
    "hms_2000": (0x1164, 0x1166, 0x1420),
}


def validate_serial(value):
    # Hoymiles serials are packed BCD: the printed digits map directly to the
    # radio-address bytes (exactly like Ahoy, which parses them as base-16).
    return parse_serial_bcd(value)


def _validate_inverter(config):
    # The serial's first four printed digits identify the hardware family, which in
    # turn fixes the record layout: pairing the wrong model with a serial decodes to
    # plausible-looking nonsense rather than failing. Only warn, though - the prefix
    # lists are what OpenDTU and Ahoy know about, and Hoymiles keeps adding
    # hardware revisions.
    prefixes = MODEL_SERIAL_PREFIXES.get(config[CONF_MODEL])
    if prefixes is None:
        return config
    prefix = (config[CONF_SERIAL] >> 32) & 0xFFFF
    if prefix not in prefixes:
        expected = " or ".join(f"{value:04X}" for value in prefixes)
        _LOGGER.warning(
            "hoymiles_dtu_base: inverter serial %012X starts with %04X, but model %s "
            "is normally sold with serial prefix %s. Double-check the label - a "
            "mismatched model decodes the telemetry with the wrong field offsets.",
            config[CONF_SERIAL],
            prefix,
            config[CONF_MODEL],
            expected,
        )
    return config


INVERTER_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(HoymilesDtuInverter),
            cv.Required(CONF_SERIAL): validate_serial,
            cv.Required(CONF_MODEL): cv.enum(MODEL_OPTIONS, lower=True),
        }
    ),
    _validate_inverter,
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


def validate_inverter_models(allowed, other_radio, other_component):
    """Reject inverter models that belong to the other radio.

    HM inverters only answer on 2.4 GHz (nRF24) and HMS inverters only on sub-GHz
    (CMT2300A), so a hub can never poll a model that belongs to the other radio.
    """

    def validator(config):
        for index, inverter in enumerate(config[CONF_INVERTERS]):
            model = inverter[CONF_MODEL]
            if model not in allowed:
                raise cv.Invalid(
                    f"{model} needs the {other_radio} radio: configure it under "
                    f"{other_component}",
                    path=[CONF_INVERTERS, index, CONF_MODEL],
                )
        return config

    return validator


async def register_hub(var, config):
    """Code shared by both radio hubs: component registration and the inverter list."""
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
