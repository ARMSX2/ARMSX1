Original GPU backend validation
===============================

These executables use production GPU code. They do not install an APK or access
an Android device, and do not predict phone FPS, audio quality or driver behavior.
Keep assertions enabled. None of the test-only headers belongs in Android builds.

native_vulkan.cpp
-----------------
Compile this file and psxlab/app/src/main/cpp/vulkan_gpu.cpp with C++17, -O2 and
-fwrapv. Include psxlab/app/src/main/cpp, Vulkan SDK headers, and the JDK's include
and platform include directories (raster_core.h includes jni.h). Link the Vulkan
loader: vulkan-1.lib on Windows or -lvulkan on Linux. Run without a display surface.
Vulkan 1.0 plus VK_KHR_get_physical_device_properties2 is sufficient for the
extension-free tests. The test enables ordered color access only when advertised.

The scalar oracle checks non-feedback canonical VRAM for all primitive families,
texture depths, blend modes, texture windows and masks; source textures and CLUTs
are kept outside the destination. True feedback is checked separately against the
existing hardware engine's pre-draw snapshot contract, not live-RAM software
feedback. 1x, 2x and 3x each exercise 312 raster fixtures, 14,000 queued draws,
wrapped transfers, overlapping copies and scanout. The footprint check samples
covered triangles at scales 1-5 and includes the false-feedback regression.

On a driver supporting ordered color attachment access, the additional fast 3x
test checks deferred native resolves, mixed blending/masks, large HD batches,
fresh HD texture reads, CLUTs, actual feedback, untouched gaps and state restoration.
The extension-free test context deliberately disables that optional feature even
when the test device supports it. Production HD initialization instead retains
GLES if the fast ordered native pipeline is unavailable.

To enable core and synchronization validation, supply the official Khronos layer,
set VK_LAYER_PATH to its manifest directory, and set ARMSX_GPU_VALIDATION=1.
Validation errors make the executable fail. Select a private test ICD with
VK_ICD_FILENAMES if required; use absolute paths for library_path in that manifest.
Do not change system driver or layer registration. Test binaries and dependencies
are kept outside the repository under the local codex-builds cache.

command_upload.cpp
------------------
Compile with C++17 and the include order:
  -Itools/gpu_backend_validation/include -Ipsxlab/app/src/main/cpp
This deterministic GL shim verifies the actual upload helper's allocation-failure
fallback, old-store lifetime, bounds checking and rejection of an undersized store.
It does not emulate a GPU or measure driver speed.

command_upload_gl.cpp
---------------------
Compile with real GLES 3.1/EGL headers and libraries, plus the production cpp
directory. Do not include the test shim directory for this executable. With a
desktop EGL implementation such as ANGLE, it runs 4,096 real compute dispatches
using the production uploader and checks every result after buffer readback.

Shader regeneration
-------------------
python tools/build_vulkan_gpu_shaders.py --compiler <NDK glslc executable>

This compiles the engine's own shared GLSL to Vulkan 1.0 SPIR-V and runs spirv-val
when it is beside glslc. Generated binaries are checked in to avoid a Python or
shader-compiler dependency during normal Android builds. Use --output <temp file>
and compare it with vulkan_gpu_shaders.h when verifying reproducibility.

Android checks
--------------
Run the shared engine JVM suite in psxlab with -Poptimized=true
testReleaseUnitTest. In android, build with -PpsxlabRuntimeTest and run
:app:assembleGithubDebug :app:testGithubDebugUnitTest
:app:assembleGithubDebugAndroidTest. Building the instrumentation APK does not
execute it. The existing GpuEfficiencyTest output/scale matrix also covers the
native Vulkan selection or its automatic GLES compatibility branch on-device.

Physical Android testing remains necessary for surface recreation, postprocessing
subframes, device-specific drivers, sustained thermals and real-game performance.
