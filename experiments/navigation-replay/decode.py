import struct,json,collections,datetime,sys
from pathlib import Path
import numpy as np
from mcap.stream_reader import StreamReader
from mcap.records import Schema,Channel,Message,Metadata
ROOT=Path(sys.argv[1]).resolve()
class FB:
 def __init__(self,b):
  assert b[4:8]==b'PINS';self.b=b;self.t=struct.unpack_from('<I',b)[0];self.v=self.t-struct.unpack_from('<i',b,self.t)[0];self.n=struct.unpack_from('<H',b,self.v)[0]
 def field(self,i):
  p=struct.unpack_from('<H',self.b,self.v+4+2*i)[0] if 4+2*i<self.n else 0
  return self.t+p if p else None
 def get(self,i,fmt,default=0):
  p=self.field(i);return struct.unpack_from('<'+fmt,self.b,p)[0] if p else default
 def vector(self,i,dtype='u1'):
  p=self.field(i)
  if not p:return np.array([],dtype=dtype)
  p+=struct.unpack_from('<I',self.b,p)[0];n=struct.unpack_from('<I',self.b,p)[0]
  return np.frombuffer(self.b,dtype=dtype,count=n,offset=p+4)
 def string(self,i):return self.vector(i).tobytes().decode(errors='replace')

def decode(path):
 name=path.name.replace('.mcap','');dest=ROOT/'decoded';dest.mkdir(exist_ok=True);channels={};kinds=collections.Counter();topics=collections.Counter();imu=[];gps=[];state=[];reasons={};meta=[];rawbuf=bytearray();pvt=[];segments={};stream=None;segment=-1
 wf=(dest/(name+'.wire')).open('wb');error=None
 def reason(s):
  if s not in reasons:reasons[s]=len(reasons)
  return reasons[s]
 try:
  with path.open('rb') as f:
   for r in StreamReader(f,validate_crcs=True).records:
    if isinstance(r,Channel):channels[r.id]=r
    elif isinstance(r,Metadata):
     if r.name=='pl64.boundary':
      try:d=json.loads(r.metadata.get('detail','{}'));meta.append(dict(source=r.metadata.get('source'),utc_ns=r.metadata.get('utc_ns'),detail=d))
      except ValueError:pass
    elif isinstance(r,Message):
     c=channels[r.channel_id];topics[c.topic]+=1
     if c.topic!='/navigation/events':continue
     fb=FB(r.data);k=fb.get(0,'b');src=fb.get(1,'b');kinds[str(k)]+=1
     st=c.metadata.get('stream_id','unknown')
     if st!=stream:
      stream=st;segment+=1;rawbuf.clear();segments[segment]={'stream_id':stream,'channel':c.metadata};wf.write(struct.pack('<BqqqdQI',254,0,0,0,0,0,0))
     a=fb.get(3,'q');u=fb.get(4,'q');m=fb.get(5,'q');unc=fb.get(6,'d');seq=fb.get(2,'Q');flag=fb.get(11,'I');t=m/1e9;arrival=a/1e9;utc=u/1e9
     if k==0:
      payload=fb.vector(8).tobytes();wf.write(struct.pack('<BqqqdQI',src,a,u,m,unc/1e9,seq,len(payload)));wf.write(payload)
      if src==1:
       rawbuf.extend(payload)
       while True:
        p=rawbuf.find(b'\xb5\x62')
        if p<0:rawbuf=rawbuf[-1:];break
        if p:del rawbuf[:p]
        if len(rawbuf)<6:break
        size=8+struct.unpack_from('<H',rawbuf,4)[0]
        if size>8192:del rawbuf[0];continue
        if len(rawbuf)<size:break
        f=bytes(rawbuf[:size]);del rawbuf[:size];ca=cb=0
        for x in f[2:-2]:ca=(ca+x)&255;cb=(cb+ca)&255
        if f[-2:]!=bytes([ca,cb]):continue
        if f[2:4]==b'\x01\x07' and size==92:
         p=f[6:-2];pvt.append([arrival,utc,struct.unpack_from('<I',p)[0],p[11],p[20],p[21],p[23],struct.unpack_from('<I',p,40)[0]*.001,struct.unpack_from('<I',p,44)[0]*.001,struct.unpack_from('<I',p,68)[0]*.001,segment])
     elif k==1:
      v=fb.vector(9,'<f8');imu.append([t,arrival,utc,*v[:6],flag,fb.get(7,'Q'),segment])
     elif k==2:
      v=fb.vector(9,'<f8');gps.append([t,arrival,utc,*v[:12],flag,seq,segment])
     elif k==3:
      v=fb.vector(9,'<f8');x=np.full(28,np.nan);x[:min(len(v),28)]=v[:28];cov=fb.vector(10,'<f8');at=cov.reshape(15,15)[6:9,6:9].ravel() if len(cov)==225 else np.full(9,np.nan);vs=np.diag(cov.reshape(15,15))[3:6] if len(cov)==225 else np.full(3,np.nan)
      state.append([t,arrival,utc,flag,fb.get(13,'d'),fb.get(14,'d'),fb.get(15,'d'),reason(fb.string(12)),*x,*at,*vs,segment])
 except Exception as e:error=f'{type(e).__name__}: {e}'
 finally:wf.close()
 arrays={k:np.asarray(v,dtype=float) for k,v in [('imu',imu),('gps',gps),('state',state),('pvt',pvt)]};np.savez(dest/(name+'.npz'),**arrays)
 report=dict(file=path.name,topics=dict(topics),kinds=dict(kinds),error=error,reasons=reasons,metadata=meta,segments=segments)
 (dest/(name+'.json')).write_text(json.dumps(report,indent=2));print(path.name,{k:len(v) for k,v in arrays.items()},'error',error,flush=True)
 if error and '.partial.' not in path.name:raise RuntimeError(error)
for p in sorted((ROOT/'originals').glob('*.mcap')):decode(p)
