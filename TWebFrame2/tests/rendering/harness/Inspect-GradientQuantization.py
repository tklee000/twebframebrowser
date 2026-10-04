"""Read-only analysis of gradient interpolation, not a comparison tolerance."""
from pathlib import Path
import argparse
import numpy as np
from PIL import Image

p = argparse.ArgumentParser()
p.add_argument('capture', type=Path)
a = p.parse_args()
ref = np.array(Image.open(a.capture / 'reference.png').convert('RGB'))
scale = ref.shape[1] / 800
y, x = np.mgrid[int(55*scale):int(135*scale), int(55*scale):int(235*scale)]
angle = np.deg2rad(120)
dx, dy = np.sin(angle), -np.cos(angle)
t = ((x + .5 - 148*scale)*dx + (y + .5 - 95.5*scale)*dy) / ((abs(dx)*240 + abs(dy)*135)*scale) + .5
color = np.array([68,140,158]) + t[:,:,None] * np.array([158,49,-42])
target = ref[y, x].astype(float)
low = np.max(target - .5 - color, axis=2)
high = np.min(target + .5 - color, axis=2)
print('shared perturbation possible', np.count_nonzero(low <= high), '/', low.size)
print('quantization mismatch', np.count_nonzero(np.any(np.rint(color) != target, axis=2)))
print('delta means', np.mean(target-color,axis=(0,1)), 'std', np.std(target-color,axis=(0,1)))
best = (low.size, None)
for flip in (False,True):
 for swap in (False,True):
  for phase_x in range(8):
   for phase_y in range(8):
    X,Y = (y,x) if swap else (x,y)
    X=X+phase_x
    Y=(-Y if flip else Y)+phase_y
    Y=Y^X
    m=((Y&1)<<5)|((X&1)<<4)|((Y&2)<<2)|((X&2)<<1)|((Y&4)>>1)|((X&4)>>2)
    dither=m/64-63/128
    for amplitude in (0, .5, 1, 2):
     predicted=np.rint(color+dither[:,:,None]*amplitude)
     mismatch=np.count_nonzero(np.any(predicted!=target,axis=2))
     if mismatch<best[0]: best=(int(mismatch),(flip,swap,phase_x,phase_y,amplitude))
print('best Bayer',best)
quantized_best=(low.size,None)
for yflip in (False,True):
 for xor in (False,True):
  for px in range(8):
   for py in range(8):
    X=x+px; Y=(-y if yflip else y)+py
    if xor: Y=Y^X
    M=((Y&1)<<5)|((X&1)<<4)|((Y&2)<<2)|((X&2)<<1)|((Y&4)>>1)|((X&4)>>2)
    noise=np.rint(((M/64-63/128)+.5)*255)/255-.5
    for endpoint_half in (False,True):
     c0=np.array([68,140,158])/255; c1=np.array([226,189,116])/255
     if endpoint_half: c0=c0.astype(np.float16).astype(float); c1=c1.astype(np.float16).astype(float)
     for t_half in (False,True):
      tt=t+1e-5
      if t_half: tt=tt.astype(np.float16).astype(float)
      for calc_half in (False,True):
       cc=c0+tt[:,:,None]*(c1-c0)
       if calc_half: cc=cc.astype(np.float16).astype(float)
       predicted=np.rint(cc*255+noise[:,:,None])
       mismatch=np.count_nonzero(np.any(predicted!=target,axis=2))
       if mismatch<quantized_best[0]: quantized_best=(int(mismatch),(yflip,xor,px,py,endpoint_half,t_half,calc_half))
print('GPU half best',quantized_best)
M=((y&1)<<5)|((x&1)<<4)|((y&2)<<2)|((x&2)<<1)|((y&4)>>1)|((x&4)>>2)
lut=np.rint(((M/64-63/128)+.5)*255)/255
staged_best=(low.size,None)
for colors_half in (False,True):
 for texture_half in (False,True):
  for range_half in (False,True):
   for output_half in (False,True):
    for mix_method in ('lerp','weighted','slope'):
     c0=np.array([68,140,158])/255; c1=np.array([226,189,116])/255
     if colors_half: c0=c0.astype(np.float16).astype(float);c1=c1.astype(np.float16).astype(float)
     ll=lut.astype(np.float16).astype(float) if texture_half else lut
     rr=float(np.float16(1/255)) if range_half else 1/255
     tt=t+1e-5
     if mix_method=='lerp': cc=c0+tt[:,:,None]*(c1-c0)
     elif mix_method=='weighted': cc=c0*(1-tt[:,:,None])+c1*tt[:,:,None]
     else: cc=c0+tt[:,:,None]*((np.array([158,49,-42])/255).astype(np.float16).astype(float))
     cc=cc+(ll[:,:,None]-.5)*rr
     if output_half: cc=cc.astype(np.float16).astype(float)
     mismatch=np.count_nonzero(np.any(np.rint(cc*255)!=target,axis=2))
     if mismatch<staged_best[0]: staged_best=(int(mismatch),(colors_half,texture_half,range_half,output_half,mix_method))
print('staged GPU best',staged_best)
def half_truncate(v):
 h=v.astype(np.float16)
 return np.where(h.astype(float)>v,np.nextafter(h,np.float16(-np.inf)),h).astype(float)
best_truncated=(low.size,None)
for end_mode in ('none','nearest','truncate'):
 for slope_mode in ('none','nearest','truncate'):
  for noise_mode in ('round','floor','ideal'):
   c0=np.array([68,140,158])/255;c1=np.array([226,189,116])/255
   if end_mode=='nearest':c0=c0.astype(np.float16).astype(float);c1=c1.astype(np.float16).astype(float)
   if end_mode=='truncate':c0=half_truncate(c0);c1=half_truncate(c1)
   slope=c1-c0
   if slope_mode=='nearest':slope=slope.astype(np.float16).astype(float)
   if slope_mode=='truncate':slope=half_truncate(slope)
   noise=np.rint(((M/64-63/128)+.5)*255)/255-.5
   if noise_mode=='floor':noise=np.floor(((M/64-63/128)+.5)*255)/255-.5
   elif noise_mode=='ideal':noise=M/64-63/128
   cc=c0+(t[:,:,None]+1e-5)*slope
   mismatch=np.count_nonzero(np.any(np.rint(cc*255+noise[:,:,None])!=target,axis=2))
   if mismatch<best_truncated[0]:best_truncated=(int(mismatch),(end_mode,slope_mode,noise_mode))
print('half conversion best',best_truncated)
noise=np.rint(((M/64-63/128)+.5)*255)/255-.5
coeff=np.linalg.lstsq(np.stack([np.ones_like(t),t],axis=-1).reshape(-1,2),(target-noise[:,:,None]).reshape(-1,3),rcond=None)[0]
print('inferred color coefficients',coeff.tolist())
print('half start', (np.array([68,140,158])/255).astype(np.float16).astype(float)*255)
print('half end', (np.array([226,189,116])/255).astype(np.float16).astype(float)*255)
predicted=coeff[0]+t[:,:,None]*coeff[1]+noise[:,:,None]
print('affine diagnostic mismatch',np.count_nonzero(np.any(np.rint(predicted)!=target,axis=2)))
for period in (2,4,8,16,32):
 compatible=0
 for py in range(period):
  for px in range(period):
   selected=(x%period==px)&(y%period==py)
   compatible += (low[selected].max() <= high[selected].min())
 print('phase intervals',period,compatible,'/',period*period)
 if period==8:
  for py in range(period):
   print('noise',py,[(round(low[(x%period==px)&(y%period==py)].max(),3),round(high[(x%period==px)&(y%period==py)].min(),3)) for px in range(period)])
for i in range(8):
 print('row',i, np.round((target-color)[i,:12],2).tolist())
