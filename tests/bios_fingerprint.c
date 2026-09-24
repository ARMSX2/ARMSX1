#include "../psx/psx.h"
#include "../psx/state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static void write_bios(const char* name, size_t size, unsigned char pattern) {
    FILE* f=fopen(name,"wb"); assert(f);
    for(size_t i=0;i<size;i++) assert(fputc(pattern^(i&255),f)!=EOF);
    assert(fclose(f)==0);
}
int main(void) {
    write_bios("bios-a.bin",PSX_BIOS_SIZE,17);
    write_bios("bios-b.bin",PSX_BIOS_SIZE,29);
    write_bios("bios-big.bin",PSX_BIOS_SIZE*2,53);
    write_bios("bios-invalid.bin",7,1);
    psx_t* p=psx_create(); assert(p && psx_init(p,"bios-a.bin",NULL)==0);
    void* state=NULL; size_t cap=0,size=0;
    assert(psx_save_state_to_memory_ex(p,&state,&cap,&size,PSX_STATE_SAVE_NO_THUMBNAIL)==0);
    assert(psx_bios_fingerprint(p->bios)==psx_state_fnv1a(p->bios->buf,p->bios->io_size,PSX_STATE_FNV_SEED));
    assert(psx_bios_load(p->bios,"bios-b.bin")==0);
    assert(psx_load_state_from_memory(p,state,size)==PSX_STATE_ERR_WRONG_BIOS);
    assert(psx_bios_load(p->bios,"bios-a.bin")==0);
    assert(psx_load_state_from_memory(p,state,size)==0);
    assert(psx_bios_load(p->bios,"bios-invalid.bin")!=0);
    assert(psx_load_state_from_memory(p,state,size)==0);
    assert(psx_bios_load(p->bios,"bios-big.bin")==0);
    assert(psx_bios_fingerprint(p->bios)==psx_state_fnv1a(p->bios->buf,p->bios->io_size,PSX_STATE_FNV_SEED));
    assert(psx_load_state_from_memory(p,state,size)==PSX_STATE_ERR_WRONG_BIOS);
    free(state); psx_destroy(p);
    remove("bios-a.bin"); remove("bios-b.bin"); remove("bios-big.bin"); remove("bios-invalid.bin");
    puts("BIOS fingerprint reload, wrong-BIOS refusal and 1 MiB checks passed");
    return 0;
}
