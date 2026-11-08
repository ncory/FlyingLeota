// The touchscreen: the board's 7 in panel through Espressif's board support package and
// LVGL port, showing the same UI as the PC simulator (ui/). LVGL runs in the port's own
// task; this one refreshes the UI from the rig 20 times a second.
#pragma once

namespace fw {

void screen_start();

}  // namespace fw
