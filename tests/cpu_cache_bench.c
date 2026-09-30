/* Synthetic emulation-loop benchmark: run from a directory with build/tests.
   Compare identical compiler flags and CPU affinity; this is not a game FPS claim. */
#define main cpu_differential_main
#include "cpu_differential.c"
#undef main
#include <time.h>
#include "psx/log.h"
static double seconds(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void) {
 const char* bios="build/tests/blank-bios.bin"; if(!write_blank_bios(bios))return 1;
 log_set_quiet(1);
 for(unsigned words=1024;words<=16384;words*=4) {
  psx_t* p=psx_create();if(psx_init(p,bios,NULL))return 1;
  for(unsigned i=0;i<words-2;i++) {
   unsigned op=(i%3==0)?0x24420001u:(i%3==1)?0x38431234u:0x00622021u;
   psx_bus_write32(p->bus,TEST_OFFSET+i*4,op);
  }
  psx_bus_write32(p->bus,TEST_OFFSET+(words-2)*4,0x08000400u);
  psx_bus_write32(p->bus,TEST_OFFSET+(words-1)*4,0);
  p->cpu->pc=TEST_PC;p->cpu->next_pc=TEST_PC+4;
  for(unsigned i=0;i<100000;i++)psx_update(p);
  for(unsigned run=0;run<3;run++) {
   double begin=seconds();for(unsigned i=0;i<2000000;i++)psx_update(p);
   printf("words=%u run=%u ms=%.3f r2=%u cycles=%u\n",words,run,(seconds()-begin)*1000,p->cpu->r[2],p->cpu->total_cycles);
  }
  psx_destroy(p);
 }
 return 0;
}
