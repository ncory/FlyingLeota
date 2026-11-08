#include "leota/calibration.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace leota {

namespace {

// How much each kind of disagreement counts in the fit: an error of this size costs 1.
constexpr double kLengthSigma = 0.5;   // mm
constexpr double kForceSigma = 0.3;    // N
constexpr double kTorqueSigma = 30.0;  // N·mm
constexpr double kAnchorSigma = 300.0; // mm: how far from the tape measurements to trust
constexpr double kMassSigma = 2.0;     // kg

constexpr int kFixed = kLines * 3 + 1;  // pulleys, then the mass
constexpr int kPerPoint = 6;            // the ball's centre and its lean, at each point

std::string fmt(const char* f, double a = 0, double b = 0, double c = 0) {
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

double sq_sum(const std::vector<double>& v) {
    double s = 0.0;
    for (double x : v) s += x * x;
    return s;
}

// Solves A x = b (n × n) in place by Gaussian elimination with partial pivoting.
bool solve(std::vector<double>& A, std::vector<double>& b, int n) {
    for (int c = 0; c < n; ++c) {
        int p = c;
        for (int r = c + 1; r < n; ++r)
            if (std::fabs(A[r * n + c]) > std::fabs(A[p * n + c])) p = r;
        if (std::fabs(A[p * n + c]) < 1e-14) return false;
        if (p != c) {
            for (int k = 0; k < n; ++k) std::swap(A[c * n + k], A[p * n + k]);
            std::swap(b[c], b[p]);
        }
        for (int r = c + 1; r < n; ++r) {
            const double f = A[r * n + c] / A[c * n + c];
            if (f == 0.0) continue;
            for (int k = c; k < n; ++k) A[r * n + k] -= f * A[c * n + k];
            b[r] -= f * b[c];
        }
    }
    for (int c = n - 1; c >= 0; --c) {
        double s = b[c];
        for (int k = c + 1; k < n; ++k) s -= A[c * n + k] * b[k];
        b[c] = s / A[c * n + c];
    }
    return true;
}

// The disagreements for one guess at the pulleys, the mass, and each point's pose.
// Line lengths: the controller counted line from homing, when it assumed the measured
// pulleys; with other pulleys the line at the dock, and so every length since, is
// different by the same amount.
struct Problem {
    const RigConfig& cfg;
    LineArray home_t;
    const std::vector<CalSample>& s;
    LineArray model_dock{};

    Problem(const RigConfig& c, const LineArray& t, const std::vector<CalSample>& samples)
        : cfg(c), home_t(t), s(samples) {
        model_dock = unstretched_lengths(cfg, cfg.dock, home_t);
    }
    int unknowns() const { return kFixed + kPerPoint * static_cast<int>(s.size()); }

    // Fills r, scaled; if `raw_len`/`raw_force` are given, also the unscaled sums of squares.
    void residuals(const std::vector<double>& p, std::vector<double>& r, double* raw_len = nullptr,
                   double* raw_force = nullptr) const {
        r.clear();
        Vec3 a[kLines];
        for (int i = 0; i < kLines; ++i) {
            a[i] = {p[3 * i], p[3 * i + 1], p[3 * i + 2]};
            const Vec3 d = a[i] - cfg.lines[i].anchor;
            r.push_back(d.x / kAnchorSigma);
            r.push_back(d.y / kAnchorSigma);
            r.push_back(d.z / kAnchorSigma);
        }
        const double m = p[kLines * 3];
        r.push_back((m - cfg.mass) / kMassSigma);
        double len2 = 0.0, force2 = 0.0;
        for (size_t k = 0; k < s.size(); ++k) {
            const double* q = &p[kFixed + kPerPoint * k];
            const Vec3 c{q[0], q[1], q[2]}, rot{q[3], q[4], q[5]};
            const Vec3 weight{0.0, 0.0, -m * kGravity};
            Vec3 f = weight, torque = rotate(rot, cfg.centre_of_weight).cross(weight);
            for (int i = 0; i < kLines; ++i) {
                const Vec3 at = rotate(rot, cfg.lines[i].attach);
                const Vec3 d = a[i] - (c + at);
                const double len = d.norm();
                const double true_dock =
                    (a[i] - (cfg.dock + cfg.lines[i].attach)).norm() / (1.0 + home_t[i] / cfg.line_ea);
                const double out = s[k].out[i] - model_dock[i] + true_dock;
                const double e = len - out * (1.0 + s[k].tension[i] / cfg.line_ea);
                len2 += e * e;
                r.push_back(e / kLengthSigma);
                const Vec3 pull = d * (s[k].tension[i] / len);
                f += pull;
                torque += at.cross(pull);
            }
            force2 += f.dot(f);
            r.push_back(f.x / kForceSigma);
            r.push_back(f.y / kForceSigma);
            r.push_back(f.z / kForceSigma);
            r.push_back(torque.x / kTorqueSigma);
            r.push_back(torque.y / kTorqueSigma);
            r.push_back(torque.z / kTorqueSigma);
        }
        if (raw_len) *raw_len = len2;
        if (raw_force) *raw_force = force2;
    }

    double step(int j) const {
        if (j < kLines * 3) return 1e-3;
        if (j == kLines * 3) return 1e-4;
        return (j - kFixed) % kPerPoint < 3 ? 1e-3 : 1e-7;
    }
};

}  // namespace

std::vector<Vec3> calibration_points(const RigConfig& cfg) {
    const Box ws = workspace(cfg);
    const double cx = 0.5 * (ws.lo.x + ws.hi.x), cy = 0.5 * (ws.lo.y + ws.hi.y);
    const double sx = 0.4 * 0.5 * (ws.hi.x - ws.lo.x), sy = 0.4 * 0.5 * (ws.hi.y - ws.lo.y);
    const double zl = std::min(cfg.dock.z + 200.0, ws.hi.z), zh = std::max(ws.hi.z - 100.0, ws.lo.z);
    const double zm = 0.5 * (zl + zh);
    std::vector<Vec3> want;
    if (cfg.two_axis) {
        const double y = cfg.fixed_y;
        want = {{cx, y, zm},      {cx - sx, y, zm}, {cx + sx, y, zm}, {cx + sx, y, zh}, {cx, y, zh},
                {cx - sx, y, zh}, {cx - sx, y, zl}, {cx, y, zl},      {cx + sx, y, zl}};
    } else {
        want = {{cx, cy, zm},      {cx - sx, cy + sy, zm}, {cx + sx, cy + sy, zm}, {cx + sx, cy - sy, zm},
                {cx - sx, cy - sy, zm}, {cx, cy, zh},      {cx + sx, cy, zh},      {cx - sx, cy, zh},
                {cx, cy, zl}};
    }
    std::vector<Vec3> out;
    for (const Vec3& p : want)
        if (check_point(cfg, p, ws) == Spot::Ok) out.push_back(p);
    return out;
}

CalFit fit_calibration(const RigConfig& cfg, const LineArray& home_tension, const std::vector<CalSample>& samples) {
    CalFit out;
    if (samples.size() < 4) {
        out.summary = "Too few points were measured to work anything out.";
        return out;
    }
    Problem prob(cfg, home_tension, samples);
    const int n = prob.unknowns();
    std::vector<double> p(n);
    for (int i = 0; i < kLines; ++i) {
        p[3 * i] = cfg.lines[i].anchor.x;
        p[3 * i + 1] = cfg.lines[i].anchor.y;
        p[3 * i + 2] = cfg.lines[i].anchor.z;
    }
    p[kLines * 3] = cfg.mass;
    for (size_t k = 0; k < samples.size(); ++k) {
        const Pose pose = hanging_pose(cfg, samples[k].target, cfg.plan_tension_min);
        double* q = &p[kFixed + kPerPoint * k];
        q[0] = samples[k].target.x;
        q[1] = samples[k].target.y;
        q[2] = samples[k].target.z;
        q[3] = pose.rot.x;
        q[4] = pose.rot.y;
        q[5] = pose.rot.z;
    }

    // Levenberg–Marquardt with a numerical Jacobian: a few dozen unknowns, so plain
    // dense maths is quick enough even on the controller.
    std::vector<double> r, r2;
    prob.residuals(p, r);
    const int m = static_cast<int>(r.size());
    std::vector<double> J(static_cast<size_t>(m) * n), A(static_cast<size_t>(n) * n), g(n);
    double lambda = 1e-3;
    for (int iter = 0; iter < 80; ++iter) {
        for (int j = 0; j < n; ++j) {
            const double h = prob.step(j);
            std::vector<double> q = p;
            q[j] += h;
            prob.residuals(q, r2);
            for (int i = 0; i < m; ++i) J[static_cast<size_t>(i) * n + j] = (r2[i] - r[i]) / h;
        }
        std::fill(A.begin(), A.end(), 0.0);
        std::fill(g.begin(), g.end(), 0.0);
        for (int i = 0; i < m; ++i) {
            const double* row = &J[static_cast<size_t>(i) * n];
            for (int j = 0; j < n; ++j) {
                if (row[j] == 0.0) continue;
                g[j] -= row[j] * r[i];
                for (int k = 0; k < n; ++k) A[static_cast<size_t>(j) * n + k] += row[j] * row[k];
            }
        }
        bool better = false;
        double gain = 0.0;
        for (int tries = 0; tries < 12 && !better; ++tries) {
            std::vector<double> B = A, d = g;
            for (int j = 0; j < n; ++j) B[static_cast<size_t>(j) * n + j] *= 1.0 + lambda;
            if (!solve(B, d, n)) {
                lambda *= 10.0;
                continue;
            }
            std::vector<double> q = p;
            for (int j = 0; j < n; ++j) q[j] += d[j];
            prob.residuals(q, r2);
            const double before = sq_sum(r), after = sq_sum(r2);
            if (after < before) {
                gain = (before - after) / std::max(before, 1e-12);
                p = q;
                r = r2;
                lambda = std::max(lambda / 10.0, 1e-9);
                better = true;
            } else {
                lambda *= 10.0;
            }
        }
        if (!better || gain < 1e-10) break;
    }

    double len2 = 0.0, force2 = 0.0;
    prob.residuals(p, r, &len2, &force2);
    out.length_rms = std::sqrt(len2 / (kLines * samples.size()));
    out.force_rms = std::sqrt(force2 / (3.0 * samples.size()));
    out.mass = p[kLines * 3];
    double worst = 0.0;
    for (int i = 0; i < kLines; ++i) {
        out.anchors[i] = {p[3 * i], p[3 * i + 1], p[3 * i + 2]};
        out.moved[i] = (out.anchors[i] - cfg.lines[i].anchor).norm();
        worst = std::max(worst, out.moved[i]);
    }

    std::string s;
    const bool fits = out.length_rms < 2.0 && out.force_rms < 1.0;
    if (!fits) {
        s += fmt("The measurements don't agree well (%.1f mm, %.1f N). Something may have touched a line, or the "
                 "wind moved the ball. Try again in still air. ",
                 out.length_rms, out.force_rms);
    }
    std::string far;
    int n_far = 0;
    for (int i = 0; i < kLines; ++i) {
        if (out.moved[i] < 10.0) continue;
        far += (far.empty() ? "" : ", ") + std::string(1, cfg.lines[i].name) + fmt(" %.0f mm", out.moved[i]);
        ++n_far;
    }
    if (n_far == 0) s += "The pulleys are where they were measured, within 10 mm. ";
    else s += std::string(n_far == 1 ? "One pulley is" : "Pulleys are") + " away from where measured: " + far + ". ";
    if (std::fabs(out.mass - cfg.mass) >= 0.05)
        s += fmt("The head weighs %.2f kg, not %.2f kg. ", out.mass, cfg.mass);
    else
        s += "The head weight is right. ";
    if (worst >= 30.0) s += "After using these, home and run the check again: big corrections come out better the second time.";
    while (!s.empty() && s.back() == ' ') s.pop_back();
    out.summary = s;
    out.ok = fits && std::isfinite(out.mass) && out.mass > 0.0;
    return out;
}

// -- CalibrationRun ----------------------------------------------------------------------

std::string CalibrationRun::not_ready(const Rig& rig) {
    if (rig.estop()) return "The E-stop chain is open.";
    if (rig.state() == RigState::Fault) return "Clear the fault first.";
    if (!rig.armed()) return "Arm the rig first.";
    if (!rig.homed()) return "Home at the dock first.";
    if (rig.remote()) return "Turn remote control off first, so only this screen moves the ball.";
    if (rig.state() != RigState::Idle || rig.busy()) return "Wait until the ball is still.";
    if (rig.config().wind.enabled && rig.wind().level() != WindLevel::Green) return "The wind must be Green.";
    return "";
}

bool CalibrationRun::running() const {
    return phase_ == Phase::Flying || phase_ == Phase::Settling || phase_ == Phase::Measuring ||
           phase_ == Phase::Parking;
}

Result CalibrationRun::start(Rig& rig, double now) {
    const std::string why = not_ready(rig);
    if (!why.empty()) return {Refusal::Busy, why};
    cfg_ = rig.config();
    points_ = calibration_points(cfg_);
    if (points_.size() < 4) return {Refusal::OutOfRange, "The workspace is too small to check from."};
    home_t_ = rig.home_tensions();
    samples_.clear();
    fit_ = CalFit{};
    index_ = 0;
    fly(rig, now);
    return {};
}

void CalibrationRun::fly(Rig& rig, double now) {
    const Vec3 to = points_[index_], from = rig.position();
    const double dist = (to - from).norm();
    const double secs = std::max(2.0, dist / speed + 1.0);
    for (int a = 0; a < kAxes; ++a) {
        if (!rig.axis_present(a)) continue;
        const double v = a == 0 ? to.x : a == 1 ? to.y : to.z;
        Result r = rig.move(a, v, std::nullopt, secs, std::nullopt, now);
        if (!r) {
            rig.stop(std::nullopt, now);
            fail("Couldn't fly to the next point: " + r.detail);
            return;
        }
    }
    phase_ = Phase::Flying;
    phase_t_ = now;
    message_ = fmt("Flying to point %.0f of %.0f.", index_ + 1.0, static_cast<double>(points_.size()));
}

void CalibrationRun::fail(const std::string& why) {
    phase_ = Phase::Failed;
    message_ = why;
}

void CalibrationRun::cancel(Rig& rig, double now) {
    if (!running()) return;
    if (phase_ != Phase::Parking) rig.stop(std::nullopt, now);
    fail("Stopped. The ball is where it stopped.");
}

void CalibrationRun::update(Rig& rig, double now) {
    if (!running()) return;
    if (rig.estop() || rig.state() == RigState::Fault) {
        fail("The rig stopped: " + rig.message());
        return;
    }
    if (phase_ == Phase::Parking) {
        if (!rig.armed()) {
            phase_ = Phase::Done;
            message_ = "Done. The ball is back on the dock.";
        }
        return;
    }
    if (!rig.armed()) {
        fail("The rig was disarmed, so the check stopped.");
        return;
    }
    if (rig.remote()) {
        rig.stop(std::nullopt, now);
        fail("Remote control came on, so the check stopped.");
        return;
    }
    if (phase_ == Phase::Flying) {
        if (rig.busy()) return;
        if ((rig.position() - points_[index_]).norm() > 1.0) {
            fail("The move was stopped, so the check stopped.");
            return;
        }
        phase_ = Phase::Settling;
        phase_t_ = now;
        message_ = fmt("Point %.0f of %.0f: letting the ball settle.", index_ + 1.0, static_cast<double>(points_.size()));
    } else if (phase_ == Phase::Settling) {
        if (now - phase_t_ < settle_time) return;
        phase_ = Phase::Measuring;
        phase_t_ = now;
        sum_ = CalSample{};
        count_ = 0;
        message_ = fmt("Point %.0f of %.0f: measuring.", index_ + 1.0, static_cast<double>(points_.size()));
    } else if (phase_ == Phase::Measuring) {
        const auto lines = rig.lines();
        for (int i = 0; i < kLines; ++i) {
            sum_.out[i] += lines[i].out;
            sum_.tension[i] += lines[i].tension;
        }
        ++count_;
        if (now - phase_t_ < measure_time) return;
        CalSample s;
        s.target = rig.position();
        for (int i = 0; i < kLines; ++i) {
            s.out[i] = sum_.out[i] / count_;
            s.tension[i] = sum_.tension[i] / count_;
        }
        samples_.push_back(s);
        if (++index_ < points_.size()) {
            fly(rig, now);
            return;
        }
        fit_ = fit_calibration(cfg_, home_t_, samples_);
        Result r = rig.park(now);
        if (!r) {
            phase_ = Phase::Done;
            message_ = "Done, but the ball couldn't park: " + r.detail;
            return;
        }
        phase_ = Phase::Parking;
        message_ = "Measured. Parking the ball on the dock.";
    }
}

}  // namespace leota
