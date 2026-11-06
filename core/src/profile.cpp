#include "leota/profile.hpp"

#include <algorithm>
#include <cmath>

namespace leota {

namespace {
double sign(double v) { return v < 0 ? -1.0 : 1.0; }
}

void Trajectory::add(double dt, double a) {
    if (dt <= 0.0 || count_ >= static_cast<int>(pieces_.size())) return;
    pieces_[count_++] = {dt, a};
    total_ += dt;
}

Trajectory Trajectory::stop(double p0, double v0, double decel) {
    Trajectory tr;
    tr.p0_ = p0;
    tr.v0_ = v0;
    const double t = std::fabs(v0) / decel;
    tr.add(t, -sign(v0) * decel);
    tr.p1_ = p0 + v0 * t / 2.0;
    return tr;
}

Trajectory Trajectory::to_target(double p0, double v0, double p1, double vmax, double a) {
    Trajectory tr;
    tr.p0_ = p0;
    tr.v0_ = v0;
    tr.p1_ = p1;
    double p = p0, v = v0;
    double d = p1 - p;
    double s = std::fabs(d) > 1e-12 ? sign(d) : -sign(v);
    double vt = v * s;  // speed toward the target; negative is away
    if (vt < 0.0 || vt * vt / (2.0 * a) > std::fabs(d) + 1e-9) {
        // Moving away, or too fast to stop in time: stop first, then come back.
        const double t = std::fabs(v) / a;
        tr.add(t, -sign(v) * a);
        p += v * t / 2.0;
        v = 0.0;
        d = p1 - p;
        s = sign(d);
        vt = 0.0;
    }
    const double dist = std::fabs(d);
    if (dist < 1e-12) return tr;
    double vp = std::sqrt((2.0 * a * dist + vt * vt) / 2.0);  // peak of a triangle
    vp = std::min(vp, vmax);
    tr.add(std::fabs(vp - vt) / a, vp >= vt ? s * a : -s * a);
    const double d1 = std::fabs(vp * vp - vt * vt) / (2.0 * a);
    const double d3 = vp * vp / (2.0 * a);
    const double d2 = std::max(0.0, dist - d1 - d3);
    if (vp > 0.0) tr.add(d2 / vp, 0.0);
    tr.add(vp / a, -s * a);
    return tr;
}

void Trajectory::sample(double t, double& p, double& v) const {
    if (t >= total_) {
        p = p1_;
        v = 0.0;
        return;
    }
    p = p0_;
    v = v0_;
    for (int i = 0; i < count_; ++i) {
        const Piece& pc = pieces_[i];
        const double dt = std::min(t, pc.dt);
        p += v * dt + 0.5 * pc.a * dt * dt;
        v += pc.a * dt;
        t -= dt;
        if (t <= 0.0) return;
    }
}

double timed_speed(double distance, double duration, double vmax, double accel, double* late) {
    if (late) *late = 0.0;
    if (distance <= 0.0) return vmax;
    const double disc = (accel * duration) * (accel * duration) - 4.0 * accel * distance;
    if (disc >= 0.0) {
        const double v = (accel * duration - std::sqrt(disc)) / 2.0;
        if (v <= vmax) return v;
    }
    // As fast as allowed: how long does that take?
    double v = vmax, fastest;
    if (distance >= v * v / accel) {
        fastest = distance / v + v / accel;
    } else {
        fastest = 2.0 * std::sqrt(distance / accel);
        v = std::sqrt(distance * accel);
    }
    if (late) *late = std::max(0.0, fastest - duration);
    return v;
}

void Smoother::reset(double value, int window) {
    buf_.assign(static_cast<size_t>(std::max(1, window)), value);
    sum_ = value * static_cast<double>(buf_.size());
    last_ = value;
    at_ = 0;
    same_ = static_cast<int>(buf_.size());
    since_resum_ = 0;
}

double Smoother::push(double x) {
    same_ = (x == last_) ? same_ + 1 : 0;
    last_ = x;
    sum_ += x - buf_[at_];
    buf_[at_] = x;
    at_ = (at_ + 1) % static_cast<int>(buf_.size());
    if (++since_resum_ >= static_cast<int>(buf_.size())) {  // keep rounding from creeping in
        since_resum_ = 0;
        sum_ = 0.0;
        for (double b : buf_) sum_ += b;
    }
    return value();
}

void AxisMotion::reset(double position, int window) {
    mode_ = Mode::Idle;
    pos_ = position;
    vel_ = out_vel_ = 0.0;
    traj_ = Trajectory::to_target(position, 0.0, position, 1.0, 1.0);
    smooth_.reset(position, window);
}

void AxisMotion::move(double target, double vmax, double accel) {
    traj_ = Trajectory::to_target(pos_, vel_, target, vmax, accel);
    t_ = 0.0;
    mode_ = Mode::Move;
}

void AxisMotion::jog(double velocity, double until, double accel, double lo, double hi) {
    jog_v_ = velocity;
    jog_until_ = until;
    jog_accel_ = accel;
    jog_lo_ = lo;
    jog_hi_ = hi;
    mode_ = Mode::Jog;
}

void AxisMotion::stop(double decel) {
    if (mode_ == Mode::Idle) return;
    traj_ = Trajectory::stop(pos_, vel_, decel);
    t_ = 0.0;
    mode_ = Mode::Stop;
}

double AxisMotion::end() const {
    if (mode_ == Mode::Move || mode_ == Mode::Stop) return traj_.end();
    return pos_;
}

void AxisMotion::step(double dt, double now) {
    if (mode_ == Mode::Move || mode_ == Mode::Stop) {
        t_ += dt;
        traj_.sample(t_, pos_, vel_);
        if (t_ >= traj_.duration()) mode_ = Mode::Idle;
    } else if (mode_ == Mode::Jog) {
        double want = now <= jog_until_ ? jog_v_ : 0.0;
        // Never faster than can stop before a soft limit.
        if (want > 0) want = std::min(want, std::sqrt(2.0 * jog_accel_ * std::max(0.0, jog_hi_ - pos_)));
        if (want < 0) want = std::max(want, -std::sqrt(2.0 * jog_accel_ * std::max(0.0, pos_ - jog_lo_)));
        const double dv = jog_accel_ * dt;
        vel_ = want > vel_ ? std::min(want, vel_ + dv) : std::max(want, vel_ - dv);
        pos_ = std::clamp(pos_ + vel_ * dt, jog_lo_, jog_hi_);
        if ((pos_ <= jog_lo_ && vel_ < 0) || (pos_ >= jog_hi_ && vel_ > 0)) vel_ = 0.0;
        if (vel_ == 0.0 && want == 0.0 && now > jog_until_) mode_ = Mode::Idle;
    }
    const double before = smooth_.value();
    smooth_.push(pos_);
    out_vel_ = dt > 0 ? (smooth_.value() - before) / dt : 0.0;
}

}  // namespace leota
