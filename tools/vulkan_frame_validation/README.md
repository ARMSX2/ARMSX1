# Vulkan shared-frame fence gate

From the repository root in a Windows C++ developer shell with clang++ and the
SDL dependency's Khronos headers available:

```powershell
clang++ -std=c++17 -O2 -Ithird_party/SDL/src/video/khronos -Ipsxlab/app/src/main/cpp tools/vulkan_frame_validation/main.cpp -o "$env:TEMP/armsx-vulkan-frame-validation.exe"
& "$env:TEMP/armsx-vulkan-frame-validation.exe"
```

The fixture compiles the production gate against a controllable fence status.
It exercises 10,000 pending polls without linking a wait, reset, acquire or
submit implementation, then signalled reuse and a lost device. Keep assertions
enabled. This verifies host-side fence ownership/error handling, not hardware
rendering, frame pacing or device performance.
