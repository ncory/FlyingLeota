// Wind levels (docs/plan.md, section 6). A gust is the strongest 3-second average in
// the last 10 minutes. Green, Amber and Red follow the gust, the lines' tension swing
// while the ball is still, and the sensor's health. Once Red, it stays Red until it has
// been Green for the hold time; any gust at Amber or above restarts the count.
#pragma once

#include <deque>
#include <utility>

#include "leota/config.hpp"

namespace leota {

enum class WindLevel { Green = 0, Amber = 1, Red = 2 };
const char* wind_level_name(WindLevel l);  // "green", "amber", "red"

class WindMonitor {
public:
    explicit WindMonitor(const WindConfig& cfg) : cfg_(cfg) {}
    // New thresholds; what has been measured so far is kept.
    void configure(const WindConfig& cfg) { cfg_ = cfg; }

    void sample(double now, double speed);  // m/s, from the anemometer
    // The largest line-tension swing (N) while the ball is still; pass a negative number
    // when the ball is moving or on the dock, so the swing doesn't count.
    void update(double now, double still_swing);

    WindLevel level() const { return level_; }
    bool sensor_ok() const { return sensor_ok_; }
    bool ever_read() const { return have_sample_; }
    double speed() const { return speed_; }
    double gust() const { return gust_; }
    double average() const { return average_; }
    // Seconds until a Red hold ends (the whole hold if the count hasn't started); 0
    // when not held.
    double calm_left() const { return calm_left_; }
    bool holding() const { return level_ == WindLevel::Red && hold_; }

private:
    WindConfig cfg_;
    std::deque<std::pair<double, double>> recent_;  // (time, speed), for averages
    std::deque<std::pair<double, double>> gusts_;   // (second, strongest 3 s average)
    bool have_sample_ = false, sensor_ok_ = false, hold_ = false;
    double last_sample_ = 0.0, speed_ = 0.0, gust_ = 0.0, average_ = 0.0;
    double calm_since_ = -1.0, calm_left_ = 0.0, last_swing_ = -1.0;
    WindLevel level_ = WindLevel::Red;  // no sensor yet
};

}  // namespace leota
