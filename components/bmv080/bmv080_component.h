/**
 * @file bmv080_component.h
 * @brief ESPHome component header for the Bosch BMV080 particulate matter sensor.
 *
 * IMPORTANT: This file is named bmv080_component.h (not bmv080.h) to avoid a header
 * name collision with the Bosch SDK's bmv080.h. ESPHome copies component source files
 * to a flat build directory, so both would share the same include path. The SDK header
 * is found via the -I<bosch_dir> build flag added by __init__.py.
 *
 * Architecture Overview:
 *   The component uses a two-thread design:
 *
 *   1. Main Thread (ESPHome loop, ~8KB stack):
 *      - setup(): Creates mutex and launches the FreeRTOS task
 *      - loop(): Checks for task failure and marks component as failed
 *      - update(): Reads cached sensor data (under mutex) and publishes to HA
 *
 *   2. BMV080 Task Thread (dedicated, 64KB stack):
 *      - Waits 10 seconds for network/API to start
 *      - Runs SDK init sequence (bmv080_open, reset, configure, start)
 *      - Loops bmv080_serve_interrupt() every 500ms
 *      - Data arrives via data_ready_cb_ -> stored under mutex
 *
 *   The 64KB stack is required because the Bosch SDK's bmv080_serve_interrupt()
 *   uses approximately 60KB of stack space internally.
 *
 * Thread Safety:
 *   - last_output_ and data_available_ are protected by data_mutex_
 *   - sensor_initialized_ and sensor_failed_ are volatile (single-writer pattern)
 *   - Configuration values are read-only after setup(), no synchronization needed
 *   - ESPHome I2C operations (comp->read/write) are thread-safe in ESP-IDF mode
 *     because the I2C driver has internal mutex locking
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/i2c/i2c.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

// Bosch SDK headers — resolved via -I<bosch_dir> build flag
#include "bmv080_defs.h"
#include "bmv080.h"

// FreeRTOS headers for dedicated task and mutex
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace esphome {
namespace bmv080 {

/**
 * @brief Measurement mode for the BMV080 sensor.
 *
 * - CONTINUOUS: Sensor runs constantly, producing data every ~1 second.
 *   Best for real-time monitoring. Higher power consumption.
 *
 * - DUTY_CYCLE: Sensor cycles between ON and sleep states.
 *   ON time = integration_time, total period = duty_cycling_period.
 *   Lower power consumption, data only at end of each integration window.
 *   Forces FAST_RESPONSE algorithm regardless of measurement_algorithm setting.
 */
enum MeasurementMode {
  MEASUREMENT_MODE_CONTINUOUS = 0,
  MEASUREMENT_MODE_DUTY_CYCLE = 1,
};

/**
 * @brief Measurement algorithm choice affecting precision vs. response time.
 *
 * These map directly to the Bosch SDK's bmv080_measurement_algorithm_t enum.
 * Values 1-3 match E_BMV080_MEASUREMENT_ALGORITHM_FAST_RESPONSE through
 * E_BMV080_MEASUREMENT_ALGORITHM_HIGH_PRECISION.
 *
 * - FAST_RESPONSE (1): Quickest updates, lower accuracy. Required for duty cycle mode.
 * - BALANCED (2):      Middle ground between speed and accuracy.
 * - HIGH_PRECISION (3): Slowest updates, highest accuracy. Default.
 */
enum MeasurementAlgorithm {
  ALGORITHM_FAST_RESPONSE = 1,
  ALGORITHM_BALANCED = 2,
  ALGORITHM_HIGH_PRECISION = 3,
};

/**
 * @brief ESPHome component for the Bosch BMV080 particulate matter sensor.
 *
 * Inherits from PollingComponent (provides setup/loop/update lifecycle with
 * configurable update_interval) and I2CDevice (provides read/write/address_).
 *
 * Usage:
 *   Instantiated and configured by __init__.py code generation.
 *   Sensors registered by sensor.py and binary_sensor.py.
 *   All heavy lifting (SDK init + data acquisition) runs in a separate
 *   FreeRTOS task to avoid blocking the main ESPHome loop.
 */
class BMV080Component : public PollingComponent, public i2c::I2CDevice {
 public:
  // --- ESPHome Component Lifecycle ---

  /** @brief Creates mutex and launches the dedicated FreeRTOS task. */
  void setup() override;

  /** @brief Checks if the background task reported initialization failure. */
  void loop() override;

  /** @brief Reads cached sensor data and publishes to Home Assistant. */
  void update() override;

  /** @brief Logs component configuration at startup. */
  void dump_config() override;

  /**
   * @brief Returns LATE setup priority to run after network components.
   *
   * This ensures the ethernet/WiFi and API server are initialized before
   * the BMV080 component's setup() runs. Combined with the 10-second delay
   * in the FreeRTOS task, this guarantees OTA accessibility even if the
   * BMV080 sensor has issues.
   */
  float get_setup_priority() const override { return setup_priority::LATE; }

  // --- Configuration Setters (called by generated code from __init__.py) ---

  void set_mode(MeasurementMode mode) { this->mode_ = mode; }
  void set_measurement_algorithm(MeasurementAlgorithm algo) { this->algorithm_ = algo; }
  void set_integration_time(float time) { this->integration_time_ = time; }
  void set_duty_cycling_period(uint16_t period) { this->duty_cycling_period_ = period; }
  void set_obstruction_detection(bool enabled) { this->obstruction_detection_ = enabled; }
  void set_vibration_filtering(bool enabled) { this->vibration_filtering_ = enabled; }

  // --- Sensor Setters (called by generated code from sensor.py / binary_sensor.py) ---

  void set_pm_1_0_sensor(sensor::Sensor *sensor) { this->pm_1_0_sensor_ = sensor; }
  void set_pm_2_5_sensor(sensor::Sensor *sensor) { this->pm_2_5_sensor_ = sensor; }
  void set_pm_10_sensor(sensor::Sensor *sensor) { this->pm_10_sensor_ = sensor; }
  void set_pm_1_0_count_sensor(sensor::Sensor *sensor) { this->pm_1_0_count_sensor_ = sensor; }
  void set_pm_2_5_count_sensor(sensor::Sensor *sensor) { this->pm_2_5_count_sensor_ = sensor; }
  void set_pm_10_count_sensor(sensor::Sensor *sensor) { this->pm_10_count_sensor_ = sensor; }
  void set_runtime_sensor(sensor::Sensor *sensor) { this->runtime_sensor_ = sensor; }
  void set_obstructed_binary_sensor(binary_sensor::BinarySensor *sensor) { this->obstructed_sensor_ = sensor; }
  void set_out_of_range_binary_sensor(binary_sensor::BinarySensor *sensor) { this->out_of_range_sensor_ = sensor; }

  /**
   * @brief Stores new sensor data from the Bosch SDK callback.
   *
   * Called from data_ready_cb_() in the FreeRTOS task thread.
   * Updates last_output_ and sets data_available_ under mutex protection.
   *
   * @param output The sensor output structure from the Bosch SDK.
   */
  void on_data_ready(bmv080_output_t output);

 protected:
  // --- Bosch SDK Static Callbacks ---
  // These are static functions with C-compatible signatures, required by the
  // Bosch SDK's callback-based architecture. They receive the component pointer
  // as bmv080_sercom_handle_t (cast to void*) and cast it back to access
  // ESPHome's I2C methods.

  /**
   * @brief I2C read callback for the Bosch SDK.
   *
   * Translates the SDK's 16-bit word-oriented read protocol into ESPHome I2C calls:
   * 1. Shifts header left by 1 for I2C mode (BMV080 protocol requirement)
   * 2. Writes 2-byte header as a separate I2C transaction
   * 3. Reads payload bytes in 32-byte chunks (I2C transaction size limit)
   * 4. Reassembles bytes into uint16_t words (MSB-first)
   */
  static int8_t read_cb_(bmv080_sercom_handle_t handle, uint16_t header,
                         uint16_t *payload, uint16_t payload_length);

  /**
   * @brief I2C write callback for the Bosch SDK.
   *
   * Translates the SDK's 16-bit word-oriented write protocol into ESPHome I2C calls:
   * 1. Shifts header left by 1 for I2C mode
   * 2. Builds a single buffer with 2-byte header + all payload bytes (MSB-first)
   * 3. Sends the entire buffer in one I2C write transaction
   */
  static int8_t write_cb_(bmv080_sercom_handle_t handle, uint16_t header,
                          const uint16_t *payload, uint16_t payload_length);

  /**
   * @brief Delay callback for the Bosch SDK.
   *
   * Uses vTaskDelay() instead of delay() since this runs in the FreeRTOS task
   * context. vTaskDelay yields to the scheduler, allowing other tasks (including
   * the main loop) to continue running during SDK delay operations.
   */
  static int8_t delay_cb_(uint32_t duration_ms);

  /**
   * @brief Data ready callback invoked by bmv080_serve_interrupt().
   *
   * Called from the FreeRTOS task thread when the SDK has processed sensor data
   * and has a new bmv080_output_t available. Logs the values and forwards to
   * on_data_ready() for mutex-protected storage.
   */
  static void data_ready_cb_(bmv080_output_t output, void *user_data);

  /**
   * @brief Tick callback returning milliseconds since boot.
   *
   * Required by the SDK for duty cycling mode timing. Returns millis() which
   * wraps at ~49 days. Not used in continuous mode.
   */
  static uint32_t tick_cb_();

  /**
   * @brief FreeRTOS task function that runs all BMV080 SDK operations.
   *
   * This function runs in a dedicated task with 64KB stack. Sequence:
   * 1. Wait 10 seconds for the network stack and API server to start
   * 2. Initialize sensor (bmv080_open, reset, get_sensor_id)
   * 3. Configure parameters (integration_time, algorithm, etc.)
   * 4. Start measurement (continuous or duty cycle)
   * 5. Loop forever: call bmv080_serve_interrupt every 500ms
   *
   * If any step fails, sets sensor_failed_ = true and deletes the task.
   * The main loop's loop() checks this flag and calls mark_failed().
   */
  static void sensor_task_(void *arg);

  // --- Internal Helper Methods ---
  // These are called from the FreeRTOS task thread.

  /** @brief Opens the SDK handle, resets the sensor, reads the sensor ID. */
  bool init_sensor_();

  /** @brief Sets all configurable parameters via bmv080_set_parameter(). */
  bool configure_parameters_();

  /** @brief Starts continuous or duty-cycle measurement. */
  bool start_measurement_();

  /** @brief Calls bmv080_serve_interrupt() and logs the result. */
  void service_sensor_();

  // --- Member Variables ---

  /** Opaque handle to the Bosch SDK sensor instance. Created by bmv080_open(). */
  bmv080_handle_t handle_{nullptr};

  // Configuration values (set once during code generation, read-only at runtime)
  MeasurementMode mode_{MEASUREMENT_MODE_CONTINUOUS};
  MeasurementAlgorithm algorithm_{ALGORITHM_HIGH_PRECISION};
  float integration_time_{10.0f};          ///< Measurement window in seconds
  uint16_t duty_cycling_period_{30};       ///< Total duty cycle period in seconds
  bool obstruction_detection_{true};       ///< Enable obstruction detection
  bool vibration_filtering_{false};        ///< Enable vibration noise filtering

  // Shared state between FreeRTOS task and main loop (mutex-protected)
  bmv080_output_t last_output_{};          ///< Most recent sensor reading
  volatile bool data_available_{false};    ///< True when new data is ready
  volatile bool sensor_initialized_{false}; ///< True after successful init
  volatile bool sensor_failed_{false};     ///< True if task-side init failed
  SemaphoreHandle_t data_mutex_{nullptr};  ///< Mutex protecting last_output_

  // FreeRTOS task management
  TaskHandle_t task_handle_{nullptr};      ///< Handle to the dedicated sensor task

  // Sensor pointers (nullptr if not configured in YAML)
  sensor::Sensor *pm_1_0_sensor_{nullptr};        ///< PM1.0 mass concentration
  sensor::Sensor *pm_2_5_sensor_{nullptr};        ///< PM2.5 mass concentration
  sensor::Sensor *pm_10_sensor_{nullptr};         ///< PM10 mass concentration
  sensor::Sensor *pm_1_0_count_sensor_{nullptr};  ///< PM1.0 number concentration
  sensor::Sensor *pm_2_5_count_sensor_{nullptr};  ///< PM2.5 number concentration
  sensor::Sensor *pm_10_count_sensor_{nullptr};   ///< PM10 number concentration
  sensor::Sensor *runtime_sensor_{nullptr};       ///< Measurement runtime
  binary_sensor::BinarySensor *obstructed_sensor_{nullptr};   ///< Obstruction flag
  binary_sensor::BinarySensor *out_of_range_sensor_{nullptr}; ///< Out-of-range flag
};

}  // namespace bmv080
}  // namespace esphome
