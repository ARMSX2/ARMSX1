#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <sys/stat.h>
extern "C" {
#include "../psx/psx.h"
#include "../psx/state.h"
}

// Exercise the actual cross-thread slot queue without advancing the CPU,
// matching a save made while the pause menu is open. Fixtures are synthetic
// and live in a new temporary directory; no user BIOS, cards or saves are used.
static int request(int op, int slot, const char* root) {
    std::atomic<bool> done{false};
    int result = PSX_STATE_ERR_TIMEOUT;
    std::thread worker([&] {
        result = psx_state_request_slot(op, slot, root, 3000);
        done.store(true);
    });
    while (!done.load()) {
        psx_state_service_requests();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    worker.join();
    return result;
}

static void fixture(const std::string& path, size_t bytes, int value) {
    FILE* f = fopen(path.c_str(), "wb");
    assert(f);
    for (size_t i = 0; i < bytes; ++i) assert(fputc(value, f) != EOF);
    assert(fclose(f) == 0);
}

static void discFixture(const std::string& path, int value) {
    FILE* f = fopen(path.c_str(), "wb");
    assert(f);
    for (int lba = 0; lba < 75; ++lba) {
        unsigned char sector[2352] = {};
        memset(sector + 1, 0xff, 10);
        sector[15] = 2;
        memset(sector + 24, value, 2048);
        if (lba == 16) {
            sector[24] = 1;
            memcpy(sector + 25, "CD001", 5);
            memcpy(sector + 32, "PLAYSTATION                     ", 32);
        }
        assert(fwrite(sector, 1, sizeof(sector), f) == sizeof(sector));
    }
    assert(fclose(f) == 0);
}

int main() {
    char root[] = "/data/local/tmp/armsx-autosave-test-XXXXXX";
    assert(mkdtemp(root));
    const std::string bios = std::string(root) + "/bios.bin";
    // SAF launches use a fresh parent directory each time. Single-track games
    // often have identical TOCs, so their stable basenames must distinguish them.
    const std::string sessionA = std::string(root) + "/session-a";
    const std::string sessionB = std::string(root) + "/session-b";
    const std::string sessionA2 = std::string(root) + "/session-a-reopened";
    assert(mkdir(sessionA.c_str(), 0700) == 0);
    assert(mkdir(sessionB.c_str(), 0700) == 0);
    assert(mkdir(sessionA2.c_str(), 0700) == 0);
#ifdef TEST_SHARED_SAF_NAME
    const std::string nameA = "game.bin", nameB = "game.bin";
#else
    const std::string nameA = "game-" + std::string(64, 'a') + ".bin";
    const std::string nameB = "game-" + std::string(64, 'b') + ".bin";
#endif
    const std::string discA = sessionA + "/" + nameA;
    const std::string discB = sessionB + "/" + nameB;
    const std::string discA2 = sessionA2 + "/" + nameA;
    fixture(bios, PSX_BIOS_SIZE, 0);
    discFixture(discA, 0);
    discFixture(discB, 1);
    discFixture(discA2, 0);
    auto boot = [&](const std::string& disc) {
        psx_t* p = psx_create();
        assert(p && psx_init(p, bios.c_str(), nullptr) == 0);
        assert(psx_cdrom_open(p->cdrom, disc.c_str()) != 0);
        // A real BIOS programs the display before a game can be paused.
        p->gpu->display_mode = 1;
        p->gpu->disp_y1 = 16;
        p->gpu->disp_y2 = 256;
        return p;
    };
    psx_t* p = boot(discA);
    char autoPath[1024], slotPath[1024], gamePath[1024];
    assert(psx_state_slot_path(p, PSX_STATE_SLOT_AUTOSAVE, root, autoPath, sizeof(autoPath)) == 0);
    assert(strstr(autoPath, ".autosave.pss"));
    assert(psx_state_slot_path(p, -2, root, slotPath, sizeof(slotPath)) == PSX_STATE_ERR_ARG);
    assert(psx_state_slot_path(p, PSX_STATE_SLOT_AUTOSAVE, root, slotPath, 5) == PSX_STATE_ERR_ARG);
    assert(!psx_state_slot_info(PSX_STATE_SLOT_AUTOSAVE, root, gamePath, sizeof(gamePath)));
    assert(request(PSX_STATE_OP_LOAD, PSX_STATE_SLOT_AUTOSAVE, root) != 0);
    for (int slot = 0; slot < 10; ++slot) {
        assert(psx_state_slot_path(p, slot, root, slotPath, sizeof(slotPath)) == 0);
        assert(strcmp(autoPath, slotPath));
        p->ram->buf[99] = slot;
        assert(request(PSX_STATE_OP_SAVE, slot, root) == 0);
    }
    p->ram->buf[99] = 200;
    assert(request(PSX_STATE_OP_SAVE, PSX_STATE_SLOT_AUTOSAVE, root) == 0);
    p->ram->buf[99] = 201;
    assert(request(PSX_STATE_OP_SAVE, PSX_STATE_SLOT_AUTOSAVE, root) == 0);
    assert(psx_state_slot_info(PSX_STATE_SLOT_AUTOSAVE, root, gamePath, sizeof(gamePath)));
    assert(discA == gamePath);
    void* png = nullptr;
    size_t pngSize = 0;
    assert(psx_state_slot_thumbnail(PSX_STATE_SLOT_AUTOSAVE, root, &png, &pngSize) == 0);
    assert(png && pngSize > 8 && !memcmp(png, "\x89PNG\r\n\x1a\n", 8));
    free(png);
    psx_destroy(p);
    p = boot(discB);
    assert(!psx_state_slot_info(PSX_STATE_SLOT_AUTOSAVE, root, gamePath, sizeof(gamePath)));
    assert(psx_load_state(p, autoPath) == PSX_STATE_ERR_WRONG_DISC);
    p->ram->buf[99] = 208;
    assert(request(PSX_STATE_OP_SAVE, PSX_STATE_SLOT_AUTOSAVE, root) == 0);
    psx_destroy(p);
    p = boot(discA2);
    assert(psx_state_slot_path(p, PSX_STATE_SLOT_AUTOSAVE, root, slotPath, sizeof(slotPath)) == 0);
    assert(strcmp(autoPath, slotPath) == 0);
    assert(request(PSX_STATE_OP_LOAD, PSX_STATE_SLOT_AUTOSAVE, root) == 0);
    assert(p->ram->buf[99] == 201);
    for (int slot = 0; slot < 10; ++slot) {
        assert(request(PSX_STATE_OP_LOAD, slot, root) == 0);
        assert(p->ram->buf[99] == slot);
    }
    psx_destroy(p);
    assert(request(PSX_STATE_OP_SAVE, PSX_STATE_SLOT_AUTOSAVE, root) == PSX_STATE_ERR_NO_MACHINE);
    printf("AUTO_SAVE PASS: paused queue, separate games/slots, replacement, restart/load, preview and invalid paths (%s)\n", root);
}
