#define PSXE_DIAG_STDIO_DISABLE

#include "diagnostics.h"

#include <SDL.h>

#include <errno.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

enum {
    PSXE_DIAG_BREADCRUMB_CAPACITY = 64,
    PSXE_DIAG_LINE_CAPACITY = 4096,
    PSXE_DIAG_STREAM_BUFFER_CAPACITY = 2048,
};

typedef struct {
    char buffer[PSXE_DIAG_STREAM_BUFFER_CAPACITY];
    size_t length;
} psxe_diag_stream_buffer_t;

static FILE* g_diag_file = NULL;
static char g_diag_log_path[1024] = {0};
static char g_diag_run_tag[64] = {0};
static int g_diag_enabled = 0;
static char g_diag_breadcrumbs[PSXE_DIAG_BREADCRUMB_CAPACITY][PSXE_DIAG_LINE_CAPACITY];
static size_t g_diag_breadcrumb_count = 0;
static size_t g_diag_breadcrumb_head = 0;
static psxe_diag_stream_buffer_t g_stdout_buffer = {0};
static psxe_diag_stream_buffer_t g_stderr_buffer = {0};

static void psxe_diag_load_run_tag(void) {
    g_diag_run_tag[0] = '\0';

    const char* env = getenv("ARMSX_TRACE_TAG");
    if (!env || !env[0]) {
        return;
    }

    size_t out = 0;
    for (const char* cursor = env; *cursor && (out + 1u) < sizeof(g_diag_run_tag); cursor++) {
        const unsigned char ch = (unsigned char)*cursor;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') {
            g_diag_run_tag[out++] = (char)tolower(ch);
        }
    }

    g_diag_run_tag[out] = '\0';
}

static void psxe_diag_output_debug_string(const char* line) {
#ifdef _WIN32
    if (!line || !line[0]) {
        return;
    }

    int wide_length = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
    if (wide_length <= 0) {
        OutputDebugStringA(line);
        OutputDebugStringA("\n");
        return;
    }

    wchar_t* wide = (wchar_t*)malloc(sizeof(wchar_t) * (size_t)wide_length);
    if (!wide) {
        OutputDebugStringA(line);
        OutputDebugStringA("\n");
        return;
    }

    MultiByteToWideChar(CP_UTF8, 0, line, -1, wide, wide_length);
    OutputDebugStringW(wide);
    OutputDebugStringW(L"\n");
    free(wide);
#else
    (void)line;
#endif
}

static void psxe_diag_make_directory(const char* path) {
    if (!path || !path[0]) {
        return;
    }

#ifdef _WIN32
    if ((_mkdir(path) != 0) && (errno != EEXIST)) {
        return;
    }
#else
    if ((mkdir(path, 0755) != 0) && (errno != EEXIST)) {
        return;
    }
#endif
}

static void psxe_diag_close_file(void) {
    if (!g_diag_file) {
        return;
    }

    fflush(g_diag_file);
    fclose(g_diag_file);
    g_diag_file = NULL;
}

static void psxe_diag_update_file_handle(void) {
    psxe_diag_close_file();

    if (!g_diag_enabled || !g_diag_log_path[0]) {
        return;
    }

    g_diag_file = fopen(g_diag_log_path, "a");
}

static void psxe_diag_timestamp(char* buffer, size_t capacity) {
    if (!buffer || capacity == 0) {
        return;
    }

    const time_t now = time(NULL);
    struct tm local_time;

#ifdef _WIN32
    localtime_s(&local_time, &now);
#else
    localtime_r(&now, &local_time);
#endif

    strftime(buffer, capacity, "%Y-%m-%d %H:%M:%S", &local_time);
}

static void psxe_diag_emit_line(const char* source, const char* line) {
    if (!g_diag_enabled || !line || !line[0]) {
        return;
    }

    char timestamp[32] = {0};
    char formatted[PSXE_DIAG_LINE_CAPACITY] = {0};
    psxe_diag_timestamp(timestamp, sizeof(timestamp));

    if (source && source[0]) {
        SDL_snprintf(formatted, sizeof(formatted), "%s [%s] %s", timestamp, source, line);
    } else {
        SDL_snprintf(formatted, sizeof(formatted), "%s %s", timestamp, line);
    }

    if (g_diag_file) {
        fputs(formatted, g_diag_file);
        fputc('\n', g_diag_file);
        fflush(g_diag_file);
    }

    psxe_diag_output_debug_string(formatted);
}

static char* psxe_diag_vformat(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    const int required = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (required < 0) {
        return NULL;
    }

    char* buffer = (char*)malloc((size_t)required + 1u);
    if (!buffer) {
        return NULL;
    }

    va_copy(copy, args);
    vsnprintf(buffer, (size_t)required + 1u, fmt, copy);
    va_end(copy);
    return buffer;
}

static void psxe_diag_write_text(const char* source, const char* text) {
    if (!g_diag_enabled || !text || !text[0]) {
        return;
    }

    const char* line_start = text;

    while (*line_start) {
        const char* cursor = line_start;
        while (*cursor && *cursor != '\n' && *cursor != '\r') {
            cursor++;
        }

        if (cursor > line_start) {
            size_t length = (size_t)(cursor - line_start);
            char line[PSXE_DIAG_LINE_CAPACITY] = {0};
            if (length >= sizeof(line)) {
                length = sizeof(line) - 1u;
            }
            memcpy(line, line_start, length);
            psxe_diag_emit_line(source, line);
        }

        while (*cursor == '\n' || *cursor == '\r') {
            cursor++;
        }

        line_start = cursor;
    }
}

static void psxe_diag_stream_buffer_flush(psxe_diag_stream_buffer_t* buffer, const char* source) {
    if (!buffer || buffer->length == 0) {
        return;
    }

    buffer->buffer[buffer->length] = '\0';
    psxe_diag_write_text(source, buffer->buffer);
    buffer->length = 0;
}

static void psxe_diag_stream_buffer_append(psxe_diag_stream_buffer_t* buffer, const char* source, int ch) {
    if (!g_diag_enabled || !buffer) {
        return;
    }

    if (ch == '\n' || ch == '\r') {
        psxe_diag_stream_buffer_flush(buffer, source);
        return;
    }

    if (buffer->length >= (sizeof(buffer->buffer) - 1u)) {
        psxe_diag_stream_buffer_flush(buffer, source);
    }

    buffer->buffer[buffer->length++] = (char)ch;
}

void psxe_diag_initialize(const char* pref_path) {
    psxe_diag_close_file();

    g_diag_log_path[0] = '\0';
    psxe_diag_load_run_tag();

    if (pref_path && pref_path[0]) {
        char log_directory[1024] = {0};
        SDL_snprintf(log_directory, sizeof(log_directory), "%slogs", pref_path);
        psxe_diag_make_directory(log_directory);

#if defined(UWP_TARGET)
        if (g_diag_run_tag[0]) {
            SDL_snprintf(g_diag_log_path, sizeof(g_diag_log_path), "%s/armsx-uwp-%s.log", log_directory, g_diag_run_tag);
        } else {
            SDL_snprintf(g_diag_log_path, sizeof(g_diag_log_path), "%s/armsx-uwp.log", log_directory);
        }
#else
        if (g_diag_run_tag[0]) {
            SDL_snprintf(g_diag_log_path, sizeof(g_diag_log_path), "%s/armsx-%s.log", log_directory, g_diag_run_tag);
        } else {
            SDL_snprintf(g_diag_log_path, sizeof(g_diag_log_path), "%s/armsx.log", log_directory);
        }
#endif
    }

    psxe_diag_update_file_handle();

    if (g_diag_enabled) {
        psxe_diag_logf("diag", "Diagnostics initialized%s%s%s%s",
            g_diag_run_tag[0] ? " tag=" : "",
            g_diag_run_tag[0] ? g_diag_run_tag : "",
            g_diag_log_path[0] ? " at " : "",
            g_diag_log_path[0] ? g_diag_log_path : "");
    }
}

void psxe_diag_shutdown(void) {
    psxe_diag_stream_buffer_flush(&g_stdout_buffer, "stdout");
    psxe_diag_stream_buffer_flush(&g_stderr_buffer, "stderr");

    psxe_diag_close_file();
}

const char* psxe_diag_log_path(void) {
    return g_diag_log_path;
}

void psxe_diag_set_enabled(int enabled) {
    const int normalized = enabled ? 1 : 0;
    if (g_diag_enabled == normalized) {
        return;
    }

    if (!normalized) {
        psxe_diag_stream_buffer_flush(&g_stdout_buffer, "stdout");
        psxe_diag_stream_buffer_flush(&g_stderr_buffer, "stderr");
    }

    g_diag_enabled = normalized;
    psxe_diag_update_file_handle();
}

int psxe_diag_is_enabled(void) {
    return g_diag_enabled;
}

void psxe_diag_log_line(const char* source, const char* line) {
    psxe_diag_write_text(source, line);
}

void psxe_diag_vlogf(const char* source, const char* fmt, va_list args) {
    char* message = psxe_diag_vformat(fmt, args);
    if (!message) {
        return;
    }

    psxe_diag_write_text(source, message);
    free(message);
}

void psxe_diag_logf(const char* source, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    psxe_diag_vlogf(source, fmt, args);
    va_end(args);
}

void psxe_diag_breadcrumb_line(const char* line) {
    if (!line || !line[0]) {
        return;
    }

    char timestamp[32] = {0};
    psxe_diag_timestamp(timestamp, sizeof(timestamp));

    SDL_snprintf(
        g_diag_breadcrumbs[g_diag_breadcrumb_head],
        sizeof(g_diag_breadcrumbs[g_diag_breadcrumb_head]),
        "%s %s",
        timestamp,
        line
    );
    g_diag_breadcrumb_head = (g_diag_breadcrumb_head + 1u) % PSXE_DIAG_BREADCRUMB_CAPACITY;
    if (g_diag_breadcrumb_count < PSXE_DIAG_BREADCRUMB_CAPACITY) {
        g_diag_breadcrumb_count++;
    }
}

void psxe_diag_breadcrumbf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char* message = psxe_diag_vformat(fmt, args);
    va_end(args);

    if (!message) {
        return;
    }

    psxe_diag_breadcrumb_line(message);
    free(message);
}

void psxe_diag_dump_breadcrumbs(void) {
    if (g_diag_breadcrumb_count == 0) {
        psxe_diag_logf("crash", "No breadcrumbs recorded.");
        return;
    }

    psxe_diag_logf("crash", "Recent breadcrumbs:");

    const size_t start =
        (g_diag_breadcrumb_head + PSXE_DIAG_BREADCRUMB_CAPACITY - g_diag_breadcrumb_count) %
        PSXE_DIAG_BREADCRUMB_CAPACITY;

    for (size_t index = 0; index < g_diag_breadcrumb_count; index++) {
        const size_t slot = (start + index) % PSXE_DIAG_BREADCRUMB_CAPACITY;
        psxe_diag_logf("crash", "  %s", g_diag_breadcrumbs[slot]);
    }
}

void psxe_diag_set_crash_context_callback(psxe_diag_crash_context_cb callback, void* userdata) {
    (void)callback;
    (void)userdata;
}

int psxe_diag_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int written = vfprintf(stdout, fmt, args);
    va_end(args);
    psxe_diag_vlogf("stdout", fmt, copy);
    va_end(copy);
    return written;
}

int psxe_diag_fprintf(FILE* stream, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    const int written = vfprintf(stream, fmt, args);
    va_end(args);

    if (stream == stderr) {
        psxe_diag_vlogf("stderr", fmt, copy);
    } else if (stream == stdout) {
        psxe_diag_vlogf("stdout", fmt, copy);
    } else {
        psxe_diag_vlogf("stdio", fmt, copy);
    }

    va_end(copy);
    return written;
}

int psxe_diag_puts(const char* str) {
    const int written = puts(str);
    psxe_diag_write_text("stdout", str);
    return written;
}

int psxe_diag_putc(int ch, FILE* stream) {
    const int written = fputc(ch, stream);

    if (stream == stderr) {
        psxe_diag_stream_buffer_append(&g_stderr_buffer, "stderr", ch);
    } else {
        psxe_diag_stream_buffer_append(&g_stdout_buffer, "stdout", ch);
    }

    return written;
}
