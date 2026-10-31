#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "psx/dev/cdrom/cue.h"

static void parse_case(const char* text, int success) {
    char path[] = "/tmp/armsx-cue-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE* file = fdopen(fd, "wb");
    assert(file && fwrite(text, 1, strlen(text), file) == strlen(text) && fclose(file) == 0);
    cue_t* cue = cue_create();
    cue_init(cue);
    assert((cue_parse(cue, path) == CUE_OK) == success);
    cue_destroy(cue);
    assert(unlink(path) == 0);
}

int main(void) {
    parse_case("FILE \"game.bin\" BINARY\n TRACK 01 MODE2/2352\n INDEX 01 00:00:00\n", 1);
    parse_case("\xef\xbb\xbf" "file \"game.bin\" binary\n track 01 mode2/2352\n index 01 00:00:00\n", 1);
    parse_case("FILE \"unterminated", 0);
    parse_case("FILE \"unterminated\n TRACK 01 MODE2/2352\n", 0);
    parse_case("TRACK 01 MODE2/2352\n", 0);
    parse_case("INDEX 01 00:00:00\n", 0);
    parse_case("FILE \"game.bin\" BINARY\n", 0);
    parse_case("FILE \"game.bin\" BINARY\n TRACK 999999999999999999999 MODE2/2352\n", 0);
    char long_keyword[1024];
    memset(long_keyword, 'A', sizeof(long_keyword) - 1);
    long_keyword[sizeof(long_keyword) - 1] = 0;
    parse_case(long_keyword, 0);
    puts("CUE quoted-file, field bounds, ordering, BOM and case checks passed");
    return 0;
}
