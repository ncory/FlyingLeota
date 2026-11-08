#include "motion.hpp"

#include <algorithm>

#include "app.hpp"
#include "board_io.hpp"
#include "driver/gptimer.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace fw {

namespace {

const char* TAG = "motion";

TaskHandle_t g_task = nullptr;
BoardIo* g_io = nullptr;
volatile std::int64_t g_fired_us = 0;
portMUX_TYPE g_stats_mux = portMUX_INITIALIZER_UNLOCKED;
MotionStats g_stats;
double g_run_sum = 0.0;
std::function<bool(double)> g_bench;  // under the rig lock

bool IRAM_ATTR on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t*, void*) {
    g_fired_us = esp_timer_get_time();
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(g_task, &woken);
    return woken == pdTRUE;
}

void motion_task(void*) {
    bool last_key = true;
    int key_count = 0;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        const std::int64_t woke = esp_timer_get_time();
        const std::int64_t fired = g_fired_us;
        rig_lock();
        const std::int64_t locked = esp_timer_get_time();
        App& a = app();
        a.rig->update(now_s());  // disarmed during a bench test, so it moves nothing
        if (g_bench) {
            // The bench test owns the drives while the rig is disarmed; arming ends it.
            if (a.rig->armed() || !g_bench(0.001)) g_bench = nullptr;
        }
        {
            // The key switch, every 20 ms: Remote lets Show Controller in.
            if (g_io->key_fitted() && ++key_count >= 20) {
                key_count = 0;
                const bool remote = g_io->key_remote();
                if (remote != last_key) {
                    a.rig->set_remote(remote, now_s());
                    a.log(leota::LogKind::Safety, remote ? "Key switch turned to Remote." : "Key switch turned to Local.");
                    last_key = remote;
                }
            }
        }
        rig_unlock();
        g_io->pump_tick();
        const std::int64_t done = esp_timer_get_time();
        const auto run = static_cast<std::uint32_t>(done - woke);
        const auto late = static_cast<std::uint32_t>(std::max<std::int64_t>(0, woke - fired));
        const auto wait = static_cast<std::uint32_t>(locked - woke);
        portENTER_CRITICAL(&g_stats_mux);
        ++g_stats.cycles;
        g_stats.max_run_us = std::max(g_stats.max_run_us, run);
        g_stats.max_late_us = std::max(g_stats.max_late_us, late);
        g_stats.max_wait_us = std::max(g_stats.max_wait_us, wait);
        if (run > 1000) ++g_stats.over_1ms;
        g_run_sum += run;
        portEXIT_CRITICAL(&g_stats_mux);
    }
}

}  // namespace

void motion_start(BoardIo& io) {
    g_io = &io;
    // Highest priority below the system's own, alone on core 1: nothing waits for it
    // except a brief rig lock.
    xTaskCreatePinnedToCore(motion_task, "motion", 8192, nullptr, configMAX_PRIORITIES - 2, &g_task, 1);
    gptimer_handle_t timer = nullptr;
    gptimer_config_t tc = {};
    tc.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    tc.direction = GPTIMER_COUNT_UP;
    tc.resolution_hz = 1'000'000;
    ESP_ERROR_CHECK(gptimer_new_timer(&tc, &timer));
    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = on_alarm;
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, nullptr));
    gptimer_alarm_config_t ac = {};
    ac.alarm_count = 1000;  // 1 ms
    ac.reload_count = 0;
    ac.flags.auto_reload_on_alarm = 1;
    ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &ac));
    ESP_ERROR_CHECK(gptimer_enable(timer));
    ESP_ERROR_CHECK(gptimer_start(timer));
    ESP_LOGI(TAG, "Running every 1 ms on core 1.");
}

MotionStats motion_stats() {
    portENTER_CRITICAL(&g_stats_mux);
    MotionStats s = g_stats;
    s.mean_run_us = s.cycles ? g_run_sum / static_cast<double>(s.cycles) : 0.0;
    portEXIT_CRITICAL(&g_stats_mux);
    return s;
}

void motion_reset_stats() {
    portENTER_CRITICAL(&g_stats_mux);
    g_stats = MotionStats{};
    g_run_sum = 0.0;
    portEXIT_CRITICAL(&g_stats_mux);
}

bool motion_bench(std::function<bool(double)> step) {
    RigLock lock;
    if (g_bench || app().rig->armed()) return false;
    g_bench = std::move(step);
    return true;
}

bool motion_bench_running() {
    RigLock lock;
    return static_cast<bool>(g_bench);
}

}  // namespace fw
