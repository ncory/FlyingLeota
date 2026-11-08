#include <cmath>

#include "doctest.h"
#include "leota/line_test.hpp"
#include "leota/settings.hpp"
#include "sim.hpp"

using namespace leota;
using test::Sim;

namespace {

// Drives and load cells that don't read true, a little differently on each line.
std::array<LineSensors, kLines> off_sensors() {
    return {{{1.015, 2.5, 0.96}, {0.99, -1.8, 1.03}, {1.02, 3.1, 0.98}, {0.985, -2.2, 1.05}}};
}

double worst_landing(Sim& sim) {
    double worst = 0.0;
    for (const Vec3& p : {Vec3{0, 0, 1400}, Vec3{300, 300, 1200}, Vec3{-300, 200, 1500}, Vec3{200, -300, 1100}}) {
        REQUIRE(sim.fly_to(p, 3.0));
        sim.run(1.0);
        worst = std::max(worst, (sim.plant.ball() - p).norm());
    }
    return worst;
}

// Runs the rig and the test together until the test isn't busy.
void settle(Sim& sim, LineTest& t, double limit = 30.0) {
    for (double end = sim.t + limit; sim.t < end;) {
        sim.run(0.05);
        t.update(sim.rig, sim.t);
        if (!t.busy()) return;
    }
}

}  // namespace

TEST_CASE("drives and load cells that don't read true put the ball off, until corrected") {
    RigConfig c = gazebo_config();
    Sim off(c);
    off.plant.set_sensors(off_sensors());
    REQUIRE(off.arm_and_home());
    const double uncorrected = worst_landing(off);

    c.sensors = off_sensors();
    Sim fixed(c);
    fixed.plant.set_sensors(off_sensors());
    REQUIRE(fixed.arm_and_home());
    const double corrected = worst_landing(fixed);
    MESSAGE("worst landing uncorrected ", uncorrected, " mm, corrected ", corrected, " mm");
    CHECK(uncorrected > 5.0);
    CHECK(corrected < 0.5);
    const auto lines = fixed.rig.lines();
    for (int i = 0; i < kLines; ++i) CHECK(lines[i].tension == doctest::Approx(lines[i].expected).epsilon(0.02));
}

TEST_CASE("the line test measures a line's load cell and drum") {
    Sim sim;
    sim.plant.set_sensors(off_sensors());
    const LineSensors truth = off_sensors()[1];
    LineTest t;
    REQUIRE(t.start(sim.rig, 1));
    CHECK(t.step() == LineTest::Step::Empty);

    sim.plant.hang(1, 0.0);  // unhooked, nothing on it
    REQUIRE(t.measure(sim.rig, sim.t));
    settle(sim, t);
    REQUIRE(t.step() == LineTest::Step::Weight);
    CHECK(t.result().load_zero == doctest::Approx(truth.load_zero).epsilon(0.001));

    sim.plant.hang(1, 5.0);
    t.set_weight(5.0);
    REQUIRE(t.measure(sim.rig, sim.t));
    settle(sim, t);
    REQUIRE(t.step() == LineTest::Step::Raise);
    CHECK(t.result().load_scale == doctest::Approx(truth.load_scale).epsilon(0.001));

    CHECK_FALSE(t.raise(sim.rig, sim.t));  // not armed
    REQUIRE(sim.arm());
    CHECK_FALSE(t.raise(sim.rig, sim.t));  // remote control on
    sim.rig.set_remote(false, sim.t);
    const double before = sim.plant.hang_height(1);
    REQUIRE(t.raise(sim.rig, sim.t));
    settle(sim, t);
    REQUIRE(t.step() == LineTest::Step::Rise);
    const double rose = sim.plant.hang_height(1) - before;
    CHECK_FALSE(t.rise(200.0));  // nonsense is refused
    REQUIRE(t.rise(std::round(rose)));
    CHECK(t.step() == LineTest::Step::Done);
    CHECK(t.result().drum == doctest::Approx(truth.drum).epsilon(0.002));
    CHECK_FALSE(sim.rig.homed());
}

TEST_CASE("a weight that isn't hanging is noticed") {
    Sim sim;
    LineTest t;
    REQUIRE(t.start(sim.rig, 0));
    sim.plant.hang(0, 0.0);
    REQUIRE(t.measure(sim.rig, sim.t));
    settle(sim, t);
    t.set_weight(5.0);  // but nothing hung
    REQUIRE(t.measure(sim.rig, sim.t));
    settle(sim, t);
    CHECK(t.step() == LineTest::Step::Weight);
    CHECK(t.message().find("barely changed") != std::string::npos);
}

TEST_CASE("STOP while raising means raising again") {
    Sim sim;
    LineTest t;
    REQUIRE(t.start(sim.rig, 2));
    sim.plant.hang(2, 0.0);
    t.measure(sim.rig, sim.t);
    settle(sim, t);
    sim.plant.hang(2, 4.0);
    t.set_weight(4.0);
    t.measure(sim.rig, sim.t);
    settle(sim, t);
    REQUIRE(sim.arm());
    sim.rig.set_remote(false, sim.t);
    REQUIRE(t.raise(sim.rig, sim.t));
    sim.run(2.0);
    t.update(sim.rig, sim.t);
    sim.rig.stop(std::nullopt, sim.t);
    settle(sim, t);
    CHECK(t.step() == LineTest::Step::Raise);
    CHECK(t.message().find("before the end") != std::string::npos);
}

TEST_CASE("moving one line alone needs the rig armed, still and local, and means homing again") {
    Sim sim;
    CHECK_FALSE(sim.rig.move_line(0, 100, sim.t));
    REQUIRE(sim.arm_and_home());
    CHECK(sim.rig.move_line(0, 100, sim.t).detail.find("remote") != std::string::npos);
    sim.rig.set_remote(false, sim.t);
    CHECK_FALSE(sim.rig.move_line(7, 100, sim.t));
    REQUIRE(sim.rig.move_line(0, -20, sim.t));
    CHECK(sim.rig.busy());
    CHECK_FALSE(sim.rig.homed());
    REQUIRE(sim.until([&] { return !sim.rig.moving_line(); }, 3.0));
    CHECK(sim.rig.state() == RigState::Idle);
}

TEST_CASE("sensor corrections are settings; a new drum means homing again") {
    Settings s = settings_for(gazebo_config());
    find_field("sensor.C.drum")->set(s, 1.02);
    find_field("sensor.C.zero")->set(s, -1.5);
    Settings back = settings_for(gazebo_config());
    CHECK(settings_from_json(settings_to_json(s), back).empty());
    CHECK(back.rig.sensors[2].drum == doctest::Approx(1.02));
    CHECK(back.rig.sensors[2].load_zero == doctest::Approx(-1.5));
    CHECK(field_text(*find_field("sensor.C.drum"), s) == "1.0200");

    RigConfig a = gazebo_config(), b = a;
    b.sensors[0].load_zero = 2.0;
    CHECK_FALSE(Rig::needs_homing(a, b));
    b.sensors[0].drum = 1.01;
    CHECK(Rig::needs_homing(a, b));
    b.sensors[0].drum = 3.0;
    CHECK(check_config(b).find("far from 1") != std::string::npos);
}
