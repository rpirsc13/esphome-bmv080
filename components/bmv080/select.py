import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select

from . import CONF_BMV080_ID, BMV080Component, bmv080_ns

BMV080Select = bmv080_ns.class_("BMV080Select", select.Select, cg.Component)

CONFIG_SCHEMA = select.select_schema(BMV080Select, icon="mdi:tune").extend(
    {
        cv.Required(CONF_BMV080_ID): cv.use_id(BMV080Component),
    }
).extend(cv.COMPONENT_SCHEMA)

DEPENDENCIES = ["bmv080"]


async def to_code(config):
    hub = await cg.get_variable(config[CONF_BMV080_ID])
    var = await select.new_select(
        config, options=["Fast Response", "Balanced", "High Precision"]
    )
    await cg.register_component(var, config)
    cg.add(var.set_parent(hub))
    cg.add(hub.set_preset_select_entity(var))
