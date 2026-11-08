// The Show Controller Motion Protocol, version 1 (BEDShowController docs/scmp.md),
// including its Optional fields: remote/local, wind, lines, coupled axes and message.
// Transport-free: the firmware and the PC simulator feed it datagrams or lines and send
// back what it returns.
#pragma once

#include <array>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>

#include "leota/rig.hpp"

namespace leota {

constexpr int kScmpVersion = 1;

class ScmpServer {
public:
    explicit ScmpServer(Rig& rig, std::string firmware = "flying-leota 0.1.0");

    // One message in. Returns the acknowledgement to send back, or "" for none
    // (a heartbeat). Any message counts as hearing from Show Controller.
    std::string handle(std::string_view message, double now);

    // The status message, as JSON.
    std::string status() const;
    // Whether a status is due: 10 a second while anything moves, at least once a
    // second otherwise, and at once when the state, faults, remote or wind level change.
    bool status_due(double now);
    void status_soon() { force_ = true; }  // e.g. after a command that changes things
    // The type ("t") of the last message handled, so a transport can tell where to send
    // status: to whoever last sent a hello or a heartbeat.
    const std::string& last_type() const { return last_type_; }

    // Called after a `config` with `set` changes the rig, so the host can keep the new
    // values (SCMP says calibration lives in the controller's flash).
    std::function<void()> on_config_set;

private:
    struct Answer { bool used = false; long long seq = 0; std::string ack; };
    std::string signature() const;

    Rig& rig_;
    std::string firmware_;
    std::array<Answer, 16> answered_{};  // the last 16 acks, to answer repeats again
    int next_ = 0;
    double last_status_ = -1e9;
    std::string last_sig_;
    bool force_ = false;
    std::string last_type_;
};

}  // namespace leota
