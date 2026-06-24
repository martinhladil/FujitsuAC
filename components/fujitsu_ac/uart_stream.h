#pragma once

#include <Stream.h>
#include "esphome/components/uart/uart.h"

namespace esphome::fujitsu_ac {

class UartStreamAdapter : public Stream {
 public:
  explicit UartStreamAdapter(uart::UARTDevice &uart) : uart_(uart) {}

  int available() override { return this->uart_.available(); }

  int read() override {
    uint8_t data;
    if (this->uart_.read_byte(&data)) {
      return data;
    }
    return -1;
  }

  int peek() override { return -1; }

  void flush() override {}

  size_t write(uint8_t data) override {
    this->uart_.write_byte(data);
    return 1;
  }

  size_t write(const uint8_t *buffer, size_t size) override {
    this->uart_.write_array(buffer, size);
    return size;
  }

 private:
  uart::UARTDevice &uart_;
};

}  // namespace esphome::fujitsu_ac
