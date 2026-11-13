import sys
from pathlib import Path
import numpy as np,json
from scipy.signal import savgol_filter
R=Path(sys.argv[1]).resolve();M=np.array([[0,1,0],[-1,0,0],[0,0,1]]);rows=[]
for p in sorted((R/'decoded').glob('*.npz'))[:3]:
 d=np.load(p);im=d['imu'];g=d['gps'];g=g[(g[:,15].astype(int)&1)!=0];down=-np.median(im[:,6:9],0);down/=np.linalg.norm(down);vv=savgol_filter(g[:,6:8],21,2,axis=0);hh=np.unwrap(np.arctan2(vv[:,1],vv[:,0]));candidates=[]
 for duration in [10,20,30]:
  for t in np.arange(g[0,0]+2,g[-1,0]-duration-2,2):
   end=t+duration;a=g[abs(g[:,0]-t)<1];b=g[abs(g[:,0]-end)<1];ix=(g[:,0]>=t)&(g[:,0]<=end)
   if min(len(a),len(b))<10 or np.max(np.diff(g[ix,0]))>.3 or np.min(np.linalg.norm(vv[ix],axis=1))<2.5:continue
   va=a[:,6:9].mean(0);vb=b[:,6:9].mean(0);sa=np.linalg.norm(va[:2]);sb=np.linalg.norm(vb[:2]);ua=np.median(a[:,12]);ub=np.median(b[:,12]);ha=np.arctan2(va[1],va[0]);hb=np.arctan2(vb[1],vb[0]);ha+=2*np.pi*round((np.interp(t,g[:,0],hh)-ha)/(2*np.pi));hb+=2*np.pi*round((np.interp(end,g[:,0],hh)-hb)/(2*np.pi));ang=np.rad2deg(hb-ha);sigma=np.rad2deg(np.hypot(ua/sa,ub/sb))
   if min(sa,sb)<4 or max(ua/sa,ub/sb)>.45 or max(abs(va[2]),abs(vb[2]))>3 or not 25<abs(ang)<165 or abs(ang)/sigma<2:continue
   ii=im[(im[:,0]>=t)&(im[:,0]<=end)]
   if len(ii)<190*duration or np.max(np.diff(ii[:,0]))>.02:continue
   candidates.append({'file':p.name,'start':float(t),'end':float(end),'elapsed':float(t-im[0,0]),'gnss_turn_deg':float(ang),'gnss_sigma_deg':float(sigma),'score':float(abs(ang)/sigma),'speeds':[float(sa),float(sb)]})
 selected=[]
 for c in sorted(candidates,key=lambda x:x['score'],reverse=True):
  if any(c['start']<q['end']+2 and c['end']>q['start']-2 for q in selected):continue
  selected.append(c)
 for c in sorted(selected,key=lambda x:x['start']):
  ii=im[(im[:,0]>=c['start'])&(im[:,0]<=c['end'])];iw=np.trapezoid(ii[:,3:6],ii[:,0],axis=0);c['identity_turn_deg']=float(np.rad2deg(down@iw));c['candidate_turn_deg']=float(np.rad2deg(down@M@iw));c['raw_integrated_gyro_deg']=np.rad2deg(iw).tolist();c['down']=down.tolist();rows.append(c)
print(json.dumps(rows,indent=2));summary={}
for n in ['identity','candidate']:
 e=np.array([r[n+'_turn_deg']-r['gnss_turn_deg'] for r in rows]);s=np.array([r['gnss_sigma_deg'] for r in rows]);summary[n]={'rms_deg':float(np.sqrt(np.mean(e*e))),'chi2':float(np.sum((e/s)**2)),'n':len(e)}
print(summary);(R/'identification'/'gnss-selected-turns.json').write_text(json.dumps({'selection':'GPS only: 10/20/30 s, 2 s grid; >=2 sigma turn, endpoints >4 m/s, no overlapping selected windows','intervals':rows,'scores':summary},indent=2))
