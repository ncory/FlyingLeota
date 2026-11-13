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
        const double real = positions[i] * sensors_[i].drum;
        if (pos_[i] != real) dirty_ = true;
        pos_[i] = real;
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
        const LineSensors& s = sensors_[i];
        fb[i] = {pos_[i] / s.drum, s.load_zero + std::max(0.0, tension_[i] + n) / s.load_scale, enabled_ && !alarm_[i],
                 alarm_[i]};
    }
    return fb;
}

void Plant::forces(const Vec3& c, const Vec3& rot, std::array<double, kLines>& t) const {
    for (int i = 0; i < kLines; ++i) {
        const double out = zero_[i] + pos_[i];
        const double d = (cfg_.lines[i].anchor - (c + rotate(rot, cfg_.lines[i].attach))).norm();
        t[i] = cut_[i] || hung_[i] || d <= out ? 0.0 : cfg_.line_ea * (d - out) / out;
    }
}

void Plant::residual(const Vec3& c, const Vec3& rot, double out[6]) const {
    const double w = cfg_.mass * kGravity;
    Vec3 f = push_ + Vec3{0.0, 0.0, -w};
    const Vec3 cg = rotate(rot, cfg_.centre_of_weight);
    Vec3 tau = cg.cross({0.0, 0.0, -w});
    std::array<double, kLines> t;
    forces(c, rot, t);
    for (int i = 0; i < kLines; ++i) {
        if (t[i] <= 0.0) continue;
        const Vec3 r = rotate(rot, cfg_.lines[i].attach);
        const Vec3 d = cfg_.lines[i].anchor - (c + r);
        const Vec3 pull = d * (t[i] / d.norm());
        f += pull;
        tau += r.cross(pull);
    }
    // Twist is scaled to about the same size as push: a lever of 100 mm.
    out[0] = f.x; out[1] = f.y; out[2] = f.z;
    out[3] = tau.x / 100.0; out[4] = tau.y / 100.0; out[5] = tau.z / 100.0;
}

namespace {

// Solves the 6×6 system a·x = b in place by Gaussian elimination. False if singular.
bool solve6(double a[6][6], double b[6], double x[6]) {
    for (int col = 0; col < 6; ++col) {
        int piv = col;
        for (int r = col + 1; r < 6; ++r)
            if (std::fabs(a[r][col]) > std::fabs(a[piv][col])) piv = r;
        if (std::fabs(a[piv][col]) < 1e-12) return false;
        for (int k = 0; k < 6; ++k) std::swap(a[col][k], a[piv][k]);
        std::swap(b[col], b[piv]);
        for (int r = col + 1; r < 6; ++r) {
            const double m = a[r][col] / a[col][col];
            for (int k = col; k < 6; ++k) a[r][k] -= m * a[col][k];
            b[r] -= m * b[col];
        }
    }
    for (int r = 5; r >= 0; --r) {
        double v = b[r];
        for (int k = r + 1; k < 6; ++k) v -= a[r][k] * x[k];
        x[r] = v / a[r][r];
    }
    return true;
}

double size6(const double v[6]) {
    double s = 0.0;
    for (int k = 0; k < 6; ++k) s += v[k] * v[k];
    return std::sqrt(s);
}

}  // namespace

// Newton's method on position and lean together, with a line search so slack lines
// switching in and out can't throw it.
bool Plant::equilibrium(Vec3& c, Vec3& rot) const {
    int taut = 0;
    for (int i = 0; i < kLines; ++i) taut += cut_[i] || hung_[i] ? 0 : 1;
    if (taut < 3) return false;
    double r[6];
    residual(c, rot, r);
    for (int iter = 0; iter < 80; ++iter) {
        const double now = size6(r);
        if (now < 1e-7) return true;
        double jac[6][6];
        const double h[6] = {1e-4, 1e-4, 1e-4, 1e-7, 1e-7, 1e-7};
        for (int k = 0; k < 6; ++k) {
            Vec3 c2 = c, r2 = rot;
            if (k < 3) (k == 0 ? c2.x : k == 1 ? c2.y : c2.z) += h[k];
            else (k == 3 ? r2.x : k == 4 ? r2.y : r2.z) += h[k];
            double rk[6];
            residual(c2, r2, rk);
            for (int m = 0; m < 6; ++m) jac[m][k] = (rk[m] - r[m]) / h[k];
        }
        double b[6], step[6];
        for (int m = 0; m < 6; ++m) b[m] = -r[m];
        if (!solve6(jac, b, step)) return false;
        // Keep each step modest: 50 mm, or about 6°.
        double scale = 1.0;
        const double move = std::sqrt(step[0] * step[0] + step[1] * step[1] + step[2] * step[2]);
        const double turn = std::sqrt(step[3] * step[3] + step[4] * step[4] + step[5] * step[5]);
        if (move > 50.0) scale = std::min(scale, 50.0 / move);
        if (turn > 0.1) scale = std::min(scale, 0.1 / turn);
        for (int tries = 0; tries < 20; ++tries, scale *= 0.5) {
            const Vec3 c2 = c + Vec3{step[0], step[1], step[2]} * scale;
            const Vec3 r2 = rot + Vec3{step[3], step[4], step[5]} * scale;
            double rn[6];
            residual(c2, r2, rn);
            if (size6(rn) < now || tries == 19) {
                c = c2;
                rot = r2;
                for (int m = 0; m < 6; ++m) r[m] = rn[m];
                break;
            }
        }
        if (rot.norm() > 1.0) return false;  // tipped right over: it can't hang like this
    }
    return size6(r) < 1e-3;
}

void Plant::solve() {
    if (!dirty_) return;
    dirty_ = false;
    const double w = cfg_.mass * kGravity;
    const Vec3 dock = cfg_.dock;
    auto upward_at_dock = [&]() {
        std::array<double, kLines> t;
        forces(dock, {}, t);
        double up = push_.z;
        for (int i = 0; i < kLines; ++i) {
            const Vec3 dv = cfg_.lines[i].anchor - (dock + cfg_.lines[i].attach);
            up += t[i] * dv.z / dv.norm();
        }
        return up;
    };
    if (resting_ && upward_at_dock() <= w) {
        ball_ = dock;  // still sitting upright in the cup
        rot_ = {};
    } else {
        Vec3 c = resting_ ? dock + Vec3{0, 0, 0.1} : ball_;
        Vec3 r = resting_ ? Vec3{} : rot_;
        if (!equilibrium(c, r)) {
            fell_ = true;
            c = {ball_.x, ball_.y, cfg_.ball_radius};  // on the deck
            r = {};
        } else {
            fell_ = false;
        }
        const double dx = c.x - dock.x, dy = c.y - dock.y;
        if (c.z <= dock.z && std::sqrt(dx * dx + dy * dy) <= cfg_.dock_radius) {
            resting_ = true;
            ball_ = dock;
            rot_ = {};
        } else {
            resting_ = false;
            ball_ = c;
            rot_ = r;
        }
    }
    forces(ball_, rot_, tension_);
    for (int i = 0; i < kLines; ++i) {
        if (!hung_[i] || cut_[i]) continue;
        tension_[i] = hang_height(i) > 0.0 ? hang_kg_[i] * kGravity : 0.0;  // on the floor: slack
    }
}

double Plant::dock_watts() {
    if (!dock_fitted_ || !dock_on_) return 0.0;
    solve();
    double w = resting_ ? (ball_full_ ? 1.0 : 20.0) : 0.8;
    if (metal_) w += 30.0;
    return w;
}

double Plant::hang_height(int i) {
    const double out = zero_[i] + pos_[i];
    const double t = hang_kg_[i] * kGravity;
    return std::max(0.0, cfg_.lines[i].anchor.z - out * (1.0 + t / cfg_.line_ea));  // the floor stops it
}

}  // namespace leota
