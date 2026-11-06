// What the controller needs from the winches, brakes and E-stop chain. The firmware
// implements it for the real drives (step/dir closed-loop steppers, or ODrives over
// CAN); plant.hpp implements it as a simulation for tests and the PC simulator.
//
// Positions are in mm of line, counted by each drive from wherever it started. More
// line paid out is a larger number. Homing works out how they relate to real lengths.
#pragma once

#include <array>

#include "leota/config.hpp"

namespace leota {

struct LineFeedback {
    double position = 0.0;  // mm of line paid out, in the drive's own count
    double tension = 0.0;   // N, from the line's load cell
    bool ready = false;     // drive enabled and holding position
    bool alarm = false;     // the drive reports a fault (following error, overheating...)
};

class Hardware {
public:
    virtual ~Hardware() = default;
    virtual void set_enabled(bool on) = 0;           // drive power and enable
    virtual void set_brakes_released(bool on) = 0;  // fail-safe brakes: false = gripping
    virtual void command(const std::array<double, kLines>& positions) = 0;
    virtual std::array<LineFeedback, kLines> feedback() = 0;
    virtual bool estop_open() = 0;                   // the hardware chain, read back
};

}  // namespace leota
