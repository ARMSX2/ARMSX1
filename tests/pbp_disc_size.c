/* Synthetic PBP regressions; no game data is needed.
   Build with pbp.c, psx/log.c and third_party/libchdr/deps/miniz-3.1.1/miniz.c. */
#include "psx/dev/cdrom/pbp.h"
#include "miniz.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void put32(FILE* f, long off, unsigned v) {
    unsigned char b[4] = {v, v >> 8, v >> 16, v >> 24};
    assert(!fseek(f, off, SEEK_SET)); assert(fwrite(b, 1, 4, f) == 4);
}
static unsigned char bcd(unsigned v) { return (v / 10) * 16 + v % 10; }
static void fixture(const char* path, unsigned sectors, int toc, int compressed, int marker, int invalid) {
    const unsigned base = 0x28, data = 0x110000, blocks = (sectors + 15) / 16;
    unsigned char raw[16 * CD_SECTOR_SIZE]; memset(raw, 0x5a, sizeof(raw));
    size_t size = sizeof(raw);
    void* packed = compressed ? tdefl_compress_mem_to_heap(raw, sizeof(raw), &size, 0) : raw;
    assert(packed);
    FILE* f = fopen(path, "wb+"); assert(f);
    assert(fwrite("\0PBP", 1, 4, f) == 4);
    put32(f, 0x24, base);
    fseek(f, base, SEEK_SET); fwrite("PSISOIMG0000", 1, 12, f);
    /* Reproduce the erroneous 156261-sector limit in the reported Dino 2 boot. */
    put32(f, base + 12, 156261u * CD_SECTOR_SIZE);
    put32(f, base + 0xbfc, data);
    if (toc) {
        unsigned end = sectors + 150;
        unsigned char rows[40] = {
            0x41,0,0xa0,0,0,0,0,1,0,0,
            0x41,0,0xa1,0,0,0,0,1,0,0,
            0x41,0,0xa2,0,0,0,0,0,0,0,
            0x41,0,1,0,0,0,0,0,2,0};
        rows[27]=bcd(end/4500); rows[28]=bcd(end/75%60); rows[29]=bcd(end%75);
        fseek(f, base+0x800, SEEK_SET); fwrite(rows, 1, sizeof(rows), f);
    }
    for (unsigned i=0; i<blocks; ++i) {
        put32(f, base+0x4000+i*32, 0);
        put32(f, base+0x4004+i*32, (invalid && i==blocks-1) ? 0 : (unsigned)size | (marker ? 0x10000 : 0));
    }
    fseek(f, base+data, SEEK_SET); assert(fwrite(packed, 1, size, f)==size);
    fclose(f); if (compressed) free(packed);
}
int main(int argc, char** argv) {
    assert(argc==2);
    for (int compressed=0; compressed<=1; ++compressed) {
        fixture(argv[1], 230001, 1, compressed, 1, 0);
        pbp_t* p=pbp_create(); pbp_init(p); assert(pbp_load(p, argv[1])==0);
        unsigned char sector[CD_SECTOR_SIZE];
        assert(pbp_query(p, 201392)==TS_DATA);
        assert(pbp_read_sector(p, 201392, sector)==TS_DATA);
        for (unsigned j=0;j<sizeof(sector);++j) assert(sector[j]==0x5a);
        assert(pbp_query(p, 230150)==TS_DATA);
        assert(pbp_read_sector(p, 230150, sector)==TS_DATA);
        assert(pbp_query(p, 230151)==TS_FAR);
        assert(pbp_read_sector(p, 230151, sector)==TS_FAR);
        assert(pbp_get_track_lba(p, 0)==230151);
        assert(pbp_get_track_lba(p, 1)==150);
        assert(pbp_query(p, 149)==TS_PREGAP);
        pbp_destroy(p);
    }
    fixture(argv[1], 32, 0, 0, 0, 0);
    pbp_t* p=pbp_create(); pbp_init(p); assert(!pbp_load(p, argv[1]));
    assert(pbp_get_track_lba(p, 1)==150);
    assert(pbp_query(p,181)==TS_DATA); assert(pbp_query(p,182)==TS_FAR);
    pbp_destroy(p);
    fixture(argv[1], 32, 1, 0, 0, 1);
    p=pbp_create(); pbp_init(p); assert(pbp_load(p, argv[1])!=0); pbp_destroy(p);
    remove(argv[1]); puts("PBP high-LBA, compressed/raw, marker, boundaries and malformed-index tests passed");
    return 0;
}
