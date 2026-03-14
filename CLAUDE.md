# ESPHome BMV080 Custom Component - Developer Reference

## Project Overview

ESPHome external component for the **Bosch BMV080** particulate matter sensor over I2C.
The BMV080 is Bosch's compact photoacoustic PM sensor that measures PM1, PM2.5, and PM10
mass and number concentrations in a single, maintenance-free package.

This component wraps the Bosch precompiled C SDK (`lib_bmv080.a` + `lib_postProcessor.a`)
and exposes all sensor outputs as Home Assistant entities via ESPHome.

**Status: WORKING** — Verified on ESP32-C3 (RISC-V) with W5500 ethernet and SparkFun BMV080 breakout.

### Supported Platforms

| Platform | Architecture | ISA | Library Source | Status |
|----------|-------------|-----|----------------|--------|
| ESP32 | Xtensa | LX6 | DFRobot repo | Bundled, untested |
| ESP32-S3 | Xtensa | LX7 | DFRobot repo | Bundled, untested |
| ESP32-C3 | RISC-V | rv32imc | SparkFun repo (esp32c6 libs) | **Tested, working** |
| ESP32-C6 | RISC-V | rv32imc | SparkFun repo | Bundled, untested |
| ESP32-S2 | Xtensa | LX7 | Not available | Not supported |

### Supported Frameworks

Both **Arduino** and **ESP-IDF** frameworks are supported. Linking is done through
PlatformIO build flags (`-I`, `-L`, `-l`) which work with both toolchains.

---

## Directory Structure

```
esphome-bmv080/
├── CLAUDE.md                    # This file — developer reference
├── README.md                    # User-facing documentation and setup guide
├── TEST_PLAN.md                 # Verification checklist
├── native-esp-w5500.yaml        # Test config: ESP32-C3 + W5500 ethernet + BMV080
├── components/
│   └── bmv080/
│       ├── __init__.py          # Component registration, config schema, SDK linking
│       ├── sensor.py            # Sensor platform (PM mass + number + runtime)
│       ├── binary_sensor.py     # Binary sensor platform (obstructed, out_of_range)
│       ├── bmv080_component.h   # C++ component header
│       ├── bmv080.cpp           # C++ implementation (FreeRTOS task, callbacks, publishing)
│       └── bosch/               # Bosch SDK (precompiled, bundled)
│           ├── bmv080.h         # SDK C API function declarations
│           ├── bmv080_defs.h    # SDK type definitions, enums, callback typedefs
│           ├── esp32/           # Prebuilt .a files for ESP32 (Xtensa LX6)
│           │   ├── lib_bmv080.a
│           │   └── lib_postProcessor.a
│           ├── esp32s3/         # Prebuilt .a files for ESP32-S3 (Xtensa LX7)
│           │   ├── lib_bmv080.a
│           │   └── lib_postProcessor.a
│           ├── esp32c3/         # Prebuilt .a files for ESP32-C3 (RISC-V rv32imc)
│           │   ├── lib_bmv080.a
│           │   └── lib_postProcessor.a
│           └── esp32c6/         # Prebuilt .a files for ESP32-C6 (RISC-V rv32imc)
│               ├── lib_bmv080.a
│               └── lib_postProcessor.a
```

---

## Architecture Overview

### Threading Model

The component uses a **two-thread architecture** to separate the blocking Bosch SDK
operations from the ESPHome main loop:

```
┌─────────────────────────────────┐    ┌──────────────────────────────────┐
│     Main Loop Thread            │    │     BMV080 Task Thread           │
│     (ESPHome default ~8KB)      │    │     (Dedicated 64KB stack)       │
│                                 │    │                                  │
│  setup()                        │    │  sensor_task_()                  │
│    ├── Create mutex             │    │    ├── Wait 10s (let API start)  │
│    └── Create FreeRTOS task ────┼────┼──> ├── init_sensor_()            │
│                                 │    │    │   ├── bmv080_get_driver_ver │
│  loop()                         │    │    │   ├── bmv080_open()         │
│    └── Check sensor_failed_     │    │    │   ├── bmv080_reset()        │
│                                 │    │    │   └── bmv080_get_sensor_id  │
│  update() [every 5s]            │    │    ├── configure_parameters_()   │
│    ├── Lock mutex               │    │    ├── start_measurement_()      │
│    ├── Copy last_output_        │    │    └── Loop forever:             │
│    ├── Unlock mutex             │    │        ├── service_sensor_()     │
│    └── Publish to HA sensors    │    │        │   └── bmv080_serve_     │
│                                 │    │        │       interrupt()        │
│  API server (runs in main loop) │    │        │   └── data_ready_cb_()  │
│  Web server                     │    │        │       ├── Lock mutex    │
│  OTA handler                    │    │        │       ├── Cache output  │
│  Other ESPHome components       │    │        │       └── Unlock mutex  │
│                                 │    │        └── vTaskDelay(500ms)     │
└─────────────────────────────────┘    └──────────────────────────────────┘
```

**Why a separate task?**

1. **Stack size**: The Bosch SDK's `bmv080_serve_interrupt()` requires ~60KB of stack.
   The ESP32 default main task stack is only 8-16KB. Running the SDK in the main loop
   causes immediate stack overflow and crashes.

2. **Non-blocking**: The SDK init sequence (`bmv080_open` + `bmv080_reset`) takes ~3-5
   seconds with blocking delay callbacks. Running this in the main loop prevents the
   API server, web server, and OTA handler from accepting connections, making the device
   appear dead after flashing.

3. **10-second startup delay**: The task waits 10 seconds before initializing the sensor,
   giving the network stack and API server time to fully start. This ensures the device
   is accessible for OTA updates even if the BMV080 sensor has issues.

### Data Flow

```
Bosch SDK (in task thread)
    └── data_ready_cb_(output, user_data)
        └── on_data_ready(output)
            ├── xSemaphoreTake(data_mutex_)
            ├── last_output_ = output
            ├── data_available_ = true
            └── xSemaphoreGive(data_mutex_)

ESPHome main loop (update() every 5s)
    ├── xSemaphoreTake(data_mutex_)
    ├── local_copy = last_output_
    ├── data_available_ = false
    ├── xSemaphoreGive(data_mutex_)
    └── publish_state() for each configured sensor
```

### I2C Callback Bridge

The Bosch SDK uses a callback pattern for I2C communication. The ESPHome component
bridges this by casting the component pointer as the SDK's `sercom_handle`:

```
bmv080_open(&handle, (bmv080_sercom_handle_t) this, read_cb_, write_cb_, delay_cb_)
                     ↑                               ↑
                     │                               │
                     └── Pointer to BMV080Component  └── Static callbacks that cast
                         stored as opaque void*          sercom_handle back to
                                                         BMV080Component* to access
                                                         ESPHome I2C methods
```

This pattern is used by both the DFRobot and SparkFun Arduino libraries.

---

## Bosch SDK Details

### Source

- **Headers** (`bmv080.h`, `bmv080_defs.h`): From [DFRobot/DFRobot_BMV080](https://github.com/DFRobot/DFRobot_BMV080)
- **Xtensa .a files** (esp32, esp32s3): From [DFRobot/DFRobot_BMV080](https://github.com/DFRobot/DFRobot_BMV080)
- **RISC-V .a files** (esp32c3, esp32c6): From [SparkFun/SparkFun_BMV080_Arduino_Library](https://github.com/sparkfun/SparkFun_BMV080_Arduino_Library)
- **SDK version**: 24.2.0 (as reported by `bmv080_get_driver_version`)

### API Surface

```c
// Lifecycle — must be called in this order
bmv080_get_driver_version(major*, minor*, patch*, git_hash, commits_ahead*) -> status
bmv080_open(handle*, sercom_handle, read_cb, write_cb, delay_cb) -> status
bmv080_reset(handle) -> status
bmv080_get_sensor_id(handle, id[13]) -> status

// Configuration — must be called BEFORE starting measurement
bmv080_set_parameter(handle, key, value*) -> status
bmv080_get_parameter(handle, key, value*) -> status

// Measurement control
bmv080_start_continuous_measurement(handle) -> status
bmv080_start_duty_cycling_measurement(handle, tick_cb, duty_cycling_mode) -> status
bmv080_stop_measurement(handle) -> status

// Data retrieval — MUST be called >= 1/sec during measurement
bmv080_serve_interrupt(handle, data_ready_cb, user_data*) -> status

// Cleanup
bmv080_close(handle*) -> status
```

### Callback Signatures

```c
// I2C communication — 16-bit word-oriented, all data MSB-first
typedef int8_t(*bmv080_callback_read_t)(sercom_handle, uint16_t header,
                                         uint16_t* payload, uint16_t length);
typedef int8_t(*bmv080_callback_write_t)(sercom_handle, uint16_t header,
                                          const uint16_t* payload, uint16_t length);

// Delay — called internally by SDK during init/reset
typedef int8_t(*bmv080_callback_delay_t)(uint32_t duration_in_ms);

// Tick — required for duty cycling mode
typedef uint32_t(*bmv080_callback_tick_t)(void);

// Data ready — called by bmv080_serve_interrupt when new PM data is available
typedef void(*bmv080_callback_data_ready_t)(bmv080_output_t output, void* user_data);
```

### Output Structure

```c
typedef struct {
    float runtime_in_sec;              // Time since measurement start (seconds)
    float pm2_5_mass_concentration;    // PM2.5 in ug/m3
    float pm1_mass_concentration;      // PM1.0 in ug/m3
    float pm10_mass_concentration;     // PM10 in ug/m3
    float pm2_5_number_concentration;  // PM2.5 in particles/cm3
    float pm1_number_concentration;    // PM1.0 in particles/cm3
    float pm10_number_concentration;   // PM10 in particles/cm3
    bool is_obstructed;                // Optical path obstruction detected
    bool is_outside_measurement_range; // PM2.5 outside 0..1000 ug/m3
    float reserved_0, reserved_1, reserved_2;
    struct bmv080_extended_info_s *extended_info;
} bmv080_output_t;
```

### I2C Protocol

The BMV080 uses a **non-standard 16-bit word-oriented I2C protocol**. Key differences
from typical I2C sensor communication:

- **16-bit words**: All transfers are in 16-bit words, not 8-bit bytes
- **Header shift**: For I2C mode, the header must be shifted left by 1: `header << 1`
- **MSB-first**: All data bytes are transmitted most-significant byte first
- **Burst reads**: Multi-word reads must be supported (read header, then N payload words)
- **No clock stretching**: BMV080 does not use clock stretching

**I2C Read Sequence:**
```
Master TX: [addr+W] [header_MSB] [header_LSB]
Master RX: [addr+R] [word0_MSB] [word0_LSB] ... [wordN_MSB] [wordN_LSB]
```

**I2C Write Sequence:**
```
Master TX: [addr+W] [header_MSB] [header_LSB] [word0_MSB] [word0_LSB] ... [wordN]
```

**I2C Addresses:** 0x54, 0x55, 0x56, 0x57 (selectable via address pins)
**Default:** 0x57 (SparkFun breakout default)
**Speeds:** 100 kHz, 400 kHz, 1 MHz (Fast-mode Plus)

### Configurable Parameters

Parameters must be set BEFORE starting measurement. Setting during measurement
returns `E_BMV080_ERROR_PARAM_LOCKED` (179).

| Parameter Key | Type | Default | Range | Description |
|--------------|------|---------|-------|-------------|
| `integration_time` | float | 10.0 | s | Measurement window duration. Also the ON time in duty cycle mode. |
| `duty_cycling_period` | uint16_t | 30 | s | Total period for duty cycling. Must exceed integration_time by >= 2s. |
| `do_obstruction_detection` | bool | true | | Enable optical path obstruction detection. |
| `do_vibration_filtering` | bool | false | | Enable vibration noise filtering. |
| `measurement_algorithm` | enum | 3 (HIGH_PRECISION) | 1-3 | 1=FAST_RESPONSE, 2=BALANCED, 3=HIGH_PRECISION. Duty cycle mode forces FAST_RESPONSE. |
| `volumetric_mass_density` | float | (default) | g/cm3 | Particle density assumption for mass calculation. |
| `distribution_id` | uint32_t | (default) | | Particle size distribution model. |

### Status Codes

| Code | Name | Meaning | Hint |
|------|------|---------|------|
| 0 | E_BMV080_OK | Success | |
| 1 | E_BMV080_WARNING_INVALID_REG_READ | Misuse in API integration | Review I2C implementation |
| 2 | E_BMV080_WARNING_INVALID_REG_WRITE | Misuse in API integration | Review I2C implementation |
| 3 | E_BMV080_WARNING_FIFO_READ | FIFO read issue | Review multi-word reads |
| 4 | E_BMV080_WARNING_FIFO_EVENTS_OVERFLOW | Too many events | Call serve_interrupt more often |
| 100 | E_BMV080_ERROR_NULLPTR | Null handle/pointer | Check pointer arguments |
| 105 | E_BMV080_ERROR_HW_READ | I2C read failed | Check I2C wiring/implementation |
| 106 | E_BMV080_ERROR_HW_WRITE | I2C write failed | Check I2C wiring/implementation |
| 107 | E_BMV080_ERROR_MISMATCH_CHIP_ID | Wrong chip ID | Check I2C address/wiring |
| 115 | E_BMV080_ERROR_PARAM_INVALID | Invalid parameter key | Check parameter name string |
| 123 | E_BMV080_ERROR_PARAM_INVALID_VALUE | Invalid parameter value | Check value range |
| 179 | E_BMV080_ERROR_PARAM_LOCKED | Param locked during measurement | Stop measurement first |
| 180 | E_BMV080_ERROR_PRECONDITION_UNSATISFIED | Wrong API call order | Follow init sequence |
| 403 | E_BMV080_ERROR_MEMORY_ALLOCATION | Memory/FIFO issue | Check I2C read implementation |

---

## ESPHome Component Design

### Python Files

#### `__init__.py` — Hub Component

Registers the `bmv080` hub component with ESPHome's code generation system.
Responsibilities:
- Defines the YAML config schema (mode, algorithm, timing, detection flags)
- Maps YAML enum strings to C++ enum values
- Generates C++ code to instantiate and configure `BMV080Component`
- Discovers Bosch SDK library paths by scanning `bosch/<architecture>/` subdirectories
- Adds PlatformIO build flags: `-I` for SDK headers, `-L` for library paths, `-l` for linking

**Linking strategy:** All architecture-specific `-L` paths are added simultaneously.
The linker automatically selects the compatible `.a` files and skips incompatible ones
(e.g., skips Xtensa `.a` when building for RISC-V). This is confirmed by linker output:
```
ld: skipping incompatible .../esp32/lib_bmv080.a when searching for -l_bmv080
```

#### `sensor.py` — Sensor Platform

Defines 7 optional sensor entities that can be individually enabled in YAML:
- PM1.0, PM2.5, PM10 mass concentrations (ug/m3, device classes: pm1, pm25, pm10)
- PM1.0, PM2.5, PM10 number concentrations (#/cm3, no device class)
- Runtime (seconds, diagnostic entity)

#### `binary_sensor.py` — Binary Sensor Platform

Defines 2 optional binary sensor entities:
- `obstructed` — optical path blocked (device_class: problem)
- `out_of_range` — PM2.5 outside 0-1000 ug/m3 range (device_class: problem)

### C++ Files

#### `bmv080_component.h` — Component Header

**Important naming note:** This file was originally named `bmv080.h` but was renamed to
`bmv080_component.h` to avoid a **header name collision** with the Bosch SDK's `bmv080.h`.
ESPHome copies component source files to a flat build directory, so both headers would
have been in the same include path. The SDK header is found via the `-I<bosch_dir>` flag.

The header declares `BMV080Component` which inherits from:
- `PollingComponent` — provides `setup()`, `loop()`, `update()` lifecycle with configurable interval
- `i2c::I2CDevice` — provides `read()`, `write()` methods and `address_` member

Key members:
- Static Bosch SDK callback functions (C-linkage compatible)
- FreeRTOS task function and handle
- Mutex for thread-safe data access
- Configuration values from YAML
- Sensor pointer storage

#### `bmv080.cpp` — Component Implementation

Contains all the runtime logic:
1. **I2C callbacks** (`read_cb_`, `write_cb_`) — Bridge between Bosch SDK's 16-bit word
   protocol and ESPHome's byte-oriented I2C methods
2. **Delay callback** (`delay_cb_`) — Uses `vTaskDelay()` instead of `delay()` since
   it runs in the FreeRTOS task context
3. **Data callback** (`data_ready_cb_`) — Receives PM data from SDK, stores under mutex
4. **FreeRTOS task** (`sensor_task_`) — Runs init sequence then loops `serve_interrupt`
5. **ESPHome lifecycle** (`setup`, `loop`, `update`, `dump_config`) — Creates task,
   checks for failures, publishes cached sensor data to Home Assistant

---

## ESPHome YAML Configuration

### Hub Config

```yaml
bmv080:
  id: bmv080_sensor                      # Required: component ID
  address: 0x57                          # I2C address (default 0x57)
  mode: continuous                       # continuous | duty_cycle
  measurement_algorithm: high_precision  # fast_response | balanced | high_precision
  integration_time: 10.0                 # seconds (1.0 - 300.0)
  duty_cycling_period: 30                # seconds (12 - 3600), duty_cycle mode only
  obstruction_detection: true            # enable optical path obstruction detection
  vibration_filtering: false             # enable vibration noise filtering
  update_interval: 5s                    # how often to publish to HA
```

### Sensor Config

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
```

### Binary Sensor Config

```yaml
binary_sensor:
  - platform: bmv080
    bmv080_id: bmv080_sensor
    obstructed:
      name: "Sensor Obstructed"
    out_of_range:
      name: "Out of Range"
```

---

## Design Decisions and Rationale

### 1. SDK Bundling (vs. package manager)

**Decision:** Bundle Bosch SDK headers and `.a` files directly in the repository.

**Rationale:** The Bosch BMV080 SDK does not have PlatformIO library packaging (unlike BSEC2
which uses `cg.add_library()`). The `.a` files are architecture-specific precompiled binaries.
Both DFRobot and SparkFun bundle them directly. Using build flags (`-I`, `-L`, `-l`) provides
a reliable, self-contained build.

### 2. FreeRTOS Task (vs. main loop)

**Decision:** Run all Bosch SDK operations in a dedicated FreeRTOS task with 64KB stack.

**Rationale:** The SDK requires ~60KB stack for `bmv080_serve_interrupt()`. The default
ESP32 main task stack is 8-16KB. Attempts to run in the main loop caused:
- Stack overflow crashes (device unreachable after OTA)
- API server blocked during 3-5s init sequence (device appeared dead)
- SparkFun's `SET_LOOP_TASK_STACK_SIZE(60*1024)` is Arduino-only and affects all components

### 3. 10-Second Startup Delay

**Decision:** The BMV080 task waits 10 seconds after boot before initializing the sensor.

**Rationale:** The BMV080 init sequence (`bmv080_open` + `bmv080_reset`) blocks for 3-5
seconds via delay callbacks. If started too early, the API server and OTA handler haven't
fully started, making the device unreachable. The 10-second delay ensures OTA recovery
is always possible, even if the sensor hardware has issues.

### 4. Header Renaming

**Decision:** Component header renamed from `bmv080.h` to `bmv080_component.h`.

**Rationale:** ESPHome copies component source files to a flat build directory. The
component's `bmv080.h` was shadowing the Bosch SDK's `bmv080.h`, causing "not declared
in this scope" errors for SDK functions. Renaming eliminates the collision while the SDK
header is found via the `-I<bosch_dir>` build flag.

### 5. ESP32-C3 RISC-V Support

**Decision:** Use ESP32-C6 `.a` files from SparkFun for ESP32-C3 builds.

**Rationale:** ESP32-C3 and ESP32-C6 share the same RISC-V `rv32imc` ISA. Bosch only
provides C6 libraries, but they are binary-compatible with C3. The linker confirms this
by selecting the C3/C6 `.a` files and skipping the incompatible Xtensa ones.

### 6. Mutex for Data Sharing

**Decision:** Use a FreeRTOS mutex to protect `last_output_` between threads.

**Rationale:** The `bmv080_output_t` struct contains multiple float fields. Without
synchronization, the main loop's `update()` could read a partially-updated struct from
the task thread's `data_ready_cb_()`. The mutex ensures atomic reads/writes.

---

## Test Results

### Test Environment (2026-03-14)

- **Board:** ESP32-C3-DevKitM-1 (RISC-V rv32imc, 1 core, 320KB RAM, 4MB flash)
- **Network:** W5500 SPI ethernet
- **Sensor:** SparkFun BMV080 breakout (I2C address 0x57, 2.2k pull-ups)
- **I2C:** GPIO5 (SDA), GPIO4 (SCL)
- **Framework:** ESP-IDF 5.5.2
- **ESPHome:** 2026.2.4

### Results

| Test | Result | Notes |
|------|--------|-------|
| Compile | PASS | 22s build time, links RISC-V .a files correctly |
| OTA Upload | PASS | 694KB firmware, 3s upload over ethernet |
| API Connectivity | PASS | API server stays responsive during BMV080 init |
| SDK Init | PASS | bmv080_open + reset + configure in ~3s |
| Driver Version | PASS | Reports SDK v24.2.0 |
| Sensor ID | PASS | 12-character ID retrieved |
| I2C Communication | PASS | Continuous TX/RX at 0x57, no errors |
| Data Acquisition | PASS | First reading at runtime=1.9s, then every ~1s |
| PM Mass Values | PASS | PM1=0-5, PM2.5=0-11, PM10=0-15 ug/m3 (clean air) |
| PM Number Values | PASS | ~24 #/cm3 during initial readings |
| Obstruction Detection | PASS | Reports `false` (clear path) |
| Out of Range | PASS | Reports `false` (normal air) |
| HA Publishing | PASS | All 9 entities publish correctly |
| Stability | PASS | 4+ minutes continuous operation, no crashes |
| Memory | PASS | RAM: 9.2%, Flash: 37.6% |

### Observed Timing

- Boot to API ready: ~2s
- BMV080 task start: boot + 10s (intentional delay)
- SDK init duration: ~3s (open + reset + configure + start)
- First PM data: ~1.9s after measurement start
- Subsequent data: every ~1.0s (continuous mode, 10s integration)
- update() publish interval: every 5s (configurable)

---

## Troubleshooting

### Device unreachable after OTA

The BMV080 SDK init is blocking and can prevent the API server from starting if
the 10-second delay is insufficient or if init hangs. The device should always
be pingable (ethernet/WiFi starts independently). If the API port (6053) is closed:

1. Power cycle the device
2. If persistent, increase the delay in `sensor_task_()` or disable the bmv080 component

### I2C errors (status 105/106)

Check:
- SDA/SCL wiring and pull-up resistors (2.2k-4.7k to 3.3V)
- I2C address matches hardware config (default 0x57)
- No other device on the same address
- I2C bus scan shows the device (`scan: true` in YAML)

### Stack overflow / crash

Should not occur with the FreeRTOS task approach. If it does:
- Check ESP32 has enough free heap for the 64KB task stack
- Monitor heap usage via debug sensors

### No PM data (serve_interrupt returns OK but no callbacks)

- Sensor may need warmup time (~2s after measurement start)
- Ensure `serve_interrupt` is called at least once per second
- Check that continuous measurement was started successfully

### FIFO overflow warnings (status 4)

The sensor is generating more events than can be processed. This can happen with
heavy particulate loads. Call `serve_interrupt` more frequently (reduce the 500ms
interval in the task loop).

---

## Known Issues / Future Work

- **ESP32-S2**: No prebuilt .a files available from Bosch/DFRobot/SparkFun
- **SPI support**: The Bosch SDK supports SPI but this component is I2C-only
- **Hardware IRQ**: The BMV080 has an interrupt pin for event-driven serving (not yet implemented)
- **Runtime parameter changes**: Parameters are set once during init. Could add HA services to stop/reconfigure/restart measurement
- **Arduino framework**: Untested (ESP-IDF verified). The build flags should work but the FreeRTOS task API calls may need guards
- **Multi-sensor**: MULTI_CONF=True in `__init__.py` but only one FreeRTOS task is created per instance. Multiple BMV080 sensors on different I2C addresses should work but are untested

---

## Reference Implementations Studied

### SparkFun BMV080 Arduino Library
- Repository: https://github.com/sparkfun/SparkFun_BMV080_Arduino_Library
- Uses SparkFun Toolkit bus abstraction (`sfTkIBus`)
- Static callbacks with bus pointer as `sercom_handle`
- Requires `SET_LOOP_TASK_STACK_SIZE(60 * 1024)` for ESP32
- I2C header shift: `if (theBus->type() == ksfTkBusTypeI2C) header = header << 1`
- `precompiled=true` + `ldflags=-l_bmv080 -l_postProcessor` in library.properties

### DFRobot BMV080 Library
- Repository: https://github.com/DFRobot/DFRobot_BMV080
- Simpler: casts `this` as `bmv080_sercom_handle_t` directly
- Manual byte-swapping with `DFRobot_swap16()` for MSB-first
- Bundles SDK headers and .a files in repo (our approach follows this)
- I2C reads chunked to 32 bytes via `Wire.requestFrom()`

### ESPHome BSEC2 Component
- Path: esphome/components/bme68x_bsec2/
- Closest ESPHome precedent for Bosch prebuilt library integration
- Uses `cg.add_library()` (BSEC2 has proper PlatformIO packaging — BMV080 does not)
- Pattern: PollingComponent with `setup()`, `loop()`, `dump_config()`

---

## Build & Test Commands

```bash
# Compile only (no upload)
esphome compile native-esp-w5500.yaml

# Compile + OTA upload + watch logs
esphome run native-esp-w5500.yaml

# Watch logs only (device already running)
esphome logs native-esp-w5500.yaml
```
