"""Check instrumentation parity and sensitivity to the start of recording."""
import argparse
import concurrent.futures
import json
from pathlib import Path
import struct
import subprocess

p = argparse.ArgumentParser(__doc__)
p.add_argument("data", type=Path)
p.add_argument("--unmodified", type=Path, required=True)
p.add_argument("--instrumented", type=Path, required=True)
p.add_argument("--offsets", type=int, nargs="*", default=[])
args = p.parse_args()
root = args.data.resolve()
out = root / "verification"
out.mkdir(exist_ok=True)
header = struct.Struct("<BqqIQ")


def execute(binary, events, settings, output):
    subprocess.run([str(binary.resolve()), str(events), str(settings), str(output)],
                   check=True, capture_output=True, text=True)
    return [json.loads(line) for line in output.open()]


def parity(events):
    settings = events.with_suffix(".config.json")
    a = execute(args.unmodified, events, settings, out / (events.stem + "-unmodified.jsonl"))
    b = execute(args.instrumented, events, settings, out / (events.stem + "-instrumented.jsonl"))
    b = [r for r in b if r["event"] in ("reset", "state", "summary")]
    assert a == b, events
    return {"file": events.name, "uninstrumented_matches_instrumented_default_exactly": True,
            "compared_records": len(a)}


def shifted(item):
    events, offset = item
    target = out / (events.stem + "-offset" + str(offset) + ".events")
    first = None
    with events.open("rb") as source, target.open("wb") as dest:
        dest.write(header.pack(254, 0, 0, 0, 0))
        while h := source.read(header.size):
            kind, epoch, arrival, flags, counter = header.unpack(h)
            values = source.read(48 if kind == 1 else 96 if kind == 2 else 0)
            if kind == 254:
                dest.write(h)
                continue
            if first is None:
                first = arrival
            if arrival - first >= offset * 1e9:
                dest.write(h + values)
    settings = root / "configs" / (events.stem + "-gyro_frame_hist60_calibrated.json")
    rows = execute(args.instrumented, target, settings, target.with_suffix(".jsonl"))
    return {"file": events.name, "start_offset_s": offset, **rows[-1]}


events = sorted((root / "decoded").glob("*.events"))
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
    result = list(pool.map(parity, events))
    (out / "parity.json").write_text(json.dumps(result, indent=2))
    if args.offsets:
        result = list(pool.map(shifted, [(e, offset) for e in events for offset in args.offsets]))
        (out / "start-sensitivity.json").write_text(json.dumps(result, indent=2))
print(out)
