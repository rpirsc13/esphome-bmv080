import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import number

from . import CONF_BMV080_ID, BMV080Component, bmv080_ns

BMV080IntegrationNumber = bmv080_ns.class_(
    "BMV080IntegrationNumber", number.Number, cg.Component
)

CONFIG_SCHEMA = number.number_schema(
    BMV080IntegrationNumber,
    icon="mdi:clock-fast",
    unit_of_measurement="s",
).extend(
    {
        cv.Required(CONF_BMV080_ID): cv.use_id(BMV080Component),
    }
).extend(cv.COMPONENT_SCHEMA)

DEPENDENCIES = ["bmv080"]


async def to_code(config):
    hub = await cg.get_variable(config[CONF_BMV080_ID])
    var = await number.new_number(config, min_value=10, max_value=3600, step=1)
    await cg.register_component(var, config)
    cg.add(var.set_parent(hub))
    cg.add(hub.set_integration_time_number_entity(var))
