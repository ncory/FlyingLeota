// What the firmware's tasks share. The motion task runs the rig every millisecond; the
// SCMP, screen, console and sensor tasks reach it only while holding the rig lock, and
// never for long. Lock order, where both are needed: the display lock, then the rig lock.
#pragma once

#include <deque>
#include <string>
#include <utility>

#include "leota/rig.hpp"
#include "leota/scmp.hpp"
#include "leota/settings.hpp"
#include "leota/ui.hpp"

namespace fw {

class Drives;

double now_s();  // seconds since boot, the rig's clock

void rig_lock();
void rig_unlock();
struct RigLock {
    RigLock() { rig_lock(); }
    ~RigLock() { rig_unlock(); }
    RigLock(const RigLock&) = delete;
    RigLock& operator=(const RigLock&) = delete;
};

// The link to Show Controller, for the screen and console. Under the rig lock.
struct Link {
    std::string from;          // who sent the last hello or heartbeat
    double last_heard = -1.0;  // now_s() of the last message, -1 if never
    long commands = 0;
    std::string own_address;   // "192.168.1.50", or "" until Ethernet has an address
    bool cable = false;        // Ethernet link up
};

struct App {
    leota::SettingsKeeper* keeper = nullptr;
    leota::Rig* rig = nullptr;
    leota::ScmpServer* scmp = nullptr;
    Drives* drives = nullptr;
    Link link;
    // Lines for the screen's Log, from tasks that can't reach the UI. Under the rig lock;
    // the screen task takes them.
    std::deque<std::pair<leota::LogKind, std::string>> logs;
    void log(leota::LogKind kind, std::string text) {
        logs.emplace_back(kind, std::move(text));
        while (logs.size() > 40) logs.pop_front();
    }
};

App& app();

}  // namespace fw
