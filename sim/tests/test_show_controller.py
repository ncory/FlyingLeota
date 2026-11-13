"""Show Controller against the Flying Leota simulator.

Show Controller's own SCMP client and motion module connect to leota-sim over UDP, as
they will to the real controller, and arm, home and fly the simulated ball.

Needs a built simulator (cmake --build build) and a Show Controller checkout with its
virtualenv. From the FlyingLeota folder:

    SHOWCONTROLLER=../bedshowcontroller ../bedshowcontroller/.venv/bin/python -m pytest sim/tests
"""

from __future__ import annotations

import asyncio
import json
import os
import pathlib
import subprocess
import sys
import time
import urllib.request

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SC = pathlib.Path(os.environ.get("SHOWCONTROLLER", ROOT.parent / "bedshowcontroller")).resolve()
SIM = pathlib.Path(os.environ.get("LEOTA_SIM", ROOT / "build" / "sim" / "leota-sim")).resolve()
sys.path[:0] = [str(SC), str(SC / "tests")]

pytestmark = pytest.mark.skipif(
    not SIM.exists() or not (SC / "showcontroller").is_dir(),
    reason="needs build/sim/leota-sim and a Show Controller checkout (SHOWCONTROLLER=...)")


class Sim:
    """leota-sim in a subprocess, on ports it picks itself."""

    def __init__(self) -> None:
        self.proc = subprocess.Popen([str(SIM), "--host", "127.0.0.1", "--port", "0", "--http", "0"],
                                     stdout=subprocess.PIPE, text=True)
        self.port = self.http = 0
        for line in self.proc.stdout:  # type: ignore[union-attr]
            if line.startswith("SCMP on UDP port"):
                self.port = int(line.split()[-1])
            elif line.startswith("Viewer on"):
                self.http = int(line.rstrip("/\n").rsplit(":", 1)[1])
            elif line.strip() == "ready":
                break

    def state(self) -> dict:
        with urllib.request.urlopen(f"http://127.0.0.1:{self.http}/api/state", timeout=2) as r:
            return json.loads(r.read())

    def action(self, **body) -> dict:
        req = urllib.request.Request(f"http://127.0.0.1:{self.http}/api/action",
                                     data=json.dumps(body).encode(), method="POST")
        with urllib.request.urlopen(req, timeout=2) as r:
            return json.loads(r.read())

    def ball_near(self, x: float, y: float, z: float, within: float = 1.0) -> bool:
        b = self.state()["ball"]
        return abs(b["x"] - x) < within and abs(b["y"] - y) < within and abs(b["z"] - z) < within

    def stop(self) -> None:
        self.proc.terminate()
        self.proc.wait(timeout=5)


@pytest.fixture
def sim():
    s = Sim()
    yield s
    s.stop()


async def wait(cond, timeout: float = 10.0) -> None:
    end = time.monotonic() + timeout
    while not cond():
        if time.monotonic() > end:
            raise AssertionError("timed out waiting")
        await asyncio.sleep(0.05)


async def test_show_controllers_scmp_client_flies_the_ball(sim):
    from showcontroller.scmp import ScmpClient, ScmpError

    c = ScmpClient("127.0.0.1", sim.port)
    info = await c.connect()
    try:
        assert info["name"] == "Flying Leota" and info["coupled"] is True
        assert info["tension_min"] == 5 and info["tension_max"] == 150
        assert [a["axis"] for a in info["axes"]] == ["x", "y", "z"]
        assert all(a["units"] == "mm" and a["needs_homing"] for a in info["axes"])

        await c.command("arm")
        await wait(lambda: c.status and c.status["armed"] and c.axis("z")["state"] == "idle")
        with pytest.raises(ScmpError, match="not_homed"):
            await c.command("move", axis="z", target=1200, duration=2)
        await c.command("home", axis="z")
        await wait(lambda: c.axis("z")["homed"] and c.axis("z")["state"] == "idle", 40)
        assert c.status["remote"] is True and c.status["wind"]["level"] == "green"
        assert [l["line"] for l in c.status["lines"]] == ["A", "B", "C", "D"]

        await c.command("move", axis="x", target=-300, duration=2.5)
        await c.command("move", axis="z", target=1300, duration=2.5)
        await wait(lambda: c.axis("z")["state"] == "moving", 2)
        await wait(lambda: sim.ball_near(-300, 0, 1300), 6)
        await wait(lambda: c.axis("x")["state"] == "idle", 3)
        assert c.axis("x")["position"] == pytest.approx(-300, abs=0.01)

        ack = await c.command("move", axis="z", target=1500, duration=0.4)
        assert ack["late"] > 0.5
        await c.command("stop")

        sim.action(do="remote", on=False)  # the touchscreen takes over
        await wait(lambda: c.status["remote"] is False, 2)
        with pytest.raises(ScmpError, match="local"):
            await c.command("move", axis="z", target=1200)
        await c.command("stop")  # stopping always works
        sim.action(do="remote", on=True)

        sim.action(do="estop", on=True)
        await wait(lambda: c.status["estop"] and not c.status["armed"], 2)
        assert "estop" in c.axis("z")["faults"]
        with pytest.raises(ScmpError, match="estop"):
            await c.command("arm")
    finally:
        await c.close()


async def test_show_controller_runs_a_show_on_the_simulator(sim, tmp_path):
    from conftest import base_project
    from showcontroller.app import ShowController

    p = base_project()
    p["devices"] = [{"id": "leota", "name": "Flying Leota", "type": "scmp",
                     "config": {"host": "127.0.0.1", "port": sim.port}}]
    positions = {"x": {"centre": 0, "left": -400}, "y": {"centre": 0},
                 "z": {"low": 1000, "float": 1300, "high": 1450}}
    for axis, named in positions.items():
        p["outputs"].append({"id": f"leota-{axis}", "unit": "leader", "type": "motion",
                             "config": {"device": "leota", "axis": axis, "positions": named}})
    p["timelines"][1]["items"] = [
        {"id": "rise", "type": "move", "output": "leader/leota-z", "offset": 0.5,
         "params": {"position": "float", "duration": 3.0}},
        {"id": "drift", "type": "move", "output": "leader/leota-x", "offset": 1.0,
         "params": {"position": "left", "duration": 2.5}},
        {"id": "higher", "type": "move", "output": "leader/leota-z", "offset": 5.0,
         "params": {"position": "high", "duration": 2.0}},
        {"id": "back", "type": "move", "output": "leader/leota-x", "offset": 5.0,
         "params": {"position": "centre", "duration": 2.0}},
    ]
    p["timelines"][1]["duration"] = 9

    sc = ShowController(tmp_path, renderer="inprocess", gpio="sim")
    await sc.start()
    try:
        sc.replace_project(p)
        await wait(lambda: sc.modules_ctx.devices["leota"].connected, 5)
        st = await sc.bus.run("motion.status", {})
        assert st["devices"]["leota"]["info"]["name"] == "Flying Leota"
        assert sorted(a["axis"] for a in st["axes"]) == ["x", "y", "z"]

        await sc.bus.run("motion.arm", {"device": "leota"})
        await wait(lambda: sim.state()["state"] == "ARMED", 3)
        await sc.bus.run("motion.home", {"output": "leader/leota-z"})
        await wait(lambda: sim.state()["homed"] and sim.state()["state"] == "ARMED", 40)
        await sc.bus.run("motion.goto", {"output": "leader/leota-z", "target": 1000})
        await wait(lambda: sim.ball_near(0, 0, 1000), 6)

        sc.engine.request_start("show")
        await wait(lambda: sim.ball_near(-400, 0, 1300), 6)  # rise and drift, together
        await wait(lambda: sim.ball_near(0, 0, 1450), 6)     # higher, and back to the centre
        assert sim.state()["faults"] == []
        log = "\n".join(sim.state()["log"])
        assert "SCMP move z to 1450: ok" in log

        await sc.bus.run("motion.disarm", {"device": "leota"})
        await wait(lambda: sim.state()["state"] == "DISARMED", 4)
    finally:
        await sc.stop()


async def test_show_controller_shows_what_the_controller_reports(sim, tmp_path):
    """SCMP's optional fields, from leota-sim to Show Controller's motion status, events,
    show log and skipped moves (docs/scmp.md, "Optional fields")."""
    from conftest import base_project
    from showcontroller.app import ShowController

    p = base_project()
    p["devices"] = [{"id": "leota", "name": "Leota rig", "type": "scmp",
                     "config": {"host": "127.0.0.1", "port": sim.port}}]
    for axis in ("x", "y", "z"):
        p["outputs"].append({"id": f"leota-{axis}", "unit": "leader", "type": "motion",
                             "config": {"device": "leota", "axis": axis,
                                        "positions": {"up": 1200, "centre": 0}}})
    p["timelines"][1]["items"] = [
        {"id": "rise", "type": "move", "output": "leader/leota-z", "offset": 0.2,
         "params": {"position": "up", "duration": 2.0}}]
    p["timelines"][1]["duration"] = 3

    sc = ShowController(tmp_path, renderer="inprocess", gpio="sim")
    events: list = []
    await sc.start()
    sc.bus.subscribe(lambda e: events.append(e) if e.type.startswith("motion.") else None)
    try:
        sc.replace_project(p)
        dev = lambda: sc.modules_ctx.devices["leota"]  # noqa: E731
        await wait(lambda: dev().connected and dev().status()["lines"] is not None, 5)
        st = dev().status()
        assert st["coupled"] is True and st["remote"] is True
        assert st["wind"]["level"] == "green"
        assert [l["line"] for l in st["lines"]] == ["A", "B", "C", "D"]
        assert st["message"].startswith("Started.")

        # The dock's charger: on whenever the ball rests on it, armed or not.
        dock = lambda: dev().status()["dock"] or {}  # noqa: E731
        await wait(lambda: dock().get("state") == "charging" and dock().get("watts") == 20.0, 5)

        await sc.bus.run("motion.arm", {"device": "leota"})
        await wait(lambda: sim.state()["state"] == "ARMED", 3)
        await asyncio.sleep(0.5)
        assert dock()["state"] == "charging"  # armed between shows: still charging
        await sc.bus.run("motion.home", {"output": "leader/leota-z"})
        await wait(lambda: sim.state()["homed"] and sim.state()["state"] == "ARMED", 40)
        await wait(lambda: all(l["tension"] > 5 for l in dev().status()["lines"]), 3)
        await wait(lambda: dock().get("state") == "empty", 3)  # lifted 30 mm off it after homing
        # Set down at dock height (the bottom of z), still armed: charging again.
        z_min = next(a for a in dev().info["axes"] if a["axis"] == "z")["min"]
        await sc.bus.run("motion.goto", {"output": "leader/leota-z", "target": z_min})
        await wait(lambda: dock().get("state") == "charging", 6)
        assert sim.state()["armed"]

        # The touchscreen takes over: Show Controller says so, and a move is skipped.
        sim.action(do="remote", on=False)
        await wait(lambda: dev().status()["remote"] is False, 3)
        await wait(lambda: any(e.type == "motion.remote" for e in events), 2)
        sc.engine.request_start("show")
        await wait(lambda: any(e.type == "motion.skipped" for e in events), 5)
        skipped = next(e for e in events if e.type == "motion.skipped")
        assert skipped.data["error"] == "local" and "Remote control is off" in skipped.data["reason"]
        assert not any(e.type == "item.failed" for e in sc.bus._history)
        sim.action(do="remote", on=True)
        await wait(lambda: dev().status()["remote"] is True, 3)

        # The wind sensor goes quiet: Red, with the hold counting down.
        sim.action(do="wind", sensor=False)
        await wait(lambda: (dev().status()["wind"] or {}).get("level") == "red", 10)
        wind = dev().status()["wind"]
        assert wind["calm_left"] > 0
        red = next(e for e in events if e.type == "motion.wind")
        assert red.data["level"] == "red" and red.data["name"] == "Leota rig"
        await wait(lambda: any(e.type == "motion.message" and "wind sensor" in e.data["message"]
                               for e in events), 3)

        # Red parks the ball on the dock and disarms: charging again. Then metal on the pad.
        await wait(lambda: dock().get("state") == "charging", 30)
        sim.action(do="metal", on=True)
        await wait(lambda: dock().get("state") == "fault", 3)
        await wait(lambda: any(e.type == "motion.dock" and e.data["state"] == "fault" for e in events), 3)

        entries = (tmp_path / "logs" / "show.jsonl").read_text()
        assert "under local control" in entries
        assert "was skipped: Remote control is off at the controller" in entries
        assert "the wind is Red" in entries
        assert "found something other than the ball on its pad" in entries
    finally:
        await sc.stop()
