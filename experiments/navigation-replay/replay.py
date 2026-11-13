"""Run named, isolated counterfactuals without changing production settings."""
import argparse
import concurrent.futures
import itertools
import json
from pathlib import Path
import subprocess

p = argparse.ArgumentParser(__doc__)
p.add_argument("data", type=Path)
p.add_argument("--binary", type=Path, required=True)
p.add_argument("--jobs", type=int, default=4)
p.add_argument("--holdout", type=int, default=0, help="Withhold every Nth valid fix after alignment")
p.add_argument("--axis-sweep", action="store_true", help="Explore all 48 signed gyro permutations")
p.add_argument("--variants", nargs="+")
args = p.parse_args()
root = args.data.resolve()
variants = {
    "baseline": {},
    "history30": {"analysis_history_s": 30},
    "history60": {"analysis_history_s": 60},
    "gyro_bias1deg": {"gyro_initial_sigma_rad_s": 0.017453292519943295},
    "accel_bias05": {"accel_initial_sigma_mps2": 0.5},
    "bias_walk100": {"gyro_bias_walk": [2.42406840554768e-5] * 3,
                     "accel_bias_walk": [0.002] * 3},
    "gnss_sigma2": {"analysis_velocity_sigma_scale": 2},
    "imu_delay_plus100ms": {"imu_delay_s": 0.101},
    "imu_delay_minus100ms": {"imu_delay_s": -0.099},
    "imu_noise10": {
        "gyro_noise_rad_sqrt_s": [0.0024900030661785766, 0.0017220581953010717,
                                 0.0020303996964867366],
        "accel_noise_m_s_sqrt_s": [0.01415, 0.014616666666666667, 0.014833333333333332]},
}
for history in (12, 20, 30, 60):
    variants["gyro_frame_hist" + str(history)] = {
        "analysis_history_s": history,
        "analysis_gyro_matrix": [0, 1, 0, -1, 0, 0, 0, 0, 1]}
variants["gyro_frame_hist60_calibrated"] = {
    **variants["gyro_frame_hist60"],
    # ARW was measured in the raw gyro axes; rotate its diagonal with the rates.
    "gyro_noise_rad_sqrt_s": [0.00017220581953010717, 0.00024900030661785766,
                             0.00020303996964867366]}
if args.axis_sweep:
    variants = {}
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product((-1, 1), repeat=3):
            matrix = [0] * 9
            for row in range(3):
                matrix[row * 3 + perm[row]] = signs[row]
            name = "map" + "".join(map(str, perm)) + "".join("p" if v > 0 else "m" for v in signs)
            variants[name] = {"analysis_gyro_matrix": matrix}
if args.variants:
    variants = {name: variants[name] for name in args.variants}
for directory in ("configs", "replays"):
    (root / directory).mkdir(exist_ok=True)


def run(item):
    events, name, change = item
    config = json.loads(events.with_suffix(".config.json").read_text())
    config.update(change)
    settings = root / "configs" / (events.stem + "-" + name + ".json")
    settings.write_text(json.dumps(config, indent=2))
    suffix = "-holdout" + str(args.holdout) if args.holdout else ""
    output = root / "replays" / (events.stem + "-" + name + suffix + ".jsonl")
    result = subprocess.run([str(args.binary.resolve()), str(events), str(settings),
                             str(output), str(args.holdout)], capture_output=True, text=True)
    print(name, events.stem, result.returncode, result.stdout.strip(), result.stderr, flush=True)
    return result.returncode


with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    codes = list(pool.map(run, [(events, name, change)
                                for events in sorted((root / "decoded").glob("*.events"))
                                for name, change in variants.items()]))
raise SystemExit(int(any(codes)))
