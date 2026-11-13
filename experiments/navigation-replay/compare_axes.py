import sys,json,itertools,concurrent.futures,numpy as np,time
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent))
from identify_axes import blocks,Kernel,CANDIDATE
R=Path(sys.argv[1]).resolve();O=R/'identification';K=Kernel(sys.argv[2])
# Each worker owns its block objects and mutable integration cache.
def run(spec):
 label,m,sgn=spec;bs=blocks(R,K);rows=[]
 for b in bs:
  x=b.fit(m,accel_sign=sgn);rows.append(x)
 (O/f'hypothesis-{label}-force{sgn}.json').write_text(json.dumps(rows,indent=2))
 a=[x for x in rows if '5395' in x['block']];score=sum(x['heldout_normalized_sse'] for x in a);print(label,sgn,round(score,3),flush=True);return {'label':label,'matrix':m.tolist(),'force_sign':sgn,'middle_heldout_score':score,'middle_count':sum(x['heldout_count'] for x in a)}
spec=[]
for perm in itertools.permutations(range(3)):
 for signs in itertools.product([-1,1],repeat=3):
  m=np.zeros((3,3));m[np.arange(3),perm]=signs;label=''.join(map(str,perm))+''.join('p' if v>0 else 'm' for v in signs)
  spec.append((label,m,1))
# Test a force-sign error separately using both main gyro conventions and all
# alternatives suggested by the initial zero-bias screen.
for row in json.loads((O/'screen.json').read_text())[:8]+[{'label':'012ppp','matrix':np.eye(3).tolist()}]:spec.append((row['label'],np.array(row['matrix']),-1))
with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:res=list(pool.map(run,spec))
res.sort(key=lambda x:x['middle_heldout_score']);(O/'hypotheses-ranking.json').write_text(json.dumps(res,indent=2));print('BEST',json.dumps(res[:6]),flush=True)
