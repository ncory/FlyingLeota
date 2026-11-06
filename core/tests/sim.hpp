// A controller wired to a simulated rig, stepped in fixed time steps.
#pragma once

#include <functional>

#include "leota/plant.hpp"
#include "leota/rig.hpp"

namespace test {

using namespace leota;

struct Sim {
    RigConfig cfg;
    Plant plant;
    Rig rig;
    double t = 0.0;
    double wind = 2.0;       // m/s fed to the controller twice a second
    bool wind_feed = true;
    double next_wind = 0.0;

    explicit Sim(const RigConfig& c = gazebo_config()) : Sim(c, c) {}
    Sim(const RigConfig& c, const RigConfig& truth) : cfg(c), plant(truth), rig(c, plant) {
        rig.update(t);
        feed();
    }

    void feed() {
        if (wind_feed && t >= next_wind) {
            rig.wind_sample(wind, t);
            next_wind = t + 0.5;
        }
    }

    // Runs for `seconds`, `step` at a time (the controller runs every 1 ms inside).
    void run(double seconds, double step = 0.01) {
        const int n = static_cast<int>(seconds / step + 0.5);
        for (int k = 0; k < n; ++k) {
            t += step;
            feed();
            rig.update(t);
        }
    }

    // Runs until `done` is true, or `limit` seconds pass. Returns whether it came true.
    bool until(const std::function<bool()>& done, double limit = 30.0, double step = 0.01) {
        const double end = t + limit;
        while (t < end) {
            if (done()) return true;
            run(step, step);
        }
        return done();
    }

    bool arm() {
        if (!rig.arm(t)) return false;
        return until([&] { return rig.state() == RigState::Idle; }, 3.0);
    }

    bool arm_and_home() {
        if (!arm()) return false;
        if (!rig.home(t)) return false;
        return until([&] { return rig.state() == RigState::Idle && rig.homed(); }, 60.0);
    }

    // Moves every axis to `p` together, over `seconds`.
    bool fly_to(const Vec3& p, double seconds) {
        for (int a = 0; a < kAxes; ++a) {
            if (!rig.axis_present(a)) continue;
            const double v = a == 0 ? p.x : a == 1 ? p.y : p.z;
            if (!rig.move(a, v, std::nullopt, seconds, std::nullopt, t)) return false;
        }
        return until([&] { return !rig.busy(); }, seconds + 5.0);
    }
};

}  // namespace test
