import logging
from typing import NamedTuple

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import CONF_ID
from esphome.cpp_generator import MockObj

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

# Radios, with the hub component and display name of each.
NRF24 = "nrf24"
CMT = "cmt2300a"
RADIOS = {
    NRF24: ("hoymiles_dtu_rf24", "nRF24"),
    CMT: ("hoymiles_dtu_cmt", "CMT2300A"),
}
# The radio hub components whose `inverters:` lists define the inverters.
HUB_DOMAINS = tuple(domain for domain, _ in RADIOS.values())


class ModelInfo(NamedTuple):
    model: MockObj  # HmModel enum value
    radio: str  # NRF24 or CMT
    channels: int  # channel count
    prefixes: tuple[int, ...]  # serial prefixes


MODELS = {
    "hm_300": ModelInfo(HmModel.HM_300, NRF24, 1, (0x1022, 0x1121, 0x1122)),
    "hm_350": ModelInfo(HmModel.HM_350, NRF24, 1, (0x1022, 0x1121, 0x1122)),
    "hm_400": ModelInfo(HmModel.HM_400, NRF24, 1, (0x1022, 0x1121, 0x1122)),
    "hm_600": ModelInfo(HmModel.HM_600, NRF24, 2, (0x1042, 0x1141, 0x1142)),
    "hm_700": ModelInfo(HmModel.HM_700, NRF24, 2, (0x1042, 0x1141, 0x1142)),
    "hm_800": ModelInfo(HmModel.HM_800, NRF24, 2, (0x1042, 0x1141, 0x1142)),
    "hm_1000": ModelInfo(HmModel.HM_1000, NRF24, 4, (0x1062, 0x1161, 0x1162)),
    "hm_1200": ModelInfo(HmModel.HM_1200, NRF24, 4, (0x1062, 0x1161, 0x1162)),
    "hm_1500": ModelInfo(HmModel.HM_1500, NRF24, 4, (0x1062, 0x1161, 0x1162)),
    "herf_300": ModelInfo(HmModel.HERF_300, NRF24, 1, (0x2841,)),
    "herf_600": ModelInfo(HmModel.HERF_600, NRF24, 2, (0x2821,)),
    "herf_800": ModelInfo(HmModel.HERF_800, NRF24, 2, (0x2821,)),
    "herf_1600": ModelInfo(HmModel.HERF_1600, NRF24, 4, (0x2801,)),
    "herf_1800": ModelInfo(HmModel.HERF_1800, NRF24, 4, (0x2801,)),
    "hms_300": ModelInfo(HmModel.HMS_300, CMT, 1, (0x1124,)),
    "hms_350": ModelInfo(HmModel.HMS_350, CMT, 1, (0x1124,)),
    "hms_400": ModelInfo(HmModel.HMS_400, CMT, 1, (0x1124,)),
    "hms_450": ModelInfo(HmModel.HMS_450, CMT, 1, (0x1124,)),
    "hms_500": ModelInfo(HmModel.HMS_500, CMT, 1, (0x1124,)),
    "hms_450_v2": ModelInfo(HmModel.HMS_450_V2, CMT, 1, (0x1125, 0x1400)),
    "hms_500_v2": ModelInfo(HmModel.HMS_500_V2, CMT, 1, (0x1125, 0x1400)),
    "hms_600": ModelInfo(
        HmModel.HMS_600, CMT, 2, (0x1143, 0x1144, 0x1146, 0x114A, 0x1410)
    ),
    "hms_700": ModelInfo(
        HmModel.HMS_700, CMT, 2, (0x1143, 0x1144, 0x1146, 0x114A, 0x1410)
    ),
    "hms_800": ModelInfo(
        HmModel.HMS_800, CMT, 2, (0x1143, 0x1144, 0x1146, 0x114A, 0x1410)
    ),
    "hms_900": ModelInfo(
        HmModel.HMS_900, CMT, 2, (0x1143, 0x1144, 0x1146, 0x114A, 0x1410)
    ),
    "hms_1000": ModelInfo(
        HmModel.HMS_1000, CMT, 2, (0x1143, 0x1144, 0x1146, 0x114A, 0x1410)
    ),
    "hms_1600": ModelInfo(HmModel.HMS_1600, CMT, 4, (0x1164, 0x1166, 0x1420, 0x1421)),
    "hms_1800": ModelInfo(HmModel.HMS_1800, CMT, 4, (0x1164, 0x1166, 0x1420, 0x1421)),
    "hms_2000": ModelInfo(HmModel.HMS_2000, CMT, 4, (0x1164, 0x1166, 0x1420, 0x1421)),
}


def find_inverter_model(full_config, inverter_id):
    """Model key (e.g. "hms_1000") of the inverter with this id, or None."""
    for domain in HUB_DOMAINS:
        hub = full_config.get(domain)
        if hub is None:
            continue
        for inverter in hub.get(CONF_INVERTERS, []):
            if inverter[CONF_ID].id == inverter_id.id:
                return str(inverter[CONF_MODEL])
    return None


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
    prefixes = MODELS[config[CONF_MODEL]].prefixes
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
            cv.Required(CONF_MODEL): cv.enum(
                {key: info.model for key, info in MODELS.items()}, lower=True
            ),
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


def validate_inverter_models(radio):
    """Reject inverter models that need another radio than this hub's.

    HM and HERF inverters only answer on 2.4 GHz (nRF24) and HMS inverters only on
    sub-GHz (CMT2300A), so a hub can never poll a model that belongs to the other radio.
    """

    def validator(config):
        for index, inverter in enumerate(config[CONF_INVERTERS]):
            model = inverter[CONF_MODEL]
            needed = MODELS[model].radio
            if needed != radio:
                component, name = RADIOS[needed]
                raise cv.Invalid(
                    f"{model} needs the {name} radio: configure it under {component}",
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
