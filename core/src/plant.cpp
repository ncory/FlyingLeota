#include "leota/plant.hpp"

#include <algorithm>
#include <cmath>

#include "leota/geometry.hpp"

namespace leota {

Plant::Plant(const RigConfig& truth, double slack) : cfg_(truth), ball_(truth.dock) {
    LineArray g = geometric_lengths(cfg_, cfg_.dock);
    for (int i = 0; i < kLines; ++i) {
        pos_[i] = 0.0;
        // Each drive's count starts somewhere arbitrary: give each line its own offset.
        zero_[i] = g[i] + slack + 7.0 * i;
    }
}

void Plant::set_enabled(bool on) { enabled_ = on && !estop_; }
void Plant::set_brakes_released(bool on) { released_ = on && !estop_; }

void Plant::command(const std::array<double, kLines>& positions) {
    if (!enabled_ || !released_ || estop_) return;  // brakes or no power: nothing moves
    for (int i = 0; i < kLines; ++i) {
        if (alarm_[i]) continue;
        if (pos_[i] != positions[i]) dirty_ = true;
        pos_[i] = positions[i];
    }
}

std::array<double, kLines> Plant::line_out() const {
    std::array<double, kLines> out{};
    for (int i = 0; i < kLines; ++i) out[i] = zero_[i] + pos_[i];
    return out;
}

std::array<LineFeedback, kLines> Plant::feedback() {
    solve();
    std::array<LineFeedback, kLines> fb{};
    for (int i = 0; i < kLines; ++i) {
        double n = 0.0;
        if (noise_ > 0) {
            noise_state_ = noise_state_ * 1103515245u + 12345u;
            n = noise_ * ((noise_state_ >> 8 & 0xffff) / 32767.5 - 1.0);
        }
        fb[i] = {pos_[i], std::max(0.0, tension_[i] + n), enabled_ && !alarm_[i], alarm_[i]};
    }
    return fb;
}

void Plant::forces(const Vec3& c, std::array<double, kLines>& t) const {
    for (int i = 0; i < kLines; ++i) {
        const double out = zero_[i] + pos_[i];
        const double d = (cfg_.lines[i].anchor - (c + cfg_.lines[i].attach)).norm();
        t[i] = cut_[i] || d <= out ? 0.0 : cfg_.line_ea * (d - out) / out;
    }
}

// Minimises the energy of the lines plus gravity by Newton's method, from ball_.
bool Plant::equilibrium(Vec3& c) const {
    const double w = cfg_.mass * kGravity;
    for (int iter = 0; iter < 60; ++iter) {
        Vec3 grad{-push_.x, -push_.y, w - push_.z};
        double h[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        int taut = 0;
        for (int i = 0; i < kLines; ++i) {
            if (cut_[i]) continue;
            const double out = zero_[i] + pos_[i];
            const Vec3 dv = cfg_.lines[i].anchor - (c + cfg_.lines[i].attach);
            const double d = dv.norm();
            if (d <= out) continue;
            ++taut;
            const Vec3 u = dv * (1.0 / d);
            const double k = cfg_.line_ea / out, f = k * (d - out);
            grad -= u * f;  // the line pulls the ball toward its pulley
            const double uu[3] = {u.x, u.y, u.z};
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                    h[a][b] += k * uu[a] * uu[b] + (f / d) * ((a == b ? 1.0 : 0.0) - uu[a] * uu[b]);
        }
        if (taut < 3) return false;
        // Solve h · step = -grad.
        const Vec3 r0{h[0][0], h[0][1], h[0][2]}, r1{h[1][0], h[1][1], h[1][2]},
            r2{h[2][0], h[2][1], h[2][2]};
        const double det = r0.dot(r1.cross(r2));
        if (std::fabs(det) < 1e-12) return false;
        const Vec3 b = -grad;
        Vec3 step{b.dot(r1.cross(r2)) / det, r0.dot(b.cross(r2)) / det, r0.dot(r1.cross(b)) / det};
        const double len = step.norm();
        if (len > 50.0) step = step * (50.0 / len);  // don't leap
        c += step;
        if (len < 1e-7) return true;
    }
    return true;
}

void Plant::solve() {
    if (!dirty_) return;
    dirty_ = false;
    const double w = cfg_.mass * kGravity;
    const Vec3 dock = cfg_.dock;
    auto upward_at_dock = [&]() {
        std::array<double, kLines> t;
        forces(dock, t);
        double up = push_.z;
        for (int i = 0; i < kLines; ++i) {
            const Vec3 dv = cfg_.lines[i].anchor - (dock + cfg_.lines[i].attach);
            up += t[i] * dv.z / dv.norm();
        }
        return up;
    };
    if (resting_ && upward_at_dock() <= w) {
        ball_ = dock;  // still sitting in the cup
    } else {
        Vec3 c = resting_ ? dock + Vec3{0, 0, 0.1} : ball_;
        if (!equilibrium(c)) {
            fell_ = true;
            c = {ball_.x, ball_.y, cfg_.ball_radius};  // on the deck
        } else {
            fell_ = false;
        }
        const double dx = c.x - dock.x, dy = c.y - dock.y;
        if (c.z <= dock.z && std::sqrt(dx * dx + dy * dy) <= cfg_.dock_radius) {
            resting_ = true;
            ball_ = dock;
        } else {
            resting_ = false;
            ball_ = c;
        }
    }
    forces(ball_, tension_);
}

}  // namespace leota
