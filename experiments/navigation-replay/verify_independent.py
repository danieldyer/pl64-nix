import ctypes,sys,json,numpy as np
from pathlib import Path
from scipy.spatial.transform import Rotation
sys.path.insert(0,str(Path(__file__).resolve().parent));from identify_axes import Kernel
R=Path(sys.argv[1]).resolve();k=Kernel(sys.argv[2]);lib=ctypes.CDLL(sys.argv[3]);ptr=np.ctypeslib.ndpointer(dtype=np.float64,flags='C_CONTIGUOUS');lib.production_rotations.argtypes=[ctypes.c_int]+[ptr]*4;lib.production_rotations.restype=None
out=[]
for axis in range(3):
 for sign in [-1,1]:
  t=np.linspace(0,2,401);w=np.zeros((len(t),3));w[:,axis]=sign*np.pi/4;f=np.tile([.3,-2.,-9.6],(len(t),1));o=k.integrate(t,w,f,np.eye(3),np.zeros(3));expected=Rotation.from_rotvec(w[-1]*2).as_matrix();p=np.empty((len(t),9));lib.production_rotations(len(t),t,w,f,p);err=float(np.max(abs(o[-1,:9].reshape(3,3)-expected)));errp=float(np.max(abs(p-o[:,:9])));assert err<1e-10 and errp<1e-9;out.append({'axis':axis,'sign':sign,'independent_vs_analytic_max_matrix_error':err,'production_vs_independent_max_matrix_error':errp})
for path in sorted((R/'decoded').glob('*.npz'))[:3]:
 d=np.load(path)['imu'];d=d[(d[:,0]>=d[0,0]+10)&(d[:,0]<=d[0,0]+80)];t=np.ascontiguousarray(d[:,0]-d[0,0]);w=np.ascontiguousarray(d[:,3:6]);f=np.ascontiguousarray(d[:,6:9]);o=k.integrate(t,w,f,np.eye(3),np.zeros(3));p=np.empty((len(t),9));lib.production_rotations(len(t),t,w,f,p);err=float(np.max(abs(p-o[:,:9])));assert err<1e-6;out.append({'file':path.name,'production_vs_independent_max_matrix_error':err})
# Arbitrary common re-expression of all body vectors is a coordinate gauge.
t=np.linspace(0,10,2001);w=np.tile([.1,-.05,.2],(len(t),1));f=np.tile([.5,-1.2,-9.8],(len(t),1));o=k.integrate(t,w,f,np.eye(3),np.zeros(3));m=Rotation.from_rotvec([.3,-.6,.2]).as_matrix();oo=k.integrate(t,np.ascontiguousarray(w@m.T),np.ascontiguousarray(f@m.T),np.eye(3),np.zeros(3));err=float(np.max(abs(oo[:,9:12]-o[:,9:12]@m.T)));assert err<1e-9;out.append({'common_frame_force_invariance_max_error':err})
(R/'identification'/'independent-integrator-checks.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
