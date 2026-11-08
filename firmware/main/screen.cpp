#include "screen.hpp"

#include <ctime>

#include "app.hpp"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "leota/ui.hpp"
#include "sdkconfig.h"

namespace fw {

namespace {

const char* TAG = "screen";
leota::Ui* g_ui = nullptr;

std::string clock_text() {
    const std::time_t t = std::time(nullptr);
    std::tm local = {};
    localtime_r(&t, &local);
    if (local.tm_year + 1900 < 2025) return "";  // no time from the network yet
    char buf[8];
    std::strftime(buf, sizeof buf, "%H:%M", &local);
    return buf;
}

void screen_task(void*) {
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        const std::string clock = clock_text();
        if (!bsp_display_lock(100)) continue;
        {
            RigLock lock;  // display lock first, then the rig's: always in that order
            App& a = app();
            while (!a.logs.empty()) {
                g_ui->log(a.logs.front().first, a.logs.front().second);
                a.logs.pop_front();
            }
            const double t = now_s();
            leota::UiLink link;
            link.heard = a.link.last_heard < 0 ? -1.0 : t - a.link.last_heard;
            link.connected = link.heard >= 0 && link.heard < 1.5;
            link.address = a.link.from;
            link.commands = a.link.commands;
            link.own_address = a.link.own_address.empty() ? (a.link.cable ? "waiting for an address" : "no cable")
                                                          : a.link.own_address;
            link.scmp_port = a.keeper->settings().net.scmp_port;
            g_ui->update(t, link, clock);
        }
        bsp_display_unlock();
    }
}

}  // namespace

void screen_start() {
    lv_display_t* disp = bsp_display_start();
    if (!disp) {
        ESP_LOGE(TAG, "The display didn't start: running without a screen.");
        return;
    }
    bsp_display_backlight_on();
    bsp_display_lock(0);
    {
        RigLock lock;
        g_ui = new leota::Ui(*app().rig, disp, app().keeper);
    }
    // Touch handlers run in LVGL's task and reach the rig: they take its lock.
    g_ui->set_lock(rig_lock, rig_unlock);
    bsp_display_unlock();
    xTaskCreatePinnedToCore(screen_task, "screen", 8192, nullptr, 3, nullptr, 0);
    ESP_LOGI(TAG, "Touchscreen running.");
}

}  // namespace fw
