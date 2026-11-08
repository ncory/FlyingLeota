#include "load_cells.hpp"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace fw {

namespace {
const char* TAG = "loadcells";
}

void LoadCells::start() {
    sck_ = CONFIG_LEOTA_HX711_SCK_PIN;
    dout_ = {CONFIG_LEOTA_HX711_DOUT_PIN_A, CONFIG_LEOTA_HX711_DOUT_PIN_B, CONFIG_LEOTA_HX711_DOUT_PIN_C,
             CONFIG_LEOTA_HX711_DOUT_PIN_D};
    if (sck_ < 0) {
        ESP_LOGW(TAG, "No load cells set: every line reads 0 N.");
        return;
    }
    gpio_set_level(static_cast<gpio_num_t>(sck_), 0);
    gpio_config_t out = {};
    out.pin_bit_mask = 1ULL << sck_;
    out.mode = GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&out));
    for (int p : dout_) {
        if (p < 0) continue;
        gpio_config_t in = {};
        in.pin_bit_mask = 1ULL << p;
        in.mode = GPIO_MODE_INPUT;
        in.pull_up_en = GPIO_PULLUP_ENABLE;
        ESP_ERROR_CHECK(gpio_config(&in));
    }
    xTaskCreatePinnedToCore(task, "loadcells", 3072, this, 6, nullptr, 0);
}

void LoadCells::task(void* self) { static_cast<LoadCells*>(self)->run(); }

void LoadCells::run() {
    for (;;) {
        // Each HX711 pulls its data line low when a reading is ready (80 per second at
        // most). They run free, so wait until every fitted one is ready, then read all
        // four together on the shared clock.
        bool ready = false;
        for (int waited = 0; waited < 40 && !ready; ++waited) {
            ready = true;
            for (int p : dout_)
                if (p >= 0 && gpio_get_level(static_cast<gpio_num_t>(p)) != 0) ready = false;
            if (!ready) vTaskDelay(1);
        }
        if (!ready || !read_all()) {
            vTaskDelay(2);
            continue;
        }
        vTaskDelay(5);  // the next reading is at least 12 ms away
    }
}

bool LoadCells::read_all() {
    std::array<std::int32_t, leota::kLines> v{};
    const auto sck = static_cast<gpio_num_t>(sck_);
    // 24 data bits, then one more pulse to select channel A at gain 128 for the next
    // reading. The clock must never stay high for 60 us (the chip would power down), so
    // no interrupts on this core while it runs: about 60 us in all.
    portMUX_TYPE local = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&local);
    for (int bit = 0; bit < 25; ++bit) {
        gpio_set_level(sck, 1);
        esp_rom_delay_us(1);
        if (bit < 24)
            for (int i = 0; i < leota::kLines; ++i)
                if (dout_[i] >= 0) v[i] = (v[i] << 1) | gpio_get_level(static_cast<gpio_num_t>(dout_[i]));
        gpio_set_level(sck, 0);
        esp_rom_delay_us(1);
    }
    portEXIT_CRITICAL(&local);
    for (auto& x : v)
        if (x & 0x800000) x -= 0x1000000;  // 24-bit two's complement
    portENTER_CRITICAL(&mux_);
    raw_ = v;
    portEXIT_CRITICAL(&mux_);
    last_us_ = esp_timer_get_time();
    readings_ = readings_ + 1;
    return true;
}

double LoadCells::newtons(int line) const {
    if (!fitted(line)) return 0.0;
    return static_cast<double>(raw(line)) / CONFIG_LEOTA_HX711_COUNTS_PER_N;
}

std::int32_t LoadCells::raw(int line) const {
    portENTER_CRITICAL(&mux_);
    const std::int32_t r = raw_[line];
    portEXIT_CRITICAL(&mux_);
    return r;
}

std::uint32_t LoadCells::stale_ms() const {
    if (!fitted() || last_us_ == 0) return UINT32_MAX;
    return static_cast<std::uint32_t>((esp_timer_get_time() - last_us_) / 1000);
}

}  // namespace fw
