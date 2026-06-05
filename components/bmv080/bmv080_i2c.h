/**
 * @file bmv080_i2c.h
 * @brief I2C bus implementation for the BMV080 component.
 */

#pragma once

#ifdef USE_BMV080_I2C

#include "bmv080_component.h"
#include "esphome/components/i2c/i2c.h"

namespace esphome {
namespace bmv080 {

class I2CBMV080Component : public BMV080Component, public i2c::I2CDevice {
 public:
  void dump_config() override;

 protected:
  int8_t bus_read_(uint16_t header, uint16_t *payload, uint16_t payload_length) override;
  int8_t bus_write_(uint16_t header, const uint16_t *payload, uint16_t payload_length) override;
};

}  // namespace bmv080
}  // namespace esphome

#endif  // USE_BMV080_I2C
