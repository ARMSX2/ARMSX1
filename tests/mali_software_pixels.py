"""SDL pixel-equivalence regression for the Mali upload conversion.
Requires pysdl2 and a local SDL2 runtime (pysdl2-dll on Windows).
Exercises the SDL operations; Android vendor gating is verified by code review/build.
"""
import ctypes as c
import sdl2 as s

def check(rc):
 assert rc==0,s.SDL_GetError()

def draw(fmt,w,h,native,linear,rotation,crop):
 out=s.SDL_CreateRGBSurfaceWithFormat(0,682,512,32,s.SDL_PIXELFORMAT_RGBA32)
 renderer=s.SDL_CreateSoftwareRenderer(out)
 texture=s.SDL_CreateTexture(renderer,s.SDL_PIXELFORMAT_RGBA32 if native else fmt,s.SDL_TEXTUREACCESS_STREAMING,w,h)
 assert texture
 check(s.SDL_SetTextureBlendMode(texture,s.SDL_BLENDMODE_NONE))
 check(s.SDL_SetTextureScaleMode(texture,s.SDL_ScaleModeLinear if linear else s.SDL_ScaleModeNearest))
 bpp=2 if fmt==s.SDL_PIXELFORMAT_BGR555 else 3
 pitch=w*bpp+8
 data=(c.c_ubyte*(pitch*h))()
 for y in range(h):
  for x in range(w):
   rgb=((x//16*37)%256,(y//16*53)%256,((x+y)//16*19)%256)
   off=y*pitch+x*bpp
   if bpp==2:
    v=(rgb[0]>>3)|((rgb[1]>>3)<<5)|((rgb[2]>>3)<<10)
    data[off]=v&255;data[off+1]=v>>8
   else:
    data[off:off+3]=rgb
 # Full frame followed by partial update, testing pitched source and locked subrect.
 for first,rows in [(0,h),(h//3,7)]:
  if first:
   for i in range(first*pitch,(first+rows)*pitch): data[i]=255
  rect=s.SDL_Rect(0,first,w,rows)
  source=c.cast(c.byref(data,first*pitch),c.c_void_p)
  if native:
   dest=c.c_void_p();dp=c.c_int()
   check(s.SDL_LockTexture(texture,c.byref(rect),c.byref(dest),c.byref(dp)))
   check(s.SDL_ConvertPixels(w,rows,fmt,source,pitch,s.SDL_PIXELFORMAT_RGBA32,dest,dp.value))
   s.SDL_UnlockTexture(texture)
  else: check(s.SDL_UpdateTexture(texture,c.byref(rect),source,pitch))
 check(s.SDL_SetRenderDrawColor(renderer,0,0,0,255));check(s.SDL_RenderClear(renderer))
 src=s.SDL_Rect(3,2,w-7,h-5) if crop else None
 dst=s.SDL_Rect(11,9,660,494)
 if rotation:
  check(s.SDL_RenderCopyEx(renderer,texture,c.byref(src) if src else None,c.byref(dst),rotation*90,None,s.SDL_FLIP_NONE))
 else:check(s.SDL_RenderCopy(renderer,texture,c.byref(src) if src else None,c.byref(dst)))
 s.SDL_RenderPresent(renderer)
 result=c.string_at(out.contents.pixels,out.contents.pitch*out.contents.h)
 s.SDL_DestroyTexture(texture);s.SDL_DestroyRenderer(renderer);s.SDL_FreeSurface(out)
 return result
for fmt in (s.SDL_PIXELFORMAT_BGR555,s.SDL_PIXELFORMAT_RGB24):
 for w,h in ((320,240),(512,216),(640,478)):
  for linear,rotation,crop in ((False,0,False),(False,0,True),(True,0,True),(False,1,False)):
   a=draw(fmt,w,h,False,linear,rotation,crop);b=draw(fmt,w,h,True,linear,rotation,crop)
   diffs=sum(x!=y for x,y in zip(a,b))
   assert diffs == 0, (fmt,w,h,linear,rotation,crop,diffs)
print('24 SDL pixel-equivalence cases passed')
