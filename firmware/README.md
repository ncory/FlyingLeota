# Firmware (ESP32-P4)

The Flying Leota controller on Espressif's ESP32-P4 Function EV Board, with its 7 in
1024 × 600 touch panel. The motion core (`core/`), the touchscreen (`ui/`) and the
settings are the same code the PC simulator runs; this folder adds the hardware around
them.

**Status: compiles, not yet run.** It builds without warnings on ESP-IDF 5.5.6 for all
three kinds of drive (a 1.4 MB app; a fifth of the internal RAM). Nothing here has run on
a board yet: that is Phase 1 on the bench (docs/plan.md, section 8).

## Build

Needs ESP-IDF 5.5 (`. $IDF_PATH/export.sh`). From this folder:

```bash
idf.py set-target esp32p4
idf.py menuconfig        # Flying Leota: drives, pins
idf.py build flash monitor
```

In the "Flying Leota" cloud environment, ESP-IDF is already installed beside the
checkouts, in `esp/`. Start each shell with:

```bash
export IDF_TOOLS_PATH=$PWD/../../esp/tools   # from this folder
. ../../esp/esp-idf/export.sh
```

The component manager fetches the board support package and LVGL 9.2 the first time.
LVGL stays at 9.2.2, the simulator's version; Espressif's LVGL port is held at 2.7.2,
because 2.8 and later need LVGL 9.3.

## What runs

| Task | Core | Does |
| --- | --- | --- |
| motion | 1, alone | Woken every 1 ms by a hardware timer. Runs `Rig::update`, toggles the watchdog charge pump, reads the key switch. |
| scmp | 0 | SCMP on UDP, through the same `ScmpServer` as the simulator. Status to whoever last sent a hello or heartbeat. |
| screen | 0 | Refreshes the touchscreen from the rig 20 times a second. LVGL itself runs in Espressif's LVGL port task. |
| loadcells | 0 | Four HX711s on a shared clock, about 80 readings a second. |
| wind | 0 | The RS-485 Modbus anemometer, four times a second. |
| console | 0 | Bench commands on the USB serial port. |

Every task but motion reaches the rig only under one lock (`app.hpp`), briefly. Touch
handlers take it too (`Ui::set_lock`). Lock order is always the display, then the rig.

## Drives (menuconfig: Flying Leota, Drives)

- **Simulated rig** (the default): the PC simulator's plant runs inside the controller.
  Everything else is real: screen, Ethernet, SCMP, Setup, settings in flash. With no
  anemometer it feeds a calm 1.5 m/s. This is the first thing to run on the board, and
  Show Controller can fly it.
- **Step/dir** (closed-loop steppers): one RMT channel per line. Each millisecond's
  steps go out evenly spaced, padded to exactly 1 ms, at up to 60 kHz. The steps sent are
  the line's position; a drive that can't follow raises its alarm input.
- **ODrive over CAN**: CANSimple `Set_Input_Pos` every millisecond; heartbeats and
  encoder estimates come back by themselves (set them cyclic on the ODrives). A drive
  not heard for 100 ms counts as a fault.

Every pin is set in menuconfig and starts unset (-1: not fitted), except Ethernet's,
which are the board's own. Check them against the board's header before wiring.

## Safety I/O

- **E-stop chain status** input. With real drives and no input set, the chain reads
  open and the rig won't arm.
- **Drive enable** output with a **watchdog charge pump**: a 500 Hz square wave from the
  motion task while enabled. A hung controller stops it.
- **Brake release** output: high releases, so no power grips.
- **Drive alarm** inputs (step/dir). A load cell that stops answering for 250 ms also
  counts as a drive fault.
- **Local/Remote key** input, if fitted.

## The bench console

`idf.py monitor`, then:

| Command | Does |
| --- | --- |
| `status` | The rig, each line, the wind and the network. |
| `timing` | How long the motion cycles take, how late they wake, and the longest wait for the rig lock. `timing reset` starts again. |
| `inputs` | E-stop, key, alarms, raw load cell counts, the anemometer. |
| `drives` | Step counts per line, or each ODrive's state, errors and position. |
| `rig arm` … | `arm`, `disarm`, `home`, `park`, `stop`, `clear`, `remote on`/`off`. |
| `steps A 200 100` | Step/dir only, on the bench: line A out 200 mm at 100 mm/s and back, brakes released. Refused if the rig is armed or has been homed. |
| `settings` | The stored settings as JSON; `settings reset` clears them. |

Phase 1 with it: run the simulated rig with Show Controller connected and the screen in
use, and read `timing`. Then step/dir with a drive on the bench and a logic analyzer on
its step pin: `steps`, while tapping the screen and with Show Controller sending.

## Files

| File | Does |
| --- | --- |
| `main/main.cpp` | Start-up order, the rig lock. |
| `main/motion.cpp` | The 1 ms timer and motion task, timing figures, the bench hook. |
| `main/board_io.cpp` | E-stop, key, enable and pump, brakes, alarms. |
| `main/drives_sim.cpp`, `drives_stepdir.cpp`, `drives_odrive.cpp` | The three kinds of drive. |
| `main/load_cells.cpp`, `anemometer.cpp` | The sensors. |
| `main/network.cpp` | Ethernet (DHCP or Setup's fixed address) and SCMP. |
| `main/screen.cpp` | Display, touch and the UI task. |
| `main/nvs_store.cpp` | Setup's settings as one JSON blob in NVS. |
| `main/console.cpp` | The bench commands. |
| `main/Kconfig.projbuild` | menuconfig's Flying Leota menu. |
| `components/leota_core`, `components/leota_ui` | The shared code, as ESP-IDF components. |

## Not yet

- Run on a board.
- The RMT step timing on the P4 is the least proven part (docs/plan.md, Phase 1).
- Over-the-air updates, and an event log in flash.
