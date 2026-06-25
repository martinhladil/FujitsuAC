#include "fujitsu_climate.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cmath>
#include <cstdio>

namespace esphome::fujitsu_ac {

static const char *const TAG = "fujitsu_ac.climate";

using Address = FujitsuAC::TFSXW1Controller::Address;

static constexpr std::array MODE_MAP{
    std::pair{FujitsuAC::TFSXW1Enums::Mode::Auto, climate::CLIMATE_MODE_HEAT_COOL},
    std::pair{FujitsuAC::TFSXW1Enums::Mode::Cool, climate::CLIMATE_MODE_COOL},
    std::pair{FujitsuAC::TFSXW1Enums::Mode::Dry, climate::CLIMATE_MODE_DRY},
    std::pair{FujitsuAC::TFSXW1Enums::Mode::Fan, climate::CLIMATE_MODE_FAN_ONLY},
    std::pair{FujitsuAC::TFSXW1Enums::Mode::Heat, climate::CLIMATE_MODE_HEAT},
};

static constexpr std::array FAN_MODE_MAP{
    std::pair{FujitsuAC::TFSXW1Enums::FanSpeed::Auto, climate::CLIMATE_FAN_AUTO},
    std::pair{FujitsuAC::TFSXW1Enums::FanSpeed::Quiet, climate::CLIMATE_FAN_QUIET},
    std::pair{FujitsuAC::TFSXW1Enums::FanSpeed::Low, climate::CLIMATE_FAN_LOW},
    std::pair{FujitsuAC::TFSXW1Enums::FanSpeed::Medium, climate::CLIMATE_FAN_MEDIUM},
    std::pair{FujitsuAC::TFSXW1Enums::FanSpeed::High, climate::CLIMATE_FAN_HIGH},
};

template<typename A, typename B, std::size_t N>
static bool map_lookup(const std::array<std::pair<A, B>, N> &map, A key, B &out) {
  for (const auto &[from, to] : map) {
    if (from == key) {
      out = to;
      return true;
    }
  }
  return false;
}

template<typename Left, typename Right, std::size_t N>
static std::optional<Left> reverse_map_lookup(const std::array<std::pair<Left, Right>, N> &map, Right key) {
  for (const auto &entry : map) {
    if (entry.second == key) {
      return entry.first;
    }
  }
  return std::nullopt;
}

template<typename Left, typename Right, std::size_t N>
static std::optional<Left> reverse_map_lookup(const std::array<std::pair<Left, Right>, N> &map,
                                              const std::optional<Right> &key) {
  return key.has_value() ? reverse_map_lookup(map, *key) : std::nullopt;
}

void FujitsuClimate::dump_config() { LOG_CLIMATE("", "Fujitsu AC Climate", this); }

void FujitsuClimate::setup() {
  // Restore previously-detected capabilities. On a fresh device load() fails and
  // caps_ stays 0, so nothing autodetectable is advertised until the first
  // handshake completes and we reboot with the detected set.
  this->caps_pref_ = global_preferences->make_preference<uint8_t>(0xF0410CABU);
  if (!this->caps_pref_.load(&this->caps_)) {
    this->caps_ = 0;
  }
  ESP_LOGI(TAG, "Restored capabilities: 0x%02X", this->caps_);

  this->controller_.setOnRegisterChangeCallback([this](const FujitsuAC::RegistryTable::Register *reg) {
    this->on_register_change_(reg);
  });

  this->controller_.setDebugCallback([](const char *name, const char *message) {
    ESP_LOGD(TAG, "%s: %s", name, message);
  });

  this->controller_.setup();
}

void FujitsuClimate::loop() {
  this->controller_.loop();
  this->check_capabilities_();
  this->try_apply_pending_();
}

climate::ClimateTraits FujitsuClimate::traits() {
  climate::ClimateTraits traits;

  traits.add_supported_mode(climate::CLIMATE_MODE_OFF);
  for (const auto &entry : MODE_MAP) {
    traits.add_supported_mode(entry.second);
  }

  for (const auto &entry : FAN_MODE_MAP) {
    traits.add_supported_fan_mode(entry.second);
  }

  // Swing axes are advertised only when autodetected (see check_capabilities_).
  const bool v_swing = this->caps_ & CAP_VERTICAL_SWING;
  const bool h_swing = this->caps_ & CAP_HORIZONTAL_SWING;
  if (v_swing || h_swing) {
    traits.add_supported_swing_mode(climate::CLIMATE_SWING_OFF);
    if (v_swing) {
      traits.add_supported_swing_mode(climate::CLIMATE_SWING_VERTICAL);
    }
    if (h_swing) {
      traits.add_supported_swing_mode(climate::CLIMATE_SWING_HORIZONTAL);
    }
    if (v_swing && h_swing) {
      traits.add_supported_swing_mode(climate::CLIMATE_SWING_BOTH);
    }
  }

  // BOOST → Powerful, ECO → Economy (mutually exclusive), advertised only when
  // autodetected.
  if (this->caps_ & (CAP_BOOST | CAP_ECO)) {
    traits.add_supported_preset(climate::CLIMATE_PRESET_NONE);
    if (this->caps_ & CAP_BOOST) {
      traits.add_supported_preset(climate::CLIMATE_PRESET_BOOST);
    }
    if (this->caps_ & CAP_ECO) {
      traits.add_supported_preset(climate::CLIMATE_PRESET_ECO);
    }
  }

  // Heat allows 16°C; other modes clamp to 18°C inside the controller.
  traits.set_visual_min_temperature(16.0f);
  traits.set_visual_max_temperature(30.0f);
  traits.set_visual_temperature_step(0.5f);
  traits.add_feature_flags(climate::CLIMATE_SUPPORTS_CURRENT_TEMPERATURE);
  traits.set_visual_current_temperature_step(0.01f);

  return traits;
}

void FujitsuClimate::control(const climate::ClimateCall &call) {
  if (const auto target_temperature = call.get_target_temperature()) {
    this->pending_.target_temperature = *target_temperature;
  }

  if (const auto mode = call.get_mode()) {
    if (*mode == climate::CLIMATE_MODE_OFF) {
      this->pending_ = {};  // turning off supersedes any other in-flight intent
      this->pending_.power = FujitsuAC::TFSXW1Enums::Power::Off;
    } else if (const auto mapped = reverse_map_lookup(MODE_MAP, *mode)) {
      this->pending_.mode = *mapped;
      if (!this->controller_.isPoweredOn()) {
        this->pending_.power = FujitsuAC::TFSXW1Enums::Power::On;
      } else if (this->pending_.power == FujitsuAC::TFSXW1Enums::Power::Off) {
        // User flipped from OFF back to a real mode before OFF was applied.
        this->pending_.power.reset();
      }
    }
  }

  if (const auto fan = reverse_map_lookup(FAN_MODE_MAP, call.get_fan_mode())) {
    this->pending_.fan_speed = *fan;
  }

  if (const auto swing = call.get_swing_mode()) {
    using VS = FujitsuAC::TFSXW1Enums::VerticalSwing;
    using HS = FujitsuAC::TFSXW1Enums::HorizontalSwing;
    const bool v = *swing == climate::CLIMATE_SWING_VERTICAL || *swing == climate::CLIMATE_SWING_BOTH;
    const bool h = *swing == climate::CLIMATE_SWING_HORIZONTAL || *swing == climate::CLIMATE_SWING_BOTH;
    if (this->caps_ & CAP_VERTICAL_SWING) {
      this->pending_.vertical_swing = v ? VS::On : VS::Off;
    }
    if (this->caps_ & CAP_HORIZONTAL_SWING) {
      this->pending_.horizontal_swing = h ? HS::On : HS::Off;
    }
  }

  if (const auto preset = call.get_preset()) {
    using P = FujitsuAC::TFSXW1Enums::Powerful;
    using E = FujitsuAC::TFSXW1Enums::EconomyMode;
    if (this->caps_ & CAP_BOOST) {
      this->pending_.powerful = (*preset == climate::CLIMATE_PRESET_BOOST) ? P::On : P::Off;
    }
    if (this->caps_ & CAP_ECO) {
      this->pending_.economy = (*preset == climate::CLIMATE_PRESET_ECO) ? E::On : E::Off;
    }
  }

  if (!this->pending_.empty()) {
    this->pending_.deadline_ms = millis() + PENDING_TIMEOUT_MS;
    this->last_apply_attempt_ms_ = 0;
  }
}

void FujitsuClimate::clear_satisfied_pending_() {
  // Power::Off, Mode::Auto and FanSpeed::Auto are all 0, colliding with the
  // registry table's init zeros; wait for real values before matching.
  const auto *actual_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::ActualTemp));
  if (actual_reg == nullptr || actual_reg->value == 0) {
    return;
  }

  if (this->pending_.power) {
    const auto *reg = this->controller_.getRegister(static_cast<uint16_t>(Address::Power));
    if (reg != nullptr && reg->value == static_cast<uint16_t>(*this->pending_.power)) {
      this->pending_.power.reset();
    }
  }
  if (this->pending_.mode) {
    const auto *reg = this->controller_.getRegister(static_cast<uint16_t>(Address::Mode));
    if (reg != nullptr && reg->value == static_cast<uint16_t>(*this->pending_.mode)) {
      this->pending_.mode.reset();
    }
  }
  if (this->pending_.fan_speed) {
    const auto *reg = this->controller_.getRegister(static_cast<uint16_t>(Address::FanSpeed));
    if (reg != nullptr && reg->value == static_cast<uint16_t>(*this->pending_.fan_speed)) {
      this->pending_.fan_speed.reset();
    }
  }
  if (this->pending_.vertical_swing) {
    const auto *reg = this->controller_.getRegister(static_cast<uint16_t>(Address::VerticalSwing));
    if (reg != nullptr && reg->value == static_cast<uint16_t>(*this->pending_.vertical_swing)) {
      this->pending_.vertical_swing.reset();
    }
  }
  if (this->pending_.horizontal_swing) {
    const auto *reg = this->controller_.getRegister(static_cast<uint16_t>(Address::HorizontalSwing));
    if (reg != nullptr && reg->value == static_cast<uint16_t>(*this->pending_.horizontal_swing)) {
      this->pending_.horizontal_swing.reset();
    }
  }
  if (this->pending_.powerful) {
    const auto *reg = this->controller_.getRegister(static_cast<uint16_t>(Address::Powerful));
    if (reg != nullptr && reg->value == static_cast<uint16_t>(*this->pending_.powerful)) {
      this->pending_.powerful.reset();
    }
  }
  if (this->pending_.economy) {
    const auto *reg = this->controller_.getRegister(static_cast<uint16_t>(Address::EconomyMode));
    if (reg != nullptr && reg->value == static_cast<uint16_t>(*this->pending_.economy)) {
      this->pending_.economy.reset();
    }
  }
  if (this->pending_.target_temperature) {
    // Setpoint isn't writable in Fan mode (unit returns 0xFFFF) — drop it.
    const auto *mode_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::Mode));
    const auto effective_mode = this->pending_.mode
                                    ? *this->pending_.mode
                                    : static_cast<FujitsuAC::TFSXW1Enums::Mode>(mode_reg->value);
    if (effective_mode == FujitsuAC::TFSXW1Enums::Mode::Fan) {
      ESP_LOGD(TAG, "Ignoring target temperature in Fan mode");
      this->pending_.target_temperature.reset();
    } else {
      const auto *reg = this->controller_.getRegister(static_cast<uint16_t>(Address::SetpointTemp));
      if (reg != nullptr && reg->value != 0xFFFF) {
        // Controller quantises to 0.5°C; quarter-step tolerance.
        const float current = reg->value / 10.0f;
        if (std::fabs(current - *this->pending_.target_temperature) < 0.25f) {
          this->pending_.target_temperature.reset();
        }
      }
    }
  }
}

void FujitsuClimate::try_apply_pending_() {
  this->clear_satisfied_pending_();

  if (this->pending_.empty()) {
    return;
  }

  const uint32_t now = millis();
  if (static_cast<int32_t>(now - this->pending_.deadline_ms) >= 0) {
    ESP_LOGW(TAG, "Pending changes timed out, dropping");
    this->pending_ = {};
    return;
  }

  if ((now - this->last_apply_attempt_ms_) < APPLY_INTERVAL_MS) {
    return;
  }
  this->last_apply_attempt_ms_ = now;

  // Power last — some units reject setPower(On) until a mode write has primed
  // them. Matches how the MQTT bridge naturally sequences these.
  if (this->pending_.mode) {
    this->controller_.setMode(*this->pending_.mode);
  } else if (this->pending_.target_temperature) {
    char temp[8];
    snprintf(temp, sizeof(temp), "%.1f", *this->pending_.target_temperature);
    this->controller_.setTemp(temp);
  } else if (this->pending_.fan_speed) {
    this->controller_.setFanSpeed(*this->pending_.fan_speed);
  } else if (this->pending_.vertical_swing) {
    this->controller_.setVerticalSwing(*this->pending_.vertical_swing);
  } else if (this->pending_.horizontal_swing) {
    this->controller_.setHorizontalSwing(*this->pending_.horizontal_swing);
  } else if (this->pending_.powerful) {
    this->controller_.setPowerful(*this->pending_.powerful);
  } else if (this->pending_.economy) {
    this->controller_.setEconomy(*this->pending_.economy);
  } else if (this->pending_.power) {
    this->controller_.setPower(*this->pending_.power);
  }
}

void FujitsuClimate::on_register_change_(const FujitsuAC::RegistryTable::Register * /*reg*/) {
  this->apply_state_();
  // An ack just landed — let the next pending change attempt immediately
  // instead of waiting up to APPLY_INTERVAL_MS for the throttle to expire.
  this->last_apply_attempt_ms_ = 0;
}

void FujitsuClimate::apply_state_() {
  const auto *power_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::Power));
  const auto *mode_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::Mode));
  const auto *setpoint_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::SetpointTemp));
  const auto *fan_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::FanSpeed));
  const auto *actual_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::ActualTemp));

  if (power_reg == nullptr || mode_reg == nullptr || setpoint_reg == nullptr || fan_reg == nullptr ||
      actual_reg == nullptr) {
    return;
  }

  // ActualTemp raw = (°C + 50.25) × 100; 0 only occurs before the first
  // response, so it doubles as a "registers still uninitialised" gate.
  if (actual_reg->value == 0) {
    return;
  }

  if (this->controller_.isPoweredOn()) {
    climate::ClimateMode mapped_mode;
    if (map_lookup(MODE_MAP, static_cast<FujitsuAC::TFSXW1Enums::Mode>(mode_reg->value), mapped_mode)) {
      this->mode = mapped_mode;
    }
  } else {
    this->mode = climate::CLIMATE_MODE_OFF;
  }

  // Fan mode returns 0xFFFF; keep the last known value.
  if (setpoint_reg->value != 0xFFFF) {
    this->target_temperature = setpoint_reg->value / 10.0f;
  }

  this->current_temperature = (actual_reg->value - 5025) / 100.0f;

  climate::ClimateFanMode mapped_fan;
  if (map_lookup(FAN_MODE_MAP, static_cast<FujitsuAC::TFSXW1Enums::FanSpeed>(fan_reg->value), mapped_fan)) {
    this->fan_mode = mapped_fan;
  }

  const auto *v_swing_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::VerticalSwing));
  const auto *h_swing_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::HorizontalSwing));
  if (v_swing_reg != nullptr && h_swing_reg != nullptr && (this->caps_ & (CAP_VERTICAL_SWING | CAP_HORIZONTAL_SWING))) {
    const bool v = v_swing_reg->value == static_cast<uint16_t>(FujitsuAC::TFSXW1Enums::VerticalSwing::On);
    const bool h = h_swing_reg->value == static_cast<uint16_t>(FujitsuAC::TFSXW1Enums::HorizontalSwing::On);
    this->swing_mode = v && h   ? climate::CLIMATE_SWING_BOTH
                       : v      ? climate::CLIMATE_SWING_VERTICAL
                       : h      ? climate::CLIMATE_SWING_HORIZONTAL
                                : climate::CLIMATE_SWING_OFF;
  }

  const auto *powerful_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::Powerful));
  const auto *economy_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::EconomyMode));
  if (powerful_reg != nullptr && economy_reg != nullptr && (this->caps_ & (CAP_BOOST | CAP_ECO))) {
    if (powerful_reg->value == static_cast<uint16_t>(FujitsuAC::TFSXW1Enums::Powerful::On)) {
      this->preset = climate::CLIMATE_PRESET_BOOST;
    } else if (economy_reg->value == static_cast<uint16_t>(FujitsuAC::TFSXW1Enums::EconomyMode::On)) {
      this->preset = climate::CLIMATE_PRESET_ECO;
    } else {
      this->preset = climate::CLIMATE_PRESET_NONE;
    }
  }

  this->publish_state();
}

void FujitsuClimate::check_capabilities_() {
  if (this->caps_checked_) {
    return;
  }

  // The feature-support registers arrive during the init handshake, before the
  // first FrameA response populates ActualTemp. So a non-zero ActualTemp means
  // detection is complete and a 0 in a support register genuinely means
  // "unsupported" rather than "not read yet".
  const auto *actual_reg = this->controller_.getRegister(static_cast<uint16_t>(Address::ActualTemp));
  if (actual_reg == nullptr || actual_reg->value == 0) {
    return;
  }
  this->caps_checked_ = true;

  uint8_t detected = 0;
  if (this->controller_.isFeatureSupported(Address::VerticalSwingSupported)) {
    detected |= CAP_VERTICAL_SWING;
  }
  if (this->controller_.isFeatureSupported(Address::HorizontalSwingSupported)) {
    detected |= CAP_HORIZONTAL_SWING;
  }
  if (this->controller_.isFeatureSupported(Address::PowerfulSupported)) {
    detected |= CAP_BOOST;
  }
  if (this->controller_.isFeatureSupported(Address::EconomyModeSupported)) {
    detected |= CAP_ECO;
  }

  if (detected == this->caps_) {
    ESP_LOGI(TAG, "Capabilities confirmed: 0x%02X", this->caps_);
    return;
  }

  // The unit's capabilities changed (e.g. the ESP32 was moved to a different
  // unit). Traits are reported to HA at connect time only, so persist the new
  // set and reboot to re-advertise; the controls match the current unit after
  // this boot.
  ESP_LOGI(TAG, "Capabilities changed (0x%02X -> 0x%02X), persisting and rebooting", this->caps_, detected);
  this->caps_ = detected;
  this->caps_pref_.save(&this->caps_);
  global_preferences->sync();
  App.safe_reboot();
}

}  // namespace esphome::fujitsu_ac

// Protocol sources compiled from the repo Arduino library (../../src).
#include "RegistryTable.cpp"
#include "Buffer.cpp"
#include "TFSXW1Controller.cpp"
