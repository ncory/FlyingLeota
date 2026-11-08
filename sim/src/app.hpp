// The simulator: the motion core driving a simulated rig, spoken to over SCMP like the
// real controller, with a web page that shows the rig in 3-D, works its touchscreen
// controls, and makes things go wrong on purpose.
#pragma once

#include <deque>
#include <memory>
#include <string>

#include "leota/plant.hpp"
#include "leota/rig.hpp"
#include "leota/scmp.hpp"
#include "leota/settings.hpp"
#include "net.hpp"
#ifdef LEOTA_SIM_UI
#include "leota/ui.hpp"
#include "screen.hpp"
#endif

namespace leota_sim {

struct Options {
    std::string host = "0.0.0.0";
    int scmp_port = 7720;
    int http_port = 8090;  // 0: no viewer
    bool two_axis = false;  // the starting setting, when none is stored
    std::string settings_file;  // keep Setup's settings here ("": until it stops)
    double real_mass = 0.0;     // kg the head really weighs (0: as set), to try the calibration check
    double wind = 2.0;     // m/s
    double gusty = 0.3;    // 0 to 1: how much it varies
    bool push = true;      // the wind pushes the ball
    double pulley_error = 0.0;  // mm: pulley A really is this far off (x and y)
};

class SimApp {
public:
    explicit SimApp(const Options& opt);

    // A datagram from Show Controller. Returns the reply ("" for none).
    std::string datagram(const std::string& msg, const Address& from, double now);
    // Runs the controller and the wind up to `now`. Returns a status to send, or "".
    std::string step(double now);
    const Address& status_to() const { return status_to_; }

    HttpResponse http(const HttpRequest& req, double now);

private:
    void build();
    std::string state_json(double now);
    std::string action(const std::string& body, double now);
    void log(double now, const std::string& what);
    double wind_now(double now) const;

    Options opt_;
    leota::RigConfig truth_;  // the rig as built; the controller believes its settings
    std::unique_ptr<leota::SettingsStore> store_;
    std::unique_ptr<leota::SettingsKeeper> keeper_;
    std::unique_ptr<leota::Plant> plant_;
    std::unique_ptr<leota::Rig> rig_;
    std::unique_ptr<leota::ScmpServer> scmp_;

    Address status_to_;
    double last_heard_ = -1.0, next_wind_ = 0.0, started_ = 0.0;
    long commands_ = 0;
    bool sensor_on_ = true;
    double snag_ = 0.0;       // N, a sideways pull on the ball
    std::string last_message_;
    std::deque<std::string> log_;
#ifdef LEOTA_SIM_UI
    std::unique_ptr<Screen> screen_;
    std::unique_ptr<leota::Ui> ui_;
    double next_ui_ = 0.0;
#endif
};

}  // namespace leota_sim
