"""Read-only installed Windows TTF BMP glyph/advance diagnostic; not an engine correction."""
import struct
from pathlib import Path
for name in ['consola.ttf','segoeui.ttf','seguisym.ttf','arial.ttf','cour.ttf','tahoma.ttf','times.ttf']:
 p=Path('C:/Windows/Fonts')/name
 if not p.exists():continue
 data=p.read_bytes();count=struct.unpack_from('>H',data,4)[0]
 tables={data[12+i*16:16+i*16].decode('ascii'):struct.unpack_from('>II',data,20+i*16) for i in range(count)}
 cmap=tables['cmap'][0];nc=struct.unpack_from('>H',data,cmap+2)[0];table=None
 for i in range(nc):
  platform,encoding,off=struct.unpack_from('>HHI',data,cmap+4+8*i)
  if platform==3 and encoding==1:table=cmap+off;break
 if table is None:continue
 segs=struct.unpack_from('>H',data,table+6)[0]//2
 ends=table+14;starts=ends+2*segs+2;deltas=starts+2*segs;ranges=deltas+2*segs
 def glyph(ch):
  for i in range(segs):
   end=struct.unpack_from('>H',data,ends+2*i)[0];start=struct.unpack_from('>H',data,starts+2*i)[0]
   if start<=ch<=end:
    delta=struct.unpack_from('>h',data,deltas+2*i)[0];ro=struct.unpack_from('>H',data,ranges+2*i)[0]
    if ro==0:return (ch+delta)&65535
    g=struct.unpack_from('>H',data,ranges+2*i+ro+2*(ch-start))[0];return ((g+delta)&65535) if g else 0
  return 0
 units=struct.unpack_from('>H',data,tables['head'][0]+18)[0];num=struct.unpack_from('>H',data,tables['hhea'][0]+34)[0]
 print(name,[(hex(ch),glyph(ch),round(struct.unpack_from('>H',data,tables['hmtx'][0]+min(glyph(ch),num-1)*4)[0]/units*20,4)) for ch in [0x2d,0x2010,0x2011]])
