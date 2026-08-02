# iOS host scaffold

This directory contains a minimal UIKit host for the shared `libarmsx` build. It embeds SDL2 as a dynamic framework and launches the native FSUI donor shell directly through `external_main`.

## Layout
- `Frameworks/` – drop `SDL2.xcframework` (built from commit 385e995790930bc70ce43533f621caedec033895) and the iOS-built `libarmsx.dylib` here.
- `HostApp/` – source, Info.plist and an XcodeGen `project.yml` that describes a single-app target.
- `bin/bios.bin` – optional test BIOS is bundled (resource marked optional) so the iOS target can boot out-of-the-box during development.

## Building libarmsx for iOS
```
./build.sh ios
```
This forces dynamic SDL, uses the iPhone SDK toolchain, and outputs `bin/libarmsx.dylib` for device (arm64). You can tweak `IOS_SDK`/`IOS_DEPLOYMENT_TARGET` as needed.

## Generating/opening the app project
The `HostApp/project.yml` is XcodeGen-ready. After placing the frameworks:
```
cd ios/HostApp
xcodegen generate
open PSXEHost.xcodeproj
```
The target embeds both `SDL2.xcframework` and `libarmsx.dylib` into the app bundle and runs `external_main` on launch using the host `UIWindow`. There is no React Native, CocoaPods, or JS bundle step in the iOS flow anymore.
