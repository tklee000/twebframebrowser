"""Read-only diagnosis of gradient pixel centers and LUT conversion."""
import argparse
from pathlib import Path
import numpy as np
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('capture',type=Path);a=p.parse_args()
ref=np.array(Image.open(a.capture/'reference.png').convert('RGB'))
native=np.array(Image.open(a.capture/'native.png').convert('RGB'))
scale=ref.shape[1]/800
y,x=np.mgrid[int(55*scale):int(135*scale),int(55*scale):int(235*scale)]
print('interior exact differences',int(np.count_nonzero(np.any(ref[y,x]!=native[y,x],axis=2))))
angle=np.float32(120)*np.float32(np.pi)/np.float32(180)
dx=np.sin(angle);dy=-np.cos(angle)
length=(abs(dx)*240+abs(dy)*135)*scale
for targetName,target in [('reference',ref[y,x]),('native',native[y,x])]:
 best=(999999,None)
 for sx in (-1,0,1):
  for sy in (-1,0,1):
   X=x+sx;Y=y+sy
   tt=((X+.5-148*scale)*dx+(Y+.5-95.5*scale)*dy)/length+.5+1e-5
   M=((Y&1)<<5)|((X&1)<<4)|((Y&2)<<2)|((X&2)<<1)|((Y&4)>>1)|((X&4)>>2)
   noise=np.floor((M/64+1/128)*255+.5)/255-.5
   for half in (False,True):
    first=np.array([68,140,158])/255;last=np.array([226,189,116])/255
    if half:first=first.astype(np.float16).astype(float);last=last.astype(np.float16).astype(float)
    predicted=np.rint((first+tt[:,:,None]*(last-first))*255+noise[:,:,None])
    count=int(np.count_nonzero(np.any(predicted!=target,axis=2)))
    if count<best[0]:best=(count,(sx,sy,half))
 print(targetName,best)
for name,target in [('reference',ref[y,x]),('native',native[y,x])]:
 best=(999999,None)
 M=((y&1)<<5)|((x&1)<<4)|((y&2)<<2)|((x&2)<<1)|((y&4)>>1)|((x&4)>>2)
 noise=np.floor((M/64+1/128)*255+.5)/255-.5
 first=(np.array([68,140,158])/255).astype(np.float16).astype(float)
 last=(np.array([226,189,116])/255).astype(np.float16).astype(float)
 for left in (np.floor(28*scale),28*scale,np.ceil(28*scale)):
  for top in (np.floor(28*scale),28*scale,np.ceil(28*scale)):
   for width in (np.floor(240*scale),240*scale,np.ceil(240*scale)):
    for height in (np.floor(135*scale),135*scale,np.ceil(135*scale)):
     length=abs(dx)*width+abs(dy)*height
     tt=((x+.5-left-width/2)*dx+(y+.5-top-height/2)*dy)/length+.5+1e-5
     predicted=np.rint((first+tt[:,:,None]*(last-first))*255+noise[:,:,None])
     count=int(np.count_nonzero(np.any(predicted!=target,axis=2)))
     if count<best[0]:best=(count,(left,top,width,height))
 print(name,'tile',best)
M=((y&1)<<5)|((x&1)<<4)|((y&2)<<2)|((x&2)<<1)|((y&4)>>1)|((x&4)>>2)
noise=np.floor((M/64+1/128)*255+.5)/255-.5
design=np.stack([np.ones_like(x),x+.5,y+.5],axis=-1).reshape(-1,3)
for name,target in [('reference',ref[y,x]),('native',native[y,x])]:
 coeff=np.linalg.lstsq(design,(target-noise[:,:,None]).reshape(-1,3),rcond=None)[0]
 predicted=(design@coeff).reshape(*x.shape,3)+noise[:,:,None]
 print(name,'plane',coeff.tolist(),'residual',int(np.count_nonzero(np.any(np.rint(predicted)!=target,axis=2))))
target=ref[y,x];best=(999999,None)
inverse=np.linalg.pinv(design)
for flip in (False,True):
 for swap in (False,True):
  for px in range(8):
   for py in range(8):
    X,Y=(y,x) if swap else (x,y)
    X=X+px;Y=(-Y if flip else Y)+py
    M=((Y&1)<<5)|((X&1)<<4)|((Y&2)<<2)|((X&2)<<1)|((Y&4)>>1)|((X&4)>>2)
    noise=np.floor((M/64+1/128)*255+.5)/255-.5
    coeff=inverse@(target-noise[:,:,None]).reshape(-1,3)
    predicted=(design@coeff).reshape(*x.shape,3)+noise[:,:,None]
    count=int(np.count_nonzero(np.any(np.rint(predicted)!=target,axis=2)))
    if count<best[0]:best=(count,(flip,swap,px,py))
print('reference plane/dither diagnostic',best)
