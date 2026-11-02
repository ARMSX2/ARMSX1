# GPU timer lifecycle validation

This compiles the actual production `gpu_stage_timer.h` against a test-only GL shim and executes it with a deterministic fake GPU clock. No app/device install or GL driver is needed. The shim records calls and rejects result reads before availability; it does not emulate rasterization or predict device speed.

From a Windows MSVC developer shell with clang++ (or a C++17 host compiler):

```powershell
clang++ -std=c++17 -O2 -Itools/gpu_timing_validation/include -Ipsxlab/app/src/main/cpp tools/gpu_timing_validation/main.cpp -o gpu-timing-validation.exe
.\gpu-timing-validation.exe
```

Assertions must remain enabled. The test covers 5,000 disabled scopes with no query allocations/commands/result reads, nested draw/resolve accounting, nonblocking result polling, the 8,192-query bound, disjoint-result rejection and logging toggles with pending GPU results. Do not add its fake GLES include directory to Android builds.
