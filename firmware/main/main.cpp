// The Flying Leota controller on the ESP32-P4. Start-up, in order:
//   1. Settings from flash (NVS), else the standard ones (core gazebo_config()).
//   2. The safety I/O: drives off, brakes gripping, before anything else runs.
//   3. Load cells, the drives, the rig and the SCMP server.
//   4. The motion task (1 kHz, core 1), the anemometer, Ethernet and SCMP, the screen,
//      and the bench console.
// The rig starts disarmed and unhomed. Nothing moves until someone arms it.
#include <ctime>
#include <cstdlib>

#include "anemometer.hpp"
#include "app.hpp"
#include "board_io.hpp"
#include "drives.hpp"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "load_cells.hpp"
#include "motion.hpp"
#include "network.hpp"
#include "nvs_flash.h"
#include "nvs_store.hpp"
#include "screen.hpp"
#include "sdkconfig.h"

namespace fw {

BoardIo g_io;
LoadCells g_cells;
DockIo g_dock;
NvsStore g_store;
void console_start();

namespace {

const char* TAG = "leota";
SemaphoreHandle_t g_rig_mutex = nullptr;
App g_app;

#ifdef CONFIG_LEOTA_DRIVES_SIM
constexpr bool kRealDrives = false;
#else
constexpr bool kRealDrives = true;
#endif

}  // namespace

App& app() { return g_app; }
double now_s() { return static_cast<double>(esp_timer_get_time()) * 1e-6; }
void rig_lock() { xSemaphoreTake(g_rig_mutex, portMAX_DELAY); }
void rig_unlock() { xSemaphoreGive(g_rig_mutex); }

void start() {
    g_rig_mutex = xSemaphoreCreateMutex();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    static leota::SettingsKeeper keeper(leota::settings_for(leota::gazebo_config()), &g_store);
    const std::string problems = keeper.load();
    if (!problems.empty()) ESP_LOGW(TAG, "Stored settings:\n%s", problems.c_str());

    g_io.init(kRealDrives);
    g_cells.start();
    g_dock.start();
    static std::unique_ptr<Drives> drives = make_drives(keeper.settings().rig, g_io, g_cells);
    drives->attach_dock(&g_dock);
    static leota::Rig rig(keeper.settings().rig, *drives);
    static leota::ScmpServer scmp(rig, "flying-leota 0.1.0");
    scmp.on_config_set = [] { app().keeper->adopt(app().rig->config()); };
    g_app.keeper = &keeper;
    g_app.rig = &rig;
    g_app.scmp = &scmp;
    g_app.drives = drives.get();
    ESP_LOGI(TAG, "Drives: %s.", drives->kind());

    motion_start(g_io);
    anemometer_start();
    network_start(keeper.settings().net);

    setenv("TZ", CONFIG_LEOTA_TIMEZONE, 1);
    tzset();
    if (CONFIG_LEOTA_SNTP_SERVER[0]) {
        esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_LEOTA_SNTP_SERVER);
        esp_netif_sntp_init(&sc);
    }

    screen_start();
    console_start();
    ESP_LOGI(TAG, "Ready.");
}

}  // namespace fw

extern "C" void app_main() { fw::start(); }
