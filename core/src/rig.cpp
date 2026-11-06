#include "leota/rig.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace leota {

const char* faults::name(std::uint32_t f) {
    switch (f) {
        case EStop: return "estop";
        case HeartbeatLost: return "heartbeat_lost";
        case SlackLine: return "slack_line";
        case Overload: return "overload";
        case TensionMismatch: return "tension_mismatch";
        case DriveAlarm: return "drive_alarm";
        case Wind: return "wind";
        case HomingFailed: return "homing_failed";
    }
    return "fault";
}

namespace {

constexpr double kTrimDecay = 0.3;  // s: how fast trims fade near the dock

std::string fmt(const char* f, double a, double b = 0.0, double c = 0.0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

std::string fault_list(std::uint32_t f) {
    std::string s;
    for (std::uint32_t bit = 1; bit <= faults::kAll; bit <<= 1)
        if (f & bit) s += (s.empty() ? "" : ", ") + std::string(faults::name(bit));
    return s;
}

double component(const Vec3& v, int axis) { return axis == 0 ? v.x : axis == 1 ? v.y : v.z; }

}  // namespace

Rig::Rig(const RigConfig& cfg, Hardware& hw) : cfg_(cfg), hw_(hw), wind_(cfg.wind) {
    window_ = std::max(1, static_cast<int>(std::lround(cfg_.smoothing / cfg_.control_period)));
    for (int a = 0; a < kAxes; ++a) axes_[a].reset(component(cfg_.dock, a), window_);
    hw_.set_brakes_released(false);
    hw_.set_enabled(false);
    fb_ = hw_.feedback();
    hold_drives();
    message_ = "Started. Arm, then home at the dock before flying.";
}

// -- state ---------------------------------------------------------------------------

Vec3 Rig::position() const { return {axes_[0].output(), axes_[1].output(), axes_[2].output()}; }

Vec3 Rig::velocity() const {
    return {axes_[0].output_velocity(), axes_[1].output_velocity(), axes_[2].output_velocity()};
}

bool Rig::busy() const {
    for (const AxisMotion& a : axes_)
        if (!a.settled()) return true;
    return false;
}

std::array<LineStatus, kLines> Rig::lines() const {
    std::array<LineStatus, kLines> out{};
    for (int i = 0; i < kLines; ++i) {
        LineStatus& l = out[i];
        l.name = cfg_.lines[i].name;
        l.tension = fb_[i].tension;
        l.expected = expected_[i];
        l.length = homed_ ? (fb_[i].position + offset_[i]) * (1.0 + fb_[i].tension / cfg_.line_ea) : 0.0;
        l.swing = swing_[i];
        l.trim = trim_[i] + pull_ * free_[i];
        l.ready = fb_[i].ready;
        l.alarm = fb_[i].alarm;
    }
    return out;
}

bool Rig::flying() const {
    if (!homed_ || !drives_on_ || !brakes_released_ || state_ == RigState::Arming) return false;
    if (state_ == RigState::Homing && (home_ == Home::Reel || home_ == Home::Settle)) return false;
    return position().z >= cfg_.dock.z + cfg_.dock_zone;
}

double Rig::speed_scale() const {
    return cfg_.wind.enabled && wind_.level() == WindLevel::Amber ? cfg_.wind.amber_speed : 1.0;
}

Box Rig::allowed_box() const {
    Box b = workspace(cfg_);
    if (cfg_.wind.enabled && wind_.level() == WindLevel::Amber) {
        const Box& a = cfg_.wind.amber_box;
        b.lo = {std::max(b.lo.x, a.lo.x), std::max(b.lo.y, a.lo.y), std::max(b.lo.z, a.lo.z)};
        b.hi = {std::min(b.hi.x, a.hi.x), std::min(b.hi.y, a.hi.y), std::min(b.hi.z, a.hi.z)};
        if (cfg_.two_axis) b.lo.y = b.hi.y = cfg_.fixed_y;
    }
    return b;
}

void Rig::hold_drives() {
    for (int i = 0; i < kLines; ++i) cmd_[i] = fb_[i].position;
}

// -- commands --------------------------------------------------------------------------

Result Rig::arm(double) {
    if (estop_ || hw_.estop_open()) return {Refusal::EStop, "The E-stop chain is open."};
    if (cfg_.wind.enabled) {
        if (wind_.holding())
            return {Refusal::Weather,
                    fmt("The wind was Red. Arming waits until it has been Green for %.0f more minutes.",
                        std::ceil(wind_.calm_left() / 60.0))};
        if (!wind_.ever_read()) return {Refusal::Weather, "Waiting for the wind sensor."};
        if (wind_.level() == WindLevel::Red) return {Refusal::Weather, "The wind is Red."};
    }
    if (armed_) return {};
    if (disarm_ != Disarm::None) return {Refusal::Busy, "Still disarming."};
    faults_ = 0;
    state_ = RigState::Arming;
    armed_ = true;
    arm_ = Arm::Enable;
    phase_t_ = now_;
    if (!homed_) hold_drives();
    drives_on_ = true;
    hw_.set_enabled(true);
    message_ = "Arming.";
    return {};
}

Result Rig::disarm(double) {
    if (!armed_ || disarm_ != Disarm::None) return {};
    message_ = "Disarming.";
    begin_disarm();
    return {};
}

Result Rig::home(double) {
    if (estop_) return {Refusal::EStop, "The E-stop chain is open."};
    if (state_ == RigState::Fault) return {Refusal::Fault, fault_list(faults_)};
    if (!armed_) return {Refusal::Disarmed, "Arm first."};
    if (state_ == RigState::Arming) return {Refusal::Busy, "Still arming."};
    if (state_ == RigState::Homing) return {Refusal::Busy, "Already homing."};
    if (state_ != RigState::Idle || park_ != Park::None) return {Refusal::Busy, "Can't home while moving."};
    if (homed_) {
        const Vec3 off = position() - cfg_.dock;
        if (std::sqrt(off.x * off.x + off.y * off.y) > 5.0 || off.z > 5.0 ||
            off.z < -cfg_.park_sink - 5.0)
            return {Refusal::Busy, "Park first: homing starts with the ball on the dock."};
    }
    state_ = RigState::Homing;
    home_ = Home::Reel;
    tight_ = {};
    reeled_ = {};
    trim_ = {};
    pull_ = 0.0;
    homed_ = false;
    hold_drives();
    message_ = "Homing at the dock.";
    return {};
}

Result Rig::move(int axis, double target, std::optional<double> velocity,
                 std::optional<double> duration, std::optional<double> accel, double) {
    static const char* names[kAxes] = {"x", "y", "z"};
    if (!axis_present(axis)) return {Refusal::NoAxis, "There's no such axis."};
    if (estop_) return {Refusal::EStop, "The E-stop chain is open."};
    if (state_ == RigState::Fault) return {Refusal::Fault, fault_list(faults_)};
    if (!armed_) return {Refusal::Disarmed, "Arm first."};
    if (!homed_) return {Refusal::NotHomed, "Home at the dock first."};
    if (state_ == RigState::Homing) return {Refusal::Busy, "Homing."};
    if (park_ != Park::None) return {Refusal::Busy, "Parking on the dock."};
    const AxisLimits& lim = cfg_.axes[axis];
    if (target < lim.min || target > lim.max)
        return {Refusal::OutOfRange,
                std::string(names[axis]) + fmt(" %g is outside %g to %g.", target, lim.min, lim.max)};
    Vec3 to{axes_[0].end(), axes_[1].end(), axes_[2].end()};
    (axis == 0 ? to.x : axis == 1 ? to.y : to.z) = target;
    if (speed_scale() < 1.0 && !allowed_box().contains(to))
        return {Refusal::Weather, "The wind is Amber: the ball must stay low and central."};
    const Spot s = check_path(cfg_, position(), to, workspace(cfg_));
    if (s != Spot::Ok) return {Refusal::OutOfRange, std::string("That path goes where ") + spot_text(s) + "."};

    double vmax = lim.max_velocity * speed_scale();
    if (velocity) vmax = std::min(*velocity, vmax);
    double a = lim.max_accel;
    if (accel) a = std::min(*accel, a);
    Result r;
    if (duration) {
        const double dist = std::fabs(target - axes_[axis].planned());
        const double v = timed_speed(dist, std::max(0.0, *duration - cfg_.smoothing), vmax, a, nullptr);
        axes_[axis].move(target, v, a);
        const double late = axes_[axis].plan_duration() + cfg_.smoothing - *duration;
        r.late = late > 1e-6 ? late : 0.0;
    } else {
        axes_[axis].move(target, vmax, a);
    }
    if (state_ != RigState::Arming) state_ = RigState::Moving;
    return r;
}

Result Rig::jog(int axis, double velocity, double now) {
    if (!axis_present(axis)) return {Refusal::NoAxis, "There's no such axis."};
    if (estop_) return {Refusal::EStop, "The E-stop chain is open."};
    if (state_ == RigState::Fault) return {Refusal::Fault, fault_list(faults_)};
    if (!armed_) return {Refusal::Disarmed, "Arm first."};
    if (!homed_) return {Refusal::NotHomed, "Home at the dock first."};
    if (state_ == RigState::Homing) return {Refusal::Busy, "Homing."};
    if (park_ != Park::None) return {Refusal::Busy, "Parking on the dock."};
    const AxisLimits& lim = cfg_.axes[axis];
    const double vmax = lim.max_velocity * speed_scale();
    velocity = std::clamp(velocity, -vmax, vmax);
    // How far it may go each way: inside the allowed box, and everywhere on the way
    // somewhere the lines can hold it.
    const Box box = allowed_box(), ws = workspace(cfg_);
    const double here = axes_[axis].planned();
    Vec3 p{axes_[0].planned(), axes_[1].planned(), axes_[2].planned()};
    auto reach = [&](double dir, double limit) {
        double last = here;
        const int steps = static_cast<int>(std::ceil(std::fabs(limit - here) / 10.0));
        for (int k = 1; k <= steps; ++k) {
            const double x = k == steps ? limit : here + dir * 10.0 * k;
            Vec3 q = p;
            (axis == 0 ? q.x : axis == 1 ? q.y : q.z) = x;
            if (check_point(cfg_, q, ws) != Spot::Ok) break;
            last = x;
        }
        return last;
    };
    const double lo = std::min(here, reach(-1.0, std::max(component(box.lo, axis), lim.min)));
    const double hi = std::max(here, reach(+1.0, std::min(component(box.hi, axis), lim.max)));
    axes_[axis].jog(velocity, std::max(now, now_) + cfg_.jog_hold, lim.max_accel, lo, hi);
    if (state_ != RigState::Arming && velocity != 0.0) state_ = RigState::Moving;
    return {};
}

Result Rig::stop(std::optional<double> decel, double) {
    if (!armed_) return {};
    if (state_ == RigState::Homing) {
        homed_ = false;
        message_ = "Homing stopped. Home again before flying.";
        if (home_ == Home::Reel) {
            state_ = RigState::Idle;
            return {};
        }
    }
    if (park_ != Park::None) message_ = "Parking stopped.";
    park_ = Park::None;
    for (int a = 0; a < kAxes; ++a) axes_[a].stop(decel ? *decel : cfg_.axes[a].max_accel);
    if (state_ != RigState::Arming) state_ = RigState::Stopping;
    return {};
}

Result Rig::park(double) {
    if (estop_) return {Refusal::EStop, "The E-stop chain is open."};
    if (state_ == RigState::Fault) return {Refusal::Fault, fault_list(faults_)};
    if (!armed_) return {Refusal::Disarmed, "Arm first."};
    if (!homed_) return {Refusal::NotHomed, "Home at the dock first."};
    if (state_ == RigState::Homing || state_ == RigState::Arming) return {Refusal::Busy, "Busy."};
    const Vec3 p = position();
    Vec3 to{cfg_.dock.x, cfg_.dock.y, std::min(std::max(p.z, cfg_.dock.z + 50.0), cfg_.axes[2].max)};
    move_together(to, cfg_.park_speed);
    park_ = Park::Approach;
    state_ = RigState::Moving;
    message_ = "Parking on the dock.";
    return {};
}

Result Rig::set_limits(int axis, const AxisLimits& l) {
    if (!axis_present(axis)) return {Refusal::NoAxis, "There's no such axis."};
    if (!(l.min < l.max) || l.max_velocity <= 0 || l.max_accel <= 0)
        return {Refusal::BadMessage, "Min must be below max, and speed and acceleration above 0."};
    if (armed_ && busy()) return {Refusal::Busy, "Not while moving."};
    cfg_.axes[axis] = l;
    return {};
}

void Rig::set_remote(bool on, double now) {
    if (remote_ == on) return;
    remote_ = on;
    if (on) {
        last_heard_ = std::max(now, now_);  // a fresh start for the heartbeat
        message_ = "Remote control is on.";
        return;
    }
    message_ = "Remote control is off. The controller's screen is in charge.";
    if (armed_ && park_ == Park::None && state_ == RigState::Moving) {
        for (int a = 0; a < kAxes; ++a) axes_[a].stop(cfg_.axes[a].max_accel);
        state_ = RigState::Stopping;
    }
}

void Rig::heard(double now) { last_heard_ = now; }

void Rig::wind_sample(double speed, double now) { wind_.sample(now, speed); }

void Rig::move_together(const Vec3& to, double speed) {
    double t = 0.0;
    double dist[kAxes];
    for (int a = 0; a < kAxes; ++a) {
        dist[a] = std::fabs(component(to, a) - axes_[a].planned());
        const double v = std::min(speed, cfg_.axes[a].max_velocity), acc = cfg_.axes[a].max_accel;
        const double ta = dist[a] >= v * v / acc ? dist[a] / v + v / acc : 2.0 * std::sqrt(dist[a] / acc);
        t = std::max(t, ta);
    }
    for (int a = 0; a < kAxes; ++a) {
        const double v = std::min(speed, cfg_.axes[a].max_velocity), acc = cfg_.axes[a].max_accel;
        axes_[a].move(component(to, a), dist[a] > 0 ? timed_speed(dist[a], t, v, acc, nullptr) : v, acc);
    }
}

// -- the control loop ------------------------------------------------------------------

void Rig::update(double now) {
    if (!started_) {
        started_ = true;
        now_ = now;
        return;
    }
    int n = 0;
    while (now_ + cfg_.control_period <= now + 1e-9) {
        now_ += cfg_.control_period;
        tick();
        if (++n >= 1000) {  // far behind: skip ahead rather than spiral
            now_ = now;
            break;
        }
    }
}

void Rig::tick() {
    const double dt = cfg_.control_period;
    fb_ = hw_.feedback();
    if (hw_.estop_open()) {
        estop_ = true;
        if (state_ != RigState::EStop) enter_estop();
        step_wind();
        return;
    }
    estop_ = false;
    if (state_ == RigState::EStop) {
        state_ = RigState::Fault;
        message_ = "The E-stop chain is closed again. Arm to carry on.";
    }
    if (drives_on_) {
        for (int i = 0; i < kLines; ++i)
            if (fb_[i].alarm) {
                hard_fault(faults::DriveAlarm,
                           std::string("Line ") + cfg_.lines[i].name + "'s drive reports a fault.");
                break;
            }
    }
    if (armed_ && remote_ && last_heard_ >= 0.0 && now_ - last_heard_ > cfg_.heartbeat_timeout)
        soft_fault(faults::HeartbeatLost, "Lost Show Controller's heartbeat. Stopped and disarmed.");
    step_wind();
    if (state_ == RigState::Arming) step_arming();
    if (state_ == RigState::Homing) step_homing(dt);
    step_motion(dt);
    check_lines(dt);
    step_parking();
    step_disarm();
    sample_swing();
    if (state_ == RigState::Idle || state_ == RigState::Moving)
        state_ = busy() ? RigState::Moving : RigState::Idle;
    else if (state_ == RigState::Stopping && disarm_ == Disarm::None && !busy())
        state_ = RigState::Idle;
}

void Rig::enter_estop() {
    if (state_ == RigState::Homing) homed_ = false;
    state_ = RigState::EStop;
    armed_ = false;
    faults_ |= faults::EStop;
    drives_on_ = brakes_released_ = false;
    hw_.set_brakes_released(false);
    hw_.set_enabled(false);
    for (int a = 0; a < kAxes; ++a) axes_[a].reset(axes_[a].output(), window_);
    park_ = Park::None;
    disarm_ = Disarm::None;
    message_ = "E-stop. Drive power is off and the brakes are holding.";
}

void Rig::hard_fault(std::uint32_t f, const std::string& why) {
    faults_ |= f;
    message_ = why;
    if (state_ == RigState::Homing) homed_ = false;
    armed_ = false;
    brakes_released_ = false;
    hw_.set_brakes_released(false);
    drives_on_ = false;
    hw_.set_enabled(false);
    for (int a = 0; a < kAxes; ++a) axes_[a].reset(axes_[a].output(), window_);
    park_ = Park::None;
    disarm_ = Disarm::None;
    state_ = RigState::Fault;
}

void Rig::soft_fault(std::uint32_t f, const std::string& why) {
    const bool first = !(faults_ & f);
    faults_ |= f;
    if (disarm_ != Disarm::None) {  // already stopping
        if (first) message_ = why;
        return;
    }
    message_ = why;
    begin_disarm();
}

void Rig::begin_disarm() {
    if (state_ == RigState::Homing) homed_ = false;
    park_ = Park::None;
    armed_ = false;
    if (!drives_on_ || !brakes_released_) {
        brakes_released_ = false;
        hw_.set_brakes_released(false);
        drives_on_ = false;
        hw_.set_enabled(false);
        disarm_ = Disarm::None;
        state_ = faults_ ? RigState::Fault : RigState::Disarmed;
        return;
    }
    for (int a = 0; a < kAxes; ++a) axes_[a].stop(cfg_.axes[a].max_accel);
    disarm_ = Disarm::Stopping;
    state_ = RigState::Stopping;
}

void Rig::step_disarm() {
    if (disarm_ == Disarm::Stopping && !busy()) {
        brakes_released_ = false;
        hw_.set_brakes_released(false);
        disarm_ = Disarm::Braking;
        phase_t_ = now_;
    } else if (disarm_ == Disarm::Braking && now_ - phase_t_ >= cfg_.brake_time) {
        drives_on_ = false;
        hw_.set_enabled(false);
        disarm_ = Disarm::None;
        state_ = faults_ ? RigState::Fault : RigState::Disarmed;
        if (!faults_ && message_ == "Disarming.") message_ = "Disarmed. The brakes are on.";
    }
}

void Rig::step_arming() {
    if (arm_ == Arm::Enable) {
        hw_.command(cmd_);
        bool ready = true;
        for (const LineFeedback& f : fb_) ready = ready && f.ready;
        if (ready) {
            brakes_released_ = true;
            hw_.set_brakes_released(true);
            arm_ = Arm::Release;
            phase_t_ = now_;
        } else if (now_ - phase_t_ > cfg_.arm_timeout) {
            hard_fault(faults::DriveAlarm, "The drives didn't come on.");
        }
    } else if (now_ - phase_t_ >= cfg_.brake_time) {
        state_ = busy() ? RigState::Moving : RigState::Idle;
        message_ = homed_ ? "Armed." : "Armed. Home at the dock before flying.";
    }
}

void Rig::step_homing(double dt) {
    if (!brakes_released_) return;
    if (home_ == Home::Reel) {
        bool all = true;
        for (int i = 0; i < kLines; ++i) {
            if (fb_[i].tension > cfg_.tension_max) {
                hard_fault(faults::Overload, std::string("Line ") + cfg_.lines[i].name +
                                                 " pulled too hard while homing.");
                return;
            }
            if (tight_[i]) continue;
            if (fb_[i].tension >= cfg_.pretension) {
                tight_[i] = true;
                continue;
            }
            all = false;
            cmd_[i] -= cfg_.homing_speed * dt;
            reeled_[i] += cfg_.homing_speed * dt;
            if (reeled_[i] > cfg_.homing_reel_max) {
                hard_fault(faults::HomingFailed, std::string("Line ") + cfg_.lines[i].name +
                                                     " never went tight. Is the ball on the dock?");
                return;
            }
        }
        hw_.command(cmd_);
        if (!all) return;
        // Every line is tight with the ball on the dock: now every length is known.
        LineArray t{};
        for (int i = 0; i < kLines; ++i) t[i] = fb_[i].tension;
        const LineArray now_out = unstretched_lengths(cfg_, cfg_.dock, t);
        const Tensions e = expected_tensions(cfg_, cfg_.dock, cfg_.plan_tension_min);
        const LineArray want = unstretched_lengths(cfg_, cfg_.dock, e.t);
        pull_ = 0.0;
        for (int i = 0; i < kLines; ++i) {
            offset_[i] = now_out[i] - fb_[i].position;
            trim_[i] = now_out[i] - want[i];  // fades away, taking up the ball's weight gently
        }
        for (int a = 0; a < kAxes; ++a) axes_[a].reset(component(cfg_.dock, a), window_);
        homed_ = true;
        home_ = Home::Settle;
        phase_t_ = now_;
    } else if (home_ == Home::Settle) {
        bool small = true;
        for (double tr : trim_) small = small && std::fabs(tr) < 0.2;
        if (small || now_ - phase_t_ > 5.0) {
            axes_[2].move(cfg_.dock.z + cfg_.homing_lift, 30.0, cfg_.axes[2].max_accel);
            home_ = Home::Lift;
        }
    } else if (home_ == Home::Lift) {
        if (!busy()) {
            home_ = Home::Check;
            phase_t_ = now_;
        }
    } else if (now_ - phase_t_ >= cfg_.homing_settle) {
        int worst = 0;
        double err = 0.0;
        for (int i = 0; i < kLines; ++i) {
            const double e = std::fabs(fb_[i].tension - expected_[i]);
            if (e > err) { err = e; worst = i; }
        }
        if (err > cfg_.mismatch_limit) {
            soft_fault(faults::TensionMismatch,
                       std::string("After homing, line ") + cfg_.lines[worst].name +
                           fmt(" pulls %.1f N, not %.1f N. Check the dock and pulley positions.",
                               fb_[worst].tension, expected_[worst]));
            homed_ = false;
        } else {
            state_ = RigState::Idle;
            message_ = "Homed at the dock.";
        }
    }
}

void Rig::step_motion(double dt) {
    if (!drives_on_ || !brakes_released_) return;
    if (state_ == RigState::Homing && home_ == Home::Reel) return;
    if (state_ != RigState::Arming)
        for (AxisMotion& a : axes_) a.step(dt, now_);
    if (!homed_) {
        hw_.command(cmd_);  // position unknown: hold the drives where they are
        return;
    }
    const Vec3 p = position();
    const Tensions e = expected_tensions(cfg_, p, cfg_.plan_tension_min);
    expected_ = e.t;
    free_ = e.free;
    const LineArray out = unstretched_lengths(cfg_, p, e.t);
    const bool near_dock = p.z < cfg_.dock.z + cfg_.dock_zone;
    if (near_dock) pull_ *= std::exp(-dt / kTrimDecay);
    for (int i = 0; i < kLines; ++i) {
        if (near_dock) trim_[i] *= std::exp(-dt / kTrimDecay);
        cmd_[i] = out[i] + trim_[i] + pull_ * free_[i] - offset_[i];
    }
    hw_.command(cmd_);
}

void Rig::check_lines(double dt) {
    if (!flying()) {
        slack_t_ = {};
        mismatch_t_ = {};
        return;
    }
    // The tension loop. Four lines hold three axes, so the lines can pull against each
    // other harder or softer than planned without the ball moving. Only that part of
    // the error is trimmed, along the free direction: the rest means the ball isn't
    // where the model thinks, which trimming would hide. That part is left to show up.
    double along = 0.0;
    for (int i = 0; i < kLines; ++i) along += (fb_[i].tension - expected_[i]) * free_[i];
    pull_ = std::clamp(pull_ + cfg_.trim_gain * along * dt, -cfg_.trim_limit, cfg_.trim_limit);
    for (int i = 0; i < kLines; ++i) {
        const double t = fb_[i].tension, err = t - expected_[i];
        const std::string line = std::string("Line ") + cfg_.lines[i].name;
        if (t > cfg_.tension_max) {
            soft_fault(faults::Overload, line + fmt(" pulled %.0f N, over the %.0f N limit.", t, cfg_.tension_max));
            return;
        }
        slack_t_[i] = t < cfg_.tension_min ? slack_t_[i] + dt : 0.0;
        if (slack_t_[i] >= cfg_.slack_time) {
            soft_fault(faults::SlackLine, line + " went slack.");
            return;
        }
        mismatch_t_[i] = std::fabs(err) > cfg_.mismatch_limit ? mismatch_t_[i] + dt : 0.0;
        if (mismatch_t_[i] >= cfg_.mismatch_time) {
            soft_fault(faults::TensionMismatch,
                       line + fmt(" pulls %.1f N but should pull %.1f N. Is something caught on it?", t,
                                  expected_[i]));
            return;
        }
    }
}

void Rig::step_parking() {
    if (park_ == Park::Approach && !busy()) {
        axes_[2].move(cfg_.dock.z - cfg_.park_sink, cfg_.park_speed * 0.3, cfg_.axes[2].max_accel);
        park_ = Park::Descend;
    } else if (park_ == Park::Descend && !busy()) {
        park_ = Park::None;
        message_ = faults_ & faults::Wind ? "Parked on the dock: the wind is Red." : "Parked on the dock.";
        begin_disarm();
    }
}

void Rig::step_wind() {
    if (!cfg_.wind.enabled) return;
    // The swing only says something about the wind once the ball has hung still for
    // the whole 2 s it covers: before that it still holds the move's own changes.
    double still = -1.0;
    if (state_ == RigState::Idle && !busy() && flying()) {
        if (still_since_ < 0.0) still_since_ = now_;
        if (now_ - still_since_ >= kSwingSamples * 0.02)
            still = *std::max_element(swing_.begin(), swing_.end());
    } else {
        still_since_ = -1.0;
    }
    wind_.update(now_, still);
    if (wind_.level() != WindLevel::Red || !wind_.ever_read() || (faults_ & faults::Wind)) return;
    faults_ |= faults::Wind;
    message_ = wind_.sensor_ok() ? "The wind is Red." : "The wind sensor isn't answering: treating it as Red.";
    if (armed_ && disarm_ == Disarm::None) {
        if (flying() && park_ == Park::None && state_ != RigState::Homing) {
            const Vec3 p = position();
            move_together({cfg_.dock.x, cfg_.dock.y,
                           std::min(std::max(p.z, cfg_.dock.z + 50.0), cfg_.axes[2].max)},
                          cfg_.park_speed);
            park_ = Park::Approach;
            state_ = RigState::Moving;
            message_ += " Parking on the dock.";
        } else if (park_ == Park::None) {
            begin_disarm();
        }
    } else if (state_ == RigState::Disarmed) {
        state_ = RigState::Fault;
    }
}

void Rig::sample_swing() {
    if (now_ < swing_next_) return;
    swing_next_ = now_ + 0.02;
    for (int i = 0; i < kLines; ++i) swing_buf_[i][swing_at_] = static_cast<float>(fb_[i].tension);
    swing_at_ = (swing_at_ + 1) % kSwingSamples;
    swing_count_ = std::min(swing_count_ + 1, kSwingSamples);
    for (int i = 0; i < kLines; ++i) {
        const auto b = swing_buf_[i].begin(), e = b + swing_count_;
        const auto mm = std::minmax_element(b, e);
        swing_[i] = (*mm.second - *mm.first) / 2.0;
    }
}

}  // namespace leota
