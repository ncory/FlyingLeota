// Four HX711 load cell amplifiers sharing one clock pin, read together about 80 times a
// second by their own task. Readings are newtons from the nominal scale in menuconfig;
// the rig corrects them with Setup's line test (config().sensors).
#pragma once

#include <array>
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "leota/config.hpp"

namespace fw {

class LoadCells {
public:
    void start();
    bool fitted() const { return sck_ >= 0; }
    bool fitted(int line) const { return sck_ >= 0 && dout_[line] >= 0; }
    double newtons(int line) const;          // the latest reading, nominal N
    std::int32_t raw(int line) const;        // counts, for the console
    std::uint32_t readings() const { return readings_; }
    std::uint32_t stale_ms() const;          // since the last complete reading

private:
    static void task(void* self);
    void run();
    bool read_all();

    int sck_ = -1;
    std::array<int, leota::kLines> dout_{-1, -1, -1, -1};
    std::array<std::int32_t, leota::kLines> raw_{};
    volatile std::uint32_t readings_ = 0;
    volatile std::int64_t last_us_ = 0;
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

}  // namespace fw
