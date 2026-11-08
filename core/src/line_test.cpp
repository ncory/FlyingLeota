#include "leota/line_test.hpp"

#include <cmath>
#include <cstdio>

namespace leota {

namespace {

std::string fmt(const char* f, double a = 0, double b = 0) {
    char buf[200];
    std::snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

}  // namespace

Result LineTest::start(const Rig& rig, int line) {
    if (line < 0 || line >= kLines) return {Refusal::BadMessage, "There's no such line."};
    if (rig.estop()) return {Refusal::EStop, "The E-stop chain is open."};
    if (rig.busy()) return {Refusal::Busy, "Wait until everything is still."};
    line_ = line;
    before_ = result_ = rig.config().sensors[line];
    line_ea_ = rig.config().line_ea;
    measuring_ = raising_ = false;
    step_ = Step::Empty;
    message_ = "";
    return {};
}

double LineTest::reading(const Rig& rig) const { return rig.raw_feedback()[line_].tension; }

Result LineTest::measure(const Rig& rig, double now) {
    if (step_ != Step::Empty && step_ != Step::Weight) return {Refusal::Busy, "Not now."};
    if (measuring_) return {};
    if (step_ == Step::Weight && !(weight_kg_ > 0.0)) return {Refusal::BadMessage, "Enter the weight first."};
    (void)rig;
    measuring_ = true;
    t0_ = now;
    sum_ = 0.0;
    count_ = 0;
    message_ = "Measuring. Keep everything still.";
    return {};
}

Result LineTest::raise(Rig& rig, double now) {
    if (step_ != Step::Raise) return {Refusal::Busy, "Not now."};
    const Result r = rig.move_line(line_, -move_mm, now);
    if (!r) return r;
    start_count_ = rig.raw_feedback()[line_].position;
    raising_ = true;
    message_ = fmt("Raising the weight %.0f mm.", move_mm);
    return {};
}

Result LineTest::rise(double mm) {
    if (step_ != Step::Rise) return {Refusal::Busy, "Not now."};
    // The line reeled in was stretched too, under the weight's pull.
    const double expected = move_mm * (1.0 + hang_tension_ / line_ea_);
    const double ratio = mm / expected;
    if (!(ratio > 0.8 && ratio < 1.25))
        return {Refusal::OutOfRange,
                fmt("That's far from the %.0f mm the winch reeled in. Measure the rise again.", move_mm)};
    result_.drum = before_.drum * ratio;
    step_ = Step::Done;
    message_ = std::string("Done. The drum moves ") + fmt("%.2f%% ", std::fabs(ratio - 1.0) * 100.0) +
               (ratio >= 1.0 ? "more" : "less") + " line than it counts.";
    return {};
}

void LineTest::finish() {
    if (step_ == Step::Off || busy()) return;
    step_ = Step::Done;
    message_ = "Done.";
}

void LineTest::cancel(Rig& rig, double now) {
    if (raising_) rig.stop(std::nullopt, now);
    measuring_ = raising_ = false;
    step_ = Step::Off;
    message_ = "";
}

void LineTest::update(Rig& rig, double now) {
    if (measuring_) {
        sum_ += reading(rig);
        ++count_;
        if (now - t0_ < average_time) return;
        measuring_ = false;
        const double avg = sum_ / count_;
        if (step_ == Step::Empty) {
            result_.load_zero = avg;
            step_ = Step::Weight;
            message_ = fmt("Zero measured: the load cell reads %.2f N with nothing on it.", avg);
        } else {
            const double above = avg - result_.load_zero;
            const double newtons = weight_kg_ * kGravity;
            if (above < 0.2 * newtons) {
                message_ = "The load cell barely changed. Is the weight hanging from this line, clear of the floor?";
                return;
            }
            result_.load_scale = newtons / above;
            hang_tension_ = newtons;
            step_ = Step::Raise;
            message_ = fmt("Scale measured: %.2f kg reads as %.1f N.", weight_kg_, above);
        }
        return;
    }
    if (raising_) {
        if (rig.moving_line()) return;
        raising_ = false;
        const double moved = (start_count_ - rig.raw_feedback()[line_].position) * rig.config().sensors[line_].drum;
        if (std::fabs(moved - move_mm) > 1.0) {
            step_ = Step::Raise;
            message_ = "The winch stopped before the end: " + rig.message() +
                       " Mark the weight where it is now and raise it again.";
            return;
        }
        step_ = Step::Rise;
        message_ = "Raised. Measure how far the weight went up.";
    }
}

}  // namespace leota
