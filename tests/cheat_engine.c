/*
    Host gate for the PS1 cheat engine (psx/cheats.c).

    Runs on the build machine because everything worth checking is pure logic: how a `.cht`
    file parses, which entries a name list arms, and what each GameShark code type does to
    RAM. None of it needs a phone, a disc or a GPU — and all of it is the kind of thing that
    is invisible until a user reports "this cheat does nothing", which is unfalsifiable
    without a test that says what the code types are supposed to mean.

    The machine is real (psx_init against a blank BIOS), so the writes go through the same
    psx_ram_write / psx_cpu_invalidate_range path the emulator uses, not a stand-in.
*/

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx/psx.h"
#include "psx/cheats.h"

static int g_failures = 0;

static void check(int condition, const char* case_name, const char* what) {
    if (!condition) {
        printf("CHEAT_ENGINE failed case=%s check=%s\n", case_name, what);
        g_failures++;
    }
}

static void check_u32(uint32_t got, uint32_t want, const char* case_name, const char* what) {
    if (got != want) {
        printf("CHEAT_ENGINE failed case=%s check=%s got=0x%08X want=0x%08X\n",
               case_name, what, (unsigned)got, (unsigned)want);
        g_failures++;
    }
}

static int write_blank_bios(const char* path) {
    FILE* file = fopen(path, "wb");
    uint8_t block[4096] = {0};
    size_t offset;

    if (!file)
        return 0;

    for (offset = 0; offset < 512u * 1024u; offset += sizeof(block)) {
        if (fwrite(block, 1, sizeof(block), file) != sizeof(block)) {
            fclose(file);
            return 0;
        }
    }

    return fclose(file) == 0;
}

static int write_text(const char* path, const char* text) {
    FILE* file = fopen(path, "wb");

    if (!file)
        return 0;

    fputs(text, file);

    return fclose(file) == 0;
}

/* ------------------------------------------------------------------------ */

static const char* kSampleCht =
    "# A cheat file, as people actually write them.\n"
    "; semicolons comment too\n"
    "\n"
    "[Infinite Health]\n"
    "@desc Keeps the health word pinned.\n"
    "800A0000 0063\n"
    "800A0002 0063\n"
    "\n"
    "[Colon And Glued]\n"
    "800A0010:1234\n"
    "800A00125678\n"
    "\n"
    "[Byte Write]\n"
    "300A0020 007F\n"
    "\n"
    "[Conditional]\n"
    "D00A0030 0001\n"
    "800A0032 4321\n"
    "E00A0034 0002\n"
    "300A0035 0099\n"
    "\n"
    "[Slide]\n"
    "50000502 0001\n"
    "800A0040 1000\n"
    "\n"
    "[Unsupported Type]\n"
    "C00A0050 0001\n";

static int case_parse(const char* path) {
    const char* name = "parse";
    int count;

    check(write_text(path, kSampleCht), name, "write-sample");

    count = psx_cheats_load_file(path);
    check(count == 6, name, "entry-count");

    check(strcmp(psx_cheats_name(0), "Infinite Health") == 0, name, "first-name");
    check(strcmp(psx_cheats_description(0), "Keeps the health word pinned.") == 0, name, "desc");
    check(psx_cheats_line_count(0) == 2, name, "first-lines");

    /* `800A0010:1234` and `800A00125678` are the same code as `800A0010 1234`. */
    check(psx_cheats_line_count(1) == 2, name, "separator-forms");

    check(psx_cheats_has_unsupported(5) != 0, name, "unsupported-flagged");
    check(psx_cheats_has_unsupported(0) == 0, name, "supported-not-flagged");
    check(strcmp(psx_cheats_unsupported_types(), "C0") == 0, name, "unsupported-list");
    check(strcmp(psx_cheats_source_path(), path) == 0, name, "source-path");

    /* The snapshot the host actually reads. Same six entries, same fields, one owned string. */
    {
        char* described = psx_cheats_describe();
        int separators = 0;
        const char* p;

        check(described != NULL, name, "describe-not-null");

        if (described) {
            for (p = described; *p; p++) {
                if (*p == '\x1E')
                    separators++;
            }

            check(separators == 5, name, "describe-record-count");
            check(strncmp(described, "Infinite Health\x1F" "2\x1F" "0\x1F", 19) == 0,
                  name, "describe-first-record");
            check(strstr(described, "Unsupported Type\x1F" "1\x1F" "1") != NULL,
                  name, "describe-unsupported-flag");
            free(described);
        }
    }

    /* A file that is not there is -1, and it must not leave the previous game's list armed. */
    check(psx_cheats_load_file("build/tests/definitely-not-here.cht") == -1, name, "missing-file");

    /* An empty path CLEARS: this is what a game with no cheat file does, and leaving the
       previous disc's catalogue behind is how a cheat follows a user to the wrong game. */
    check(psx_cheats_load_file("") == 0, name, "empty-path-clears");
    check(psx_cheats_count() == 0, name, "cleared-count");
    check(strcmp(psx_cheats_name(0), "") == 0, name, "cleared-name-not-null");
    check(strcmp(psx_cheats_description(-1), "") == 0, name, "negative-index-not-null");

    check(psx_cheats_load_file(path) == 6, name, "reload");

    return 1;
}

static int case_arm(void) {
    const char* name = "arm";
    const char* wanted[3];
    int missing = -1;
    int armed;

    /* Case and surrounding space are ignored, so a hand-edited settings.toml behaves the way
       it reads. A name with no entry is REPORTED, not silently dropped. */
    wanted[0] = "  infinite HEALTH ";
    wanted[1] = "Byte Write";
    wanted[2] = "A Cheat That Was Renamed";

    armed = psx_cheats_arm(1, wanted, 3, &missing);
    check(armed == 2, name, "armed-count");
    check(missing == 1, name, "missing-count");
    check(psx_cheats_armed_count() == 2, name, "armed-count-readback");

    /* Master off disarms regardless of the list. */
    check(psx_cheats_arm(0, wanted, 3, NULL) == 0, name, "master-off");
    check(psx_cheats_armed_count() == 0, name, "master-off-readback");

    check(psx_cheats_arm(1, NULL, 0, NULL) == 0, name, "null-names");

    return 1;
}

static int case_apply(const char* bios_path) {
    const char* name = "apply";
    const char* wanted[5];
    psx_t* psx = psx_create();
    uint32_t before;

    check(psx != NULL, name, "create");

    if (!psx || psx_init(psx, bios_path, NULL) != 0) {
        printf("CHEAT_ENGINE failed case=%s check=init\n", name);
        g_failures++;
        return 0;
    }

    /* Nothing armed: the fast path must leave memory alone. */
    psx_cheats_arm(0, NULL, 0, NULL);
    psx_ram_write16(psx->ram, 0x0A0000, 0xDEAD);
    psx_cheats_apply(psx);
    psx_cheats_apply(psx);
    check_u32(psx_ram_read16(psx->ram, 0x0A0000), 0xDEAD, name, "inactive-no-write");

    wanted[0] = "Infinite Health";
    wanted[1] = "Colon And Glued";
    wanted[2] = "Byte Write";
    wanted[3] = "Conditional";
    wanted[4] = "Slide";
    check(psx_cheats_arm(1, wanted, 5, NULL) == 5, name, "arm-five");

    /* Conditional setup: the D0 test passes, the E0 test fails. */
    psx_ram_write16(psx->ram, 0x0A0030, 0x0001);
    psx_ram_write8(psx->ram, 0x0A0034, 0x00);
    psx_ram_write16(psx->ram, 0x0A0032, 0x0000);
    psx_ram_write8(psx->ram, 0x0A0035, 0x00);

    psx_cheats_apply(psx);

    check_u32(psx_ram_read16(psx->ram, 0x0A0000), 0x0063, name, "write16-a");
    check_u32(psx_ram_read16(psx->ram, 0x0A0002), 0x0063, name, "write16-b");
    check_u32(psx_ram_read16(psx->ram, 0x0A0010), 0x1234, name, "write16-colon");
    check_u32(psx_ram_read16(psx->ram, 0x0A0012), 0x5678, name, "write16-glued");
    check_u32(psx_ram_read8(psx->ram, 0x0A0020), 0x7F, name, "write8");

    /* D0 matched, so the line after it ran. */
    check_u32(psx_ram_read16(psx->ram, 0x0A0032), 0x4321, name, "cond-taken");
    /* E0 did not match, so the line after it did NOT run. */
    check_u32(psx_ram_read8(psx->ram, 0x0A0035), 0x00, name, "cond-skipped");

    /* 50000502 0001 / 800A0040 1000 — five writes, +2 bytes and +1 value each. */
    check_u32(psx_ram_read16(psx->ram, 0x0A0040), 0x1000, name, "slide-0");
    check_u32(psx_ram_read16(psx->ram, 0x0A0042), 0x1001, name, "slide-1");
    check_u32(psx_ram_read16(psx->ram, 0x0A0044), 0x1002, name, "slide-2");
    check_u32(psx_ram_read16(psx->ram, 0x0A0046), 0x1003, name, "slide-3");
    check_u32(psx_ram_read16(psx->ram, 0x0A0048), 0x1004, name, "slide-4");
    check_u32(psx_ram_read16(psx->ram, 0x0A004A), 0x0000, name, "slide-stops");

    /* Now make the D0 test fail and the E0 test pass, and confirm both flip. The conditional
       state must NOT leak past the end of a cheat into the next one, which is what the
       boundary marker in the compiled program is for: "Slide" runs either way. */
    psx_ram_write16(psx->ram, 0x0A0030, 0x0000);
    psx_ram_write16(psx->ram, 0x0A0032, 0x0000);
    psx_ram_write8(psx->ram, 0x0A0034, 0x02);
    psx_ram_write16(psx->ram, 0x0A0040, 0x0000);

    psx_cheats_apply(psx);

    check_u32(psx_ram_read16(psx->ram, 0x0A0032), 0x0000, name, "cond-now-skipped");
    check_u32(psx_ram_read8(psx->ram, 0x0A0035), 0x0099, name, "cond-now-taken");
    check_u32(psx_ram_read16(psx->ram, 0x0A0040), 0x1000, name, "boundary-resets-skip");

    /* Hardcore. The program must go empty on the next frame WITHOUT the catalogue selection
       being touched, so clearing hardcore restores exactly what the user had. */
    psx_ram_write16(psx->ram, 0x0A0000, 0x0000);
    psx_cheats_set_inhibited(1);
    check(psx_cheats_inhibited() != 0, name, "inhibited-flag");
    check(psx_cheats_armed_count() == 0, name, "inhibited-armed-zero");
    psx_cheats_apply(psx);
    check_u32(psx_ram_read16(psx->ram, 0x0A0000), 0x0000, name, "inhibited-no-write");

    /* And arming while inhibited must refuse rather than arm-and-hope. */
    check(psx_cheats_arm(1, wanted, 5, NULL) == 0, name, "inhibited-arm-refused");
    psx_cheats_apply(psx);
    check_u32(psx_ram_read16(psx->ram, 0x0A0000), 0x0000, name, "inhibited-arm-no-write");

    psx_cheats_set_inhibited(0);
    check(psx_cheats_arm(1, wanted, 5, NULL) == 5, name, "re-arm-after-hardcore");
    psx_cheats_apply(psx);
    check_u32(psx_ram_read16(psx->ram, 0x0A0000), 0x0063, name, "re-armed-writes");

    /* An address past the end of this machine's RAM is REFUSED, not wrapped: the ram writer
       masks with (size - 1), so a code written for an 8 MB debug unit would otherwise land
       somewhere real. */
    check(write_text("build/tests/cheat-oob.cht",
                     "[Out Of Range]\n"
                     "807FFF00 BEEF\n") != 0,
          name, "write-oob");
    check(psx_cheats_load_file("build/tests/cheat-oob.cht") == 1, name, "load-oob");
    wanted[0] = "Out Of Range";
    check(psx_cheats_arm(1, wanted, 1, NULL) == 1, name, "arm-oob");
    before = psx_ram_read16(psx->ram, 0x7FFF00 & (uint32_t)(psx->ram->size - 1u));
    psx_cheats_apply(psx);
    check_u32(psx_ram_read16(psx->ram, 0x7FFF00 & (uint32_t)(psx->ram->size - 1u)), before,
              name, "oob-refused");

    psx_cheats_shutdown();
    psx_destroy(psx);

    return 1;
}

int main(void) {
    const char* bios_path = "build/tests/blank-bios-cheats.bin";
    const char* cht_path = "build/tests/sample.cht";

    if (!write_blank_bios(bios_path)) {
        fprintf(stderr, "CHEAT_ENGINE failed reason=create-bios path=%s\n", bios_path);
        return 1;
    }

    case_parse(cht_path);
    case_arm();
    case_apply(bios_path);

    if (g_failures) {
        printf("CHEAT_ENGINE %d check(s) failed\n", g_failures);
        return 1;
    }

    printf("CHEAT_ENGINE all cases passed\n");
    return 0;
}
