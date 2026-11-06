// The touchscreen against the simulated rig: LVGL renders in memory, and taps go in
// through LVGL's own pointer input, as a finger on the panel would.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
#include "leota/ui.hpp"
#include "sim.hpp"

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

    explicit Panel(const RigConfig& cfg = gazebo_config()) : sim(cfg) {
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
        ui = std::make_unique<Ui>(sim.rig, disp);
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
    CHECK(p.ui->find("setup.lock") == nullptr);
    for (const char* k : {"setup.key.1", "setup.key.2", "setup.key.3", "setup.key.4"}) p.tap(k);
    REQUIRE(p.ui->find("setup.lock") != nullptr);
    CHECK(p.visible("setup.lock"));
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
