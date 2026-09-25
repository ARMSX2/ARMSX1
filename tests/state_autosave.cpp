#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
extern "C" {
#include "../psx/psx.h"
#include "../psx/state.h"
}

#ifdef PSX_STATE_QUEUE_TEST
static std::mutex queueMutex;
static std::condition_variable queueChanged;
static int blockedStage;
static bool released;
static bool seen[5];

static void queueTestHook(int stage) {
    std::unique_lock<std::mutex> lock(queueMutex);
    seen[stage] = true;
    queueChanged.notify_all();
    if (stage == PSX_STATE_QUEUE_PUBLISHED && blockedStage == PSX_STATE_QUEUE_CLAIMED)
        queueChanged.wait(lock, [] { return seen[PSX_STATE_QUEUE_CLAIMED]; });
    if (stage == blockedStage)
        queueChanged.wait(lock, [] { return released; });
}

static void blockQueue(int stage) {
    std::lock_guard<std::mutex> lock(queueMutex);
    memset(seen, 0, sizeof(seen));
    blockedStage = stage;
    released = false;
}

static void waitQueue(int stage) {
    std::unique_lock<std::mutex> lock(queueMutex);
    assert(queueChanged.wait_for(lock, std::chrono::seconds(3), [=] { return seen[stage]; }));
}

static void releaseQueue() {
    std::lock_guard<std::mutex> lock(queueMutex);
    released = true;
    queueChanged.notify_all();
}

static void checkQueueOwnership(psx_t* p, const char* root) {
    psx_state_set_queue_test_hook(queueTestHook);
    int result = PSX_STATE_ERR_TIMEOUT;
    std::atomic<bool> done{false};
    blockQueue(PSX_STATE_QUEUE_RESERVED);
    std::thread first([&] {
        result = psx_state_request_slot(PSX_STATE_OP_SAVE, 20, root, 3000);
        done.store(true);
    });
    waitQueue(PSX_STATE_QUEUE_RESERVED);
    assert(psx_state_request_slot(PSX_STATE_OP_SAVE, 21, root, 0) == PSX_STATE_ERR_BUSY);
    releaseQueue();
    while (!done.load()) {
        psx_state_service_requests();
        std::this_thread::yield();
    }
    first.join();
    assert(result == PSX_STATE_OK);
    char game[1024];
    assert(psx_state_slot_info(20, root, game, sizeof(game)));
    assert(!psx_state_slot_info(21, root, game, sizeof(game)));

    done.store(false);
    blockQueue(PSX_STATE_QUEUE_CLAIMED);
    std::thread timed([&] {
        result = psx_state_request_slot(PSX_STATE_OP_SAVE, 22, root, 100);
        done.store(true);
    });
    waitQueue(PSX_STATE_QUEUE_PUBLISHED);
    std::thread consumer([] { psx_state_service_requests(); });
    waitQueue(PSX_STATE_QUEUE_CLAIMED);
    waitQueue(PSX_STATE_QUEUE_WAITING);
    assert(!done.load());
    psx_state_service_requests();
    assert(!done.load());
    assert(psx_state_request_slot(PSX_STATE_OP_SAVE, 23, root, 0) == PSX_STATE_ERR_BUSY);
    releaseQueue();
    consumer.join();
    timed.join();
    assert(result == PSX_STATE_OK);
    assert(psx_state_slot_info(22, root, game, sizeof(game)));
    assert(!psx_state_slot_info(23, root, game, sizeof(game)));

    blockQueue(0);
    assert(psx_state_request_slot(PSX_STATE_OP_SAVE, 23, root, 0) == PSX_STATE_ERR_TIMEOUT);
    psx_state_service_requests();
    assert(!psx_state_slot_info(23, root, game, sizeof(game)));
    assert(psx_state_request_slot(PSX_STATE_OP_SAVE, -2, root, 0) == PSX_STATE_ERR_ARG);
    assert(psx_state_request_slot(PSX_STATE_OP_SAVE, 23, "", 0) == PSX_STATE_ERR_ARG);

    blockQueue(0);
    std::thread stale([&] {
        result = psx_state_request_slot(PSX_STATE_OP_SAVE, 24, root, 3000);
    });
    waitQueue(PSX_STATE_QUEUE_PUBLISHED);
    psx_state_set_machine(nullptr);
    psx_state_set_machine(p);
    psx_state_service_requests();
    stale.join();
    assert(result == PSX_STATE_ERR_NO_MACHINE);
    assert(!psx_state_slot_info(24, root, game, sizeof(game)));
    psx_state_set_queue_test_hook(nullptr);
}
#endif

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

int main(int argc, char** argv) {
    const char* temp = argc > 1 ? argv[1] : getenv("TMPDIR");
    if (!temp || !*temp) {
#ifdef __ANDROID__
        temp = "/data/local/tmp";
#else
        temp = "/tmp";
#endif
    }
    const std::string pattern = std::string(temp) + "/armsx-autosave-test-XXXXXX";
    std::vector<char> directory(pattern.begin(), pattern.end());
    directory.push_back('\0');
    const char* root = mkdtemp(directory.data());
    assert(root);
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
#ifdef PSX_STATE_QUEUE_TEST
    checkQueueOwnership(p, root);
#endif
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
    struct rlimit previousLimit, shortLimit;
    assert(getrlimit(RLIMIT_FSIZE, &previousLimit) == 0);
    shortLimit = previousLimit;
    shortLimit.rlim_cur = 1024;
    const auto previousSignal = std::signal(SIGXFSZ, SIG_IGN);
    assert(previousSignal != SIG_ERR && setrlimit(RLIMIT_FSIZE, &shortLimit) == 0);
    p->ram->buf[99] = 202;
    const int shortWrite = request(PSX_STATE_OP_SAVE, PSX_STATE_SLOT_AUTOSAVE, root);
    assert(setrlimit(RLIMIT_FSIZE, &previousLimit) == 0);
    assert(std::signal(SIGXFSZ, previousSignal) != SIG_ERR);
    assert(shortWrite == PSX_STATE_ERR_IO);
    assert(request(PSX_STATE_OP_LOAD, PSX_STATE_SLOT_AUTOSAVE, root) == 0);
    assert(p->ram->buf[99] == 201);
    const std::string blockedPath = std::string(root) + "/savestates/blocked.pss";
    assert(mkdir(blockedPath.c_str(), 0700) == 0);
    assert(psx_save_state(p, blockedPath.c_str()) == PSX_STATE_ERR_IO);
    assert(std::filesystem::is_directory(blockedPath));
    for (const auto& entry : std::filesystem::directory_iterator(std::string(root) + "/savestates"))
        assert(entry.path().filename().string().find(".tmp.") == std::string::npos);
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
    std::filesystem::remove_all(root);
}
