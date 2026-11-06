// Motion along one axis: trapezoidal plans from any starting speed, timed moves, jog,
// stops, and a moving-average filter that turns the trapezoids into jerk-limited S-curves.
#pragma once

#include <array>
#include <vector>

#include "leota/config.hpp"

namespace leota {

// A plan made of up to six stretches of constant acceleration, ending at rest.
class Trajectory {
public:
    Trajectory() = default;
    // From position p0 at speed v0 (signed) to rest at p1, no faster than vmax and with
    // acceleration `accel`. If v0 points away, or is too fast to stop in time, it stops
    // first and comes back.
    static Trajectory to_target(double p0, double v0, double p1, double vmax, double accel);
    // From p0 at v0 to rest, braking at `decel`.
    static Trajectory stop(double p0, double v0, double decel);

    double duration() const { return total_; }
    double end() const { return p1_; }
    // Position and speed `t` seconds after the start (at the end: p1, 0).
    void sample(double t, double& p, double& v) const;

private:
    struct Piece { double dt, a; };
    void add(double dt, double a);
    double p0_ = 0.0, v0_ = 0.0, p1_ = 0.0, total_ = 0.0;
    std::array<Piece, 6> pieces_{};
    int count_ = 0;
};

// The cruising speed for a move from rest to rest that covers `distance` in `duration`,
// and how many seconds late it will be (0 if on time). Matches plan_speed() in Show
// Controller's showcontroller/scmp.py.
double timed_speed(double distance, double duration, double vmax, double accel, double* late);

// A moving average over a fixed number of control cycles. Fed a trapezoidal path, its
// output is an S-curve: the same top speed and acceleration, with the jerk limited to
// accel / window. It lags by half the window and moves on for one window after the
// input stops.
class Smoother {
public:
    void reset(double value, int window);
    double push(double x);  // one control cycle; returns the new output
    double value() const { return sum_ / static_cast<double>(buf_.size()); }
    bool settled() const { return same_ >= static_cast<int>(buf_.size()); }

private:
    std::vector<double> buf_{0.0};
    double sum_ = 0.0, last_ = 0.0;
    int at_ = 0, same_ = 0, since_resum_ = 0;
};

// One axis of the ball's motion, before the rig turns it into line lengths.
class AxisMotion {
public:
    enum class Mode { Idle, Move, Jog, Stop };

    void reset(double position, int window);  // stop at once, here
    void move(double target, double vmax, double accel);
    void jog(double velocity, double until, double accel, double lo, double hi);
    void stop(double decel);
    void step(double dt, double now);  // one control cycle

    Mode mode() const { return mode_; }
    bool busy() const { return mode_ != Mode::Idle; }
    bool settled() const { return !busy() && smooth_.settled(); }
    double planned() const { return pos_; }        // before smoothing
    double planned_velocity() const { return vel_; }
    double output() const { return smooth_.value(); }
    double output_velocity() const { return out_vel_; }
    double end() const;                             // where it will come to rest
    bool has_target() const { return mode_ == Mode::Move; }
    double target() const { return traj_.end(); }
    double plan_duration() const { return traj_.duration(); }  // of the current move

private:
    Mode mode_ = Mode::Idle;
    Trajectory traj_;
    double t_ = 0.0, pos_ = 0.0, vel_ = 0.0, out_vel_ = 0.0;
    double jog_v_ = 0.0, jog_until_ = 0.0, jog_accel_ = 1.0, jog_lo_ = 0.0, jog_hi_ = 0.0;
    Smoother smooth_;
};

}  // namespace leota
