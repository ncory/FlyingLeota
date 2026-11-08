// The RS-485 Modbus anemometer (docs/plan.md, 3.5): its wind speed, read four times a
// second and given to the rig. Gusts and levels are the rig's work. With no anemometer
// set and the simulated rig, it feeds a calm 1.5 m/s so the bench can arm.
#pragma once

namespace fw {

void anemometer_start();
bool anemometer_fitted();
// The last reading in m/s, and how long ago (s); -1 if none yet.
double anemometer_last(double* age_s);

}  // namespace fw
