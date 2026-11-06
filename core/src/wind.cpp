#include "leota/wind.hpp"

#include <algorithm>
#include <cmath>

namespace leota {

const char* wind_level_name(WindLevel l) {
    switch (l) {
        case WindLevel::Green: return "green";
        case WindLevel::Amber: return "amber";
        case WindLevel::Red: return "red";
    }
    return "red";
}

void WindMonitor::sample(double now, double speed) {
    have_sample_ = true;
    last_sample_ = now;
    speed_ = speed;
    recent_.emplace_back(now, speed);
    const double keep = std::max(cfg_.average_window, cfg_.gust_seconds);
    while (!recent_.empty() && recent_.front().first < now - keep) recent_.pop_front();

    double sum = 0.0;
    int n = 0;
    for (auto it = recent_.rbegin(); it != recent_.rend() && it->first > now - cfg_.gust_seconds; ++it) {
        sum += it->second;
        ++n;
    }
    const double avg3 = n ? sum / n : speed;
    const double second = std::floor(now);
    if (!gusts_.empty() && gusts_.back().first == second)
        gusts_.back().second = std::max(gusts_.back().second, avg3);
    else
        gusts_.emplace_back(second, avg3);
    update(now, last_swing_);  // a new reading counts at once
}

void WindMonitor::update(double now, double still_swing) {
    last_swing_ = still_swing;
    while (!gusts_.empty() && gusts_.front().first < now - cfg_.gust_window) gusts_.pop_front();
    gust_ = 0.0;
    for (const auto& g : gusts_) gust_ = std::max(gust_, g.second);
    double sum = 0.0;
    int n = 0;
    for (const auto& r : recent_)
        if (r.first >= now - cfg_.average_window) { sum += r.second; ++n; }
    average_ = n ? sum / n : 0.0;

    sensor_ok_ = have_sample_ && now - last_sample_ <= cfg_.sensor_timeout;
    WindLevel raw = WindLevel::Green;
    if (!sensor_ok_ || gust_ >= cfg_.red || still_swing >= cfg_.swing_red)
        raw = WindLevel::Red;
    else if (gust_ >= cfg_.amber || still_swing >= cfg_.swing_amber)
        raw = WindLevel::Amber;

    if (raw == WindLevel::Red) {
        // Waiting for the first reading after start-up is no reason to hold.
        if (have_sample_) hold_ = true;
        level_ = WindLevel::Red;
        calm_since_ = -1.0;
    } else if (level_ == WindLevel::Red && hold_) {
        if (raw != WindLevel::Green) {
            calm_since_ = -1.0;  // the count restarts
        } else {
            if (calm_since_ < 0) calm_since_ = now;
            if (now - calm_since_ >= cfg_.hold) {
                hold_ = false;
                level_ = WindLevel::Green;
            }
        }
    } else {
        hold_ = false;
        level_ = raw;
    }
    if (level_ == WindLevel::Red && hold_)
        calm_left_ = calm_since_ < 0 ? cfg_.hold : std::max(0.0, cfg_.hold - (now - calm_since_));
    else
        calm_left_ = 0.0;
}

}  // namespace leota
