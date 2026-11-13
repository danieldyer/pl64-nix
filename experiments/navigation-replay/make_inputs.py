from pathlib import Path
import numpy as np,json,struct,sys
root=Path(sys.argv[1]).resolve()/'decoded'
for p in sorted(root.glob('*.npz')):
 z=np.load(p);i=z['imu'];g=z['gps'];meta=json.load(open(p.with_suffix('.json')))
 settings=None
 for b in meta['metadata']:
  d=b['detail']
  if b['source']=='ins' and 'metadata' in d:settings=d['metadata'].get('effective_navigation',settings)
 assert settings,p
 settings.setdefault('coarse_alignment_history_s',12.) # Pre-normalization recording default.
 settings['analysis_recorded_imu_delay_s']=settings['imu_delay_s']
 p.with_suffix('.config.json').write_text(json.dumps(settings,indent=2))
 events=sorted([(x[1],1,n,int(x[-1])) for n,x in enumerate(i)]+[(x[1],2,n,int(x[-1])) for n,x in enumerate(g)])
 with p.with_suffix('.events').open('wb') as f:
  last=None
  for a,k,n,seg in events:
   if seg!=last:f.write(struct.pack('<BqqIQ',254,0,0,0,0));last=seg
   x=i[n] if k==1 else g[n];flags=int(x[9] if k==1 else x[15]);counter=int(x[10] if k==1 else x[16]);v=x[3:9] if k==1 else x[3:15]
   f.write(struct.pack('<BqqIQ',k,round(x[0]*1e9),round(x[1]*1e9),flags,counter));f.write(np.asarray(v,dtype='<f8').tobytes())
 print(p.stem,len(events),flush=True)
