// The controller's touchscreen: seven screens on a 1024 × 600 display, in LVGL 9.
// It reads the rig's state and calls the rig's commands, just as SCMP does. The host
// (the firmware, or the PC simulator) makes the LVGL display, runs lv_timer_handler(),
// and calls update() about 20 times a second.
//
// The layout follows the mockup linked from docs/plan.md, section 5.6.
#pragma once

#include <memory>
#include <string>

#include "leota/rig.hpp"
#include "lvgl.h"

namespace leota {

// What the host knows about the link to Show Controller.
struct UiLink {
    bool connected = false;  // heard from recently
    std::string address;     // Show Controller's address, if known
    double heard = -1.0;     // seconds since the last message, -1 if never
    long commands = 0;       // commands received since starting
    std::string own_address; // this controller's address
    int scmp_port = 7720;
};

enum class LogKind { Safety, Motion, Wind, Network, Show };

class Ui {
public:
    // Builds every screen on `display`. `pin` unlocks Setup.
    Ui(Rig& rig, lv_display_t* display, std::string pin = "1234");
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // Refreshes the screens from the rig. `clock` is the time to show, such as "21:47".
    void update(double now, const UiLink& link, const std::string& clock);
    // Adds a line to the Log screen (the UI logs the rig's own messages itself).
    void log(LogKind kind, const std::string& text);

    // For tests and the simulator: a widget by name, such as "tab.controls" or
    // "controls.arm" (see ui.cpp), or nullptr.
    lv_obj_t* find(const std::string& name) const;
    std::string page() const;  // "status", "controls", ...

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

}  // namespace leota
