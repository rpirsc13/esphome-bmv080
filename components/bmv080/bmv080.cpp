/**
 * @file bmv080.cpp
 * @brief ESPHome component implementation for the Bosch BMV080 particulate matter sensor.
 *
 * This file contains the complete runtime implementation including:
 *
 * 1. Bosch SDK I2C Callbacks:
 *    read_cb_()  — Translates SDK 16-bit word reads to ESPHome I2C byte operations
 *    write_cb_() — Translates SDK 16-bit word writes to ESPHome I2C byte operations
 *    delay_cb_() — Provides blocking delay using FreeRTOS vTaskDelay
 *    data_ready_cb_() — Receives PM data from SDK and caches it
 *    tick_cb_() — Provides millisecond tick for duty cycling mode
 *
 * 2. FreeRTOS Sensor Task:
 *    sensor_task_() — Dedicated task with 64KB stack for all SDK operations
 *
 * 3. ESPHome Component Lifecycle:
 *    setup()       — Creates mutex and launches FreeRTOS task
 *    loop()        — Checks for task failure
 *    update()      — Publishes cached sensor data to Home Assistant
 *    dump_config() — Logs configuration at startup
 *
 * Threading Model:
 *    The Bosch SDK requires ~60KB stack for bmv080_serve_interrupt(). The default
 *    ESP32 main task has only 8-16KB. Additionally, the SDK init sequence blocks
 *    for 3-5 seconds, which would prevent the API server from accepting connections.
 *
 *    Solution: All SDK calls run in a dedicated FreeRTOS task. The main loop only
 *    reads cached data (under mutex) and publishes to Home Assistant.
 *
 * I2C Protocol:
 *    The BMV080 uses a non-standard 16-bit word-oriented I2C protocol:
 *    - Header must be shifted left by 1 for I2C mode: (header << 1)
 *    - All data is MSB-first (most significant byte transmitted first)
 *    - Reads: write 2-byte header, then read N*2 bytes of payload
 *    - Writes: write 2-byte header + N*2 bytes of payload in one transaction
 *    - Reads are chunked to 32 bytes per I2C transaction
 */

#include "bmv080_component.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esphome/core/application.h"

namespace esphome {
namespace bmv080 {

/// Log tag for all BMV080 component messages
static const char *const TAG = "bmv080";

// =============================================================================
// Bosch SDK Static Callbacks
// =============================================================================
//
// The Bosch SDK uses a callback pattern for hardware abstraction. During
// bmv080_open(), we pass:
//   - (bmv080_sercom_handle_t) this  ->  stored as opaque void* by SDK
//   - read_cb_, write_cb_, delay_cb_ ->  called by SDK for I2C and timing
//
// When the SDK calls these callbacks, it passes back the sercom_handle, which
// we cast back to BMV080Component* to access ESPHome's I2C methods. This is
// the same pattern used by DFRobot and SparkFun in their Arduino libraries.
// =============================================================================

/**
 * @brief I2C read callback for the Bosch SDK.
 *
 * The SDK calls this to read a register or data block from the BMV080. The protocol:
 * 1. Write a 2-byte header (with I2C mode shift applied) as a separate I2C write
 * 2. Read payload_length * 2 bytes from the sensor
 * 3. Reassemble raw bytes into uint16_t words (MSB-first byte order)
 *
 * The header shift (header << 1) is required by the BMV080 I2C protocol. In SPI mode,
 * the header is used as-is. Since we're I2C-only, we always apply the shift.
 *
 * Reads are chunked to 32 bytes per I2C transaction, matching typical I2C buffer limits.
 *
 * @param handle The sercom handle (actually a BMV080Component pointer)
 * @param header The 16-bit register/command header from the SDK
 * @param payload Output buffer for uint16_t words to be read
 * @param payload_length Number of 16-bit words to read
 * @return 0 on success, negative on error
 */
int8_t BMV080Component::read_cb_(bmv080_sercom_handle_t handle, uint16_t header,
                                  uint16_t *payload, uint16_t payload_length) {
  if (handle == nullptr) {
    ESP_LOGE(TAG, "read_cb: null handle");
    return -1;
  }

  // Cast the opaque sercom handle back to our component pointer
  auto *comp = (BMV080Component *) handle;

  // Apply I2C mode header shift — BMV080 protocol requirement
  // In I2C mode, the header must be shifted left by 1 bit
  uint16_t i2c_header = header << 1;

  ESP_LOGVV(TAG, "read_cb: header=0x%04X i2c_header=0x%04X len=%u", header, i2c_header, payload_length);

  // Prepare 2-byte header for I2C write (MSB-first)
  uint8_t header_bytes[2];
  header_bytes[0] = (uint8_t) (i2c_header >> 8);   // High byte
  header_bytes[1] = (uint8_t) (i2c_header & 0xFF);  // Low byte

  // Step 1: Write the 2-byte header to tell the sensor what to read
  auto err = comp->write(header_bytes, 2);
  if (err != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "read_cb: failed to write header, i2c error=%d", (int) err);
    return -2;
  }

  // Step 2: Read payload bytes from the sensor
  // Each uint16_t word = 2 bytes, so total bytes = payload_length * 2
  size_t byte_count = payload_length * 2;
  uint8_t buffer[512];  // Stack buffer for raw bytes (max 256 words)

  if (byte_count > sizeof(buffer)) {
    ESP_LOGE(TAG, "read_cb: payload too large: %u bytes", byte_count);
    return -2;
  }

  // Read in chunks of 32 bytes — I2C typically has a per-transaction limit.
  // The ESP-IDF I2C driver can handle larger reads, but chunking ensures
  // compatibility across frameworks and bus configurations.
  size_t bytes_read = 0;
  while (bytes_read < byte_count) {
    size_t chunk = byte_count - bytes_read;
    if (chunk > 32)
      chunk = 32;
    err = comp->read(buffer + bytes_read, chunk);
    if (err != i2c::ERROR_OK) {
      ESP_LOGE(TAG, "read_cb: failed to read chunk at offset %u, i2c error=%d", bytes_read, (int) err);
      return -2;
    }
    bytes_read += chunk;
  }

  // Step 3: Reassemble raw bytes into uint16_t words (MSB-first byte order)
  // Each pair of consecutive bytes forms one word: [MSB][LSB] -> (MSB << 8) | LSB
  for (uint16_t i = 0; i < payload_length; i++) {
    payload[i] = ((uint16_t) buffer[i * 2] << 8) | buffer[i * 2 + 1];
  }

  return 0;  // Success
}

/**
 * @brief I2C write callback for the Bosch SDK.
 *
 * The SDK calls this to write a register or data block to the BMV080. The protocol:
 * 1. Build a single buffer containing: [header_MSB][header_LSB][word0_MSB][word0_LSB]...
 * 2. Send the entire buffer in one I2C write transaction
 *
 * Unlike reads (which use separate header write + payload read transactions),
 * writes send everything in a single I2C transaction for atomicity.
 *
 * @param handle The sercom handle (actually a BMV080Component pointer)
 * @param header The 16-bit register/command header from the SDK
 * @param payload Input buffer of uint16_t words to write
 * @param payload_length Number of 16-bit words to write
 * @return 0 on success, negative on error
 */
int8_t BMV080Component::write_cb_(bmv080_sercom_handle_t handle, uint16_t header,
                                   const uint16_t *payload, uint16_t payload_length) {
  if (handle == nullptr) {
    ESP_LOGE(TAG, "write_cb: null handle");
    return -1;
  }

  auto *comp = (BMV080Component *) handle;

  // Apply I2C mode header shift
  uint16_t i2c_header = header << 1;

  ESP_LOGVV(TAG, "write_cb: header=0x%04X i2c_header=0x%04X len=%u", header, i2c_header, payload_length);

  // Build a single contiguous buffer: 2 header bytes + payload bytes (MSB-first)
  size_t total_bytes = 2 + payload_length * 2;
  uint8_t buffer[512];

  if (total_bytes > sizeof(buffer)) {
    ESP_LOGE(TAG, "write_cb: payload too large: %u bytes", total_bytes);
    return -3;
  }

  // Header bytes (MSB-first)
  buffer[0] = (uint8_t) (i2c_header >> 8);
  buffer[1] = (uint8_t) (i2c_header & 0xFF);

  // Payload words -> bytes (MSB-first)
  for (uint16_t i = 0; i < payload_length; i++) {
    buffer[2 + i * 2] = (uint8_t) (payload[i] >> 8);
    buffer[2 + i * 2 + 1] = (uint8_t) (payload[i] & 0xFF);
  }

  // Send everything in one I2C write transaction
  auto err = comp->write(buffer, total_bytes);
  if (err != i2c::ERROR_OK) {
    ESP_LOGE(TAG, "write_cb: i2c write failed, error=%d, total_bytes=%u", (int) err, total_bytes);
    return -3;
  }

  return 0;  // Success
}

/**
 * @brief Delay callback for the Bosch SDK.
 *
 * Called internally by the SDK during init/reset operations. Uses vTaskDelay()
 * instead of Arduino's delay() since this runs in the FreeRTOS task context.
 * vTaskDelay properly yields to the RTOS scheduler, allowing the main loop
 * thread (and thus the API server) to continue running during SDK delays.
 *
 * @param duration_ms Delay duration in milliseconds
 * @return 0 (always succeeds)
 */
int8_t BMV080Component::delay_cb_(uint32_t duration_ms) {
  ESP_LOGVV(TAG, "delay_cb: %u ms", duration_ms);
  vTaskDelay(pdMS_TO_TICKS(duration_ms));
  return 0;
}

/**
 * @brief Data ready callback invoked by bmv080_serve_interrupt().
 *
 * The SDK calls this function (from within bmv080_serve_interrupt) whenever
 * new PM data has been processed. In continuous mode, this fires approximately
 * once per second. In duty cycle mode, it fires once per duty_cycling_period.
 *
 * Note: This runs in the FreeRTOS task thread, NOT the main loop thread.
 * Data is passed to on_data_ready() which stores it under mutex protection
 * for later retrieval by update() in the main loop.
 *
 * @param output The sensor output containing all PM data
 * @param user_data Pointer to the BMV080Component instance
 */
void BMV080Component::data_ready_cb_(bmv080_output_t output, void *user_data) {
  auto *comp = (BMV080Component *) user_data;
  ESP_LOGD(TAG, "Data ready callback: PM1=%.1f PM2.5=%.1f PM10=%.1f ug/m3, runtime=%.1fs, obstructed=%s, oor=%s",
           output.pm1_mass_concentration,
           output.pm2_5_mass_concentration,
           output.pm10_mass_concentration,
           output.runtime_in_sec,
           output.is_obstructed ? "YES" : "no",
           output.is_outside_measurement_range ? "YES" : "no");
  comp->on_data_ready(output);
}

/**
 * @brief Tick callback returning milliseconds since boot.
 *
 * Required by the SDK for duty cycling mode. The SDK uses this to track
 * elapsed time for ON/OFF cycle management. In continuous mode, this
 * callback is not used but must still be available.
 *
 * @return Current tick count in milliseconds (wraps at ~49 days)
 */
uint32_t BMV080Component::tick_cb_() { return millis(); }

// =============================================================================
// Data Transfer (task thread -> main loop thread)
// =============================================================================

/**
 * @brief Stores new sensor data from the SDK callback (task thread).
 *
 * Called from data_ready_cb_() which runs in the FreeRTOS task thread.
 * Uses mutex to ensure the main loop thread's update() never reads a
 * partially-written bmv080_output_t struct.
 *
 * @param output The complete sensor output from the Bosch SDK
 */
void BMV080Component::on_data_ready(bmv080_output_t output) {
  if (this->data_mutex_ != nullptr) {
    xSemaphoreTake(this->data_mutex_, portMAX_DELAY);
  }
  this->last_output_ = output;
  this->data_available_ = true;
  if (this->data_mutex_ != nullptr) {
    xSemaphoreGive(this->data_mutex_);
  }
}

// =============================================================================
// ESPHome Component Lifecycle
// =============================================================================

/**
 * @brief Component setup — creates mutex and launches the FreeRTOS sensor task.
 *
 * This is intentionally lightweight. The heavy SDK initialization is deferred
 * to the FreeRTOS task, which waits 10 seconds before starting. This ensures:
 * 1. The API server is fully started and accepting connections
 * 2. OTA updates can be received even if the BMV080 has issues
 * 3. The main loop is never blocked by SDK operations
 */
void BMV080Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up BMV080...");

  // Create mutex for thread-safe access to last_output_ between the
  // FreeRTOS task (writer) and the main loop update() (reader)
  this->data_mutex_ = xSemaphoreCreateMutex();
  if (this->data_mutex_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create data mutex");
    this->mark_failed();
    return;
  }

  // Launch the dedicated FreeRTOS task with 64KB stack.
  //
  // Why 64KB? The Bosch SDK's bmv080_serve_interrupt() uses approximately
  // 60KB of stack space internally. The default ESP32 main task has only
  // 8-16KB, which causes immediate stack overflow and crashes.
  //
  // SparkFun's Arduino library works around this with
  // SET_LOOP_TASK_STACK_SIZE(60*1024), but that's Arduino-only and affects
  // ALL code running in the main loop. A dedicated task isolates the
  // stack requirement to just the BMV080 SDK operations.
  //
  // Task parameters:
  //   Priority 1: Just above idle, won't preempt the main loop
  //   Core 0: Pinned to core 0 (ESP32-C3 only has one core anyway)
  BaseType_t ret = xTaskCreatePinnedToCore(
      sensor_task_,        // Task entry function
      "bmv080",            // Task name (for debugging)
      64 * 1024,           // Stack size: 64KB
      this,                // Parameter: pointer to this component
      1,                   // Priority: low (just above idle)
      &this->task_handle_, // Output: task handle for management
      0                    // Core: pin to core 0
  );

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create BMV080 task (not enough memory for 64KB stack?)");
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "BMV080 task created with 64KB stack, will initialize in ~10 seconds");
}

// =============================================================================
// FreeRTOS Sensor Task
// =============================================================================

/**
 * @brief FreeRTOS task function — runs all BMV080 SDK operations.
 *
 * This function runs in its own task with 64KB stack, completely independent
 * from the ESPHome main loop. Sequence:
 *
 * 1. Wait 10 seconds: Let the network stack, API server, and OTA handler
 *    fully initialize. This ensures the device is always OTA-recoverable.
 *
 * 2. Initialize sensor: bmv080_open() + bmv080_reset() + bmv080_get_sensor_id()
 *    This takes ~3 seconds due to SDK delay callbacks during sensor probing.
 *
 * 3. Configure parameters: Set integration_time, algorithm, detection flags
 *    via bmv080_set_parameter(). Must be done BEFORE starting measurement.
 *
 * 4. Start measurement: bmv080_start_continuous_measurement() or
 *    bmv080_start_duty_cycling_measurement() depending on config.
 *
 * 5. Service loop: Call bmv080_serve_interrupt() every 500ms forever.
 *    This reads the sensor's FIFO and triggers data_ready callbacks.
 *    Must be called at least once per second per SDK requirements.
 *
 * If any step fails, sets sensor_failed_ = true and deletes the task.
 * The main loop's loop() detects this and calls mark_failed().
 *
 * @param arg Pointer to the BMV080Component instance
 */
void BMV080Component::sensor_task_(void *arg) {
  auto *comp = (BMV080Component *) arg;

  // --- Step 1: Startup Delay ---
  // Wait for the network stack and API server to fully start.
  // Without this delay, the SDK init (which blocks for 3-5 seconds)
  // would prevent the API server from accepting connections, making
  // the device appear dead after an OTA update.
  ESP_LOGI(TAG, "BMV080 task started, waiting 10 seconds for system startup...");
  vTaskDelay(pdMS_TO_TICKS(10000));

  // --- Step 2: Initialize Sensor ---
  ESP_LOGI(TAG, "Starting BMV080 initialization...");

  if (!comp->init_sensor_()) {
    ESP_LOGE(TAG, "Failed to initialize BMV080 sensor");
    comp->sensor_failed_ = true;
    vTaskDelete(nullptr);  // Delete this task
    return;
  }

  // --- Step 3: Configure Parameters ---
  if (!comp->configure_parameters_()) {
    ESP_LOGE(TAG, "Failed to configure BMV080 parameters");
    comp->sensor_failed_ = true;
    vTaskDelete(nullptr);
    return;
  }

  // --- Step 4: Start Measurement ---
  if (!comp->start_measurement_()) {
    ESP_LOGE(TAG, "Failed to start BMV080 measurement");
    comp->sensor_failed_ = true;
    vTaskDelete(nullptr);
    return;
  }

  // Signal to the main loop that the sensor is ready
  comp->sensor_initialized_ = true;
  ESP_LOGI(TAG, "BMV080 initialized and measurement started successfully");

  // --- Step 5: Service Loop ---
  // Call bmv080_serve_interrupt() every 500ms. The SDK requires this to be
  // called at least once per second during measurement. Calling at 500ms
  // provides comfortable margin.
  //
  // Each call reads the sensor's FIFO buffer. If PM data is ready (in
  // continuous mode, approximately once per second), the SDK invokes
  // data_ready_cb_() with the new output. If data isn't ready yet,
  // serve_interrupt returns E_BMV080_OK without calling the callback.
  while (true) {
    comp->service_sensor_();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// =============================================================================
// SDK Initialization Helpers (run in FreeRTOS task thread)
// =============================================================================

/**
 * @brief Opens the SDK handle, resets the sensor, and reads the sensor ID.
 *
 * SDK Calling Sequence:
 *   1. bmv080_get_driver_version() — works without a handle
 *   2. bmv080_open() — creates the SDK handle, verifies communication
 *   3. bmv080_reset() — hardware + software reset
 *   4. bmv080_get_sensor_id() — reads 12-character unique ID
 *
 * bmv080_open() is the first function that performs I2C communication.
 * It verifies the chip ID and configures internal SDK state. This
 * typically takes 2-3 seconds due to delay callbacks.
 *
 * @return true if all steps succeeded, false on any error
 */
bool BMV080Component::init_sensor_() {
  // --- Get SDK driver version (no handle/communication needed) ---
  uint16_t major, minor, patch;
  char git_hash[12];
  int32_t commits_ahead;

  ESP_LOGD(TAG, "Getting BMV080 driver version...");
  bmv080_status_code_t status = bmv080_get_driver_version(&major, &minor, &patch, git_hash, &commits_ahead);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to get driver version, status=%d", status);
    return false;
  }
  ESP_LOGI(TAG, "BMV080 SDK driver version: %d.%d.%d (git: %.11s, +%d)", major, minor, patch, git_hash, commits_ahead);

  // --- Open sensor handle ---
  // Pass 'this' as the sercom_handle. The SDK stores this opaque pointer and
  // passes it back to read_cb_/write_cb_ on every I2C operation. We cast it
  // back to BMV080Component* in the callbacks to access ESPHome I2C methods.
  ESP_LOGD(TAG, "Opening BMV080 sensor handle (I2C address: 0x%02X)...", this->address_);
  status = bmv080_open(&this->handle_, (bmv080_sercom_handle_t) this,
                       (bmv080_callback_read_t) read_cb_,
                       (bmv080_callback_write_t) write_cb_,
                       (bmv080_callback_delay_t) delay_cb_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "bmv080_open failed, status=%d", status);
    return false;
  }
  ESP_LOGD(TAG, "bmv080_open succeeded, handle=%p", this->handle_);

  // --- Reset sensor (hardware + software) ---
  // This reverts all parameters to defaults. We reconfigure them in
  // configure_parameters_() immediately after.
  ESP_LOGD(TAG, "Resetting BMV080 sensor...");
  status = bmv080_reset(this->handle_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "bmv080_reset failed, status=%d", status);
    return false;
  }
  ESP_LOGD(TAG, "BMV080 reset complete");

  // --- Read sensor ID ---
  // The sensor ID is a 12-character unique identifier burned into the chip.
  // Useful for identifying specific sensor units in multi-sensor setups.
  char sensor_id[13];  // 12 chars + null terminator
  ESP_LOGD(TAG, "Reading BMV080 sensor ID...");
  status = bmv080_get_sensor_id(this->handle_, sensor_id);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "bmv080_get_sensor_id failed, status=%d", status);
    return false;
  }
  ESP_LOGI(TAG, "BMV080 sensor ID: %s", sensor_id);

  return true;
}

/**
 * @brief Sets all configurable parameters via the Bosch SDK.
 *
 * Parameters are set using string-keyed API calls (bmv080_set_parameter).
 * Each parameter is a void* cast of the appropriate type. Parameters MUST
 * be set before starting measurement — setting them during measurement
 * returns E_BMV080_ERROR_PARAM_LOCKED (179).
 *
 * Parameters set:
 *   - integration_time (float, seconds): Measurement window duration
 *   - duty_cycling_period (uint16_t, seconds): Total ON+OFF period
 *   - do_obstruction_detection (bool): Enable optical path monitoring
 *   - do_vibration_filtering (bool): Enable vibration noise filter
 *   - measurement_algorithm (enum): Precision vs. speed tradeoff
 *
 * @return true if all parameters set successfully, false on any error
 */
bool BMV080Component::configure_parameters_() {
  bmv080_status_code_t status;

  ESP_LOGD(TAG, "Configuring BMV080 parameters...");

  // Integration time — how long the sensor measures before producing output
  ESP_LOGD(TAG, "  Setting integration_time=%.1f s", this->integration_time_);
  status = bmv080_set_parameter(this->handle_, "integration_time", &this->integration_time_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to set integration_time, status=%d", status);
    return false;
  }

  // Duty cycling period — total time for one ON/OFF cycle (duty cycle mode only)
  // Must exceed integration_time by at least 2 seconds
  ESP_LOGD(TAG, "  Setting duty_cycling_period=%u s", this->duty_cycling_period_);
  status = bmv080_set_parameter(this->handle_, "duty_cycling_period", &this->duty_cycling_period_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to set duty_cycling_period, status=%d", status);
    return false;
  }

  // Obstruction detection — monitors the optical path for blockages
  ESP_LOGD(TAG, "  Setting do_obstruction_detection=%s", YESNO(this->obstruction_detection_));
  status = bmv080_set_parameter(this->handle_, "do_obstruction_detection", &this->obstruction_detection_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to set do_obstruction_detection, status=%d", status);
    return false;
  }

  // Vibration filtering — reduces noise from mechanical vibrations
  ESP_LOGD(TAG, "  Setting do_vibration_filtering=%s", YESNO(this->vibration_filtering_));
  status = bmv080_set_parameter(this->handle_, "do_vibration_filtering", &this->vibration_filtering_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to set do_vibration_filtering, status=%d", status);
    return false;
  }

  // Measurement algorithm — controls precision vs. response time tradeoff
  // Note: Duty cycle mode forces FAST_RESPONSE regardless of this setting
  bmv080_measurement_algorithm_t algo = (bmv080_measurement_algorithm_t) this->algorithm_;
  const char *algo_name = "unknown";
  switch (this->algorithm_) {
    case ALGORITHM_FAST_RESPONSE: algo_name = "FAST_RESPONSE"; break;
    case ALGORITHM_BALANCED: algo_name = "BALANCED"; break;
    case ALGORITHM_HIGH_PRECISION: algo_name = "HIGH_PRECISION"; break;
  }
  ESP_LOGD(TAG, "  Setting measurement_algorithm=%s (%d)", algo_name, (int) algo);
  status = bmv080_set_parameter(this->handle_, "measurement_algorithm", &algo);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to set measurement_algorithm, status=%d", status);
    return false;
  }

  ESP_LOGD(TAG, "All BMV080 parameters configured successfully");
  return true;
}

/**
 * @brief Starts continuous or duty-cycle measurement based on configuration.
 *
 * Continuous Mode:
 *   - Sensor runs constantly, data arrives every ~1 second
 *   - Higher power consumption
 *   - Supports all measurement algorithms
 *
 * Duty Cycle Mode:
 *   - Sensor cycles: ON for integration_time, OFF for remainder of period
 *   - Data arrives once per duty_cycling_period
 *   - Forces FAST_RESPONSE algorithm
 *   - Requires tick_cb_ for timing
 *
 * @return true if measurement started successfully, false on error
 */
bool BMV080Component::start_measurement_() {
  bmv080_status_code_t status;

  if (this->mode_ == MEASUREMENT_MODE_CONTINUOUS) {
    ESP_LOGD(TAG, "Starting continuous measurement...");
    status = bmv080_start_continuous_measurement(this->handle_);
  } else {
    ESP_LOGD(TAG, "Starting duty-cycle measurement (period=%u s)...", this->duty_cycling_period_);
    bmv080_duty_cycling_mode_t dc_mode = E_BMV080_DUTY_CYCLING_MODE_0;
    status = bmv080_start_duty_cycling_measurement(this->handle_, (bmv080_callback_tick_t) tick_cb_, dc_mode);
  }

  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to start measurement, status=%d", status);
    return false;
  }

  ESP_LOGI(TAG, "BMV080 measurement started in %s mode",
           this->mode_ == MEASUREMENT_MODE_CONTINUOUS ? "CONTINUOUS" : "DUTY_CYCLE");
  return true;
}

/**
 * @brief Calls bmv080_serve_interrupt() to process the sensor's FIFO.
 *
 * This must be called at least once per second during measurement. The SDK
 * reads the sensor's hardware FIFO, processes any queued events, and calls
 * data_ready_cb_ if new PM data is available.
 *
 * If called more frequently than data is available (e.g., every 500ms for
 * ~1s data intervals), the SDK simply returns E_BMV080_OK without invoking
 * the callback. This is normal and expected.
 *
 * If the FIFO is not read frequently enough, events are lost and the SDK
 * returns warning code 4 (E_BMV080_WARNING_FIFO_EVENTS_OVERFLOW).
 */
void BMV080Component::service_sensor_() {
  if (this->handle_ == nullptr)
    return;

  ESP_LOGV(TAG, "Calling bmv080_serve_interrupt...");
  bmv080_status_code_t status = bmv080_serve_interrupt(
      this->handle_, (bmv080_callback_data_ready_t) data_ready_cb_, (void *) this);

  if (status == E_BMV080_OK) {
    ESP_LOGV(TAG, "bmv080_serve_interrupt: OK");
  } else if (status <= 4) {
    // Status codes 1-4 are warnings — sensor continues operating
    // Common: code 4 = FIFO overflow (serve_interrupt called too infrequently)
    ESP_LOGW(TAG, "bmv080_serve_interrupt warning: %d", status);
  } else {
    // Status codes >= 100 are errors
    ESP_LOGE(TAG, "bmv080_serve_interrupt error: %d", status);
  }
}

// =============================================================================
// ESPHome Main Loop Methods
// =============================================================================

/**
 * @brief Main loop — checks for background task failure.
 *
 * This runs every main loop iteration (typically every few milliseconds).
 * Its only job is to detect if the FreeRTOS task reported a failure during
 * initialization and propagate that to ESPHome's component failure system.
 *
 * All sensor I/O is handled by the FreeRTOS task — this method does NOT
 * call any Bosch SDK functions.
 */
void BMV080Component::loop() {
  // Check if the background task reported an initialization failure.
  // The task sets sensor_failed_ = true and then deletes itself.
  if (this->sensor_failed_) {
    this->sensor_failed_ = false;
    this->mark_failed();
  }
}

/**
 * @brief Periodic update — reads cached sensor data and publishes to Home Assistant.
 *
 * Called at the configured update_interval (default: 5 seconds). Reads the most
 * recent sensor data from the FreeRTOS task's cache (under mutex protection) and
 * publishes each configured sensor's state to Home Assistant.
 *
 * The mutex ensures we never read a partially-written bmv080_output_t struct.
 * The data is copied to a local variable, then published after releasing the mutex
 * to minimize the time the mutex is held.
 *
 * Note: Data may be published multiple times if update_interval < data interval.
 * In continuous mode with 10s integration, new data arrives every ~1 second.
 * With default update_interval of 5s, each reading is published approximately
 * 5 times. This is harmless (HA deduplicates unchanged values).
 */
void BMV080Component::update() {
  if (!this->sensor_initialized_) {
    ESP_LOGD(TAG, "Update: sensor not initialized yet (task still starting)");
    return;
  }

  if (!this->data_available_) {
    ESP_LOGD(TAG, "Update: no new data available yet");
    return;
  }

  // Copy data under mutex protection, then release immediately
  bmv080_output_t output;
  if (this->data_mutex_ != nullptr) {
    xSemaphoreTake(this->data_mutex_, portMAX_DELAY);
  }
  output = this->last_output_;
  this->data_available_ = false;
  if (this->data_mutex_ != nullptr) {
    xSemaphoreGive(this->data_mutex_);
  }

  // Log the values being published
  ESP_LOGD(TAG, "Publishing sensor data:");
  ESP_LOGD(TAG, "  PM1=%.1f ug/m3, PM2.5=%.1f ug/m3, PM10=%.1f ug/m3",
           output.pm1_mass_concentration,
           output.pm2_5_mass_concentration,
           output.pm10_mass_concentration);
  ESP_LOGD(TAG, "  PM1_count=%.0f, PM2.5_count=%.0f, PM10_count=%.0f #/cm3",
           output.pm1_number_concentration,
           output.pm2_5_number_concentration,
           output.pm10_number_concentration);
  ESP_LOGD(TAG, "  runtime=%.1fs, obstructed=%s, out_of_range=%s",
           output.runtime_in_sec,
           output.is_obstructed ? "YES" : "no",
           output.is_outside_measurement_range ? "YES" : "no");

  // Publish each configured sensor to Home Assistant
  // Sensors that weren't configured in YAML have nullptr pointers and are skipped
  if (this->pm_1_0_sensor_ != nullptr)
    this->pm_1_0_sensor_->publish_state(output.pm1_mass_concentration);
  if (this->pm_2_5_sensor_ != nullptr)
    this->pm_2_5_sensor_->publish_state(output.pm2_5_mass_concentration);
  if (this->pm_10_sensor_ != nullptr)
    this->pm_10_sensor_->publish_state(output.pm10_mass_concentration);
  if (this->pm_1_0_count_sensor_ != nullptr)
    this->pm_1_0_count_sensor_->publish_state(output.pm1_number_concentration);
  if (this->pm_2_5_count_sensor_ != nullptr)
    this->pm_2_5_count_sensor_->publish_state(output.pm2_5_number_concentration);
  if (this->pm_10_count_sensor_ != nullptr)
    this->pm_10_count_sensor_->publish_state(output.pm10_number_concentration);
  if (this->runtime_sensor_ != nullptr)
    this->runtime_sensor_->publish_state(output.runtime_in_sec);
  if (this->obstructed_sensor_ != nullptr)
    this->obstructed_sensor_->publish_state(output.is_obstructed);
  if (this->out_of_range_sensor_ != nullptr)
    this->out_of_range_sensor_->publish_state(output.is_outside_measurement_range);
}

/**
 * @brief Logs the component configuration at startup.
 *
 * Called by ESPHome during the boot sequence. Logs all configured parameters,
 * sensor entities, I2C address, and update interval. Useful for debugging
 * configuration issues — the output appears at log level CONFIG.
 */
void BMV080Component::dump_config() {
  ESP_LOGCONFIG(TAG, "BMV080 Particulate Matter Sensor:");
  ESP_LOGCONFIG(TAG, "  Mode: %s",
                this->mode_ == MEASUREMENT_MODE_CONTINUOUS ? "continuous" : "duty_cycle");
  const char *algo_str = "unknown";
  switch (this->algorithm_) {
    case ALGORITHM_FAST_RESPONSE:
      algo_str = "fast_response";
      break;
    case ALGORITHM_BALANCED:
      algo_str = "balanced";
      break;
    case ALGORITHM_HIGH_PRECISION:
      algo_str = "high_precision";
      break;
  }
  ESP_LOGCONFIG(TAG, "  Measurement Algorithm: %s", algo_str);
  ESP_LOGCONFIG(TAG, "  Integration Time: %.1f s", this->integration_time_);
  ESP_LOGCONFIG(TAG, "  Duty Cycling Period: %u s", this->duty_cycling_period_);
  ESP_LOGCONFIG(TAG, "  Obstruction Detection: %s", YESNO(this->obstruction_detection_));
  ESP_LOGCONFIG(TAG, "  Vibration Filtering: %s", YESNO(this->vibration_filtering_));
  ESP_LOGCONFIG(TAG, "  Task Stack: 64KB (dedicated FreeRTOS task)");
  LOG_I2C_DEVICE(this);
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Communication with BMV080 failed!");
  }
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "PM 1.0", this->pm_1_0_sensor_);
  LOG_SENSOR("  ", "PM 2.5", this->pm_2_5_sensor_);
  LOG_SENSOR("  ", "PM 10", this->pm_10_sensor_);
  LOG_SENSOR("  ", "PM 1.0 Count", this->pm_1_0_count_sensor_);
  LOG_SENSOR("  ", "PM 2.5 Count", this->pm_2_5_count_sensor_);
  LOG_SENSOR("  ", "PM 10 Count", this->pm_10_count_sensor_);
  LOG_SENSOR("  ", "Runtime", this->runtime_sensor_);
  LOG_BINARY_SENSOR("  ", "Obstructed", this->obstructed_sensor_);
  LOG_BINARY_SENSOR("  ", "Out of Range", this->out_of_range_sensor_);
}

}  // namespace bmv080
}  // namespace esphome
