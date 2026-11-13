# Brief for Show Controller: the ball's battery

**Done.** Show Controller built both (ncory/BEDShowController fcff52d, November 2026): battery
reporting as "Battery-powered units" (design section 9), and the `dock` field exactly as
proposed. FlyingLeota's controller, `leota-sim` and `sim/tests` send and test the `dock`
field. Tell Show Controller which gauge and battery board the ball ends up with: its I²C
drivers are untested on hardware.

From the Flying Leota work (repo ncory/FlyingLeota), for whoever is building Show
Controller. Nate asked for this to be passed on. It proposes two things; Show
Controller's design decisions are its own, so treat both as proposals.

Background: `docs/plan.md` in FlyingLeota, section 4, "Power on board" (the head's power,
inductive charging and "Watching the charge").

## The setup

The flying ball carries a Raspberry Pi Zero 2 W running Show Controller as a follower. It
plays the face video on an Ultimems HD309D1-C1 laser projector and runs on a 60–75 Wh
lithium-ion pack. The pack charges inductively whenever the ball sits on its dock.

Inside the ball, on the Pi's I²C bus:

- **A fuel gauge** (a 1S gauge such as Maxim MAX17260 or TI BQ27441, or whatever the
  chosen battery board has): charge %, voltage, current (+ charging, − discharging),
  battery temperature, time to empty or full.
- **The charger's status:** "charging" and "power good", on GPIO pins or over I²C.

The parts aren't chosen yet (test H3 in the plan), so the reading code should take a small
driver per gauge, and a simulated one for tests.

## 1. A follower that reports its battery

The general feature: any battery-powered unit reports its battery. It's not specific to
this ball.

- **Read** the gauge every few seconds: charge %, volts, amps, temperature, charging
  (yes/no), and time left if the gauge gives it. Configured per unit, for example a
  `battery` block in the unit's settings: driver, I²C bus and address, charger status
  pins.
- **Report** it to the leader with the follower's other state (`STATE_EVERY`), and in
  the follower's own status page.
- **Show** it: the Units page and the Dashboard follower rows (charge %, with a charging
  mark), and a health warning below a set charge (suggested 25%, per unit).
- **Log** it: show log entries when charging starts and stops, and when the charge
  crosses the warning level.
- **Shut down cleanly** at a set charge (suggested 5%), so the SD card isn't damaged. The
  battery board turns the Pi back on when charging starts. Say so in the log first.
- **A trigger condition** for "this unit's battery is at least N%", so a show can refuse
  to start on a nearly flat ball.

The ball's touchscreen-side copy (the motion controller showing the ball's charge) is
FlyingLeota's job: the Pi will send it a small UDP message itself. Nothing for Show
Controller there, unless you'd rather relay it.

## 2. An SCMP optional field for dock charging

The motion controller switches the dock's charger and measures the power it draws, so it
knows when the ball is charging even when the Pi is flat and silent. Proposed for
`docs/scmp.md`, "Optional fields":

```json
"dock": {"state": "charging", "watts": 18.4}
```

- `state`: `charging` (taking power), `charged` (seated, taking only a trickle),
  `off` (seated, but the controller has the charger off: only when Setup's "Charge while
  armed" is turned off and the rig is armed),
  `empty` (nothing on the dock), or `fault` (drawing power with no ball: something metal
  on the pad; the controller turns it off).
- `watts`: what the charger draws, one decimal place.
- Sent in `status` by controllers with a charging dock; left out otherwise.

Suggested display: on the Motion page, a line under the controller such as "Dock:
charging, 18 W", and a show log entry for `fault`. Names are open: change them if they fit
Show Controller better, and FlyingLeota's controller and `leota-sim` will follow.

When this is agreed, the FlyingLeota side adds it to the controller, the simulator and its
tests against Show Controller (`sim/tests/test_show_controller.py`).
