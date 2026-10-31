-dontwarn org.libsdl.app.**
-keep class org.libsdl.app.** { *; }

# The application ID is com.nanodata.armsx but the SOURCES live in com.armsx2 — this rule
# only ever matched the former, i.e. nothing. Kept for safety, but the real packages are below.
-keepclassmembers class com.nanodata.armsx.** { *; }

# ---------------------------------------------------------------------------------------
# JNI. R8 cannot see a method that only C++ reaches through GetMethodID(), so anything
# native calls BACK into looks dead and gets renamed or stripped. That fails at runtime with
# no crash and no log — the feature is simply silently gone (sound effects, rumble, RA
# toasts). frontend/android_jni.cpp resolves kr/co/iefriends/pcsx2/NativeApp and calls
# playSound on it, so this whole bridge must survive verbatim, names included.
-keep class kr.co.iefriends.pcsx2.** { *; }
-keepclassmembers class kr.co.iefriends.pcsx2.** { *; }

# Java->native declarations. The default optimize config carries this too; being explicit so
# it cannot be lost if the default file is ever swapped.
-keepclasseswithmembernames,includedescriptorclasses class * {
    native <methods>;
}

# The app's actual sources. Broad on purpose: the JNI surface, Compose entry points and the
# settings serialisation are all name-sensitive, and a wrong-but-quiet release build costs far
# more than the few MB this gives back. Narrow it only with a device pass behind it.
-keep class com.armsx2.** { *; }
-keepclassmembers class com.armsx2.** { *; }
