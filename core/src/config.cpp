#include "leota/config.hpp"

#include <cmath>

#include "leota/geometry.hpp"

namespace leota {

namespace {
constexpr double kInch = 25.4;
}

RigConfig gazebo_config() {
    RigConfig c;
    c.name = "Flying Leota";

    const double half = 91.0 * kInch / 2.0;  // posts 91 in apart, centre to centre
    const double top = 79.0 * kInch;         // pulleys on the roof's corner pieces
    c.ball_radius = 14.0 * kInch / 2.0;
    // Four attachment points 10 cm apart across the diagonal, on top of the ball.
    const double r = 50.0, k = r / std::sqrt(2.0);
    const double za = std::sqrt(c.ball_radius * c.ball_radius - r * r);
    c.lines = {{
        {'A', {-half, +half, top}, {-k, +k, za}},
        {'B', {+half, +half, top}, {+k, +k, za}},
        {'C', {-half, -half, top}, {-k, -k, za}},
        {'D', {+half, -half, top}, {+k, -k, za}},
    }};
    c.mass = 4.0;
    c.line_ea = 5000.0;  // black UHMWPE braid of about 0.6 mm, roughly

    c.dock = {0.0, 0.0, 750.0 + c.ball_radius};  // pedestal top at 0.75 m
    c.dock_radius = 40.0;
    c.dock_zone = 25.0;
    c.table = {{-450.0, -450.0, 0.0}, {450.0, 450.0, 700.0}};

    const double side = half - c.ball_radius - 300.0;      // 0.3 m clear of the curtains
    const double zmax = top - 300.0 - c.ball_radius;       // ball top 12 in below the pulleys
    c.axes = {{
        {-std::floor(side), std::floor(side), 300.0, 500.0},
        {-std::floor(side), std::floor(side), 300.0, 500.0},
        {std::floor(c.dock.z), std::floor(zmax), 300.0, 500.0},
    }};

    c.control_period = 0.001;
    c.smoothing = 0.15;

    c.tension_min = 5.0;
    c.tension_max = 150.0;
    c.plan_tension_min = 8.0;
    c.plan_tension_max = 120.0;
    c.slack_time = 0.1;
    c.mismatch_limit = 15.0;
    c.mismatch_time = 0.5;
    c.trim_gain = 0.3;
    c.trim_limit = 20.0;

    c.pretension = 8.0;
    c.homing_speed = 10.0;
    c.homing_reel_max = 300.0;
    c.homing_lift = 30.0;
    c.homing_settle = 0.5;

    c.arm_timeout = 1.0;
    c.brake_time = 0.2;
    c.heartbeat_timeout = 0.5;
    c.jog_hold = 0.25;
    c.park_speed = 100.0;
    c.park_sink = 15.0;

    c.wind.amber_box = {{-400.0, -400.0, c.axes[2].min}, {400.0, 400.0, 1300.0}};
    return c;
}

std::string check_config(const RigConfig& c) {
    std::string out;
    auto say = [&out](const std::string& s) { out += s + "\n"; };
    if (c.mass <= 0) say("The mass must be more than 0.");
    if (c.line_ea <= 0) say("The line stiffness must be more than 0.");
    if (c.ball_radius <= 0) say("The ball radius must be more than 0.");
    if (c.control_period <= 0 || c.control_period > 0.01)
        say("The control period must be between 0 and 10 ms.");
    if (c.smoothing < 0) say("Smoothing can't be negative.");
    const char* names[kAxes] = {"x", "y", "z"};
    for (int i = 0; i < kAxes; ++i) {
        const AxisLimits& a = c.axes[i];
        if (!(a.min < a.max)) say(std::string("Axis ") + names[i] + ": min must be below max.");
        if (a.max_velocity <= 0 || a.max_accel <= 0)
            say(std::string("Axis ") + names[i] + ": speed and acceleration must be more than 0.");
    }
    for (const Line& l : c.lines) {
        if (l.anchor.z <= c.axes[2].max + c.ball_radius)
            say(std::string("Line ") + l.name + "'s pulley is below the top of the workspace.");
    }
    if (c.tension_min <= 0 || c.tension_max <= c.tension_min)
        say("Tension limits must be above 0, with the maximum above the minimum.");
    if (c.plan_tension_max > c.tension_max || c.plan_tension_min < c.tension_min ||
        c.plan_tension_min >= c.plan_tension_max)
        say("Planning tensions must sit inside the tension limits.");
    // Homing must not lift the ball: the lines' pull at pretension must stay under its weight.
    double lift = 0.0;
    for (const Line& l : c.lines) {
        Vec3 d = l.anchor - (c.dock + l.attach);
        lift += c.pretension * d.z / d.norm();
    }
    if (lift >= c.mass * kGravity)
        say("The homing pretension would lift the ball off the dock. Lower it.");
    if (c.pretension <= 0 || c.pretension >= c.tension_max) say("The pretension is out of range.");
    if (c.two_axis && std::fabs(c.dock.y - c.fixed_y) > 0.5)
        say("In two-axis mode the dock must sit in the fixed depth plane.");
    return out;
}

}  // namespace leota
