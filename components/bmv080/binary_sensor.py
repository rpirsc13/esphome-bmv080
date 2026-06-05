import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor

from . import CONF_BMV080_ID, BMV080Component

DEPENDENCIES = ["bmv080"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_BMV080_ID): cv.use_id(BMV080Component),
        cv.Optional("obstruction"): binary_sensor.binary_sensor_schema(
            device_class="problem"
        ),
        cv.Optional("saturation"): binary_sensor.binary_sensor_schema(
            device_class="safety"
        ),
        # Legacy keys
        cv.Optional("obstructed"): binary_sensor.binary_sensor_schema(
            device_class="problem"
        ),
        cv.Optional("out_of_range"): binary_sensor.binary_sensor_schema(
            device_class="problem"
        ),
    }
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_BMV080_ID])
    if "obstruction" in config:
        sens = await binary_sensor.new_binary_sensor(config["obstruction"])
        cg.add(hub.set_obstruction_binary_sensor(sens))
    if "saturation" in config:
        sens = await binary_sensor.new_binary_sensor(config["saturation"])
        cg.add(hub.set_saturation_binary_sensor(sens))
    if "obstructed" in config:
        sens = await binary_sensor.new_binary_sensor(config["obstructed"])
        cg.add(hub.set_obstruction_binary_sensor(sens))
    if "out_of_range" in config:
        sens = await binary_sensor.new_binary_sensor(config["out_of_range"])
        cg.add(hub.set_saturation_binary_sensor(sens))
