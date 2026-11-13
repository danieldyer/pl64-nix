#!/usr/bin/env python3
"""End-to-end service/control/MCAP test with a synthetic DVB-T waveform.

No hardware is opened. The test navigation publisher is explicitly a simulator.
Pass built executables and an existing CI16 fixture to avoid network dependencies.
"""
import argparse
import json
import os
import pathlib
import signal
import socket
import subprocess
import tempfile
import time


def request(root, service, message):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(2)
        connection.connect(str(root / ("pl64-" + service) / "control.sock"))
        connection.sendall(json.dumps(message).encode() + b"\n")
        data = bytearray()
        while b"\n" not in data:
            part = connection.recv(65536)
            if not part:
                raise RuntimeError("Control connection closed")
            data.extend(part)
        return json.loads(data.split(b"\n", 1)[0])


def wait_for(predicate, timeout=60):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, KeyError):
            pass
        time.sleep(0.05)
    raise RuntimeError("Timed out waiting for service state")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tools", type=pathlib.Path, required=True)
    parser.add_argument("--radar", type=pathlib.Path, required=True)
    parser.add_argument("--radar-schema", type=pathlib.Path, required=True)
    parser.add_argument("--system-schema", type=pathlib.Path, required=True)
    parser.add_argument("--navigation-schema", type=pathlib.Path, required=True)
    parser.add_argument("--test-source", type=pathlib.Path, required=True)
    parser.add_argument("--metadata", type=pathlib.Path, required=True)
    parser.add_argument("--fixture", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--gui", type=pathlib.Path)
    parser.add_argument("--ins", type=pathlib.Path)
    parser.add_argument("--ui-scale", type=float, default=1.5)
    parser.add_argument("--range-metres", type=float, default=30000)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    processes, logs = [], []
    with tempfile.TemporaryDirectory(prefix="p64-") as temporary:
        root = pathlib.Path(temporary)

        def launch(name, command):
            log = (args.output / (name + ".log")).open("wb")
            logs.append(log)
            process = subprocess.Popen(list(map(str, command)), stdout=log, stderr=subprocess.STDOUT,
                                       env=os.environ | {"XDG_CONFIG_HOME": str(root / "config")})
            processes.append(process)
            return process

        try:
            launch("navigation", [args.test_source, root, args.navigation_schema])
            launch("recorder", [args.tools / "pl64-recorder", "--runtime-dir", root, "--directory", args.output / "recordings"])
            launch("sar", [args.tools / "pl64-sar", "--runtime-dir", root, "--schema", args.system_schema])
            radar_command = [args.radar, "--runtime-dir", root / "pl64-radar", "--metadata", args.metadata,
                             "--schema", args.radar_schema, "--preview-schema", args.system_schema,
                             "--config", root / "radar-settings.json"]
            radar = launch("radar", radar_command)
            for service in ("ins", "radar", "recorder", "sar"):
                wait_for(lambda service=service: request(root, service, {"op": "status"}).get("ok"))
            assert request(root, "radar", {"op": "status"})["state"] == "Idle"
            assert request(root, "sar", {"op": "status"})["state"] == "Idle"
            assert not request(root, "recorder", {"op": "status"})["enabled"]
            assert request(root, "sar", {"op": "start"})["ok"]
            status = request(root, "radar", {"op": "status"})
            settings = status["settings"]
            settings.update(simulation=str(args.fixture.resolve()), simulate_once=True, unpaced=True,
                            range_metres=args.range_metres, integration=0.5, update=0.25, fec_workers=2, frame_slots=5)
            # One RF channel is enough to exercise the complete decoder, profile,
            # shared-memory, recorder and GUI paths without a long qualification run.
            settings["metadata_json"]["signals"] = settings["metadata_json"]["signals"][:1]
            invalid = dict(settings, frame_slots=1)
            assert not request(root, "radar", {"op": "configure", "settings": invalid})["ok"]
            assert request(root, "radar", {"op": "configure", "settings": settings})["ok"]
            stored = (root / "radar-settings.json").read_bytes()
            session_settings = dict(settings, doppler=125)
            assert request(root, "radar", {"op": "configure", "settings": session_settings, "persist": False})["ok"]
            assert (root / "radar-settings.json").read_bytes() == stored
            assert request(root, "radar", {"op": "configure", "settings": settings, "persist": False})["ok"]
            gui = None
            if args.gui:
                gui = launch("gui", [args.gui, "--runtime-dir", root, "--width", "1920", "--height", "1280",
                                     "--ui-scale", str(args.ui_scale), "--reset-layout", "--frames", "500", "--screenshot", args.output / "control.ppm"])
            assert request(root, "radar", {"op": "start"})["ok"]
            assert not request(root, "radar", {"op": "start"})["ok"]
            assert not request(root, "radar", {"op": "configure", "settings": settings})["ok"]
            wait_for(lambda: request(root, "radar", {"op": "status"})["state"] in ("Running", "Draining", "Fault"), 90)
            current = request(root, "radar", {"op": "status"})
            if current["state"] == "Fault":
                raise RuntimeError(current["error"])
            assert request(root, "recorder", {"op": "recording", "enabled": False})["ok"]
            wait_for(lambda: request(root, "recorder", {"op": "status"})["state"] == "Idle")
            assert request(root, "recorder", {"op": "recording", "enabled": True})["ok"]
            wait_for(lambda: request(root, "radar", {"op": "status"})["state"] in ("Idle", "Fault"), 180)
            final = request(root, "radar", {"op": "status"})
            assert final["state"] == "Idle", final
            assert final["stream"]["records"] > 4, final
            wait_for(lambda: request(root, "sar", {"op": "status"})["inputs"]["radar"]["records"] > 4)
            assert request(root, "sar", {"op": "stop"})["ok"]
            request(root, "recorder", {"op": "recording", "enabled": False})
            wait_for(lambda: request(root, "recorder", {"op": "status"})["state"] == "Idle")
            summaries = []
            for path in sorted((args.output / "recordings").glob("*.mcap")):
                assert ".partial." not in path.name, path
                result = subprocess.run([str(args.tools / "pl64-mcap-check"), str(path)], check=True, capture_output=True, text=True)
                summaries.append(json.loads(result.stdout))
                if args.ins:
                    subprocess.run([str(args.ins), "replay", "--input", str(path)],
                                   check=True, capture_output=True, text=True)
            topics = {topic for result in summaries for topic in result["topics"]}
            assert {"/navigation/events", "/radar/capture", "/sar/events"} <= topics, topics
            assert sum(result.get("record_kinds", {}).get("/radar/capture", {}).get("Profiles", 0)
                       for result in summaries) > 0, "No scientific profile batches were recorded"
            assert sum(result.get("record_kinds", {}).get("/radar/capture", {}).get("Complete", 0)
                       for result in summaries) == 1, "Recorder did not drain the capture completion record"
            assert not list(root.rglob("*.dvblive")) and not list(root.rglob("diagnostics.jsonl"))
            if gui:
                assert gui.wait(timeout=90) == 0
            # Exercise the user-requested stop path while acquisition is active,
            # separately from the first capture's natural end of fixture.
            settings.update(simulate_once=False, unpaced=False)
            assert request(root, "radar", {"op": "configure", "settings": settings})["ok"]
            assert request(root, "recorder", {"op": "start"})["ok"]
            wait_for(lambda: request(root, "radar", {"op": "status"})["state"] == "Running", 90)
            assert request(root, "recorder", {"op": "stop"})["ok"]
            assert request(root, "radar", {"op": "status"})["state"] == "Running"
            assert request(root, "radar", {"op": "stop"})["ok"]

            def draining():
                value = request(root, "radar", {"op": "status"})
                assert value["state"] in ("Draining", "Idle", "Fault"), value
                return value if value["state"] in ("Idle", "Fault") else None

            stopped = wait_for(draining, 90)
            assert stopped["state"] == "Idle", stopped
            radar.send_signal(signal.SIGTERM)
            assert radar.wait(timeout=30) == 0
            restarted = launch("radar-restarted", radar_command)
            restored = wait_for(lambda: request(root, "radar", {"op": "status"}))
            assert restored["state"] == "Idle" and restored["settings"] == settings
            restarted.send_signal(signal.SIGTERM)
            assert restarted.wait(timeout=30) == 0
            (root / "radar-settings.json").write_text("invalid settings")
            launch("radar-fallback", radar_command)
            fallback = wait_for(lambda: request(root, "radar", {"op": "status"}))
            assert fallback["state"] == "Idle" and fallback["configuration_warning"]
            assert request(root, "radar", {"op": "configure", "settings": settings})["ok"]
            assert not request(root, "radar", {"op": "status"})["configuration_warning"]
            (args.output / "result.json").write_text(json.dumps({"passed": True, "radar": final, "recordings": summaries}, indent=2))
            print("Boot idle, validation, start/stop, recording toggles, shared subscriptions and MCAP verification passed")
        finally:
            for process in reversed(processes):
                if process.poll() is None:
                    process.send_signal(signal.SIGTERM)
            for process in reversed(processes):
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            for log in logs:
                log.close()


if __name__ == "__main__":
    main()
