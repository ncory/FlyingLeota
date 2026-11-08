// The touchscreen against the simulated rig: LVGL renders in memory, and taps go in
// through LVGL's own pointer input, as a finger on the panel would.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "leota/ui.hpp"
#include "sim.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

using namespace leota;

namespace {

uint32_t g_ms = 0;
struct Touch { int x = 0, y = 0; bool down = false; } g_touch;

uint32_t tick() { return g_ms; }

void read_touch(lv_indev_t*, lv_indev_data_t* data) {
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
    data->state = g_touch.down ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

void flush(lv_display_t* d, const lv_area_t*, uint8_t*) { lv_display_flush_ready(d); }

// One rig, one display, one UI. LVGL is set up once for every test.
struct Panel {
    test::Sim sim;
    lv_display_t* disp = nullptr;
    std::unique_ptr<Ui> ui;
    std::vector<uint8_t> buf = std::vector<uint8_t>(1024 * 60 * 2);

    explicit Panel(const RigConfig& cfg = gazebo_config(), const RigConfig* truth = nullptr,
                   SettingsKeeper* keeper = nullptr)
        : sim(cfg, truth ? *truth : cfg) {
        static bool ready = false;
        if (!ready) {
            lv_init();
            lv_tick_set_cb(tick);
            lv_indev_t* in = lv_indev_create();
            lv_indev_set_type(in, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(in, read_touch);
            ready = true;
        }
        disp = lv_display_create(1024, 600);
        lv_display_set_default(disp);
        lv_display_set_buffers(disp, buf.data(), nullptr, buf.size(), LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(disp, flush);
        lv_indev_set_display(lv_indev_get_next(nullptr), disp);
        ui = std::make_unique<Ui>(sim.rig, disp, keeper);
        step(0.1);
    }
    ~Panel() {
        ui.reset();
        lv_display_delete(disp);
    }
    // Runs the rig, the UI and LVGL together for `seconds`.
    void step(double seconds) {
        for (double end = sim.t + seconds; sim.t < end;) {
            sim.run(0.01);
            g_ms = static_cast<uint32_t>(sim.t * 1000);
            ui->update(sim.t, UiLink{}, "21:47");
            lv_timer_handler();
        }
    }
    bool until(const std::function<bool()>& done, double limit) {
        for (double end = sim.t + limit; sim.t < end; step(0.05))
            if (done()) return true;
        return done();
    }
    lv_obj_t* at(const char* name) {
        lv_obj_t* o = ui->find(name);
        REQUIRE_MESSAGE(o != nullptr, name);
        return o;
    }
    void press(const char* name) {
        lv_obj_update_layout(lv_display_get_screen_active(disp));
        lv_obj_scroll_to_view_recursive(at(name), LV_ANIM_OFF);
        lv_obj_update_layout(lv_display_get_screen_active(disp));
        lv_area_t a;
        lv_obj_get_coords(at(name), &a);
        g_touch = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2, true};
        step(0.08);
    }
    void release() {
        g_touch.down = false;
        step(0.08);
    }
    void tap(const char* name) {
        press(name);
        release();
    }
    bool visible(const char* name) { return !lv_obj_has_flag(at(name), LV_OBJ_FLAG_HIDDEN); }
    bool enabled(const char* name) { return !lv_obj_has_state(at(name), LV_STATE_DISABLED); }
    // Shown on screen: neither it nor anything it sits in is hidden.
    bool shown(const char* name) {
        for (lv_obj_t* o = at(name); o; o = lv_obj_get_parent(o))
            if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
        return true;
    }
    // A label's text; for a button or row, its labels' texts joined with " | ".
    std::string label(const char* name) {
        lv_obj_t* o = at(name);
        if (lv_obj_check_type(o, &lv_label_class)) return lv_label_get_text(o);
        std::string out;
        for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
            lv_obj_t* c = lv_obj_get_child(o, static_cast<int32_t>(i));
            if (!lv_obj_check_type(c, &lv_label_class)) continue;
            out += (out.empty() ? "" : " | ") + std::string(lv_label_get_text(c));
        }
        return out;
    }
    void unlock(const char* pin = "1234") {
        tap("tab.setup");
        for (const char* k = pin; *k; ++k) tap((std::string("setup.key.") + *k).c_str());
    }
    // Types on the number pad and taps Set.
    void type(const std::string& keys) {
        for (char k : keys) {
            const std::string id = k == '.' ? "dot" : k == '-' ? "minus" : std::string(1, k);
            tap(("edit.key." + id).c_str());
        }
        tap("edit.set");
    }
};

}  // namespace

TEST_CASE("it starts on the Status screen, and the tabs switch screens") {
    Panel p;
    CHECK(p.ui->page() == "status");
    CHECK(p.visible("page.status"));
    for (const char* tab : {"controls", "jog", "weather", "diagnostics", "setup", "log", "status"}) {
        p.tap((std::string("tab.") + tab).c_str());
        CHECK(p.ui->page() == tab);
        CHECK(p.visible((std::string("page.") + tab).c_str()));
    }
}

TEST_CASE("arming asks for confirmation, and won't arm until the area is checked") {
    Panel p;
    p.tap("tab.controls");
    p.tap("controls.arm");
    REQUIRE(p.visible("dialog.arm"));
    CHECK_FALSE(p.enabled("dialog.arm"));
    p.tap("dialog.arm");  // greyed out: nothing happens
    p.step(0.3);
    CHECK_FALSE(p.sim.rig.armed());
    p.tap("dialog.check");
    CHECK(p.enabled("dialog.arm"));
    p.tap("dialog.arm");
    CHECK(p.until([&] { return p.sim.rig.state() == RigState::Idle; }, 3.0));
    CHECK(p.sim.rig.armed());

    p.tap("controls.arm");  // now it says DISARM, with no dialog
    CHECK(p.until([&] { return p.sim.rig.state() == RigState::Disarmed; }, 3.0));
}

TEST_CASE("homing from the screen, then STOP stops a move") {
    Panel p;
    p.tap("tab.controls");
    p.tap("controls.arm");
    p.tap("dialog.check");
    p.tap("dialog.arm");
    REQUIRE(p.until([&] { return p.sim.rig.state() == RigState::Idle; }, 3.0));
    REQUIRE(p.enabled("controls.home"));
    p.tap("controls.home");
    REQUIRE(p.until([&] { return p.sim.rig.homed() && p.sim.rig.state() == RigState::Idle; }, 60.0));
    REQUIRE(p.sim.rig.move(2, 1500, std::nullopt, 4.0, std::nullopt, p.sim.t));
    p.step(1.0);
    p.tap("stop");
    CHECK(p.until([&] { return p.sim.rig.state() == RigState::Idle; }, 2.0));
    CHECK(p.sim.rig.position().z < 1400);
}

TEST_CASE("jog needs remote control off, and moves only while held") {
    Panel p;
    REQUIRE(p.sim.rig.arm(p.sim.t));
    p.step(0.5);
    REQUIRE(p.sim.rig.home(p.sim.t));
    REQUIRE(p.until([&] { return p.sim.rig.homed() && p.sim.rig.state() == RigState::Idle; }, 60.0));
    const double z0 = p.sim.rig.position().z;

    p.tap("tab.jog");
    CHECK_FALSE(p.enabled("jog.up"));  // remote is on
    p.tap("tab.controls");
    p.tap("controls.remote");
    REQUIRE_FALSE(p.sim.rig.remote());
    p.tap("tab.jog");
    REQUIRE(p.enabled("jog.up"));
    p.press("jog.up");
    p.step(1.0);
    const double held = p.sim.rig.position().z;
    CHECK(held > z0 + 50);
    p.release();
    p.step(1.0);
    CHECK(p.sim.rig.position().z < held + 40);  // stopped soon after letting go
    CHECK_FALSE(p.sim.rig.busy());
}

TEST_CASE("Setup opens with the PIN, and not without it") {
    Panel p;
    p.tap("tab.setup");
    for (const char* k : {"setup.key.9", "setup.key.9", "setup.key.9", "setup.key.9"}) p.tap(k);
    CHECK_FALSE(p.shown("setup.lock"));
    for (const char* k : {"setup.key.1", "setup.key.2", "setup.key.3", "setup.key.4"}) p.tap(k);
    CHECK(p.shown("setup.lock"));
    p.tap("setup.lock");
    CHECK_FALSE(p.shown("setup.lock"));
}

TEST_CASE("a setting typed on the number pad goes to the rig when saved, and into the log") {
    Panel p;
    p.unlock();
    p.tap("setup.group.2");  // Wind
    CHECK_FALSE(p.enabled("setup.save"));
    p.tap("setup.field.wind.red");
    REQUIRE(p.shown("editor"));
    CHECK(p.label("edit.title") == "Red at gust");
    CHECK(p.label("edit.value") == "11.0 m/s");
    p.type("12.5");
    CHECK_FALSE(p.shown("editor"));
    CHECK(p.label("setup.field.wind.red").find("12.5 m/s") != std::string::npos);
    CHECK(p.sim.rig.config().wind.red == 11.0);  // not until saved
    CHECK(p.label("setup.status") == "1 change not saved.");
    REQUIRE(p.enabled("setup.save"));
    p.tap("setup.save");
    CHECK(p.sim.rig.config().wind.red == 12.5);
    CHECK(p.label("setup.status") == "Saved until the controller restarts.");
    CHECK_FALSE(p.enabled("setup.save"));
}

TEST_CASE("the number pad refuses a value out of range, and Cancel leaves it alone") {
    Panel p;
    p.unlock();
    p.tap("setup.group.2");
    p.tap("setup.field.wind.red");
    p.type("99");
    CHECK(p.shown("editor"));
    CHECK(p.label("edit.note").find("must be from 1.0 to 40.0 m/s") != std::string::npos);
    p.tap("edit.cancel");
    CHECK_FALSE(p.shown("editor"));
    CHECK_FALSE(p.enabled("setup.save"));
}

TEST_CASE("a field far down the list scrolls into reach; negative numbers can be typed") {
    Panel p;
    p.unlock();
    p.tap("setup.field.smoothing");  // Flight is open first; smoothing is last
    CHECK(p.label("edit.title") == "Smoothing");
    p.tap("edit.cancel");
    p.tap("setup.field.workspace.x_min");
    p.type("-600");
    CHECK(p.label("setup.field.workspace.x_min").find("\xE2\x88\x92" "600 mm") != std::string::npos);
    p.tap("setup.save");
    CHECK(p.sim.rig.config().axes[0].min == -600);
}

TEST_CASE("toggles flip at a tap, two-axis mode hides the depth edges, and Undo puts it all back") {
    Panel p;
    p.unlock();
    REQUIRE(p.ui->find("setup.field.workspace.y_min") != nullptr);
    p.tap("setup.field.flight.two_axis");
    CHECK(p.label("setup.field.flight.two_axis").find("On") != std::string::npos);
    CHECK(p.ui->find("setup.field.workspace.y_min") == nullptr);
    p.tap("setup.group.1");
    p.tap("setup.field.remote_arm");
    CHECK(p.label("setup.status") == "2 changes not saved.");
    p.tap("setup.undo");
    CHECK_FALSE(p.enabled("setup.save"));
    p.tap("setup.group.0");
    CHECK(p.ui->find("setup.field.workspace.y_min") != nullptr);
}

TEST_CASE("saving the rig's settings waits until it's disarmed") {
    Panel p;
    p.unlock();
    REQUIRE(p.sim.arm());
    p.tap("setup.group.2");
    p.tap("setup.field.wind.red");
    p.type("13");
    CHECK(p.label("setup.status") == "1 change not saved. Disarm to save.");
    p.tap("setup.save");
    CHECK(p.sim.rig.config().wind.red == 11.0);
    REQUIRE(p.sim.rig.disarm(p.sim.t));
    REQUIRE(p.until([&] { return p.sim.rig.state() == RigState::Disarmed; }, 3.0));
    p.tap("setup.save");
    CHECK(p.sim.rig.config().wind.red == 13.0);
}

TEST_CASE("a new PIN is entered twice and works once saved") {
    SettingsKeeper keeper(settings_for(gazebo_config()));
    Panel p(gazebo_config(), nullptr, &keeper);
    p.unlock();
    p.tap("setup.group.5");
    p.tap("setup.field.pin");
    p.type("5678");
    CHECK(p.label("edit.title") == "New PIN again");
    p.type("5679");
    CHECK(p.label("edit.note").find("didn't match") != std::string::npos);
    p.type("5678");
    p.type("5678");
    CHECK_FALSE(p.shown("editor"));
    p.tap("setup.save");
    CHECK(keeper.settings().pin == "5678");
    p.tap("setup.lock");
    p.unlock("1234");
    CHECK_FALSE(p.shown("setup.lock"));
    p.unlock("5678");
    CHECK(p.shown("setup.lock"));
}

TEST_CASE("the calibration check runs from Setup, and what it finds can be used") {
    const RigConfig c = gazebo_config();
    RigConfig truth = c;
    truth.lines[1].anchor += Vec3{0, 0, -50};  // pulley B is really lower than measured
    SettingsKeeper keeper(settings_for(c));
    Panel p(c, &truth, &keeper);
    p.unlock();
    p.tap("setup.group.3");
    p.tap("setup.calibrate");
    REQUIRE(p.shown("cal.dialog"));
    CHECK(p.label("cal.ready") == "Arm the rig first.");
    CHECK_FALSE(p.enabled("cal.start"));
    p.tap("cal.close");
    REQUIRE(p.sim.arm_and_home());
    p.sim.rig.set_remote(false, p.sim.t);
    p.tap("setup.calibrate");
    CHECK(p.label("cal.ready") == "Ready to start.");
    p.tap("cal.start");
    CHECK_FALSE(p.enabled("cal.close"));
    REQUIRE(p.until([&] { return p.enabled("cal.use"); }, 200.0));
    CHECK(p.label("cal.summary").find("B ") != std::string::npos);
    CHECK_FALSE(p.sim.rig.armed());  // parked and disarmed
    p.tap("cal.use");
    CHECK_FALSE(p.shown("cal.dialog"));
    p.tap("setup.save");
    CHECK(std::fabs(p.sim.rig.config().lines[1].anchor.z - truth.lines[1].anchor.z) < 15.0);
    CHECK_FALSE(p.sim.rig.homed());  // new measurements: home again
}

TEST_CASE("an E-stop takes over the screen") {
    Panel p;
    CHECK_FALSE(p.visible("page.estop"));
    p.sim.plant.set_estop(true);
    p.step(0.2);
    CHECK(p.visible("page.estop"));
    p.sim.plant.set_estop(false);
    p.step(0.2);
    CHECK_FALSE(p.visible("page.estop"));
    p.tap("tab.controls");
    REQUIRE(p.enabled("controls.clear"));
    p.tap("controls.clear");
    CHECK(p.sim.rig.state() == RigState::Disarmed);
}
