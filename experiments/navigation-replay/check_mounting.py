import sys,json,numpy as np
from pathlib import Path
from scipy.spatial.transform import Rotation
sys.path.insert(0,str(Path(__file__).resolve().parent));from identify_axes import Kernel,blocks,CANDIDATE,DEG
R=Path(sys.argv[1]).resolve();O=R/'identification';bs=blocks(R,Kernel(sys.argv[2]));out={}
for name,m,rows in [('candidate',CANDIDATE,json.loads((O/'candidate.json').read_text())),('identity',np.eye(3),json.loads((O/'identity.json').read_text()))]:
 lookup={x['block']:x['parameters'] for x in rows};values=[]
 for b in bs:
  x=np.array(lookup[b.name]);b.integrals(m,x[3:6]);f=b.body_rotation[:,:,0]@Rotation.from_rotvec(x[:3]).as_matrix().T;f=Rotation.from_rotvec(-(b.epoch-b.epoch[0])[:,None]*b.omega).apply(f);v=b.velocity;speed=np.linalg.norm(v[:,:2],axis=1);good=speed>5;delta=np.arctan2(np.sin(np.arctan2(f[:,1],f[:,0])-np.arctan2(v[:,1],v[:,0])),np.cos(np.arctan2(f[:,1],f[:,0])-np.arctan2(v[:,1],v[:,0])))/DEG
  if good.any():values.append({'block':b.name,'median_forward_course_offset_deg':float(np.median(delta[good])),'median_absolute_offset_deg':float(np.median(abs(delta[good]))),'count':int(good.sum())})
 out[name]=values
 print(name,[(x['block'].split('682-')[-1],round(x['median_forward_course_offset_deg'],1)) for x in values if '5395' in x['block']]);print('medianabsolute',np.median([x['median_absolute_offset_deg'] for x in values]))
(O/'mounting-check.json').write_text(json.dumps(out,indent=2))
