# Original PSX Lab engine

This folder contains the original Java/JNI Android emulator and its shared backend
library. ArmSX1 includes `backend` as `:psxlabBackend`; the standalone `app` and its
JVM regressions remain independently buildable. See [the integration notes](../PSXLAB-BACKEND.md)
for host contracts, validation, save compatibility and unsupported features.

BIOS images and games are supplied by the user and are never included here.
libchdr and SoundTouch retain their upstream source and license notices in
`app/src/main/cpp/vendor`.

Standalone 0.13.8 mounts CUE/BIN tracks and unencrypted PS1 PSISOIMG PBP files
through `DiscMedia`, including URI signature detection, Android tree-folder access,
track metadata and CDDA. The library hides BIN tracks referenced by a CUE, and
artwork serial lookup shares the launch reader. Encrypted PSN/PSP containers are
rejected explicitly. Add a game's folder so its CUE and BIN files are accessible
together; file-only CUE grants cannot grant sibling-track access.

The headless `probeDiscFormats=true` instrumentation check uses original generated
ISO/PBP/executable fixtures and an opaque-ID document provider. It checks CUE/PBP
mounting, nested and case-matched paths, ISO/EXE regressions, executable execution,
library track filtering and descriptor cleanup without playing audio or opening a game.
