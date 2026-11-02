#pragma once
#define _SC_PAGESIZE 1
inline long sysconf(int){return 4096;}
