#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    uint8_t block[4096] = {0x00, 0x00, 0xf0, 0x0b};
    FILE* bios = fopen("bios.bin", "wb");
    assert(bios);
    assert(fwrite(block, 1, sizeof(block), bios) == sizeof(block));
    memset(block, 0, sizeof(block));
    for (int i = 1; i < 128; ++i)
        assert(fwrite(block, 1, sizeof(block), bios) == sizeof(block));
    assert(fclose(bios) == 0);

    FILE* disc = fopen("disc.bin", "wb");
    assert(disc);
    for (int lba = 0; lba < 75; ++lba) {
        uint8_t sector[2352] = {0};
        memset(sector + 1, 0xff, 10);
        sector[15] = 2;
        if (lba == 16) {
            sector[24] = 1;
            memcpy(sector + 25, "CD001", 5);
            memcpy(sector + 32, "PLAYSTATION                     ", 32);
        }
        assert(fwrite(sector, 1, sizeof(sector), disc) == sizeof(sector));
    }
    assert(fclose(disc) == 0);
    puts("Synthetic BIOS loop and blank disc fixtures created");
    return 0;
}
