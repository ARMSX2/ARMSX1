#include <stdio.h>

#include "exe.h"
#include "log.h"

int psx_exe_load(psx_cpu_t* cpu, const char* path) {
    if (!path)
        return 0;

    FILE* file = fopen(path, "rb");

    if (!file)
        return 1;

    // Read header
    psx_exe_hdr_t hdr;
    
    if (!fread((char*)&hdr, 1, sizeof(psx_exe_hdr_t), file))
        return 2;

    // Seek to program start 
    fseek(file, 0x800, SEEK_SET);

    // Read to RAM directly
    uint32_t offset = hdr.ramdest & 0x7fffffff;

    if (!fread(cpu->bus->ram->buf + offset, 1, hdr.filesz, file))
        return 3;

    psx_cpu_invalidate_range(cpu, offset, hdr.filesz);

    // Load initial register values
    cpu->pc = hdr.ipc;
    cpu->next_pc = cpu->pc + 4;
    cpu->r[28] = hdr.igp;

    if (hdr.ispb) {
        cpu->r[29] = hdr.ispb + hdr.ispoff;
        cpu->r[30] = cpu->r[29];
    }

    log_info("Loaded PS-X EXE file \"%s\"", path);
    log_fatal("PC=%08x SP=%08x (%08x) GP=%08x", cpu->pc, cpu->r[29], hdr.ispb, cpu->r[28]);

    fclose(file);

    return 0;
}
