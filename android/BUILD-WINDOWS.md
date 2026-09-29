# Compose Android build on Windows

Use the android-compose branch. Install JDK 17 or 21, Android SDK platform 36,
NDK 27.0.12077973 and CMake 3.30.5 through the Android SDK manager.

Initialize the required submodules from the repository root:

    git submodule update --init third_party/SDL third_party/libchdr

The Compose interface does not require the retired fsui-lib submodule.
Set JAVA_HOME and ANDROID_HOME, then from the android folder run:

    gradlew.bat :app:assembleGithubDebug --console=plain

The debug-signed APK is app/build/outputs/apk/github/debug/app-github-debug.apk.
CMake builds the native libraries through Ninja, so Bash and Make are not required.
Custom driver hook libraries and the Discord helper are built alongside the core.
Shader chains require the optional librashader binary, as in the original build.sh.

An APK signed with your local debug key cannot replace an official release signed
with a different key. Back up existing app data before uninstalling an official APK.
