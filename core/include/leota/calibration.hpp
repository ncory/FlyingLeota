// Checking the rig's measurements by flying. The ball visits a few points around the
// middle of the gazebo; at each it hangs still while the controller records how much
// line each winch has paid out and how hard each line pulls. Those have to agree with
// where the pulleys are and what the head weighs. A least-squares fit finds the pulley
// positions and the weight that agree best, starting from the measured ones.
//
// What it can't find: turning every pulley together about the dock, which changes
// nothing it can measure. That comes from the tape measurements, so measure the pulleys
// as well as you can; the fit spreads one pulley's error a little across all four.
#pragma once

#include <array>
#include <string>
#include <vector>

#include "leota/rig.hpp"

namespace leota {

// One point: where the ball was told to be, and the averages while it hung there.
struct CalSample {
    Vec3 target;
    LineArray out{};      // mm of unstretched line paid out, as the controller counts it
    LineArray tension{};  // N
};

struct CalFit {
    bool ok = false;                   // the fit worked and agrees with what was measured
    std::array<Vec3, kLines> anchors{};
    double mass = 0.0;                 // kg
    std::array<double, kLines> moved{};  // mm each pulley is from its measured position
    double length_rms = 0.0;           // mm: how far the line lengths still disagree
    double force_rms = 0.0;            // N: how far the forces still disagree
    std::string summary;               // what it found, in sentences
};

// The points to fly to: a cross and corners around the middle, at three heights, all
// well inside the workspace.
std::vector<Vec3> calibration_points(const RigConfig& cfg);

// `home_tension`: each line's tension when homing found it tight (Rig::home_tensions()).
CalFit fit_calibration(const RigConfig& cfg, const LineArray& home_tension,
                       const std::vector<CalSample>& samples);

// Flies the points, measures, fits, then parks the ball on the dock (which disarms).
// The touchscreen runs it; update() needs calling every 50 ms or so.
class CalibrationRun {
public:
    enum class Phase { Off, Flying, Settling, Measuring, Parking, Done, Failed };

    // Why it can't start now ("" when it can): it needs the rig armed, homed, still,
    // with remote control off and the wind Green.
    static std::string not_ready(const Rig& rig);

    Result start(Rig& rig, double now);
    void update(Rig& rig, double now);
    void cancel(Rig& rig, double now);  // stops the ball where it is

    Phase phase() const { return phase_; }
    bool running() const;
    int measured() const { return static_cast<int>(samples_.size()); }
    int points() const { return static_cast<int>(points_.size()); }
    const std::string& message() const { return message_; }
    const CalFit& fit() const { return fit_; }
    const std::vector<CalSample>& samples() const { return samples_; }

    double settle_time = 2.0;   // s to hang still before measuring
    double measure_time = 1.0;  // s of measuring at each point
    double speed = 150.0;       // mm/s between points

private:
    void fly(Rig& rig, double now);
    void fail(const std::string& why);

    Phase phase_ = Phase::Off;
    std::vector<Vec3> points_;
    std::vector<CalSample> samples_;
    size_t index_ = 0;
    double phase_t_ = 0.0;
    CalSample sum_;
    int count_ = 0;
    LineArray home_t_{};
    RigConfig cfg_;
    std::string message_;
    CalFit fit_;
};

}  // namespace leota
