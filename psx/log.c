/*
 * Copyright (c) 2020 rxi
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef PSXE_DIAG_STDIO_DISABLE
#define PSXE_DIAG_STDIO_DISABLE
#endif
#include "log.h"

#define MAX_CALLBACKS 32

typedef struct {
  log_LogFn fn;
  void *udata;
  int level;
} Callback;

static struct {
  void *udata;
  log_LockFn lock;
  int level;
  bool quiet;
  Callback callbacks[MAX_CALLBACKS];
} L;


static const char *level_strings[] = {
  "trace", "debug", "info", "warn", "error", "fatal"
};

#ifdef LOG_USE_COLOR
static const char *level_colors[] = {
  "\x1b[94m", "\x1b[36m", "\x1b[32m", "\x1b[33m", "\x1b[31m", "\x1b[35m"
};
#endif


static void stdout_callback(log_Event *ev) {
#ifdef LOG_USE_COLOR
  fprintf(
    ev->udata, "psx: %s%-5s\x1b[0m \x1b[90m%s:\x1b[0m ",
    level_colors[ev->level], level_strings[ev->level],
    ev->file);
#else
  fprintf(
    ev->udata, "psx: %-5s %s: ",
    level_strings[ev->level], ev->file);
#endif
  vfprintf(ev->udata, ev->fmt, ev->ap);
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}


static void file_callback(log_Event *ev) {
  char buf[64];
  buf[strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", ev->time)] = '\0';
  fprintf(
    ev->udata, "%s %-5s %s:%d: ",
    buf, level_strings[ev->level], ev->file, ev->line);
  vfprintf(ev->udata, ev->fmt, ev->ap);
  fprintf(ev->udata, "\n");
  fflush(ev->udata);
}


static void lock(void)   {
  if (L.lock) { L.lock(true, L.udata); }
}


static void unlock(void) {
  if (L.lock) { L.lock(false, L.udata); }
}


const char* log_level_string(int level) {
  return level_strings[level];
}


void log_set_lock(log_LockFn fn, void *udata) {
  L.lock = fn;
  L.udata = udata;
}


void log_set_level(int level) {
  L.level = level;
}


int log_get_level(void) {
  return L.level;
}


void log_set_quiet(bool enable) {
  L.quiet = enable;
}


int log_add_callback(log_LogFn fn, void *udata, int level) {
  for (int i = 0; i < MAX_CALLBACKS; i++) {
    if (!L.callbacks[i].fn) {
      L.callbacks[i] = (Callback) { fn, udata, level };
      return 0;
    }
  }
  return -1;
}


int log_add_fp(FILE *fp, int level) {
  return log_add_callback(file_callback, fp, level);
}


static void init_event(log_Event *ev, void *udata) {
  if (!ev->time) {
    time_t t = time(NULL);
    ev->time = localtime(&t);
  }
  ev->udata = udata;
}


void log_log(int level, const char *file, int line, const char *fmt, ...) {
  /* Filter before acquiring the logger lock or invoking callbacks. */
  if (level < L.level) {
    return;
  }

  log_Event ev = {
    .fmt   = fmt,
    .file  = file,
    .line  = line,
    .level = level,
  };

  lock();

  if (!L.quiet && level >= L.level) {
    init_event(&ev, stderr);
    va_start(ev.ap, fmt);
    stdout_callback(&ev);
    va_end(ev.ap);
  }

  for (int i = 0; i < MAX_CALLBACKS && L.callbacks[i].fn; i++) {
    Callback *cb = &L.callbacks[i];
    if (level >= cb->level) {
      init_event(&ev, cb->udata);
      va_start(ev.ap, fmt);
      cb->fn(&ev);
      va_end(ev.ap);
    }
  }

  unlock();
}

/* Fixed-size key set for one-shot hot-path diagnostics. */

#define LOG_ONCE_SLOTS 512

static uint64_t log_once_keys[LOG_ONCE_SLOTS];
static bool log_once_used[LOG_ONCE_SLOTS];

static bool log_first_key(uint64_t key) {
  int i;

  lock();

  for (i = 0; i < LOG_ONCE_SLOTS; i++) {
    if (!log_once_used[i])
      break;

    if (log_once_keys[i] == key) {
      unlock();
      return false;
    }
  }

  if (i >= LOG_ONCE_SLOTS) {
    /* Saturation stays silent. */
    unlock();
    return false;
  }

  log_once_keys[i] = key;
  log_once_used[i] = true;

  unlock();

  return true;
}

bool log_first_access(uint32_t address, bool write, unsigned width) {
  unsigned width_key;

  if (L.quiet || LOG_DEBUG < L.level) {
    return false;
  }

  switch (width) {
    case 8:  width_key = 0; break;
    case 16: width_key = 1; break;
    case 32: width_key = 2; break;
    default: width_key = 3; break;
  }

  return log_first_key(((uint64_t)address << 3) |
                       ((uint64_t)(write ? 1u : 0u) << 2) |
                       width_key);
}
