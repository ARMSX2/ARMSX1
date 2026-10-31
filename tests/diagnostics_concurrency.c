#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE 1
#define PSXE_DIAG_STDIO_DISABLE
#include "frontend/diagnostics.h"
#include <SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int writer(void* data) {
    char message[1024];
    memset(message, 'a' + (int)(intptr_t)data, sizeof(message) - 1);
    message[sizeof(message) - 1] = 0;
    for (int i = 0; i < 6000; i++) {
        psxe_diag_logf("stress", "%d %s", i, message);
        if ((i % 100) == 0) psxe_diag_breadcrumbf("writer=%ld step=%d", (long)(intptr_t)data, i);
    }
    return 0;
}

static int toggle(void* data) {
    const char* path = data;
    for (int i = 0; i < 100; i++) {
        psxe_diag_set_enabled(0);
        psxe_diag_initialize(path);
        psxe_diag_set_enabled(1);
        SDL_Delay(1);
    }
    return 0;
}

int main(void) {
    char directory[] = "/tmp/armsx-diagnostics-XXXXXX";
    assert(mkdtemp(directory));
    char pref[1024], log[1024], previous[1040], logs[1024];
    snprintf(pref, sizeof(pref), "%s/", directory);
    snprintf(logs, sizeof(logs), "%s/logs", directory);
    snprintf(log, sizeof(log), "%s/logs/armsx.log", directory);
    snprintf(previous, sizeof(previous), "%s.previous", log);
    psxe_diag_initialize(pref);
    psxe_diag_set_enabled(1);
    SDL_Thread* threads[5];
    for (int i = 0; i < 4; i++) threads[i] = SDL_CreateThread(writer, "writer", (void*)(intptr_t)i);
    threads[4] = SDL_CreateThread(toggle, "toggle", pref);
    for (int i = 0; i < 5; i++) {
        assert(threads[i]);
        SDL_WaitThread(threads[i], NULL);
    }
    writer(NULL);
    psxe_diag_dump_breadcrumbs();
    psxe_diag_shutdown();
    struct stat current_stat, previous_stat;
    assert(stat(log, &current_stat) == 0);
    assert(stat(previous, &previous_stat) == 0);
    assert(current_stat.st_size <= 4 * 1024 * 1024 + 4096);
    assert(previous_stat.st_size <= 4 * 1024 * 1024 + 4096);
    psxe_diag_set_enabled(0);
    psxe_diag_logf("disabled", "%s", "must not be written");
    struct stat after;
    assert(stat(log, &after) == 0 && after.st_size == current_stat.st_size);
    assert(unlink(log) == 0);
    assert(unlink(previous) == 0);
    assert(rmdir(logs) == 0);
    assert(rmdir(directory) == 0);
    puts("DIAGNOSTICS_CONCURRENCY passed rotation, concurrent lifecycle, disabled capture");
    return 0;
}
