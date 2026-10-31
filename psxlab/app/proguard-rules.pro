# Preserve the original core API and JNI/instrumentation entry points while
# allowing R8 to inline and optimize the runtime call sites.
-keep,allowoptimization class dev.psxlab.** { *; }
-keepattributes SourceFile,LineNumberTable
