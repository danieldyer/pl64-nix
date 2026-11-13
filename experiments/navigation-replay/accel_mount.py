import sys
import numpy as np,json
from pathlib import Path
from scipy.signal import savgol_filter
from scipy.spatial.transform import Rotation
R=Path(sys.argv[1]).resolve();out=[]
for p in sorted((R/'decoded').glob('*.npz'))[:3]:
 d=np.load(p);g=d['gps'];im=d['imu'];v=savgol_filter(g[:,6:9],51,3,axis=0);a=savgol_filter(g[:,6:9],51,3,deriv=1,delta=.1,axis=0);speed=np.linalg.norm(v[:,:2],axis=1);forward=np.c_[v[:,:2]/np.maximum(speed[:,None],1e-8),np.zeros(len(v))];right=np.c_[-forward[:,1],forward[:,0],np.zeros(len(v))];reference=np.c_[np.sum(a*forward,axis=1),np.sum(a*right,axis=1),a[:,2]-9.798];f=np.column_stack([np.interp(g[:,0],im[:,0],im[:,k]) for k in range(6,9)]);f=savgol_filter(f,51,3,axis=0);good=((g[:,15].astype(int)&1)!=0)&(speed>5)&(abs(v[:,2])<3)&(g[:,12]<2.8);good[:30]=False;good[-30:]=False;w=1/g[good,12]**2;H=(reference[good]*w[:,None]).T@f[good];u,s,vt=np.linalg.svd(H);S=np.eye(3);S[2,2]=np.linalg.det(u@vt);C=u@S@vt;pred=f[good]@C.T;err=pred-reference[good];row={'file':p.name,'matrix_raw_accel_to_car':C.tolist(),'car_forward_axis_in_raw_accel':C[0].tolist(),'car_down_axis_in_raw_accel':C[2].tolist(),'raw_X_heading_in_car_deg':float(np.rad2deg(np.arctan2(C[1,0],C[0,0]))),'rms_error_ms2':np.sqrt(np.mean(err**2,axis=0)).tolist(),'samples':int(good.sum()),'singular_values':s.tolist()};out.append(row);print(row)
(R/'identification'/'accelerometer-only-mount.json').write_text(json.dumps(out,indent=2))
