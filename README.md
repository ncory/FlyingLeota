# Flying Leota

A crystal ball with a lifesize head inside, flying on four nearly invisible lines inside a
gazebo. Four winches move it like Skycam. An ESP32-P4 controller with Ethernet and a touchscreen
drives them, and Show Controller tells it what to do over SCMP.

| Where | What |
| --- | --- |
| `docs/plan.md` | The plan: site, hardware, safety, sizing, software, wind rules, tests. |
| `docs/FlyingLeota-plan.pdf` | The plan as a PDF to share. |
| `core/` | The motion core: the controller's logic in plain C++, with tests. See `core/README.md`. |

Licensed under the MIT License (see `LICENSE`). Third-party code keeps its own license.
