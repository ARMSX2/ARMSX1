#include "../psx/state.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
static uint16_t frame[524288], decoded[524288];
int main(void) {
{
 uint16_t a[]={0,1,127,255,32767,(uint16_t)-1,(uint16_t)0x89abcdef}, b[7], c[7];
 psx_state_writer_t x={0}, y={0};
 psx_sw_u16_array(&x,a,7);
 for(int i=0;i<7;i++) psx_sw_u16(&y,a[i]);
 assert(x.size==y.size && !memcmp(x.buf,y.buf,x.size));
 for(size_t n=0;n<=x.size;n++) {
  psx_state_reader_t r,q; psx_sr_init(&r,x.buf,n); psx_sr_init(&q,x.buf,n);
  psx_sr_u16_array(&r,b,7);
  for(int i=0;i<7;i++) c[i]=(uint16_t)psx_sr_u16(&q);
  assert(!memcmp(b,c,sizeof(b)) && r.offset==q.offset && r.error==q.error);
 }
 psx_sw_free(&x); psx_sw_free(&y);
}
{
 int16_t a[]={0,1,127,255,32767,(int16_t)-1,(int16_t)0x89abcdef}, b[7], c[7];
 psx_state_writer_t x={0}, y={0};
 psx_sw_i16_array(&x,a,7);
 for(int i=0;i<7;i++) psx_sw_u16(&y,a[i]);
 assert(x.size==y.size && !memcmp(x.buf,y.buf,x.size));
 for(size_t n=0;n<=x.size;n++) {
  psx_state_reader_t r,q; psx_sr_init(&r,x.buf,n); psx_sr_init(&q,x.buf,n);
  psx_sr_i16_array(&r,b,7);
  for(int i=0;i<7;i++) c[i]=(int16_t)psx_sr_u16(&q);
  assert(!memcmp(b,c,sizeof(b)) && r.offset==q.offset && r.error==q.error);
 }
 psx_sw_free(&x); psx_sw_free(&y);
}
{
 uint32_t a[]={0,1,127,255,32767,(uint32_t)-1,(uint32_t)0x89abcdef}, b[7], c[7];
 psx_state_writer_t x={0}, y={0};
 psx_sw_u32_array(&x,a,7);
 for(int i=0;i<7;i++) psx_sw_u32(&y,a[i]);
 assert(x.size==y.size && !memcmp(x.buf,y.buf,x.size));
 for(size_t n=0;n<=x.size;n++) {
  psx_state_reader_t r,q; psx_sr_init(&r,x.buf,n); psx_sr_init(&q,x.buf,n);
  psx_sr_u32_array(&r,b,7);
  for(int i=0;i<7;i++) c[i]=(uint32_t)psx_sr_u32(&q);
  assert(!memcmp(b,c,sizeof(b)) && r.offset==q.offset && r.error==q.error);
 }
 psx_sw_free(&x); psx_sw_free(&y);
}
{
 int32_t a[]={0,1,127,255,32767,(int32_t)-1,(int32_t)0x89abcdef}, b[7], c[7];
 psx_state_writer_t x={0}, y={0};
 psx_sw_i32_array(&x,a,7);
 for(int i=0;i<7;i++) psx_sw_u32(&y,a[i]);
 assert(x.size==y.size && !memcmp(x.buf,y.buf,x.size));
 for(size_t n=0;n<=x.size;n++) {
  psx_state_reader_t r,q; psx_sr_init(&r,x.buf,n); psx_sr_init(&q,x.buf,n);
  psx_sr_i32_array(&r,b,7);
  for(int i=0;i<7;i++) c[i]=(int32_t)psx_sr_u32(&q);
  assert(!memcmp(b,c,sizeof(b)) && r.offset==q.offset && r.error==q.error);
 }
 psx_sw_free(&x); psx_sw_free(&y);
}
psx_state_writer_t w={0};
for(int i=0;i<524288;i++) frame[i]=(uint16_t)i;
clock_t start=clock();
for(int i=0;i<120;i++) {
 w.size=0; psx_sw_u16_array(&w,frame,524288);
 psx_state_reader_t r; psx_sr_init(&r,w.buf,w.size);
 psx_sr_u16_array(&r,decoded,524288);
 assert(!r.error && !memcmp(frame,decoded,sizeof(frame)));
}
printf("Array compatibility tests passed; 120 VRAM round trips: %.3f ms\n",1000.0*(clock()-start)/CLOCKS_PER_SEC);
psx_sw_free(&w); return 0;
}
