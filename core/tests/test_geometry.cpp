#include <cmath>
#include "doctest.h"
#include "leota/geometry.hpp"

using namespace leota;

namespace {

// The lines' total pull on the ball (N), with the ball turned by `rot`.
Vec3 net_force(const RigConfig& cfg, const Vec3& c, const LineArray& t, const Vec3& rot = {}) {
    Vec3 f;
    for (int i = 0; i < kLines; ++i) {
        Vec3 d = cfg.lines[i].anchor - (c + rotate(rot, cfg.lines[i].attach));
        f += d * (t[i] / d.norm());
    }
    return f;
}

}  // namespace

TEST_CASE("the default config is sound") {
    CHECK(check_config(gazebo_config()) == "");
}

TEST_CASE("every line is the same length with the ball in the middle") {
    const RigConfig cfg = gazebo_config();
    const LineArray l = geometric_lengths(cfg, cfg.dock);
    for (double x : l) CHECK(x == doctest::Approx(l[0]));
    CHECK(l[0] > 1000.0);
}

TEST_CASE("the tensions hold the ball up, and no more") {
    const RigConfig cfg = gazebo_config();
    const double weight = cfg.mass * kGravity;
    for (const Vec3& p : {cfg.dock, Vec3{-300, 200, 1200}, Vec3{600, -600, 1400}, Vec3{0, 0, 1528}}) {
        const Tensions t = expected_tensions(cfg, p, cfg.tension_min);
        CAPTURE(p.x);
        CAPTURE(p.z);
        REQUIRE(t.feasible);
        const Vec3 f = net_force(cfg, p, t.t);
        CHECK(f.x == doctest::Approx(0.0).epsilon(1e-6).scale(weight));
        CHECK(f.y == doctest::Approx(0.0).epsilon(1e-6).scale(weight));
        CHECK(f.z == doctest::Approx(weight));
        for (double x : t.t) CHECK(x >= cfg.tension_min - 1e-9);
    }
}

TEST_CASE("in the middle all four lines share the load equally") {
    const RigConfig cfg = gazebo_config();
    const Tensions t = expected_tensions(cfg, {0, 0, 1200}, cfg.tension_min);
    for (double x : t.t) CHECK(x == doctest::Approx(t.t[0]));
}

TEST_CASE("accelerating upward takes more pull") {
    const RigConfig cfg = gazebo_config();
    const Vec3 p{100, 0, 1200};
    const Tensions still = expected_tensions(cfg, p, cfg.tension_min);
    const Tensions up = expected_tensions(cfg, p, cfg.tension_min, {0, 0, 500});
    CHECK(net_force(cfg, p, up.t).z == doctest::Approx(cfg.mass * (kGravity + 0.5)));
    CHECK(net_force(cfg, p, up.t).z > net_force(cfg, p, still.t).z);
}

TEST_CASE("lines are paid out a little short, to allow for stretch") {
    const RigConfig cfg = gazebo_config();
    const Vec3 p{0, 0, 1200};
    const Tensions t = expected_tensions(cfg, p, cfg.tension_min);
    const LineArray g = geometric_lengths(cfg, p), u = unstretched_lengths(cfg, p, t.t);
    for (int i = 0; i < kLines; ++i) CHECK(u[i] * (1.0 + t.t[i] / cfg.line_ea) == doctest::Approx(g[i]));
}

TEST_CASE("right under one pulley, the other lines can't stay tight") {
    const RigConfig cfg = gazebo_config();
    const Vec3 under_a = cfg.lines[0].anchor - cfg.lines[0].attach - Vec3{0, 0, 600};
    CHECK_FALSE(expected_tensions(cfg, under_a, cfg.tension_min).feasible);
    const Box everywhere{{-5000, -5000, 0}, {5000, 5000, 5000}};
    CHECK(check_point(cfg, under_a, everywhere) == Spot::Slack);
}

TEST_CASE("a heavy ball near the pulleys overloads the lines") {
    RigConfig cfg = gazebo_config();
    cfg.mass = 20.0;
    CHECK(check_point(cfg, {0, 0, cfg.axes[2].max}, workspace(cfg)) == Spot::Overload);
}

TEST_CASE("the workspace check keeps the ball inside its limits and off the table") {
    RigConfig cfg = gazebo_config();
    const Box ws = workspace(cfg);
    CHECK(check_point(cfg, {0, 0, 1200}, ws) == Spot::Ok);
    CHECK(check_point(cfg, cfg.dock, ws) == Spot::Ok);
    CHECK(check_point(cfg, {0, 0, cfg.axes[2].max + 1}, ws) == Spot::Outside);
    CHECK(check_point(cfg, {cfg.axes[0].max + 1, 0, 1200}, ws) == Spot::Outside);

    cfg.axes[2].min = 300.0;  // let it come down beside the table
    const Box low = workspace(cfg);
    // The table is 900 mm square: its keep-out reaches 628 mm out from the middle.
    CHECK(check_point(cfg, {-650, 0, 600}, low) == Spot::Ok);
    CHECK(check_point(cfg, {-600, 0, 600}, low) == Spot::Table);
    CHECK(check_point(cfg, {0, 0, 860}, low) == Spot::Table);
    CHECK(check_path(cfg, {-650, 0, 600}, {650, 0, 600}, low) == Spot::Table);
    CHECK(check_path(cfg, {-650, 0, 1000}, {650, 0, 1000}, low) == Spot::Ok);
}

TEST_CASE("two-axis mode pins the depth") {
    RigConfig cfg = gazebo_config();
    cfg.two_axis = true;
    CHECK(check_config(cfg) == "");
    const Box ws = workspace(cfg);
    CHECK(check_point(cfg, {0, 0, 1200}, ws) == Spot::Ok);
    CHECK(check_point(cfg, {0, 100, 1200}, ws) == Spot::Outside);
}

TEST_CASE("in the middle the ball hangs upright") {
    const RigConfig cfg = gazebo_config();
    const Pose p = hanging_pose(cfg, {0, 0, 1200}, cfg.plan_tension_min);
    REQUIRE(p.ok);
    CHECK(p.rot.norm() < 1e-6);
}

TEST_CASE("off centre the ball leans a few degrees, but stays square to the audience") {
    const RigConfig cfg = gazebo_config();
    const double deg = 180.0 / 3.14159265358979;
    for (const Vec3& c : {Vec3{500, 0, 1230}, Vec3{500, 500, 1230}, Vec3{-600, 300, 1000}}) {
        CAPTURE(c.x);
        CAPTURE(c.y);
        const Pose p = hanging_pose(cfg, c, cfg.plan_tension_min);
        REQUIRE(p.ok);
        const Vec3 tau = net_torque(cfg, c, p.rot, p.t.t);
        CHECK(tau.norm() < 1e-3);
        const Vec3 f = net_force(cfg, c, p.t.t, p.rot);  // the lines hold the ball's weight
        CHECK(std::hypot(f.x, f.y) < 1e-6);
        CHECK(f.z == doctest::Approx(cfg.mass * kGravity));
        const double tilt = std::hypot(p.rot.x, p.rot.y) * deg, turn = std::fabs(p.rot.z) * deg;
        CHECK(tilt > 1.5);
        CHECK(tilt < 8.0);
        CHECK(turn < 0.1);
    }
    // Along the x axis it tips about the y axis only.
    const Pose side = hanging_pose(cfg, {500, 0, 1230}, cfg.plan_tension_min);
    CHECK(std::fabs(side.rot.x) < 1e-6);
    CHECK(std::fabs(side.rot.y) * deg == doctest::Approx(3.3).epsilon(0.2));
}

TEST_CASE("lines from a leaning ball reach their own attachment points") {
    const RigConfig cfg = gazebo_config();
    const Vec3 c{400, -300, 1300};
    const Pose p = hanging_pose(cfg, c, cfg.plan_tension_min);
    const LineArray level = geometric_lengths(cfg, c), leaning = geometric_lengths(cfg, c, p.rot);
    double most = 0.0;
    for (int i = 0; i < kLines; ++i) most = std::max(most, std::fabs(level[i] - leaning[i]));
    CHECK(most > 1.0);   // the lean moves the attachment points by millimetres
    CHECK(most < 10.0);
}
