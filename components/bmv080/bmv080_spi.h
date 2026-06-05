/**
 * @file bmv080_spi.h
 * @brief SPI bus implementation for the BMV080 component (Polverine-compatible).
 */

#pragma once

#ifdef USE_BMV080_SPI

#include "bmv080_component.h"
#include "esphome/components/spi/spi.h"

namespace esphome {
namespace bmv080 {

class SPIBMV080Component : public BMV080Component,
                           public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                                 spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_1MHZ> {
 public:
  void setup() override;
  void dump_config() override;

 protected:
  int8_t bus_read_(uint16_t header, uint16_t *payload, uint16_t payload_length) override;
  int8_t bus_write_(uint16_t header, const uint16_t *payload, uint16_t payload_length) override;
};

}  // namespace bmv080
}  // namespace esphome

#endif  // USE_BMV080_SPI
