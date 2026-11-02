#pragma once
// Emit-only Windows test adapter. ARM64 bytes are read by Unicorn, never called on x64.
#include <cstdlib>
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_PRIVATE 2
#define MAP_ANONYMOUS 32
#define MAP_FAILED reinterpret_cast<void*>(-1)
inline void* mmap(void*,size_t bytes,int,int,int,long){return std::malloc(bytes);}
inline int mprotect(void*,size_t,int){return 0;}
inline int munmap(void* p,size_t){std::free(p);return 0;}
