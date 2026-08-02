git fetch --all --tags

$VERSION_TAG = git describe --always --tags --abbrev=0
$COMMIT_HASH = git rev-parse --short HEAD
$OS_INFO = (Get-WMIObject win32_operatingsystem).caption + " " + `
           (Get-WMIObject win32_operatingsystem).version + " " + `
           (Get-WMIObject win32_operatingsystem).OSArchitecture

$RepoRoot = (Resolve-Path ".").Path
$SDL2_DIR = "SDL2-2.30.3\x86_64-w64-mingw32"
$FSUI_BUILD_DIR = Join-Path $RepoRoot "build\fsui\win64"
$SDL2_ROOT = Join-Path $RepoRoot $SDL2_DIR
$SDL2_INCLUDE = Join-Path $SDL2_ROOT "include\SDL2"
$SDL2_LIB = Join-Path $SDL2_ROOT "lib"

if (!(Test-Path $SDL2_ROOT)) {
    throw "SDL2 dependency directory not found at $SDL2_ROOT. Run .\build-deps.ps1 first."
}

mkdir -Force -Path bin > $null
mkdir -Force -Path "bin\icons" > $null

cmake -S third_party/fsui-lib -B $FSUI_BUILD_DIR -G "MinGW Makefiles" `
    -DFSUI_BUILD_SAMPLES=OFF `
    -DFSUI_PLATFORM_BACKEND=SDL2 `
    -DFSUI_USE_SYSTEM_SDL2=ON `
    -DCMAKE_PREFIX_PATH="$SDL2_ROOT"
cmake --build $FSUI_BUILD_DIR -j4

mingw32-make clean
mingw32-make `
    SDL_STATIC=0 `
    FSUI_BUILD_DIR="$FSUI_BUILD_DIR" `
    SDL_CFLAGS="-I$SDL2_INCLUDE" `
    SDL_LIBS_DYNAMIC="-L$SDL2_LIB -lSDL2 -lsetupapi -limm32 -lversion -lwinmm -lgdi32 -lole32 -loleaut32 -lshell32 -luuid -lopengl32"

Copy-Item -Path "$($SDL2_DIR)\bin\SDL2.dll" -Destination "bin"
Copy-Item -Path "icons\*" -Destination "bin\icons" -Recurse -Force
