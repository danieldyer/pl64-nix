#!/usr/bin/env python3
"""Seeded, held-out blind DC/IQ correction qualification. No fitted output phase.

Inputs: three CF32 files from carriers.cpp (ETSI DVB-T modem, 6817 carriers,
8192 useful samples, GI 1/16). Output contains actual residual widely-linear
coefficients, independent of finite-sample image measurement noise.
"""
import argparse,json,pathlib
import numpy as np
from scipy.signal import resample_poly


def moments(z):
    i=z.real.astype(np.float64);q=z.imag.astype(np.float64)
    mi=i.mean();mq=q.mean();vi=np.mean(i*i)-mi*mi;vq=np.mean(q*q)-mq*mq
    cross=np.mean(i*q)-mi*mq
    k=cross/vi; g=np.sqrt(max(vq/vi-k*k,1e-12))
    w=np.array([[1.,0.],[-k/g,1/g]])
    # Keep rows bounded within existing signed Q1.23 coefficient range.
    scale=.93/max(np.sum(abs(w),axis=1))
    return np.rint(w*scale*2**23)/2**23, np.rint(np.array([mi,mq])*32768)/32768


def matrix(gain_db,phase_deg):
    g=10**(gain_db/20);p=np.deg2rad(phase_deg)
    return np.array([[1.,0.],[g*np.sin(p),g*np.cos(p)]])


def operator_metrics(w,m):
    t=w@m
    a=(t[0,0]+t[1,1]+1j*(t[1,0]-t[0,1]))/2
    b=(t[0,0]-t[1,1]+1j*(t[1,0]+t[0,1]))/2
    return float(-20*np.log10(max(abs(b/a),1e-16))),float(np.angle(a,deg=True)),float(abs(a))


def observe(s,m,dc=(.01285,.01032)):
    i=m[0,0]*s.real+m[0,1]*s.imag+dc[0]
    q=m[1,0]*s.real+m[1,1]*s.imag+dc[1]
    # Physical 12-bit ADC, followed by left justification in native CI16.
    return (np.clip(np.rint(i*2048),-2048,2047)/2048 +
            1j*np.clip(np.rint(q*2048),-2048,2047)/2048).astype(np.complex64)


def waveform(path):
    c=np.fromfile(path,dtype='<c8').reshape(-1,6817)
    f=np.zeros((len(c),8192),dtype=np.complex64)
    f[:,(np.arange(6817)-3408)%8192]=c
    x=np.fft.ifft(f,axis=1).astype(np.complex64)
    return np.concatenate((x[:,-512:],x),axis=1).ravel()


def main():
    p=argparse.ArgumentParser();p.add_argument('carriers',nargs=3,type=pathlib.Path);p.add_argument('--c31',type=pathlib.Path);p.add_argument('--output',type=pathlib.Path,required=True);a=p.parse_args()
    result={'sample_rate':64000000,'adc_bits':12,'coefficient_format':'signed Q1.23','training_oracle_used':False,'mean_format':'signed native CI16 units','output_format':'saturated rounded CI16','single_channel_constellation':'QPSK' if a.c31 else '64QAM','scenarios':[]}
    base=[waveform(x) for x in a.carriers]
    c31=waveform(a.c31) if a.c31 else base[0]
    for name,offsets,levels in [('lower-three',[-7.125e6,-.125e6,7e6],[1.,.9,.7]),('upper-two',[-3.5e6,3.5e6],[1.,.8]),('single-centred',[0.],[1.]),('single-offset',[.125e6],[1.])]:
        s=np.zeros(len(base[0])*8,dtype=np.complex64)
        for index,(offset,level) in enumerate(zip(offsets,levels)):
            x=resample_poly(c31 if name.startswith("single") else base[index],8,1).astype(np.complex64)
            # Chunk the NCO to avoid a large float64 phase array.
            for start in range(0,len(x),2**20):
                end=min(start+2**20,len(x));n=np.arange(start,end,dtype=np.float64)
                s[start:end]+=level*x[start:end]*np.exp(2j*np.pi*(offset/64e6)*n).astype(np.complex64)
            del x
        s*=.10/np.sqrt(np.mean(abs(s)**2))
        # Keep held-out input for genuine complex error and DC residual metrics.
        hold=s[-2**20:]; train=s[:-2**20]
        for gain,phase in [(0.,0.),(.23,-2.55),(1.,5.),(3.,10.)]:
            m=matrix(gain,phase);y=observe(train,m);test=observe(hold,m)
            before=operator_metrics(np.eye(2),m)
            rows=[]
            for bits in (16,18,20,22):
                n=2**bits;trials=min(12,len(y)//n);metrics=[];dc_errors=[];evms=[]
                for trial in range(trials):
                    w,mean=moments(y[trial*n:(trial+1)*n]);metrics.append(operator_metrics(w,m))
                    out=w[0,0]*(test.real-mean[0])+1j*(w[1,0]*(test.real-mean[0])+w[1,1]*(test.imag-mean[1]))
                    out=(np.clip(np.rint(out.real*32768),-32768,32767)/32768+
                         1j*np.clip(np.rint(out.imag*32768),-32768,32767)/32768)
                    # The known injected matrix defines expected gain/phase. No fitting.
                    t=w@m; ideal=(t[0,0]+t[1,1]+1j*(t[1,0]-t[0,1]))/2
                    error=out-ideal*hold
                    evms.append(float(20*np.log10(np.sqrt(np.mean(abs(error)**2)/np.mean(abs(ideal*hold)**2)))))
                    dc_errors.append(float(abs(error.mean())))
                v=np.array(metrics)
                rows.append({'window_samples':n,'window_ms':n/64000,'trials':trials,'irr_db_min_median_max':np.percentile(v[:,0],[0,50,100]).tolist(),'phase_deg_rms':float(np.sqrt(np.mean(v[:,1]**2))),'phase_deg_max_abs':float(abs(v[:,1]).max()),'holdout_error_db_median':float(np.median(evms)),'holdout_dc_dbfs_median':float(20*np.log10(np.median(dc_errors)))})
            result['scenarios'].append({'signal':name,'injected_gain_db':gain,'injected_phase_deg':phase,'before_irr_db':before[0],'windows':rows})
        del train,s,y,test
    a.output.parent.mkdir(parents=True,exist_ok=True);a.output.write_text(json.dumps(result,indent=2)+'\n')
    for x in result['scenarios']:
        r=x['windows'][2]
        print(x['signal'],x['injected_gain_db'],x['injected_phase_deg'],'IRR',round(x['before_irr_db'],2),'->',np.round(r['irr_db_min_median_max'],2),'phase rms/max',round(r['phase_deg_rms'],4),round(r['phase_deg_max_abs'],4),'holdout',round(r['holdout_error_db_median'],2))


if __name__=='__main__':main()
