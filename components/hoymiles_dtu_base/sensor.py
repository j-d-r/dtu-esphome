import logging

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import sensor
from esphome.const import (
    CONF_NUMBER,
    CONF_PLATFORM,
    CONF_SIGNAL_STRENGTH,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_FREQUENCY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    DEVICE_CLASS_REACTIVE_POWER,
    DEVICE_CLASS_SIGNAL_STRENGTH,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_CELSIUS,
    UNIT_DECIBEL_MILLIWATT,
    UNIT_EMPTY,
    UNIT_HERTZ,
    UNIT_KILOWATT_HOURS,
    UNIT_VOLT,
    UNIT_VOLT_AMPS_REACTIVE,
    UNIT_WATT,
    UNIT_WATT_HOURS,
)
from esphome.core import CORE

from . import (
    MODELS,
    HoymilesDtuBase,
    HoymilesDtuInverter,
    find_inverter_model,
)
from .const import (
    CONF_AC_CURRENT,
    CONF_AC_FREQUENCY,
    CONF_AC_POWER,
    CONF_AC_VOLTAGE,
    CONF_CHANNELS,
    CONF_DC_CURRENT,
    CONF_DC_POWER,
    CONF_DC_VOLTAGE,
    CONF_DTU_ID,
    CONF_INVERTER_ID,
    CONF_POWER_FACTOR,
    CONF_REACTIVE_POWER,
    CONF_SCAN_FOUND_COUNT,
    CONF_SCAN_SEVERITY,
    CONF_TEMPERATURE,
    CONF_YIELD_TODAY,
    CONF_YIELD_TOTAL,
)

SENSOR_TYPES = {
    CONF_AC_POWER: sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_AC_VOLTAGE: sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_AC_CURRENT: sensor.sensor_schema(
        unit_of_measurement=UNIT_AMPERE,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_CURRENT,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_AC_FREQUENCY: sensor.sensor_schema(
        unit_of_measurement=UNIT_HERTZ,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_FREQUENCY,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_REACTIVE_POWER: sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT_AMPS_REACTIVE,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_REACTIVE_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_POWER_FACTOR: sensor.sensor_schema(
        unit_of_measurement=UNIT_EMPTY,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_POWER_FACTOR,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_TEMPERATURE: sensor.sensor_schema(
        unit_of_measurement=UNIT_CELSIUS,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_TEMPERATURE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_YIELD_TODAY: sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT_HOURS,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_ENERGY,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    ),
    CONF_YIELD_TOTAL: sensor.sensor_schema(
        unit_of_measurement=UNIT_KILOWATT_HOURS,
        accuracy_decimals=3,
        device_class=DEVICE_CLASS_ENERGY,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    ),
    # Only the CMT2300A reports per-packet RSSI, so this stays empty on an
    # nRF24 (HM) hub.
    CONF_SIGNAL_STRENGTH: sensor.sensor_schema(
        unit_of_measurement=UNIT_DECIBEL_MILLIWATT,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_SIGNAL_STRENGTH,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    ),
}

CHANNEL_SENSOR_TYPES = {
    CONF_DC_VOLTAGE: sensor.sensor_schema(
        unit_of_measurement=UNIT_VOLT,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_VOLTAGE,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_DC_CURRENT: sensor.sensor_schema(
        unit_of_measurement=UNIT_AMPERE,
        accuracy_decimals=2,
        device_class=DEVICE_CLASS_CURRENT,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_DC_POWER: sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT,
        accuracy_decimals=1,
        device_class=DEVICE_CLASS_POWER,
        state_class=STATE_CLASS_MEASUREMENT,
    ),
    CONF_YIELD_TODAY: sensor.sensor_schema(
        unit_of_measurement=UNIT_WATT_HOURS,
        accuracy_decimals=0,
        device_class=DEVICE_CLASS_ENERGY,
        state_class=STATE_CLASS_TOTAL_INCREASING,
    ),
}

DTU_SENSOR_TYPES = {
    CONF_SCAN_SEVERITY: sensor.sensor_schema(
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon="mdi:shield-alert",
    ),
    CONF_SCAN_FOUND_COUNT: sensor.sensor_schema(
        accuracy_decimals=0,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        icon="mdi:radar",
    ),
}

CHANNEL_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NUMBER): cv.int_range(min=1, max=4),
        **{cv.Optional(key): schema for key, schema in CHANNEL_SENSOR_TYPES.items()},
    }
)


def validate_sensor_config(config):
    if (
        any(key in config for key in SENSOR_TYPES) or CONF_CHANNELS in config
    ) and CONF_INVERTER_ID not in config:
        raise cv.Invalid("inverter sensors require inverter_id")
    if any(key in config for key in DTU_SENSOR_TYPES) and CONF_DTU_ID not in config:
        raise cv.Invalid("scan_severity requires dtu_id")
    if CONF_INVERTER_ID not in config and CONF_DTU_ID not in config:
        raise cv.Invalid("either inverter_id or dtu_id is required")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Optional(CONF_INVERTER_ID): cv.use_id(HoymilesDtuInverter),
            cv.Optional(CONF_DTU_ID): cv.use_id(HoymilesDtuBase),
            **{cv.Optional(key): schema for key, schema in SENSOR_TYPES.items()},
            cv.Optional(CONF_CHANNELS): cv.ensure_list(CHANNEL_SCHEMA),
            **{cv.Optional(key): schema for key, schema in DTU_SENSOR_TYPES.items()},
        }
    ),
    validate_sensor_config,
)


_LOGGER = logging.getLogger(__name__)


# Inverter ids whose channels were already checked during this config run. The check
# looks at every sensor entry of an inverter at once (packages split them across
# entries), so it only needs to run - and warn - once per inverter.
_CHECKED_KEY = "hoymiles_dtu_base_channels_checked"


def _declared_channels(full_config, inverter_id):
    """Channel numbers declared for this inverter in any hoymiles_dtu_base sensor."""
    numbers = set()
    for entry in full_config.get("sensor", []):
        if entry.get(CONF_PLATFORM) != "hoymiles_dtu_base":
            continue
        if CONF_INVERTER_ID not in entry or entry[CONF_INVERTER_ID].id != inverter_id:
            continue
        numbers.update(channel[CONF_NUMBER] for channel in entry.get(CONF_CHANNELS, []))
    return numbers


def _check_channels(config):
    # Compare the declared channels with the model's DC inputs. Only warn, like the
    # serial-prefix check: the model may be the wrong part, not the channels.
    if CONF_INVERTER_ID not in config:
        return config
    inverter_id = config[CONF_INVERTER_ID].id
    checked = CORE.data.setdefault(_CHECKED_KEY, set())
    if inverter_id in checked:
        return config
    checked.add(inverter_id)

    full_config = fv.full_config.get()
    model = find_inverter_model(full_config, config[CONF_INVERTER_ID])
    declared = _declared_channels(full_config, inverter_id)
    if model is None or not declared:
        # No channels at all reads as a deliberate choice (AC values only).
        return config
    count = MODELS[model].family.channels
    inputs = f"{count} DC input{'' if count == 1 else 's'}"

    def channel_list(numbers):
        word = "channel" if len(numbers) == 1 else "channels"
        return f"{word} {', '.join(map(str, numbers))}"

    if extra := sorted(n for n in declared if n > count):
        _LOGGER.warning(
            "hoymiles_dtu_base: inverter %s (model %s) has %s, so its %s sensors "
            "will only ever report NaN (unknown). Check the model or remove them.",
            inverter_id,
            model,
            inputs,
            channel_list(extra),
        )
    if missing := [n for n in range(1, count + 1) if n not in declared]:
        _LOGGER.warning(
            "hoymiles_dtu_base: inverter %s (model %s) has %s, but no sensors are "
            "declared for %s. Check the model, or add them (with the packages: set "
            "channels: %d on the inverter.yaml package).",
            inverter_id,
            model,
            inputs,
            channel_list(missing),
            count,
        )
    return config


FINAL_VALIDATE_SCHEMA = _check_channels


async def to_code(config):
    if CONF_INVERTER_ID in config:
        inverter = await cg.get_variable(config[CONF_INVERTER_ID])
        for key in SENSOR_TYPES:
            if key not in config:
                continue
            sens = await sensor.new_sensor(config[key])
            setter = getattr(inverter, f"set_{key}_sensor")
            cg.add(setter(sens))

        for channel_config in config.get(CONF_CHANNELS, []):
            number = channel_config[CONF_NUMBER]
            for key in CHANNEL_SENSOR_TYPES:
                if key not in channel_config:
                    continue
                sens = await sensor.new_sensor(channel_config[key])
                setter_name = f"set_channel_{key}_sensor"
                cg.add(getattr(inverter, setter_name)(number, sens))

    if CONF_DTU_ID in config:
        dtu = await cg.get_variable(config[CONF_DTU_ID])
        for key in DTU_SENSOR_TYPES:
            if key not in config:
                continue
            sens = await sensor.new_sensor(config[key])
            setter = getattr(dtu, f"set_{key}_sensor")
            cg.add(setter(sens))
