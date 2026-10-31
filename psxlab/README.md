# Original PSX Lab engine

This folder contains the original Java/JNI Android emulator and its shared backend
library. ArmSX1 includes `backend` as `:psxlabBackend`; the standalone `app` and its
JVM regressions remain independently buildable. See [the integration notes](../PSXLAB-BACKEND.md)
for host contracts, validation, save compatibility and unsupported features.

BIOS images and games are supplied by the user and are never included here.
libchdr and SoundTouch retain their upstream source and license notices in
`app/src/main/cpp/vendor`.
