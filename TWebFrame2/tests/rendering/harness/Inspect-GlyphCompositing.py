"""Read-only blend precision investigation, never a pixel comparison policy."""
from pathlib import Path
import argparse
import numpy as np
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('capture',type=Path);a=p.parse_args()
ref=np.array(Image.open(a.capture/'reference.png').convert('RGB'))
native=np.array(Image.open(a.capture/'native.png').convert('RGB'))
s=ref.shape[1]/800;y,x=np.indices(ref.shape[:2])
selected=(x>=21*s)&(x<270*s)&(y>=24*s)&(y<90*s)
fg=np.zeros_like(native,dtype=float)+[23,32,42]
fg[(x>=160*s)&(x<260*s)&(y<44*s)]=[167,79,43]
bg=np.zeros_like(fg)+[238,242,246]
q=np.rint((bg-native)/(bg-fg)*[31,63,31]).clip(0,[31,63,31]).astype(int)
for channel in range(3):
 print('channel',channel)
 for qq in np.unique(q[:,:,channel][selected&np.all(fg==[23,32,42],axis=2)]):
  m=selected&(q[:,:,channel]==qq)&np.all(fg==[23,32,42],axis=2)
  vals,counts=np.unique(ref[:,:,channel][m],return_counts=True)
  predicted=int(np.rint(bg[0,0,channel]+(fg[0,0,channel]-bg[0,0,channel])*qq/[31,63,31][channel]))
  print(int(qq),'expected',predicted,list(zip(vals.tolist(),counts.tolist())))
for method in ('exact','separate-rounding','replicate','replicate-separate','replicate-256-separate','integer256','floor','roundHalfColor'):
 alpha=q/[31,63,31]
 if method in ('replicate','replicate-separate','replicate-256-separate','integer256'):
  alpha=np.stack([(q[:,:,0]<<3)|(q[:,:,0]>>2),(q[:,:,1]<<2)|(q[:,:,1]>>4),(q[:,:,2]<<3)|(q[:,:,2]>>2)],axis=2)/255
 if method=='replicate-256-separate':alpha=np.rint(alpha*256)/256
 if method in ('separate-rounding','replicate-separate','replicate-256-separate'): pred=np.rint(bg*(1-alpha))+np.rint(fg*alpha)
 elif method=='integer256': pred=bg+(fg-bg)*np.ceil(alpha*255+.5)/256
 elif method=='roundHalfColor': pred=((bg/255).astype(np.float16).astype(float)*(1-alpha)+(fg/255).astype(np.float16).astype(float)*alpha)*255
 else: pred=bg+(fg-bg)*alpha
 pred=np.floor(pred) if method=='floor' else np.rint(pred)
 delta=np.abs(pred-ref)
 print(method,'different',np.count_nonzero(np.any(delta!=0,axis=2)&selected),'delta>1',np.count_nonzero(np.any(delta>1,axis=2)&selected))
