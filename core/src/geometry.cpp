#include "leota/geometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace leota {

namespace {

constexpr double kInf = std::numeric_limits<double>::infinity();

double det3(const Vec3& a, const Vec3& b, const Vec3& c) { return a.dot(b.cross(c)); }

// Solves M y = b for a symmetric 3×3 M given as rows. Returns false if singular.
bool solve3(const Vec3 m[3], const Vec3& b, Vec3& y) {
    double d = det3(m[0], m[1], m[2]);
    if (std::fabs(d) < 1e-12) return false;
    // Cramer's rule, with the columns of M (equal to its rows: M is symmetric).
    y.x = det3(b, m[1], m[2]) / d;
    y.y = det3(m[0], b, m[2]) / d;
    y.z = det3(m[0], m[1], b) / d;
    return true;
}

}  // namespace

LineArray geometric_lengths(const RigConfig& cfg, const Vec3& c) {
    LineArray out{};
    for (int i = 0; i < kLines; ++i)
        out[i] = (cfg.lines[i].anchor - (c + cfg.lines[i].attach)).norm();
    return out;
}

Tensions expected_tensions(const RigConfig& cfg, const Vec3& c, double tmin, const Vec3& accel) {
    std::array<Vec3, kLines> u;
    for (int i = 0; i < kLines; ++i) {
        Vec3 d = cfg.lines[i].anchor - (c + cfg.lines[i].attach);
        u[i] = d * (1.0 / d.norm());
    }
    // The lines must supply m (a + g) upward, in newtons (accel is in mm/s²).
    const Vec3 b{cfg.mass * accel.x / 1000.0, cfg.mass * accel.y / 1000.0,
                 cfg.mass * (accel.z / 1000.0 + kGravity)};

    // A free direction: n with sum n_i u_i = 0 (signed 3×3 minors of [u0 u1 u2 u3]).
    const double n[kLines] = {det3(u[1], u[2], u[3]), -det3(u[0], u[2], u[3]),
                              det3(u[0], u[1], u[3]), -det3(u[0], u[1], u[2])};
    // A particular answer: the least-squares one, t = Uᵀ (U Uᵀ)⁻¹ b.
    Vec3 m[3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (const Vec3& v : u) {
        m[0] += v * v.x;
        m[1] += v * v.y;
        m[2] += v * v.z;
    }
    Vec3 y;
    Tensions out;
    if (!solve3(m, b, y)) return out;  // all lines in a plane: can't hold anything
    double tp[kLines];
    for (int i = 0; i < kLines; ++i) tp[i] = u[i].dot(y);

    // Every answer is tp + λn. Keep each t_i >= tmin: an interval of λ.
    double lo = -kInf, hi = kInf;
    bool ok = true;
    for (int i = 0; i < kLines; ++i) {
        if (std::fabs(n[i]) < 1e-12) {
            if (tp[i] < tmin) ok = false;
        } else if (n[i] > 0) {
            lo = std::max(lo, (tmin - tp[i]) / n[i]);
        } else {
            hi = std::min(hi, (tmin - tp[i]) / n[i]);
        }
    }
    ok = ok && lo <= hi + 1e-12;

    auto value = [&](double l, bool highest) {
        double v = highest ? -kInf : kInf;
        for (int i = 0; i < kLines; ++i)
            v = highest ? std::max(v, tp[i] + l * n[i]) : std::min(v, tp[i] + l * n[i]);
        return v;
    };
    // The best λ is where two lines cross, or at an end of the interval.
    std::vector<double> cand;
    for (int i = 0; i < kLines; ++i)
        for (int j = i + 1; j < kLines; ++j)
            if (std::fabs(n[i] - n[j]) > 1e-12) cand.push_back((tp[j] - tp[i]) / (n[i] - n[j]));
    double best = 0.0;
    if (ok) {
        if (std::isfinite(lo)) cand.push_back(lo);
        if (std::isfinite(hi)) cand.push_back(hi);
        double bv = kInf;
        bool any = false;
        for (double l : cand) {
            if (l < lo - 1e-9 || l > hi + 1e-9) continue;
            l = std::clamp(l, lo, hi);
            double v = value(l, true);
            if (v < bv) { bv = v; best = l; any = true; }
        }
        if (!any) best = std::isfinite(lo) ? lo : (std::isfinite(hi) ? hi : 0.0);
    } else {
        double bv = -kInf;
        for (double l : cand) {
            double v = value(l, false);
            if (v > bv) { bv = v; best = l; }
        }
    }
    out.feasible = ok;
    const double nn = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2] + n[3] * n[3]);
    for (int i = 0; i < kLines; ++i) {
        out.t[i] = tp[i] + best * n[i];
        out.free[i] = nn > 0 ? n[i] / nn : 0.0;
    }
    return out;
}

LineArray unstretched_lengths(const RigConfig& cfg, const Vec3& c, const LineArray& t) {
    LineArray g = geometric_lengths(cfg, c);
    for (int i = 0; i < kLines; ++i) g[i] /= 1.0 + std::max(0.0, t[i]) / cfg.line_ea;
    return g;
}

const char* spot_text(Spot s) {
    switch (s) {
        case Spot::Ok: return "fine";
        case Spot::Outside: return "outside the workspace";
        case Spot::Table: return "too close to the table";
        case Spot::Slack: return "a line would go slack there";
        case Spot::Overload: return "the lines would pull too hard there";
    }
    return "";
}

Box workspace(const RigConfig& cfg) {
    Box b{{cfg.axes[0].min, cfg.axes[1].min, cfg.axes[2].min},
          {cfg.axes[0].max, cfg.axes[1].max, cfg.axes[2].max}};
    if (cfg.two_axis) b.lo.y = b.hi.y = cfg.fixed_y;
    return b;
}

Spot check_point(const RigConfig& cfg, const Vec3& p, const Box& box) {
    if (!box.contains(p)) return Spot::Outside;
    const double r = cfg.ball_radius;
    const Box keep_out{cfg.table.lo - Vec3{r, r, r}, cfg.table.hi + Vec3{r, r, r}};
    const double dx = p.x - cfg.dock.x, dy = p.y - cfg.dock.y;
    const bool on_dock_column =
        std::sqrt(dx * dx + dy * dy) <= cfg.dock_radius && p.z >= cfg.dock.z - 1.0;
    if (keep_out.contains(p, 0.0) && !on_dock_column) return Spot::Table;
    Tensions t = expected_tensions(cfg, p, cfg.plan_tension_min);
    if (!t.feasible) return Spot::Slack;
    if (*std::max_element(t.t.begin(), t.t.end()) > cfg.plan_tension_max) return Spot::Overload;
    return Spot::Ok;
}

Spot check_path(const RigConfig& cfg, const Vec3& a, const Vec3& b, const Box& box,
                double step) {
    const double len = (b - a).norm();
    const int n = std::max(1, static_cast<int>(std::ceil(len / step)));
    for (int i = 0; i <= n; ++i) {
        Spot s = check_point(cfg, a + (b - a) * (static_cast<double>(i) / n), box);
        if (s != Spot::Ok) return s;
    }
    return Spot::Ok;
}

}  // namespace leota
