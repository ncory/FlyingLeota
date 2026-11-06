#include "screen.hpp"

#include <chrono>
#include <cstring>

#include <zlib.h>

namespace leota_sim {

namespace {

uint32_t ms_now() {
    static const auto t0 = std::chrono::steady_clock::now();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

void put32(std::string& s, uint32_t v) {
    for (int i = 3; i >= 0; --i) s += static_cast<char>((v >> (8 * i)) & 0xff);
}

void chunk(std::string& out, const char* type, const std::string& data) {
    put32(out, static_cast<uint32_t>(data.size()));
    std::string body = std::string(type, 4) + data;
    out += body;
    put32(out, static_cast<uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()))));
}

}  // namespace

Screen::Screen() : fb_(kWidth * kHeight, 0), draw_(kWidth * 60 * 2) {
    static bool ready = false;
    if (!ready) {
        lv_init();
        lv_tick_set_cb(ms_now);
        ready = true;
    }
    disp_ = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(disp_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp_, draw_.data(), nullptr, static_cast<uint32_t>(draw_.size()), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp_, flush);
    lv_display_set_driver_data(disp_, this);
    indev_ = lv_indev_create();
    lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev_, read);
    lv_indev_set_driver_data(indev_, this);
    lv_indev_set_display(indev_, disp_);
}

Screen::~Screen() {
    lv_indev_delete(indev_);
    lv_display_delete(disp_);
}

void Screen::run() { lv_timer_handler(); }

void Screen::flush(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
    Screen* s = static_cast<Screen*>(lv_display_get_driver_data(d));
    const int w = a->x2 - a->x1 + 1;
    const uint16_t* src = reinterpret_cast<const uint16_t*>(px);
    for (int y = a->y1; y <= a->y2; ++y, src += w)
        std::memcpy(&s->fb_[static_cast<size_t>(y) * kWidth + a->x1], src, static_cast<size_t>(w) * 2);
    if (lv_display_flush_is_last(d)) ++s->frame_;
    lv_display_flush_ready(d);
}

void Screen::read(lv_indev_t* in, lv_indev_data_t* data) {
    const Screen* s = static_cast<const Screen*>(lv_indev_get_driver_data(in));
    data->point.x = s->x_;
    data->point.y = s->y_;
    data->state = s->down_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

std::string Screen::png() const {
    // Rows of RGB, each led by filter byte 0, deflated.
    std::string raw;
    raw.reserve(static_cast<size_t>(kHeight) * (1 + kWidth * 3));
    for (int y = 0; y < kHeight; ++y) {
        raw += '\0';
        for (int x = 0; x < kWidth; ++x) {
            const uint16_t c = fb_[static_cast<size_t>(y) * kWidth + x];
            raw += static_cast<char>(((c >> 11) & 0x1f) * 255 / 31);
            raw += static_cast<char>(((c >> 5) & 0x3f) * 255 / 63);
            raw += static_cast<char>((c & 0x1f) * 255 / 31);
        }
    }
    uLongf len = compressBound(static_cast<uLong>(raw.size()));
    std::string z(len, '\0');
    compress2(reinterpret_cast<Bytef*>(&z[0]), &len, reinterpret_cast<const Bytef*>(raw.data()),
              static_cast<uLong>(raw.size()), 3);
    z.resize(len);
    std::string out = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    put32(ihdr, kWidth);
    put32(ihdr, kHeight);
    ihdr += std::string("\x08\x02\x00\x00\x00", 5);  // 8-bit RGB
    chunk(out, "IHDR", ihdr);
    chunk(out, "IDAT", z);
    chunk(out, "IEND", "");
    return out;
}

}  // namespace leota_sim
