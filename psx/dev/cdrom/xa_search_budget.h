#ifndef ARMSX_XA_SEARCH_BUDGET_H
#define ARMSX_XA_SEARCH_BUDGET_H
#include <stdint.h>

/* Time only disc reads, never the audio mixing between them. */
#ifdef __ANDROID__
#include <time.h>
static inline uint64_t xa_search_clock_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}
#else
static inline uint64_t xa_search_clock_ns(void) { return 0; }
#endif

typedef struct {
    unsigned reads;
    uint64_t read_ns;
} xa_search_budget_t;

static inline int xa_search_budget_available(const xa_search_budget_t* budget) {
    /* Allow a small interleaved run even on a slow device, but never an
       unbounded scan. Only time inside sector reads is charged. */
    return budget->reads < 64 &&
        (budget->reads < 16 || budget->read_ns < UINT64_C(2000000));
}

static inline void xa_search_budget_charge(xa_search_budget_t* budget,
                                           uint64_t start, uint64_t end) {
    ++budget->reads;
    if (start && end >= start)
        budget->read_ns += end - start;
}
#endif
