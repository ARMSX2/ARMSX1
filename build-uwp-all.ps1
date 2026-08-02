param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "x86")]
    [string]$Platform = "x64"
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path $PSScriptRoot).Path
Set-Location $RepoRoot

switch ($Platform) {
    "x64" {
        $SDL2Dir = "SDL2-2.30.3\x86_64-w64-mingw32"
        $FsuiBuildDir = Join-Path $RepoRoot "build\fsui\uwp-x64"
    }
    "x86" {
        $SDL2Dir = "SDL2-2.30.3\i686-w64-mingw32"
        $FsuiBuildDir = Join-Path $RepoRoot "build\fsui\uwp-x86"
    }
    default {
        throw "Unsupported UWP platform '$Platform'."
    }
}

$SDL2Root = Join-Path $RepoRoot $SDL2Dir
$SDL2Include = Join-Path $SDL2Root "include\SDL2"
$SDL2Lib = Join-Path $SDL2Root "lib"
$UwpOutputDir = Join-Path $RepoRoot ("uwp\ARMSX\bin\{0}\{1}" -f $Platform, $Configuration)
$UwpIconsDir = Join-Path $UwpOutputDir "icons"
$SolutionPath = Join-Path $RepoRoot "uwp\ARMSX.sln"

if (!(Test-Path $SDL2Root)) {
    throw "SDL2 dependency directory not found at $SDL2Root. Reuse the existing MinGW SDL package or override the script."
}

$MakeCommand = Get-Command "mingw32-make" -ErrorAction SilentlyContinue
if (-not $MakeCommand) {
    $MakeCommand = Get-Command "make" -ErrorAction SilentlyContinue
}
if (-not $MakeCommand) {
    throw "Unable to locate mingw32-make or make in PATH."
}

$MsBuildCommand = Get-Command "MSBuild.exe" -ErrorAction SilentlyContinue
if (-not $MsBuildCommand) {
    $MsBuildCommand = Get-Command "msbuild" -ErrorAction SilentlyContinue
}
if (-not $MsBuildCommand) {
    throw "Unable to locate MSBuild in PATH."
}

New-Item -ItemType Directory -Force -Path bin, $UwpOutputDir, $UwpIconsDir | Out-Null

cmake -S third_party/fsui-lib -B $FsuiBuildDir -G "MinGW Makefiles" `
    -DFSUI_BUILD_SAMPLES=OFF `
    -DFSUI_PLATFORM_BACKEND=SDL2 `
    -DFSUI_USE_SYSTEM_SDL2=ON `
    -DCMAKE_PREFIX_PATH="$SDL2Root"
cmake --build $FsuiBuildDir -j4

& $MakeCommand.Source clean
& $MakeCommand.Source shared `
    WINDOWS_TARGET=1 `
    UWP_TARGET=1 `
    SDL_STATIC=0 `
    FSUI_BUILD_DIR="$FsuiBuildDir" `
    SDL_CFLAGS="-I$SDL2Include" `
    SDL_LIBS_DYNAMIC="-L$SDL2Lib -lSDL2"

Copy-Item -Path (Join-Path $RepoRoot "bin\libarmsx.dll") -Destination $UwpOutputDir -Force
Copy-Item -Path (Join-Path $RepoRoot "icons\*") -Destination $UwpIconsDir -Recurse -Force

& $MsBuildCommand.Source $SolutionPath `
    /m `
    /p:Configuration=$Configuration `
    /p:Platform=$Platform
