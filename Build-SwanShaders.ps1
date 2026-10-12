# SPDX-License-Identifier: GPL-3.0-or-later
param([string]$WorkDirectory)
$ErrorActionPreference='Stop'
$taskSdk = if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
$taskWork = if ($WorkDirectory) { [IO.Path]::GetFullPath($WorkDirectory) } else { Join-Path $PSScriptRoot 'build-shaders' }
$taskCrate = Join-Path $PSScriptRoot 'third_party/librashader/source/librashader-capi-0.12.0.crate'
$taskSource = Join-Path $taskWork 'librashader-capi-0.12.0'
New-Item -ItemType Directory -Force -Path $taskWork | Out-Null
if (-not (Test-Path -LiteralPath (Join-Path $taskSource 'Cargo.toml'))) {
    & tar -xf $taskCrate -C $taskWork
    if ($LASTEXITCODE -ne 0) { throw 'librashader source extraction failed' }
}
$taskToolchain = Join-Path $taskSdk 'ndk/29.0.14206865/toolchains/llvm/prebuilt/windows-x86_64/bin'
$env:CC_aarch64_linux_android=Join-Path $taskToolchain 'aarch64-linux-android28-clang.cmd'
$env:CXX_aarch64_linux_android=Join-Path $taskToolchain 'aarch64-linux-android28-clang++.cmd'
$env:AR_aarch64_linux_android=Join-Path $taskToolchain 'llvm-ar.exe'
$env:CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$env:CC_aarch64_linux_android
$env:CARGO_TARGET_AARCH64_LINUX_ANDROID_RUSTFLAGS='-C link-arg=-Wl,-z,max-page-size=16384 -C link-arg=-Wl,-z,common-page-size=16384'
$env:CARGO_TARGET_DIR=Join-Path $taskWork 'target'
Push-Location -LiteralPath $taskSource
try {
    & cargo build --locked --release --target aarch64-linux-android --no-default-features --features runtime-vulkan,runtime-opengl
    if ($LASTEXITCODE -ne 0) { throw 'librashader compilation failed' }
} finally { Pop-Location }
$taskBinary = Join-Path $env:CARGO_TARGET_DIR 'aarch64-linux-android/release/liblibrashader_capi.so'
$taskDependencies = & (Join-Path $taskToolchain 'llvm-readelf.exe') -d $taskBinary
if ($LASTEXITCODE -ne 0 -or ($taskDependencies -match 'Shared library: \[libvulkan')) { throw 'Shader binary links the system Vulkan loader' }
$taskDestination=Join-Path $PSScriptRoot 'android/app/build/generated/swan-jni/arm64-v8a'
New-Item -ItemType Directory -Force -Path $taskDestination | Out-Null
Copy-Item -LiteralPath $taskBinary -Destination (Join-Path $taskDestination 'liblibrashader_capi.so')
Copy-Item -LiteralPath (Join-Path $taskToolchain '../sysroot/usr/lib/aarch64-linux-android/libc++_shared.so') -Destination (Join-Path $taskDestination 'libc++_shared.so')
