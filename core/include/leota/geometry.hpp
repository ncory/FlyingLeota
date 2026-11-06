// Cable geometry: line lengths for a ball position, the tensions the lines should carry,
// and where the ball may go. The ball is treated as staying level (the lean model from
// docs/plan.md section 4 comes later, as a correction table).
#pragma once

#include <array>

#include "leota/config.hpp"

namespace leota {

using LineArray = std::array<double, kLines>;

// Straight-line distance from each pulley to its attachment point.
LineArray geometric_lengths(const RigConfig& cfg, const Vec3& centre);

struct Tensions {
    bool feasible = false;  // every line at or above the minimum
    LineArray t{};          // N
    // The one way to change all four tensions without changing their pull on the
    // ball: tighter on some lines, looser on others (unit length). Paying line out
    // along it changes how hard the lines pull against each other, not where the
    // ball is.
    LineArray free{};
};

// The tensions that hold the ball still at `centre` (or accelerate it at `accel`,
// mm/s²). Four lines and three equations leave one free choice; this picks the spread
// with the lowest highest tension, keeping every line at or above `min_tension`. When
// that can't be done, `feasible` is false and `t` is the spread with the highest lowest
// tension.
Tensions expected_tensions(const RigConfig& cfg, const Vec3& centre, double min_tension,
                           const Vec3& accel = {});

// How much line to pay out so the lines reach the geometric lengths at tensions `t`:
// a line stretches by t / EA of its length.
LineArray unstretched_lengths(const RigConfig& cfg, const Vec3& centre, const LineArray& t);

// Whether the ball's centre may be at a point, and if not, why.
enum class Spot { Ok, Outside, Table, Slack, Overload };
const char* spot_text(Spot s);

// The box the axes' soft limits allow (y pinned in two-axis mode).
Box workspace(const RigConfig& cfg);

Spot check_point(const RigConfig& cfg, const Vec3& p, const Box& box);
// Checks points every `step` mm along the straight path, ends included.
Spot check_path(const RigConfig& cfg, const Vec3& a, const Vec3& b, const Box& box,
                double step = 20.0);

}  // namespace leota
