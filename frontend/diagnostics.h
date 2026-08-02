#ifndef FRONTEND_DIAGNOSTICS_H
#define FRONTEND_DIAGNOSTICS_H

#include <stdarg.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*psxe_diag_crash_context_cb)(const char* reason, void* userdata);

void psxe_diag_initialize(const char* pref_path);
void psxe_diag_shutdown(void);
const char* psxe_diag_log_path(void);
void psxe_diag_set_enabled(int enabled);
int psxe_diag_is_enabled(void);

void psxe_diag_log_line(const char* source, const char* line);
void psxe_diag_logf(const char* source, const char* fmt, ...);
void psxe_diag_vlogf(const char* source, const char* fmt, va_list args);

void psxe_diag_breadcrumb_line(const char* line);
void psxe_diag_breadcrumbf(const char* fmt, ...);
void psxe_diag_dump_breadcrumbs(void);

void psxe_diag_set_crash_context_callback(psxe_diag_crash_context_cb callback, void* userdata);
void psxe_diag_write_native_stacktrace(void);

int psxe_diag_printf(const char* fmt, ...);
int psxe_diag_fprintf(FILE* stream, const char* fmt, ...);
int psxe_diag_puts(const char* str);
int psxe_diag_putc(int ch, FILE* stream);

#ifdef __cplusplus
}
#endif

#if !defined(PSXE_DIAG_STDIO_DISABLE)
#define printf(...) psxe_diag_printf(__VA_ARGS__)
#define fprintf(stream, ...) psxe_diag_fprintf((stream), __VA_ARGS__)
#define puts(str) psxe_diag_puts((str))
#define putc(ch, stream) psxe_diag_putc((ch), (stream))
#define fputc(ch, stream) psxe_diag_putc((ch), (stream))
#endif

#endif
