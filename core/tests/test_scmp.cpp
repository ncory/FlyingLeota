#include <string>

#include "ArduinoJson.h"
#include "doctest.h"
#include "leota/scmp.hpp"
#include "sim.hpp"

using namespace leota;
using test::Sim;

namespace {

struct Link {
    Sim sim;
    ScmpServer scmp{sim.rig};
    int seq = 0;

    explicit Link(const RigConfig& c = gazebo_config()) : sim(c) {}

    // Sends a command with the next seq; returns the parsed ack.
    JsonDocument send(std::string body) {
        body.insert(body.find('{') + 1, "\"seq\":" + std::to_string(++seq) + ",");
        return parse(scmp.handle(body, sim.t));
    }
    JsonDocument raw(const std::string& msg) { return parse(scmp.handle(msg, sim.t)); }
    static JsonDocument parse(const std::string& s) {
        JsonDocument d;
        REQUIRE_FALSE(deserializeJson(d, s));
        return d;
    }
    // Keeps Show Controller's heartbeat going while time passes.
    void run(double seconds) {
        for (double end = sim.t + seconds; sim.t < end;) {
            scmp.handle(R"({"t":"heartbeat"})", sim.t);
            sim.run(0.1);
        }
    }
    bool until(const std::function<bool()>& done, double limit) {
        for (double end = sim.t + limit; sim.t < end; run(0.1))
            if (done()) return true;
        return done();
    }
};

}  // namespace

TEST_CASE("hello describes the rig as three coupled axes") {
    Link l;
    JsonDocument ack = l.send(R"({"t":"hello","scmp":1})");
    CHECK(ack["t"] == "ack");
    CHECK(ack["seq"] == 1);
    CHECK(ack["ok"] == true);
    JsonObject info = ack["info"];
    CHECK(info["scmp"] == 1);
    CHECK(info["name"] == "Flying Leota");
    CHECK(info["coupled"] == true);
    CHECK(info["tension_min"].as<double>() == 5.0);
    CHECK(info["tension_max"].as<double>() == 150.0);
    CHECK(info["heartbeat_timeout"] == 0.5);
    REQUIRE(info["axes"].size() == 3);
    CHECK(info["axes"][2]["axis"] == "z");
    CHECK(info["axes"][2]["units"] == "mm");
    CHECK(info["axes"][2]["needs_homing"] == true);
    CHECK(info["axes"][0]["max"].as<double>() == l.sim.cfg.axes[0].max);

    ack = l.send(R"({"t":"hello","scmp":2})");
    CHECK(ack["ok"] == false);
    CHECK(ack["error"] == "version");
}

TEST_CASE("heartbeats get no answer; nonsense gets bad_message") {
    Link l;
    CHECK(l.scmp.handle(R"({"t":"heartbeat"})", 0.0).empty());
    JsonDocument ack = l.raw("not json");
    CHECK(ack["ok"] == false);
    CHECK(ack["error"] == "bad_message");
    CHECK(ack["seq"].isNull());
    ack = l.send(R"({"t":"dance"})");
    CHECK(ack["error"] == "bad_message");
}

TEST_CASE("a repeated seq gets the same answer and isn't acted on twice") {
    Link l;
    l.send(R"({"t":"hello","scmp":1})");
    const std::string arm = R"({"t":"arm","seq":2})";
    const std::string first = l.scmp.handle(arm, l.sim.t);
    l.run(0.5);
    REQUIRE(l.sim.rig.armed());
    l.scmp.handle(R"({"t":"disarm","seq":3})", l.sim.t);
    l.run(1.0);
    REQUIRE_FALSE(l.sim.rig.armed());
    CHECK(l.scmp.handle(arm, l.sim.t) == first);  // sent again: answered again...
    l.run(0.5);
    CHECK_FALSE(l.sim.rig.armed());  // ...but not armed again
}

TEST_CASE("Show Controller can arm, home and fly the ball") {
    Link l;
    l.send(R"({"t":"hello","scmp":1})");
    CHECK(l.send(R"({"t":"move","axis":"z","target":1200,"duration":2})")["error"] == "disarmed");
    CHECK(l.send(R"({"t":"arm","axis":"x"})")["ok"] == true);  // any axis arms them all
    REQUIRE(l.until([&] { return l.sim.rig.state() == RigState::Idle; }, 2.0));
    CHECK(l.send(R"({"t":"move","axis":"z","target":1200,"duration":2})")["error"] == "not_homed");
    CHECK(l.send(R"({"t":"home","axis":"z"})")["ok"] == true);
    REQUIRE(l.until([&] { return l.sim.rig.homed() && l.sim.rig.state() == RigState::Idle; }, 60.0));

    JsonDocument ack = l.send(R"({"t":"move","axis":"z","target":1400,"duration":3})");
    CHECK(ack["ok"] == true);
    CHECK(ack["late"].isNull());
    CHECK(l.send(R"({"t":"move","axis":"x","target":-200,"duration":3})")["ok"] == true);
    REQUIRE(l.until([&] { return !l.sim.rig.busy(); }, 6.0));
    CHECK((l.sim.plant.ball() - Vec3{-200, 0, 1400}).norm() < 0.5);

    ack = l.send(R"({"t":"move","axis":"z","target":1000,"duration":0.3})");
    CHECK(ack["late"].as<double>() > 0.5);
    ack = l.send(R"({"t":"move","axis":"x","target":5000,"velocity":100})");
    CHECK(ack["error"] == "out_of_range");
    CHECK(std::string(ack["detail"].as<const char*>()).rfind("x 5000 is outside", 0) == 0);
    CHECK(l.send(R"({"t":"move","axis":"w","target":0})")["error"] == "no_axis");
    CHECK(l.send(R"({"t":"move","axis":"z","target":true})")["error"] == "bad_message");
    CHECK(l.send(R"({"t":"move","axis":"z","target":1100,"velocity":-5})")["error"] == "bad_message");
    CHECK(l.send(R"({"t":"stop"})")["ok"] == true);
}

TEST_CASE("with remote control off, the network can watch and stop, nothing more") {
    Link l;
    l.sim.rig.set_remote(false, l.sim.t);
    JsonDocument ack = l.send(R"({"t":"arm"})");
    CHECK(ack["ok"] == false);
    CHECK(ack["error"] == "local");
    CHECK(l.send(R"({"t":"home","axis":"z"})")["error"] == "local");
    CHECK(l.send(R"({"t":"config","axis":"x","set":{"max":500}})")["error"] == "local");
    CHECK(l.send(R"({"t":"config","axis":"x","get":["max"]})")["ok"] == true);
    CHECK(l.send(R"({"t":"stop"})")["ok"] == true);
    CHECK(l.send(R"({"t":"disarm"})")["ok"] == true);
    CHECK(l.send(R"({"t":"status"})")["status"]["remote"] == false);
}

TEST_CASE("Red wind refuses arming with weather") {
    Link l;
    l.sim.wind = 13.0;
    l.run(4.0);
    JsonDocument ack = l.send(R"({"t":"arm"})");
    CHECK(ack["error"] == "weather");
    JsonDocument st = l.send(R"({"t":"status"})");
    CHECK(st["status"]["wind"]["level"] == "red");
    CHECK(st["status"]["wind"]["calm_left"].as<double>() > 0);
    CHECK(st["status"]["axes"][0]["faults"][0] == "wind");
}

TEST_CASE("status carries the optional fields") {
    Link l;
    l.run(0.5);
    JsonDocument st = l.Link::parse(l.scmp.status());
    CHECK(st["t"] == "status");
    CHECK(st["estop"] == false);
    CHECK(st["armed"] == false);
    CHECK(st["remote"] == true);
    CHECK(st["wind"]["level"] == "green");
    CHECK(st["wind"]["speed"] == 2.0);
    CHECK(st["wind"]["calm_left"].isNull());
    REQUIRE(st["lines"].size() == 4);
    CHECK(st["lines"][1]["line"] == "B");
    CHECK(st["message"].is<const char*>());
    REQUIRE(st["axes"].size() == 3);
    JsonObject z = st["axes"][2];
    CHECK(z["axis"] == "z");
    CHECK(z["state"] == "disarmed");
    CHECK(z["homed"] == false);
    CHECK(z["target"].isNull());
    CHECK(z["faults"].size() == 0);
    CHECK(z["limits"]["low"] == false);
}

TEST_CASE("config reads and sets the soft limits") {
    Link l;
    JsonDocument ack = l.send(R"({"t":"config","axis":"x"})");
    CHECK(ack["values"]["units"] == "mm");
    CHECK(ack["values"]["max"].as<double>() == l.sim.cfg.axes[0].max);
    ack = l.send(R"({"t":"config","axis":"x","set":{"max":500,"max_velocity":200}})");
    CHECK(ack["ok"] == true);
    CHECK(ack["values"]["max"] == 500);
    CHECK(l.sim.rig.limits(0).max == 500);
    CHECK(l.send(R"({"t":"config","axis":"x","set":{"units":"in"}})")["error"] == "read_only");
    CHECK(l.send(R"({"t":"config","axis":"x","set":{"min":900}})")["error"] == "bad_message");
    CHECK(l.send(R"({"t":"config","axis":"x","get":["colour"]})")["error"] == "bad_message");
}

TEST_CASE("two-axis mode leaves y out") {
    RigConfig c = gazebo_config();
    c.two_axis = true;
    Link l(c);
    JsonDocument ack = l.send(R"({"t":"hello","scmp":1})");
    CHECK(ack["info"]["axes"].size() == 2);
    CHECK(l.send(R"({"t":"jog","axis":"y","velocity":10})")["error"] == "no_axis");
}

TEST_CASE("status goes out at once on a change, 10 a second while moving, once a second otherwise") {
    Link l;
    CHECK(l.scmp.status_due(l.sim.t));
    CHECK_FALSE(l.scmp.status_due(l.sim.t + 0.5));
    CHECK(l.scmp.status_due(l.sim.t + 1.1));
    l.sim.rig.set_remote(false, l.sim.t);
    CHECK(l.scmp.status_due(l.sim.t + 1.2));  // remote changed
}
