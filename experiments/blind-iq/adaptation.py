#!/usr/bin/env python3
"""Compare continuous and frozen correction under explicit, known analog drift."""
import json,pathlib,sys
import numpy as np
from scipy.signal import resample_poly
from evaluate import waveform,observe,moments,matrix,operator_metrics

paths=list(map(pathlib.Path,sys.argv[1:4]));output=pathlib.Path(sys.argv[4])
s=None
for path,freq,level in zip(paths,[-7.125e6,-.125e6,7e6],[1.,.9,.7]):
 x=resample_poly(waveform(path),8,1).astype(np.complex64)
 if s is None:s=np.zeros_like(x)
 for begin in range(0,len(x),2**20):
  end=min(begin+2**20,len(x));n=np.arange(begin,end,dtype=np.float64)
  s[begin:end]+=level*x[begin:end]*np.exp(2j*np.pi*freq*n/64e6).astype(np.complex64)
 del x
s*=.10/np.sqrt(np.mean(abs(s)**2));results=[]
for bits in [18,20,22]:
 n=2**bits
 for gain_slope,phase_slope in [(0.,0.),(.1,.1),(1.,1.)]:
  previous=None;frozen=None;continuous=[];fixed=[];means=[];epochs=[]
  for start in range(0,len(s)-n+1,n):
   t=(start+n/2)/64e6
   m=matrix(.23+gain_slope*t,-2.55+phase_slope*t)
   if previous is not None:
    continuous.append(operator_metrics(previous,m));fixed.append(operator_metrics(frozen,m));epochs.append(t)
   y=observe(s[start:start+n],m)
   previous,mean=moments(y)
   if frozen is None:frozen=previous.copy()
  def summary(values):
   a=np.asarray(values)
   phase=np.deg2rad(a[:,1]);amplitude=a[:,2]
   coherent=np.abs(np.mean(amplitude*np.exp(1j*phase)))**2/np.mean(amplitude**2)
   return {'irr_db_min_median':np.percentile(a[:,0],[0,50]).tolist(),'phase_deg_rms':float(np.sqrt(np.mean(a[:,1]**2))),'phase_step_deg_max':float(abs(np.diff(a[:,1])).max()),'coherent_power_loss_db':float(-10*np.log10(coherent))}
  results.append({'window_ms':n/64000,'gain_drift_db_s':gain_slope,'phase_drift_deg_s':phase_slope,'duration_s':epochs[-1],'continuous_previous_window':summary(continuous),'frozen_first_window':summary(fixed)})
output.write_text(json.dumps({'note':'DVB-T lower three, 12-bit ADC/Q1.23, one-window delay. Injected analog matrix constant inside each window; changes at window boundaries. No output phase fitting. Common reference/echo cancellation is not credited.','results':results},indent=2)+'\n')
for r in results:
 print(r['window_ms'],r['gain_drift_db_s'],'adaptive',r['continuous_previous_window'],'frozen',r['frozen_first_window'])
