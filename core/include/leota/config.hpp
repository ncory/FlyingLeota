// Everything about one rig that the controller needs: where the pulleys are, the ball,
// the workspace, the limits and the timings. Units: mm, s, N, kg unless a name says
// otherwise. Positions are of the ball's centre, in the stage frame (see vec3.hpp).
#pragma once

#include <array>
#include <string>

#include "leota/vec3.hpp"

namespace leota {

constexpr int kLines = 4;
constexpr int kAxes = 3;               // x, y, z
constexpr double kGravity = 9.80665;   // m/s²

struct AxisLimits {
    double min = 0.0, max = 0.0;       // mm: soft limits for the ball's centre
    double max_velocity = 0.0;         // mm/s
    double max_accel = 0.0;            // mm/s²
};

struct Box {
    Vec3 lo, hi;
    bool contains(const Vec3& p, double eps = 1e-6) const {
        return p.x >= lo.x - eps && p.x <= hi.x + eps && p.y >= lo.y - eps &&
               p.y <= hi.y + eps && p.z >= lo.z - eps && p.z <= hi.z + eps;
    }
};

struct Line {
    char name = 'A';
    Vec3 anchor;   // where the line leaves its top pulley
    Vec3 attach;   // where it fixes to the ball, from the ball's centre
};

// Corrections for one line's drive and load cell. The drives report line in mm from
// the drum's nominal size, and the load cells newtons from their data sheet; these
// make both read true. Setup's line test measures them.
struct LineSensors {
    double drum = 1.0;        // real mm of line per mm the drive counts
    double load_zero = 0.0;   // N the load cell reads with nothing pulling
    double load_scale = 1.0;  // real N per N it reads above its zero
};

// The dock's charger (docs/plan.md, section 4: inductive charging). The controller turns it
// on whenever the ball rests on the dock, armed or not, and tells what it's doing from the
// power it draws.
struct DockConfig {
    bool while_armed = true;    // charge while armed too (off: only while disarmed)
    double charging_w = 3.0;    // W: drawing more, the ball is charging; less, it's charged
    double empty_w = 1.5;       // W: the most the charger draws with nothing on its pad
    double max_w = 45.0;        // W: drawing more is a fault (something metal on the pad?)
    double settle = 2.0;        // s after switching on before its reading counts
    double probe_every = 30.0;  // s: while the ball is away, check the pad is empty this often (0: never)
    double probe_time = 1.0;    // s: how long each check switches the charger on
    double retry = 60.0;        // s: after an overload, wait this long before trying again
};

struct WindConfig {
    bool enabled = true;         // false: no wind sensor fitted (bench tests)
    double amber = 7.0;          // m/s: a gust at or above this is Amber
    double red = 11.0;           // m/s: a gust at or above this is Red
    double hold = 900.0;         // s of Green before a Red hold ends
    double sensor_timeout = 5.0; // s without a reading: Red
    double gust_seconds = 3.0;   // a gust is the strongest average over this long
    double gust_window = 600.0;  // s: gusts are remembered this long
    double average_window = 120.0;
    double swing_amber = 1.5;    // N: a line's tension swing, while the ball is still
    double swing_red = 3.0;
    double amber_speed = 0.5;    // share of top speed allowed at Amber
    Box amber_box;               // where the ball may go at Amber
};

struct RigConfig {
    std::string name;
    std::array<Line, kLines> lines;  // A back left, B back right, C front left, D front right
    std::array<LineSensors, kLines> sensors{};
    double ball_radius = 0.0;
    double mass = 0.0;               // kg: everything that flies
    Vec3 centre_of_weight;           // from the ball's centre; below it, so the ball hangs upright
    bool lean_model = true;          // allow for the ball leaning (docs/plan.md, section 4)
    int lean_every = 10;             // re-solve the lean every this many cycles (it changes slowly)
    double line_ea = 0.0;            // N: line stiffness (force per unit stretch)

    std::array<AxisLimits, kAxes> axes;  // x, y, z
    bool two_axis = false;           // fly in a fixed depth plane: y stays at fixed_y
    double fixed_y = 0.0;

    Vec3 dock;                       // ball centre when resting on the dock
    double dock_radius = 0.0;        // the dock's cup keeps the ball within this of its centre
    double dock_zone = 0.0;          // within this above the dock, tension checks are off
    Box table;                       // keep-out: the ball stays ball_radius clear of it

    double control_period = 0.001;   // s: one control cycle
    double smoothing = 0.0;          // s: the jerk-limiting filter's length

    double tension_min = 0.0;        // N: below this a line counts as slack
    double tension_max = 0.0;        // N: above this a line is overloaded
    double plan_tension_min = 0.0;   // N: plans keep every line at least this tight
    double plan_tension_max = 0.0;   // N: moves may not plan a static tension above this
    double slack_time = 0.0;         // s a line may stay slack before it's a fault
    double mismatch_limit = 0.0;     // N between measured and expected tension
    double mismatch_time = 0.0;      // s
    double trim_gain = 0.0;          // mm of line per N of error per s
    double trim_limit = 0.0;         // mm: the most the trim may add to a line

    double pretension = 0.0;         // N: homing reels each line in until this
    double homing_speed = 0.0;       // mm/s of line while reeling in
    double homing_reel_max = 0.0;    // mm: give up if a line hasn't gone tight by then
    double homing_lift = 0.0;        // mm: lift after homing, to check the tensions
    double homing_settle = 0.0;      // s to wait before the check

    double arm_timeout = 0.0;        // s for the drives to say they're ready
    double brake_time = 0.0;         // s for a brake to grip or release
    double heartbeat_timeout = 0.0;  // s
    bool remote_arm = true;          // Show Controller may arm the rig (else only the screen)
    double jog_hold = 0.0;           // s a jog lasts without a repeat
    double park_speed = 0.0;         // mm/s when parking
    double park_sink = 0.0;          // mm below the dock height when setting down

    WindConfig wind;
    DockConfig charger;  // the dock's charger
};

// The 2026 site: a gazebo with posts 91 in apart, pulleys at 79 in, a 14 in ball
// resting on a pedestal 0.75 m up, a 4 kg head. See docs/plan.md, section 4.
RigConfig gazebo_config();

// Problems with a config, in plain sentences. Empty when it is fine.
std::string check_config(const RigConfig& cfg);

}  // namespace leota
