from pathlib import Path
import numpy as np,json,itertools,sys
from scipy.spatial.transform import Rotation
R=Path(sys.argv[1]).resolve()
res=[]
for p in sorted((R/'decoded').glob('*.npz'))[:3]:
 d=np.load(p); im=d['imu'];g=d['gps'];t=im[:,0];a=im[:,6:9];w=im[:,3:6]; rows=[]
 # Local gravity closure: 2 s endpoint means around 6 s gyro propagation.
 # Choose endpoints close to g, weak endpoint force change, and >10 degree rotation.
 for k in np.arange(t[0]+3,t[-1]-9,4):
  left=(t>=k-1)&(t<k+1);right=(t>=k+5)&(t<k+7);b=(t>=k)&(t<=k+6)
  if left.sum()<350 or right.sum()<350 or b.sum()<1100:continue
  fa=np.mean(a[left],0);fb=np.mean(a[right],0)
  if abs(np.linalg.norm(fa)-9.81)>.5 or abs(np.linalg.norm(fb)-9.81)>.5:continue
  q=Rotation.identity();ix=np.flatnonzero(b)
  for n,m in zip(ix[:-1],ix[1:]):q=q*Rotation.from_rotvec((w[n]+w[m])*.5*(t[m]-t[n]))
  pred=q.inv().apply(fa);angle=np.rad2deg(np.arccos(np.clip(pred@fb/np.linalg.norm(pred)/np.linalg.norm(fb),-1,1)));rotation=np.linalg.norm(q.as_rotvec())*180/np.pi
  if rotation>10:rows.append({'start_s':float(k-t[0]),'end_s':float(k+6-t[0]),'gyro_rotation_deg':float(rotation),'gravity_closure_deg':float(angle),'force_endpoint_angle_deg':float(np.rad2deg(np.arccos(np.clip(fa@fb/np.linalg.norm(fa)/np.linalg.norm(fb),-1,1))))})
 res.append({'file':p.name,'intervals':rows});print(p.name,'count',len(rows),'worst',sorted(rows,key=lambda x:x['gravity_closure_deg'],reverse=True)[:3])
(R/'gravity-closure.json').write_text(json.dumps(res,indent=2))
