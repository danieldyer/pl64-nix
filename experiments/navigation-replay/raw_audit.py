"""Independently check recorded IMU values against checksum-valid serial frames."""
import argparse
import collections
import json
from pathlib import Path
import struct
import numpy as np

p = argparse.ArgumentParser(__doc__)
p.add_argument("data", type=Path)
root = p.parse_args().data.resolve()
header = struct.Struct("<BqqqdQI")
result = []
for path in sorted((root / "decoded").glob("*.wire")):
    frames = collections.Counter()
    buffer = bytearray()
    bad = 0
    with path.open("rb") as stream:
        while h := stream.read(header.size):
            kind, arrival, utc, measurement, uncertainty, sequence, size = header.unpack(h)
            payload = stream.read(size)
            if kind == 254:
                buffer.clear()
                continue
            if kind != 0:
                continue
            buffer.extend(payload)
            while len(buffer) >= 2:
                if buffer[0] != 0x77 or not 4 <= buffer[1] <= 128:
                    del buffer[0]
                    continue
                size = buffer[1] + 1
                if len(buffer) < size:
                    break
                frame = bytes(buffer[:size])
                del buffer[:size]
                if sum(frame[1:-1]) % 256 != frame[-1]:
                    bad += 1
                    continue
                if size == 33 and frame[3] == 0x70:
                    values = np.array(struct.unpack_from("<6f", frame, 4))
                    values *= np.array([np.pi / 180] * 3 + [9.80665] * 3)
                    counter = int(f"{frame[28]:02x}{frame[29]:02x}")
                    frames[(*values, counter)] += 1
    decoded = np.load(path.with_suffix(".npz"))["imu"]
    stored = collections.Counter((*r[3:9], int(r[10])) for r in decoded)
    # Individual raw and decoded messages can be lost at subscription boundaries.
    # Compare multisets including native counters; don't invent missing samples.
    result.append({
        "file": path.name, "raw_frames": sum(frames.values()),
        "decoded_samples": len(decoded), "bad_frame_checksums": bad,
        "decoded_samples_without_bit_exact_raw_match": sum((stored - frames).values()),
        "raw_frames_without_recorded_decoded_sample": sum((frames - stored).values()),
    })
(root / "raw-audit.json").write_text(json.dumps(result, indent=2))
print(json.dumps(result, indent=2))
