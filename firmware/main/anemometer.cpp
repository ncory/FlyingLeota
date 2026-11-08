#include "anemometer.hpp"

#include <cstdint>

#include "app.hpp"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace fw {

namespace {

const char* TAG = "wind";
volatile double g_speed = -1.0, g_at = -1.0;

std::uint16_t crc16(const std::uint8_t* p, int n) {
    std::uint16_t crc = 0xFFFF;
    for (int i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int b = 0; b < 8; ++b) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    return crc;
}

bool fitted() { return CONFIG_LEOTA_WIND_TX_PIN >= 0 && CONFIG_LEOTA_WIND_RX_PIN >= 0; }

void give(double speed) {
    const double t = now_s();
    g_speed = speed;
    g_at = t;
    RigLock lock;
    app().rig->wind_sample(speed, t);
}

// Modbus RTU "read holding registers" for one register.
bool read_once(uart_port_t port, double* speed) {
    std::uint8_t q[8] = {static_cast<std::uint8_t>(CONFIG_LEOTA_WIND_ADDRESS), 0x03,
                         static_cast<std::uint8_t>(CONFIG_LEOTA_WIND_REGISTER >> 8),
                         static_cast<std::uint8_t>(CONFIG_LEOTA_WIND_REGISTER & 0xFF), 0x00, 0x01, 0, 0};
    const std::uint16_t c = crc16(q, 6);
    q[6] = c & 0xFF;
    q[7] = c >> 8;
    uart_flush_input(port);
    uart_write_bytes(port, q, sizeof q);
    std::uint8_t r[7];
    const int n = uart_read_bytes(port, r, sizeof r, pdMS_TO_TICKS(150));
    if (n != 7 || r[0] != q[0] || r[1] != 0x03 || r[2] != 2) return false;
    if (crc16(r, 5) != (r[5] | (r[6] << 8))) return false;
    *speed = ((r[3] << 8) | r[4]) / static_cast<double>(CONFIG_LEOTA_WIND_DIVISOR);
    return true;
}

void task(void*) {
    const auto port = static_cast<uart_port_t>(CONFIG_LEOTA_WIND_UART);
    int failures = 0;
    for (;;) {
        double speed = 0.0;
        if (read_once(port, &speed)) {
            failures = 0;
            give(speed);
        } else if (++failures == 8) {
            // The rig treats a silent sensor as Red after its own timeout.
            ESP_LOGW(TAG, "The anemometer isn't answering.");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

#ifdef CONFIG_LEOTA_DRIVES_SIM
void calm_task(void*) {
    for (;;) {
        give(1.5);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
#endif

}  // namespace

bool anemometer_fitted() { return fitted(); }

double anemometer_last(double* age_s) {
    if (age_s) *age_s = g_at < 0 ? -1.0 : now_s() - g_at;
    return g_speed;
}

void anemometer_start() {
    if (!fitted()) {
#ifdef CONFIG_LEOTA_DRIVES_SIM
        ESP_LOGW(TAG, "No anemometer set: feeding the simulated rig a calm 1.5 m/s.");
        xTaskCreatePinnedToCore(calm_task, "wind", 3072, nullptr, 4, nullptr, 0);
#else
        ESP_LOGW(TAG, "No anemometer set: the wind reads Red until Setup says no sensor is fitted.");
#endif
        return;
    }
    const auto port = static_cast<uart_port_t>(CONFIG_LEOTA_WIND_UART);
    uart_config_t c = {};
    c.baud_rate = CONFIG_LEOTA_WIND_BAUD;
    c.data_bits = UART_DATA_8_BITS;
    c.parity = UART_PARITY_DISABLE;
    c.stop_bits = UART_STOP_BITS_1;
    c.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    c.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_driver_install(port, 256, 0, 0, nullptr, 0));
    ESP_ERROR_CHECK(uart_param_config(port, &c));
    // The transceiver's DE/RE pin is driven as RTS in half-duplex RS-485 mode.
    ESP_ERROR_CHECK(uart_set_pin(port, CONFIG_LEOTA_WIND_TX_PIN, CONFIG_LEOTA_WIND_RX_PIN, CONFIG_LEOTA_WIND_DE_PIN,
                                 UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_mode(port, UART_MODE_RS485_HALF_DUPLEX));
    xTaskCreatePinnedToCore(task, "wind", 3072, nullptr, 4, nullptr, 0);
}

}  // namespace fw
