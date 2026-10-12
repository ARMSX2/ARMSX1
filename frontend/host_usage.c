/*
    ARMSX — host resource usage sampling. See host_usage.h for the contract.
*/

#include "host_usage.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

#define ARMSX_HOST_USAGE_INTERVAL_NS 500000000LL /* 500 ms; the OSD polls at 2 Hz */

uint64_t armsx_host_thread_cpu_ns(void) {
#if defined(CLOCK_THREAD_CPUTIME_ID)
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
#endif
    return 0;
}

/* Slots are specific to a fixed pathname, so a denial never turns into a
   repeated SELinux audit on every snapshot. */
static int diagnostic_read(const char* path, char* out, size_t capacity, unsigned slot) {
    static unsigned char unavailable[128];
    if (slot >= sizeof(unavailable) || unavailable[slot]) return 0;
    FILE* f = fopen(path, "r");
    if (!f) { unavailable[slot] = 1; return 0; }
    const size_t n = fread(out, 1, capacity - 1, f);
    fclose(f);
    out[n] = 0;
    while (strlen(out) && (out[strlen(out)-1] == '\n' || out[strlen(out)-1] == '\r'))
        out[strlen(out)-1] = 0;
    for (size_t i = 0; i < n; ++i) if (out[i] == '\n' || out[i] == '\r') out[i] = '|';
    return n != 0;
}

void armsx_host_diagnostic_snapshot(char* out, size_t capacity) {
    if (!out || !capacity) return;
    out[0] = 0;
#if defined(__linux__)
    char sched[128] = "unavailable", allowed[96] = "unavailable", status[4096];
    char cpu_pressure[256] = "unavailable", io_pressure[256] = "unavailable";
    char mem_pressure[256] = "unavailable";
    diagnostic_read("/proc/thread-self/schedstat", sched, sizeof(sched), 0);
    if (diagnostic_read("/proc/thread-self/status", status, sizeof(status), 1)) {
        const char* p = strstr(status, "Cpus_allowed_list:");
        if (p) {
            p += strlen("Cpus_allowed_list:");
            const size_t n = strcspn(p, "|");
            snprintf(allowed, sizeof(allowed), "%.*s", (int)n, p);
        }
    }
    diagnostic_read("/proc/pressure/cpu", cpu_pressure, sizeof(cpu_pressure), 2);
    diagnostic_read("/proc/pressure/io", io_pressure, sizeof(io_pressure), 3);
    diagnostic_read("/proc/pressure/memory", mem_pressure, sizeof(mem_pressure), 4);
    char clocks[768] = {0}, thermals[768] = {0}, path[160], value[64], maximum[64];
    const long cores = sysconf(_SC_NPROCESSORS_ONLN);
    for (unsigned cpu = 0; cpu < 16 && cpu < (unsigned)(cores > 0 ? cores : 0); ++cpu) {
        strcpy(value, "unavailable"); strcpy(maximum, "unavailable");
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq", cpu);
        diagnostic_read(path, value, sizeof(value), 8 + cpu * 2);
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_max_freq", cpu);
        diagnostic_read(path, maximum, sizeof(maximum), 9 + cpu * 2);
        size_t n = strlen(clocks);
        snprintf(clocks + n, sizeof(clocks) - n, "%u:%s/%s,", cpu, value, maximum);
    }
    for (unsigned zone = 0; zone < 32; ++zone) {
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%u/type", zone);
        if (!diagnostic_read(path, value, sizeof(value), 40 + zone * 2)) continue;
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%u/temp", zone);
        strcpy(maximum, "unavailable");
        diagnostic_read(path, maximum, sizeof(maximum), 41 + zone * 2);
        size_t n = strlen(thermals);
        snprintf(thermals + n, sizeof(thermals) - n, "%s:%s,", value, maximum);
    }
    struct rusage usage;
    const int have_usage = getrusage(RUSAGE_THREAD, &usage) == 0;
    unsigned current_cpu = 0;
    const int have_cpu = syscall(SYS_getcpu, &current_cpu, NULL, NULL) == 0;
    snprintf(out, capacity,
        "tid=%ld cpu=%d priority=%d allowed=%s schedstat_ns_runtime_wait_timeslices='%s' "
        "voluntary_switches=%ld involuntary_switches=%ld minor_faults=%ld major_faults=%ld "
        "cpu_khz_cur_max='%s' thermal_raw='%s' pressure_cpu='%s' pressure_io='%s' pressure_memory='%s'",
        (long)syscall(SYS_gettid), have_cpu ? (int)current_cpu : -1,
        getpriority(PRIO_PROCESS, 0), allowed, sched,
        have_usage ? usage.ru_nvcsw : -1L, have_usage ? usage.ru_nivcsw : -1L,
        have_usage ? usage.ru_minflt : -1L, have_usage ? usage.ru_majflt : -1L,
        clocks, thermals[0] ? thermals : "unavailable", cpu_pressure, io_pressure, mem_pressure);
#else
    snprintf(out, capacity, "host_scheduler=unavailable thermal=unavailable clocks=unavailable");
#endif
}

static long long now_ns(void)
{
    struct timespec ts;
#if defined(CLOCK_MONOTONIC)
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }
#endif
    return 0;
}

/* /proc/self/stat fields 14 and 15 (1-based) are utime and stime in clock ticks. The comm field
   is parenthesised and may itself contain spaces and parentheses, so the scan starts after the
   LAST ')' rather than tokenising from the front — a process named "armsx (test)" otherwise
   shifts every field and yields nonsense. */
static int read_process_cpu_ticks(unsigned long long* out_ticks)
{
    char buffer[1024];
    FILE* file = fopen("/proc/self/stat", "r");
    size_t got;
    char* tail;
    unsigned long long utime = 0, stime = 0;
    int field;

    if (!file) {
        return 0;
    }
    got = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    if (got == 0) {
        return 0;
    }
    buffer[got] = '\0';

    tail = strrchr(buffer, ')');
    if (!tail) {
        return 0;
    }
    ++tail; /* now at the space before field 3 (state) */

    /* Skip forward to field 14: we are positioned before field 3, so advance 11 fields. */
    for (field = 0; field < 11; ++field) {
        while (*tail == ' ') ++tail;
        while (*tail != '\0' && *tail != ' ') ++tail;
        if (*tail == '\0') {
            return 0;
        }
    }
    if (sscanf(tail, " %llu %llu", &utime, &stime) != 2) {
        return 0;
    }

    *out_ticks = utime + stime;
    return 1;
}

/* A named kB-valued line out of a /proc meminfo-style file, e.g. "VmRSS:     123456 kB". */
static int read_kb_field(const char* path, const char* key, double* out_mb)
{
    char line[256];
    FILE* file = fopen(path, "r");
    const size_t key_len = strlen(key);

    if (!file) {
        return 0;
    }
    while (fgets(line, sizeof(line), file)) {
        if (strncmp(line, key, key_len) == 0) {
            const char* scan = line + key_len;
            double kb = 0.0;
            while (*scan == ':' || *scan == ' ' || *scan == '\t') {
                ++scan;
            }
            kb = strtod(scan, NULL);
            fclose(file);
            *out_mb = kb / 1024.0;
            return 1;
        }
    }
    fclose(file);
    return 0;
}

/*
    GPU busy.

    Adreno exposes /sys/class/kgsl/kgsl-3d0/gpubusy as two numbers, "busy total", both since the
    last read — so the node is self-clearing and each read yields the occupancy over the interval
    since the previous one. gpu_busy_percentage, where present, is already a percentage.

    Mali exposes a "utilisation" node under its platform device, also already a percentage.

    On most retail Android these are root-only. Returning a negative is the correct outcome then;
    the OSD prints "n/a". Do NOT substitute a derived figure here — a number that looks like GPU
    load but is really frame time would be actively misleading in the one place a user goes to
    find out what is limiting them.
*/
static int read_gpu_percent(double* out_percent)
{
    /* Latched once every node has been refused.

       On retail Android these live under vendor_sysfs_kgsl and SELinux denies an untrusted app
       outright, so retrying costs a kernel audit record per node per sample — twice a second,
       forever, for a value we will never get. That noise lands in the very logcat a user sends
       with a bug report, burying whatever actually went wrong. One round of attempts is enough
       to learn the answer; the OSD keeps printing "n/a" exactly as before. */
    static int nodes_unavailable = 0;

    static const char* const percent_nodes[] = {
        "/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage",
        "/sys/devices/platform/mali.0/utilisation",
        "/sys/devices/platform/13000000.mali/utilisation",
        NULL,
    };
    char buffer[128];
    FILE* file;
    int i;

    if (nodes_unavailable) {
        return 0;
    }

    /* Try the self-clearing busy/total counter FIRST. It reports occupancy over the interval
       since the previous read, which is exactly our sample window, whereas gpu_busy_percentage
       is an instantaneous sample that reads 0 far more often than the GPU is actually idle. */
    file = fopen("/sys/class/kgsl/kgsl-3d0/gpubusy", "r");
    if (file) {
        if (fgets(buffer, sizeof(buffer), file)) {
            double busy = 0.0, total = 0.0;
            fclose(file);
            if (sscanf(buffer, "%lf %lf", &busy, &total) == 2 && total > 0.0) {
                double percent = (busy / total) * 100.0;
                if (percent < 0.0) percent = 0.0;
                if (percent > 100.0) percent = 100.0;
                *out_percent = percent;
                return 1;
            }
        } else {
            fclose(file);
        }
    }

    for (i = 0; percent_nodes[i] != NULL; ++i) {
        file = fopen(percent_nodes[i], "r");
        if (!file) {
            continue;
        }
        if (fgets(buffer, sizeof(buffer), file)) {
            double value = strtod(buffer, NULL);
            fclose(file);
            if (value >= 0.0 && value <= 100.0) {
                *out_percent = value;
                return 1;
            }
            continue;
        }
        fclose(file);
    }

    nodes_unavailable = 1;

    return 0;
}

void armsx_host_usage_sample(armsx_host_usage_t* out)
{
    static armsx_host_usage_t cached = { -1.0, 0, -1.0, -1.0, -1.0 };
    static long long last_sample_ns = 0;
    static unsigned long long last_ticks = 0;
    static int have_last_ticks = 0;

    const long long now = now_ns();
    unsigned long long ticks = 0;
    double value = 0.0;

    if (!out) {
        return;
    }

    /* Serve the cache between intervals. `now == 0` means the clock failed; sample anyway rather
       than freeze on a stale value forever. */
    if (now != 0 && last_sample_ns != 0 && (now - last_sample_ns) < ARMSX_HOST_USAGE_INTERVAL_NS) {
        *out = cached;
        return;
    }

    if (cached.cpu_cores <= 0) {
        long cores = sysconf(_SC_NPROCESSORS_ONLN);
        cached.cpu_cores = (cores > 0) ? (int)cores : 0;
    }

    if (read_process_cpu_ticks(&ticks)) {
        const long hz = sysconf(_SC_CLK_TCK);
        if (have_last_ticks && hz > 0 && now != 0 && last_sample_ns != 0 && now > last_sample_ns) {
            const double elapsed_s = (double)(now - last_sample_ns) / 1e9;
            const double busy_s = (double)(ticks - last_ticks) / (double)hz;
            double of_one_core = (elapsed_s > 0.0) ? (busy_s / elapsed_s) * 100.0 : -1.0;

            /* Divide by the core count so this is a share of the whole device, matching
               gpu_percent's scale. Guard the divisor: sysconf can fail, and dividing by zero
               would turn a real reading into inf. */
            if (of_one_core >= 0.0 && cached.cpu_cores > 0) {
                cached.cpu_percent = of_one_core / (double)cached.cpu_cores;
                if (cached.cpu_percent > 100.0) {
                    cached.cpu_percent = 100.0;
                }
            } else {
                cached.cpu_percent = (of_one_core >= 0.0) ? of_one_core : -1.0;
            }

            if (cached.cpu_percent < 0.0 && of_one_core >= 0.0) {
                cached.cpu_percent = 0.0;
            }
        }
        /* The FIRST sample has no previous reading to difference against, so it stays
           unavailable rather than reporting the process's whole lifetime average as if it
           were current. */
        last_ticks = ticks;
        have_last_ticks = 1;
    } else {
        cached.cpu_percent = -1.0;
    }

    cached.ram_mb = read_kb_field("/proc/self/status", "VmRSS", &value) ? value : -1.0;
    cached.ram_available_mb = read_kb_field("/proc/meminfo", "MemAvailable", &value) ? value : -1.0;
    cached.gpu_percent = read_gpu_percent(&value) ? value : -1.0;

    last_sample_ns = now;
    *out = cached;
}
