# Touchscreen

The controller's 7 in touchscreen (1024 × 600), in LVGL 9.2. The same code will run on the
ESP32-P4's panel and runs now inside the PC simulator, whose web page shows the real screen
and passes clicks through as taps. The screens follow the mockup in `docs/plan.md` section
5.6.

![The Controls screen in the simulator](../docs/images/simulator-touchscreen.png)

## Build and test

From the FlyingLeota folder, the root build includes it:

```bash
cmake -S . -B build -G Ninja
cmake --build build
build/ui/leota_ui_tests
```

CMake fetches LVGL (tag v9.2.2) with git the first time. To use a copy you already have,
add `-DLEOTA_LVGL_DIR=/path/to/lvgl`. `-DLEOTA_UI=OFF` leaves the touchscreen out, and the
simulator then has none.

## Using it

```cpp
leota::Ui ui(rig, display);           // display: an lv_display_t of 1024 × 600
ui.update(now, link, "21:05");        // every 50 ms or so, then lv_timer_handler()
ui.log(leota::LogKind::Show, "Show Controller: move x");
```

- `Ui` talks to the `Rig` directly, as the touchscreen does on the controller. It never
  goes through SCMP, so its controls work whether remote control is on or off.
- The caller owns LVGL: `lv_init()`, the tick, the display and its flush, and the touch
  input. `ui.update()` reads the rig and refreshes what's on screen; it doesn't run LVGL.
- `UiLink` says what the Show Controller link is doing (connected, address, last heard,
  command count) for the top bar and Diagnostics.
- `ui.log()` adds a line to the Log screen. The UI logs what is done at the screen and the
  rig's own messages (faults, wind, homing); the caller adds network and show events.
- `find("controls.arm")` and `page()` are for tests: every button that matters has a name.

## The screens

| Screen | Has |
| --- | --- |
| Status | Top view with the ball, lines, workspace and dock; height; x, y, z; tension bars; wind; a short system list. |
| Controls | Remote on/off, arm (with the confirmation dialog), disarm, home, park, clear fault. Buttons that can't be used say why. |
| Jog | Hold-to-run arrows for x, y and z at three speeds, and go to the dock. Only with remote control off. |
| Weather | Wind now, gust and average, the last 30 minutes' gusts against the Amber and Red lines, the levels and what each does, and the Red hold's countdown. |
| Diagnostics | Each winch's line out, tension, expected tension, drive and brake; safety inputs; wind sensor; network. |
| Setup | Behind a PIN (`1234` in the simulator). The rig's settings, read-only for now. |
| Log | Tonight's events, filtered by safety, motion, wind, network and show. |

During an E-stop every screen gives way to a red one: what happened, that motor power is off
and the brakes hold, and how to recover. STOP is on every screen.

## Files

| File | Does |
| --- | --- |
| `include/leota/ui.hpp` | The `Ui` class and `UiLink`. |
| `src/ui.cpp` | Every screen, the arm dialog and the E-stop page. |
| `lv_conf.h` | LVGL's settings: 16-bit color, 512 KB for LVGL's heap. |
| `fonts/` | Inter at 14, 16, 20 and 28 px with a few icons. See `fonts/README.md`. |
| `tests/test_ui.cpp` | Taps through the screens against the simulated rig, through LVGL's own pointer input. |

## On the ESP32-P4

Not done yet. The firmware will give LVGL a MIPI-DSI display with buffers in PSRAM, the
panel's touch controller as the pointer, and a tick from the system timer, then call
`ui.update()` from the UI task. The control loop runs in its own task, so the UI must only
read the rig and call its methods under the same lock the SCMP task uses.
