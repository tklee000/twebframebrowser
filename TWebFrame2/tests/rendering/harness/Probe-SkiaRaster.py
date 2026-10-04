"""Isolated upstream raster backend probe. Does not participate in pass decisions."""
import argparse
import ctypes as c
from pathlib import Path
import numpy as np
from PIL import Image

p=argparse.ArgumentParser();p.add_argument('library',type=Path);p.add_argument('capture',type=Path)
a=p.parse_args()
s=c.CDLL(str(a.library.resolve()))
def api(name,restype,*types):
 f=getattr(s,name);f.restype=restype;f.argtypes=types;return f
ptr=c.c_void_p;f=c.c_float;i=c.c_int;u=c.c_uint32
class Info(c.Structure): _fields_=[('colorspace',ptr),('width',i),('height',i),('color',i),('alpha',i)]
class Rect(c.Structure): _fields_=[('left',f),('top',f),('right',f),('bottom',f)]
new=api('sk_surface_new_raster_direct',ptr,c.POINTER(Info),ptr,c.c_size_t,ptr,ptr,ptr)
canvas=api('sk_surface_get_canvas',ptr,ptr)
paintnew=api('sk_paint_new',ptr)
color=api('sk_paint_set_color',None,ptr,u)
aa=api('sk_paint_set_antialias',None,ptr,c.c_bool)
stroke=api('sk_paint_set_style',None,ptr,i)
width=api('sk_paint_set_stroke_width',None,ptr,f)
rect=api('sk_canvas_draw_rect',None,ptr,c.POINTER(Rect),ptr)
circle=api('sk_canvas_draw_circle',None,ptr,f,f,f,ptr)
clear=api('sk_canvas_clear',None,ptr,u)
scale=api('sk_canvas_scale',None,ptr,f,f)
pathnew=api('sk_path_new',ptr)
move=api('sk_path_move_to',None,ptr,f,f)
line=api('sk_path_line_to',None,ptr,f,f)
drawpath=api('sk_canvas_draw_path',None,ptr,ptr,ptr)
ref=np.array(Image.open(a.capture/'reference.png').convert('RGBA'))
h,w=ref.shape[:2];pixels=np.empty((h,w,4),dtype=np.uint8)
surface=new(c.byref(Info(None,w,h,6,1)),pixels.ctypes.data,w*4,None,None,None)
if not surface: raise RuntimeError('Surface creation failed')
cv=canvas(surface);paint=paintnew();clear(cv,0xffffffff);scale(cv,w/800,w/800)
aa(paint,True);color(paint,0xffeef2f7);rect(cv,c.byref(Rect(12,12,312,182)),paint)
color(paint,0xff4d98a1);rect(cv,c.byref(Rect(24,27,139,107)),paint)
color(paint,0xffd49343);circle(cv,197,90,43,paint)
color(paint,0xff273e5a);stroke(paint,1);width(paint,4);circle(cv,197,90,43,paint)
path=pathnew();move(path,32,157);line(path,107,122);line(path,167,157);line(path,277,127)
color(paint,0xff714f91);width(paint,7);drawpath(cv,path,paint)
# SK_BGRA_8888 output.
rgb=pixels[:,:,[2,1,0]]
different=np.any(rgb!=ref[:,:,:3],axis=2)
print('Skia CPU pixels',int(different.sum()),'max delta',np.abs(rgb.astype(int)-ref[:,:,:3]).max())
for name,bounds in [('circle',(150,40,245,140)),('path',(20,110,290,165))]:
 x0,y0,x1,y1=[round(v*w/800) for v in bounds]
 print(name,int(different[y0:y1,x0:x1].sum()))
Image.fromarray(rgb).save(a.capture.parent/'skia-raster-probe.png')
