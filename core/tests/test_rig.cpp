#include <cmath>
#include <cstdio>

#include "doctest.h"
#include "sim.hpp"

using namespace leota;
using test::Sim;

namespace {

RigConfig quick_wind() {
    RigConfig c = gazebo_config();
    c.wind.hold = 20.0;
    c.wind.gust_window = 10.0;  // a short memory, so tests don't wait 10 minutes
    return c;
}

double dist(const Vec3& a, const Vec3& b) { return (a - b).norm(); }

}  // namespace

TEST_CASE("it starts disarmed, with the brakes on") {
    Sim s;
    CHECK(s.rig.state() == RigState::Disarmed);
    CHECK_FALSE(s.plant.enabled());
    CHECK_FALSE(s.plant.brakes_released());
    CHECK(s.plant.resting());
}

TEST_CASE("arming turns the drives on first, then releases the brakes") {
    Sim s;
    REQUIRE(s.rig.arm(s.t));
    CHECK(s.rig.armed());
    s.run(0.01);
    CHECK(s.plant.enabled());
    CHECK(s.plant.brakes_released());  // the simulated drives are ready at once
    CHECK(s.rig.state() == RigState::Arming);
    s.run(0.3);
    CHECK(s.rig.state() == RigState::Idle);
    CHECK_FALSE(s.rig.homed());
}

TEST_CASE("homing at the dock finds every line and lifts to check") {
    Sim s;
    REQUIRE(s.arm_and_home());
    CHECK(s.rig.homed());
    CHECK(s.rig.message() == "Homed at the dock.");
    const Vec3 want = s.cfg.dock + Vec3{0, 0, s.cfg.homing_lift};
    CHECK(dist(s.rig.position(), want) < 0.01);
    CHECK(dist(s.plant.ball(), want) < 0.5);
    CHECK_FALSE(s.plant.resting());
    for (const LineStatus& l : s.rig.lines()) CHECK(l.tension == doctest::Approx(l.expected).epsilon(0.02));
}

TEST_CASE("moves are refused until the rig is armed and homed") {
    Sim s;
    CHECK(s.rig.move(2, 1200, std::nullopt, 2.0, std::nullopt, s.t).error == Refusal::Disarmed);
    REQUIRE(s.arm());
    CHECK(s.rig.move(2, 1200, std::nullopt, 2.0, std::nullopt, s.t).error == Refusal::NotHomed);
    CHECK(s.rig.jog(0, 50, s.t).error == Refusal::NotHomed);
}

TEST_CASE("the ball follows a timed move and arrives on time") {
    Sim s;
    REQUIRE(s.arm_and_home());
    const double start = s.t;
    Result r = s.rig.move(2, 1300, std::nullopt, 3.0, std::nullopt, s.t);
    REQUIRE(r);
    CHECK(r.late == 0.0);
    CHECK(s.rig.state() == RigState::Moving);
    REQUIRE(s.until([&] { return !s.rig.busy(); }, 6.0));
    CHECK(s.t - start == doctest::Approx(3.0).epsilon(0.02));
    CHECK(s.rig.state() == RigState::Idle);
    CHECK(dist(s.plant.ball(), {0, 0, 1300}) < 0.5);
}

TEST_CASE("moves on several axes together fly the ball anywhere in the workspace") {
    Sim s;
    REQUIRE(s.arm_and_home());
    for (const Vec3& p : {Vec3{-500, 300, 1400}, Vec3{600, -600, 1100}, Vec3{0, 0, 1500}, Vec3{-677, -677, 1000}}) {
        CAPTURE(p.x);
        CAPTURE(p.y);
        REQUIRE(s.fly_to(p, 3.0));
        CHECK(dist(s.plant.ball(), p) < 0.5);
        CHECK(s.rig.faults() == 0);
    }
}

TEST_CASE("a move that's too quick for the limits says how late it will be") {
    Sim s;
    REQUIRE(s.arm_and_home());
    Result r = s.rig.move(2, 1500, std::nullopt, 0.5, std::nullopt, s.t);
    REQUIRE(r);
    CHECK(r.late > 1.0);
}

TEST_CASE("targets outside the limits are refused, never clamped") {
    Sim s;
    REQUIRE(s.arm_and_home());
    Result r = s.rig.move(0, 2000, std::nullopt, std::nullopt, std::nullopt, s.t);
    CHECK(r.error == Refusal::OutOfRange);
    char want[80];
    std::snprintf(want, sizeof want, "x 2000 is outside %g to %g.", s.cfg.axes[0].min, s.cfg.axes[0].max);
    CHECK(r.detail == want);
    CHECK(s.rig.move(5, 0, std::nullopt, std::nullopt, std::nullopt, s.t).error == Refusal::NoAxis);
}

TEST_CASE("an E-stop cuts power, holds the ball, and needs arming afterwards") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.rig.move(2, 1400, std::nullopt, 3.0, std::nullopt, s.t));
    s.run(1.0);
    s.plant.set_estop(true);
    s.run(0.01);
    CHECK(s.rig.state() == RigState::EStop);
    CHECK(s.rig.estop());
    CHECK_FALSE(s.rig.armed());
    CHECK_FALSE(s.plant.brakes_released());
    const Vec3 held = s.plant.ball();
    s.run(1.0);
    CHECK(dist(s.plant.ball(), held) < 1e-9);  // the brakes hold it where it was
    CHECK(s.rig.arm(s.t).error == Refusal::EStop);

    s.plant.set_estop(false);  // twist the button and press Reset
    s.run(0.01);
    CHECK(s.rig.state() == RigState::Fault);
    CHECK((s.rig.faults() & faults::EStop) != 0);
    REQUIRE(s.arm());
    CHECK(s.rig.faults() == 0);
    CHECK(s.rig.homed());
    CHECK(dist(s.rig.position(), held) < 0.5);
    REQUIRE(s.fly_to({0, 0, 1200}, 2.0));
    CHECK(dist(s.plant.ball(), {0, 0, 1200}) < 0.5);
}

TEST_CASE("a lost heartbeat stops and disarms, but only under remote control") {
    Sim s;
    REQUIRE(s.arm_and_home());
    s.rig.heard(s.t);
    s.run(0.4);
    CHECK(s.rig.armed());
    s.run(0.2);
    CHECK((s.rig.faults() & faults::HeartbeatLost) != 0);
    CHECK_FALSE(s.rig.armed());
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Fault; }, 2.0));
    CHECK_FALSE(s.plant.brakes_released());

    s.rig.set_remote(false, s.t);
    REQUIRE(s.arm());
    s.run(2.0);
    CHECK(s.rig.armed());  // local: the person at the screen is in charge
}

TEST_CASE("turning remote control off stops a move in progress") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.rig.move(2, 1500, std::nullopt, 4.0, std::nullopt, s.t));
    s.run(1.0);
    s.rig.set_remote(false, s.t);
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Idle; }, 3.0));
    CHECK(s.rig.armed());
    CHECK(s.rig.position().z < 1400);
}

TEST_CASE("Red wind parks the ball on the dock and refuses to arm until it's calm") {
    Sim s(quick_wind());
    REQUIRE(s.arm_and_home());
    REQUIRE(s.fly_to({300, 200, 1300}, 2.0));
    s.wind = 13.0;
    REQUIRE(s.until([&] { return s.rig.parking(); }, 5.0));
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Fault; }, 30.0));
    CHECK((s.rig.faults() & faults::Wind) != 0);
    CHECK(s.plant.resting());
    CHECK_FALSE(s.plant.brakes_released());
    CHECK(s.rig.arm(s.t).error == Refusal::Weather);

    s.wind = 2.0;
    s.run(15.0);  // the gust has left the (short) window, but the hold isn't over
    CHECK(s.rig.arm(s.t).error == Refusal::Weather);
    s.run(30.0);
    REQUIRE(s.arm());
    CHECK(s.rig.faults() == 0);
}

TEST_CASE("Amber wind keeps the ball low and central, and slower") {
    Sim s(quick_wind());
    REQUIRE(s.arm_and_home());
    s.wind = 8.0;
    s.run(4.0);
    REQUIRE(s.rig.wind().level() == WindLevel::Amber);
    CHECK(s.rig.move(2, 1500, std::nullopt, std::nullopt, std::nullopt, s.t).error == Refusal::Weather);
    Result r = s.rig.move(2, 1250, std::nullopt, 1.0, std::nullopt, s.t);
    REQUIRE(r);
    CHECK(r.late > 0.0);  // at half speed it can't make it in 1 s
}

TEST_CASE("a cut line is caught as slack, and the rig stops and disarms") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.fly_to({0, 0, 1300}, 2.0));
    s.plant.cut_line(1);
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Fault; }, 3.0));
    CHECK((s.rig.faults() & faults::SlackLine) != 0);
    CHECK(s.rig.message() == "Line B went slack.");
    CHECK_FALSE(s.plant.brakes_released());
}

TEST_CASE("a drive alarm puts the brakes on at once") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.rig.move(2, 1400, std::nullopt, 3.0, std::nullopt, s.t));
    s.run(0.5);
    s.plant.set_alarm(2, true);
    s.run(0.002, 0.001);
    CHECK(s.rig.state() == RigState::Fault);
    CHECK((s.rig.faults() & faults::DriveAlarm) != 0);
    CHECK_FALSE(s.plant.brakes_released());
    CHECK_FALSE(s.plant.enabled());
}

TEST_CASE("pulleys a little off where the config says still fly true") {
    // Homing measures every line at the dock, which soaks up most of the error.
    for (const Vec3& err : {Vec3{8, -5, 6}, Vec3{30, 0, 0}, Vec3{120, 0, -80}}) {
        CAPTURE(err.x);
        CAPTURE(err.z);
        RigConfig truth = gazebo_config();
        truth.lines[0].anchor += err;
        Sim s(gazebo_config(), truth);
        REQUIRE(s.arm_and_home());
        const double allowed = err.norm() < 40 ? 2.0 : 20.0;  // mm
        for (const Vec3& p : {Vec3{0, 0, 1300}, Vec3{-400, 300, 1400}, Vec3{600, -600, 1000}}) {
            REQUIRE(s.fly_to(p, 3.0));
            s.run(2.0);
            CHECK(dist(s.plant.ball(), p) < allowed);
        }
        CHECK(s.rig.faults() == 0);
    }
}

TEST_CASE("something pulling on the ball shows up as a tension mismatch") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.fly_to({0, 0, 1300}, 2.0));
    s.plant.push({0, -60, 0});  // a curtain caught on it, hauling toward the audience
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Fault; }, 5.0));
    CHECK((s.rig.faults() & faults::TensionMismatch) != 0);
    CHECK_FALSE(s.plant.brakes_released());
}

TEST_CASE("homing gives up if a line never goes tight") {
    Sim s;
    s.plant.cut_line(3);
    REQUIRE(s.arm());
    REQUIRE(s.rig.home(s.t));
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Fault; }, 60.0));
    CHECK((s.rig.faults() & faults::HomingFailed) != 0);
    CHECK_FALSE(s.rig.homed());
}

TEST_CASE("jog moves while it's repeated, and stops by itself") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.fly_to({0, 0, 1200}, 2.0));
    for (int k = 0; k < 10; ++k) {
        REQUIRE(s.rig.jog(0, 100, s.t));
        s.run(0.1);
    }
    s.run(1.5);
    CHECK_FALSE(s.rig.busy());
    CHECK(s.rig.position().x > 80);
    CHECK(s.rig.position().x < 120);
    CHECK(dist(s.plant.ball(), s.rig.position()) < 0.5);
}

TEST_CASE("jog stops at the workspace limits") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.fly_to({600, 0, 1200}, 2.0));
    for (int k = 0; k < 20; ++k) {
        REQUIRE(s.rig.jog(0, 300, s.t));
        s.run(0.1);
    }
    s.run(1.0);
    CHECK(s.rig.position().x == doctest::Approx(s.cfg.axes[0].max).epsilon(0.001));
}

TEST_CASE("parking sets the ball down on the dock and disarms") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.fly_to({-300, 400, 1350}, 2.0));
    REQUIRE(s.rig.park(s.t));
    CHECK(s.rig.move(2, 1200, std::nullopt, std::nullopt, std::nullopt, s.t).error == Refusal::Busy);
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Disarmed; }, 30.0));
    CHECK(s.plant.resting());
    CHECK(s.rig.message() == "Parked on the dock.");
    CHECK(s.rig.homed());
    // And it can go again, and home again from the dock.
    REQUIRE(s.arm());
    REQUIRE(s.rig.home(s.t));
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Idle && s.rig.homed(); }, 60.0));
}

TEST_CASE("homing away from the dock is refused") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.fly_to({0, 0, 1300}, 2.0));
    CHECK(s.rig.home(s.t).error == Refusal::Busy);
}

TEST_CASE("disarming stops first, then brakes, then turns the drives off") {
    Sim s;
    REQUIRE(s.arm_and_home());
    REQUIRE(s.rig.move(2, 1500, std::nullopt, 3.0, std::nullopt, s.t));
    s.run(1.0);
    REQUIRE(s.rig.disarm(s.t));
    CHECK(s.rig.state() == RigState::Stopping);
    CHECK(s.plant.brakes_released());
    REQUIRE(s.until([&] { return !s.plant.brakes_released(); }, 3.0));
    CHECK(s.plant.enabled());
    REQUIRE(s.until([&] { return s.rig.state() == RigState::Disarmed; }, 1.0));
    CHECK_FALSE(s.plant.enabled());
    CHECK_FALSE(s.plant.resting());  // hanging on the brakes, mid-air
}

TEST_CASE("two-axis mode has no y axis") {
    RigConfig c = gazebo_config();
    c.two_axis = true;
    Sim s(c);
    REQUIRE(s.arm_and_home());
    CHECK_FALSE(s.rig.axis_present(1));
    CHECK(s.rig.move(1, 100, std::nullopt, std::nullopt, std::nullopt, s.t).error == Refusal::NoAxis);
    REQUIRE(s.fly_to({300, 0, 1300}, 2.0));
    CHECK(dist(s.plant.ball(), {300, 0, 1300}) < 0.5);
}

TEST_CASE("the simulated ball leans just as the controller expects") {
    Sim s;
    REQUIRE(s.arm_and_home());
    for (const Vec3& p : {Vec3{500, 0, 1230}, Vec3{-450, 350, 1100}, Vec3{300, -600, 1400}}) {
        REQUIRE(s.fly_to(p, 2.5));
        s.run(0.5);
        CHECK((s.plant.rotation() - s.rig.lean()).norm() < 1e-4);  // radians
        CHECK(s.plant.rotation().norm() > 0.02);                   // it really does lean
        CHECK(dist(s.plant.ball(), p) < 0.2);
    }
}

TEST_CASE("without the lean model, the ball lands off target") {
    RigConfig flat = gazebo_config();
    flat.lean_model = false;
    Sim s(flat, gazebo_config());  // the real ball still leans
    REQUIRE(s.arm_and_home());
    double worst = 0.0;
    for (const Vec3& p : {Vec3{500, 0, 1230}, Vec3{-450, 350, 1100}, Vec3{-600, -600, 1000}}) {
        REQUIRE(s.fly_to(p, 2.5));
        s.run(0.5);
        worst = std::max(worst, dist(s.plant.ball(), p));
    }
    MESSAGE("worst miss without the lean model: " << worst << " mm");
    CHECK(worst > 3.0);
}

TEST_CASE("faults can be cleared without arming, once their cause is gone") {
    Sim s;
    REQUIRE(s.arm_and_home());
    s.plant.set_estop(true);
    s.run(0.1);
    CHECK(s.rig.clear_faults(s.t).error == Refusal::EStop);
    s.plant.set_estop(false);
    s.run(0.1);
    REQUIRE(s.rig.state() == RigState::Fault);
    REQUIRE(s.rig.clear_faults(s.t));
    CHECK(s.rig.state() == RigState::Disarmed);
    CHECK(s.rig.faults() == 0);
    CHECK_FALSE(s.rig.armed());
}
