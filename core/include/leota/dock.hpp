// The dock's charger, as the controller runs it (SCMP's optional "dock" field).
//
// - On only while the ball rests on the dock and the rig is disarmed.
// - What it draws says what's happening: above `charging_w` the ball is charging; below
//   it, charged (a trickle).
// - Drawing more than `max_w` is a fault: off at once, and tried again after `retry`.
// - While the ball is away, the pad should be empty. Every `probe_every` seconds the
//   charger goes on for `probe_time`: drawing more than `empty_w` then means something
//   else is on the pad (metal heats in the field), a fault, and it stays off. The next
//   check that finds the pad empty clears it; so does charging normally once the ball
//   lands, tried after `retry`.
// - Drawing power while switched off means the switch has stuck: a fault until it reads
//   nothing again.
#pragma once

#include <string>

#include "leota/config.hpp"
#include "leota/hardware.hpp"

namespace leota {

enum class DockState { Off, Empty, Charging, Charged, Fault };
const char* dock_state_name(DockState s);  // "off", "empty", "charging", "charged", "fault"

class DockCharger {
public:
    // Call often (every 10 ms or so). `seated`: the ball rests on the dock. `may_charge`:
    // the rig allows it (disarmed).
    void update(Hardware& hw, const DockConfig& cfg, bool seated, bool may_charge, double now);

    bool fitted() const { return fitted_; }
    bool on() const { return on_; }
    DockState state() const { return state_; }
    double watts() const { return watts_; }
    // A sentence when something worth saying happens (a fault, or one clearing); "" if
    // nothing new since the last call.
    std::string take_news();

private:
    enum class Fault { None, Overload, Pad, Stuck };
    void power(Hardware& hw, bool on, double now);
    void fault(Fault f, const std::string& why, double now);
    void clear(const std::string& why);

    bool fitted_ = false, on_ = false, probing_ = false;
    double on_since_ = 0.0, off_since_ = 0.0, probe_start_ = 0.0, next_probe_ = -1.0, retry_at_ = 0.0;
    double watts_ = 0.0;
    Fault fault_ = Fault::None;
    DockState state_ = DockState::Off;
    std::string news_;
};

}  // namespace leota
