// The motion task: a hardware timer wakes it every millisecond, on core 1, and it runs
// the rig (Rig::update), keeps the watchdog charge pump going and reads the key switch.
// It keeps figures on its own timing for the bench (console `timing`).
#pragma once

#include <cstdint>
#include <functional>

namespace fw {

class BoardIo;

struct MotionStats {
    std::uint64_t cycles = 0;
    std::uint32_t max_run_us = 0;      // the longest cycle, lock wait included
    std::uint32_t max_late_us = 0;     // the latest wake-up after the timer fired
    std::uint32_t max_wait_us = 0;     // the longest wait for the rig lock
    std::uint32_t over_1ms = 0;        // cycles that ran longer than a millisecond
    double mean_run_us = 0.0;
};

void motion_start(BoardIo& io);
MotionStats motion_stats();
void motion_reset_stats();

// The bench step test: `step(dt)` runs every cycle in place of the rig, until it returns
// false. Only while the rig is disarmed.
bool motion_bench(std::function<bool(double)> step);
bool motion_bench_running();

}  // namespace fw
