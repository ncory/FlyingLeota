// The simulated rig on the controller: the PC simulator's plant, for the bench. The
// whole controller (motion, screen, SCMP, Setup) runs with no motors, and Show Controller
// can fly it. The board's own E-stop input, enable, pump and brake outputs still work if
// they are wired, so a logic analyzer or a real E-stop button can be tried.
#include <cstdio>

#include "drives.hpp"
#include "leota/plant.hpp"
#include "sdkconfig.h"

namespace fw {

int line_sign(int line) { return (CONFIG_LEOTA_DIR_INVERT_MASK >> line) & 1 ? -1 : 1; }

#ifdef CONFIG_LEOTA_DRIVES_SIM

namespace {

class SimDrives : public Drives {
public:
    SimDrives(const leota::RigConfig& cfg, BoardIo& io) : plant_(cfg), io_(io) { plant_.fit_dock(true); }

    // A real dock charger if one is wired; else the simulated rig's own.
    bool dock_fitted() override { return true; }
    void set_dock_power(bool on) override {
        if (dock_ && dock_->fitted()) dock_->set_power(on);
        else plant_.set_dock_power(on);
    }
    double dock_watts() override { return dock_ && dock_->fitted() ? dock_->watts() : plant_.dock_watts(); }

    const char* kind() const override { return "simulated rig"; }

    void set_enabled(bool on) override {
        io_.set_drives_enabled(on);
        plant_.set_enabled(on);
    }
    void set_brakes_released(bool on) override {
        io_.set_brakes_released(on);
        plant_.set_brakes_released(on);
    }
    void command(const std::array<double, leota::kLines>& p) override {
        plant_.command(p);
        last_ = p;
    }
    std::array<leota::LineFeedback, leota::kLines> feedback() override {
        // Solving where the ball hangs is the slow part: on the P4 do it every 10 ms and
        // report the commanded positions in between, as the plant itself would.
        if (++calls_ % 10 == 1) fb_ = plant_.feedback();
        auto out = fb_;
        if (plant_.enabled() && plant_.brakes_released())
            for (int i = 0; i < leota::kLines; ++i) out[i].position = last_[i];
        return out;
    }
    bool estop_open() override {
        const bool open = io_.estop_fitted() && io_.estop_open();
        if (open != plant_.estop_open()) plant_.set_estop(open);
        return open;
    }

    std::string describe() override {
        char buf[200];
        const leota::Vec3 b = plant_.ball();
        std::snprintf(buf, sizeof buf, "Simulated rig. Ball at %.1f, %.1f, %.1f mm%s.\n", b.x, b.y, b.z,
                      plant_.resting() ? " (on the dock)" : "");
        return buf;
    }

private:
    leota::Plant plant_;
    BoardIo& io_;
    std::array<double, leota::kLines> last_{};
    std::array<leota::LineFeedback, leota::kLines> fb_{};
    unsigned calls_ = 0;
};

}  // namespace

std::unique_ptr<Drives> make_drives(const leota::RigConfig& cfg, BoardIo& io, LoadCells&) {
    return std::make_unique<SimDrives>(cfg, io);
}

#endif

}  // namespace fw
