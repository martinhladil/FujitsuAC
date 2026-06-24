#pragma once

#include "esphome/components/climate/climate.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"
#include "uart_stream.h"

#include <TFSXW1Controller.h>

#include <optional>

namespace esphome::fujitsu_ac {

class FujitsuClimate : public climate::Climate, public Component, public uart::UARTDevice {
 public:
  FujitsuClimate() : stream_(*this), controller_(stream_) {}

  void setup() override;
  void loop() override;
  void dump_config() override;

  climate::ClimateTraits traits() override;
  void control(const climate::ClimateCall &call) override;

 protected:
  void on_register_change_(const FujitsuAC::RegistryTable::Register *reg);
  void apply_state_();
  void try_apply_pending_();
  void clear_satisfied_pending_();

  UartStreamAdapter stream_;
  FujitsuAC::TFSXW1Controller controller_;

  // Mirrors user intent; drained one write per attempt because the native
  // controller's send queue holds only one register write at a time.
  struct Pending {
    std::optional<FujitsuAC::TFSXW1Enums::Power> power;
    std::optional<FujitsuAC::TFSXW1Enums::Mode> mode;
    std::optional<FujitsuAC::TFSXW1Enums::FanSpeed> fan_speed;
    std::optional<float> target_temperature;
    std::optional<FujitsuAC::TFSXW1Enums::VerticalSwing> vertical_swing;
    std::optional<FujitsuAC::TFSXW1Enums::HorizontalSwing> horizontal_swing;
    std::optional<FujitsuAC::TFSXW1Enums::Powerful> powerful;
    std::optional<FujitsuAC::TFSXW1Enums::EconomyMode> economy;
    uint32_t deadline_ms{0};

    bool empty() const {
      return !power && !mode && !fan_speed && !target_temperature && !vertical_swing && !horizontal_swing &&
             !powerful && !economy;
    }
  };

  Pending pending_;
  uint32_t last_apply_attempt_ms_{0};

  static constexpr uint32_t PENDING_TIMEOUT_MS = 60000;
  static constexpr uint32_t APPLY_INTERVAL_MS = 500;
};

}  // namespace esphome::fujitsu_ac
