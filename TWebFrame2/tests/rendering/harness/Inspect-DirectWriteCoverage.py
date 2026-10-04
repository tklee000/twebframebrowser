"""Read-only DirectWrite glyph coverage probe. Output never grants tolerance."""
import argparse
import ctypes as c
import json
from pathlib import Path
import numpy as np
from PIL import Image

ptr=c.c_void_p;u=c.c_uint32;f=c.c_float;long=c.c_int32
class Guid(c.Structure):_fields_=[('a',u),('b',c.c_uint16),('d',c.c_uint16),('rest',c.c_ubyte*8)]
class Matrix(c.Structure):_fields_=[(k,f) for k in ('m11','m12','m21','m22','dx','dy')]
class Offset(c.Structure):_fields_=[('advance',f),('ascender',f)]
class Run(c.Structure):_fields_=[('face',ptr),('em',f),('count',u),('indices',c.POINTER(c.c_uint16)),('advances',c.POINTER(f)),('offsets',c.POINTER(Offset)),('sideways',long),('bidi',u)]
class Rect(c.Structure):_fields_=[(k,long) for k in ('left','top','right','bottom')]
def call(obj,index,types,*args):
 vt=c.cast(obj,c.POINTER(c.POINTER(ptr))).contents
 result=c.WINFUNCTYPE(long,ptr,*types)(vt[index])(obj,*args)
 if result<0:raise RuntimeError(hex(result&0xffffffff))
 return result
p=argparse.ArgumentParser();p.add_argument('capture',type=Path);p.add_argument('--glyph',type=int,default=36);p.add_argument('--x',type=float,default=22);p.add_argument('--y',type=float,default=38);p.add_argument('--em',type=float,default=16);p.add_argument('--isolated',action='store_true');p.add_argument('--file-face',action='store_true');p.add_argument('--identity-only',action='store_true');p.add_argument('--warmup-mode',type=int,choices=[3,4,5]);p.add_argument('--round-mode',type=int,choices=[0,256,512,768],default=0);a=p.parse_args()
floating_control=c.c_uint()
c.CDLL('ucrtbase')._controlfp_s(c.byref(floating_control),a.round_mode,768)
print('floating-round-mode',a.round_mode,'control',hex(floating_control.value))
c.windll.ole32.CoInitializeEx(None,0)
iid=Guid();c.windll.ole32.CLSIDFromString('{b859ee5a-d838-4b5b-a2e8-1adc7d93db48}',c.byref(iid))
factory=ptr();create=c.windll.dwrite.DWriteCreateFactory;create.argtypes=[u,c.POINTER(Guid),c.POINTER(ptr)];create.restype=long
if create(int(a.isolated),c.byref(iid),c.byref(factory))<0:raise RuntimeError('factory')
factory2=ptr();iid2=Guid();c.windll.ole32.CLSIDFromString('{0439fc60-ca44-4994-8dee-3a9af7b732ec}',c.byref(iid2))
call(factory,0,[c.POINTER(Guid),c.POINTER(ptr)],c.byref(iid2),c.byref(factory2))
collection=ptr();call(factory,3,[c.POINTER(ptr),long],c.byref(collection),0)
index=u();exists=long();call(collection,5,[c.c_wchar_p,c.POINTER(u),c.POINTER(long)],'Arial',c.byref(index),c.byref(exists))
family=ptr();call(collection,4,[u,c.POINTER(ptr)],index,c.byref(family))
font=ptr();call(family,7,[u,u,u,c.POINTER(ptr)],400,5,0,c.byref(font))
face=ptr();call(font,13,[c.POINTER(ptr)],c.byref(face))
if a.file_face:
 count=u();call(face,4,[c.POINTER(u),ptr],c.byref(count),None)
 files=(ptr*count.value)();call(face,4,[c.POINTER(u),ptr],c.byref(count),files)
 vt=c.cast(face,c.POINTER(c.POINTER(ptr))).contents
 face_type=c.WINFUNCTYPE(u,ptr)(vt[3])(face)
 face_index=c.WINFUNCTYPE(u,ptr)(vt[5])(face)
 simulations=c.WINFUNCTYPE(u,ptr)(vt[7])(face)
 cloned=ptr();call(factory,9,[u,u,ptr,u,u,c.POINTER(ptr)],face_type,count,files,face_index,simulations,c.byref(cloned))
 face=cloned
 print('file-face',face_type,face_index,simulations,count.value)
ref=np.array(Image.open(a.capture/'reference.png').convert('RGB'));scale=ref.shape[1]/800
fg=np.array([23,32,42]);bg=np.array([238,242,246]);levels=np.array([31,63,31])
rawExpected=np.rint((bg-ref)/(bg-fg)*levels).clip(0,levels)
def srgb(v):return np.where(v<=.04045,v/12.92,((v+.055)/1.055)**2.4)
def encoded(v):return np.where(v<=.0031308,v*12.92,1.055*v**(1/2.4)-.055)
lut=[]
for color in fg:
 level=int(color)>>5;src=((level<<5)|(level<<2)|(level>>1))/255;dst=1-src
 raw=np.arange(256)/255;corrected=raw+(1-raw)*srgb(dst)*raw
 output=encoded(srgb(src)*corrected+(1-corrected)*srgb(dst))
 lut.append(np.rint((output-dst)/(src-dst)*255).astype(np.uint8))
glyph=c.c_uint16(a.glyph);advance=f(0);offset=Offset(0,0)
normal=float(np.float32(1));below=float(np.nextafter(np.float32(1),np.float32(0)));above=float(np.nextafter(np.float32(1),np.float32(2)))
best=(999999,None)
if a.warmup_mode:
 run=Run(face,a.em*scale,1,c.pointer(glyph),c.pointer(advance),c.pointer(offset),0,0)
 matrix=Matrix(1,0,0,1,a.x*scale-np.floor(a.x*scale),0)
 analysis=ptr();call(factory,23,[c.POINTER(Run),f,c.POINTER(Matrix),u,u,f,f,c.POINTER(ptr)],c.byref(run),1,c.byref(matrix),a.warmup_mode,0,0,0,c.byref(analysis))
 bounds=Rect();call(analysis,3,[u,c.POINTER(Rect)],1,c.byref(bounds));w=bounds.right-bounds.left;h=bounds.bottom-bounds.top
 raw=(c.c_ubyte*(w*h*3))();call(analysis,4,[u,c.POINTER(Rect),ptr,u],1,c.byref(bounds),raw,len(raw));call(analysis,2,[])
for mode in ((4 if a.em*scale<=20 else 5,) if a.identity_only else (3,4,5)):
 for m11 in ((normal,) if a.identity_only else (below,normal,above)):
  for m22 in ((normal,) if a.identity_only else (below,normal,above)):
   for em in ((a.em*scale,) if a.identity_only else (float(np.nextafter(np.float32(a.em*scale),np.float32(0))),a.em*scale,float(np.nextafter(np.float32(a.em*scale),np.float32(100))))):
    run=Run(face,em,1,c.pointer(glyph),c.pointer(advance),c.pointer(offset),0,0)
    matrix=Matrix(m11,0,0,m22,a.x*scale-np.floor(a.x*scale),0)
    analysis=ptr();call(factory,23,[c.POINTER(Run),f,c.POINTER(Matrix),u,u,f,f,c.POINTER(ptr)],c.byref(run),1,c.byref(matrix),mode,0,0,0,c.byref(analysis))
    bounds=Rect();call(analysis,3,[u,c.POINTER(Rect)],1,c.byref(bounds));w=bounds.right-bounds.left;h=bounds.bottom-bounds.top
    raw=(c.c_ubyte*(w*h*3))();call(analysis,4,[u,c.POINTER(Rect),ptr,u],1,c.byref(bounds),raw,len(raw))
    alpha=np.ctypeslib.as_array(raw).reshape(h,w,3)
    quant=np.stack([lut[ch][alpha[:,:,ch]] >> (2 if ch==1 else 3) for ch in range(3)],axis=2)
    x0=int(np.floor(a.x*scale))+bounds.left;y0=int(np.floor(a.y*scale+.5))+bounds.top
    expected=rawExpected[y0:y0+h,x0:x0+w]
    mismatch=int(np.count_nonzero(np.any(quant!=expected,axis=2)))
    if mode==(4 if em<=20 else 5) and m11==1 and m22==1 and em==a.em*scale:
     print('identity',mismatch,'bounds',x0,y0,w,h)
     for yy,xx in np.argwhere(np.any(quant!=expected,axis=2)):
      print('coverage',x0+int(xx),y0+int(yy),'raw',alpha[yy,xx].tolist(),
            'quant',quant[yy,xx].tolist(),'referenceQuant',expected[yy,xx].tolist())
    if mismatch<best[0]:best=(mismatch,(mode,m11,m22,em));print('best',best)
    call(analysis,2,[])
print('final',best)
for mode in (4,5):
 for grid in (0,1,2):
  run=Run(face,a.em*scale,1,c.pointer(glyph),c.pointer(advance),c.pointer(offset),0,0)
  matrix=Matrix(1,0,0,1,a.x*scale-np.floor(a.x*scale),0)
  analysis=ptr();call(factory2,30,[c.POINTER(Run),c.POINTER(Matrix),u,u,u,u,f,f,c.POINTER(ptr)],c.byref(run),c.byref(matrix),mode,0,grid,0,0,0,c.byref(analysis))
  bounds=Rect();call(analysis,3,[u,c.POINTER(Rect)],1,c.byref(bounds));w=bounds.right-bounds.left;h=bounds.bottom-bounds.top
  raw=(c.c_ubyte*(w*h*3))();call(analysis,4,[u,c.POINTER(Rect),ptr,u],1,c.byref(bounds),raw,len(raw))
  alpha=np.ctypeslib.as_array(raw).reshape(h,w,3)
  quant=np.stack([lut[ch][alpha[:,:,ch]] >> (2 if ch==1 else 3) for ch in range(3)],axis=2)
  x0=int(np.floor(a.x*scale))+bounds.left;y0=int(np.floor(a.y*scale+.5))+bounds.top
  expected=rawExpected[y0:y0+h,x0:x0+w]
  print('factory2',mode,grid,int(np.count_nonzero(np.any(quant!=expected,axis=2))))
  call(analysis,2,[])
