/**
 * @file bmv080_component.h
 * @brief ESPHome hub component for the Bosch BMV080 particulate matter sensor.
 *
 * Named bmv080_component.h (not bmv080.h) to avoid collision with bosch/bmv080.h.
 */

#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/select/select.h"
#include "esphome/components/number/number.h"

#include "bmv080_defs.h"
#include "bmv080.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

namespace esphome {
namespace bmv080 {

enum BMV080Preset : uint8_t {
  BMV080_PRESET_FAST = 1,
  BMV080_PRESET_BALANCED = 2,
  BMV080_PRESET_PRECISION = 3,
};

class BMV080Component;

class BMV080Select : public select::Select, public Component {
 public:
  void set_parent(BMV080Component *parent) { this->parent_ = parent; }

 protected:
  void control(const std::string &value) override;
  BMV080Component *parent_{nullptr};
};

class BMV080IntegrationNumber : public number::Number, public Component {
 public:
  void set_parent(BMV080Component *parent) { this->parent_ = parent; }

 protected:
  void control(float value) override;
  BMV080Component *parent_{nullptr};
};

/**
 * @brief Base BMV080 hub: Bosch SDK lifecycle, FreeRTOS task, HA entity publishing.
 *
 * Bus-specific I2C/SPI transport is implemented in I2CBMV080Component / SPIBMV080Component.
 */
class BMV080Component : public PollingComponent {
 public:
  void set_pm_1_0_mass_sensor(sensor::Sensor *s) { this->pm_1_0_mass_ = s; }
  void set_pm_2_5_mass_sensor(sensor::Sensor *s) { this->pm_2_5_mass_ = s; }
  void set_pm_10_0_mass_sensor(sensor::Sensor *s) { this->pm_10_0_mass_ = s; }
  void set_pm_1_0_count_sensor(sensor::Sensor *s) { this->pm_1_0_count_ = s; }
  void set_pm_2_5_count_sensor(sensor::Sensor *s) { this->pm_2_5_count_ = s; }
  void set_pm_10_0_count_sensor(sensor::Sensor *s) { this->pm_10_0_count_ = s; }
  void set_runtime_sensor(sensor::Sensor *s) { this->runtime_sensor_ = s; }

  // Backward-compatible sensor setter aliases
  void set_pm_1_0_sensor(sensor::Sensor *s) { this->set_pm_1_0_mass_sensor(s); }
  void set_pm_2_5_sensor(sensor::Sensor *s) { this->set_pm_2_5_mass_sensor(s); }
  void set_pm_10_sensor(sensor::Sensor *s) { this->set_pm_10_0_mass_sensor(s); }

  void set_obstruction_binary_sensor(binary_sensor::BinarySensor *s) { this->obstruction_alert_ = s; }
  void set_saturation_binary_sensor(binary_sensor::BinarySensor *s) { this->out_of_range_alert_ = s; }

  // Backward-compatible binary sensor setter aliases
  void set_obstructed_binary_sensor(binary_sensor::BinarySensor *s) { this->set_obstruction_binary_sensor(s); }
  void set_out_of_range_binary_sensor(binary_sensor::BinarySensor *s) { this->set_saturation_binary_sensor(s); }

  void set_preset_select_entity(BMV080Select *sel) { this->preset_select_ = sel; }
  void set_integration_time_number_entity(BMV080IntegrationNumber *num) { this->integration_number_ = num; }

  void set_initial_preset(BMV080Preset preset) { this->initial_preset_ = preset; }
  void set_initial_integration_time(uint16_t seconds) { this->initial_integration_time_ = seconds; }

  void change_runtime_preset(const std::string &state);
  void change_runtime_integration_time(float seconds);

  void setup() override;
  void loop() override;
  void update() override;
  void dump_config() override;

  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  virtual int8_t bus_read_(uint16_t header, uint16_t *payload, uint16_t payload_length) = 0;
  virtual int8_t bus_write_(uint16_t header, const uint16_t *payload, uint16_t payload_length) = 0;
  virtual void log_bus_config_() {}

  static int8_t read_cb_(bmv080_sercom_handle_t handle, uint16_t header, uint16_t *payload,
                         uint16_t payload_length);
  static int8_t write_cb_(bmv080_sercom_handle_t handle, uint16_t header, const uint16_t *payload,
                          uint16_t payload_length);
  static int8_t delay_cb_(uint32_t duration_ms);
  static void data_ready_cb_(bmv080_output_t output, void *user_data);
  static uint32_t tick_cb_();
  static void sensor_task_(void *arg);

  void on_data_ready(bmv080_output_t output);
  bool init_sensor_();
  bool configure_parameters_();
  bool start_measurement_();
  void service_sensor_();
  bool apply_runtime_parameters_();
  void publish_initial_control_states_();
  const char *preset_to_label_(BMV080Preset preset) const;
  BMV080Preset label_to_preset_(const std::string &label) const;

  bmv080_handle_t handle_{nullptr};

  sensor::Sensor *pm_1_0_mass_{nullptr};
  sensor::Sensor *pm_2_5_mass_{nullptr};
  sensor::Sensor *pm_10_0_mass_{nullptr};
  sensor::Sensor *pm_1_0_count_{nullptr};
  sensor::Sensor *pm_2_5_count_{nullptr};
  sensor::Sensor *pm_10_0_count_{nullptr};
  sensor::Sensor *runtime_sensor_{nullptr};

  binary_sensor::BinarySensor *obstruction_alert_{nullptr};
  binary_sensor::BinarySensor *out_of_range_alert_{nullptr};

  BMV080Select *preset_select_{nullptr};
  BMV080IntegrationNumber *integration_number_{nullptr};

  BMV080Preset initial_preset_{BMV080_PRESET_PRECISION};
  uint16_t initial_integration_time_{20};
  BMV080Preset current_preset_{BMV080_PRESET_PRECISION};
  float current_integration_time_{20.0f};

  volatile bool pending_runtime_reconfig_{false};
  volatile BMV080Preset pending_preset_{BMV080_PRESET_PRECISION};
  volatile float pending_integration_time_{20.0f};

  bmv080_output_t last_output_{};
  volatile bool data_available_{false};
  volatile bool sensor_initialized_{false};
  volatile bool sensor_failed_{false};
  SemaphoreHandle_t data_mutex_{nullptr};
  TaskHandle_t task_handle_{nullptr};
};

}  // namespace bmv080
}  // namespace esphome
