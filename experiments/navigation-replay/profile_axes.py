import sys,json,numpy as np,concurrent.futures
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parent));from identify_axes import blocks,Kernel,CANDIDATE
R=Path(sys.argv[1]).resolve();O=R/'identification'
def run(spec):
 label,m,lag,gp,ap,lever=spec;bs=blocks(R,Kernel(sys.argv[2]));rows=[]
 for b in bs:
  fit=b.fit(m,lag=lag,gyro_prior=gp,accel_prior=ap,lever=lever);rows.append(fit)
 n=f'{label}-lag{lag}-g{gp}-a{ap}-lever{int(lever)}';(O/(n+'.json')).write_text(json.dumps(rows,indent=2));print(n,sum(x['heldout_normalized_sse'] for x in rows if '5395' in x['block']),flush=True);return {'name':n,'rows':rows}
spec=[(n,m,lag,gp,ap,lever) for n,m in [('identity',np.eye(3)),('candidate',CANDIDATE)] for lag in [-.5,0,.5] for gp,ap,lever in [(.3,.5,True),(3,3,True)]]
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:result=list(pool.map(run,spec))
(O/'nuisance-profiles.json').write_text(json.dumps(result,indent=2))
