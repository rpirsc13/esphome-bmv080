"""
ESPHome BMV080 Particulate Matter Sensor — Hub Component

This module registers the BMV080 as an ESPHome hub component. It defines the YAML
configuration schema, generates C++ code for the BMV080Component class, and sets up
PlatformIO build flags to link against the Bosch precompiled SDK libraries.

The BMV080 uses the Bosch precompiled SDK (lib_bmv080.a + lib_postProcessor.a) which
is bundled in the bosch/ subdirectory. These are architecture-specific static libraries
(Xtensa for ESP32/S3, RISC-V for C3/C6). The linker automatically selects the correct
.a files and skips incompatible ones.

Hub/Platform Pattern:
  - This file (__init__.py) defines the hub component (BMV080Component)
  - sensor.py defines the sensor platform (PM mass, number, runtime)
  - binary_sensor.py defines the binary sensor platform (obstructed, out_of_range)
  - Sensors reference the hub via bmv080_id

The BMV080 supports both I2C and SPI. Configure one or the other via cv.requires_one.

YAML Example (I2C):
  bmv080:
    id: bmv080_sensor
    i2c:
      address: 0x57
    mode: continuous
    measurement_algorithm: high_precision
    integration_time: 10.0
    update_interval: 5s

YAML Example (SPI):
  bmv080:
    id: bmv080_sensor
    spi:
      cs_pin: GPIO5
    mode: continuous
    measurement_algorithm: high_precision
    integration_time: 10.0
    update_interval: 5s
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import i2c, spi
from esphome.const import CONF_ID
import os
import logging

_LOGGER = logging.getLogger(__name__)

# Component metadata
CODEOWNERS = ["@sweitzja"]
DEPENDENCIES = []  # Resolved at config time: i2c or spi
AUTO_LOAD = ["sensor", "binary_sensor"]  # Auto-load sensor platforms
MULTI_CONF = True  # Allow multiple BMV080 instances (different addresses or CS pins)

# Configuration keys for YAML schema
CONF_BMV080_ID = "bmv080_id"
CONF_I2C = "i2c"
CONF_SPI = "spi"
CONF_MODE = "mode"
CONF_MEASUREMENT_ALGORITHM = "measurement_algorithm"
CONF_INTEGRATION_TIME = "integration_time"
CONF_DUTY_CYCLING_PERIOD = "duty_cycling_period"
CONF_OBSTRUCTION_DETECTION = "obstruction_detection"
CONF_VIBRATION_FILTERING = "vibration_filtering"

# C++ namespace and class references for code generation
# BMV080Component is the base type used by sensor/binary_sensor platforms (use_id)
# BMV080I2CComponent and BMV080SPIComponent are the concrete implementations
bmv080_ns = cg.esphome_ns.namespace("bmv080")
BMV080Component = bmv080_ns.class_("BMV080Component", cg.PollingComponent)
BMV080I2CComponent = bmv080_ns.class_(
    "BMV080I2CComponent", BMV080Component, i2c.I2CDevice
)
BMV080SPIComponent = bmv080_ns.class_(
    "BMV080SPIComponent", BMV080Component, spi.SPIDevice
)

# Measurement mode enum mapping (YAML string -> C++ enum value)
# - continuous: Sensor runs constantly, data every ~1s
# - duty_cycle: Sensor cycles on/off to save power, data every duty_cycling_period
MeasurementMode = bmv080_ns.enum("MeasurementMode")
MODE_OPTIONS = {
    "continuous": MeasurementMode.MEASUREMENT_MODE_CONTINUOUS,
    "duty_cycle": MeasurementMode.MEASUREMENT_MODE_DUTY_CYCLE,
}

# Measurement algorithm enum mapping (YAML string -> C++ enum value)
# - fast_response: Quickest updates, lower accuracy
# - balanced: Middle ground
# - high_precision: Slowest updates, highest accuracy (default)
# Note: Duty cycle mode forces fast_response regardless of this setting
MeasurementAlgorithm = bmv080_ns.enum("MeasurementAlgorithm")
ALGORITHM_OPTIONS = {
    "fast_response": MeasurementAlgorithm.ALGORITHM_FAST_RESPONSE,
    "balanced": MeasurementAlgorithm.ALGORITHM_BALANCED,
    "high_precision": MeasurementAlgorithm.ALGORITHM_HIGH_PRECISION,
}

# YAML configuration schema
# Nested i2c or spi config — exactly one required
CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(BMV080Component),
            # Measurement mode: continuous (always on) or duty_cycle (periodic on/off)
            cv.Optional(CONF_MODE, default="continuous"): cv.enum(
                MODE_OPTIONS, lower=True
            ),
            # Algorithm precision vs. speed tradeoff
            cv.Optional(
                CONF_MEASUREMENT_ALGORITHM, default="high_precision"
            ): cv.enum(ALGORITHM_OPTIONS, lower=True),
            # Integration time in seconds — the measurement window duration
            # Also serves as the ON time in duty cycle mode
            cv.Optional(CONF_INTEGRATION_TIME, default=10.0): cv.float_range(
                min=1.0, max=300.0
            ),
            # Duty cycling period in seconds — total ON+OFF period
            # Must exceed integration_time by at least 2 seconds
            cv.Optional(CONF_DUTY_CYCLING_PERIOD, default=30): cv.int_range(
                min=12, max=3600
            ),
            # Enable/disable optical path obstruction detection
            cv.Optional(CONF_OBSTRUCTION_DETECTION, default=True): cv.boolean,
            # Enable/disable vibration noise filtering
            cv.Optional(CONF_VIBRATION_FILTERING, default=False): cv.boolean,
            # Bus: exactly one of i2c or spi required
            cv.Optional(CONF_I2C): i2c.i2c_device_schema(0x57),
            cv.Optional(CONF_SPI): spi.spi_device_schema(cs_pin_required=True),
        }
    ).extend(cv.polling_component_schema("1s")),
    cv.requires_one(CONF_I2C, CONF_SPI),
)


async def to_code(config):
    """Generate C++ code from the YAML configuration.

    This function:
    1. Creates a BMV080I2CComponent or BMV080SPIComponent based on config
    2. Registers it with ESPHome's component and I2C/SPI bus
    3. Passes all configuration values to the C++ setters
    4. Discovers and adds Bosch SDK library paths as build flags
    5. Links the precompiled SDK static libraries
    """
    # Create the C++ component variable — I2C or SPI based on config
    if CONF_I2C in config:
        var = cg.new_Pvariable(config[CONF_ID], BMV080I2CComponent)
        await i2c.register_i2c_device(var, config[CONF_I2C])
    else:
        var = cg.new_Pvariable(config[CONF_ID], BMV080SPIComponent)
        await spi.register_spi_device(var, config[CONF_SPI])

    await cg.register_component(var, config)

    # Pass configuration values to C++ setters
    cg.add(var.set_mode(config[CONF_MODE]))
    cg.add(var.set_measurement_algorithm(config[CONF_MEASUREMENT_ALGORITHM]))
    cg.add(var.set_integration_time(config[CONF_INTEGRATION_TIME]))
    cg.add(var.set_duty_cycling_period(config[CONF_DUTY_CYCLING_PERIOD]))
    cg.add(var.set_obstruction_detection(config[CONF_OBSTRUCTION_DETECTION]))
    cg.add(var.set_vibration_filtering(config[CONF_VIBRATION_FILTERING]))

    # --- Bosch SDK Linking ---
    # The SDK consists of headers (bmv080.h, bmv080_defs.h) and precompiled static
    # libraries (lib_bmv080.a, lib_postProcessor.a) in architecture-specific subdirs.

    # Get absolute path to the bosch/ directory containing SDK files
    component_dir = os.path.dirname(os.path.abspath(__file__))
    bosch_dir = os.path.join(component_dir, "bosch")

    # Add include path so C++ code can #include "bmv080.h" and "bmv080_defs.h"
    # This resolves to bosch/bmv080.h (not the component's bmv080_component.h)
    cg.add_build_flag(f"-I{bosch_dir}")

    # Scan for architecture-specific library directories.
    # Each contains lib_bmv080.a and lib_postProcessor.a compiled for that architecture.
    # The linker will automatically skip incompatible .a files (e.g., Xtensa .a on RISC-V).
    #
    # Architecture mapping:
    #   esp32    -> Xtensa LX6 (ESP32 original)
    #   esp32s2  -> Xtensa LX7 (not currently available from Bosch)
    #   esp32s3  -> Xtensa LX7
    #   esp32c3  -> RISC-V rv32imc (uses esp32c6 libs — same ISA)
    #   esp32c6  -> RISC-V rv32imc
    arch_dirs = ["esp32", "esp32s2", "esp32s3", "esp32c3", "esp32c6"]

    found_any = False
    for arch in arch_dirs:
        lib_path = os.path.join(bosch_dir, arch)
        if os.path.isdir(lib_path):
            # Add library search path — linker will look here for -l_bmv080 etc.
            cg.add_build_flag(f"-L{lib_path}")
            _LOGGER.info("BMV080: Found library path for %s: %s", arch, lib_path)
            found_any = True

    if not found_any:
        _LOGGER.error(
            "BMV080: No prebuilt library directories found in %s. "
            "Expected subdirectories like esp32/, esp32s3/, esp32c3/",
            bosch_dir,
        )

    # Link the two Bosch SDK static libraries.
    # Note the underscore prefix in the library names: lib_bmv080.a -> -l_bmv080
    # Both libraries are required — lib_bmv080 is the main SDK, lib_postProcessor
    # handles PM data post-processing algorithms.
    cg.add_build_flag("-l_bmv080")
    cg.add_build_flag("-l_postProcessor")
