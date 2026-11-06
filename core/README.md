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
| `geometry.hpp` | Line lengths for a ball position, the tensions that hold it there, how much line to pay out for the stretch, and whether a point or a path is allowed. |
| `profile.hpp` | One axis's motion: trapezoid plans from any speed, timed moves (the same maths as Show Controller's `plan_speed`), jog, stops, and the smoothing filter that limits jerk. |
| `wind.hpp` | Gusts, averages, the Green, Amber and Red levels, and the Red hold. |
| `hardware.hpp` | What the controller needs from the hardware. The firmware implements it. |
| `rig.hpp` | The controller: states, arming, homing, flying, tension checks, faults, remote and local, wind, parking. |
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

## Not yet

- The lean model (docs/plan.md, section 4). The ball is treated as staying level, so its
  attachment points are a few mm off when it leans. This comes later as a correction table
  made on a PC.
- The SCMP proposed multi-axis `move`. Separate moves with the same duration do the job.
- Per-rig settings in flash, and SCMP `config` keys beyond the axis limits.

## Third-party code

Vendored under `third_party/`, both MIT licensed: ArduinoJson 7.2.1 (Benoit Blanchon), used
by the SCMP layer; doctest 2.4.11 (Viktor Kirilov), used by the tests only.
