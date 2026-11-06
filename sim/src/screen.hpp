// The touchscreen, rendered in memory for the simulator's web page: LVGL draws into a
// 1024 × 600 frame buffer, the page fetches it as a PNG, and sends taps back.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "lvgl.h"

namespace leota_sim {

class Screen {
public:
    static constexpr int kWidth = 1024, kHeight = 600;
    Screen();
    ~Screen();
    lv_display_t* display() const { return disp_; }
    void run();  // lets LVGL redraw and read touches
    unsigned frame() const { return frame_; }
    std::string png() const;
    void touch(int x, int y, bool down) {
        x_ = x;
        y_ = y;
        down_ = down;
    }

private:
    static void flush(lv_display_t* d, const lv_area_t* area, uint8_t* px);
    static void read(lv_indev_t* in, lv_indev_data_t* data);
    lv_display_t* disp_ = nullptr;
    lv_indev_t* indev_ = nullptr;
    std::vector<uint16_t> fb_;
    std::vector<uint8_t> draw_;
    unsigned frame_ = 0;
    int x_ = 0, y_ = 0;
    bool down_ = false;
};

}  // namespace leota_sim
