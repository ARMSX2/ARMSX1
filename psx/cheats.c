#include "cheats.h"

#include "psx.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* -------------------------------------------------------------------------- */
/* Portability shims                                                          */
/* -------------------------------------------------------------------------- */

/* Same shape (and the same reasoning) as psx/state.c: C11 atomics where the toolchain has
   them, volatile where it does not. The values crossing threads here are a single int flag
   and a single pointer, both written by one producer and consumed by one consumer. */
#if !defined(__STDC_NO_ATOMICS__) && defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
#include <stdatomic.h>
#define PSX_CHEATS_ATOMIC_INT _Atomic int
#define PSX_CHEATS_LOAD(p) atomic_load_explicit(&(p), memory_order_acquire)
#define PSX_CHEATS_STORE(p, v) atomic_store_explicit(&(p), (v), memory_order_release)
static atomic_flag g_cheats_lock = ATOMIC_FLAG_INIT;
#define PSX_CHEATS_LOCK()                                                            \
    while (atomic_flag_test_and_set_explicit(&g_cheats_lock, memory_order_acquire)) { \
    }
#define PSX_CHEATS_UNLOCK() atomic_flag_clear_explicit(&g_cheats_lock, memory_order_release)
#else
#define PSX_CHEATS_ATOMIC_INT volatile int
#define PSX_CHEATS_LOAD(p) (p)
#define PSX_CHEATS_STORE(p, v) ((p) = (v))
#define PSX_CHEATS_LOCK() ((void)0)
#define PSX_CHEATS_UNLOCK() ((void)0)
#endif

/* -------------------------------------------------------------------------- */
/* Code types                                                                 */
/* -------------------------------------------------------------------------- */

#define CHEAT_T_WRITE8   0x30u
#define CHEAT_T_WRITE16  0x80u
#define CHEAT_T_SLIDE    0x50u
#define CHEAT_T_IF16_EQ  0xD0u
#define CHEAT_T_IF16_NE  0xD1u
#define CHEAT_T_IF16_LT  0xD2u
#define CHEAT_T_IF16_GT  0xD3u
#define CHEAT_T_IF8_EQ   0xE0u
#define CHEAT_T_IF8_NE   0xE1u
#define CHEAT_T_IF8_LT   0xE2u
#define CHEAT_T_IF8_GT   0xE3u

/* Not a GameShark type. Separates one armed cheat from the next inside the flat program so
   a dangling conditional or an unconsumed slide can never leak across the boundary. */
#define CHEAT_T_BOUNDARY 0xFFu

static int psx_cheat_type_supported(uint32_t type) {
    switch (type) {
        case CHEAT_T_WRITE8:
        case CHEAT_T_WRITE16:
        case CHEAT_T_SLIDE:
        case CHEAT_T_IF16_EQ:
        case CHEAT_T_IF16_NE:
        case CHEAT_T_IF16_LT:
        case CHEAT_T_IF16_GT:
        case CHEAT_T_IF8_EQ:
        case CHEAT_T_IF8_NE:
        case CHEAT_T_IF8_LT:
        case CHEAT_T_IF8_GT:
            return 1;
        default:
            return 0;
    }
}

/* -------------------------------------------------------------------------- */
/* Catalogue                                                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    char* name;        /* owned */
    char* description; /* owned, "" when the file gave none */
    uint32_t* words;   /* owned; 2 per code line: code, value */
    int line_count;
    int armed;
    int has_unsupported;
} psx_cheat_entry_t;

static psx_cheat_entry_t* g_entries = NULL;
static int g_entry_count = 0;
static char* g_source_path = NULL;      /* owned */
static char* g_unsupported = NULL;      /* owned */

/* -------------------------------------------------------------------------- */
/* Program (what the emulation thread walks)                                  */
/* -------------------------------------------------------------------------- */

typedef struct {
    int word_count; /* number of uint32_t, always even */
    int cheat_count;
    uint32_t words[1]; /* over-allocated */
} psx_cheat_program_t;

/* Parked by the arming thread, adopted by the emulation thread. Both transitions happen
   under the lock, so the pointer can never be freed while the other side holds it. */
static psx_cheat_program_t* g_pending = NULL;
static PSX_CHEATS_ATOMIC_INT g_publish_pending = 0;

/* Emulation thread ONLY. Never read or written anywhere else — that is what makes the
   per-frame fast path a plain load with no synchronisation at all. */
static psx_cheat_program_t* g_active = NULL;

static PSX_CHEATS_ATOMIC_INT g_armed_count = 0;
static PSX_CHEATS_ATOMIC_INT g_inhibited = 0;

/* -------------------------------------------------------------------------- */
/* Small helpers                                                              */
/* -------------------------------------------------------------------------- */

/* Every string this module keeps is its own copy. A borrowed pointer from a caller that
   later frees (or, worse, is a mapped file) is how a previous device path in this codebase
   ended up truncating a user's disc image. */
static char* psx_cheat_strdup(const char* s, size_t max) {
    size_t len;
    char* out;

    if (!s)
        s = "";

    len = strlen(s);

    if (len > max)
        len = max;

    out = (char*)malloc(len + 1);

    if (!out)
        return NULL;

    memcpy(out, s, len);
    out[len] = '\0';

    return out;
}

static void psx_cheat_trim(char* s) {
    size_t len;
    size_t start = 0;

    if (!s)
        return;

    len = strlen(s);

    while (len > start && (unsigned char)s[len - 1] <= ' ')
        len--;

    while (start < len && (unsigned char)s[start] <= ' ')
        start++;

    if (start)
        memmove(s, s + start, len - start);

    s[len - start] = '\0';
}

/* The [start, end) of `s` with leading and trailing whitespace removed. */
static void psx_cheat_span(const char* s, const char** start, const char** end) {
    const char* first = s ? s : "";
    const char* last;

    while (*first && (unsigned char)*first <= ' ')
        first++;

    last = first + strlen(first);

    while (last > first && (unsigned char)*(last - 1) <= ' ')
        last--;

    *start = first;
    *end = last;
}

/* Case-insensitive, leading/trailing space ignored — so a name typed by hand into
   settings.toml matches the one the file declares. */
static int psx_cheat_name_matches(const char* a, const char* b) {
    const char* a_start;
    const char* a_end;
    const char* b_start;
    const char* b_end;

    if (!a || !b)
        return 0;

    psx_cheat_span(a, &a_start, &a_end);
    psx_cheat_span(b, &b_start, &b_end);

    if ((a_end - a_start) != (b_end - b_start))
        return 0;

    while (a_start < a_end) {
        if (tolower((unsigned char)*a_start) != tolower((unsigned char)*b_start))
            return 0;

        a_start++;
        b_start++;
    }

    return 1;
}

/* -------------------------------------------------------------------------- */
/* Parsing                                                                    */
/* -------------------------------------------------------------------------- */

/* A run of hex digits, at least `min` and at most `max` of them. Returns the count consumed
   (0 = no match) and leaves *out holding the value. */
static int psx_cheat_hex(const char* s, int min, int max, uint32_t* out) {
    uint32_t value = 0;
    int used = 0;

    while (used < max) {
        int c = (unsigned char)s[used];
        int digit;

        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            break;

        value = (value << 4) | (uint32_t)digit;
        used++;
    }

    if (used < min)
        return 0;

    *out = value;

    return used;
}

/*
    One code line -> (code, value).

    Accepts the three shapes that actually appear in the wild:

        800AB3C4 0063      the usual one
        800AB3C4:0063      colon-separated, as some sites render it
        800AB3C40063       run together, as some copy/paste loses the space

    Returns 0 when the line is not a code line at all.
*/
static int psx_cheat_parse_code(const char* line, uint32_t* code, uint32_t* value) {
    const char* p = line;
    uint32_t hi = 0;
    uint32_t lo = 0;
    int used;

    while (*p && (unsigned char)*p <= ' ')
        p++;

    used = psx_cheat_hex(p, 8, 8, &hi);

    if (!used)
        return 0;

    p += used;

    /* Run-together form: the value is glued to the address. */
    if (*p && ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) {
        used = psx_cheat_hex(p, 1, 8, &lo);

        if (!used)
            return 0;

        p += used;
    } else {
        while (*p && ((unsigned char)*p <= ' ' || *p == ':' || *p == ','))
            p++;

        used = psx_cheat_hex(p, 1, 8, &lo);

        if (!used)
            return 0;

        p += used;
    }

    /* Anything after the two words that is not blank or a comment means this was prose that
       merely started with hex, not a code. Refusing it is what keeps a description line out
       of the program. */
    while (*p && (unsigned char)*p <= ' ')
        p++;

    if (*p && *p != '#' && *p != ';' && !(p[0] == '/' && p[1] == '/'))
        return 0;

    *code = hi;
    *value = lo;

    return 1;
}

static void psx_cheat_free_entries(psx_cheat_entry_t* entries, int count) {
    int i;

    if (!entries)
        return;

    for (i = 0; i < count; i++) {
        free(entries[i].name);
        free(entries[i].description);
        free(entries[i].words);
    }

    free(entries);
}

/* Append `type` to the "types this build does not implement" set, once each. */
static void psx_cheat_note_unsupported(char* buffer, size_t capacity, uint32_t type) {
    char token[8];
    size_t len;

    snprintf(token, sizeof(token), "%02X", (unsigned)(type & 0xFFu));

    if (strstr(buffer, token))
        return;

    len = strlen(buffer);

    if (len + strlen(token) + 2 >= capacity)
        return;

    if (len) {
        buffer[len++] = ' ';
        buffer[len] = '\0';
    }

    memcpy(buffer + len, token, strlen(token) + 1);
}

int psx_cheats_load_file(const char* path) {
    FILE* file;
    char line[1024];
    psx_cheat_entry_t* entries = NULL;
    int count = 0;
    int capacity = 0;
    int total_lines = 0;
    int truncated = 0;
    int first_line = 1;
    char unsupported[128];
    char* new_path;
    char* new_unsupported;
    psx_cheat_entry_t* old_entries;
    int old_count;
    char* old_path;
    char* old_unsupported;

    unsupported[0] = '\0';

    /* No file for this game is the NORMAL case, not a failure. Clearing here rather than
       leaving the previous game's catalogue in place is what stops a cheat list following
       the user from one disc to the next. */
    if (!path || !path[0]) {
        PSX_CHEATS_LOCK();
        old_entries = g_entries;
        old_count = g_entry_count;
        old_path = g_source_path;
        old_unsupported = g_unsupported;
        g_entries = NULL;
        g_entry_count = 0;
        g_source_path = NULL;
        g_unsupported = NULL;
        PSX_CHEATS_UNLOCK();

        psx_cheat_free_entries(old_entries, old_count);
        free(old_path);
        free(old_unsupported);

        return 0;
    }

    file = fopen(path, "rb");

    if (!file) {
        log_info("cheats: no cheat file at %s", path);
        return -1;
    }

    while (fgets(line, (int)sizeof(line), file)) {
        char* p = line;
        uint32_t code = 0;
        uint32_t value = 0;

        /* Strip the newline and any UTF-8 BOM the first line may carry. Without the BOM strip
           the first `[Name]` is not recognised and the whole file's codes end up in a single
           "Unnamed codes" group — Windows editors write that BOM by default. */
        p[strcspn(p, "\r\n")] = '\0';

        if (first_line) {
            first_line = 0;

            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
                (unsigned char)p[2] == 0xBF) {
                p += 3;
            }
        }

        while (*p && (unsigned char)*p <= ' ')
            p++;

        if (!*p || *p == '#' || *p == ';' || (p[0] == '/' && p[1] == '/'))
            continue;

        if (*p == '[') {
            char* end = strrchr(p, ']');
            psx_cheat_entry_t* entry;

            if (end)
                *end = '\0';

            if (count >= PSX_CHEAT_MAX_ENTRIES) {
                truncated = 1;
                break;
            }

            if (count == capacity) {
                int next = capacity ? capacity * 2 : 32;
                psx_cheat_entry_t* grown =
                    (psx_cheat_entry_t*)realloc(entries, (size_t)next * sizeof(psx_cheat_entry_t));

                if (!grown) {
                    truncated = 1;
                    break;
                }

                entries = grown;
                capacity = next;
            }

            entry = &entries[count];
            memset(entry, 0, sizeof(*entry));

            psx_cheat_trim(p + 1);
            entry->name = psx_cheat_strdup(p + 1, PSX_CHEAT_NAME_MAX);
            entry->description = psx_cheat_strdup("", PSX_CHEAT_DESC_MAX);

            if (!entry->name || !entry->description) {
                free(entry->name);
                free(entry->description);
                truncated = 1;
                break;
            }

            /* An unnamed group is still a group: it just gets a name a user can select. */
            if (!entry->name[0]) {
                free(entry->name);
                entry->name = psx_cheat_strdup("(unnamed)", PSX_CHEAT_NAME_MAX);

                if (!entry->name) {
                    free(entry->description);
                    truncated = 1;
                    break;
                }
            }

            count++;
            continue;
        }

        if (p[0] == '@') {
            /* `@desc <text>` — the one piece of metadata this reads. Anything else beginning
               with @ is ignored so the format can grow without breaking old builds. */
            if (count > 0 && strncmp(p + 1, "desc", 4) == 0) {
                char* text = p + 5;
                char* merged;
                size_t have;
                size_t add;

                while (*text && (unsigned char)*text <= ' ')
                    text++;

                have = strlen(entries[count - 1].description);
                add = strlen(text);

                if (have + add + 2 <= PSX_CHEAT_DESC_MAX) {
                    merged = (char*)realloc(entries[count - 1].description, have + add + 2);

                    if (merged) {
                        if (have)
                            merged[have++] = ' ';

                        memcpy(merged + have, text, add + 1);
                        entries[count - 1].description = merged;
                    }
                }
            }

            continue;
        }

        if (!psx_cheat_parse_code(p, &code, &value))
            continue;

        if (total_lines >= PSX_CHEAT_MAX_LINES) {
            truncated = 1;
            break;
        }

        /* Codes before the first [Name]. Rather than dropping them, give them a group so the
           user can see and switch them on — a plain list of codes with no header is a real
           thing people paste. */
        if (count == 0) {
            psx_cheat_entry_t* entry;

            if (count == capacity) {
                int next = capacity ? capacity * 2 : 32;
                psx_cheat_entry_t* grown =
                    (psx_cheat_entry_t*)realloc(entries, (size_t)next * sizeof(psx_cheat_entry_t));

                if (!grown) {
                    truncated = 1;
                    break;
                }

                entries = grown;
                capacity = next;
            }

            entry = &entries[count];
            memset(entry, 0, sizeof(*entry));
            entry->name = psx_cheat_strdup("Unnamed codes", PSX_CHEAT_NAME_MAX);
            entry->description = psx_cheat_strdup("", PSX_CHEAT_DESC_MAX);

            if (!entry->name || !entry->description) {
                free(entry->name);
                free(entry->description);
                truncated = 1;
                break;
            }

            count++;
        }

        {
            psx_cheat_entry_t* entry = &entries[count - 1];
            uint32_t* grown =
                (uint32_t*)realloc(entry->words, (size_t)(entry->line_count + 1) * 2 * sizeof(uint32_t));

            if (!grown) {
                truncated = 1;
                break;
            }

            entry->words = grown;
            entry->words[entry->line_count * 2 + 0] = code;
            entry->words[entry->line_count * 2 + 1] = value;
            entry->line_count++;
            total_lines++;

            if (!psx_cheat_type_supported(code >> 24)) {
                entry->has_unsupported = 1;
                psx_cheat_note_unsupported(unsupported, sizeof(unsupported), code >> 24);
            }
        }
    }

    fclose(file);

    if (truncated) {
        log_warn("cheats: %s is malformed or too large; stopped after %d entries", path, count);
    }

    new_path = psx_cheat_strdup(path, 4095);
    new_unsupported = psx_cheat_strdup(unsupported, sizeof(unsupported));

    if (!new_path || !new_unsupported) {
        free(new_path);
        free(new_unsupported);
        psx_cheat_free_entries(entries, count);
        log_error("cheats: out of memory loading %s", path);
        return -1;
    }

    PSX_CHEATS_LOCK();
    old_entries = g_entries;
    old_count = g_entry_count;
    old_path = g_source_path;
    old_unsupported = g_unsupported;
    g_entries = entries;
    g_entry_count = count;
    g_source_path = new_path;
    g_unsupported = new_unsupported;
    PSX_CHEATS_UNLOCK();

    psx_cheat_free_entries(old_entries, old_count);
    free(old_path);
    free(old_unsupported);

    if (unsupported[0]) {
        log_info("cheats: loaded %d entries from %s (unimplemented code types present: %s)",
                 count, path, unsupported);
    } else {
        log_info("cheats: loaded %d entries from %s", count, path);
    }

    return count;
}

/* -------------------------------------------------------------------------- */
/* Catalogue accessors                                                        */
/* -------------------------------------------------------------------------- */

const char* psx_cheats_source_path(void) {
    return g_source_path ? g_source_path : "";
}

int psx_cheats_count(void) {
    return g_entry_count;
}

const char* psx_cheats_name(int index) {
    if (index < 0 || index >= g_entry_count || !g_entries)
        return "";

    return g_entries[index].name ? g_entries[index].name : "";
}

const char* psx_cheats_description(int index) {
    if (index < 0 || index >= g_entry_count || !g_entries)
        return "";

    return g_entries[index].description ? g_entries[index].description : "";
}

int psx_cheats_line_count(int index) {
    if (index < 0 || index >= g_entry_count || !g_entries)
        return 0;

    return g_entries[index].line_count;
}

int psx_cheats_has_unsupported(int index) {
    if (index < 0 || index >= g_entry_count || !g_entries)
        return 0;

    return g_entries[index].has_unsupported;
}

const char* psx_cheats_unsupported_types(void) {
    return g_unsupported ? g_unsupported : "";
}

char* psx_cheats_describe(void) {
    size_t needed = 1;
    char* out;
    size_t used = 0;
    int i;

    PSX_CHEATS_LOCK();

    for (i = 0; i < g_entry_count; i++) {
        /* name + SEP + up to 10 digits + SEP + 1 + SEP + description + record separator */
        needed += strlen(g_entries[i].name ? g_entries[i].name : "") +
                  strlen(g_entries[i].description ? g_entries[i].description : "") + 24;
    }

    out = (char*)malloc(needed);

    if (!out) {
        PSX_CHEATS_UNLOCK();
        return NULL;
    }

    out[0] = '\0';

    for (i = 0; i < g_entry_count; i++) {
        const char* name = g_entries[i].name ? g_entries[i].name : "";
        const char* desc = g_entries[i].description ? g_entries[i].description : "";
        int written;

        if (used)
            out[used++] = '\x1E';

        written = snprintf(out + used, needed - used, "%s\x1F%d\x1F%d\x1F%s", name,
                           g_entries[i].line_count, g_entries[i].has_unsupported ? 1 : 0, desc);

        if (written < 0 || (size_t)written >= needed - used) {
            /* Cannot happen with the sizing above; truncate rather than run off the end. */
            out[used] = '\0';
            break;
        }

        used += (size_t)written;
    }

    PSX_CHEATS_UNLOCK();

    return out;
}

/* -------------------------------------------------------------------------- */
/* Arming                                                                     */
/* -------------------------------------------------------------------------- */

int psx_cheats_arm(int master, const char* const* names, int count, int* missing) {
    psx_cheat_program_t* program = NULL;
    psx_cheat_program_t* replaced = NULL;
    int armed = 0;
    int words = 0;
    int i;
    int j;

    if (missing)
        *missing = 0;

    /* Hardcore. Refuse rather than arm-and-hope: a cheat that armed here and was only
       stopped further down would be one refactor away from reaching the game. */
    if (PSX_CHEATS_LOAD(g_inhibited)) {
        master = 0;
    }

    if (!names)
        count = 0;

    /*
        Held across the WHOLE walk, not just the publish.

        This runs on the UI thread through JNI and on the emulation thread once at boot, and it
        reads g_entries end to end — so a psx_cheats_load_file() landing mid-walk would free the
        array under it. Everything inside is a comparison or a memcpy of a few hundred bytes;
        the file read that would actually be worth not holding a lock across happens in
        psx_cheats_load_file(), which parses into locals and only takes the lock to swap.
    */
    PSX_CHEATS_LOCK();

    for (i = 0; i < g_entry_count; i++) {
        int wanted = 0;

        for (j = 0; j < count; j++) {
            if (psx_cheat_name_matches(g_entries[i].name, names[j])) {
                wanted = 1;
                break;
            }
        }

        g_entries[i].armed = (master && wanted) ? 1 : 0;

        if (g_entries[i].armed) {
            armed++;
            /* +1 line for this cheat's boundary marker. */
            words += (g_entries[i].line_count + 1) * 2;
        }
    }

    if (missing) {
        for (j = 0; j < count; j++) {
            int found = 0;

            for (i = 0; i < g_entry_count; i++) {
                if (psx_cheat_name_matches(g_entries[i].name, names[j])) {
                    found = 1;
                    break;
                }
            }

            if (!found)
                (*missing)++;
        }
    }

    if (armed > 0 && words > 0) {
        program = (psx_cheat_program_t*)malloc(sizeof(psx_cheat_program_t) +
                                               (size_t)(words - 1) * sizeof(uint32_t));

        if (!program) {
            PSX_CHEATS_UNLOCK();
            log_error("cheats: out of memory arming %d entries", armed);
            return 0;
        }

        program->word_count = 0;
        program->cheat_count = armed;

        for (i = 0; i < g_entry_count; i++) {
            if (!g_entries[i].armed)
                continue;

            memcpy(&program->words[program->word_count], g_entries[i].words,
                   (size_t)g_entries[i].line_count * 2 * sizeof(uint32_t));
            program->word_count += g_entries[i].line_count * 2;

            program->words[program->word_count++] = (uint32_t)CHEAT_T_BOUNDARY << 24;
            program->words[program->word_count++] = 0;
        }
    }

    replaced = g_pending;
    g_pending = program;
    PSX_CHEATS_STORE(g_publish_pending, 1);
    PSX_CHEATS_UNLOCK();

    /* Whatever we displaced was never handed to the emulation thread (it took it out of
       g_pending under the same lock, or it is still sitting there), so this is ours to free. */
    free(replaced);

    PSX_CHEATS_STORE(g_armed_count, armed);

    log_info("cheats: %d of %d entries armed%s", armed, g_entry_count,
             PSX_CHEATS_LOAD(g_inhibited) ? " (hardcore: forced off)" : "");

    return armed;
}

int psx_cheats_armed_count(void) {
    return PSX_CHEATS_LOAD(g_armed_count);
}

void psx_cheats_set_inhibited(int inhibited) {
    const int want = inhibited ? 1 : 0;

    if (PSX_CHEATS_LOAD(g_inhibited) == want)
        return;

    PSX_CHEATS_STORE(g_inhibited, want);

    if (want) {
        /* Publish an empty program NOW. The selection itself is untouched in the catalogue,
           so clearing hardcore and re-arming restores exactly what the user had. */
        psx_cheat_program_t* replaced;

        PSX_CHEATS_LOCK();
        replaced = g_pending;
        g_pending = NULL;
        PSX_CHEATS_STORE(g_publish_pending, 1);
        PSX_CHEATS_UNLOCK();

        free(replaced);
        PSX_CHEATS_STORE(g_armed_count, 0);

        log_info("cheats: disabled — RetroAchievements hardcore mode is active");
    }
}

int psx_cheats_inhibited(void) {
    return PSX_CHEATS_LOAD(g_inhibited);
}

/* -------------------------------------------------------------------------- */
/* Per-frame application (emulation thread)                                   */
/* -------------------------------------------------------------------------- */

static void psx_cheat_write(psx_t* psx, uint32_t address, uint32_t value, int width) {
    psx_ram_t* ram = psx->ram;

    /* Refuse rather than alias. psx_ram_write*() masks with (size - 1), so a code written
       for an 8 MB debug unit would otherwise land somewhere real on a 2 MB console. */
    if ((size_t)address + (size_t)width > ram->size)
        return;

    if (width == 1)
        psx_ram_write8(ram, address, (uint8_t)value);
    else
        psx_ram_write16(ram, address, (uint16_t)value);

    /* The cached interpreter caches decoded words by physical address. A cheat that patches
       code (a `jr $ra` over a check is a common one) would otherwise keep running the old
       instruction until the entry was evicted. No-op when the plain interpreter is selected. */
    psx_cpu_invalidate_range(psx->cpu, address, (uint32_t)width);
}

void psx_cheats_apply(struct psx_t* psx_opaque) {
    psx_t* psx = (psx_t*)psx_opaque;
    psx_cheat_program_t* program;
    int index;
    int skip_next = 0;
    uint32_t slide_count = 0;
    uint32_t slide_addr_step = 0;
    uint32_t slide_value_step = 0;

    /* THE fast path. One predictable branch on a thread-local pointer plus one atomic load,
       once per frame, for every session that never arms a cheat. */
    if (!g_active && !PSX_CHEATS_LOAD(g_publish_pending))
        return;

    if (PSX_CHEATS_LOAD(g_publish_pending)) {
        psx_cheat_program_t* adopted;

        PSX_CHEATS_LOCK();
        adopted = g_pending;
        g_pending = NULL;
        PSX_CHEATS_STORE(g_publish_pending, 0);
        PSX_CHEATS_UNLOCK();

        free(g_active);
        g_active = adopted;
    }

    program = g_active;

    if (!program || !psx || !psx->ram || !psx->cpu)
        return;

    for (index = 0; index + 1 < program->word_count; index += 2) {
        const uint32_t code = program->words[index];
        const uint32_t value = program->words[index + 1];
        const uint32_t type = code >> 24;
        const uint32_t address = code & 0x00FFFFFFu;

        if (type == CHEAT_T_BOUNDARY) {
            skip_next = 0;
            slide_count = 0;
            continue;
        }

        if (skip_next) {
            skip_next = 0;
            continue;
        }

        switch (type) {
            case CHEAT_T_SLIDE:
                /* 5000ccii vvvv — arms the NEXT write. Held rather than applied. */
                slide_count = (code >> 8) & 0xFFu;
                slide_addr_step = code & 0xFFu;
                slide_value_step = value;
                break;

            case CHEAT_T_WRITE8:
            case CHEAT_T_WRITE16: {
                const int width = (type == CHEAT_T_WRITE8) ? 1 : 2;
                uint32_t repeats = slide_count ? slide_count : 1;
                uint32_t step = slide_count ? slide_addr_step : 0;
                uint32_t delta = slide_count ? slide_value_step : 0;
                uint32_t current_address = address;
                uint32_t current_value = value;
                uint32_t n;

                for (n = 0; n < repeats; n++) {
                    psx_cheat_write(psx, current_address, current_value, width);
                    current_address += step;
                    current_value += delta;
                }

                slide_count = 0;
                break;
            }

            case CHEAT_T_IF16_EQ:
                skip_next = (psx_ram_read16(psx->ram, address) != (uint16_t)value);
                break;
            case CHEAT_T_IF16_NE:
                skip_next = (psx_ram_read16(psx->ram, address) == (uint16_t)value);
                break;
            case CHEAT_T_IF16_LT:
                skip_next = !(psx_ram_read16(psx->ram, address) < (uint16_t)value);
                break;
            case CHEAT_T_IF16_GT:
                skip_next = !(psx_ram_read16(psx->ram, address) > (uint16_t)value);
                break;

            case CHEAT_T_IF8_EQ:
                skip_next = (psx_ram_read8(psx->ram, address) != (uint8_t)value);
                break;
            case CHEAT_T_IF8_NE:
                skip_next = (psx_ram_read8(psx->ram, address) == (uint8_t)value);
                break;
            case CHEAT_T_IF8_LT:
                skip_next = !(psx_ram_read8(psx->ram, address) < (uint8_t)value);
                break;
            case CHEAT_T_IF8_GT:
                skip_next = !(psx_ram_read8(psx->ram, address) > (uint8_t)value);
                break;

            default:
                /* Unimplemented type. Reported once at load time by name; SILENT here,
                   because this runs sixty times a second and one error-level log on a
                   per-frame path in this codebase once produced 89,000 lines in 105
                   seconds. */
                break;
        }
    }
}

void psx_cheats_shutdown(void) {
    psx_cheat_entry_t* entries;
    int count;
    char* path;
    char* unsupported;
    psx_cheat_program_t* pending;

    PSX_CHEATS_LOCK();
    entries = g_entries;
    count = g_entry_count;
    path = g_source_path;
    unsupported = g_unsupported;
    pending = g_pending;
    g_entries = NULL;
    g_entry_count = 0;
    g_source_path = NULL;
    g_unsupported = NULL;
    g_pending = NULL;
    PSX_CHEATS_STORE(g_publish_pending, 0);
    PSX_CHEATS_UNLOCK();

    psx_cheat_free_entries(entries, count);
    free(path);
    free(unsupported);
    free(pending);
    free(g_active);
    g_active = NULL;

    PSX_CHEATS_STORE(g_armed_count, 0);
}
