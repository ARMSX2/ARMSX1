if (Test-Path "SDL2-2.30.3") {
    Remove-Item -Recurse "SDL2-2.30.3"
}

$SDL2_URL = "https://github.com/libsdl-org/SDL/releases/download/release-2.30.3/SDL2-devel-2.30.3-mingw.zip"

Invoke-WebRequest -URI $SDL2_URL -OutFile "sdl2.zip"
Expand-Archive "sdl2.zip" -DestinationPath "." -Force

Remove-Item "sdl2.zip"