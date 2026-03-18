# ESPHome BMV080 - Bosch Particulate Matter Sensor Component

An [ESPHome](https://esphome.io/) external component for the [Bosch BMV080](https://www.bosch-sensortec.com/products/environmental-sensors/particulate-matter-sensors/bmv080/) particulate matter sensor. Measures PM1.0, PM2.5, and PM10 mass and number concentrations over **I2C or SPI** using the Bosch precompiled SDK.

## Features

- **PM1.0, PM2.5, PM10** mass concentrations (ug/m3)
- **PM1.0, PM2.5, PM10** number concentrations (particles/cm3)
- **Obstruction detection** — alerts when the sensor's optical path is blocked
- **Out-of-range detection** — alerts when PM2.5 exceeds 1000 ug/m3
- **Measurement runtime** tracking (seconds since measurement start)
- **Continuous** and **duty cycle** measurement modes
- **Three algorithm presets**: fast response, balanced, high precision
- Configurable integration time, duty cycling period, vibration filtering
- Full Home Assistant integration with proper device classes

## Supported Hardware

### ESP32 Platforms

| Platform | Architecture | Status |
|----------|-------------|--------|
| ESP32 | Xtensa LX6 | Supported (untested) |
| ESP32-S3 | Xtensa LX7 | Supported (untested) |
| **ESP32-C3** | **RISC-V** | **Tested, verified working** |
| ESP32-C6 | RISC-V | Supported (untested) |

### BMV080 Breakout Boards

- [SparkFun Particulate Matter Sensor - BMV080 (Qwiic)](https://www.sparkfun.com/products/24959) — Default I2C address 0x57, 2.2k pull-ups included
- [DFRobot Gravity BMV080](https://www.dfrobot.com/product-2842.html) — Default I2C address 0x57

### Frameworks

Both **Arduino** and **ESP-IDF** frameworks are supported.

## Quick Start

### 1. Add the External Component

Add this repository as an external component source in your ESPHome YAML:

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/sweitzja/esphome-bmv080
    components: [bmv080]
```

Or for local development:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [bmv080]
```

### 2. Configure the BMV080 Hub

**I2C (default):**

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  scan: true

bmv080:
  id: bmv080_sensor
  i2c:
    address: 0x57
  mode: continuous
  measurement_algorithm: high_precision
  update_interval: 5s
```

**SPI (e.g. SparkFun breakout in SPI mode):**

```yaml
spi:
  clk_pin: GPIO18
  miso_pin: GPIO19
  mosi_pin: GPIO23

bmv080:
  id: bmv080_sensor
  spi:
    cs_pin: GPIO5
  mode: continuous
  measurement_algorithm: high_precision
  update_interval: 5s
```

> **Note:** Configure the BMV080 breakout for SPI mode via jumpers before use. See your breakout's documentation.

### 3. Add Sensors

```yaml
sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor
    pm_1_0:
      name: "PM 1.0"
    pm_2_5:
      name: "PM 2.5"
    pm_10:
      name: "PM 10"
    pm_1_0_count:
      name: "PM 1.0 Count"
    pm_2_5_count:
      name: "PM 2.5 Count"
    pm_10_count:
      name: "PM 10 Count"
    runtime:
      name: "BMV080 Runtime"

binary_sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor
    obstructed:
      name: "Sensor Obstructed"
    out_of_range:
      name: "Out of Range"
```

## Complete Configuration Reference

### Hub Configuration

Configure **either** I2C **or** SPI (exactly one required):

**I2C:**

```yaml
bmv080:
  id: bmv080_sensor
  i2c:
    address: 0x57      # 0x54, 0x55, 0x56, or 0x57 (default: 0x57)
    i2c_id: bus_i2c    # Optional: if you have multiple I2C buses

  # ... measurement options below ...
```

**SPI:**

```yaml
bmv080:
  id: bmv080_sensor
  spi:
    cs_pin: GPIO5      # Chip select pin (required for SPI)
    spi_id: spi_bus   # Optional: if you have multiple SPI buses
    data_rate: 1MHz   # Optional: default 1MHz
    spi_mode: MODE0   # Optional: MODE0, MODE1, MODE2, or MODE3

  # Measurement mode (default: continuous)
  #   continuous  — Always on, data every ~1 second, higher power
  #   duty_cycle  — Periodic on/off, saves power, data every duty_cycling_period
  mode: continuous

  # Measurement algorithm (default: high_precision)
  #   fast_response  — Quickest updates, lower accuracy
  #   balanced       — Middle ground
  #   high_precision — Slowest updates, highest accuracy
  # Note: duty_cycle mode forces fast_response regardless of this setting
  measurement_algorithm: high_precision

  # Integration time in seconds (default: 10.0, range: 1.0-300.0)
  # How long the sensor measures before producing an output.
  # Also serves as the ON time in duty cycle mode.
  integration_time: 10.0

  # Duty cycling period in seconds (default: 30, range: 12-3600)
  # Total period for one on/off cycle (duty_cycle mode only).
  # Must exceed integration_time by at least 2 seconds.
  duty_cycling_period: 30

  # Enable optical path obstruction detection (default: true)
  obstruction_detection: true

  # Enable vibration noise filtering (default: false)
  vibration_filtering: false

  # How often to publish sensor data to Home Assistant (default: 1s)
  update_interval: 5s
```

### Sensor Configuration

All sensors are optional. Only add the ones you need.

```yaml
sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor

    # PM1.0 mass concentration (ug/m3)
    # Device class: pm1 — shows as PM1 in Home Assistant
    pm_1_0:
      name: "PM 1.0"

    # PM2.5 mass concentration (ug/m3)
    # Device class: pm25 — shows as PM2.5 in Home Assistant
    pm_2_5:
      name: "PM 2.5"

    # PM10 mass concentration (ug/m3)
    # Device class: pm10 — shows as PM10 in Home Assistant
    pm_10:
      name: "PM 10"

    # PM1.0 number concentration (particles/cm3)
    pm_1_0_count:
      name: "PM 1.0 Count"

    # PM2.5 number concentration (particles/cm3)
    pm_2_5_count:
      name: "PM 2.5 Count"

    # PM10 number concentration (particles/cm3)
    pm_10_count:
      name: "PM 10 Count"

    # Time since measurement start (seconds)
    # Diagnostic entity — hidden by default in Home Assistant
    runtime:
      name: "BMV080 Runtime"
```

### Binary Sensor Configuration

```yaml
binary_sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor

    # Optical path obstruction detected
    # Device class: problem — shows as alert in Home Assistant
    obstructed:
      name: "Sensor Obstructed"

    # PM2.5 outside measurement range (0-1000 ug/m3)
    # Device class: problem — shows as alert in Home Assistant
    out_of_range:
      name: "Out of Range"
```

## Example Configurations

### Minimal (PM2.5 Only)

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/sweitzja/esphome-bmv080
    components: [bmv080]

i2c:
  sda: GPIO21
  scl: GPIO22

bmv080:
  id: bmv080_sensor

sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor
    pm_2_5:
      name: "PM 2.5"
```

### Full Configuration (All Sensors)

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/sweitzja/esphome-bmv080
    components: [bmv080]

i2c:
  sda: GPIO21
  scl: GPIO22
  scan: true

bmv080:
  id: bmv080_sensor
  address: 0x57
  mode: continuous
  measurement_algorithm: high_precision
  integration_time: 10.0
  obstruction_detection: true
  vibration_filtering: false
  update_interval: 5s

sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor
    pm_1_0:
      name: "PM 1.0"
    pm_2_5:
      name: "PM 2.5"
    pm_10:
      name: "PM 10"
    pm_1_0_count:
      name: "PM 1.0 Count"
    pm_2_5_count:
      name: "PM 2.5 Count"
    pm_10_count:
      name: "PM 10 Count"
    runtime:
      name: "BMV080 Runtime"

binary_sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor
    obstructed:
      name: "Sensor Obstructed"
    out_of_range:
      name: "Out of Range"
```

### Duty Cycle Mode (Low Power)

```yaml
bmv080:
  id: bmv080_sensor
  mode: duty_cycle
  integration_time: 10.0       # ON for 10 seconds
  duty_cycling_period: 60      # Total cycle: 60 seconds (50s sleep)
  update_interval: 60s         # Publish once per cycle

sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor
    pm_2_5:
      name: "PM 2.5"
    pm_10:
      name: "PM 10"
```

### ESP32-C3 with W5500 Ethernet

See `native-esp-w5500.yaml` in this repository for a complete example using an ESP32-C3 with W5500 SPI ethernet adapter and the SparkFun BMV080 breakout.

## How It Works

### Architecture

The component uses a **two-thread architecture** to separate the resource-intensive Bosch SDK operations from the ESPHome main loop:

**Main Loop Thread** (~8KB stack):
- Runs the ESPHome API server, web server, and OTA handler
- Publishes cached sensor data to Home Assistant at the configured interval
- Never calls any Bosch SDK functions

**BMV080 Task Thread** (64KB stack):
- Runs all Bosch SDK operations in a dedicated FreeRTOS task
- Waits 10 seconds after boot for the network stack to start
- Initializes the sensor (open, reset, configure, start measurement)
- Loops calling `bmv080_serve_interrupt()` every 500ms to read sensor data
- Stores new PM data under mutex protection for the main loop to read

### Why a Separate Task?

The Bosch SDK requires approximately **60KB of stack space** for its `bmv080_serve_interrupt()` function. The default ESP32 main task has only 8-16KB of stack, which causes immediate stack overflow. Additionally, the SDK initialization sequence blocks for 3-5 seconds, which would prevent the API server from accepting connections.

### Startup Sequence

1. **Boot** (0s) — ESP32 boots, network starts, ESPHome components initialize
2. **Setup** (2s) — BMV080 component creates a mutex and launches the FreeRTOS task
3. **API Ready** (3s) — ESPHome API server starts accepting connections
4. **Task Starts** (10s) — BMV080 task wakes up and begins sensor initialization
5. **SDK Init** (13s) — Sensor probed, reset, configured, measurement started
6. **First Data** (15s) — First PM reading available (~1.9s after measurement start)
7. **Steady State** — New PM data every ~1 second, published per update_interval

### I2C Protocol

The BMV080 uses a non-standard **16-bit word-oriented** I2C protocol:
- All transfers are in 16-bit words, not standard 8-bit register reads
- The header is shifted left by 1 for I2C mode (`header << 1`)
- Data is MSB-first (most significant byte first)
- The component handles all protocol translation internally

## Wiring

### SparkFun BMV080 Breakout (Qwiic)

| BMV080 Pin | ESP32 Pin | Description |
|-----------|-----------|-------------|
| SDA | GPIO21 (or your SDA) | I2C Data |
| SCL | GPIO22 (or your SCL) | I2C Clock |
| 3.3V | 3.3V | Power supply |
| GND | GND | Ground |

The SparkFun breakout includes 2.2k pull-up resistors on SDA/SCL. If using a bare BMV080 chip, add 2.2k-4.7k pull-ups to 3.3V on both I2C lines.

### SparkFun BMV080 Breakout (SPI Mode)

Configure the board for SPI mode via jumpers on the back before wiring. Solder headers and connect:

| BMV080 Pin | ESP32 Pin | Description |
|------------|-----------|-------------|
| 3.3V | 3.3V | Power |
| GND | GND | Ground |
| SCLK | GPIO18 | SPI clock |
| MOSI | GPIO23 | SPI data out (ESP → BMV080) |
| MISO | GPIO19 | SPI data in (BMV080 → ESP) |
| CS | GPIO5 | Chip select |

### I2C Address Selection

The BMV080 supports four I2C addresses, selected via address pins on the breakout board:

| Address | A1 | A0 |
|---------|----|----|
| 0x54 | LOW | LOW |
| 0x55 | LOW | HIGH |
| 0x56 | HIGH | LOW |
| 0x57 (default) | HIGH | HIGH |

## Troubleshooting

### Device Not Responding After Flash

The BMV080 initialization takes about 3 seconds and runs in a background task that starts 10 seconds after boot. The API server should be available within 3 seconds of boot.

If the device is completely unreachable:
1. **Power cycle** the device (unplug and replug)
2. Check the I2C wiring — the sensor needs SDA, SCL, 3.3V, and GND
3. Verify the I2C address matches your hardware (default 0x57)

### I2C Scan Shows No Devices

```
[W][i2c:xxx]: Found no devices on the I2C bus!
```

- Check physical connections (SDA, SCL, power, ground)
- Verify pull-up resistors are present (2.2k-4.7k to 3.3V)
- Try a lower I2C frequency (100kHz)
- Check that the BMV080 is powered (3.3V)

### BMV080 Initialization Failed

Check the log for specific error codes:

| Status Code | Meaning | Solution |
|------------|---------|----------|
| 105 | I2C read failed | Check wiring and pull-ups |
| 106 | I2C write failed | Check wiring and pull-ups |
| 107 | Chip ID mismatch | Wrong I2C address or wiring issue |
| 180 | Wrong API call order | Component bug — file an issue |

### No PM Data (Sensor Initialized but No Readings)

- The sensor needs ~2 seconds of warmup after starting measurement
- Ensure `update_interval` is reasonable (5s or less for continuous mode)
- Check logs for `bmv080_serve_interrupt` errors
- In duty cycle mode, data only appears once per `duty_cycling_period`

### FIFO Overflow Warnings

```
[W][bmv080:xxx]: bmv080_serve_interrupt warning: 4
```

The sensor is generating more events than can be processed. This is usually harmless but may indicate very high particulate levels. The internal service interval (500ms) should be sufficient for normal conditions.

## Resource Usage

Measured on ESP32-C3 with all sensors enabled:

| Resource | Usage |
|----------|-------|
| Flash | ~38% of 1.8MB (690KB) |
| RAM | ~9% of 320KB (30KB) |
| Task Stack | 64KB (dedicated FreeRTOS task) |
| I2C Bus | ~50% utilization during serve_interrupt |

## Development

### Building from Source

```bash
# Clone the repository
git clone https://github.com/sweitzja/esphome-bmv080.git
cd esphome-bmv080

# Compile (no upload)
esphome compile native-esp-w5500.yaml

# Compile + upload + watch logs
esphome run native-esp-w5500.yaml

# Watch logs only
esphome logs native-esp-w5500.yaml
```

### Project Structure

```
components/bmv080/
  __init__.py          — Hub component: config schema, code generation, SDK linking
  sensor.py            — Sensor platform: PM mass, number, runtime entities
  binary_sensor.py     — Binary sensor platform: obstructed, out_of_range entities
  bmv080_component.h   — C++ header: class declaration, thread model, data types
  bmv080.cpp           — C++ implementation: I2C callbacks, FreeRTOS task, lifecycle
  bosch/               — Bosch SDK (precompiled)
    bmv080.h           — SDK C API declarations
    bmv080_defs.h      — SDK type definitions and enums
    esp32/             — Prebuilt libraries for ESP32 (Xtensa)
    esp32s3/           — Prebuilt libraries for ESP32-S3 (Xtensa)
    esp32c3/           — Prebuilt libraries for ESP32-C3 (RISC-V)
    esp32c6/           — Prebuilt libraries for ESP32-C6 (RISC-V)
```

### Adding Support for New Architectures

To add support for a new ESP32 variant:

1. Obtain the prebuilt `.a` files (`lib_bmv080.a` and `lib_postProcessor.a`) for the target architecture from Bosch or a vendor library
2. Create a new subdirectory under `bosch/` (e.g., `bosch/esp32h2/`)
3. Place both `.a` files in the new directory
4. The `__init__.py` will automatically detect and add the library path

Note: ESP32-C3 and ESP32-C6 share the same RISC-V `rv32imc` ISA, so their libraries are interchangeable.

## SDK Sources

The Bosch BMV080 SDK is distributed as precompiled static libraries. This component bundles them from the following open-source repositories:

- **Headers** (`bmv080.h`, `bmv080_defs.h`): [DFRobot/DFRobot_BMV080](https://github.com/DFRobot/DFRobot_BMV080)
- **Xtensa libraries** (ESP32, ESP32-S3): [DFRobot/DFRobot_BMV080](https://github.com/DFRobot/DFRobot_BMV080)
- **RISC-V libraries** (ESP32-C3, ESP32-C6): [SparkFun/SparkFun_BMV080_Arduino_Library](https://github.com/sparkfun/SparkFun_BMV080_Arduino_Library)

SDK version: **24.2.0**

## Related Resources

- [Bosch BMV080 Product Page](https://www.bosch-sensortec.com/products/environmental-sensors/particulate-matter-sensors/bmv080/)
- [ESPHome Feature Request #3178](https://github.com/esphome/feature-requests/issues/3178) — Community request for BMV080 support
- [SparkFun BMV080 Arduino Library](https://github.com/sparkfun/SparkFun_BMV080_Arduino_Library)
- [DFRobot BMV080 Arduino Library](https://github.com/DFRobot/DFRobot_BMV080)

## License

This component's source code (Python and C++ files) is provided under the MIT License.

The Bosch SDK headers and prebuilt libraries (`bosch/` directory) are provided by Bosch Sensortec GmbH under their own license terms. See the copyright notice in `bosch/bmv080.h` and `bosch/bmv080_defs.h` for details.
