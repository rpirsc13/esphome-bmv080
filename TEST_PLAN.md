# BMV080 ESPHome Component Test Plan

## Test 1: Compile
- [ ] `esphome compile native-esp-w5500.yaml` succeeds with no errors
- [ ] Bosch SDK headers found and included
- [ ] Bosch `.a` libraries linked (esp32c3 RISC-V)
- [ ] No undefined symbol errors from lib_bmv080 or lib_postProcessor

## Test 2: OTA Upload
- [ ] `esphome run native-esp-w5500.yaml` uploads over network (OTA)
- [ ] Device reboots and reconnects

## Test 3: Boot Sequence (from logs)
- [ ] I2C bus initializes on GPIO5 (SDA) / GPIO4 (SCL)
- [ ] I2C scan detects device at address 0x57
- [ ] `bmv080_get_driver_version` succeeds — logs SDK version (e.g. "11.x.x")
- [ ] `bmv080_open` succeeds — logs handle address
- [ ] `bmv080_reset` succeeds
- [ ] `bmv080_get_sensor_id` succeeds — logs 12-char sensor ID
- [ ] Parameters configured: integration_time, obstruction_detection, vibration_filtering, measurement_algorithm
- [ ] `bmv080_start_continuous_measurement` succeeds
- [ ] dump_config shows all configured sensors and settings

## Test 4: Data Acquisition
- [ ] `bmv080_serve_interrupt` called every ~500ms (visible at VERBOSE log level)
- [ ] First `data_ready_cb` fires within ~2-3 seconds of measurement start
- [ ] Subsequent data callbacks arrive approximately every ~1 second
- [ ] `bmv080_output_t` values are plausible:
  - PM1 mass: 0-1000 ug/m3 (typical indoor: 1-30)
  - PM2.5 mass: 0-1000 ug/m3 (typical indoor: 2-50)
  - PM10 mass: 0-1000 ug/m3 (typical indoor: 5-80)
  - Number concentrations: positive floats
  - runtime_in_sec: increasing monotonically
  - is_obstructed: false (assuming clear path)
  - is_outside_measurement_range: false (assuming normal air)

## Test 5: Sensor Publishing
- [ ] PM 1.0 sensor publishes to Home Assistant
- [ ] PM 2.5 sensor publishes to Home Assistant
- [ ] PM 10 sensor publishes to Home Assistant
- [ ] PM 1.0 Count sensor publishes
- [ ] PM 2.5 Count sensor publishes
- [ ] PM 10 Count sensor publishes
- [ ] Runtime sensor publishes (increasing value)
- [ ] Obstructed binary sensor publishes (OFF expected)
- [ ] Out of Range binary sensor publishes (OFF expected)

## Test 6: Stability
- [ ] No stack overflow / watchdog resets after 5+ minutes
- [ ] No I2C errors accumulating in logs
- [ ] Loop time stays reasonable (debug sensor reports <100ms)
- [ ] Heap usage stable (not continuously growing)

## Failure Modes to Watch For
| Symptom | Likely Cause |
|---------|-------------|
| Stack overflow / crash | 60KB stack needed — need FreeRTOS task |
| `bmv080_open` returns 105/106 | I2C read/write callback broken |
| `bmv080_open` returns 107 | Chip ID mismatch — wrong address or wiring |
| No data callbacks | `serve_interrupt` not called frequently enough |
| All PM values = 0 | Sensor needs warmup time (~2s) |
| `status=180` | API called in wrong order |
| I2C timeout errors | SDA/SCL wiring, pull-ups, or clock speed |
