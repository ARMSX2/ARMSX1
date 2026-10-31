param(
    [string]$AndroidSdk = "$env:LOCALAPPDATA/Android/Sdk",
    [string]$NdkVersion = '28.2.13676358',
    [string]$WorkDirectory = "$env:LOCALAPPDATA/Temp/psxlab-shader-runtime"
)
$ErrorActionPreference = 'Stop'
$taskRepo = Split-Path -Parent $PSScriptRoot
$taskNdk = Join-Path $AndroidSdk "ndk/$NdkVersion"
$taskToolchain = Join-Path $taskNdk 'toolchains/llvm/prebuilt/windows-x86_64'
if (!(Test-Path -LiteralPath "$taskToolchain/bin/clang.exe")) { throw "Missing Android NDK: $taskNdk" }
$taskNinja = Get-ChildItem -LiteralPath (Join-Path $AndroidSdk 'cmake') -Filter ninja.exe -Recurse | Select-Object -First 1
if (!$taskNinja) { throw 'Install the Android SDK CMake tools first.' }
Get-Command cargo,rustup,tar -ErrorAction Stop | Out-Null
New-Item -ItemType Directory -Force -Path $WorkDirectory | Out-Null
$taskArchive = Join-Path $taskRepo 'third_party/librashader/source/librashader-capi-0.12.0.crate'
& tar -xzf $taskArchive -C $WorkDirectory
if ($LASTEXITCODE) { throw 'Could not extract the pinned librashader source.' }
$taskSource = Join-Path $WorkDirectory 'librashader-capi-0.12.0'
$taskVariableNames = @('CARGO_TARGET_DIR','CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER','RUSTFLAGS','CC_aarch64_linux_android','CXX_aarch64_linux_android','AR_aarch64_linux_android','CFLAGS_aarch64_linux_android','CXXFLAGS_aarch64_linux_android','ANDROID_NDK_ROOT','CMAKE_TOOLCHAIN_FILE','CMAKE_GENERATOR','PATH')
$taskPrevious = @{}
foreach ($taskName in $taskVariableNames) { $taskPrevious[$taskName] = [Environment]::GetEnvironmentVariable($taskName,'Process') }
try {
    $env:CARGO_TARGET_DIR = Join-Path $WorkDirectory 'target'
    $env:CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER = "$taskToolchain/bin/clang.exe"
    $env:RUSTFLAGS = '-C link-arg=--target=aarch64-linux-android28 -C link-arg=-Wl,-z,max-page-size=16384'
    $env:CC_aarch64_linux_android = "$taskToolchain/bin/clang.exe"
    $env:CXX_aarch64_linux_android = "$taskToolchain/bin/clang++.exe"
    $env:AR_aarch64_linux_android = "$taskToolchain/bin/llvm-ar.exe"
    $env:CFLAGS_aarch64_linux_android = '--target=aarch64-linux-android28'
    $env:CXXFLAGS_aarch64_linux_android = '--target=aarch64-linux-android28'
    $env:ANDROID_NDK_ROOT = $taskNdk
    $env:CMAKE_TOOLCHAIN_FILE = "$taskNdk/build/cmake/android.toolchain.cmake"
    $env:CMAKE_GENERATOR = 'Ninja'
    $env:PATH = "$($taskNinja.DirectoryName);$taskToolchain/bin;$env:PATH"
    & rustup target add aarch64-linux-android
    if ($LASTEXITCODE) { throw 'Could not install the Rust Android target.' }
    Push-Location -LiteralPath $taskSource
    try { & cargo build --locked --release --target aarch64-linux-android --no-default-features --features runtime-vulkan }
    finally { Pop-Location }
    if ($LASTEXITCODE) { throw 'librashader build failed.' }
    $taskLibrary = Join-Path $env:CARGO_TARGET_DIR 'aarch64-linux-android/release/liblibrashader_capi.so'
    $taskNeeded = & "$taskToolchain/bin/llvm-readelf.exe" -d $taskLibrary
    if ($LASTEXITCODE -or ($taskNeeded -match '\(NEEDED\).*libvulkan')) { throw 'Invalid runtime: it must load Vulkan through the supplied entry points.' }
    $taskDestination = Join-Path $taskRepo 'psxlab/backend/src/main/jniLibs/arm64-v8a'
    New-Item -ItemType Directory -Force -Path $taskDestination | Out-Null
    Copy-Item -LiteralPath $taskLibrary -Destination "$taskDestination/liblibrashader_capi.so"
    Copy-Item -LiteralPath "$taskToolchain/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" -Destination $taskDestination
    & "$taskToolchain/bin/llvm-strip.exe" --strip-unneeded "$taskDestination/liblibrashader_capi.so" "$taskDestination/libc++_shared.so"
    if ($LASTEXITCODE) { throw 'Could not strip the packaged runtimes.' }
    Get-FileHash -Algorithm SHA256 -LiteralPath "$taskDestination/liblibrashader_capi.so"
} finally {
    foreach ($taskName in $taskVariableNames) { [Environment]::SetEnvironmentVariable($taskName,$taskPrevious[$taskName],'Process') }
}
