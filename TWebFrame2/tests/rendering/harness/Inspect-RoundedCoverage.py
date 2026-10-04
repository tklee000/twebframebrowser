"""Read-only diagnosis of circle clip coverage and shader derivatives."""
import argparse,json
from pathlib import Path
import numpy as np
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('capture',type=Path);a=p.parse_args()
ref=np.array(Image.open(a.capture/'reference.png').convert('RGB'))
data=json.loads((a.capture/'reference.json').read_text(encoding='utf-8-sig'))
scale=ref.shape[1]/800
bg=np.array([114,173,178]);fg=np.array([219,170,90])
for box in data['boxes']:
 if box['id'] not in ('card1','card2','card3'):continue
 left,top,w,h=box['rect'];right=round((left+w)*scale);bottom=round((top+h)*scale)
 x0=round((left+w-47)*scale);y0=round((top+h-43)*scale)
 width=round((left+w+8)*scale)-x0;height=round((top+h+12)*scale)-y0
 cx=x0+width/2;cy=y0+height/2;r=27.5*scale
 y,x=np.mgrid[y0:bottom,x0:right];dx=x+.5-cx;dy=y+.5-cy
 distance=np.hypot(dx,dy);fn=(dx*dx+dy*dy-r*r)
 for mode in ('L2','L1','HW','ellipse'):
  if mode=='L2':coverage=np.clip(r+.5-distance,0,1)
  elif mode=='L1':coverage=np.clip(.5-fn/(2*(abs(dx)+abs(dy))),0,1)
  elif mode=='HW':coverage=np.clip(.5-fn/(2*(abs((x//2)*2+1-cx)+abs((y//2)*2+1-cy))),0,1)
  else:coverage=np.clip(.5-fn/(2*distance),0,1)
  for composite in ('nearest','separate'):
   if composite=='nearest':pixel=np.rint(bg+(fg-bg)*coverage[:,:,None])
   else:pixel=np.rint(bg*(1-coverage[:,:,None]))+np.rint(fg*coverage[:,:,None])
   count=int(np.count_nonzero(np.any(pixel!=ref[y,x],axis=2)))
   print(box['id'],mode,composite,count)
