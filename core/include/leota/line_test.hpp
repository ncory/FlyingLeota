// The line test, from Setup: measures one line's load cell and drum, with that line
// unhooked from the ball.
//
//   1. Empty: nothing on the line. The load cell's reading is its zero.
//   2. Weight: a known weight hangs from the line, clear of the floor and still. Its
//      reading above the zero gives the load cell's scale.
//   3. Raise: the winch reels in `move_mm` of line on its own (armed, remote off),
//      lifting the weight. (Lowering could put it on the floor: hanging from a corner
//      pulley, a line long enough to reach the ball nearly reaches the deck.)
//   4. Rise: a person measures how far the weight went up. Against what the winch
//      counted, allowing for the line's stretch, that gives the drum's correction.
//
// result() then holds the line's new LineSensors, for Setup to save. The rig needs homing
// again afterwards, with the line back on the ball.
#pragma once

#include <string>

#include "leota/rig.hpp"

namespace leota {

class LineTest {
public:
    enum class Step { Off, Empty, Weight, Raise, Rise, Done };

    Result start(const Rig& rig, int line);
    Result measure(const Rig& rig, double now);  // in Empty or Weight: averages for `average_time`
    void set_weight(double kg) { weight_kg_ = kg; }
    Result raise(Rig& rig, double now);          // in Raise
    Result rise(double measured_mm);             // in Rise
    void finish();                               // keep what's measured so far, skip the rest
    void update(Rig& rig, double now);           // call every 50 ms or so
    void cancel(Rig& rig, double now);

    Step step() const { return step_; }
    bool busy() const { return measuring_ || raising_; }
    int line() const { return line_; }
    double weight_kg() const { return weight_kg_; }
    // The load cell's reading now, before any correction (N, as it reads).
    double reading(const Rig& rig) const;
    const LineSensors& before() const { return before_; }
    const LineSensors& result() const { return result_; }
    const std::string& message() const { return message_; }

    double average_time = 2.0;  // s
    double move_mm = 500.0;     // how far the winch raises the weight

private:
    Step step_ = Step::Off;
    int line_ = 0;
    double weight_kg_ = 5.0;
    LineSensors before_, result_;
    bool measuring_ = false, raising_ = false;
    double t0_ = 0.0, sum_ = 0.0, hang_tension_ = 0.0, line_ea_ = 1.0, start_count_ = 0.0;
    int count_ = 0;
    std::string message_;
};

}  // namespace leota
