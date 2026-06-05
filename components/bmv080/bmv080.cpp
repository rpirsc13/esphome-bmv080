/**
 * @file bmv080.cpp
 * @brief BMV080 hub implementation: Bosch SDK task, bus transport, runtime HA controls.
 */

#include "bmv080_component.h"
#ifdef USE_BMV080_I2C
#include "bmv080_i2c.h"
#endif
#ifdef USE_BMV080_SPI
#include "bmv080_spi.h"
#endif
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

namespace esphome {
namespace bmv080 {

static const char *const TAG = "bmv080";

// =============================================================================
// Bosch SDK callbacks
// =============================================================================

int8_t BMV080Component::read_cb_(bmv080_sercom_handle_t handle, uint16_t header, uint16_t *payload,
                                   uint16_t payload_length) {
  if (handle == nullptr)
    return -1;
  return static_cast<BMV080Component *>(handle)->bus_read_(header, payload, payload_length);
}

int8_t BMV080Component::write_cb_(bmv080_sercom_handle_t handle, uint16_t header, const uint16_t *payload,
                                    uint16_t payload_length) {
  if (handle == nullptr)
    return -1;
  return static_cast<BMV080Component *>(handle)->bus_write_(header, payload, payload_length);
}

int8_t BMV080Component::delay_cb_(uint32_t duration_ms) {
  vTaskDelay(pdMS_TO_TICKS(duration_ms));
  return 0;
}

void BMV080Component::data_ready_cb_(bmv080_output_t output, void *user_data) {
  static_cast<BMV080Component *>(user_data)->on_data_ready(output);
}

uint32_t BMV080Component::tick_cb_() { return millis(); }

// =============================================================================
// I2C transport
// =============================================================================

#ifdef USE_BMV080_I2C

int8_t I2CBMV080Component::bus_read_(uint16_t header, uint16_t *payload, uint16_t payload_length) {
  uint16_t bus_header = header << 1;
  uint8_t header_bytes[2] = {(uint8_t) (bus_header >> 8), (uint8_t) (bus_header & 0xFF)};

  if (this->write(header_bytes, 2) != i2c::ERROR_OK)
    return -2;

  size_t byte_count = payload_length * 2;
  uint8_t buffer[512];
  if (byte_count > sizeof(buffer))
    return -2;

  size_t bytes_read = 0;
  while (bytes_read < byte_count) {
    size_t chunk = byte_count - bytes_read;
    if (chunk > 32)
      chunk = 32;
    if (this->read(buffer + bytes_read, chunk) != i2c::ERROR_OK)
      return -2;
    bytes_read += chunk;
  }

  for (uint16_t i = 0; i < payload_length; i++) {
    payload[i] = ((uint16_t) buffer[i * 2] << 8) | buffer[i * 2 + 1];
  }
  return 0;
}

int8_t I2CBMV080Component::bus_write_(uint16_t header, const uint16_t *payload, uint16_t payload_length) {
  uint16_t bus_header = header << 1;
  size_t total_bytes = 2 + payload_length * 2;
  uint8_t buffer[512];
  if (total_bytes > sizeof(buffer))
    return -3;

  buffer[0] = (uint8_t) (bus_header >> 8);
  buffer[1] = (uint8_t) (bus_header & 0xFF);
  for (uint16_t i = 0; i < payload_length; i++) {
    buffer[2 + i * 2] = (uint8_t) (payload[i] >> 8);
    buffer[2 + i * 2 + 1] = (uint8_t) (payload[i] & 0xFF);
  }

  if (this->write(buffer, total_bytes) != i2c::ERROR_OK)
    return -3;
  return 0;
}

void I2CBMV080Component::dump_config() {
  BMV080Component::dump_config();
  LOG_I2C_DEVICE(this);
}

#endif  // USE_BMV080_I2C

// =============================================================================
// SPI transport (Bosch 16-bit word protocol, no I2C header shift)
// =============================================================================

#ifdef USE_BMV080_SPI

int8_t SPIBMV080Component::bus_read_(uint16_t header, uint16_t *payload, uint16_t payload_length) {
  uint8_t header_bytes[2] = {(uint8_t) (header >> 8), (uint8_t) (header & 0xFF)};
  size_t byte_count = payload_length * 2;
  uint8_t buffer[512];
  if (byte_count > sizeof(buffer))
    return -2;

  this->enable();
  this->write_array(header_bytes, 2);
  this->read_array(buffer, byte_count);
  this->disable();

  for (uint16_t i = 0; i < payload_length; i++) {
    payload[i] = ((uint16_t) buffer[i * 2] << 8) | buffer[i * 2 + 1];
  }
  return 0;
}

int8_t SPIBMV080Component::bus_write_(uint16_t header, const uint16_t *payload, uint16_t payload_length) {
  size_t total_bytes = 2 + payload_length * 2;
  uint8_t buffer[512];
  if (total_bytes > sizeof(buffer))
    return -3;

  buffer[0] = (uint8_t) (header >> 8);
  buffer[1] = (uint8_t) (header & 0xFF);
  for (uint16_t i = 0; i < payload_length; i++) {
    buffer[2 + i * 2] = (uint8_t) (payload[i] >> 8);
    buffer[2 + i * 2 + 1] = (uint8_t) (payload[i] & 0xFF);
  }

  this->enable();
  this->write_array(buffer, total_bytes);
  this->disable();
  return 0;
}

void SPIBMV080Component::dump_config() {
  BMV080Component::dump_config();
  LOG_SPI_DEVICE(this);
}

#endif  // USE_BMV080_SPI

// =============================================================================
// Runtime HA controls
// =============================================================================

const char *BMV080Component::preset_to_label_(BMV080Preset preset) const {
  switch (preset) {
    case BMV080_PRESET_FAST:
      return "Fast Response";
    case BMV080_PRESET_BALANCED:
      return "Balanced";
    case BMV080_PRESET_PRECISION:
    default:
      return "High Precision";
  }
}

BMV080Preset BMV080Component::label_to_preset_(const std::string &label) const {
  if (label == "Fast Response")
    return BMV080_PRESET_FAST;
  if (label == "Balanced")
    return BMV080_PRESET_BALANCED;
  return BMV080_PRESET_PRECISION;
}

void BMV080Component::change_runtime_preset(const std::string &state) {
  this->pending_preset_ = this->label_to_preset_(state);
  this->pending_runtime_reconfig_ = true;
  ESP_LOGI(TAG, "Runtime algorithm preset queued: %s", state.c_str());
}

void BMV080Component::change_runtime_integration_time(float seconds) {
  this->pending_integration_time_ = seconds;
  this->pending_runtime_reconfig_ = true;
  ESP_LOGI(TAG, "Runtime integration time queued: %.0f s", seconds);
}

void BMV080Select::control(const std::string &value) {
  if (this->parent_ != nullptr)
    this->parent_->change_runtime_preset(value);
  this->publish_state(value);
}

void BMV080IntegrationNumber::control(float value) {
  if (this->parent_ != nullptr)
    this->parent_->change_runtime_integration_time(value);
  this->publish_state(value);
}

void BMV080Component::publish_initial_control_states_() {
  if (this->preset_select_ != nullptr)
    this->preset_select_->publish_state(this->preset_to_label_(this->current_preset_));
  if (this->integration_number_ != nullptr)
    this->integration_number_->publish_state(this->current_integration_time_);
}

// =============================================================================
// Data path and SDK lifecycle
// =============================================================================

void BMV080Component::on_data_ready(bmv080_output_t output) {
  if (this->data_mutex_ != nullptr)
    xSemaphoreTake(this->data_mutex_, portMAX_DELAY);
  this->last_output_ = output;
  this->data_available_ = true;
  if (this->data_mutex_ != nullptr)
    xSemaphoreGive(this->data_mutex_);
}

bool BMV080Component::apply_runtime_parameters_() {
  if (!this->pending_runtime_reconfig_)
    return true;

  this->current_preset_ = this->pending_preset_;
  this->current_integration_time_ = this->pending_integration_time_;
  this->pending_runtime_reconfig_ = false;

  if (this->handle_ == nullptr || !this->sensor_initialized_) {
    return true;
  }

  bmv080_status_code_t status = bmv080_stop_measurement(this->handle_);
  if (status != E_BMV080_OK) {
    ESP_LOGW(TAG, "bmv080_stop_measurement before runtime reconfig: %d", status);
  }

  status = bmv080_set_parameter(this->handle_, "integration_time", &this->current_integration_time_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to set integration_time at runtime, status=%d", status);
    return false;
  }

  bmv080_measurement_algorithm_t algo = (bmv080_measurement_algorithm_t) this->current_preset_;
  status = bmv080_set_parameter(this->handle_, "measurement_algorithm", &algo);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to set measurement_algorithm at runtime, status=%d", status);
    return false;
  }

  status = bmv080_start_continuous_measurement(this->handle_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "Failed to restart measurement after runtime reconfig, status=%d", status);
    return false;
  }

  this->publish_initial_control_states_();
  ESP_LOGI(TAG, "Runtime parameters applied (preset=%s, integration=%.0fs)",
           this->preset_to_label_(this->current_preset_), this->current_integration_time_);
  return true;
}

void BMV080Component::setup() {
  ESP_LOGCONFIG(TAG, "Setting up BMV080...");

  this->current_preset_ = this->initial_preset_;
  this->current_integration_time_ = static_cast<float>(this->initial_integration_time_);

  this->data_mutex_ = xSemaphoreCreateMutex();
  if (this->data_mutex_ == nullptr) {
    this->mark_failed();
    return;
  }

  BaseType_t ret = xTaskCreatePinnedToCore(sensor_task_, "bmv080", 64 * 1024, this, 1, &this->task_handle_, 0);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create BMV080 task");
    this->mark_failed();
    return;
  }

  ESP_LOGI(TAG, "BMV080 task created, initialization begins in ~10 seconds");
}

void BMV080Component::sensor_task_(void *arg) {
  auto *comp = static_cast<BMV080Component *>(arg);

  ESP_LOGI(TAG, "BMV080 task started, waiting 10 seconds for system startup...");
  vTaskDelay(pdMS_TO_TICKS(10000));

  comp->apply_runtime_parameters_();

  if (!comp->init_sensor_() || !comp->configure_parameters_() || !comp->start_measurement_()) {
    comp->sensor_failed_ = true;
    vTaskDelete(nullptr);
    return;
  }

  comp->sensor_initialized_ = true;
  comp->publish_initial_control_states_();
  ESP_LOGI(TAG, "BMV080 initialized and measurement started");

  while (true) {
    if (!comp->apply_runtime_parameters_()) {
      ESP_LOGW(TAG, "Runtime reconfiguration failed");
    }
    comp->service_sensor_();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

bool BMV080Component::init_sensor_() {
  uint16_t major, minor, patch;
  char git_hash[12];
  int32_t commits_ahead;

  bmv080_status_code_t status =
      bmv080_get_driver_version(&major, &minor, &patch, git_hash, &commits_ahead);
  if (status != E_BMV080_OK)
    return false;

  ESP_LOGI(TAG, "BMV080 SDK driver version: %d.%d.%d", major, minor, patch);

  status = bmv080_open(&this->handle_, (bmv080_sercom_handle_t) this, (bmv080_callback_read_t) read_cb_,
                       (bmv080_callback_write_t) write_cb_, (bmv080_callback_delay_t) delay_cb_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "bmv080_open failed, status=%d", status);
    return false;
  }

  status = bmv080_reset(this->handle_);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "bmv080_reset failed, status=%d", status);
    return false;
  }

  char sensor_id[13];
  status = bmv080_get_sensor_id(this->handle_, sensor_id);
  if (status != E_BMV080_OK) {
    ESP_LOGE(TAG, "bmv080_get_sensor_id failed, status=%d", status);
    return false;
  }
  ESP_LOGI(TAG, "BMV080 sensor ID: %s", sensor_id);
  return true;
}

bool BMV080Component::configure_parameters_() {
  bmv080_status_code_t status =
      bmv080_set_parameter(this->handle_, "integration_time", &this->current_integration_time_);
  if (status != E_BMV080_OK)
    return false;

  bool obstruction_detection = true;
  status = bmv080_set_parameter(this->handle_, "do_obstruction_detection", &obstruction_detection);
  if (status != E_BMV080_OK)
    return false;

  bool vibration_filtering = false;
  status = bmv080_set_parameter(this->handle_, "do_vibration_filtering", &vibration_filtering);
  if (status != E_BMV080_OK)
    return false;

  bmv080_measurement_algorithm_t algo = (bmv080_measurement_algorithm_t) this->current_preset_;
  status = bmv080_set_parameter(this->handle_, "measurement_algorithm", &algo);
  return status == E_BMV080_OK;
}

bool BMV080Component::start_measurement_() {
  return bmv080_start_continuous_measurement(this->handle_) == E_BMV080_OK;
}

void BMV080Component::service_sensor_() {
  if (this->handle_ == nullptr)
    return;

  bmv080_status_code_t status =
      bmv080_serve_interrupt(this->handle_, (bmv080_callback_data_ready_t) data_ready_cb_, (void *) this);
  if (status != E_BMV080_OK && status > 4) {
    ESP_LOGE(TAG, "bmv080_serve_interrupt error: %d", status);
  }
}

void BMV080Component::loop() {
  if (this->sensor_failed_) {
    this->sensor_failed_ = false;
    this->mark_failed();
  }
}

void BMV080Component::update() {
  if (!this->sensor_initialized_ || !this->data_available_)
    return;

  bmv080_output_t output;
  if (this->data_mutex_ != nullptr)
    xSemaphoreTake(this->data_mutex_, portMAX_DELAY);
  output = this->last_output_;
  this->data_available_ = false;
  if (this->data_mutex_ != nullptr)
    xSemaphoreGive(this->data_mutex_);

  if (this->pm_1_0_mass_ != nullptr)
    this->pm_1_0_mass_->publish_state(output.pm1_mass_concentration);
  if (this->pm_2_5_mass_ != nullptr)
    this->pm_2_5_mass_->publish_state(output.pm2_5_mass_concentration);
  if (this->pm_10_0_mass_ != nullptr)
    this->pm_10_0_mass_->publish_state(output.pm10_mass_concentration);
  if (this->pm_1_0_count_ != nullptr)
    this->pm_1_0_count_->publish_state(output.pm1_number_concentration);
  if (this->pm_2_5_count_ != nullptr)
    this->pm_2_5_count_->publish_state(output.pm2_5_number_concentration);
  if (this->pm_10_0_count_ != nullptr)
    this->pm_10_0_count_->publish_state(output.pm10_number_concentration);
  if (this->runtime_sensor_ != nullptr)
    this->runtime_sensor_->publish_state(output.runtime_in_sec);
  if (this->obstruction_alert_ != nullptr)
    this->obstruction_alert_->publish_state(output.is_obstructed);
  if (this->out_of_range_alert_ != nullptr)
    this->out_of_range_alert_->publish_state(output.is_outside_measurement_range);
}

void BMV080Component::dump_config() {
  ESP_LOGCONFIG(TAG, "BMV080 Particulate Matter Sensor:");
  ESP_LOGCONFIG(TAG, "  Algorithm Preset: %s", this->preset_to_label_(this->current_preset_));
  ESP_LOGCONFIG(TAG, "  Integration Time: %.0f s", this->current_integration_time_);
  ESP_LOGCONFIG(TAG, "  Task Stack: 64KB (dedicated FreeRTOS task)");
  this->log_bus_config_();
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Communication with BMV080 failed!");
  }
  LOG_UPDATE_INTERVAL(this);
  LOG_SENSOR("  ", "PM 1.0 Mass", this->pm_1_0_mass_);
  LOG_SENSOR("  ", "PM 2.5 Mass", this->pm_2_5_mass_);
  LOG_SENSOR("  ", "PM 10 Mass", this->pm_10_0_mass_);
  LOG_SENSOR("  ", "PM 1.0 Count", this->pm_1_0_count_);
  LOG_SENSOR("  ", "PM 2.5 Count", this->pm_2_5_count_);
  LOG_SENSOR("  ", "PM 10 Count", this->pm_10_0_count_);
  LOG_SENSOR("  ", "Runtime", this->runtime_sensor_);
  LOG_BINARY_SENSOR("  ", "Obstruction", this->obstruction_alert_);
  LOG_BINARY_SENSOR("  ", "Saturation", this->out_of_range_alert_);
}

}  // namespace bmv080
}  // namespace esphome
