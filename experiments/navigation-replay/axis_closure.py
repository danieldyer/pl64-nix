from pathlib import Path
import numpy as np,json,itertools,sys
from scipy.spatial.transform import Rotation
R=Path(sys.argv[1]).resolve()
maps=[]
for perm in itertools.permutations(range(3)):
 for signs in itertools.product([-1,1],repeat=3):
  m=np.zeros((3,3));m[np.arange(3),perm]=signs;maps.append(('map'+''.join(str(i) for i in perm)+''.join('p' if s>0 else 'm' for s in signs),m))
result=[]
for p in sorted((R/'decoded').glob('*.npz'))[:3]:
 d=np.load(p);im=d['imu'];t=im[:,0];a=im[:,6:9];w=im[:,3:6];out={n:[] for n,m in maps}
 for k in np.arange(t[0]+3,t[-1]-9,4):
  left=(t>=k-1)&(t<k+1);right=(t>=k+5)&(t<k+7);b=(t>=k)&(t<=k+6)
  if left.sum()<350 or right.sum()<350 or b.sum()<1100:continue
  fa=np.mean(a[left],0);fb=np.mean(a[right],0)
  if abs(np.linalg.norm(fa)-9.81)>.5 or abs(np.linalg.norm(fb)-9.81)>.5:continue
  qp=Rotation.identity();qm=Rotation.identity();ix=np.flatnonzero(b)
  for n,m in zip(ix[:-1],ix[1:]):
   dr=(w[n]+w[m])*.5*(t[m]-t[n]);qp=qp*Rotation.from_rotvec(dr);qm=qm*Rotation.from_rotvec(-dr)
  if np.linalg.norm(qp.as_rotvec())*180/np.pi<10:continue
  for name,m in maps:
   q=m@(qp if np.linalg.det(m)>0 else qm).as_matrix()@m.T
   pred=q.T@fa;angle=np.rad2deg(np.arccos(np.clip(pred@fb/np.linalg.norm(pred)/np.linalg.norm(fb),-1,1)));out[name].append(angle)
 rows=[{'mapping':n,'matrix':m.ravel().tolist(),'rms_deg':float(np.sqrt(np.mean(np.array(out[n])**2))),'median_deg':float(np.median(out[n])),'max_deg':float(np.max(out[n])),'n':len(out[n])} for n,m in maps]
 result.append({'file':p.name,'results':rows});print(p.stem,sorted(rows,key=lambda x:x['rms_deg'])[:4],flush=True)
(R/'axis-closure.json').write_text(json.dumps(result,indent=2))
