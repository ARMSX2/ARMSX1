#include <stdio.h>
#include <stdlib.h>
__attribute__((noinline)) int check(float width, float height, float top, float bottom) {
 int bad=0;
 float area=width*height;
 for(int y=0;y<(int)height;y++) for(int x=0;x<(int)width;x++) {
  if(x*height+y*width>area) continue;
  float z0=area-x*height-y*width,z1=x*height,z2=y*width;
  float v=(z0*top+z1*top+z2*bottom)/area;
  int expected=((int)top*((int)height-y)+(int)bottom*y)/(int)height;
  if((int)v!=expected) { if(bad<4) printf("x=%d y=%d sample=%.9g row=%d expected=%d\n",x,y,v,(int)v,expected); bad++; }
 }
 return bad;
}
int main(int argc,char**argv) {
 float w=argc>1?atof(argv[1]):256, h=argc>2?atof(argv[2]):16;
 int bad=check(w,h,68,83)+check(200,38,60,99)+check(96,16,64,79);
 printf("Incorrect texture rows: %d\n",bad);return bad?1:0;
}
