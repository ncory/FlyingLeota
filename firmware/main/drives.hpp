// The winches, as the motion core sees them (leota::Hardware), in three kinds chosen in
// menuconfig: the simulated rig (bench, no motors), closed-loop steppers on step/dir, or
// ODrives over CAN. Each also reports itself for the console.
#pragma once

#include <memory>
#include <string>

#include "board_io.hpp"
#include "leota/config.hpp"
#include "leota/hardware.hpp"
#include "load_cells.hpp"

namespace fw {

class Drives : public leota::Hardware {
public:
    virtual const char* kind() const = 0;
    virtual std::string describe() = 0;  // a few lines for the console's `drives`
    // The bench step test (console `steps`): moves one line directly while the rig is
    // disarmed. Only step/dir drives have it.
    virtual bool can_bench() const { return false; }
    virtual void bench_command(int /*line*/, double /*mm*/) {}
};

std::unique_ptr<Drives> make_drives(const leota::RigConfig& cfg, BoardIo& io, LoadCells& cells);

// Nominal line per mm, per line, with direction: +1, or -1 for a line in
// CONFIG_LEOTA_DIR_INVERT_MASK.
int line_sign(int line);

}  // namespace fw
