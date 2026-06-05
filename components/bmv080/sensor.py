import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    DEVICE_CLASS_PM1,
    DEVICE_CLASS_PM25,
    DEVICE_CLASS_PM10,
    UNIT_MICROGRAMS_PER_CUBIC_METER,
    STATE_CLASS_MEASUREMENT,
)

from . import CONF_BMV080_ID, BMV080Component

DEPENDENCIES = ["bmv080"]

TYPES = {
    "pm_1_0_mass": "set_pm_1_0_mass_sensor",
    "pm_2_5_mass": "set_pm_2_5_mass_sensor",
    "pm_10_0_mass": "set_pm_10_0_mass_sensor",
    "pm_1_0_count": "set_pm_1_0_count_sensor",
    "pm_2_5_count": "set_pm_2_5_count_sensor",
    "pm_10_0_count": "set_pm_10_0_count_sensor",
    "runtime": "set_runtime_sensor",
    # Backward-compatible YAML keys
    "pm_1_0": "set_pm_1_0_mass_sensor",
    "pm_2_5": "set_pm_2_5_mass_sensor",
    "pm_10": "set_pm_10_0_mass_sensor",
    "pm_10_count": "set_pm_10_0_count_sensor",
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_BMV080_ID): cv.use_id(BMV080Component),
        cv.Optional("pm_1_0_mass"): sensor.sensor_schema(
            unit_of_measurement=UNIT_MICROGRAMS_PER_CUBIC_METER,
            device_class=DEVICE_CLASS_PM1,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("pm_2_5_mass"): sensor.sensor_schema(
            unit_of_measurement=UNIT_MICROGRAMS_PER_CUBIC_METER,
            device_class=DEVICE_CLASS_PM25,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("pm_10_0_mass"): sensor.sensor_schema(
            unit_of_measurement=UNIT_MICROGRAMS_PER_CUBIC_METER,
            device_class=DEVICE_CLASS_PM10,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("pm_1_0_count"): sensor.sensor_schema(
            unit_of_measurement="particles/cm³",
            icon="mdi:counter",
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("pm_2_5_count"): sensor.sensor_schema(
            unit_of_measurement="particles/cm³",
            icon="mdi:counter",
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("pm_10_0_count"): sensor.sensor_schema(
            unit_of_measurement="particles/cm³",
            icon="mdi:counter",
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("runtime"): sensor.sensor_schema(
            unit_of_measurement="s",
            icon="mdi:timer-outline",
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        # Legacy keys
        cv.Optional("pm_1_0"): sensor.sensor_schema(
            unit_of_measurement=UNIT_MICROGRAMS_PER_CUBIC_METER,
            device_class=DEVICE_CLASS_PM1,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("pm_2_5"): sensor.sensor_schema(
            unit_of_measurement=UNIT_MICROGRAMS_PER_CUBIC_METER,
            device_class=DEVICE_CLASS_PM25,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("pm_10"): sensor.sensor_schema(
            unit_of_measurement=UNIT_MICROGRAMS_PER_CUBIC_METER,
            device_class=DEVICE_CLASS_PM10,
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
        cv.Optional("pm_10_count"): sensor.sensor_schema(
            unit_of_measurement="particles/cm³",
            icon="mdi:counter",
            state_class=STATE_CLASS_MEASUREMENT,
            accuracy_decimals=0,
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_BMV080_ID])
    for key, setter in TYPES.items():
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(hub, setter)(sens))
