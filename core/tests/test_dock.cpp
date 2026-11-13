#include "ArduinoJson.h"
#include "doctest.h"
#include "leota/dock.hpp"
#include "leota/scmp.hpp"
#include "sim.hpp"

using namespace leota;
using test::Sim;

namespace {

// Short timings, so the tests don't wait half a minute for each check of the pad.
RigConfig quick_dock() {
    RigConfig c = gazebo_config();
    c.charger.settle = 0.5;
    c.charger.probe_every = 2.0;
    c.charger.probe_time = 0.5;
    c.charger.retry = 3.0;
    return c;
}

JsonDocument status(Sim& sim) {
    ScmpServer scmp(sim.rig);
    JsonDocument d;
    REQUIRE_FALSE(deserializeJson(d, scmp.status()));
    return d;
}

// A dock whose switch has stuck on: it draws whatever it's told.
struct StuckDock : Hardware {
    double w = 0.0;
    bool on = false;
    void set_enabled(bool) override {}
    void set_brakes_released(bool) override {}
    void command(const std::array<double, kLines>&) override {}
    std::array<LineFeedback, kLines> feedback() override { return {}; }
    bool estop_open() override { return false; }
    bool dock_fitted() override { return true; }
    void set_dock_power(bool o) override { on = o; }
    double dock_watts() override { return w; }
};

}  // namespace

TEST_CASE("a ball resting on the dock charges, armed or not, until it lifts off") {
    Sim sim(quick_dock());
    sim.plant.fit_dock(true);
    sim.run(1.0);
    CHECK(sim.plant.dock_on());
    CHECK(sim.rig.dock().state() == DockState::Charging);
    CHECK(sim.rig.dock().watts() == doctest::Approx(20.0));
    sim.plant.set_ball_full(true);
    sim.run(0.2);
    CHECK(sim.rig.dock().state() == DockState::Charged);
    sim.plant.set_ball_full(false);
    REQUIRE(sim.arm());
    sim.run(0.1);
    CHECK(sim.plant.dock_on());  // between shows, armed on the dock: still charging
    CHECK(sim.rig.dock().state() == DockState::Charging);
    REQUIRE(sim.rig.home(sim.t));
    REQUIRE(sim.until([&] { return sim.rig.homed() && sim.rig.state() == RigState::Idle; }, 60.0));
    CHECK(sim.rig.dock().state() == DockState::Empty);  // lifted 30 mm off it after homing
    CHECK_FALSE(sim.plant.dock_on());
    // Set down at dock height, still armed: charging again.
    REQUIRE(sim.fly_to(sim.cfg.dock, 2.0));
    sim.run(1.0);
    CHECK(sim.rig.armed());
    CHECK(sim.rig.dock().state() == DockState::Charging);
}

TEST_CASE("with charging while armed turned off, arming turns the charger off") {
    RigConfig c = quick_dock();
    c.charger.while_armed = false;
    Sim sim(c);
    sim.plant.fit_dock(true);
    sim.run(1.0);
    CHECK(sim.rig.dock().state() == DockState::Charging);
    REQUIRE(sim.arm());
    sim.run(0.1);
    CHECK_FALSE(sim.plant.dock_on());
    CHECK(sim.rig.dock().state() == DockState::Off);
}

TEST_CASE("while the ball flies the dock is empty, and something left on its pad is found") {
    Sim sim(quick_dock());
    sim.plant.fit_dock(true);
    REQUIRE(sim.arm_and_home());
    REQUIRE(sim.fly_to({0, 0, 1300}, 3.0));
    CHECK(sim.rig.dock().state() == DockState::Empty);
    int checks = 0;
    bool was_on = false;
    for (int i = 0; i < 500; ++i) {  // 5 s: two or three checks of the pad
        sim.run(0.01);
        if (sim.plant.dock_on() && !was_on) ++checks;
        was_on = sim.plant.dock_on();
    }
    CHECK(checks >= 2);
    CHECK(sim.rig.dock().state() == DockState::Empty);

    sim.plant.set_metal_on_pad(true);
    REQUIRE(sim.until([&] { return sim.rig.dock().state() == DockState::Fault; }, 4.0));
    CHECK(sim.rig.message().find("Something is on the dock's charging pad") != std::string::npos);
    sim.run(0.1);
    CHECK_FALSE(sim.plant.dock_on());

    sim.plant.set_metal_on_pad(false);
    REQUIRE(sim.until([&] { return sim.rig.dock().state() == DockState::Empty; }, 4.0));
    CHECK(sim.rig.message() == "The dock's charging pad is clear again.");
}

TEST_CASE("drawing too much turns the charger off, and it tries again later") {
    Sim sim(quick_dock());
    sim.plant.fit_dock(true);
    sim.run(1.0);
    REQUIRE(sim.rig.dock().state() == DockState::Charging);
    sim.plant.set_metal_on_pad(true);  // 50 W with the ball on it: too much
    sim.run(0.1);
    CHECK(sim.rig.dock().state() == DockState::Fault);
    CHECK_FALSE(sim.plant.dock_on());
    CHECK(sim.rig.message().find("drew 50 W") != std::string::npos);
    sim.run(1.0);
    CHECK_FALSE(sim.plant.dock_on());  // waits before trying again
    sim.plant.set_metal_on_pad(false);
    REQUIRE(sim.until([&] { return sim.rig.dock().state() == DockState::Charging; }, 5.0));
    CHECK(sim.rig.message() == "The dock's charger is charging normally again.");
}

TEST_CASE("a switch that won't turn off is a fault until it does") {
    StuckDock hw;
    DockCharger dock;
    DockConfig cfg;
    double t = 0.0;
    auto run = [&](double s, bool seated, bool may) {
        for (double end = t + s; t < end; t += 0.01) dock.update(hw, cfg, seated, may, t);
    };
    run(3.0, true, false);  // seated, armed: should be off, drawing nothing
    CHECK(dock.state() == DockState::Off);
    hw.w = 12.0;  // but it draws power
    run(1.0, true, false);
    CHECK(dock.state() == DockState::Fault);
    CHECK(dock.take_news().find("check its switch") != std::string::npos);
    hw.w = 0.0;
    run(0.1, true, false);
    CHECK(dock.state() == DockState::Off);
    CHECK(dock.take_news() == "The dock's charger is off again.");
}

TEST_CASE("SCMP status carries the dock only when one is fitted") {
    Sim sim(quick_dock());
    sim.run(1.0);
    CHECK(status(sim)["dock"].isNull());
    sim.plant.fit_dock(true);
    sim.run(1.0);
    JsonDocument d = status(sim);
    CHECK(d["dock"]["state"] == "charging");
    CHECK(d["dock"]["watts"].as<double>() == doctest::Approx(20.0));
}
