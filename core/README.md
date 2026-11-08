# Motion core

The Flying Leota controller's logic, as plain C++17 with no hardware or operating-system
calls. The same code runs in the ESP32-P4 firmware, in the PC simulator and in these tests.
The firmware supplies the hardware (drives, brakes, E-stop chain, load cells, wind sensor)
through one small interface, and a transport for SCMP.

## Build and test

```bash
cmake -S core -B core/build -G Ninja
cmake --build core/build
core/build/leota_core_tests
```

Needs CMake 3.16 or later and a C++17 compiler. The tests take under a second.

## What's here

| File | Does |
| --- | --- |
| `config.hpp` | Everything about one rig: pulleys, ball, workspace, limits, timings. `gazebo_config()` is this year's site. `check_config()` finds mistakes. |
| `geometry.hpp` | Line lengths for a ball position, the tensions that hold it there, how the ball leans there, how much line to pay out for the stretch, and whether a point or a path is allowed. |
| `profile.hpp` | One axis's motion: trapezoid plans from any speed, timed moves (the same maths as Show Controller's `plan_speed`), jog, stops, and the smoothing filter that limits jerk. |
| `wind.hpp` | Gusts, averages, the Green, Amber and Red levels, and the Red hold. |
| `hardware.hpp` | What the controller needs from the hardware. The firmware implements it. The rig corrects its readings with `config().sensors`. |
| `rig.hpp` | The controller: states, arming, homing, flying, tension checks, faults, remote and local, wind, parking. |
| `settings.hpp` | What a person may change on site, as one table of fields with labels, units and ranges; checks; JSON for flash; `SettingsKeeper` applies changes to the rig and stores them through a `SettingsStore` (NVS on the board, a file in the simulator). |
| `calibration.hpp` | The calibration check: points to fly to, `CalibrationRun` to fly and measure them, and `fit_calibration()`, which finds the pulley positions and head weight that agree with what was measured. |
| `line_test.hpp` | The line test: one line's load cell zero and scale from a known weight, and its drum correction from raising that weight 500 mm. |
| `scmp.hpp` | SCMP version 1 with its Optional fields: messages in, acks and status out. No transport. |
| `plant.hpp` | A simulated rig for tests and the PC simulator. Not used by the firmware. |

## Conventions

- **Frame:** x to the audience's right, y away from the audience (upstage), z up from the
  deck. The origin is the middle of the gazebo floor. Positions are of the ball's centre.
- **Units:** mm, s, N and kg. Speeds in mm/s, accelerations in mm/s². Wind in m/s.
- **Lines:** A back left, B back right, C front left, D front right.
- **Time:** every call takes `now` in seconds. `Rig::update(now)` runs the control loop in
  fixed cycles of `control_period` (1 ms), catching up to `now`.
- **No exceptions, no threads.** Refusals come back as a `Result` with an SCMP error name and
  a plain sentence.

## How it works, in short

- **Flying.** Each axis plans its own trapezoid, and a moving-average filter turns it into an
  S-curve. Every cycle the rig works out the ball's position, the tensions that should hold it
  there, and from those how much line each winch should pay out (allowing for stretch).
- **The lean.** Off centre, the ball leans until its weight, hanging below its centre
  (`centre_of_weight`), balances the lines' uneven pull: a few degrees, while staying square
  to the audience. `hanging_pose()` solves for the lean, and line lengths are worked out to
  the leaning attachment points. It is re-solved every `lean_every` cycles (10), starting
  from the last answer; the lean changes slowly, so the last answer serves in between.
  Without it the ball lands up to about 15 mm off target; with it, within 0.2 mm in the
  simulation.
- **Four lines, three axes.** One line is always extra, so the lines can pull against each
  other without the ball moving. The tensions are spread so the highest is as low as
  possible, keeping every line at least `plan_tension_min` (8 N) tight. Slack is a fault below
  `tension_min` (5 N), so a plan never sits on the fault line.
- **The tension loop** trims only that pulling-against-each-other part, by paying line in or
  out along the one direction that doesn't move the ball. Any other tension error means the
  ball isn't where the model thinks. Trimming it would hide that, so it is left to show up,
  and a large one is a `tension_mismatch` fault.
- **Homing** happens on the dock. Each line reels in until it reaches `pretension`, which is
  below what would lift the ball. That gives every line's length. The extra slack is taken up
  gently, then the ball lifts 30 mm and the tensions are checked against the model.
- **Faults.** A drive alarm or the E-stop puts the brakes on at once. Every other fault stops
  smoothly, then brakes, then turns the drives off. `arm` clears faults once their cause is
  gone.
- **Wind.** At Amber, new moves must stay in a smaller box at half speed. At Red the ball
  flies back over the dock, sets down and disarms. Arming then waits for the hold time.

What the tests found about calibration: homing measures every line at the dock, which soaks
up most pulley-position error. With a pulley 120 mm sideways and 80 mm low, the ball lands
within about 15 mm and the tensions are within about 5 N. Errors of a few centimetres cost a
millimetre or two.

The calibration check finds what homing can't. Each point it flies to gives four line
lengths (counted from homing) and four tensions; with the ball hanging still, the tensions
must balance its weight and the lengths must reach the pulleys. Levenberg–Marquardt fits
the 12 pulley coordinates, the weight, and each point's centre and lean together, with the
tape measurements as a loose prior (300 mm). The prior matters for one thing only: turning
all four pulleys together about the dock changes nothing measurable. Tests: B 50 mm low and
C 36 mm off take the worst landing from 11 mm to under 0.5 mm; a 0.8 kg heavier head is
weighed to 1%; 0.3 N of load-cell noise still finds a 85 mm error to within 15 mm.

## On the ESP32-P4

On this PC a control cycle's geometry takes about 0.3 µs, or 4 µs on a cycle that re-solves
the lean. The P4 runs at about 360–400 MHz, and its floating-point unit is single precision,
so the core's `double` maths runs in software there. Measure the cycle time on the board in
Phase 1. If it's tight, re-solve the lean less often (`lean_every`), or switch the core to
`float`.

## Not yet

- The SCMP proposed multi-axis `move`. Separate moves with the same duration do the job.
- SCMP `config` keys beyond the axis limits. (Settings do persist: the touchscreen's
  Setup changes them all, and limits set over SCMP are kept with them.)
- Writing settings to NVS: the firmware's `SettingsStore`.

## Third-party code

Vendored under `third_party/`, both MIT licensed: ArduinoJson 7.2.1 (Benoit Blanchon), used
by the SCMP layer; doctest 2.4.11 (Viktor Kirilov), used by the tests only.
