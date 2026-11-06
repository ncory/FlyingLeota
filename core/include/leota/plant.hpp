// A simulated rig: four ideal drives, fail-safe brakes, an E-stop chain, stretchy lines
// and a ball that settles where the lines and gravity balance it, position and lean
// both (quasi-static: no swing). The ball rests upright in the dock's cup when the lines
// don't hold its weight. For tests and the PC simulator; the firmware doesn't use it.
#pragma once

#include "leota/hardware.hpp"

namespace leota {

class Plant : public Hardware {
public:
    // `truth` is the real rig; give it a slightly different geometry from the
    // controller's config to try calibration errors. The ball starts on the dock with
    // every line `slack` mm longer than it needs to be.
    explicit Plant(const RigConfig& truth, double slack = 40.0);

    // Hardware
    void set_enabled(bool on) override;
    void set_brakes_released(bool on) override;
    void command(const std::array<double, kLines>& positions) override;
    std::array<LineFeedback, kLines> feedback() override;
    bool estop_open() override { return estop_; }

    // Things a test can do to it
    void set_estop(bool open) { estop_ = open; if (open) enabled_ = released_ = false; }
    void set_alarm(int line, bool on) { alarm_[line] = on; }
    void cut_line(int line) { cut_[line] = true; dirty_ = true; }
    void mend_line(int line) { cut_[line] = false; dirty_ = true; }
    bool line_cut(int line) const { return cut_[line]; }
    bool alarm(int line) const { return alarm_[line]; }
    void push(const Vec3& force_n) { push_ = force_n; dirty_ = true; }  // e.g. wind
    void tension_noise(double amplitude_n) { noise_ = amplitude_n; }

    Vec3 ball() { solve(); return ball_; }
    Vec3 rotation() { solve(); return rot_; }  // the ball's lean and turn (rotation vector)
    bool resting() { solve(); return resting_; }
    bool fell() { solve(); return fell_; }
    bool enabled() const { return enabled_; }
    bool brakes_released() const { return released_; }
    std::array<double, kLines> tensions() { solve(); return tension_; }
    std::array<double, kLines> line_out() const;  // unstretched mm of line, per line

private:
    void solve();
    bool equilibrium(Vec3& c, Vec3& rot) const;  // free hanging; false if it can't hang
    void forces(const Vec3& c, const Vec3& rot, std::array<double, kLines>& t) const;
    // The net push (N) and twist (N·mm, about the centre) on the ball at a pose.
    void residual(const Vec3& c, const Vec3& rot, double out[6]) const;

    RigConfig cfg_;
    std::array<double, kLines> pos_{}, zero_{};  // drive counts; line out = zero + pos
    std::array<bool, kLines> alarm_{}, cut_{};
    std::array<double, kLines> tension_{};
    Vec3 ball_, rot_, push_;
    bool enabled_ = false, released_ = false, estop_ = false;
    bool resting_ = true, fell_ = false, dirty_ = true;
    double noise_ = 0.0;
    unsigned noise_state_ = 12345u;
};

}  // namespace leota
