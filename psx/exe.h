#ifndef EXE_H
#define EXE_H

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

#include "cpu.h"
#include "bus_init.h"

/*
PSX executables are having an 800h-byte header, followed by the code/data.

  000h-007h ASCII ID "PS-X EXE"
  008h-00Fh Zerofilled
  010h      Initial PC                   (usually 80010000h, or higher)
  014h      Initial GP/R28               (usually 0)
  018h      Destination Address in RAM   (usually 80010000h, or higher)
  01Ch      Filesize (must be N*800h)    (excluding 800h-byte header)
  020h      Unknown/Unused ;Addr         (usually 0)  ;\optional overlay?
  024h      Unknown/Unused ;Size         (usually 0)  ;/(not auto-loaded)
  028h      Memfill Start Address        (usually 0) (when below Size=None)
  02Ch      Memfill Size in bytes        (usually 0) (0=None)
  030h      Initial SP/R29 & FP/R30 Base (usually 801FFFF0h) (or 0=None)
  034h      Initial SP/R29 & FP/R30 Offs (usually 0, added to above Base)
  038h-04Bh Reserved for A(43h) Function (should be zerofilled in exefile)
  04Ch-xxxh ASCII marker
             "Sony Computer Entertainment Inc. for Japan area"         ;NTSC
             "Sony Computer Entertainment Inc. for Europe area"        ;PAL
             "Sony Computer Entertainment Inc. for North America area" ;NTSC
             (or often zerofilled in some homebrew files)
             (the BIOS doesn't verify this string, and boots fine without it)
  xxxh-7FFh Zerofilled
  800h...   Code/Data                  (loaded to entry[018h] and up)
*/

typedef struct {
    char id[16];
    uint32_t ipc;
    uint32_t igp;
    uint32_t ramdest;
    uint32_t filesz;
    uint32_t unk20;
    uint32_t unk24;
    uint32_t mfill_start;
    uint32_t mfill_size;
    uint32_t ispb;
    uint32_t ispoff;
} psx_exe_hdr_t;

int psx_exe_load(psx_cpu_t*, const char*);

#endif