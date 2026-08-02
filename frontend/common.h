#ifndef COMMON_H
#define COMMON_H

#include "diagnostics.h"

#ifndef OS_INFO
#define OS_INFO unknown
#endif
#ifndef REP_VERSION
#define REP_VERSION latest
#endif
#ifndef REP_COMMIT_HASH
#define REP_COMMIT_HASH latest
#endif

#if defined(__DLL_BUILD) && defined(_WIN32)
#define PSXE_API __declspec(dllexport)
#elif defined(__DLL_BUILD)
#define PSXE_API __attribute__((visibility("default")))
#else
#define PSXE_API
#endif

#define STR1(m) #m
#define STR(m) STR1(m)

#endif
