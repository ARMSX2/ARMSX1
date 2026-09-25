#include "../psx/psx.h"
#include "../psx/state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
int main(void) {
 FILE* f=fopen("blank-bios.bin","wb"); assert(f);
 unsigned char block[4096]={0};
 for(int i=0;i<128;i++) assert(fwrite(block,1,sizeof(block),f)==sizeof(block));
 fclose(f);
 psx_t* p=psx_create(); assert(p && psx_init(p,"blank-bios.bin",NULL)==0);
 void *data=NULL,*again=NULL; size_t cap=0,size=0,cap2=0,size2=0;
 p->cpu->pc=0x80001000; p->cpu->next_pc=0x80001004;
 psx_bus_write32(p->bus,0x1000,0x24010007); /* addiu r1,zero,7 */
 assert(psx_save_state_to_memory_ex(p,&data,&cap,&size,PSX_STATE_SAVE_NO_THUMBNAIL)==0);
 /* Populate the cache with a different opcode at the same address. Restore
    must execute the saved opcode even while retaining decoded entries. */
 psx_bus_write32(p->bus,0x1000,0x24010009);
 psx_cpu_cycle(p->cpu); assert(p->cpu->r[1]==9);
 assert(psx_load_state_from_memory_ex(p,data,size,PSX_STATE_LOAD_KEEP_DECODE_CACHE)==0);
 psx_cpu_cycle(p->cpu); assert(p->cpu->r[1]==7);
 assert(psx_load_state_from_memory_ex(p,data,size,PSX_STATE_LOAD_KEEP_DECODE_CACHE)==0);
 assert(psx_save_state_to_memory_ex(p,&again,&cap2,&size2,PSX_STATE_SAVE_NO_THUMBNAIL)==0);
 /* Loading normalizes derived GPU status in the existing implementation.
    Compare a second roundtrip against that canonical state. */
 assert(psx_load_state_from_memory_ex(p,again,size2,PSX_STATE_LOAD_KEEP_DECODE_CACHE)==0);
 assert(psx_save_state_to_memory_ex(p,&data,&cap,&size,PSX_STATE_SAVE_NO_THUMBNAIL)==0);
 assert(size==size2 && !memcmp(data,again,size));
 clock_t start=clock();
 for(int i=0;i<120;i++) {
  assert(psx_save_state_to_memory_ex(p,&data,&cap,&size,PSX_STATE_SAVE_NO_THUMBNAIL)==0);
  p->ram->buf[123]=99; p->gpu->vram[456]=123;
  assert(psx_load_state_from_memory_ex(p,data,size,PSX_STATE_LOAD_KEEP_DECODE_CACHE)==0);
  assert(p->ram->buf[123]==0 && p->gpu->vram[456]==0);
 }
 printf("Full-state roundtrip and changed-opcode checks passed; 120 saves/restores: %.3f ms; bytes=%zu\n",1000.0*(clock()-start)/CLOCKS_PER_SEC,size);
 free(data); free(again); psx_destroy(p); remove("blank-bios.bin"); return 0;
}
