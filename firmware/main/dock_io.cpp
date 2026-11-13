#include "dock_io.hpp"

#include <cstdint>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace fw {

namespace {

const char* TAG = "dock";

// INA226 registers. Its shunt voltage is 2.5 uV a count and its bus voltage 1.25 mV a count.
constexpr std::uint8_t kConfig = 0x00, kShunt = 0x01, kBus = 0x02;
constexpr std::uint16_t kConfigValue = 0x4527;  // 16-sample average, 1.1 ms conversions, both, continuous

bool read16(i2c_master_dev_handle_t dev, std::uint8_t reg, std::int32_t* out, bool is_signed) {
    std::uint8_t buf[2];
    if (i2c_master_transmit_receive(dev, &reg, 1, buf, 2, 20) != ESP_OK) return false;
    const std::uint16_t v = static_cast<std::uint16_t>((buf[0] << 8) | buf[1]);
    *out = is_signed ? static_cast<std::int16_t>(v) : v;
    return true;
}

}  // namespace

void DockIo::start() {
    fitted_ = CONFIG_LEOTA_DOCK_SWITCH_PIN >= 0 && CONFIG_LEOTA_DOCK_SDA_PIN >= 0 && CONFIG_LEOTA_DOCK_SCL_PIN >= 0;
    if (!fitted_) {
        ESP_LOGI(TAG, "No dock charger set.");
        return;
    }
    const int pin = CONFIG_LEOTA_DOCK_SWITCH_PIN;
    const auto sw = static_cast<gpio_num_t>(pin);
    gpio_set_level(sw, 0);
    gpio_config_t out = {};
    out.pin_bit_mask = pin >= 0 ? 1ULL << pin : 0;
    out.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_set_level(sw, 0);

    // Its own bus, port 0: the board's touch panel has port 1.
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = 0;
    bc.sda_io_num = static_cast<gpio_num_t>(CONFIG_LEOTA_DOCK_SDA_PIN);
    bc.scl_io_num = static_cast<gpio_num_t>(CONFIG_LEOTA_DOCK_SCL_PIN);
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = 1;
    i2c_master_bus_handle_t bus = nullptr;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bc, &bus));
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = static_cast<std::uint16_t>(CONFIG_LEOTA_DOCK_INA226_ADDR);
    dc.scl_speed_hz = 100'000;
    i2c_master_dev_handle_t dev = nullptr;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dc, &dev));
    dev_ = dev;
    xTaskCreatePinnedToCore(task, "dock", 3072, this, 4, nullptr, 0);
}

void DockIo::set_power(bool on) {
    on_ = on;
    if (fitted_) gpio_set_level(static_cast<gpio_num_t>(CONFIG_LEOTA_DOCK_SWITCH_PIN), on ? 1 : 0);
}

void DockIo::task(void* self) { static_cast<DockIo*>(self)->run(); }

void DockIo::run() {
    auto dev = static_cast<i2c_master_dev_handle_t>(dev_);
    bool configured = false;
    int failures = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!configured) {
            const std::uint8_t cfg[3] = {kConfig, static_cast<std::uint8_t>(kConfigValue >> 8),
                                         static_cast<std::uint8_t>(kConfigValue & 0xFF)};
            configured = i2c_master_transmit(dev, cfg, sizeof cfg, 20) == ESP_OK;
            if (!configured) continue;
        }
        std::int32_t shunt = 0, bus = 0;
        if (!read16(dev, kShunt, &shunt, true) || !read16(dev, kBus, &bus, false)) {
            if (++failures == 10) ESP_LOGW(TAG, "The dock's power monitor isn't answering.");
            ok_ = false;
            configured = failures < 10 && configured;
            continue;
        }
        failures = 0;
        ok_ = true;
        const double amps = shunt * 2.5e-6 / (CONFIG_LEOTA_DOCK_SHUNT_MOHM / 1000.0);
        const double volts = bus * 1.25e-3;
        watts_ = volts * (amps > 0 ? amps : 0.0);
    }
}

}  // namespace fw
