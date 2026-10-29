param([int]$Jobs = 8, [switch]$CheckedLutFastmem = $true, [switch]$DisableCheckedLutFastmem, [ValidateRange(0,7)][int]$CpuOptimizationMask = 7)
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$taskSdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } elseif ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
$taskCore = Join-Path $PSScriptRoot 'core/swanstation'
$taskCommit = & git -C $taskCore rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $taskCommit -ne 'dd4805619bbbfc6e0d1776372cb16377088b448e') {
    throw 'Initialize the pinned SwanStation submodule before building.'
}
$taskCmake = Join-Path $taskSdk 'cmake/3.30.5/bin/cmake.exe'
$taskBuild = Join-Path $PSScriptRoot 'build-core'
$taskCheckedLut = if ($CheckedLutFastmem -and !$DisableCheckedLutFastmem) { 'ON' } else { 'OFF' }
& $taskCmake -S (Join-Path $PSScriptRoot 'core/swanstation') -B $taskBuild -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$taskSdk/cmake/3.30.5/bin/ninja.exe" `
    "-DCMAKE_TOOLCHAIN_FILE=$taskSdk/ndk/29.0.14206865/build/cmake/android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static `
    -DCMAKE_BUILD_TYPE=RelWithDebInfo `
    "-DSWAN_CHECKED_LUT_FASTMEM=$taskCheckedLut" `
    "-DSWAN_CPU_OPTIMIZATION_MASK=$CpuOptimizationMask" `
    "-DCMAKE_PROJECT_INCLUDE=$PSScriptRoot/native/swanstation/core_bridge.cmake" `
    '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384'
if ($LASTEXITCODE -ne 0) { throw 'SwanStation configuration failed' }
& $taskCmake --build $taskBuild --target swanstation_libretro -j $Jobs
if ($LASTEXITCODE -ne 0) { throw 'SwanStation compilation failed' }
$taskDestination = Join-Path $PSScriptRoot 'android/app/build/generated/swan-jni/arm64-v8a'
New-Item -ItemType Directory -Force -Path $taskDestination | Out-Null
Copy-Item -LiteralPath (Join-Path $taskBuild 'swanstation_libretro_android.so') -Destination (Join-Path $taskDestination 'libswanstation.so')
$taskAngle = Join-Path $PSScriptRoot 'third_party/angle-prebuilt'
$taskAngleCommit = & git -C $taskAngle rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $taskAngleCommit -ne '9a0834cc8a5f3427dae2f6e87de4dfae50cb781c') { throw 'Initialize the pinned official AOSP ANGLE submodule.' }
foreach ($taskLibrary in @('libEGL_angle.so','libGLESv2_angle.so')) {
    $taskLibrarySource = Join-Path $taskAngle "lib/arm64/$taskLibrary"
    $taskExpectedHash = if ($taskLibrary -eq 'libEGL_angle.so') { 'CB93BE360BB9FD727C469F0F7ABBCDD2B751B2A1C9BFABB33B09093849455C0E' } else { '43FE2374F3361AC26C8597F4866EFED8205FB223ECE2D14C857AF645182D9DDC' }
    $taskHashStream = [IO.File]::OpenRead($taskLibrarySource)
    $taskSha = [Security.Cryptography.SHA256]::Create()
    try { $taskInputHash = ([BitConverter]::ToString($taskSha.ComputeHash($taskHashStream))).Replace('-','') }
    finally { $taskHashStream.Dispose(); $taskSha.Dispose() }
    if ($taskInputHash -ne $taskExpectedHash) { throw "Changed ANGLE input: $taskLibrary" }
    Copy-Item -LiteralPath $taskLibrarySource -Destination (Join-Path $taskDestination $taskLibrary)
}
if (Get-ChildItem -LiteralPath $taskDestination | Where-Object Name -notin @('libswanstation.so','libEGL_angle.so','libGLESv2_angle.so','liblibrashader_capi.so','libc++_shared.so')) {
    throw 'Unexpected native library in SwanStation staging directory'
}
& (Join-Path $PSScriptRoot 'Build-SwanShaders.ps1') -WorkDirectory $env:ARMSX_SHADER_WORK_DIR
