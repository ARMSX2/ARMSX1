# Local ARM64 translator validation

This harness emits code from the actual `cpu_arm64.cpp` and executes its AArch64 bytes in Unicorn 2.1.4. It does not install an app or require an Android device. Unicorn is a test-only Python dependency; it is not packaged in the APK.

Build `bridge.cpp` and `psxlab/app/src/main/cpp/cpu_arm64.cpp` as a shared library with C++17 and `__aarch64__` defined. On Windows, use an MSVC developer shell with clang++, add `tools/arm64_validation/include` and `psxlab/app/src/main/cpp` as include directories, and pass `-shared -O2 -fwrapv`. The Windows mmap adapter only allocates an emission buffer; it never executes generated code in the host process. Do not put that adapter on an Android build's include path.

Example from the repository root in a developer shell:

```powershell
clang++ -std=c++17 -O2 -fwrapv -shared -D__aarch64__ -Itools/arm64_validation/include -Ipsxlab/app/src/main/cpp tools/arm64_validation/bridge.cpp psxlab/app/src/main/cpp/cpu_arm64.cpp -o arm64-validation.dll
python -m pip install unicorn==2.1.4
python tools/verify_arm64_blocks.py --bridge ./arm64-validation.dll --cases 8000 --seed 24301
```

Use a private environment for the dependency. Other hosts can supply their normal mmap implementation and a shared-library filename supported by ctypes.

The scalar MIPS oracle compares complete CPU/load-delay state, RAM and code-page generations. Cases cover instruction-budget prefix exits, RAM alignment/MMIO slow exits, conditional branches and safe delay slots, HI/LO corner cases, and GTE register/command calls. GTE callbacks deliberately clobber caller-saved registers and flags to check the generated call ABI; geometry arithmetic uses the existing shared engine implementation, not an independent geometry oracle. The script also checks stack/callee-saved registers, unsupported instruction rejection, hardware-register counters, and 5,000 event-budget prefix calculations.

This proves the covered generated instruction behavior. It does not measure device speed, physical frame presentation, driver behavior or audible output. Android instrumentation fixtures are compiled separately and need explicit device testing before those claims can be made.
