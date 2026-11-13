#!/usr/bin/env python3
"""Exercise the real recorder with a mock radar controller and synthetic navigation."""
import json
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time


def request(root, service, value):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(3)
        sock.connect(str(root / f"pl64-{service}/control.sock"))
        sock.sendall(json.dumps(value).encode() + b"\n")
        data = b""
        while b"\n" not in data:
            data += sock.recv(65536)
        return json.loads(data)


def wait(predicate):
    until = time.monotonic() + 8
    while time.monotonic() < until:
        try:
            value = predicate()
            if value:
                return value
        except (OSError, KeyError):
            pass
        time.sleep(.02)
    raise AssertionError("Timed out waiting for recorder")


def main():
    recorder, source, schema, checker = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="pl64-control-") as tmp:
        root = pathlib.Path(tmp)
        directory = root / "sessions"
        radar = {"ok": True, "state": "Idle", "settings": {"test_preset": "lower VHF"}}
        starts = []
        reject = [False]
        (root / "pl64-radar").mkdir()
        server = socket.socket(socket.AF_UNIX)
        server.bind(str(root / "pl64-radar/control.sock"))
        server.listen()

        def serve():
            while True:
                try:
                    connection, _ = server.accept()
                except OSError:
                    return
                with connection:
                    data = b""
                    while b"\n" not in data:
                        data += connection.recv(65536)
                    value = json.loads(data)
                    reply = dict(radar)
                    if value["op"] == "start":
                        starts.append(True)
                        if reject[0]:
                            reply = {"ok": False, "error": "Radio is busy"}
                        else:
                            radar["state"] = "Running"
                            reply = dict(radar)
                    connection.sendall(json.dumps(reply).encode() + b"\n")

        threading.Thread(target=serve, daemon=True).start()
        processes = []
        try:
            processes.append(subprocess.Popen([source, str(root), schema]))
            processes.append(subprocess.Popen([recorder, "--runtime-dir", str(root), "--directory", str(directory)]))
            status = lambda: request(root, "recorder", {"op": "status"})
            wait(status)
            assert not status()["enabled"]
            wait(lambda: status()["sources"].get("ins", {}).get("records", 0) > 10)
            assert not list(directory.glob("*.mcap")), "Boot must not record"
            nav_before = status()["sources"]["ins"]["records"]
            # Starting capture auto-starts only DVB-T; navigation remains alive.
            assert request(root, "recorder", {"op": "start"})["ok"]
            wait(lambda: status()["state"] == "Recording")
            first = status()["file"]
            assert radar["state"] == "Running" and len(starts) == 1
            assert request(root, "recorder", {"op": "start"})["ok"]
            assert status()["file"] == first and len(starts) == 1
            assert request(root, "recorder", {"op": "stop"})["ok"]
            wait(lambda: status()["state"] == "Idle")
            assert radar["state"] == "Running"
            wait(lambda: status()["sources"]["ins"]["records"] > nav_before + 30)
            before = sorted(directory.glob("*.mcap"))
            time.sleep(.15)
            assert before == sorted(directory.glob("*.mcap")), "Preview must not write files"
            # Late recording takes a fresh snapshot without restarting acquisition.
            radar["settings"] = {"test_preset": "upper VHF", "session_gain": 3}
            assert request(root, "recorder", {"op": "start"})["ok"]
            wait(lambda: status()["state"] == "Recording")
            assert status()["file"] != first and len(starts) == 1
            request(root, "recorder", {"op": "stop"})
            wait(lambda: status()["state"] == "Idle")
            summaries = [json.loads(subprocess.check_output([checker, str(path)]))
                         for path in sorted(directory.glob("*.mcap"))]
            snapshots = [json.loads(b["metadata"]["detail"])
                         for summary in summaries for b in summary["boundaries"]
                         if json.loads(b["metadata"]["detail"]).get("kind") == "session_start"]
            assert len(snapshots) == 2
            assert snapshots[0]["radar"]["settings"]["test_preset"] == "lower VHF"
            assert snapshots[1]["radar"]["settings"]["session_gain"] == 3
            assert all(s["topics"]["/navigation/events"] > 0 for s in summaries)
            assert not list(directory.glob("*.partial.mcap"))
            radar["state"] = "Draining"
            assert not request(root, "recorder", {"op": "start"})["ok"] and not status()["enabled"]
            radar["state"] = "Idle"
            reject[0] = True
            assert not request(root, "recorder", {"op": "start"})["ok"]
            wait(lambda: status()["state"] == "Idle")
            assert not status()["enabled"]
            assert all(p.poll() is None for p in processes)
            print("PASS boot idle, pipeline-only preview, automatic DVB-T start, late recording, idempotence, snapshots, navigation continuity and start failure")
        finally:
            for process in reversed(processes):
                process.terminate()
                process.wait(timeout=10)
            server.close()


if __name__ == "__main__":
    main()
