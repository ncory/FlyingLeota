#include "board_io.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace fw {

namespace {

const char* TAG = "io";

void input(int pin) {
    if (pin < 0) return;
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode = GPIO_MODE_INPUT;
    c.pull_up_en = GPIO_PULLUP_DISABLE;  // the carrier board's opto-isolators drive these
    c.pull_down_en = GPIO_PULLDOWN_DISABLE;
    c.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&c));
}

void output(int pin) {
    if (pin < 0) return;
    gpio_set_level(static_cast<gpio_num_t>(pin), 0);  // off before it's an output
    gpio_config_t c = {};
    c.pin_bit_mask = 1ULL << pin;
    c.mode = GPIO_MODE_OUTPUT;
    c.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&c));
    gpio_set_level(static_cast<gpio_num_t>(pin), 0);
}

int level(int pin) { return gpio_get_level(static_cast<gpio_num_t>(pin)); }

}  // namespace

void BoardIo::init(bool real_drives) {
    real_drives_ = real_drives;
    estop_pin_ = CONFIG_LEOTA_ESTOP_PIN;
    key_pin_ = CONFIG_LEOTA_KEY_PIN;
    enable_pin_ = CONFIG_LEOTA_ENABLE_PIN;
    pump_pin_ = CONFIG_LEOTA_PUMP_PIN;
    brake_pin_ = CONFIG_LEOTA_BRAKE_PIN;
#ifdef CONFIG_LEOTA_DRIVES_STEPDIR
    alarm_pin_ = {CONFIG_LEOTA_ALARM_PIN_A, CONFIG_LEOTA_ALARM_PIN_B, CONFIG_LEOTA_ALARM_PIN_C,
                  CONFIG_LEOTA_ALARM_PIN_D};
#endif
    // Outputs first, all off: drives disabled, brakes gripping.
    output(enable_pin_);
    output(pump_pin_);
    output(brake_pin_);
    input(estop_pin_);
    input(key_pin_);
    for (int p : alarm_pin_) input(p);
    if (!estop_fitted())
        ESP_LOGW(TAG, real_drives ? "No E-stop input set: the chain reads open, and the rig won't arm."
                                  : "No E-stop input set (fine for the simulated rig).");
    if (enable_pin_ >= 0 && pump_pin_ < 0)
        ESP_LOGW(TAG, "Drive enable without a charge pump: a hung controller won't drop the drives.");
}

bool BoardIo::estop_open() const {
    if (estop_pin_ < 0) return real_drives_;
    return level(estop_pin_) != CONFIG_LEOTA_ESTOP_OK_LEVEL;
}

bool BoardIo::key_remote() const {
    if (key_pin_ < 0) return true;
    return level(key_pin_) == CONFIG_LEOTA_KEY_REMOTE_LEVEL;
}

bool BoardIo::alarm(int line) const {
    const int p = alarm_pin_[line];
    if (p < 0) return false;
#ifdef CONFIG_LEOTA_ALARM_ACTIVE_LOW
    return level(p) == 0;
#else
    return level(p) == 1;
#endif
}

void BoardIo::set_drives_enabled(bool on) {
    enabled_ = on;
    if (enable_pin_ >= 0) gpio_set_level(static_cast<gpio_num_t>(enable_pin_), on ? 1 : 0);
    if (!on && pump_pin_ >= 0) gpio_set_level(static_cast<gpio_num_t>(pump_pin_), 0);
}

void BoardIo::set_brakes_released(bool on) {
    released_ = on;
    if (brake_pin_ >= 0) gpio_set_level(static_cast<gpio_num_t>(brake_pin_), on ? 1 : 0);
}

void BoardIo::pump_tick() {
    if (pump_pin_ < 0 || !enabled_) return;
    pump_level_ = !pump_level_;
    gpio_set_level(static_cast<gpio_num_t>(pump_pin_), pump_level_ ? 1 : 0);
}

}  // namespace fw
