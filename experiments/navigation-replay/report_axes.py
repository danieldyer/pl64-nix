"""Summarise the independent axis identification and save a comparison figure."""
import argparse
import json
from pathlib import Path
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

p=argparse.ArgumentParser(__doc__)
p.add_argument("data",type=Path)
root=p.parse_args().data.resolve()
out=root/"identification"
ranking=json.loads((out/"hypotheses-ranking.json").read_text())
best=ranking[0]
assert best["label"]=="102pmp" and best["force_sign"]==1
candidate=json.loads((out/"hypothesis-102pmp-force1.json").read_text())
result={"selected_on_middle":best,"runs":[],"paired_block_bootstrap":[]}
rng=np.random.default_rng(20261108)
for tag in ("4859","5395","6036"):
    models=[]
    for path in out.glob("hypothesis-*-force1.json"):
        rows=[r for r in json.loads(path.read_text()) if tag in r["block"]]
        models.append({"model":path.stem,"heldout_score":sum(r["heldout_normalized_sse"] for r in rows),
                       "heldout_rmse_per_component_mps":float(np.sqrt(
                           sum(r["heldout_sse"] for r in rows)/(3*sum(r["heldout_count"] for r in rows))))})
    models.sort(key=lambda r:r["heldout_score"])
    result["runs"].append({"run":tag,"models":models})
for label in ("012ppp","102mmp","102ppm"):
    other=json.loads((out/f"hypothesis-{label}-force1.json").read_text())
    for tag in ("5395","6036"):
        d=np.array([b["heldout_normalized_sse"]-a["heldout_normalized_sse"]
                    for a,b in zip(candidate,other) if tag in a["block"]])
        bootstrap=rng.choice(d,size=(50000,len(d))).sum(axis=1)
        result["paired_block_bootstrap"].append({
            "other":label,"run":tag,"blocks":len(d),"observed_score_difference":float(d.sum()),
            "95_percentile_interval":np.percentile(bootstrap,[2.5,97.5]).tolist(),
            "resampling_fraction_other_no_worse":float(np.mean(bootstrap<=0))})
for key,file in (("independent_integrator_checks","independent-integrator-checks.json"),
                 ("direct_gnss_turns","gnss-selected-turns.json"),
                 ("accelerometer_only_mount","accelerometer-only-mount.json")):
    result[key]=json.loads((out/file).read_text())
result["mounting_context"]={
    "user":"Right-handed casing: +Y mainly down, +Z boresight out left window; hence +X approximately forward.",
    "raw_accel_common_mapping_to_this_body_frame":[[-1,0,0],[0,1,0],[0,0,-1]],
    "gyro_to_raw_accel_frame":[[0,1,0],[-1,0,0],[0,0,1]],
    "composed_gyro_to_this_body_frame":[[0,-1,0],[-1,0,0],[0,0,-1]],
    "qualification":"Discrete signed-axis assignment; not a precision 3-D extrinsic calibration."}
(out/"conclusion.json").write_text(json.dumps(result,indent=2))

plt.rcParams.update({"font.size":10,"axes.spines.top":False,"axes.spines.right":False})
fig,axes=plt.subplots(1,3,figsize=(14,5.3),layout="constrained")
x=np.arange(2)
for label,color,dx,description in (("012ppp","#c8504d",-.18,"Current axes"),
                                    ("102pmp","#168772",.18,"Gyro [y,-x,z]")):
    vals=[]
    for run in result["runs"][1:]:
        row=next(r for r in run["models"] if r["model"]=="hypothesis-"+label+"-force1")
        vals.append(row["heldout_rmse_per_component_mps"])
    axes[0].bar(x+dx,vals,width=.34,color=color,label=description)
axes[0].set_xticks(x,["Middle drive","Long drive"])
axes[0].set_ylabel("Held-out velocity RMSE per component (m/s)")
axes[0].set_title("Independent IMU/GNSS physics fit")
axes[0].legend(fontsize=8)
labels=["102pmp","102mmp","102ppm","012ppp"]
for k,run in enumerate(result["runs"][1:]):
    base=run["models"][0]["heldout_score"]
    values=[next(r for r in run["models"] if r["model"]=="hypothesis-"+n+"-force1")["heldout_score"]/base for n in labels]
    axes[1].plot(np.arange(4),values,"o-",label=["Middle: selection","Long: validation"][k])
axes[1].set_xticks(np.arange(4),["[y,-x,z]","[-y,-x,z]","[y,x,-z]","[x,y,z]"],rotation=20)
axes[1].set_ylabel("Held-out weighted error / best convention")
axes[1].set_title("All 48 permutations/signs tested")
axes[1].legend(fontsize=8)
turns=result["direct_gnss_turns"]["intervals"]
index=np.arange(len(turns))
axes[2].errorbar(index,[t["gnss_turn_deg"] for t in turns],
                  yerr=[t["gnss_sigma_deg"] for t in turns],fmt="o",color="0.3",capsize=3,
                  label="GNSS course change ± reported 1σ")
axes[2].plot(index,[t["identity_turn_deg"] for t in turns],"s",color="#c8504d",label="Current gyro axes")
axes[2].plot(index,[t["candidate_turn_deg"] for t in turns],"^",color="#168772",label="Remapped gyro")
axes[2].set_xticks(index,[str(k+1) for k in index])
axes[2].set_xlabel("Non-overlapping turns selected using GNSS only")
axes[2].set_ylabel("Turn angle (degrees)")
axes[2].set_title("Direct turn-angle check; fixed car mount")
axes[2].legend(fontsize=7,loc="upper center",bbox_to_anchor=(.5,-.2))
for ax in axes:ax.grid(alpha=.15,axis="y")
fig.suptitle("Raw IMU / GNSS axis identification — independent of UKF and readiness flags")
fig.savefig(out/"axis-identification.png",dpi=170)
fig.savefig(out/"axis-identification.pdf")
print(out/"conclusion.json")
print(out/"axis-identification.png")
