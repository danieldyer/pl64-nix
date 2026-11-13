"""Match the corrected production decoder against retained legacy IMU50 bytes."""
import argparse
import collections
import json
from pathlib import Path
import subprocess

import numpy as np

p = argparse.ArgumentParser(__doc__)
p.add_argument("data", type=Path)
p.add_argument("binary", type=Path)
p.add_argument("--output", type=Path)
args = p.parse_args()
root = args.data.resolve()
output = args.output or root / "identification"
output.mkdir(parents=True, exist_ok=True)
dtype = np.dtype([("counter", "<u8"), ("measurement", "<i8"), ("arrival", "<i8"),
                  ("values", "<f8", (6,)), ("flags", "<u4")])
rows = []
for path in sorted((root / "decoded").glob("*.wire")):
    destination = output / (path.stem + "-production-interface.bin")
    proc = subprocess.run([str(args.binary.resolve()), str(path), str(destination)],
                          capture_output=True, text=True, check=True)
    actual = np.fromfile(destination, dtype=dtype)
    old = np.load(path.with_suffix(".npz"))["imu"]
    # These 8 November decoded records predate intrinsic gyro normalization.
    expected = collections.Counter((x[4], -x[3], x[5], *x[6:9], int(x[10])) for x in old)
    observed = collections.Counter((*x["values"], int(x["counter"])) for x in actual)
    missing = sum((expected - observed).values())
    assert missing == 0, path
    row = {"file": path.name, "recorded_decoded_samples": len(old),
           "production_acquisition_samples": len(actual),
           "recorded_samples_without_exact_production_match": missing,
           "expected_gyro_normalization": "[packet_y,-packet_x,packet_z]",
           **json.loads(proc.stdout)}
    rows.append(row)
    print(row, flush=True)
(output / "production-interface-check.json").write_text(json.dumps(rows, indent=2))
