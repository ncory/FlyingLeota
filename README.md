# Flying Leota

A crystal ball with a lifesize head inside, flying on four nearly invisible lines inside a
gazebo. Four winches move it like Skycam. An ESP32-P4 controller with Ethernet and a touchscreen
drives them, and Show Controller tells it what to do over SCMP.

| Where | What |
| --- | --- |
| `docs/plan.md` | The plan: site, hardware, safety, sizing, software, wind rules, tests. |
| `docs/FlyingLeota-plan.pdf` | The plan as a PDF to share. |
| `core/` | The motion core: the controller's logic in plain C++, with tests. See `core/README.md`. |
| `sim/` | The PC simulator: the core on a simulated rig, speaking SCMP, with a 3-D viewer. See `sim/README.md`. |
| `ui/` | The touchscreen, in LVGL: the same screens on the controller and in the simulator. See `ui/README.md`. |

Build everything that runs on a PC with `cmake -S . -B build -G Ninja && cmake --build build`.
The core's tests are `build/core/leota_core_tests`, the touchscreen's
`build/ui/leota_ui_tests`. The build fetches LVGL with git; to use a local copy instead, add
`-DLEOTA_LVGL_DIR=/path/to/lvgl`, or leave the touchscreen out with `-DLEOTA_UI=OFF`.

Licensed under the MIT License (see `LICENSE`). Third-party code keeps its own license.
