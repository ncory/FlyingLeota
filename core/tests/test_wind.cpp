#include "doctest.h"
#include "leota/wind.hpp"

using namespace leota;

namespace {

// Feeds `speed` twice a second for `seconds`, updating as it goes.
void blow(WindMonitor& w, double& t, double speed, double seconds, double swing = -1.0) {
    for (double end = t + seconds; t < end; t += 0.5) {
        w.sample(t, speed);
        w.update(t, swing);
    }
}

WindConfig quick() {
    WindConfig c;
    c.hold = 60.0;
    return c;
}

}  // namespace

TEST_CASE("before the sensor answers it is Red, but with no hold") {
    WindMonitor w(quick());
    w.update(0.0, -1.0);
    CHECK(w.level() == WindLevel::Red);
    CHECK_FALSE(w.holding());
    double t = 0.0;
    blow(w, t, 2.0, 1.0);
    CHECK(w.level() == WindLevel::Green);
}

TEST_CASE("gusts set the level") {
    WindMonitor w(quick());
    double t = 0.0;
    blow(w, t, 4.0, 10.0);
    CHECK(w.level() == WindLevel::Green);
    CHECK(w.gust() == doctest::Approx(4.0));
    blow(w, t, 8.0, 5.0);
    CHECK(w.level() == WindLevel::Amber);
    blow(w, t, 2.0, 30.0);
    CHECK(w.level() == WindLevel::Amber);  // the 8 m/s gust is still in the last 10 minutes
    CHECK(w.average() < 8.0);
    blow(w, t, 2.0, 600.0);
    CHECK(w.level() == WindLevel::Green);
}

TEST_CASE("a short puff isn't a gust: it takes 3 seconds of it") {
    WindMonitor w(quick());
    double t = 0.0;
    blow(w, t, 2.0, 5.0);
    w.sample(t, 12.0);  // one reading at 12 m/s, among 2 m/s ones
    w.update(t, -1.0);
    t += 0.5;
    blow(w, t, 2.0, 5.0);
    CHECK(w.gust() < 7.0);
    CHECK(w.level() == WindLevel::Green);
}

TEST_CASE("Red holds until it has been Green long enough, and a gust restarts the count") {
    WindMonitor w(quick());
    double t = 0.0;
    blow(w, t, 12.0, 5.0);
    CHECK(w.level() == WindLevel::Red);
    CHECK(w.holding());
    // Calm, but the 12 m/s gust stays in the 10-minute window: still Red, no count yet.
    blow(w, t, 2.0, 300.0);
    CHECK(w.holding());
    CHECK(w.calm_left() == doctest::Approx(60.0));
    blow(w, t, 2.0, 330.0);  // the gust has left the window: counting
    CHECK(w.holding());
    CHECK(w.calm_left() < 60.0);
    blow(w, t, 8.0, 5.0);  // an Amber gust: back to the start
    blow(w, t, 2.0, 30.0);
    CHECK(w.holding());
    CHECK(w.calm_left() == doctest::Approx(60.0));
    blow(w, t, 2.0, 660.0);
    CHECK(w.level() == WindLevel::Green);
    CHECK_FALSE(w.holding());
}

TEST_CASE("a silent sensor counts as Red") {
    WindMonitor w(quick());
    double t = 0.0;
    blow(w, t, 2.0, 5.0);
    w.update(t + 10.0, -1.0);
    CHECK_FALSE(w.sensor_ok());
    CHECK(w.level() == WindLevel::Red);
    CHECK(w.holding());
}

TEST_CASE("line swing raises the level by itself") {
    WindMonitor w(quick());
    double t = 0.0;
    blow(w, t, 2.0, 5.0, 2.0);
    CHECK(w.level() == WindLevel::Amber);
    blow(w, t, 2.0, 5.0, 4.0);
    CHECK(w.level() == WindLevel::Red);
}
