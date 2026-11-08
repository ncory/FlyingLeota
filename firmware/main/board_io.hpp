// The safety I/O on the controller's own pins: the E-stop chain's status, the
// Local/Remote key, drive enable with its watchdog charge pump, the brake relay, and the
// drives' alarm inputs. Pins come from menuconfig (Flying Leota, Safety I/O); -1 means
// not fitted.
#pragma once

#include <array>

#include "leota/config.hpp"

namespace fw {

class BoardIo {
public:
    // `real_drives`: with motors fitted, a missing E-stop input reads as an open chain,
    // so the rig can't arm without one.
    void init(bool real_drives);

    bool estop_fitted() const { return estop_pin_ >= 0; }
    bool estop_open() const;
    bool key_fitted() const { return key_pin_ >= 0; }
    bool key_remote() const;
    bool alarm_fitted(int line) const { return alarm_pin_[line] >= 0; }
    bool alarm(int line) const;

    void set_drives_enabled(bool on);
    void set_brakes_released(bool on);
    bool drives_enabled() const { return enabled_; }
    bool brakes_released() const { return released_; }
    // From the motion task, every control cycle: toggles the charge pump while enabled.
    void pump_tick();

private:
    bool real_drives_ = false;
    int estop_pin_ = -1, key_pin_ = -1, enable_pin_ = -1, pump_pin_ = -1, brake_pin_ = -1;
    std::array<int, leota::kLines> alarm_pin_{-1, -1, -1, -1};
    volatile bool enabled_ = false, released_ = false;
    bool pump_level_ = false;
};

}  // namespace fw
