"""Independent font-mask diagnostic; never replaces engine captures."""
import argparse
import ctypes as c
from pathlib import Path
import numpy as np
from PIL import Image
p=argparse.ArgumentParser();p.add_argument('library',type=Path);p.add_argument('capture',type=Path);a=p.parse_args()
s=c.CDLL(str(a.library.resolve()))
def api(name,ret,*args):
 method=getattr(s,name);method.restype=ret;method.argtypes=args;return method
ptr=c.c_void_p;f=c.c_float;i=c.c_int;u=c.c_uint32
class Info(c.Structure):_fields_=[('cs',ptr),('w',i),('h',i),('ct',i),('at',i)]
new=api('sk_surface_new_raster_direct',ptr,c.POINTER(Info),ptr,c.c_size_t,ptr,ptr,ptr)
props=api('sk_surfaceprops_new',ptr,u,i)(0,1)
fontmanager=api('sk_fontmgr_create_default',ptr)()
style=api('sk_fontstyle_new',ptr,i,i,i)(400,5,0)
face=api('sk_fontmgr_match_family_style',ptr,ptr,c.c_char_p,ptr)(fontmanager,b'Arial',style)
font=api('sk_font_new_with_values',ptr,ptr,f,f,f)(face,16,1,0)
api('sk_font_set_subpixel',None,ptr,c.c_bool)(font,True)
api('sk_font_set_edging',None,ptr,i)(font,2)
api('sk_font_set_hinting',None,ptr,i)(font,2)
paint=api('sk_paint_new',ptr)();api('sk_paint_set_color',None,ptr,u)(paint,0xff17202a)
ref=np.array(Image.open(a.capture/'reference.png').convert('RGB'))
pixel=np.empty((600,800,4),np.uint8)
surface=new(c.byref(Info(None,800,600,6,1)),pixel.ctypes.data,3200,None,None,props)
cv=api('sk_surface_get_canvas',ptr,ptr)(surface)
api('sk_canvas_clear',None,ptr,u)(cv,0xffeef2f6)
glyph=c.c_uint16(36)
api('sk_canvas_draw_simple_text',None,ptr,ptr,c.c_size_t,i,f,f,ptr,ptr)(cv,c.byref(glyph),2,3,22,38,font,paint)
rgb=pixel[:,:,[2,1,0]]
for y,x in [(28,25),(32,23),(32,24),(32,25)]:print(x,y,'SkiaCPU',rgb[y,x].tolist(),'reference',ref[y,x].tolist())
print('mask colors',np.unique(rgb[26:38,21:33].reshape(-1,3),axis=0).shape)
