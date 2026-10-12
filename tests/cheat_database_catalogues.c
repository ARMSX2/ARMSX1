/* Gate the generated catalogues against the real PS1 parser and arming logic.
   No game is booted and no cheats are applied to RAM. */
#include "psx/cheats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void log_log(int level, const char* file, int line, const char* format, ...) {
    (void)level; (void)file; (void)line; (void)format;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    FILE* index = fopen(argv[2], "rb");
    if (!index) return 2;
    char line[8192], path[1024];
    int lists = 0, cheats = 0;
    while (fgets(line, sizeof(line), index)) {
        if (line[0] == '#') continue;
        char* first_tab = strchr(line, '\t');
        if (!first_tab) return 3;
        char* counts = first_tab;
        for (int i = 0; i < 3; i++) {
            counts = strchr(counts + 1, '\t');
            if (!counts) return 3;
        }
        int expected = atoi(counts + 1);
        *first_tab = '\0';
        snprintf(path, sizeof(path), "%s/%s.cht", argv[1], line);
        int actual = psx_cheats_load_file(path);
        if (actual != expected || psx_cheats_unsupported_types()[0]) {
            fprintf(stderr, "Catalogue mismatch %s: wanted %d, got %d\n", line, expected, actual);
            return 1;
        }
        const char** names = calloc((size_t)actual, sizeof(*names));
        if (!names) return 4;
        for (int i = 0; i < actual; i++) {
            names[i] = psx_cheats_name(i);
            if (!names[i][0] || psx_cheats_line_count(i) <= 0 || psx_cheats_has_unsupported(i)) return 1;
        }
        int missing = 0;
        if (psx_cheats_arm(1, names, actual, &missing) != actual || missing) return 1;
        psx_cheats_arm(0, NULL, 0, NULL);
        free(names);
        lists++;
        cheats += actual;
    }
    fclose(index);
    psx_cheats_shutdown();
    printf("Validated %d catalogues and %d cheats against the native PS1 engine\n", lists, cheats);
    return lists > 1500 ? 0 : 1;
}
