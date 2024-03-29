#include "soyosource_inverter.h"
#include "esphome/core/log.h"

namespace esphome {
namespace soyosource_inverter {

static const char *const TAG = "soyosource_inverter";

static const uint8_t OPERATION_STATUS_SIZE = 13;
static const char *const OPERATION_STATUS[OPERATION_STATUS_SIZE] = {
    "Normal",                  // 0x00
    "Startup",                 // 0x01
    "Standby",                 // 0x02
    "Startup aborted",         // 0x03
    "Error or battery mode?",  // 0x04
    "Unknown (0x05)",          // 0x05
    "Unknown (0x06)",          // 0x06
    "Unknown (0x07)",          // 0x07
    "Unknown (0x08)",          // 0x08
    "Unknown (0x09)",          // 0x09
    "AC voltage too low",      // 0x10
    "Unknown (0x11)",          // 0x11
    "Unknown (0x12)",          // 0x12
};

void SoyosourceInverter::on_soyosource_modbus_data(const std::vector<uint8_t> &data) {
  if (data.size() != 10) {
    ESP_LOGW(TAG, "'%s': Invalid status frame length!", this->get_modbus_name());
    return;
  }

  ESP_LOGI(TAG, "Status frame (RS485, %d bytes) received", data.size());
  ESP_LOGD(TAG, "  %s", format_hex_pretty(&data.front(), data.size()).c_str());

  auto soyosource_get_16bit = [&](size_t i) -> uint16_t {
    return (uint16_t(data[i + 0]) << 8) | (uint16_t(data[i + 1]) << 0);
  };

  uint8_t raw_operation_status = data[0];
  float operation_status_id = (float) raw_operation_status;

  uint16_t raw_battery_voltage = soyosource_get_16bit(1);
  float battery_voltage = raw_battery_voltage * 0.1f;

  uint16_t raw_battery_current = soyosource_get_16bit(3);
  float battery_current = raw_battery_current * 0.1f;
  float battery_power = battery_voltage * battery_current;

  uint16_t raw_ac_voltage = soyosource_get_16bit(5);
  float ac_voltage = raw_ac_voltage * 1.0f;

  float ac_frequency = data[7] * 0.5f;
  float temperature = (soyosource_get_16bit(8) - 300) * 0.1f;

  if (battery_power > 2500 || ac_voltage > 300 || ac_frequency > 100 || temperature > 200) {
    ESP_LOGW(TAG, "'%s': Frame dropped because of unlikely measurements!", this->get_modbus_name());
    return;
  }

  // https://secondlifestorage.com/index.php?threads/limiter-inverter-with-rs485-load-setting.7631/
  // Soyosource status frame:          0x23 0x01 0x01 0x00 0x00 0x01 0xDB 0x00 0xA1 0x00 0xDD 0x64 0x02 0xCA 0x01
  // Full frame bytes:                   0    1    2    3    4    5    6    7    8    9   10   11   12   13   14
  // Data bytes:                                             0    1    2    3    4    5    6    7    8    9   CRC
  //
  // Response of my broken soyosource: 0x23 0x01 0x01 0x00 0x04 0x02 0xBD 0x00 0x46 0x00 0xEF 0x64 0x02 0x43 0x20
  //                                     0    1    2    3    4    5    6    7    8    9   10   11   12   13   14
  // Data bytes:                                             0    1    2    3    4    5    6    7    8    9   CRC
  //
  //
  // Data byte 0:          0x04 (Operation status)  0x00 (Normal), 0x01 (Startup), 0x02 (Standby),
  //                                                0x03 (Startup aborted?), 0x04 (Error or battery mode?)
  // Data byte 1...2: 0x01 0xDB (Battery voltage)
  // Data byte 3...4: 0x00 0xA1 (Battery current)
  // Data byte 5...6: 0x00 0xDD (AC voltage)
  // Data byte 7:          0x64 (AC frequency / 2)
  // Data byte 8...9: 0x02 0xCA (Temperature / 10.0 + 2 * 10)
  this->publish_state_(this->operation_status_id_sensor_, operation_status_id);
  this->publish_state_(this->battery_voltage_sensor_, battery_voltage);
  this->publish_state_(this->battery_current_sensor_, battery_current);
  this->publish_state_(this->battery_power_sensor_, battery_power);
  this->publish_state_(this->ac_voltage_sensor_, ac_voltage);
  this->publish_state_(this->ac_frequency_sensor_, ac_frequency);
  this->publish_state_(this->temperature_sensor_, temperature);
  this->publish_state_(this->fan_running_binary_sensor_, (bool) (temperature >= 45.0));

  if (raw_operation_status < OPERATION_STATUS_SIZE) {
    this->publish_state_(this->operation_status_text_sensor_, OPERATION_STATUS[raw_operation_status]);
  } else {
    this->publish_state_(this->operation_status_text_sensor_,
                         str_snprintf("Unknown (0x%02X)", 15, raw_operation_status));
  }

  this->no_response_count_ = 0;
}

void SoyosourceInverter::publish_device_offline_() {
  this->publish_state_(this->operation_status_id_sensor_, -1);
  this->publish_state_(this->battery_voltage_sensor_, NAN);
  this->publish_state_(this->battery_current_sensor_, NAN);
  this->publish_state_(this->battery_power_sensor_, NAN);
  this->publish_state_(this->ac_voltage_sensor_, NAN);
  this->publish_state_(this->ac_frequency_sensor_, NAN);
  this->publish_state_(this->temperature_sensor_, NAN);
  // this->publish_state_(this->fan_running_binary_sensor_, NAN);

  this->publish_state_(this->operation_status_text_sensor_, "Offline");
}

void SoyosourceInverter::update() {
  if (this->no_response_count_ >= NO_RESPONSE_THRESHOLD) {
    this->publish_device_offline_();
    ESP_LOGW(TAG, "'%s': The inverter didn't respond to the last %d status requests", this->get_modbus_name(),
             this->no_response_count_);
    this->no_response_count_ = 0;
  }

  no_response_count_++;
  this->query_status();
}

void SoyosourceInverter::publish_state_(binary_sensor::BinarySensor *binary_sensor, const bool &state) {
  if (binary_sensor == nullptr)
    return;

  binary_sensor->publish_state(state);
}

void SoyosourceInverter::publish_state_(sensor::Sensor *sensor, float value) {
  if (sensor == nullptr)
    return;

  sensor->publish_state(value);
}

void SoyosourceInverter::publish_state_(text_sensor::TextSensor *text_sensor, const std::string &state) {
  if (text_sensor == nullptr)
    return;

  text_sensor->publish_state(state);
}

void SoyosourceInverter::dump_config() {
  ESP_LOGCONFIG(TAG, "SoyosourceInverter:");
  ESP_LOGCONFIG(TAG, "  Address: 0x%02X", this->address_);
  LOG_SENSOR("", "Operation Status ID", this->operation_status_id_sensor_);
  LOG_TEXT_SENSOR("", "Operation Status", this->operation_status_text_sensor_);
  LOG_SENSOR("", "Battery Voltage", this->battery_voltage_sensor_);
  LOG_SENSOR("", "Battery Current", this->battery_current_sensor_);
  LOG_SENSOR("", "Battery Power", this->battery_power_sensor_);
  LOG_SENSOR("", "AC Voltage", this->ac_voltage_sensor_);
  LOG_SENSOR("", "AC Frequency", this->ac_frequency_sensor_);
  LOG_SENSOR("", "Temperature", this->temperature_sensor_);
  LOG_BINARY_SENSOR("", "Fan Running", this->fan_running_binary_sensor_);
}

}  // namespace soyosource_inverter
}  // namespace esphome
