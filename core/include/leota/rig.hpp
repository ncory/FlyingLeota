// The motion controller: one ball on four lines, flown as three axes (x, y, z).
// Commands come from SCMP (scmp.hpp) or the touchscreen; both call the same methods.
// update() runs the control loop in fixed cycles of config().control_period.
//
// States and rules follow docs/plan.md, sections 5.3 to 6, and Show Controller's
// docs/scmp.md, including its Optional fields.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "leota/config.hpp"
#include "leota/geometry.hpp"
#include "leota/hardware.hpp"
#include "leota/profile.hpp"
#include "leota/wind.hpp"

namespace leota {

enum class RigState { Disarmed, Arming, Idle, Homing, Moving, Stopping, Fault, EStop };

namespace faults {
enum : std::uint32_t {
    EStop = 1u << 0,
    HeartbeatLost = 1u << 1,
    SlackLine = 1u << 2,
    Overload = 1u << 3,
    TensionMismatch = 1u << 4,
    DriveAlarm = 1u << 5,
    Wind = 1u << 6,
    HomingFailed = 1u << 7,
};
// SCMP names: "estop", "heartbeat_lost", "slack_line", ...
const char* name(std::uint32_t one_fault);
constexpr std::uint32_t kAll = (1u << 8) - 1;
}  // namespace faults

// Why a command was refused. The SCMP names are in scmp.cpp.
enum class Refusal { None, BadMessage, NoAxis, Disarmed, EStop, NotHomed, OutOfRange, Busy, Fault, Weather };

struct Result {
    Refusal error = Refusal::None;
    std::string detail;
    double late = 0.0;  // s a timed move expects to finish late
    explicit operator bool() const { return error == Refusal::None; }
};

struct LineStatus {
    char name = 'A';
    double tension = 0.0;    // N, measured
    double expected = 0.0;   // N, from the model
    double length = 0.0;     // mm, pulley to ball, measured (0 until homed)
    double out = 0.0;        // mm of unstretched line the drive has paid out (0 until homed)
    double swing = 0.0;      // N: half the spread of tension over the last 2 s
    double trim = 0.0;       // mm the tension loop has added
    bool ready = false, alarm = false;
};

class Rig {
public:
    Rig(const RigConfig& cfg, Hardware& hw);

    // Commands. `now` is the controller's clock, in seconds.
    Result arm(double now);
    Result disarm(double now);
    Result home(double now);
    Result move(int axis, double target, std::optional<double> velocity,
                std::optional<double> duration, std::optional<double> accel, double now);
    Result jog(int axis, double velocity, double now);
    Result stop(std::optional<double> decel, double now);
    Result park(double now);  // fly back over the dock and set the ball down
    Result clear_faults(double now);  // once their causes are gone; stays disarmed
    Result set_limits(int axis, const AxisLimits& limits);
    // New settings, from Setup. Only while disarmed. A change to where the pulleys or the
    // dock are, or to two-axis mode, needs the ball on the dock and homing again.
    Result reconfigure(const RigConfig& cfg);
    static bool needs_homing(const RigConfig& a, const RigConfig& b);  // after a change from a to b
    void set_heartbeat_timeout(double s) { cfg_.heartbeat_timeout = s; }

    void set_remote(bool on, double now);  // the touchscreen toggle or key switch
    void heard(double now);                // any message from Show Controller
    void wind_sample(double speed, double now);

    void update(double now);  // runs every control cycle due up to `now`

    // State
    RigState state() const { return state_; }
    bool armed() const { return armed_; }
    bool homed() const { return homed_; }
    bool remote() const { return remote_; }
    bool estop() const { return estop_; }
    bool parking() const { return park_ != Park::None; }
    bool drives_on() const { return drives_on_; }
    bool brakes_released() const { return brakes_released_; }
    bool busy() const;  // anything moving, or settling
    bool on_dock() const;  // the commanded ball is resting on the dock
    // Each line's tension when homing found it tight (for calibration).
    const std::array<double, kLines>& home_tensions() const { return home_t_; }
    std::uint32_t faults() const { return faults_; }
    const std::string& message() const { return message_; }
    Vec3 position() const;  // the ball's commanded centre, mm
    Vec3 lean() const { return lean_; }  // how the model says the ball hangs there
    Vec3 velocity() const;
    bool has_target(int axis) const { return axes_[axis].has_target(); }
    double target(int axis) const { return axes_[axis].target(); }
    const AxisLimits& limits(int axis) const { return cfg_.axes[axis]; }
    bool axis_present(int axis) const { return axis >= 0 && axis < kAxes && !(cfg_.two_axis && axis == 1); }
    std::array<LineStatus, kLines> lines() const;
    const WindMonitor& wind() const { return wind_; }
    const RigConfig& config() const { return cfg_; }
    double now() const { return now_; }

private:
    enum class Arm { Enable, Release };
    enum class Disarm { None, Stopping, Braking };
    enum class Home { Reel, Settle, Lift, Check };
    enum class Park { None, Approach, Descend };

    void tick();
    void enter_estop();
    void hard_fault(std::uint32_t f, const std::string& why);
    void soft_fault(std::uint32_t f, const std::string& why);
    void begin_disarm();
    void step_arming();
    void step_homing(double dt);
    void step_motion(double dt);
    void check_lines(double dt);
    void step_parking();
    void step_disarm();
    void step_wind();
    void sample_swing();
    void move_together(const Vec3& to, double speed);
    Box allowed_box() const;
    double speed_scale() const;
    bool flying() const;
    void hold_drives();  // command where the drives are now

    RigConfig cfg_;
    Hardware& hw_;
    WindMonitor wind_;
    std::array<AxisMotion, kAxes> axes_;
    int window_ = 1;

    RigState state_ = RigState::Disarmed;
    bool armed_ = false, homed_ = false, remote_ = true, estop_ = false;
    bool drives_on_ = false, brakes_released_ = false;
    std::uint32_t faults_ = 0;
    std::string message_;

    std::array<LineFeedback, kLines> fb_{};
    LineArray offset_{}, trim_{}, cmd_{}, expected_{}, free_{};
    double pull_ = 0.0;  // mm along the free direction: the tension loop's trim
    Vec3 lean_;          // the ball's modelled lean (rotation vector)
    int lean_count_ = 0;
    LineArray slack_t_{}, mismatch_t_{};
    LineArray home_t_{};

    bool started_ = false;
    double now_ = 0.0, last_heard_ = -1.0, phase_t_ = 0.0;
    Arm arm_ = Arm::Enable;
    Disarm disarm_ = Disarm::None;
    Home home_ = Home::Reel;
    Park park_ = Park::None;
    std::array<bool, kLines> tight_{};
    LineArray reeled_{};

    static constexpr int kSwingSamples = 100;  // 2 s at 50 Hz
    std::array<std::array<float, kSwingSamples>, kLines> swing_buf_{};
    int swing_at_ = 0, swing_count_ = 0;
    double swing_next_ = 0.0, still_since_ = -1.0;  // the swing counts once still for 2 s
    LineArray swing_{};
};

}  // namespace leota
