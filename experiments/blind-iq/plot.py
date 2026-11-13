#!/usr/bin/env python3
import json,pathlib,sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
r=json.loads(pathlib.Path(sys.argv[1]).read_text())
fig,(top,bottom)=plt.subplots(2,1,figsize=(9,7),sharex=True,layout='constrained')
for row in r['scenarios']:
 if row['injected_gain_db']!=.23:continue
 x=[v['window_ms'] for v in row['windows']]
 values=[v['irr_db_min_median_max'] for v in row['windows']]
 median=[v[1] for v in values]
 line,=top.plot(x,median,'o-',label=row['signal'])
 top.fill_between(x,[v[0] for v in values],[v[2] for v in values],color=line.get_color(),alpha=.12)
 bottom.plot(x,[v['phase_deg_rms'] for v in row['windows']],'o-',color=line.get_color(),label=row['signal'])
top.axhline(31.74,color='.35',ls='--',lw=1,label='Before correction')
top.set(ylabel='Image rejection (dB)',title='Blind IQ correction on seeded DVB-T: 0.23 dB / −2.55° error')
bottom.set(xlabel='Estimation window at 64 MS/s (ms)',ylabel='Desired-phase error RMS (degrees)')
for ax in (top,bottom):ax.set_xscale('log',base=4);ax.grid(alpha=.25)
top.legend(ncol=2,fontsize=9);bottom.set_xticks([1.024,4.096,16.384,65.536],['1.024','4.096','16.384','65.536'])
fig.savefig(sys.argv[2],dpi=180)
