#include <cmath>

#include "doctest.h"
#include "leota/profile.hpp"

using namespace leota;

namespace {

// Samples a trajectory; returns its top speed and checks it ends at rest on target.
double top_speed(const Trajectory& tr, double target) {
    double vmax = 0.0, p = 0.0, v = 0.0;
    for (double t = 0.0; t <= tr.duration(); t += 0.001) {
        tr.sample(t, p, v);
        vmax = std::max(vmax, std::fabs(v));
    }
    tr.sample(tr.duration(), p, v);
    CHECK(p == doctest::Approx(target));
    CHECK(v == 0.0);
    return vmax;
}

}  // namespace

TEST_CASE("a move from rest cruises at the speed limit and ends on target") {
    const Trajectory tr = Trajectory::to_target(0, 0, 600, 300, 500);
    // 0.6 s up to 300 mm/s (90 mm), 1.4 s cruising (420 mm), 0.6 s down (90 mm).
    CHECK(tr.duration() == doctest::Approx(2.6));
    CHECK(top_speed(tr, 600) == doctest::Approx(300).epsilon(0.001));
    double p, v;
    tr.sample(1.3, p, v);
    CHECK(p == doctest::Approx(300));
    CHECK(v == doctest::Approx(300));
}

TEST_CASE("a short move never reaches the speed limit") {
    const Trajectory tr = Trajectory::to_target(100, 0, 80, 300, 500);
    CHECK(tr.duration() == doctest::Approx(2 * std::sqrt(20.0 / 500)));  // 20 mm
    CHECK(top_speed(tr, 80) < 300);
}

TEST_CASE("already moving the right way, it carries on smoothly") {
    const Trajectory tr = Trajectory::to_target(0, 200, 500, 300, 500);
    double p, v;
    tr.sample(0.0, p, v);
    CHECK(v == doctest::Approx(200));
    CHECK(top_speed(tr, 500) == doctest::Approx(300).epsilon(0.001));
}

TEST_CASE("too fast to stop in time: it overshoots, then comes back") {
    const Trajectory tr = Trajectory::to_target(0, 300, 20, 300, 500);  // needs 90 mm to stop
    double p, v, furthest = 0.0;
    for (double t = 0; t < tr.duration(); t += 0.001) {
        tr.sample(t, p, v);
        furthest = std::max(furthest, p);
    }
    CHECK(furthest == doctest::Approx(90).epsilon(0.01));
    top_speed(tr, 20);
}

TEST_CASE("moving the wrong way, it stops and turns round") {
    const Trajectory tr = Trajectory::to_target(0, -100, 100, 300, 500);
    double p, v;
    tr.sample(0.2, p, v);
    CHECK(v == doctest::Approx(0).scale(1));
    CHECK(p == doctest::Approx(-10));
    top_speed(tr, 100);
}

TEST_CASE("a lower speed limit while moving brakes down to it") {
    const Trajectory tr = Trajectory::to_target(0, 300, 1000, 100, 500);
    double p, v;
    tr.sample(0.5, p, v);
    CHECK(v == doctest::Approx(100));
    top_speed(tr, 1000);
}

TEST_CASE("timed moves pick their speed the way Show Controller does") {
    double late = 1.0;
    const double v = timed_speed(100, 2.0, 500, 1000, &late);
    CHECK(late == 0.0);
    CHECK(v == doctest::Approx((2000 - std::sqrt(2000.0 * 2000 - 4 * 1000 * 100)) / 2));
    CHECK(Trajectory::to_target(0, 0, 100, v, 1000).duration() == doctest::Approx(2.0));

    const double fast = timed_speed(1000, 1.0, 500, 1000, &late);
    CHECK(fast == 500);
    CHECK(late == doctest::Approx(1.5));  // 2.5 s at best
}

TEST_CASE("the smoother keeps acceleration and limits jerk") {
    AxisMotion ax;
    const double dt = 0.001, smoothing = 0.15, amax = 500;
    ax.reset(0.0, static_cast<int>(smoothing / dt));
    ax.move(400, 300, amax);
    double prev_v = 0.0, prev_a = 0.0, max_a = 0.0, max_j = 0.0, t = 0.0;
    while (!ax.settled() && t < 10) {
        t += dt;
        ax.step(dt, t);
        const double v = ax.output_velocity(), a = (v - prev_v) / dt;
        max_a = std::max(max_a, std::fabs(a));
        if (t > 2 * dt) max_j = std::max(max_j, std::fabs(a - prev_a) / dt);
        prev_v = v;
        prev_a = a;
    }
    CHECK(ax.output() == doctest::Approx(400));
    CHECK(max_a <= amax * 1.01);
    CHECK(max_j <= amax / smoothing * 1.1);
    // Takes the plan's time plus one filter length.
    CHECK(t == doctest::Approx(Trajectory::to_target(0, 0, 400, 300, amax).duration() + smoothing).epsilon(0.01));
}

TEST_CASE("jog runs while repeated, stops after, and stays inside its limits") {
    AxisMotion ax;
    ax.reset(0.0, 10);
    double t = 0.0;
    for (int k = 0; k < 150; ++k) {  // 1.5 s of jog at 200 mm/s, repeated every 0.1 s
        if (k % 10 == 0) ax.jog(200, t + 0.25, 500, -100, 150);
        t += 0.01;
        ax.step(0.01, t);
    }
    CHECK(ax.planned() == doctest::Approx(150));  // stopped at the limit
    ax.jog(-200, t + 0.25, 500, -100, 150);
    for (int k = 0; k < 100; ++k) {
        t += 0.01;
        ax.step(0.01, t);
    }
    CHECK_FALSE(ax.busy());  // the hold ran out and it stopped by itself
    CHECK(ax.planned() < 150);
    CHECK(ax.planned() > -100);
}
