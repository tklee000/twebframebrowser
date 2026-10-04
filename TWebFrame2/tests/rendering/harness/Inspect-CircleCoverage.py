"""Read-only analytic circle coverage experiment."""
import argparse
from pathlib import Path
import numpy as np
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('capture',type=Path);a=p.parse_args()
ref=np.array(Image.open(a.capture/'reference.png').convert('RGB'));scale=ref.shape[1]/800
y,x=np.mgrid[round(40*scale):round(140*scale),round(150*scale):round(245*scale)]
d=np.hypot(x+.5-197*scale,y+.5-90*scale)
target=ref[y,x].astype(float)
bg=np.array([238,242,247]);fill=np.array([212,147,67]);stroke=np.array([39,62,90])
best=(99999,None)
for mode in ('distance','squared','derivative'):
 for quant in ('none','round','floor'):
  radius=43*scale;half=2*scale
  def coverage(r):
   if mode=='distance': c=r+.5-d
   elif mode=='squared': c=(r*r-d*d)/(2*r)+.5
   else: c=(r*r-d*d)/(2*d)+.5
   c=np.clip(c,0,1)
   if quant=='round':c=np.rint(c*255)/255
   elif quant=='floor':c=np.floor(c*255)/255
   return c
  f=coverage(radius)
  inner=coverage(radius-half)
  outer=coverage(radius+half)
  s=outer*(1-inner)
  color=bg+(fill-bg)*f[:,:,None]
  color=color+(stroke-color)*s[:,:,None]
  mismatch=np.count_nonzero(np.any(np.rint(color)!=target,axis=2))
  print(mode,quant,int(mismatch))
  if mismatch<best[0]:best=(int(mismatch),(mode,quant))
print('best',best)
