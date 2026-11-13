"""Fit a general SO(3) gyro-to-accelerometer rotation using independent physics."""
import argparse
import json
from pathlib import Path
import time
import numpy as np
from scipy.optimize import least_squares
from scipy.sparse import lil_matrix
from scipy.spatial.transform import Rotation
from identify_axes import Kernel, blocks, CANDIDATE, DEG

p = argparse.ArgumentParser(__doc__)
p.add_argument("data", type=Path)
p.add_argument("--library", type=Path, required=True)
p.add_argument("--run", choices=("short", "middle", "long"), default="middle")
p.add_argument("--gyro-prior", type=float, default=.3)
p.add_argument("--accel-prior", type=float, default=.5)
p.add_argument("--lever", action="store_true")
args = p.parse_args()
root = args.data.resolve()
out = root / "identification"
kernel = Kernel(args.library)
run_id = {"short":"4859", "middle":"5395", "long":"6036"}[args.run]
bs = [b for b in blocks(root, kernel) if run_id in b.name]
initials = {r["block"]:r["parameters"] for r in json.loads((out/"candidate.json").read_text())}
count = 12 if args.lever else 9
prior_count = count-3
x0 = np.r_[np.zeros(3), np.concatenate([np.r_[initials[b.name], np.zeros(3)]
                                       if args.lever else initials[b.name] for b in bs])]
sizes = [3*b.train.sum()+prior_count for b in bs]
offsets = np.r_[0, np.cumsum(sizes)]
sparsity = lil_matrix((offsets[-1], len(x0)), dtype=int)
for i,b in enumerate(bs):
    sparsity[offsets[i]:offsets[i+1], :3] = 1
    sparsity[offsets[i]:offsets[i+1], 3+count*i:3+count*(i+1)] = 1


def residual(x):
    m = Rotation.from_rotvec(x[:3]).as_matrix() @ CANDIDATE
    return np.concatenate([b.residual(x[3+count*i:3+count*(i+1)],m,
                                      gyro_prior=args.gyro_prior,accel_prior=args.accel_prior)
                           for i,b in enumerate(bs)])


start = time.time()
fit = least_squares(residual,x0,jac_sparsity=sparsity.tocsr(),
                    x_scale=np.r_[np.ones(3)*.1,np.tile(np.r_[np.ones(3),np.ones(3)*.3,
                                                            np.ones(3)*.5,
                                                            np.ones(3) if args.lever else []],len(bs))],
                    loss="soft_l1",f_scale=2,max_nfev=160,ftol=1e-8,xtol=1e-8,gtol=1e-6)
m = Rotation.from_rotvec(fit.x[:3]).as_matrix() @ CANDIDATE
info = np.zeros((3,3))
scores = []
for i,b in enumerate(bs):
    x = fit.x[3+count*i:3+count*(i+1)]
    r = b.residual(x,m,gyro_prior=args.gyro_prior,accel_prior=args.accel_prior)
    # Independently relinearise and eliminate each block's nine nuisance parameters.
    jm = np.empty((len(r),3));jn = np.empty((len(r),count))
    for k in range(3):
        step=np.eye(3)[k]*1e-4
        jm[:,k]=(b.residual(x,Rotation.from_rotvec(step).as_matrix()@m,
                           gyro_prior=args.gyro_prior,accel_prior=args.accel_prior)-
                 b.residual(x,Rotation.from_rotvec(-step).as_matrix()@m,
                           gyro_prior=args.gyro_prior,accel_prior=args.accel_prior))/(2e-4)
    for k in range(count):
        step=np.eye(count)[k]*1e-4
        jn[:,k]=(b.residual(x+step,m,gyro_prior=args.gyro_prior,accel_prior=args.accel_prior)-
                 b.residual(x-step,m,gyro_prior=args.gyro_prior,accel_prior=args.accel_prior))/(2e-4)
    for use_priors in [True,False]:
        a,j,res=(jm,jn,r) if use_priors else (jm[:-prior_count],jn[:-prior_count],r[:-prior_count])
        projected=a-j@np.linalg.lstsq(j,a,rcond=1e-9)[0]
        ib=projected.T@projected
        sb=projected.T@res
        if use_priors:
            info+=ib;scores.append((ib,sb))
        else:
            if i==0: info_data=np.zeros((3,3))
            info_data+=ib
cov=np.linalg.inv(info)
robust=cov@sum(np.outer(s,s) for ib,s in scores)@cov*len(bs)/max(1,len(bs)-3)
result={"run":args.run,"blocks":len(bs),"gyro_prior_deg_s":args.gyro_prior,
        "accel_prior_mps2":args.accel_prior,"success":bool(fit.success),
        "message":fit.message,"evaluations":fit.nfev,"cost":float(2*fit.cost),
        "seconds":time.time()-start,"matrix":m.tolist(),
        "delta_from_candidate_rotvec_deg":(fit.x[:3]/DEG).tolist(),
        "rotation_angle_deg":float(np.linalg.norm(Rotation.from_matrix(m).as_rotvec())/DEG),
        "model_sigma_deg":(np.sqrt(np.diag(cov))/DEG).tolist(),
        "block_sandwich_sigma_deg":(np.sqrt(np.diag(robust))/DEG).tolist(),
        "information_eigenvalues":np.linalg.eigvalsh(info).tolist(),
        "information_eigenvectors":np.linalg.eigh(info)[1].tolist(),
        "information_eigenvalues_without_bias_priors":np.linalg.eigvalsh(info_data).tolist(),
        "parameters":fit.x.tolist()}
name=args.run+f"-calibration-g{args.gyro_prior}-a{args.accel_prior}"+('-lever' if args.lever else '')+'.json'
(out/name).write_text(json.dumps(result,indent=2))
print(json.dumps({k:v for k,v in result.items() if k!="parameters"},indent=2),flush=True)
