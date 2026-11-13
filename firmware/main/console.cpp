// The bench console, on the board's USB serial port (idf.py monitor): the Phase 1 tests
// (docs/plan.md, section 8) and a way in when there's no screen.
//
//   status                 the rig, lines, wind and link
//   timing [reset]         the motion task's timing
//   inputs                 E-stop, key, alarms, load cells, anemometer
//   drives                 what the drives report
//   rig arm|disarm|home|park|stop|clear|remote on|off
//   steps LINE MM SPEED    step/dir only, on the bench (rig disarmed, never homed, nothing
//                          hanging): move one line out and back, brakes released
//   settings [reset]       the stored settings as JSON; reset clears them (at next start)
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "anemometer.hpp"
#include "app.hpp"
#include "board_io.hpp"
#include "dock_io.hpp"
#include "drives.hpp"
#include "esp_console.h"
#include "esp_log.h"
#include "load_cells.hpp"
#include "motion.hpp"
#include "nvs_store.hpp"
#include "sdkconfig.h"

namespace fw {

extern BoardIo g_io;
extern LoadCells g_cells;
extern DockIo g_dock;
extern NvsStore g_store;

namespace {

const char* state_name(leota::RigState s) {
    switch (s) {
        case leota::RigState::Disarmed: return "DISARMED";
        case leota::RigState::Arming: return "ARMING";
        case leota::RigState::Idle: return "ARMED";
        case leota::RigState::Homing: return "HOMING";
        case leota::RigState::Moving: return "MOVING";
        case leota::RigState::Stopping: return "STOPPING";
        case leota::RigState::Fault: return "FAULT";
        case leota::RigState::EStop: return "E-STOP";
    }
    return "?";
}

int cmd_status(int, char**) {
    RigLock lock;
    App& a = app();
    const leota::Rig& r = *a.rig;
    const leota::Vec3 p = r.position();
    std::printf("%s%s, %s. %s\n", state_name(r.state()), r.homed() ? ", homed" : "", r.remote() ? "remote" : "local",
                r.message().c_str());
    std::printf("Ball (commanded) %.1f, %.1f, %.1f mm\n", p.x, p.y, p.z);
    for (const leota::LineStatus& l : r.lines())
        std::printf("  %c: %6.1f N (expects %6.1f), %8.1f mm out%s%s\n", l.name, l.tension, l.expected, l.out,
                    l.ready ? "" : ", not ready", l.alarm ? ", ALARM" : "");
    const leota::WindMonitor& w = r.wind();
    std::printf("Wind %.1f m/s, gust %.1f, level %s%s\n", w.speed(), w.gust(), leota::wind_level_name(w.level()),
                r.config().wind.enabled ? "" : " (no sensor fitted, per Setup)");
    const double heard = a.link.last_heard < 0 ? -1 : now_s() - a.link.last_heard;
    std::printf("Network %s, port %d, cable %s. Show Controller %s, heard %.1f s ago, %ld commands\n",
                a.link.own_address.empty() ? "(no address)" : a.link.own_address.c_str(),
                a.keeper->settings().net.scmp_port, a.link.cable ? "in" : "out",
                a.link.from.empty() ? "(none)" : a.link.from.c_str(), heard, a.link.commands);
    return 0;
}

int cmd_timing(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "reset") == 0) {
        motion_reset_stats();
        std::printf("Reset.\n");
        return 0;
    }
    const MotionStats s = motion_stats();
    std::printf("%llu cycles. Run: mean %.1f us, longest %lu us; %lu over 1 ms.\n",
                static_cast<unsigned long long>(s.cycles), s.mean_run_us, static_cast<unsigned long>(s.max_run_us),
                static_cast<unsigned long>(s.over_1ms));
    std::printf("Latest wake-up %lu us after the timer; longest wait for the rig lock %lu us.\n",
                static_cast<unsigned long>(s.max_late_us), static_cast<unsigned long>(s.max_wait_us));
    return 0;
}

int cmd_inputs(int, char**) {
    std::printf("E-stop chain: %s%s\n", g_io.estop_open() ? "OPEN" : "closed",
                g_io.estop_fitted() ? "" : " (no input set)");
    std::printf("Key: %s%s\n", g_io.key_remote() ? "Remote" : "Local", g_io.key_fitted() ? "" : " (no key set)");
    std::printf("Outputs: drives %s, brakes %s\n", g_io.drives_enabled() ? "enabled" : "off",
                g_io.brakes_released() ? "released" : "gripping");
    for (int i = 0; i < leota::kLines; ++i)
        std::printf("Line %c: alarm %s; load cell %s, %ld counts = %.2f N\n", 'A' + i,
                    g_io.alarm_fitted(i) ? (g_io.alarm(i) ? "ON" : "off") : "(none)",
                    g_cells.fitted(i) ? "fitted" : "(none)", static_cast<long>(g_cells.raw(i)), g_cells.newtons(i));
    if (g_cells.fitted())
        std::printf("Load cells: %lu readings, last %lu ms ago\n", static_cast<unsigned long>(g_cells.readings()),
                    static_cast<unsigned long>(g_cells.stale_ms()));
    if (g_dock.fitted())
        std::printf("Dock charger: %s, drawing %.1f W%s\n", g_dock.power() ? "on" : "off", g_dock.watts(),
                    g_dock.reading() ? "" : " (its power monitor isn't answering)");
    else
        std::printf("Dock charger: none set%s\n", app().drives && std::string(app().drives->kind()) == "simulated rig"
                                                       ? " (the simulated rig has its own)"
                                                       : "");
    double age = -1;
    const double w = anemometer_last(&age);
    if (w < 0) std::printf("Anemometer: no reading%s\n", anemometer_fitted() ? "" : " (none set)");
    else std::printf("Anemometer: %.1f m/s, %.1f s ago%s\n", w, age, anemometer_fitted() ? "" : " (simulated calm)");
    return 0;
}

int cmd_drives(int, char**) {
    RigLock lock;
    std::printf("%s", app().drives->describe().c_str());
    return 0;
}

int cmd_rig(int argc, char** argv) {
    if (argc < 2) {
        std::printf("rig arm|disarm|home|park|stop|clear|remote on|off\n");
        return 1;
    }
    const std::string what = argv[1];
    RigLock lock;
    leota::Rig& r = *app().rig;
    const double t = now_s();
    leota::Result res;
    if (what == "arm") res = r.arm(t);
    else if (what == "disarm") res = r.disarm(t);
    else if (what == "home") res = r.home(t);
    else if (what == "park") res = r.park(t);
    else if (what == "stop") res = r.stop(std::nullopt, t);
    else if (what == "clear") res = r.clear_faults(t);
    else if (what == "remote" && argc > 2) r.set_remote(std::strcmp(argv[2], "on") == 0, t);
    else {
        std::printf("Unknown: %s\n", what.c_str());
        return 1;
    }
    if (!res) std::printf("Refused: %s\n", res.detail.c_str());
    else app().log(leota::LogKind::Safety, "Console: rig " + what + ".");
    return res ? 0 : 1;
}

// A trapezoid on one line, straight to the drive: for checking step pulses on a logic
// analyzer while the screen and network are busy.
int cmd_steps(int argc, char** argv) {
    if (argc < 4) {
        std::printf("steps LINE MM SPEED: e.g. steps A 200 100 moves line A 200 mm out at 100 mm/s, then back.\n");
        return 1;
    }
    const int line = std::toupper(static_cast<unsigned char>(argv[1][0])) - 'A';
    const double dist = std::atof(argv[2]), speed = std::atof(argv[3]);
    if (line < 0 || line >= leota::kLines || !(speed > 0) || speed > 1000) {
        std::printf("Line A to D, and a speed up to 1000 mm/s.\n");
        return 1;
    }
    Drives* d = app().drives;
    if (!d->can_bench()) {
        std::printf("Only step/dir drives have the step test.\n");
        return 1;
    }
    double start = 0;
    {
        RigLock lock;
        if (app().rig->armed()) {
            std::printf("Disarm first.\n");
            return 1;
        }
        // The test releases the brakes and moves one line alone: never with the ball on it.
        if (app().rig->homed()) {
            std::printf("The rig is homed, so the lines may be on the ball. The step test is for the bench,\n"
                        "with nothing hanging from the line. Restart the controller to clear homing.\n");
            return 1;
        }
        start = d->feedback()[line].position;
    }
    struct Run {
        double t = 0, start, dist, speed, accel = 500;
        int line;
        Drives* d;
        BoardIo* io;
        bool started = false;
    };
    auto run = std::make_shared<Run>();
    run->start = start;
    run->dist = dist;
    run->speed = speed;
    run->line = line;
    run->d = d;
    run->io = &g_io;
    const bool ok = motion_bench([run](double dt) {
        if (!run->started) {
            run->io->set_drives_enabled(true);
            run->io->set_brakes_released(true);
            run->started = true;
        }
        run->t += dt;
        // Out and back: two trapezoids.
        const double a = run->accel, v = run->speed, D = std::fabs(run->dist);
        const double ta = std::min(v / a, std::sqrt(D / a));
        const double vp = a * ta, tc = (D - a * ta * ta) / vp, leg = 2 * ta + std::max(0.0, tc);
        auto pos = [&](double t) {
            if (t <= 0) return 0.0;
            if (t >= leg) return D;
            if (t < ta) return 0.5 * a * t * t;
            if (t < ta + tc) return 0.5 * a * ta * ta + vp * (t - ta);
            const double r = leg - t;
            return D - 0.5 * a * r * r;
        };
        const double t = run->t;
        const double s = t < leg + 0.5 ? pos(t) : D - pos(t - leg - 0.5);
        run->d->bench_command(run->line, run->start + (run->dist < 0 ? -s : s));
        if (t >= 2 * leg + 1.0) {
            run->io->set_brakes_released(false);
            run->io->set_drives_enabled(false);
            return false;
        }
        return true;
    });
    if (!ok) {
        std::printf("Couldn't start: the rig is armed, or a test is running.\n");
        return 1;
    }
    std::printf("Moving line %c %.0f mm at %.0f mm/s, then back. `timing` and a logic analyzer tell how it went.\n",
                'A' + line, dist, speed);
    return 0;
}

int cmd_settings(int argc, char** argv) {
    if (argc > 1 && std::strcmp(argv[1], "reset") == 0) {
        std::printf(g_store.erase() ? "Cleared: the standard settings come back at the next start.\n"
                                    : "Couldn't clear them.\n");
        return 0;
    }
    std::string json;
    {
        RigLock lock;
        json = leota::settings_to_json(app().keeper->settings());
    }
    std::printf("%s\n", json.c_str());
    return 0;
}

void add(const char* name, const char* help, esp_console_cmd_func_t fn) {
    esp_console_cmd_t c = {};
    c.command = name;
    c.help = help;
    c.func = fn;
    ESP_ERROR_CHECK(esp_console_cmd_register(&c));
}

}  // namespace

void console_start() {
    esp_console_repl_t* repl = nullptr;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "leota> ";
    esp_console_dev_uart_config_t uc = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uc, &rc, &repl));
    ESP_ERROR_CHECK(esp_console_register_help_command());
    add("status", "The rig, its lines, the wind and the network", cmd_status);
    add("timing", "The motion task's timing. `timing reset` starts the figures again", cmd_timing);
    add("inputs", "E-stop, key switch, drive alarms, load cells and anemometer", cmd_inputs);
    add("drives", "What the drives report", cmd_drives);
    add("rig", "rig arm|disarm|home|park|stop|clear|remote on|off", cmd_rig);
    add("steps", "steps LINE MM SPEED: step/dir test on one line, rig disarmed", cmd_steps);
    add("settings", "The stored settings as JSON. `settings reset` clears them", cmd_settings);
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

}  // namespace fw
