import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, spi
from esphome.const import CONF_ID

CONF_BUS_TYPE = "bus_type"
import logging
import os

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@sweitzja", "@polverine-community"]
MULTI_CONF = True
AUTO_LOAD = ["sensor", "binary_sensor", "select", "number"]

bmv080_ns = cg.esphome_ns.namespace("bmv080")
BMV080Component = bmv080_ns.class_("BMV080Component", cg.PollingComponent)
I2CBMV080Component = bmv080_ns.class_("I2CBMV080Component", BMV080Component, i2c.I2CDevice)
SPIBMV080Component = bmv080_ns.class_("SPIBMV080Component", BMV080Component, spi.SPIDevice)

BMV080Preset = bmv080_ns.enum("BMV080Preset")
BMV080_PRESETS = {
    "FAST_RESPONSE": BMV080Preset.BMV080_PRESET_FAST,
    "BALANCED": BMV080Preset.BMV080_PRESET_BALANCED,
    "HIGH_PRECISION": BMV080Preset.BMV080_PRESET_PRECISION,
}

CONF_BMV080_ID = "bmv080_id"
CONF_ALGORITHM_PRESET = "algorithm_preset"
CONF_INTEGRATION_TIME = "integration_time"

CONFIG_SCHEMA = cv.typed_schema(
    {
        "i2c": cv.Schema(
            {
                cv.GenerateID(CONF_ID): cv.declare_id(I2CBMV080Component),
                cv.Optional(CONF_ALGORITHM_PRESET, default="BALANCED"): cv.enum(
                    BMV080_PRESETS, upper=True
                ),
                cv.Optional(CONF_INTEGRATION_TIME, default=20): cv.int_range(
                    min=10, max=3600
                ),
            }
        )
        .extend(i2c.i2c_device_schema(0x57))
        .extend(cv.polling_component_schema("20s")),
        "spi": cv.Schema(
            {
                cv.GenerateID(CONF_ID): cv.declare_id(SPIBMV080Component),
                cv.Optional(CONF_ALGORITHM_PRESET, default="BALANCED"): cv.enum(
                    BMV080_PRESETS, upper=True
                ),
                cv.Optional(CONF_INTEGRATION_TIME, default=20): cv.int_range(
                    min=10, max=3600
                ),
            }
        )
        .extend(spi.spi_device_schema())
        .extend(cv.polling_component_schema("20s")),
    },
    key=CONF_BUS_TYPE,
)


def _add_bosch_sdk_build_flags():
    component_dir = os.path.dirname(os.path.abspath(__file__))
    bosch_dir = os.path.join(component_dir, "bosch")
    cg.add_build_flag(f"-I{bosch_dir}")

    arch_dirs = ["esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6"]
    found_any = False
    for arch in arch_dirs:
        lib_path = os.path.join(bosch_dir, arch)
        if os.path.isdir(lib_path):
            cg.add_build_flag(f"-L{lib_path}")
            _LOGGER.info("BMV080: Found library path for %s: %s", arch, lib_path)
            found_any = True

    if not found_any:
        _LOGGER.error(
            "BMV080: No prebuilt library directories found in %s", bosch_dir
        )

    cg.add_build_flag("-l_bmv080")
    cg.add_build_flag("-l_postProcessor")


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if config[CONF_BUS_TYPE] == "i2c":
        await i2c.register_i2c_device(var, config)
        cg.add_define("USE_BMV080_I2C")
    elif config[CONF_BUS_TYPE] == "spi":
        await spi.register_spi_device(var, config)
        cg.add_define("USE_BMV080_SPI")

    cg.add(var.set_initial_preset(config[CONF_ALGORITHM_PRESET]))
    cg.add(var.set_initial_integration_time(config[CONF_INTEGRATION_TIME]))

    _add_bosch_sdk_build_flags()
