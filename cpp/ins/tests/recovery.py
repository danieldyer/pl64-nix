#!/usr/bin/env python3
"""Only a GNSS transport fault may request receiver reconfiguration."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile


with tempfile.TemporaryDirectory(prefix="ins-recovery-") as directory:
    root = Path(directory)
    systemctl = root / "systemctl"
    systemctl.write_text('''#!/bin/sh
printf '%s\\n' "$*" >> "$CALLS"
if [ "$1" = is-active ]; then
    [ "$GPS_STATE" = active ]
else
    [ "$*" = '--no-block restart gpsd.service' ]
fi
''')
    systemctl.chmod(0o755)
    for result, status in [("success", "0"), ("exit-code", "1"),
                           ("signal", "TERM"), ("exit-code", "75")]:
        for state in ["active", "inactive", "activating", "failed"]:
            calls = root / "calls"
            calls.write_text("")
            environment = dict(os.environ, PATH=f"{root}:{os.environ['PATH']}",
                               CALLS=str(calls), GPS_STATE=state,
                               SERVICE_RESULT=result, EXIT_STATUS=status)
            subprocess.run(["bash", sys.argv[1]], env=environment, check=True)
            commands = calls.read_text().splitlines()
            expected = []
            if (result, status) == ("exit-code", "75"):
                expected = ["is-active --quiet gpsd.service"]
                if state == "active":
                    expected.append("--no-block restart gpsd.service")
            assert commands == expected, (result, status, state, commands)
print("GPS recovery hook: fault-specific recovery and absent/configuring guards passed")
