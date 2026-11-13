#include "leota/dock.hpp"

#include <cstdio>

namespace leota {

const char* dock_state_name(DockState s) {
    switch (s) {
        case DockState::Off: return "off";
        case DockState::Empty: return "empty";
        case DockState::Charging: return "charging";
        case DockState::Charged: return "charged";
        case DockState::Fault: return "fault";
    }
    return "off";
}

std::string DockCharger::take_news() {
    std::string n;
    n.swap(news_);
    return n;
}

void DockCharger::power(Hardware& hw, bool on, double now) {
    if (on == on_) return;
    on_ = on;
    (on ? on_since_ : off_since_) = now;
    hw.set_dock_power(on);
}

void DockCharger::fault(Fault f, const std::string& why, double now) {
    if (fault_ == Fault::None) news_ = why;
    fault_ = f;
    retry_at_ = now;
}

void DockCharger::clear(const std::string& why) {
    if (fault_ == Fault::None) return;
    fault_ = Fault::None;
    news_ = why;
}

void DockCharger::update(Hardware& hw, const DockConfig& cfg, bool seated, bool may_charge, double now) {
    fitted_ = hw.dock_fitted();
    if (!fitted_) return;
    const double w = hw.dock_watts();
    watts_ = w;
    char buf[200];

    // A switch that won't turn off: nothing the controller can do but say so.
    if (!on_ && now - off_since_ >= cfg.settle) {
        if (w > cfg.empty_w) {
            if (fault_ != Fault::Stuck)
                fault(Fault::Stuck, "The dock's charger draws power while switched off: check its switch.", now);
        } else if (fault_ == Fault::Stuck) {
            clear("The dock's charger is off again.");
        }
    }
    // Too much, whatever is going on: off at once.
    if (on_ && w > cfg.max_w) {
        std::snprintf(buf, sizeof buf,
                      "The dock's charger drew %.0f W, more than it should: it's off for now. Is something "
                      "metal on the pad?",
                      w);
        fault(Fault::Overload, buf, now);
        retry_at_ = now + cfg.retry;
        probing_ = false;
        power(hw, false, now);
    }

    if (seated) {
        probing_ = false;
        next_probe_ = -1.0;  // the pad is checked only while the ball is away
        // After an overload, or with something found on the pad, try again after a while:
        // the ball may have landed on a clear pad.
        const bool retrying = fault_ == Fault::Overload || fault_ == Fault::Pad;
        const bool waiting = retrying && now < retry_at_;
        power(hw, may_charge && !waiting && fault_ != Fault::Stuck, now);
        if (retrying && on_ && now - on_since_ >= cfg.settle)
            clear("The dock's charger is charging normally again.");
    } else {
        if (next_probe_ < 0.0) next_probe_ = now + cfg.probe_every;
        if (!probing_ && cfg.probe_every > 0.0 && now >= next_probe_ && fault_ != Fault::Stuck &&
            !(fault_ == Fault::Overload && now < retry_at_)) {
            probing_ = true;
            probe_start_ = now;
            power(hw, true, now);
        }
        if (probing_ && now - probe_start_ >= cfg.probe_time) {
            probing_ = false;
            next_probe_ = now + cfg.probe_every;
            if (w > cfg.empty_w) {
                std::snprintf(buf, sizeof buf,
                              "Something is on the dock's charging pad while the ball is away (it drew %.1f W): "
                              "the charger stays off. Metal?",
                              w);
                fault(Fault::Pad, buf, now);
                retry_at_ = now + cfg.retry;
            } else {
                clear("The dock's charging pad is clear again.");
            }
        }
        if (!probing_) power(hw, false, now);
    }

    if (fault_ != Fault::None) state_ = DockState::Fault;
    else if (!seated) state_ = DockState::Empty;
    else if (!on_) state_ = DockState::Off;
    else if (now - on_since_ < cfg.settle) state_ = DockState::Charging;  // until its reading settles
    else state_ = w >= cfg.charging_w ? DockState::Charging : DockState::Charged;
}

}  // namespace leota
