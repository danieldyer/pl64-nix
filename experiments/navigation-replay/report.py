"""Summarise real-record replay outputs and save standalone comparison figures."""
import argparse
import collections
import hashlib
import json
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

p = argparse.ArgumentParser(__doc__)
p.add_argument("data", type=Path)
root = p.parse_args().data.resolve()
stages = {"built_in_test": 0, "coarse_alignment": 1, "fine_alignment": 2,
          "navigating": 3, "degraded": 4}
report = {"recordings": [], "replays": [], "holdouts": [], "limitations": [
    "Fresh replay starts after the original service had already begun running.",
    "Stored decoded inputs, rather than serial byte timing, drive the real estimator.",
    "A subscription gap near the end of the longest recording triggers a conservative replay reset.",
    "GNSS holdouts share a receiver and nearby epochs with training observations; no independent truth.",
    "Readiness is conditional on the configured noise model; heading accuracy is not measured.",
    "Gyro remapping requires a controlled physical rotation check before deployment."]}


def records(path):
    return [json.loads(line) for line in path.open()]


for path in sorted((root / "decoded").glob("*.npz")):
    data = np.load(path)
    decoded = json.loads(path.with_suffix(".json").read_text())
    imu, gps = data["imu"], data["gps"]
    gaps = np.flatnonzero(np.diff(imu[:, 0]) > .02)
    valid = gps[:, 15].astype(int) & 1 != 0
    report["recordings"].append({
        "file": path.stem, "imu_samples": len(imu), "gnss_fixes": len(gps),
        "duration_s": float(imu[-1, 0] - imu[0, 0]),
        "gnss_valid_fraction": float(np.mean(valid)),
        "median_velocity_sigma_mps": float(np.median(gps[valid, 12])),
        "imu_flagged_samples": int(np.sum(imu[:, 9] != 0)),
        "recorded_stage_counts": {str(k): int(v) for k, v in
                                  zip(*np.unique(data["state"][:, 3].astype(int) & 255,
                                                 return_counts=True))},
        "recorded_imu_gaps_s": np.diff(imu[:, 0])[gaps].tolist(),
        "decode_error": decoded["error"],
        "subscription_boundaries": [b for b in decoded["metadata"]
                                    if b["source"] == "ins" and b["detail"].get("kind") != "attach"],
    })
for path in sorted((root / "replays").glob("*.jsonl")):
    rows = records(path)
    if not rows or rows[-1]["event"] != "summary":
        continue
    summary = rows[-1]
    align = [r for r in rows if r["event"] == "align"]
    # A delayed correction may revisit an epoch; use the latest diagnostic.
    innovations = {r["time"]: r for r in rows if r["event"] == "innovation"}
    values = [r["nis"] for r in innovations.values() if r["nis"] is not None]
    report["replays"].append({
        "file": path.name, "imu": summary["imu"], "fatal": summary["fatal"],
        "stage_sample_counts": summary["stage_counts"],
        "stage_approx_seconds_at_200hz": {k: v / 200 for k, v in summary["stage_counts"].items()},
        "alignment_reasons": dict(collections.Counter(r["reason"] for r in align)),
        "accepted_alignments": [r for r in align if r["reason"] == "accepted"],
        "innovation_epochs": len(innovations),
        "accepted_innovations": sum(r["accepted"] for r in innovations.values()),
        "nis_percentiles_50_90_99_max": np.percentile(values, [50, 90, 99, 100]).tolist() if values else [],
    })
    for stage in ("fine_alignment", "navigating", "degraded"):
        held = [r for r in rows if r["event"] == "holdout" and r["stage"] == stage]
        if not held:
            continue
        v = np.array([r["velocity_residual"] for r in held])
        report["holdouts"].append({
            "file": path.name, "stage": stage, "count": len(held),
            "velocity_rmse_ned_mps": np.sqrt(np.mean(v * v, axis=0)).tolist(),
            "velocity_rmse_norm_mps": float(np.sqrt(np.mean(np.sum(v * v, axis=1)))),
            "epoch_dt_range_s": [min(r["dt"] for r in held), max(r["dt"] for r in held)],
        })
report["original_sha256"] = {}
for path in sorted((root / "originals").glob("*.mcap")):
    sha = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1024 * 1024):
            sha.update(block)
    report["original_sha256"][path.name] = sha.hexdigest()
if (root / "axis-closure.json").exists():
    report["gravity_closure"] = json.loads((root / "axis-closure.json").read_text())
if (root / "raw-audit.json").exists():
    report["raw_imu_audit"] = json.loads((root / "raw-audit.json").read_text())
report["verification"] = {}
for name in ("parity", "start-sensitivity"):
    path = root / "verification" / (name + ".json")
    if path.exists():
        report["verification"][name] = json.loads(path.read_text())
(root / "results.json").write_text(json.dumps(report, indent=2))

plt.rcParams.update({"font.size": 10, "axes.spines.top": False, "axes.spines.right": False})
fig, axes = plt.subplots(3, 2, figsize=(13, 9), sharex="col", layout="constrained")
datasets = [v for v in report["recordings"] if not v["file"].endswith("partial")][1:3]
colors = {"baseline": "#c8504d", "gyro_frame_hist60_calibrated": "#168772"}
for col, record in enumerate(datasets):
    name = record["file"]
    raw = np.load(root / "decoded" / (name + ".npz"))
    start = raw["imu"][0, 0]
    s = raw["state"]
    axes[0, col].plot((s[:, 0] - start) / 60, s[:, 3].astype(int) & 255,
                      color="0.6", linewidth=1.8, label="Recorded live state")
    for variant, color in colors.items():
        rows = records(root / "replays" / (name + "-" + variant + ".jsonl"))
        states = [r for r in rows if r["event"] == "state"]
        t = np.array([r["time"] - start for r in states]) / 60
        label = "Unchanged replay" if variant == "baseline" else "Gyro frame + 60 s history"
        axes[0, col].step(t, [stages[r["stage"]] for r in states], where="post",
                          color=color, linewidth=1, label=label)
        tracked = [r for r in states if r["stage"] in ("fine_alignment", "navigating", "degraded")
                   and r["heading_sigma_deg"] < 100]
        margin = [max(r["heading_sigma_deg"] / 3, r["tilt_sigma_deg"],
                      r["velocity_sigma_norm"] / .3) for r in tracked]
        axes[2, col].plot([(r["time"] - start) / 60 for r in tracked], margin, color=color)
        held = records(root / "replays" / (name + "-" + variant + "-holdout5.jsonl"))
        held = [r for r in held if r["event"] == "holdout"]
        axes[1, col].scatter([(r["time"] - start) / 60 for r in held],
                             [np.linalg.norm(r["velocity_residual"]) for r in held],
                             color=color, s=3, alpha=.5)
    axes[0, col].set_title("Middle drive (440 s)" if col == 0 else "Long drive (804 s)")
    axes[0, col].set_yticks(list(stages.values()), ["BIT", "Coarse", "Fine", "Green", "Degraded"])
    axes[0, col].set_ylim(-.25, 4.25)
    axes[1, col].set_yscale("log")
    axes[1, col].set_ylim(.1, 10000)
    axes[1, col].set_ylabel("Held-out GNSS velocity error (m/s)")
    axes[2, col].set_yscale("log")
    axes[2, col].axhline(1, color="0.4", linestyle="--", linewidth=1)
    axes[2, col].set_ylabel("Worst uncertainty / readiness limit")
    axes[2, col].set_xlabel("Minutes since recording began")
    for row in range(3):
        axes[row, col].grid(alpha=.18)
    gaps = np.flatnonzero(np.diff(raw["imu"][:, 0]) > .02)
    for gap in gaps:
        for row in range(3):
            axes[row, col].axvspan((raw["imu"][gap, 0] - start) / 60,
                                   (raw["imu"][-1, 0] - start) / 60, color="0.5", alpha=.12)
axes[0, 0].legend(loc="upper left", fontsize=8)
fig.suptitle("Real production estimator replay; original gates and readiness limits\n"
             "Held-out fixes assess consistency, not independent truth. Grey tail follows recording loss.")
fig.savefig(root / "replay-comparison.png", dpi=160)
fig.savefig(root / "replay-comparison.pdf")
print(root / "results.json")
print(root / "replay-comparison.png")
