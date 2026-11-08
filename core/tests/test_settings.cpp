#include <cmath>

#include "ArduinoJson.h"
#include "doctest.h"
#include "leota/scmp.hpp"
#include "leota/settings.hpp"
#include "sim.hpp"

using namespace leota;
using test::Sim;

namespace {

struct MemoryStore : SettingsStore {
    std::string json;
    bool has = false, fail = false;
    int writes = 0;
    bool read(std::string& out) override {
        out = json;
        return has;
    }
    bool write(const std::string& in) override {
        if (fail) return false;
        json = in;
        has = true;
        ++writes;
        return true;
    }
};

Settings defaults() { return settings_for(gazebo_config()); }

void set(Settings& s, const char* key, double v) {
    const Field* f = find_field(key);
    REQUIRE_MESSAGE(f != nullptr, key);
    f->set(s, v);
}
double get(const Settings& s, const char* key) { return find_field(key)->get(s); }

}  // namespace

TEST_CASE("the standard settings pass their own checks, and every field is in range") {
    const Settings s = defaults();
    CHECK(check_settings(s).empty());
    for (const Field& f : settings_fields()) {
        std::string why;
        CHECK_MESSAGE(field_allows(f, f.get(s), &why), f.key, " ", why);
        CHECK(find_field(f.key) == &f);  // keys are unique
    }
    CHECK(field_text(*find_field("wind.red"), s) == "11.0 m/s");
    CHECK(field_text(*find_field("wind.amber_speed"), s) == "50%");
    CHECK(field_text(*find_field("workspace.x_min"), s) == "\xE2\x88\x92" "677 mm");
    CHECK(field_text(*find_field("remote_arm"), s) == "On");
    CHECK(field_text(*find_field("net.address"), s) == "192.168.1.50");
}

TEST_CASE("settings go to JSON and back unchanged") {
    Settings s = defaults();
    set(s, "wind.red", 12.5);
    set(s, "wind.hold", 20);
    set(s, "pulley.B.z", 1990);
    set(s, "dock.height", 800);
    set(s, "net.dhcp", 0);
    set(s, "net.address", 0x0A000005);
    set(s, "remote_arm", 0);
    s.pin = "246810";
    const std::string json = settings_to_json(s);
    Settings back = defaults();
    CHECK(settings_from_json(json, back).empty());
    CHECK(settings_changes(s, back).empty());
    CHECK(back.pin == "246810");
    CHECK(back.rig.wind.hold == doctest::Approx(1200));
    CHECK(back.rig.dock.z == doctest::Approx(800 + back.rig.ball_radius));
    CHECK(back.rig.table.hi.z == doctest::Approx(750));  // the table moved up with the dock
    CHECK(back.rig.axes[2].min == std::floor(back.rig.dock.z));
    CHECK_FALSE(back.rig.remote_arm);
}

TEST_CASE("stored settings that are wrong keep what was there, and say so") {
    Settings s = defaults();
    const std::string said = settings_from_json(
        R"({"version":1,"settings":{"wind.red":500,"wind.amber":"high","smoothing":0.2,"net.address":"10.0.0"},"pin":"12"})", s);
    CHECK(said.find("Red at gust") != std::string::npos);
    CHECK(said.find("Amber at gust") != std::string::npos);
    CHECK(said.find("Address") != std::string::npos);
    CHECK(said.find("PIN") != std::string::npos);
    CHECK(s.rig.wind.red == 11.0);
    CHECK(s.rig.smoothing == doctest::Approx(0.2));  // the good one is used
    CHECK(s.pin == "1234");
    Settings t = defaults();
    CHECK_FALSE(settings_from_json("not json", t).empty());
    CHECK(settings_changes(defaults(), t).empty());
}

TEST_CASE("checks catch settings that don't make sense together") {
    Settings s = defaults();
    set(s, "wind.amber", 12);
    CHECK(check_settings(s).find("Amber gust must be below") != std::string::npos);
    s = defaults();
    set(s, "workspace.x_min", 100);
    CHECK(check_settings(s).find("dock must be inside") != std::string::npos);
    s = defaults();
    set(s, "workspace.z_max", 950);
    CHECK(check_settings(s).find("100 mm above") != std::string::npos);
    s = defaults();
    set(s, "pulley.A.z", 1500);
    CHECK(check_settings(s).find("below the top of the workspace") != std::string::npos);
    s = defaults();
    s.pin = "12a4";
    CHECK(check_settings(s).find("PIN") != std::string::npos);
    s = defaults();
    set(s, "net.dhcp", 0);
    set(s, "net.netmask", 0xFF00FF00);
    CHECK(check_settings(s).find("netmask") != std::string::npos);
    set(s, "net.netmask", 0xFFFFFF00);
    set(s, "net.gateway", 0x0A000001);
    CHECK(check_settings(s).find("gateway") != std::string::npos);
    set(s, "net.dhcp", 1);
    CHECK(check_settings(s).empty());  // addresses don't matter with DHCP
}

TEST_CASE("changes are described in words") {
    Settings a = defaults(), b = a;
    set(b, "wind.red", 12);
    set(b, "flight.two_axis", 1);
    b.pin = "9999";
    const auto said = settings_changes(a, b);
    REQUIRE(said.size() == 3);
    CHECK(said[0] == "Two axes, depth fixed: Off to On.");
    CHECK(said[1] == "Red at gust: 11.0 m/s to 12.0 m/s.");
    CHECK(said[2] == "The Setup PIN changed.");
}

TEST_CASE("addresses parse only when they're whole") {
    std::uint32_t a = 0;
    CHECK(parse_address("192.168.1.20", a));
    CHECK(a == 0xC0A80114);
    CHECK(address_text(a) == "192.168.1.20");
    for (const char* bad : {"", "1.2.3", "1.2.3.4.5", "256.1.1.1", "1..2.3", "a.b.c.d", "1.2.3.4 ", "1234.1.1.1"})
        CHECK_MESSAGE(!parse_address(bad, a), bad);
}

TEST_CASE("the keeper loads, changes the rig and stores") {
    MemoryStore store;
    SettingsKeeper keeper(defaults(), &store);
    CHECK(keeper.load().empty());  // nothing stored yet: the standard settings
    Sim sim(keeper.settings().rig);

    Settings next = keeper.settings();
    set(next, "wind.red", 13);
    set(next, "speed.up", 200);
    REQUIRE(keeper.change(next, sim.rig));
    CHECK(sim.rig.config().wind.red == 13);
    CHECK(sim.rig.config().axes[2].max_velocity == 200);
    CHECK(store.writes == 1);

    SettingsKeeper again(defaults(), &store);
    CHECK(again.load().empty());
    CHECK(again.settings().rig.wind.red == 13);

    SUBCASE("not while armed") {
        REQUIRE(sim.arm());
        set(next, "wind.red", 14);
        const Result r = keeper.change(next, sim.rig);
        CHECK_FALSE(r);
        CHECK(r.detail.find("Disarm") != std::string::npos);
        CHECK(sim.rig.config().wind.red == 13);
        CHECK(keeper.settings().rig.wind.red == 13);
    }
    SUBCASE("the network and PIN change even while armed: they don't touch the rig") {
        REQUIRE(sim.arm());
        next.pin = "5555";
        set(next, "net.port", 7800);
        CHECK(keeper.change(next, sim.rig));
        CHECK(keeper.settings().net.scmp_port == 7800);
    }
    SUBCASE("settings that fail their checks are refused") {
        set(next, "wind.amber", 20);
        CHECK_FALSE(keeper.change(next, sim.rig));
        CHECK(store.writes == 1);
    }
    SUBCASE("a store that fails still changes the rig, with a warning") {
        store.fail = true;
        set(next, "wind.red", 15);
        const Result r = keeper.change(next, sim.rig);
        CHECK(r);
        CHECK_FALSE(r.detail.empty());
        CHECK(sim.rig.config().wind.red == 15);
        CHECK_FALSE(keeper.stored_ok());
    }
}

TEST_CASE("stored settings that fail their checks give the standard ones") {
    MemoryStore store;
    store.has = true;
    store.json = R"({"version":1,"settings":{"wind.amber":20,"wind.red":15}})";
    SettingsKeeper keeper(defaults(), &store);
    CHECK(keeper.load().find("standard settings") != std::string::npos);
    CHECK(keeper.settings().rig.wind.red == 11.0);
}

TEST_CASE("moving a pulley means homing again, and only with the ball on the dock") {
    SettingsKeeper keeper(defaults());
    Sim sim(keeper.settings().rig);
    REQUIRE(sim.arm_and_home());
    REQUIRE(sim.fly_to({0, 0, 1300}, 3.0));
    REQUIRE(sim.rig.disarm(sim.t));
    REQUIRE(sim.until([&] { return sim.rig.state() == RigState::Disarmed; }, 3.0));

    Settings next = keeper.settings();
    set(next, "pulley.A.x", get(next, "pulley.A.x") + 20);
    Result r = keeper.change(next, sim.rig);
    CHECK_FALSE(r);
    CHECK(r.detail.find("Park") != std::string::npos);

    // A setting that doesn't move anything is fine in mid-air.
    Settings calm = keeper.settings();
    set(calm, "wind.hold", 10);
    CHECK(keeper.change(calm, sim.rig));
    CHECK(sim.rig.homed());

    REQUIRE(sim.arm());
    REQUIRE(sim.rig.park(sim.t));
    REQUIRE(sim.until([&] { return sim.rig.state() == RigState::Disarmed; }, 30.0));
    set(next, "wind.hold", 10);
    REQUIRE(keeper.change(next, sim.rig));
    CHECK_FALSE(sim.rig.homed());
    CHECK(sim.rig.message().find("Home") != std::string::npos);
    REQUIRE(sim.arm_and_home());  // and it homes and flies with the new measurement
    CHECK(sim.fly_to({100, 100, 1200}, 3.0));
}

TEST_CASE("with arming from Show Controller turned off, it can still disarm and stop") {
    RigConfig c = gazebo_config();
    c.remote_arm = false;
    Sim sim(c);
    ScmpServer scmp(sim.rig);
    JsonDocument d;
    deserializeJson(d, scmp.handle(R"({"t":"arm","seq":1})", sim.t));
    CHECK_FALSE(d["ok"].as<bool>());
    CHECK(d["error"] == "local");
    REQUIRE(sim.arm());  // at the screen
    deserializeJson(d, scmp.handle(R"({"t":"arm","seq":2})", sim.t));
    CHECK(d["ok"].as<bool>());  // already armed: nothing to refuse
    deserializeJson(d, scmp.handle(R"({"t":"disarm","seq":3})", sim.t));
    CHECK(d["ok"].as<bool>());
}

TEST_CASE("limits set over SCMP are kept with the other settings") {
    MemoryStore store;
    SettingsKeeper keeper(defaults(), &store);
    keeper.load();
    Sim sim(keeper.settings().rig);
    ScmpServer scmp(sim.rig);
    scmp.on_config_set = [&] { keeper.adopt(sim.rig.config()); };
    JsonDocument d;
    deserializeJson(d, scmp.handle(R"({"t":"config","seq":1,"axis":"z","set":{"max_velocity":222}})", sim.t));
    REQUIRE(d["ok"].as<bool>());
    CHECK(store.writes == 1);
    SettingsKeeper again(defaults(), &store);
    again.load();
    CHECK(again.settings().rig.axes[2].max_velocity == 222);
    deserializeJson(d, scmp.handle(R"({"t":"config","seq":2,"axis":"z","get":["max"]})", sim.t));
    CHECK(store.writes == 1);  // reading doesn't store
}
