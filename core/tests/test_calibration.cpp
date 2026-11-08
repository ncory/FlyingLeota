#include <cmath>

#include "doctest.h"
#include "leota/calibration.hpp"
#include "sim.hpp"

using namespace leota;
using test::Sim;

namespace {

// Runs the whole check with the screen's rhythm: an update every 50 ms.
bool run_check(Sim& sim, CalibrationRun& run) {
    if (!run.start(sim.rig, sim.t)) return false;
    for (double end = sim.t + 200.0; sim.t < end && run.running();) {
        sim.run(0.05);
        run.update(sim.rig, sim.t);
    }
    return run.phase() == CalibrationRun::Phase::Done;
}

bool ready(Sim& sim) {
    if (!sim.arm_and_home()) return false;
    sim.rig.set_remote(false, sim.t);
    return true;
}

// The worst landing error over the check's own points, flying with `cfg` on a rig built like `truth`.
double worst_landing(const RigConfig& cfg, const RigConfig& truth) {
    Sim sim(cfg, truth);
    REQUIRE(sim.arm_and_home());
    double worst = 0.0;
    for (const Vec3& p : calibration_points(cfg)) {
        REQUIRE(sim.fly_to(p, 4.0));
        sim.run(1.0);
        worst = std::max(worst, (sim.plant.ball() - p).norm());
    }
    return worst;
}

RigConfig with_fit(RigConfig c, const CalFit& fit) {
    for (int i = 0; i < kLines; ++i) c.lines[i].anchor = fit.anchors[i];
    c.mass = fit.mass;
    return c;
}

}  // namespace

TEST_CASE("the check's points sit well inside the workspace, in the plane for two axes") {
    RigConfig c = gazebo_config();
    const auto pts = calibration_points(c);
    CHECK(pts.size() == 9);
    for (const Vec3& p : pts) CHECK(check_point(c, p, workspace(c)) == Spot::Ok);
    c.two_axis = true;
    const auto flat = calibration_points(c);
    CHECK(flat.size() == 9);
    for (const Vec3& p : flat) CHECK(p.y == c.fixed_y);
}

TEST_CASE("it won't start unless the ball is homed, still and under the screen's control") {
    Sim sim;
    CalibrationRun run;
    CHECK(CalibrationRun::not_ready(sim.rig) == "Arm the rig first.");
    REQUIRE(sim.arm());
    CHECK(CalibrationRun::not_ready(sim.rig) == "Home at the dock first.");
    REQUIRE(sim.rig.home(sim.t));
    REQUIRE(sim.until([&] { return sim.rig.state() == RigState::Idle && sim.rig.homed(); }, 60.0));
    CHECK(CalibrationRun::not_ready(sim.rig).find("remote control off") != std::string::npos);
    CHECK_FALSE(run.start(sim.rig, sim.t));
    sim.rig.set_remote(false, sim.t);
    CHECK(CalibrationRun::not_ready(sim.rig).empty());
}

TEST_CASE("with good measurements, the check says so") {
    Sim sim;
    REQUIRE(ready(sim));
    CalibrationRun run;
    REQUIRE(run_check(sim, run));
    CHECK(run.measured() == 9);
    CHECK_FALSE(sim.rig.armed());  // parked on the dock, disarmed
    CHECK(sim.rig.on_dock());
    const CalFit& f = run.fit();
    CHECK(f.ok);
    for (int i = 0; i < kLines; ++i) CHECK(f.moved[i] < 2.0);
    CHECK(f.mass == doctest::Approx(4.0).epsilon(0.005));
    CHECK(f.summary.find("where they were measured") != std::string::npos);
    CHECK(f.summary.find("weight is right") != std::string::npos);
}

TEST_CASE("a pulley measured wrong is found, and using the fit puts the ball where it's sent") {
    const RigConfig c = gazebo_config();
    RigConfig truth = c;
    truth.lines[1].anchor += Vec3{0, 0, -50};   // B is really 50 mm lower
    truth.lines[2].anchor += Vec3{30, 20, 0};   // and C a little off
    Sim sim(c, truth);
    REQUIRE(ready(sim));
    CalibrationRun run;
    REQUIRE(run_check(sim, run));
    const CalFit& f = run.fit();
    CHECK(f.ok);
    CHECK((f.anchors[1] - truth.lines[1].anchor).norm() < 15.0);
    CHECK(f.moved[1] > 30.0);
    CHECK(f.summary.find("B ") != std::string::npos);
    const double before = worst_landing(c, truth), after = worst_landing(with_fit(c, f), truth);
    MESSAGE("worst landing before ", before, " mm, after ", after, " mm");
    CHECK(before > 5.0);
    CHECK(after < 2.0);
}

TEST_CASE("a head heavier than set is weighed") {
    RigConfig truth = gazebo_config();
    truth.mass = 4.8;
    Sim sim(gazebo_config(), truth);
    REQUIRE(ready(sim));
    CalibrationRun run;
    REQUIRE(run_check(sim, run));
    CHECK(run.fit().mass == doctest::Approx(4.8).epsilon(0.01));
    CHECK(run.fit().summary.find("4.80 kg, not 4.00 kg") != std::string::npos);
}

TEST_CASE("noisy load cells still give a useful answer") {
    RigConfig truth = gazebo_config();
    truth.lines[0].anchor += Vec3{60, -60, 0};
    Sim sim(gazebo_config(), truth);
    sim.plant.tension_noise(0.3);
    REQUIRE(ready(sim));
    CalibrationRun run;
    REQUIRE(run_check(sim, run));
    CHECK(run.fit().ok);
    CHECK((run.fit().anchors[0] - truth.lines[0].anchor).norm() < 15.0);
}

TEST_CASE("stopping, or remote control coming on, ends the check where the ball is") {
    Sim sim;
    REQUIRE(ready(sim));
    CalibrationRun run;
    REQUIRE(run.start(sim.rig, sim.t));
    sim.run(1.0);
    run.update(sim.rig, sim.t);
    SUBCASE("cancel") {
        run.cancel(sim.rig, sim.t);
        CHECK(run.phase() == CalibrationRun::Phase::Failed);
    }
    SUBCASE("STOP") {
        sim.rig.stop(std::nullopt, sim.t);
        sim.until([&] { return !sim.rig.busy(); }, 3.0);
        run.update(sim.rig, sim.t);
        CHECK(run.phase() == CalibrationRun::Phase::Failed);
    }
    SUBCASE("remote") {
        sim.rig.set_remote(true, sim.t);
        run.update(sim.rig, sim.t);
        CHECK(run.phase() == CalibrationRun::Phase::Failed);
        CHECK(run.message().find("Remote") != std::string::npos);
    }
    CHECK(sim.until([&] { return !sim.rig.busy(); }, 3.0));
    CHECK(sim.rig.armed());
}

TEST_CASE("the fit needs enough points") {
    const RigConfig c = gazebo_config();
    std::vector<CalSample> few(3);
    const CalFit f = fit_calibration(c, LineArray{8, 8, 8, 8}, few);
    CHECK_FALSE(f.ok);
}
